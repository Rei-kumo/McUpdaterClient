#include "SelfUpdater.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <windows.h>
#include <limits.h>
#include <winver.h>
#include <thread>

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
        tempExePath=(tempPath/"mc_updater_new.exe").string();

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

            std::string actualHash=FileHasher::CalculateFileHash(tempExePath,hashAlgorithm);

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
    try {
        if(!std::filesystem::exists(tempExePath)) {
            g_logger<<"[ERROR] 临时启动器文件不存在"<<std::endl;
            return false;
        }

        g_logger<<"[INFO] 准备应用启动器更新..."<<std::endl;

        char buffer[MAX_PATH];
        std::string currentWorkingDir;
        if(GetCurrentDirectoryA(MAX_PATH,buffer)!=0) {
            currentWorkingDir=buffer;
            g_logger<<"[DEBUG] 当前工作目录: "<<currentWorkingDir<<std::endl;
        }
        else {
            currentWorkingDir=std::filesystem::path(currentExePath).parent_path().string();
            g_logger<<"[DEBUG] 使用可执行文件目录作为工作目录: "<<currentWorkingDir<<std::endl;
        }

        std::string batchPath=(std::filesystem::temp_directory_path()/"mc_update.bat").string();
        std::ofstream batchFile(batchPath);

        if(!batchFile) {
            g_logger<<"[ERROR] 无法创建更新脚本"<<std::endl;
            return false;
        }

        std::filesystem::path configPath=std::filesystem::path(currentWorkingDir)/"config/updater.json";

        batchFile<<"@echo off\n";
        batchFile<<"setlocal enabledelayedexpansion\n";
        batchFile<<"title McUpdater Updater\n";
        batchFile<<"echo ============================================\n";
        batchFile<<"echo       McUpdater 自更新\n";
        batchFile<<"echo ============================================\n";
        batchFile<<"\n";
        batchFile<<"set OLD_FILE=\""<<currentExePath<<"\"\n";
        batchFile<<"set NEW_FILE=\""<<tempExePath<<"\"\n";
        batchFile<<"set WORKING_DIR=\""<<currentWorkingDir<<"\"\n";
        batchFile<<"set CONFIG_FILE=\""<<configPath.string()<<"\"\n";
        batchFile<<"set LOG_FILE=\"%TEMP%\\mc_updater_update.log\"\n";
        batchFile<<"\n";
        batchFile<<"echo 原工作目录: !WORKING_DIR! > !LOG_FILE!\n";
        batchFile<<"echo 原程序路径: !OLD_FILE! >> !LOG_FILE!\n";
        batchFile<<"echo 新程序路径: !NEW_FILE! >> !LOG_FILE!\n";
        batchFile<<"echo 配置文件: !CONFIG_FILE! >> !LOG_FILE!\n";
        batchFile<<"\n";
        batchFile<<"echo 步骤 1/3: 等待原程序关闭...\n";
        batchFile<<"echo 等待原程序关闭... >> !LOG_FILE!\n";
        batchFile<<"timeout /t 3 /nobreak > nul\n";
        batchFile<<"\n";
        batchFile<<"echo 步骤 2/3: 替换文件...\n";
        batchFile<<"echo 尝试替换文件... >> !LOG_FILE!\n";
        batchFile<<"\n";
        batchFile<<":: 尝试多次删除原文件\n";
        batchFile<<"set retry_count=0\n";
        batchFile<<":delete_retry\n";
        batchFile<<"del \"!OLD_FILE!\" 2>nul\n";
        batchFile<<"if exist \"!OLD_FILE!\" (\n";
        batchFile<<"    echo 文件仍被占用，等待2秒后重试... >> !LOG_FILE!\n";
        batchFile<<"    echo 尝试结束相关进程... >> !LOG_FILE!\n";
        batchFile<<"    taskkill /f /im \"McUpdaterClient.exe\" >nul 2>&1\n";
        batchFile<<"    timeout /t 2 /nobreak > nul\n";
        batchFile<<"    set /a retry_count+=1\n";
        batchFile<<"    if !retry_count! leq 5 (\n";
        batchFile<<"        goto delete_retry\n";
        batchFile<<"    ) else (\n";
        batchFile<<"        echo 错误: 无法删除原文件，更新失败！ >> !LOG_FILE!\n";
        batchFile<<"        echo [错误] 无法删除原文件，更新失败！\n";
        batchFile<<"        pause\n";
        batchFile<<"        exit /b 1\n";
        batchFile<<"    )\n";
        batchFile<<")\n";
        batchFile<<"\n";
        batchFile<<"echo 原文件已删除，开始复制新文件... >> !LOG_FILE!\n";
        batchFile<<"copy \"!NEW_FILE!\" \"!OLD_FILE!\" >nul\n";
        batchFile<<"if errorlevel 1 (\n";
        batchFile<<"    echo [错误] 无法复制新文件！ >> !LOG_FILE!\n";
        batchFile<<"    echo [错误] 无法复制新文件！\n";
        batchFile<<"    pause\n";
        batchFile<<"    exit /b 1\n";
        batchFile<<")\n";
        batchFile<<"echo 文件替换成功！ >> !LOG_FILE!\n";
        batchFile<<"echo 文件替换成功！\n";
        batchFile<<"\n";
        batchFile<<"echo 步骤 3/3: 启动新版本...\n";
        batchFile<<"echo 启动新版本... >> !LOG_FILE!\n";
        batchFile<<"cd /d \"!WORKING_DIR!\"\n";
        batchFile<<"timeout /t 2 /nobreak > nul\n";
        batchFile<<"start \"\" /D \"!WORKING_DIR!\" \"!OLD_FILE!\"\n";
        batchFile<<"if errorlevel 1 (\n";
        batchFile<<"    echo 启动新程序失败，尝试直接执行... >> !LOG_FILE!\n";
        batchFile<<"    \"!OLD_FILE!\"\n";
        batchFile<<")\n";
        batchFile<<"\n";
        batchFile<<"echo 清理临时文件...\n";
        batchFile<<"echo 清理临时文件... >> !LOG_FILE!\n";
        batchFile<<"del \"!NEW_FILE!\" 2>nul\n";
        batchFile<<"del \"%~f0\" 2>nul\n";
        batchFile<<"\n";
        batchFile<<"echo 更新完成！新程序已启动。\n";
        batchFile<<"echo 更新完成！新程序已启动。 >> !LOG_FILE!\n";
        batchFile<<"timeout /t 3 /nobreak > nul\n";
        batchFile<<"exit\n";

        batchFile.close();

        g_logger<<"[DEBUG] 更新脚本已创建: "<<batchPath<<std::endl;

        SHELLEXECUTEINFO sei={sizeof(sei)};
        sei.lpVerb="runas";
        sei.lpFile=batchPath.c_str();
        sei.nShow=SW_HIDE;
        sei.fMask=SEE_MASK_NOCLOSEPROCESS;

        if(ShellExecuteEx(&sei)) {
            g_logger<<"[INFO] 已启动更新脚本（管理员权限）"<<std::endl;

            WaitForSingleObject(sei.hProcess,5000);
            CloseHandle(sei.hProcess);

            g_logger<<"[INFO] 主程序即将退出，等待更新脚本执行..."<<std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            return true;
        }
        else {
            DWORD err=GetLastError();
            g_logger<<"[ERROR] 无法以管理员权限执行更新脚本，错误码: "<<err<<std::endl;

            sei.lpVerb=NULL;
            sei.nShow=SW_HIDE;

            if(ShellExecuteEx(&sei)) {
                g_logger<<"[INFO] 已启动更新脚本（普通权限）"<<std::endl;

                WaitForSingleObject(sei.hProcess,5000);
                CloseHandle(sei.hProcess);

                g_logger<<"[INFO] 主程序即将退出..."<<std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                return true;
            }
            else {
                g_logger<<"[ERROR] 无法执行更新脚本"<<std::endl;
                return false;
            }
        }
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 应用更新失败: "<<e.what()<<std::endl;
        return false;
    }
}

std::string SelfUpdater::GetCurrentExePath() {
    char buffer[MAX_PATH];
    GetModuleFileName(NULL,buffer,MAX_PATH);
    return std::string(buffer);
}