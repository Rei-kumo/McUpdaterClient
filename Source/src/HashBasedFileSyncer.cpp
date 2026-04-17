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
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include "HashBasedFileSyncer.h"
#include "ConfigManager.h"
#include "UpdateOrchestrator.h"
#include "ZipExtractor.h"
HashBasedFileSyncer::HashBasedFileSyncer(HttpClient& http,
    UpdateOrchestrator& orc,
    ProgressReporter& reporter,
    FileSystemHelper& fs,
    ZipExtractor& zip,
    ConfigManager& config)
    : httpClient(http),
    updateOrchestrator(orc),
    progressReporter(reporter),
    fsHelper(fs),
    zipExtractor(zip),
    configManager(config)
{
}
bool HashBasedFileSyncer::CheckFileConsistency(const Json::Value& fileManifest,const Json::Value& directoryManifest) {
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();
    bool allFilesConsistent=true;
    int missingFiles=0;
    int mismatchedFiles=0;
    int totalChecked=0;

    g_logger<<"[DEBUG] 开始文件一致性检查..."<<std::endl;
    const int BATCH_SIZE=50;
    int processedInBatch=0;

    auto processBatch=[&]() {
        if(processedInBatch>=BATCH_SIZE) {
            HANDLE heap=GetProcessHeap();
            if(heap!=NULL) {
                HeapCompact(heap,0);
            }

            std::cout<<"\r检查进度: "<<totalChecked<<" 文件 ("<<missingFiles<<" 缺失, "<<mismatchedFiles<<" 不匹配)      ";
            std::cout.flush();

            processedInBatch=0;
        }
        };

    for(const auto& fileInfo:fileManifest) {
        processBatch();

        std::string relativePath=fileInfo["path"].asString();
        std::string expectedHash=fileInfo["hash"].asString();
        std::string fullPath=updateOrchestrator.gameDirectory+"/"+relativePath;

        totalChecked++;
        processedInBatch++;

        if(!std::filesystem::exists(fullPath)) {
            g_logger<<"[DEBUG] 文件不存在: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            missingFiles++;
            continue;
        }

        std::string actualHash=FileHasher::CalculateFileHash(fullPath,hashAlgorithm);
        if(actualHash.empty()) {
            g_logger<<"[DEBUG] 无法计算文件哈希: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            mismatchedFiles++;
        }
        else if(actualHash!=expectedHash) {
            g_logger<<"[DEBUG] 文件哈希不匹配: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            mismatchedFiles++;
        }
    }

    for(const auto& dirInfo:directoryManifest) {
        std::string relativePath=dirInfo["path"].asString();
        std::string fullPath=updateOrchestrator.gameDirectory+"/"+relativePath;

        if(!std::filesystem::exists(fullPath)) {
            g_logger<<"[DEBUG] 目录不存在: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            missingFiles++;
            continue;
        }

        const Json::Value& contents=dirInfo["contents"];
        for(const auto& contentInfo:contents) {
            processBatch();

            std::string fileRelativePath=contentInfo["path"].asString();
            std::string expectedHash=contentInfo["hash"].asString();
            std::string fileFullPath=fullPath+"/"+fileRelativePath;

            totalChecked++;
            processedInBatch++;

            if(!std::filesystem::exists(fileFullPath)) {
                g_logger<<"[DEBUG] 目录内文件不存在: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                missingFiles++;
                continue;
            }

            std::string actualHash=FileHasher::CalculateFileHash(fileFullPath,hashAlgorithm);
            if(actualHash.empty()) {
                g_logger<<"[DEBUG] 无法计算目录内文件哈希: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                mismatchedFiles++;
            }
            else if(actualHash!=expectedHash) {
                g_logger<<"[DEBUG] 目录内文件哈希不匹配: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                mismatchedFiles++;
            }
        }
    }

    std::cout<<"\r检查完成: "<<totalChecked<<" 文件 ("<<missingFiles<<" 缺失, "<<mismatchedFiles<<" 不匹配)      "<<std::endl;

    g_logger<<"[INFO] 文件一致性检查完成:"<<std::endl;
    g_logger<<"[INFO]   总共检查: "<<totalChecked<<" 个文件"<<std::endl;
    g_logger<<"[INFO]   缺失文件: "<<missingFiles<<" 个"<<std::endl;
    g_logger<<"[INFO]   不匹配文件: "<<mismatchedFiles<<" 个"<<std::endl;
    g_logger<<"[INFO]   文件一致性: "<<(allFilesConsistent?"通过":"失败")<<std::endl;

    return allFilesConsistent;
}
bool HashBasedFileSyncer::SyncFilesByHash(const Json::Value& updateInfo) {
    g_logger<<"[INFO] 开始哈希模式同步..."<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含files字段: "<<updateInfo.isMember("files")<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含directories字段: "<<updateInfo.isMember("directories")<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含file_manifest字段: "<<updateInfo.isMember("file_manifest")<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含directory_manifest字段: "<<updateInfo.isMember("directory_manifest")<<std::endl;

    if(configManager.ReadEnableFileDeletion()) {
        ProcessDeleteList(updateInfo["delete_list"]);
    }

    Json::Value fileManifest=updateInfo["files"];
    Json::Value directoryManifest=updateInfo["directories"];

    g_logger<<"[DEBUG] 文件清单数量: "<<fileManifest.size()<<std::endl;
    g_logger<<"[DEBUG] 目录清单数量: "<<directoryManifest.size()<<std::endl;

    if(!UpdateFilesByHash(fileManifest,directoryManifest)) {
        return false;
    }

    g_logger<<"[INFO] 开始创建空目录..."<<std::endl;
    int createdEmptyDirs=0;
    for(const auto& dirInfo:directoryManifest) {
        bool isEmpty=dirInfo.isMember("is_empty")&&dirInfo["is_empty"].asBool();
        if(!isEmpty) continue;

        std::string relativePath=dirInfo["path"].asString();
        std::filesystem::path fullPath=std::filesystem::absolute(updateOrchestrator.gameDirectory)/relativePath;
        if(!std::filesystem::exists(fullPath)) {
            try {
                std::filesystem::create_directories(fullPath);
                g_logger<<"[INFO] 创建空目录: "<<relativePath<<std::endl;
                createdEmptyDirs++;
            }
            catch(const std::exception& e) {
                g_logger<<"[ERROR] 创建空目录失败: "<<relativePath<<" - "<<e.what()<<std::endl;
            }
        }
        else {
            g_logger<<"[DEBUG] 空目录已存在: "<<relativePath<<std::endl;
        }
    }
    g_logger<<"[INFO] 空目录创建完成，共创建 "<<createdEmptyDirs<<" 个"<<std::endl;

    g_logger<<"[INFO] 哈希模式同步完成"<<std::endl;
    return true;
}
bool HashBasedFileSyncer::UpdateFilesByHash(const Json::Value& fileManifest,const Json::Value& directoryManifest) {
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();
    bool allSuccess=true;

    int totalFiles=fileManifest.size();
    int currentFile=0;

    std::cout<<std::endl;

    for(const auto& fileInfo:fileManifest) {
        currentFile++;
        std::string relativePath=fileInfo["path"].asString();
        std::string expectedHash=fileInfo["hash"].asString();
        std::string url=fileInfo["url"].asString();

        std::cout<<"["<<currentFile<<"/"<<totalFiles<<"] 下载: "<<relativePath<<std::endl;
        std::string fileInfoStr="["+std::to_string(currentFile)+"/"+
            std::to_string(totalFiles)+"] 下载: "+relativePath;
        int padding=80-(int)fileInfoStr.length();
        if(padding>0) {
            std::cout<<std::string(padding,' ');
        }
        std::cout<<std::endl;
        std::cout.flush();

        std::filesystem::path gameDirPath=std::filesystem::absolute(updateOrchestrator.gameDirectory);
        std::filesystem::path fullPath=gameDirPath/relativePath;
        std::string fullPathStr=fullPath.string();

        std::filesystem::path parentDir=fullPath.parent_path();
        fsHelper.EnsureDirectoryExists(parentDir.string());

        bool canWrite=false;
        try {
            std::filesystem::path testFile=parentDir/"write_test.tmp";
            std::ofstream testStream(testFile);
            if(testStream) {
                testStream<<"test";
                testStream.close();
                std::filesystem::remove(testFile);
                canWrite=true;
                g_logger<<"[DEBUG] 目录写入权限检查通过: "<<parentDir.string()<<std::endl;
            }
        }
        catch(const std::exception& e) {
            g_logger<<"[DEBUG] 目录写入权限检查失败: "<<e.what()<<std::endl;
        }

        if(!canWrite) {
            g_logger<<"[ERROR] 错误: 目录没有写入权限: "<<parentDir.string()<<std::endl;
            std::cout<<"  [权限错误]"<<std::endl;
            allSuccess=false;
            continue;
        }

        if(std::filesystem::exists(fullPath)) {
            std::string actualHash=FileHasher::CalculateFileHash(fullPathStr,hashAlgorithm);
            if(!actualHash.empty()&&actualHash==expectedHash) {
                g_logger<<"[INFO] 文件已是最新: "<<relativePath<<std::endl;
                std::cout<<"  [已是最新]"<<std::endl;
                continue;
            }
        }

        if(fileInfo.isMember("size")) {
            long long fileSize=fileInfo["size"].asInt64();
            int timeout=GetDownloadTimeoutForSize(fileSize);
            httpClient.SetDownloadTimeout(timeout);
            g_logger<<"[DEBUG] 设置文件下载超时: "<<timeout<<"秒 (大小: "<<progressReporter.FormatBytes(fileSize)<<")"<<std::endl;
        }

        std::string progressMessage="进度";

        progressReporter.ShowProgressBar(progressMessage,0,1);

        bool downloadSuccess=false;
        long long fileSize=0;

        if(fileInfo.isMember("size")) {
            fileSize=fileInfo["size"].asInt64();
        }

        auto progressCallback=[this,progressMessage,fileSize](long long downloaded,long long total,void* userdata) {
            if(total<=0&&fileSize>0) {
                progressReporter.ShowProgressBar(progressMessage,downloaded,fileSize);
            }
            else {
                progressReporter.ShowProgressBar(progressMessage,downloaded,total);
            }
            };

        if(fileSize>0) {
            int timeout=GetDownloadTimeoutForSize(fileSize);
            httpClient.SetDownloadTimeout(timeout);
        }

        downloadSuccess=httpClient.DownloadFileWithProgress(
            url,
            fullPathStr,
            progressCallback,
            nullptr
        );

        progressReporter.ClearProgressLine();

        httpClient.SetDownloadTimeout(0);

        if(!downloadSuccess) {
            std::cout<<"  [失败]"<<std::endl;
            allSuccess=false;
            continue;
        }

        std::error_code ec;
        auto actualSize=std::filesystem::file_size(fullPathStr,ec);
        std::string sizeStr=ec?"未知大小":progressReporter.FormatBytes(actualSize);

        if(!expectedHash.empty()) {
            std::string downloadedHash=FileHasher::CalculateFileHash(fullPathStr,hashAlgorithm);
            if(downloadedHash!=expectedHash) {
                std::cout<<"  [完成，大小: "<<sizeStr<<"，但哈希不匹配]"<<std::endl;
            }
            else {
                std::cout<<"  [完成，大小: "<<sizeStr<<"，已验证]"<<std::endl;
            }
        }
        else {
            std::cout<<"  [完成，大小: "<<sizeStr<<"]"<<std::endl;
        }
    }

    return allSuccess;
}
bool HashBasedFileSyncer::SyncDirectoryByHash(const Json::Value& dirInfo) {
    std::string relativePath=dirInfo["path"].asString();
    std::string url=dirInfo["url"].asString();
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();

    g_logger<<"[INFO] 同步目录: "<<relativePath<<std::endl;

    std::vector<unsigned char> zipData;
    if(!httpClient.DownloadToMemory(url,zipData)) {
        g_logger<<"[ERROR] 目录下载失败: "<<relativePath<<std::endl;
        return false;
    }

    std::string tempDir=std::filesystem::temp_directory_path().string()+"/mc_update_temp";
    fsHelper.EnsureDirectoryExists(tempDir);

    if(!zipExtractor.ExtractZip(zipData,tempDir)) {
        g_logger<<"[ERROR] 解压失败: "<<relativePath<<std::endl;
        return false;
    }

    std::filesystem::path gameDirPath=std::filesystem::absolute(updateOrchestrator.gameDirectory);
    std::string targetDir=(gameDirPath/relativePath).string();
    fsHelper.EnsureDirectoryExists(targetDir);

    const Json::Value& contents=dirInfo["contents"];
    bool dirSuccess=true;

    for(const auto& contentInfo:contents) {
        std::string fileRelativePath=contentInfo["path"].asString();
        std::string expectedHash=contentInfo["hash"].asString();

        std::string tempFilePath=tempDir+"/"+fileRelativePath;
        std::string targetFilePath=targetDir+"/"+fileRelativePath;

        if(!std::filesystem::exists(tempFilePath)) {
            g_logger<<"[WARN] 解压文件中不存在: "<<fileRelativePath<<std::endl;
            dirSuccess=false;
            continue;
        }

        if(!expectedHash.empty()) {
            std::string actualHash=FileHasher::CalculateFileHash(tempFilePath,hashAlgorithm);
            if(actualHash!=expectedHash) {
                g_logger<<"[WARN] 解压文件哈希验证失败: "<<fileRelativePath<<std::endl;
                g_logger<<"[WARN] 期望: "<<expectedHash<<std::endl;
                g_logger<<"[WARN] 实际: "<<actualHash<<std::endl;
            }
            else {
                g_logger<<"[DEBUG] 解压文件哈希验证成功: "<<fileRelativePath<<std::endl;
            }
        }

        fsHelper.EnsureDirectoryExists(std::filesystem::path(targetFilePath).parent_path().string());

        try {
            std::filesystem::copy(tempFilePath,targetFilePath,
                std::filesystem::copy_options::overwrite_existing);
            g_logger<<"[INFO] 更新文件: "<<fileRelativePath<<std::endl;
        }
        catch(const std::exception& e) {
            g_logger<<"[ERROR] 文件复制失败: "<<fileRelativePath<<" - "<<e.what()<<std::endl;
            dirSuccess=false;
        }
    }

    if(configManager.ReadEnableFileDeletion()) {
        fsHelper.CleanupOrphanedFiles(targetDir,contents);
    }

    try {
        std::filesystem::remove_all(tempDir);
    }
    catch(const std::exception& e) {
        g_logger<<"[WARN] 清理临时目录失败: "<<e.what()<<std::endl;
    }

    return dirSuccess;
}
bool HashBasedFileSyncer::ProcessDeleteList(const Json::Value& deleteList) {
    if(!deleteList.isArray()) {
        return true;
    }

    for(const auto& item:deleteList) {
        std::string path=item.asString();
        std::string fullPath=updateOrchestrator.gameDirectory+"/"+path;

        try {
            if(std::filesystem::exists(fullPath)) {
                if(std::filesystem::is_directory(fullPath)) {
                    std::filesystem::remove_all(fullPath);
                    g_logger<<"[INFO] 删除目录: "<<path<<std::endl;
                }
                else {
                    std::filesystem::remove(fullPath);
                    g_logger<<"[INFO] 删除文件: "<<path<<std::endl;
                }
            }
        }
        catch(const std::exception& e) {
            g_logger<<"[WARN] 删除失败: "<<path<<" - "<<e.what()<<std::endl;
        }
    }
    return true;
}
int HashBasedFileSyncer::GetDownloadTimeoutForSize(long long fileSize) {
    int baseTimeout=60;
    int additionalTime=static_cast<int>((fileSize/(10*1024*1024))*30);
    int totalTimeout=baseTimeout+additionalTime;
    return (totalTimeout>600)?600:totalTimeout;
}
bool HashBasedFileSyncer::ShouldForceHashUpdate(const std::string& localVersion,const std::string& remoteVersion) {
    if(configManager.ReadSkipMajorVersionCheck()) {
        return false;
    }

    std::regex versionRegex(R"((\d+)\.(\d+)\.(\d+))");
    std::smatch localMatch,remoteMatch;

    if(std::regex_match(localVersion,localMatch,versionRegex)&&
        std::regex_match(remoteVersion,remoteMatch,versionRegex)) {

        int localMajor=std::stoi(localMatch[1]);
        int remoteMajor=std::stoi(remoteMatch[1]);

        if(localMajor!=remoteMajor) {
            return true;
        }

        int localMinor=std::stoi(localMatch[2]);
        int remoteMinor=std::stoi(remoteMatch[2]);

        if(std::abs(remoteMinor-localMinor)>=3) {
            return true;
        }
    }

    return false;
}