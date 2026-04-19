#include "SelfUpdater.h"
#include "FileSystemHelper.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <windows.h>
#include <limits.h>
#include <winver.h>
#include <thread>
#include <random>

std::wstring SelfUpdater::GetCurrentExePathW() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL,buffer,MAX_PATH);
    return std::wstring(buffer);
}

std::wstring SelfUpdater::GetShortPathNameSafe(const std::wstring& longPath) {
    wchar_t shortPath[MAX_PATH];
    DWORD len=GetShortPathNameW(longPath.c_str(),shortPath,MAX_PATH);
    if(len>0&&len<MAX_PATH) {
        return std::wstring(shortPath);
    }
    if(longPath.find(L'&')!=std::wstring::npos||
        longPath.find(L'|')!=std::wstring::npos||
        longPath.find(L';')!=std::wstring::npos) {
        g_logger<<"[ERROR] 路径包含危险字符，拒绝使用: "<<FileSystemHelper::WideToUtf8(longPath)<<std::endl;
        return L"";
    }
    return longPath;
}

void SelfUpdater::CleanupOldBackup(const std::wstring& currentExePath) {
    std::wstring backupPath=currentExePath+L".old";
    if(std::filesystem::exists(backupPath)) {
        std::error_code ec;
        std::filesystem::remove(backupPath,ec);
        if(!ec) {
            g_logger<<"[INFO] 已清理旧版本备份: "<<FileSystemHelper::WideToUtf8(backupPath)<<std::endl;
        }
    }
}

bool SelfUpdater::TryNormalReplace(const std::wstring& newExe,const std::wstring& targetExe) {
    std::wstring backup=targetExe+L".old";
    DeleteFileW(backup.c_str());
    MoveFileW(targetExe.c_str(),backup.c_str());
    if(MoveFileExW(newExe.c_str(),targetExe.c_str(),MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(backup.c_str());
        g_logger<<"[INFO] 普通权限替换成功"<<std::endl;
        return true;
    }

    DWORD err=GetLastError();
    g_logger<<"[WARN] 普通权限替换失败，错误码: "<<err<<"，尝试恢复备份..."<<std::endl;
    MoveFileW(backup.c_str(),targetExe.c_str());
    return false;
}

bool SelfUpdater::RunElevatedReplace(const std::wstring& newExe,const std::wstring& targetExe) {
    std::wstring shortNew=GetShortPathNameSafe(newExe);
    std::wstring shortTarget=GetShortPathNameSafe(targetExe);
    if(shortNew.empty()||shortTarget.empty()) {
        g_logger<<"[ERROR] 无法获取短路径名，提权替换中止"<<std::endl;
        return false;
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::wstring unique=L"updater_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(gen());
    std::wstring tempDir=std::filesystem::temp_directory_path().wstring()+L"\\"+unique;
    if(!std::filesystem::create_directory(tempDir)) {
        g_logger<<"[ERROR] 创建临时目录失败"<<std::endl;
        return false;
    }
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(NULL,selfPath,MAX_PATH);
    std::wstring helperExe=tempDir+L"\\helper.exe";
    if(!CopyFileW(selfPath,helperExe.c_str(),FALSE)) {
        g_logger<<"[ERROR] 复制辅助程序失败"<<std::endl;
        std::filesystem::remove_all(tempDir);
        return false;
    }
    std::wstring cmdLine=L"\""+helperExe+L"\" --elevated-replace \""+shortNew+L"\" \""+shortTarget+L"\"";
    SHELLEXECUTEINFOW sei={sizeof(sei)};
    sei.lpVerb=L"runas";
    sei.lpFile=helperExe.c_str();
    sei.lpParameters=cmdLine.c_str();
    sei.nShow=SW_HIDE;
    sei.fMask=SEE_MASK_NOCLOSEPROCESS;

    if(!ShellExecuteExW(&sei)) {
        DWORD err=GetLastError();
        g_logger<<"[ERROR] 启动提权辅助进程失败，错误码: "<<err<<std::endl;
        std::filesystem::remove_all(tempDir);
        return false;
    }
    DWORD waitResult=WaitForSingleObject(sei.hProcess,60000);
    DWORD exitCode=1;
    if(waitResult==WAIT_OBJECT_0) {
        GetExitCodeProcess(sei.hProcess,&exitCode);
    }
    else {
        g_logger<<"[ERROR] 提权辅助进程超时或异常"<<std::endl;
        TerminateProcess(sei.hProcess,1);
    }
    CloseHandle(sei.hProcess);
    std::error_code ec;
    std::filesystem::remove_all(tempDir,ec);

    return (exitCode==0);
}

bool SelfUpdater::LaunchNewProcess(const std::wstring& exePath) {
    STARTUPINFOW si={sizeof(si)};
    PROCESS_INFORMATION pi;
    if(!CreateProcessW(exePath.c_str(),NULL,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)) {
        g_logger<<"[ERROR] 启动新进程失败: "<<GetLastError()<<std::endl;
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

SelfUpdater::SelfUpdater(HttpClient& httpClient,ConfigManager& configManager)
    : httpClient(httpClient),
    configManager(configManager),
    downloading(false),
    downloadedBytes(0),
    totalBytes(0) {
    currentExePath=GetCurrentExePath();
}
bool SelfUpdater::DownloadNewLauncher(const std::string& downloadUrl,
    const std::string& expectedHash,
    const std::string& expectedVersion) {

    downloading=true;
    downloadedBytes=0;
    totalBytes=0;

    try {
        std::filesystem::path tempPath=std::filesystem::temp_directory_path();
        DWORD pid=GetCurrentProcessId();
        tempExePath=(tempPath/("mc_updater_new_"+std::to_string(pid)+".exe")).string();

        g_logger<<"[INFO] 开始下载新启动器: "<<downloadUrl
            <<" (版本: "<<expectedVersion<<")"<<std::endl;

        if(!httpClient.DownloadFile(downloadUrl,tempExePath)) {
            g_logger<<"[ERROR] 下载启动器失败"<<std::endl;
            downloading=false;
            return false;
        }

        std::error_code ec;
        auto fileSize=std::filesystem::file_size(tempExePath,ec);
        if(ec||fileSize==0) {
            g_logger<<"[ERROR] 下载的文件无效或为空，大小: "<<fileSize<<std::endl;
            std::filesystem::remove(tempExePath);
            downloading=false;
            return false;
        }

        if(fileSize<1024) {
            g_logger<<"[WARN] 下载的文件大小异常（小于1KB），可能是错误页面，文件大小: "<<fileSize<<" 字节"<<std::endl;
            g_logger<<"[WARN] 文件内容预览: ";
            std::ifstream testFile(tempExePath,std::ios::binary);
            if(testFile) {
                char buffer[256];
                testFile.read(buffer,255);
                buffer[testFile.gcount()]='\0';
                g_logger<<buffer<<std::endl;
            }
            testFile.close();
            std::filesystem::remove(tempExePath);
            downloading=false;
            return false;
        }

        g_logger<<"[INFO] 启动器下载完成，大小: "<<fileSize<<" 字节"<<std::endl;
        g_logger<<"[DEBUG] 文件路径: "<<tempExePath<<std::endl;

        if(!expectedHash.empty()) {
            g_logger<<"[INFO] 开始验证下载文件的哈希值..."<<std::endl;

            size_t colonPos=expectedHash.find(':');
            std::string hashAlgorithm=(colonPos!=std::string::npos)?
                expectedHash.substr(0,colonPos):"md5";
            std::string expectedHashValue=(colonPos!=std::string::npos)?
                expectedHash.substr(colonPos+1):expectedHash;

            g_logger<<"[DEBUG] 使用算法: "<<hashAlgorithm
                <<", 期望哈希: "<<expectedHashValue<<std::endl;

            std::string actualHash=FileHasher::CalculateFileHashStream(tempExePath,hashAlgorithm);

            if(actualHash.empty()) {
                g_logger<<"[ERROR] 无法计算文件的哈希值"<<std::endl;
                std::filesystem::remove(tempExePath);
                downloading=false;
                return false;
            }

            g_logger<<"[DEBUG] 实际计算哈希: "<<actualHash<<std::endl;

            if(actualHash!=expectedHashValue) {
                g_logger<<"[ERROR] 文件哈希不匹配！更新中止。"<<std::endl;
                g_logger<<"[ERROR] 期望: "<<expectedHashValue<<std::endl;
                g_logger<<"[ERROR] 实际: "<<actualHash<<std::endl;
                std::filesystem::remove(tempExePath);
                downloading=false;
                return false;
            }
            else {
                g_logger<<"[INFO] 文件哈希验证通过。"<<std::endl;
            }
        }
        else {
            g_logger<<"[WARN] 未提供文件哈希，跳过校验"<<std::endl;
        }

        downloading=false;
        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 下载启动器异常: "<<e.what()<<std::endl;
        downloading=false;
        return false;
    }
}

bool SelfUpdater::ApplyUpdate() {
    if(!std::filesystem::exists(tempExePath)) {
        g_logger<<"[ERROR] 临时文件不存在: "<<tempExePath<<std::endl;
        return false;
    }

    std::wstring curExe=GetCurrentExePathW();
    std::wstring newExe=FileSystemHelper::Utf8ToWide(tempExePath);

    CleanupOldBackup(curExe);
    if(TryNormalReplace(newExe,curExe)) {
        if(LaunchNewProcess(curExe)) {
            return true;
        }
        else {
            g_logger<<"[ERROR] 新进程启动失败，尝试回滚..."<<std::endl;
            return false;
        }
    }

    g_logger<<"[INFO] 普通权限不足，尝试提权替换..."<<std::endl;
    if(RunElevatedReplace(newExe,curExe)) {
        g_logger<<"[INFO] 提权替换成功，即将退出当前进程"<<std::endl;
        return true;
    }

    g_logger<<"[ERROR] 所有替换方式均失败"<<std::endl;
    return false;
}

std::string SelfUpdater::GetCurrentExePath() {
    char buffer[MAX_PATH];
    GetModuleFileName(NULL,buffer,MAX_PATH);
    return std::string(buffer);
}