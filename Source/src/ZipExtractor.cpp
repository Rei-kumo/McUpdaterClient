#include "ZipExtractor.h"
#include "FileSystemHelper.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <zip.h>
#include <regex>
#include <cmath>
#include <set>
#include "SelfUpdater.h"
#include <thread>
#include <iomanip>
#include <sstream>
#include <queue>
#include <map>
#include <algorithm>
#include <mutex>
#include <memory>
#include "FileHasher.h"
#include "UpdateChecker.h"
#include <fcntl.h>
#include <io.h>
#include <windows.h>
ZipExtractor::ZipExtractor(HttpClient& http,ProgressReporter& reporter)
    : httpClient(http),pRepoter(reporter) {
}
bool ZipExtractor::ExtractZip(const std::vector<unsigned char>& zipData,const std::string& extractPath) {
    fsHelper.EnsureDirectoryExists(extractPath);
    std::string tempDir=std::filesystem::temp_directory_path().string();
    std::string tempZip=tempDir+"/minecraft_update_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".zip";

    g_logger<<"[INFO] 将ZIP数据写入临时文件: "<<tempZip<<std::endl;

    std::ofstream tempFile(tempZip,std::ios::binary);
    if(!tempFile) {
        g_logger<<"[ERROR] 无法创建临时ZIP文件: "<<tempZip<<std::endl;
        return false;
    }

    tempFile.write(reinterpret_cast<const char*>(zipData.data()),zipData.size());
    tempFile.close();
    bool result=ExtractZipFromFile(tempZip,extractPath);
    std::error_code ec;
    std::filesystem::remove(tempZip,ec);
    if(ec) {
        g_logger<<"[WARN] 无法删除临时文件: "<<tempZip<<" - "<<ec.message()<<std::endl;
    }

    return result;
}
bool ZipExtractor::ExtractZipFromFile(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 开始解压文件: "<<zipFilePath<<" 到 "<<extractPath<<std::endl;

    if(!std::filesystem::exists(zipFilePath)) {
        g_logger<<"[ERROR] ZIP 文件不存在: "<<zipFilePath<<std::endl;
        return false;
    }

    std::error_code ec;
    auto fileSize=std::filesystem::file_size(zipFilePath,ec);
    if(ec||fileSize==0) {
        g_logger<<"[ERROR] ZIP 文件无效或为空: "<<zipFilePath<<std::endl;
        return false;
    }

    g_logger<<"[INFO] ZIP 文件大小: "<<pRepoter.FormatBytes(fileSize)<<std::endl;

    return ExtractZipWithMiniz(zipFilePath,extractPath);
}
bool ZipExtractor::ExtractZipWithMiniz(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 调用 miniz 解压方法..."<<std::endl;
    return ExtractZipSimple(zipFilePath,extractPath);
}

bool ZipExtractor::ExtractZipSimple(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 使用简单解压方法..."<<std::endl;

    g_logger<<"[INFO] 尝试使用 Windows 系统命令解压..."<<std::endl;
    if(ExtractZipWithSystemCommand(zipFilePath,extractPath)) {
        g_logger<<"[INFO] Windows 系统命令解压成功"<<std::endl;
        if(fsHelper.ValidateExtraction(extractPath)) {
            return true;
        }
        else {
            g_logger<<"[WARN] Windows 系统命令解压验证失败，尝试备用方案"<<std::endl;
            fsHelper.CleanupTempExtractDir(extractPath);
        }
    }
    else {
        g_logger<<"[WARN] Windows 系统命令解压失败，尝试备用方案"<<std::endl;
    }

    g_logger<<"[INFO] 尝试使用原始 libzip 解压..."<<std::endl;
    if(ExtractZipOriginal(zipFilePath,extractPath)) {
        g_logger<<"[INFO] libzip 解压成功"<<std::endl;
        if(fsHelper.ValidateExtraction(extractPath)) {
            return true;
        }
        else {
            g_logger<<"[WARN] libzip 解压验证失败"<<std::endl;
            fsHelper.CleanupTempExtractDir(extractPath);
            return false;
        }
    }
    else {
        g_logger<<"[ERROR] libzip 解压失败"<<std::endl;
        fsHelper.CleanupTempExtractDir(extractPath);
        return false;
    }
}

bool ZipExtractor::ExtractZipWithSystemCommand(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 使用 Windows 系统命令解压..."<<std::endl;
    if(!std::filesystem::exists(zipFilePath)) {
        g_logger<<"[ERROR] ZIP 文件不存在: "<<zipFilePath<<std::endl;
        return false;
    }
    fsHelper.EnsureDirectoryExists(extractPath);

    std::string command;
    command="powershell -Command \"Expand-Archive -Path '"+zipFilePath+
        "' -DestinationPath '"+extractPath+"' -Force -ErrorAction Stop\"";

    g_logger<<"[DEBUG] 执行命令: "<<command<<std::endl;

    STARTUPINFOA si={0};
    PROCESS_INFORMATION pi={0};
    si.cb=sizeof(si);
    si.dwFlags=STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE;

    if(CreateProcessA(NULL,(LPSTR)command.c_str(),NULL,NULL,FALSE,
        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
        NULL,NULL,&si,&pi)) {
        DWORD waitResult=WaitForSingleObject(pi.hProcess,60000);

        DWORD exitCode=1;
        if(waitResult==WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess,&exitCode);
        }
        else if(waitResult==WAIT_TIMEOUT) {
            g_logger<<"[ERROR] 解压命令超时"<<std::endl;
            TerminateProcess(pi.hProcess,1);
            exitCode=1;
        }
        else {
            g_logger<<"[ERROR] 等待解压进程失败，等待结果: "<<waitResult<<std::endl;
            exitCode=1;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if(exitCode==0) {
            g_logger<<"[INFO] Windows 系统命令解压成功"<<std::endl;
            return true;
        }
        else {
            g_logger<<"[ERROR] Windows 系统命令解压失败，退出代码: "<<exitCode<<std::endl;
            return false;
        }
    }
    else {
        DWORD error=GetLastError();
        g_logger<<"[ERROR] 无法创建解压进程，错误代码: "<<error<<std::endl;
        return false;
    }
}
bool ZipExtractor::ExtractZipOriginal(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 使用原始libzip解压..."<<std::endl;

    int err=0;
    zip_t* zip=zip_open(zipFilePath.c_str(),0,&err);
    if(!zip) {
        g_logger<<"[ERROR] 无法打开ZIP文件: "<<zipFilePath<<"，错误码: "<<err<<std::endl;
        return false;
    }

    zip_int64_t numEntries=zip_get_num_entries(zip,0);
    g_logger<<"[INFO] 总共 "<<numEntries<<" 个条目需要解压"<<std::endl;

    if(numEntries<=0) {
        g_logger<<"[ERROR] ZIP文件为空"<<std::endl;
        zip_close(zip);
        return false;
    }
    g_logger<<"[INFO] 第一步：创建目录结构..."<<std::endl;

    std::vector<std::string> fileEntries;

    for(zip_int64_t i=0; i<numEntries; i++) {
        const char* name=zip_get_name(zip,i,ZIP_FL_ENC_UTF_8);
        if(!name) {
            name=zip_get_name(zip,i,0);
            if(!name) {
                g_logger<<"[WARN] 无法获取文件 "<<i<<" 的文件名"<<std::endl;
                continue;
            }
        }
        std::string originalName=name;
        std::string safeName=originalName;
        std::wstring wideName=fsHelper.Utf8ToWide(originalName);
        if(wideName.empty()) {
            g_logger<<"[WARN] 无法转换文件名: "<<originalName<<std::endl;
            safeName="file_"+std::to_string(i)+".dat";
            g_logger<<"[INFO] 使用替代文件名: "<<safeName<<std::endl;
        }
        std::string fullPath;
        std::wstring wideExtractPath=fsHelper.Utf8ToWide(extractPath);
        if(!wideExtractPath.empty()&&!wideName.empty()) {
            std::wstring wideFullPath=wideExtractPath+L"/"+wideName;
            fullPath=fsHelper.WideToUtf8(wideFullPath);
            if(!wideName.empty()&&wideName.back()==L'/') {
                try {
                    std::filesystem::path dirPath=wideFullPath;
                    std::filesystem::create_directories(dirPath);

                    if(i%50==0) {
                        g_logger<<"[DEBUG] 创建目录: "<<originalName<<std::endl;
                    }
                }
                catch(const std::exception& e) {
                    g_logger<<"[WARN] 无法创建目录 "<<originalName<<": "<<e.what()<<std::endl;
                }
                continue;
            }
        }
        else {
            fullPath=extractPath+"/"+safeName;
        }
        fullPath=extractPath+"/"+safeName;
        if(!safeName.empty()&&safeName.back()=='/') {
            try {
                std::filesystem::create_directories(fullPath);
                if(i%50==0) {
                    g_logger<<"[DEBUG] 创建目录: "<<originalName<<std::endl;
                }
            }
            catch(const std::exception& e) {
                g_logger<<"[WARN] 无法创建目录 "<<originalName<<": "<<e.what()<<std::endl;
            }
            continue;
        }
        fileEntries.push_back(originalName);
    }

    g_logger<<"[INFO] 第二步：解压 "<<fileEntries.size()<<" 个文件..."<<std::endl;
    const size_t bufferSize=65536;
    std::vector<char> buffer(bufferSize);
    int extractedFiles=0;
    int failedFiles=0;
    int unicodeFailedFiles=0;
    int totalFiles=static_cast<int>(fileEntries.size());

    for(size_t idx=0; idx<fileEntries.size(); idx++) {
        const auto& originalName=fileEntries[idx];
        zip_int64_t index=zip_name_locate(zip,originalName.c_str(),ZIP_FL_ENC_UTF_8);
        if(index<0) {
            index=zip_name_locate(zip,originalName.c_str(),0);
            if(index<0) {
                g_logger<<"[WARN] 无法找到文件索引: "<<originalName<<std::endl;
                failedFiles++;
                continue;
            }
        }

        zip_file_t* zfile=zip_fopen_index(zip,index,0);
        if(!zfile) {
            g_logger<<"[WARN] 无法打开文件: "<<originalName<<std::endl;
            failedFiles++;
            continue;
        }

        std::string safeName=originalName;
        std::string fullPath;
        std::wstring wideExtractPath=fsHelper.Utf8ToWide(extractPath);
        std::wstring wideName=fsHelper.Utf8ToWide(originalName);

        if(!wideExtractPath.empty()&&!wideName.empty()) {
            std::wstring wideFullPath=wideExtractPath+L"/"+wideName;
            fullPath=fsHelper.WideToUtf8(wideFullPath);
            std::filesystem::path filePath=wideFullPath;
            std::filesystem::create_directories(filePath.parent_path());
            FILE* outFile=_wfopen(wideFullPath.c_str(),L"wb");
            if(outFile) {
                zip_int64_t bytesRead;
                long long totalBytes=0;
                while((bytesRead=zip_fread(zfile,buffer.data(),bufferSize))>0) {
                    size_t written=fwrite(buffer.data(),1,(size_t)bytesRead,outFile);
                    totalBytes+=bytesRead;
                }

                fclose(outFile);
                extractedFiles++;
            }
            else {
                DWORD error=GetLastError();
                g_logger<<"[ERROR] 无法创建文件: "<<originalName<<" (错误码: "<<error<<")"<<std::endl;
                failedFiles++;
                unicodeFailedFiles++;
                std::string asciiName="file_"+std::to_string(extractedFiles+failedFiles)+".dat";
                std::string asciiFullPath=extractPath+"/"+asciiName;

                g_logger<<"[INFO] 尝试使用ASCII名称: "<<asciiName<<std::endl;

                std::ofstream asciiFile(asciiFullPath,std::ios::binary);
                if(asciiFile) {
                    zip_int64_t bytesRead;
                    long long totalBytes=0;
                    zip_fclose(zfile);
                    zfile=zip_fopen_index(zip,index,0);

                    if(zfile) {
                        while((bytesRead=zip_fread(zfile,buffer.data(),bufferSize))>0) {
                            asciiFile.write(buffer.data(),bytesRead);
                            totalBytes+=bytesRead;
                        }
                        asciiFile.close();
                        extractedFiles++;
                        g_logger<<"[INFO] 文件 "<<originalName<<" 保存为 "<<asciiName<<std::endl;
                    }
                }
            }
        }
        else {
            g_logger<<"[WARN] 无法处理Unicode文件名: "<<originalName<<std::endl;
            failedFiles++;
            unicodeFailedFiles++;
        }
        zip_fclose(zfile);
        if((extractedFiles+failedFiles)%100==0) {
            int processed=extractedFiles+failedFiles;
            int percent=static_cast<int>((processed*100)/(std::max)(totalFiles,1));
            std::cout<<"\r解压进度: "<<processed<<"/"<<totalFiles<<" 文件 ("<<percent<<"%)，成功: "<<extractedFiles<<"，失败: "<<failedFiles<<"      ";
            std::cout.flush();
            g_logger<<"[INFO] 已处理 "<<processed<<"/"<<totalFiles<<" 个文件 ("<<percent<<"%)"<<std::endl;
        }
        if(failedFiles>=20&&idx>100) {
            g_logger<<"[ERROR] 失败文件过多，停止解压 (总失败: "<<failedFiles<<", Unicode失败: "<<unicodeFailedFiles<<")"<<std::endl;
            break;
        }
    }

    zip_close(zip);

    std::cout<<"\r解压完成: "<<extractedFiles<<"/"<<totalFiles<<" 个文件已提取，失败: "<<failedFiles<<" (Unicode失败: "<<unicodeFailedFiles<<")                  "<<std::endl;
    g_logger<<"[INFO] 解压完成: "<<extractedFiles<<"/"<<totalFiles<<" 个文件已提取，失败: "<<failedFiles<<std::endl;

    if(unicodeFailedFiles>0) {
        g_logger<<"[WARN] "<<unicodeFailedFiles<<" 个文件因Unicode编码问题未能正确提取"<<std::endl;
        g_logger<<"[WARN] 建议检查系统区域设置或使用英文文件名"<<std::endl;
    }
    float successRate=(totalFiles>0)?(extractedFiles*100.0f/totalFiles):0.0f;
    g_logger<<"[INFO] 解压成功率: "<<std::fixed<<std::setprecision(1)<<successRate<<"%"<<std::endl;
    if(successRate<80.0f) {
        g_logger<<"[WARN] 解压成功率较低，可能需要手动检查"<<std::endl;
        return false;
    }

    return extractedFiles>0;
}
bool ZipExtractor::IsValidZipFile(const std::string& filePath) {
    std::ifstream file(filePath,std::ios::binary);
    if(!file) {
        g_logger<<"[DEBUG] 无法打开文件: "<<filePath<<std::endl;
        return false;
    }

    file.seekg(0,std::ios::end);
    size_t fileSize=file.tellg();
    file.seekg(0,std::ios::beg);

    if(fileSize<22) {
        g_logger<<"[DEBUG] 文件太小 ("<<fileSize<<" 字节)，可能是空ZIP文件"<<std::endl;

        if(fileSize==0) {
            return true;
        }

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(),fileSize);

        if(fileSize==22) {
            if(buffer[0]==0x50&&buffer[1]==0x4B&&
                buffer[2]==0x05&&buffer[3]==0x06) {
                g_logger<<"[DEBUG] 有效的空ZIP文件（只有目录结束标记）"<<std::endl;
                return true;
            }
        }

        return false;
    }

    char header[4];
    file.read(header,4);

    if(file.gcount()<4) {
        g_logger<<"[DEBUG] 无法读取文件头"<<std::endl;
        return false;
    }
    bool isZipSignature=(header[0]==0x50&&header[1]==0x4B&&
        header[2]==0x03&&header[3]==0x04);

    if(!isZipSignature) {
        g_logger<<"[DEBUG] 文件头不是有效的ZIP签名: ";
        for(int i=0; i<4; ++i) {
            g_logger<<std::hex<<(int)(unsigned char)header[i]<<" ";
        }
        g_logger<<std::dec<<std::endl;

        if(fileSize==0) {
            g_logger<<"[DEBUG] 空文件，可能是空目录"<<std::endl;
            return true;
        }

        return false;
    }

    g_logger<<"[DEBUG] 有效的ZIP文件签名，文件大小: "<<pRepoter.FormatBytes(fileSize)<<std::endl;
    return true;
}
//zhihouyizou
bool ZipExtractor::CheckServerResponse(const std::string& url) {
    g_logger<<"[DEBUG] 检查服务器响应: "<<url<<std::endl;

    try {
        std::string tempFile=std::filesystem::temp_directory_path().string()+"/test_response.bin";

        if(!httpClient.DownloadFileWithProgress(url,tempFile,nullptr,nullptr)) {
            g_logger<<"[DEBUG] 服务器响应测试失败"<<std::endl;
            return false;
        }

        std::error_code ec;
        auto fileSize=std::filesystem::file_size(tempFile,ec);
        std::filesystem::remove(tempFile);

        if(ec||fileSize==0) {
            g_logger<<"[DEBUG] 服务器返回空文件或错误"<<std::endl;
            return false;
        }

        g_logger<<"[DEBUG] 服务器响应正常，文件大小: "<<pRepoter.FormatBytes(fileSize)<<std::endl;
        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[DEBUG] 检查服务器响应异常: "<<e.what()<<std::endl;
        return false;
    }
}
bool ZipExtractor::DownloadAndExtract(const std::string& url,const std::string& relativePath,const std::string& targetBaseDir) {
    g_logger<<"[INFO] 下载并解压: "<<url<<" -> "<<relativePath<<std::endl;
    std::string tempDir=std::filesystem::temp_directory_path().string();
    std::string tempZip=tempDir+"/mc_temp_"+
        std::to_string(GetCurrentProcessId())+"_"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".zip";
    std::string progressMessage="下载 "+relativePath;
    pRepoter.ShowProgressBar(progressMessage,0,1);

    bool downloadSuccess=httpClient.DownloadFileWithProgress(
        url,
        tempZip,
        [this,progressMessage](long long downloaded,long long total,void* userdata) {
            pRepoter.ShowProgressBar(progressMessage,downloaded,total);
        },
        nullptr
    );

    pRepoter.ClearProgressLine();

    if(!downloadSuccess) {
        g_logger<<"[ERROR] 下载失败: "<<url<<std::endl;
        return false;
    }
    std::error_code ec;
    auto fileSize=std::filesystem::file_size(tempZip,ec);
    if(ec) {
        g_logger<<"[ERROR] 无法获取文件大小: "<<ec.message()<<std::endl;
        std::filesystem::remove(tempZip);
        return false;
    }

    g_logger<<"[INFO] 下载完成，文件大小: "<<pRepoter.FormatBytes(fileSize)<<std::endl;
    if(fileSize<1024) {
        std::ifstream file(tempZip,std::ios::binary);
        if(file) {
            std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            file.close();
            if(content.find("404")!=std::string::npos||
                content.find("Not Found")!=std::string::npos||
                content.find("Error")!=std::string::npos) {

                g_logger<<"[INFO] 服务器返回错误页面，可能是空文件夹，将创建空目录"<<std::endl;
                g_logger<<"[DEBUG] 服务器响应: "<<content<<std::endl;
                std::filesystem::remove(tempZip);
                std::filesystem::path gameDirPath=std::filesystem::absolute(targetBaseDir);
                std::string extractPath=(gameDirPath/relativePath).string();
                try {
                    if(!std::filesystem::exists(extractPath)) {
                        std::filesystem::create_directories(extractPath);
                        g_logger<<"[INFO] 已创建空目录: "<<extractPath<<std::endl;
                    }
                    else {
                        g_logger<<"[INFO] 目录已存在: "<<extractPath<<std::endl;
                    }
                    return true;
                }
                catch(const std::exception& e) {
                    g_logger<<"[ERROR] 创建目录失败: "<<e.what()<<std::endl;
                    return false;
                }
            }
        }
    }
    if(!IsValidZipFile(tempZip)) {
        g_logger<<"[ERROR] 下载的文件不是有效的ZIP文件，大小: "<<pRepoter.FormatBytes(fileSize)<<std::endl;
        if(fileSize<1024) {
            std::ifstream file(tempZip,std::ios::binary);
            if(file) {
                std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                g_logger<<"[DEBUG] 文件内容: "<<content<<std::endl;
            }
            file.close();
        }

        std::filesystem::remove(tempZip);
        return false;
    }
    std::filesystem::path gameDirPath=std::filesystem::absolute(targetBaseDir);
    std::string extractPath=(gameDirPath/relativePath).string();
    if(std::filesystem::exists(extractPath)) {
        g_logger<<"[INFO] 备份原有目录..."<<std::endl;
        fsHelper.BackupFile(extractPath);
    }
    bool extractSuccess=ExtractZipOriginal(tempZip,extractPath);
    std::filesystem::remove(tempZip);

    if(!extractSuccess) {
        g_logger<<"[ERROR] 解压失败"<<std::endl;
        return false;
    }

    return true;
}