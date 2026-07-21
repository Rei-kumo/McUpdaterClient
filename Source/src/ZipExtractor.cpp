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

	LOG_INFO("将ZIP数据写入临时文件: {}",tempZip);

    std::ofstream tempFile(tempZip,std::ios::binary);
    if(!tempFile) {
        LOG_ERROR("无法创建临时ZIP文件: {}", tempZip);
        return false;
    }

    tempFile.write(reinterpret_cast<const char*>(zipData.data()),zipData.size());
    tempFile.close();
    bool result=ExtractZipFromFile(tempZip,extractPath);
    std::error_code ec;
    std::filesystem::remove(tempZip,ec);
    if(ec) {
        LOG_WARN("无法删除临时文件: {} - {}", tempZip, ec.message());
    }

    return result;
}
bool ZipExtractor::ExtractZipFromFile(const std::string& zipFilePath,const std::string& extractPath) {
    LOG_INFO("开始解压文件: {} 到 {}", zipFilePath, extractPath);

    std::error_code ec;
    if(!std::filesystem::exists(zipFilePath,ec)) {
        if(ec) {
            LOG_ERROR("检查ZIP文件存在性失败: {}", ec.message());
        }
        else {
            LOG_ERROR("ZIP 文件不存在: {}", zipFilePath);
        }
        return false;
    }

    auto fileSize=std::filesystem::file_size(zipFilePath,ec);
    if(ec) {
        LOG_ERROR("无法获取ZIP文件大小: {}", ec.message());
        return false;
    }
    if(fileSize==0) {
        LOG_ERROR("ZIP 文件为空: {}", zipFilePath);
        return false;
    }

    LOG_INFO("ZIP 文件大小: {}", pRepoter.FormatBytes(fileSize));

    return ExtractZipWithMiniz(zipFilePath,extractPath);
}
bool ZipExtractor::ExtractZipWithMiniz(const std::string& zipFilePath,const std::string& extractPath) {
    LOG_INFO("调用 miniz 解压方法...");
    return ExtractZipSimple(zipFilePath,extractPath);
}

bool ZipExtractor::ExtractZipSimple(const std::string& zipFilePath,const std::string& extractPath) {
    LOG_INFO("使用简单解压方法...");

    LOG_INFO("尝试使用原始 libzip 解压...");
    if(ExtractZipOriginal(zipFilePath,extractPath)) {
        LOG_INFO("libzip 解压成功");
        if(fsHelper.ValidateExtraction(extractPath)) {
            return true;
        }
        else {
            LOG_WARN("libzip 解压验证失败");
            fsHelper.CleanupTempExtractDir(extractPath);
            return false;
        }
    }
    else {
        LOG_ERROR("libzip 解压失败");
        fsHelper.CleanupTempExtractDir(extractPath);
        return false;
    }
}
bool ZipExtractor::ExtractZipOriginal(const std::string& zipFilePath,const std::string& extractPath) {
    LOG_INFO("使用原始libzip解压...");

    int err=0;
    zip_t* zip=zip_open(zipFilePath.c_str(),0,&err);
    if(!zip) {
        LOG_ERROR("无法打开ZIP文件: {}，错误码: {}", zipFilePath, err);
        return false;
    }

    zip_int64_t numEntries=zip_get_num_entries(zip,0);
    LOG_INFO("总共 {} 个条目需要解压", numEntries);

    if(numEntries<=0) {
        LOG_ERROR("ZIP文件为空");
        zip_close(zip);
        return false;
    }
    LOG_INFO("第一步：创建目录结构...");

    std::vector<std::string> fileEntries;

    for(zip_int64_t i=0; i<numEntries; i++) {
        const char* name=zip_get_name(zip,i,ZIP_FL_ENC_UTF_8);
        if(!name) {
            name=zip_get_name(zip,i,0);
            if(!name) {
                LOG_WARN("无法获取文件 {} 的文件名", i);
                continue;
            }
        }
        std::string originalName=name;
        std::string safeName=originalName;
        std::wstring wideName=fsHelper.Utf8ToWide(originalName);
        if(wideName.empty()) {
            LOG_WARN("无法转换文件名: {}", originalName);
            safeName="file_"+std::to_string(i)+".dat";
            LOG_INFO("使用替代文件名: {}", safeName);
        }
        std::string fullPath;
        std::wstring wideExtractPath=fsHelper.Utf8ToWide(extractPath);
        if(!wideExtractPath.empty()&&!wideName.empty()) {
            std::wstring safeFullPath;
            try {
                safeFullPath=FileSystemHelper::SecureCombineW(wideExtractPath,wideName);
            }
            catch(const std::exception& e) {
                LOG_ERROR("ZIP目录路径遍历被阻止: {} (条目: {})", e.what(), originalName);
                continue;
            }
            fullPath=fsHelper.WideToUtf8(safeFullPath);
            if(!wideName.empty()&&wideName.back()==L'/') {
                try {
                    std::filesystem::path dirPath=safeFullPath;
                    std::filesystem::create_directories(dirPath);

                    if(i%50==0) {
                        LOG_DEBUG("创建目录: {}", originalName);
                    }
                }
                catch(const std::exception& e) {
                    LOG_WARN("无法创建目录 {}: {}", originalName, e.what());
                }
                continue;
            }
        }
        else {
            try {
                fullPath=FileSystemHelper::SecureCombine(extractPath,safeName);
            }
            catch(const std::exception& e) {
				LOG_ERROR("ZIP路径遍历被阻止: {} (条目: {})",e.what(),originalName);
                continue;
            }
        }
        if(!safeName.empty()&&safeName.back()=='/') {
            try {
                std::filesystem::create_directories(fullPath);
                if(i%50==0) {
                    LOG_DEBUG("创建目录: {}", originalName);
                }
            }
            catch(const std::exception& e) {
                LOG_WARN("无法创建目录 {}: {}", originalName, e.what());
            }
            continue;
        }
        fileEntries.push_back(originalName);
    }

    LOG_INFO("第二步：解压 {} 个文件...", fileEntries.size());
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
                LOG_WARN("无法找到文件索引: {}", originalName);
                failedFiles++;
                continue;
            }
        }

        zip_file_t* zfile=zip_fopen_index(zip,index,0);
        if(!zfile) {
            LOG_WARN("无法打开文件: {}", originalName);
            failedFiles++;
            continue;
        }
        struct ZipFileCloser {
            zip_file_t* file;
            ~ZipFileCloser() { if(file) zip_fclose(file); }
        } closer{zfile};

        std::string safeName=originalName;
        std::string fullPath;
        std::wstring wideExtractPath=fsHelper.Utf8ToWide(extractPath);
        std::wstring wideName=fsHelper.Utf8ToWide(originalName);

        if(!wideExtractPath.empty()&&!wideName.empty()) {
            std::wstring safeFullPath;
            try {
                safeFullPath=FileSystemHelper::SecureCombineW(wideExtractPath,wideName);
            }
            catch(const std::exception& e) {
				LOG_ERROR("ZIP路径遍历被阻止: {} (条目: {})",e.what(),originalName);
                failedFiles++;
                unicodeFailedFiles++;
                continue;
            }
            fullPath=fsHelper.WideToUtf8(safeFullPath);
            std::filesystem::path filePath=safeFullPath;
            std::filesystem::create_directories(filePath.parent_path());
            FILE* outFile=_wfopen(safeFullPath.c_str(),L"wb");
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
                LOG_ERROR("无法创建文件: {} (错误码: {})", originalName, error);
                failedFiles++;
                unicodeFailedFiles++;
                std::string asciiName="file_"+std::to_string(extractedFiles+failedFiles)+".dat";
                std::string asciiFullPath;
                try {
                    asciiFullPath=FileSystemHelper::SecureCombine(extractPath,asciiName);
                }
                catch(const std::exception& e) {
                    LOG_ERROR("ASCII后备路径遍历被阻止: {} (条目: {})", e.what(), originalName);
                    failedFiles++;
                    continue;
                }

                LOG_INFO("尝试使用ASCII名称: {}", asciiName);

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
                        LOG_INFO("文件 {} 保存为 {}", originalName, asciiName);
                    }
                }
            }
        }
        else {
            LOG_WARN("无法处理Unicode文件名: {}", originalName);
            failedFiles++;
            unicodeFailedFiles++;
        }
        zip_fclose(zfile);
        if((extractedFiles+failedFiles)%100==0) {
            int processed=extractedFiles+failedFiles;
            int percent=static_cast<int>((processed*100)/(std::max)(totalFiles,1));
            std::cout<<"\r解压进度: "<<processed<<"/"<<totalFiles<<" 文件 ("<<percent<<"%)，成功: "<<extractedFiles<<"，失败: "<<failedFiles<<"      ";
            std::cout.flush();
            LOG_INFO("已处理 {} / {} 个文件 ({}%)", processed, totalFiles, percent);
        }
        if(failedFiles>=20&&idx>100) {
            LOG_ERROR("失败文件过多，停止解压 (总失败: {}, Unicode失败: {})", failedFiles, unicodeFailedFiles);
            break;
        }
    }

    zip_close(zip);

    std::cout<<"\r解压完成: "<<extractedFiles<<"/"<<totalFiles<<" 个文件已提取，失败: "<<failedFiles<<" (Unicode失败: "<<unicodeFailedFiles<<")                  "<<std::endl;
    LOG_INFO("解压完成: {} / {} 个文件已提取，失败: {}", extractedFiles, totalFiles, failedFiles);

    if(unicodeFailedFiles>0) {
        LOG_WARN("{} 个文件因Unicode编码问题未能正确提取", unicodeFailedFiles);
        LOG_WARN("建议检查系统区域设置或使用英文文件名");
    }
    float successRate=(totalFiles>0)?(extractedFiles*100.0f/totalFiles):0.0f;
	LOG_INFO("解压成功率: {:.1f}%",successRate);
    if(successRate<80.0f) {
        LOG_WARN("解压成功率较低，可能需要手动检查");
        return false;
    }

    return extractedFiles>0;
}
bool ZipExtractor::IsValidZipFile(const std::string& filePath) {
    std::ifstream file(filePath,std::ios::binary);
    if(!file) {
        LOG_DEBUG("无法打开文件: {}", filePath);
        return false;
    }

    file.seekg(0,std::ios::end);
    size_t fileSize=file.tellg();
    file.seekg(0,std::ios::beg);

    if(fileSize<22) {
        LOG_DEBUG("文件太小 ({} 字节)，可能是空ZIP文件", fileSize);

        if(fileSize==0) {
            return true;
        }

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(),fileSize);

        if(fileSize==22) {
            if(buffer[0]==0x50&&buffer[1]==0x4B&&
                buffer[2]==0x05&&buffer[3]==0x06) {
                LOG_DEBUG("有效的空ZIP文件（只有目录结束标记）");
                return true;
            }
        }

        return false;
    }

    char header[4];
    file.read(header,4);

    if(file.gcount()<4) {
        LOG_DEBUG("无法读取文件头");
        return false;
    }
    bool isZipSignature=(header[0]==0x50&&header[1]==0x4B&&
        header[2]==0x03&&header[3]==0x04);

    if(!isZipSignature) {
        LOG_DEBUG("文件头不是有效的ZIP签名: {:02x} {:02x} {:02x} {:02x}",
            static_cast<unsigned char>(header[0]),
            static_cast<unsigned char>(header[1]),
            static_cast<unsigned char>(header[2]),
            static_cast<unsigned char>(header[3]));

        if(fileSize==0) {
            LOG_DEBUG("空文件，可能是空目录");
            return true;
        }
        return false;
    }

    LOG_DEBUG("有效的ZIP文件签名，文件大小: {}", pRepoter.FormatBytes(fileSize));
    return true;
}
//zhihouyizou
bool ZipExtractor::CheckServerResponse(const std::string& url) {
    LOG_DEBUG("检查服务器响应: {}", url);

    try {
        std::string tempFile=std::filesystem::temp_directory_path().string()+"/test_response.bin";

        if(!httpClient.DownloadFileWithProgress(url,tempFile,nullptr,nullptr)) {
            LOG_DEBUG("服务器响应测试失败");
            return false;
        }

        std::error_code ec;
        auto fileSize=std::filesystem::file_size(tempFile,ec);
        std::filesystem::remove(tempFile);

        if(ec||fileSize==0) {
            LOG_DEBUG("服务器返回空文件或错误");
            return false;
        }

        LOG_DEBUG("服务器响应正常，文件大小: {}", pRepoter.FormatBytes(fileSize));
        return true;
    }
    catch(const std::exception& e) {
        LOG_DEBUG("检查服务器响应异常: {}", e.what());
        return false;
    }
}
bool ZipExtractor::DownloadAndExtract(const std::string& url,const std::string& relativePath,const std::string& targetBaseDir) {
    LOG_DEBUG("下载并解压: {} -> {}", url, relativePath);

    DWORD pid=GetCurrentProcessId();
    auto timestamp=std::chrono::steady_clock::now().time_since_epoch().count();
    std::string tempZip=(std::filesystem::temp_directory_path()/
        ("minecraft_update_"+std::to_string(pid)+"_"+std::to_string(timestamp)+".zip")).string();

    std::string progressMessage="下载 "+relativePath;
    pRepoter.show(progressMessage,0,1);

    bool downloadSuccess=httpClient.DownloadFileWithProgress(
        url,
        tempZip,
        [this,progressMessage](long long downloaded,long long total,void* userdata) {
            pRepoter.show(progressMessage,downloaded,total);
        },
        nullptr
    );

    pRepoter.clear();

    if(!downloadSuccess) {
        LOG_ERROR("下载失败: {}", url);
        return false;
    }
    std::error_code ec;
    auto fileSize=std::filesystem::file_size(tempZip,ec);
    if(ec) {
        LOG_ERROR("无法获取文件大小: {}", ec.message());
        std::filesystem::remove(tempZip);
        return false;
    }

    LOG_INFO("下载完成，文件大小: {}", pRepoter.FormatBytes(fileSize));
    if(fileSize<1024) {
        std::ifstream file(tempZip,std::ios::binary);
        if(file) {
            std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            file.close();
            if(content.find("404")!=std::string::npos||
                content.find("Not Found")!=std::string::npos||
                content.find("Error")!=std::string::npos) {

                LOG_INFO("服务器返回错误页面，可能是空文件夹，将创建空目录");
                LOG_DEBUG("服务器响应: {}", content);
                std::filesystem::remove(tempZip);
                std::string extractPath;
                try {
                    extractPath=FileSystemHelper::SecureCombine(targetBaseDir,relativePath);
                }
                catch(const std::exception& e) {
                    LOG_ERROR("下载并解压中的路径遍历被阻止: {}", e.what());
                    std::filesystem::remove(tempZip);
                    return false;
                }
                try {
                    if(!std::filesystem::exists(extractPath)) {
                        std::filesystem::create_directories(extractPath);
                        LOG_INFO("已创建空目录: {}", extractPath);
                    }
                    else {
                        LOG_INFO("目录已存在: {}", extractPath);
                    }
                    return true;
                }
                catch(const std::exception& e) {
                    LOG_ERROR("创建目录失败: {}", e.what());
                    return false;
                }
            }
        }
    }
    if(!IsValidZipFile(tempZip)) {
        LOG_ERROR("下载的文件不是有效的ZIP文件，大小: {}", pRepoter.FormatBytes(fileSize));
        if(fileSize<1024) {
            std::ifstream file(tempZip,std::ios::binary);
            if(file) {
                std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                LOG_DEBUG("文件内容: {}", content);
            }
            file.close();
        }

        std::filesystem::remove(tempZip);
        return false;
    }
    std::string extractPath;
    try {
        extractPath=FileSystemHelper::SecureCombine(targetBaseDir,relativePath);
    }
    catch(const std::exception& e) {
		LOG_ERROR("路径遍历被阻止: {}",e.what());
        return false;
    }
    if(std::filesystem::exists(extractPath)) {
        LOG_INFO("备份原有目录...");
        fsHelper.BackupFile(extractPath);
    }
    bool extractSuccess=ExtractZipOriginal(tempZip,extractPath);
    std::filesystem::remove(tempZip);

    if(!extractSuccess) {
        LOG_ERROR("解压失败");
        return false;
    }

    return true;
}