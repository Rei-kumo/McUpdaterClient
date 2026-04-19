#include "IncrementalUpdatePlanner.h"
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
#include "UpdateOrchestrator.h"
IncrementalUpdatePlanner::IncrementalUpdatePlanner(HttpClient& http,
    FileSystemHelper& fs,
    ProgressReporter& reporter,
    ConfigManager& config,
    UpdateOrchestrator& orc,
    ZipExtractor& zip)
    : httpClient(http),
    fsHelper(fs),
    progressReporter(reporter),
    configManager(config),
    updateOrchestrator(orc),
    zipExtractor(zip)
{
}
bool IncrementalUpdatePlanner::ShouldUseIncrementalUpdate(const std::string& localVersion,const std::string& remoteVersion) {
    if(localVersion>=remoteVersion) {
        return false;
    }

    std::regex versionRegex(R"((\d+)\.(\d+)\.(\d+))");
    std::smatch localMatch,remoteMatch;

    if(!std::regex_match(localVersion,localMatch,versionRegex)||
        !std::regex_match(remoteVersion,remoteMatch,versionRegex)) {
        g_logger<<"[WARN] 版本号格式不正确，跳过增量更新"<<std::endl;
        return false;
    }

    int localMajor=std::stoi(localMatch[1]);
    int remoteMajor=std::stoi(remoteMatch[1]);

    if(localMajor!=remoteMajor) {
        g_logger<<"[INFO] 检测到主要版本变更 ("<<localVersion<<" -> "<<remoteVersion<<")，建议使用全量更新"<<std::endl;
    }

    return true;
}
std::vector<std::string> IncrementalUpdatePlanner::GetUpdatePackagePath(const Json::Value& packages,const std::string& fromVersion,const std::string& toVersion) {
    std::vector<std::string> result;

    g_logger<<"[INFO] 寻找更新路径: "<<fromVersion<<" -> "<<toVersion<<std::endl;

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")||!package.isMember("archive")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from==fromVersion&&to==toVersion) {
            g_logger<<"[INFO] 找到直接合并包: "<<archive<<" ("<<from<<" -> "<<to<<")"<<std::endl;
            return {archive};
        }
    }

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from=="0.0.0"&&to==toVersion) {
            g_logger<<"[INFO] 找到全量更新包: "<<archive<<" (0.0.0 -> "<<to<<")"<<std::endl;
            return {archive};
        }
    }

    std::map<std::string,std::vector<std::string>> graph;
    std::map<std::string,std::string> archiveMap;

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")||!package.isMember("archive")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from=="0.0.1") {
            continue;
        }

        graph[from].push_back(to);
        std::string edgeKey=from+"->"+to;
        archiveMap[edgeKey]=archive;
    }

    std::queue<std::pair<std::string,std::vector<std::string>>> q;
    std::set<std::string> visited;

    q.push({fromVersion,{}});
    visited.insert(fromVersion);

    while(!q.empty()) {
        auto [current,path]=q.front();
        q.pop();

        if(current==toVersion) {
            std::vector<std::string> archivePath;
            for(size_t i=0; i<path.size(); i++) {
                std::string from=(i==0)?fromVersion:path[i-1];
                std::string to=path[i];
                std::string edgeKey=from+"->"+to;

                if(archiveMap.find(edgeKey)!=archiveMap.end()) {
                    archivePath.push_back(archiveMap[edgeKey]);
                }
            }

            g_logger<<"[INFO] 找到增量更新路径，包含 "<<archivePath.size()<<" 个包"<<std::endl;
            return archivePath;
        }

        if(graph.find(current)!=graph.end()) {
            for(const auto& next:graph[current]) {
                if(visited.find(next)==visited.end()) {
                    visited.insert(next);
                    std::vector<std::string> newPath=path;
                    newPath.push_back(next);
                    q.push({next,newPath});
                }
            }
        }
    }

    g_logger<<"[WARN] 无法找到增量更新路径: "<<fromVersion<<" -> "<<toVersion<<std::endl;
    return {};
}
bool IncrementalUpdatePlanner::ApplyIncrementalUpdate(const Json::Value& updateInfo,const std::string& localVersion,const std::string& remoteVersion) {
    updateOrchestrator.ClearCachedUpdateInfo();

    const Json::Value& packages=updateInfo["incremental_packages"];
    if(!packages.isArray()||packages.size()==0) {
        g_logger<<"[INFO] 没有可用的增量更新包"<<std::endl;
        return false;
    }

    g_logger<<"[INFO] 开始处理增量更新: "<<localVersion<<" -> "<<remoteVersion<<std::endl;

    std::vector<std::string> packagePaths=GetUpdatePackagePath(packages,localVersion,remoteVersion);

    if(packagePaths.empty()) {
        g_logger<<"[INFO] 没有找到合适的增量更新包路径"<<std::endl;
        return false;
    }

    g_logger<<"[INFO] 需要应用 "<<packagePaths.size()<<" 个更新包"<<std::endl;

    for(size_t i=0; i<packagePaths.size(); i++) {
        const std::string& packagePath=packagePaths[i];
        g_logger<<"[INFO] ("<<(i+1)<<"/"<<packagePaths.size()<<") 处理更新包: "<<packagePath<<std::endl;

        if(i>0) {
            updateOrchestrator.OptimizeMemoryUsage();
        }

        Json::Value packageInfo;
        for(const auto& package:packages) {
            if(package["archive"].asString()==packagePath) {
                packageInfo=package;
                break;
            }
        }

        if(packageInfo.isNull()) {
            g_logger<<"[ERROR] 找不到包信息: "<<packagePath<<std::endl;
            continue;
        }

        std::string expectedHash=packageInfo["hash"].asString();
        long long expectedSize=packageInfo.isMember("size")?packageInfo["size"].asInt64():0;

        DWORD pid=GetCurrentProcessId();
        auto timestamp=std::chrono::steady_clock::now().time_since_epoch().count();
        std::string tempDir=std::filesystem::temp_directory_path().string();
        std::string tempZip=tempDir+"/mc_pkg_"+std::to_string(pid)+"_"+std::to_string(timestamp)+"_"+std::to_string(i)+".zip";

        g_logger<<"[INFO] 开始下载更新包..."<<std::endl;
        std::string progressMessage="下载更新包 "+std::to_string(i+1)+"/"+std::to_string(packagePaths.size());
        progressReporter.ShowProgressBar(progressMessage,0,1);

        bool downloadSuccess=httpClient.DownloadFileWithProgress(
            packagePath,
            tempZip,
            [this,progressMessage,expectedSize](long long downloaded,long long total,void* userdata) {
                if(total<=0&&expectedSize>0) {
                    total=expectedSize;
                }
                progressReporter.ShowProgressBar(progressMessage,downloaded,total);
            },
            nullptr
        );

        progressReporter.ClearProgressLine();

        if(!downloadSuccess) {
            g_logger<<"[ERROR] 下载更新包失败: "<<packagePath<<std::endl;
            return false;
        }

        g_logger<<"[INFO] 下载完成"<<std::endl;

        if(expectedSize>0) {
            std::error_code ec;
            auto actualSize=std::filesystem::file_size(tempZip,ec);
            if(!ec&&actualSize!=expectedSize) {
                g_logger<<"[WARN] 文件大小不匹配: 期望 "<<progressReporter.FormatBytes(expectedSize)<<", 实际 "<<progressReporter.FormatBytes(actualSize)<<std::endl;
            }
        }

        if(!expectedHash.empty()) {
            g_logger<<"[INFO] 验证文件哈希..."<<std::endl;

            std::string actualHash=FileHasher::CalculateFileHashStream(tempZip,"md5");
            if(actualHash!=expectedHash) {
                g_logger<<"[ERROR] 更新包哈希验证失败"<<std::endl;
                g_logger<<"[ERROR] 期望: "<<expectedHash<<std::endl;
                g_logger<<"[ERROR] 实际: "<<actualHash<<std::endl;

                std::filesystem::remove(tempZip);
                return false;
            }
            else {
                g_logger<<"[INFO] 更新包哈希验证通过"<<std::endl;
            }
        }

        std::string tempExtractDir=tempDir+"/mc_extract_"+std::to_string(pid)+"_"+std::to_string(timestamp)+"_"+std::to_string(i);
        fsHelper.EnsureDirectoryExists(tempExtractDir);

        g_logger<<"[INFO] 解压更新包..."<<std::endl;
        if(!zipExtractor.ExtractZipFromFile(tempZip,tempExtractDir)) {
            g_logger<<"[ERROR] 解压更新包失败: "<<packagePath<<std::endl;
            std::filesystem::remove_all(tempExtractDir);
            std::filesystem::remove(tempZip);
            return false;
        }

        g_logger<<"[INFO] 应用更新..."<<std::endl;
        std::string manifestPath=tempExtractDir+"/update_manifest.txt";
        if(std::filesystem::exists(manifestPath)) {
            if(!ApplyUpdateFromManifest(manifestPath,tempExtractDir)) {
                g_logger<<"[ERROR] 应用清单更新失败"<<std::endl;
                std::filesystem::remove_all(tempExtractDir);
                std::filesystem::remove(tempZip);
                return false;
            }
        }
        else {
            g_logger<<"[WARN] 未找到清单文件，使用传统文件复制方式"<<std::endl;
            if(!ApplyUpdateFromDirectory(tempExtractDir)) {
                g_logger<<"[ERROR] 应用更新失败"<<std::endl;
                std::filesystem::remove_all(tempExtractDir);
                std::filesystem::remove(tempZip);
                return false;
            }
        }

        std::filesystem::remove_all(tempExtractDir);
        std::filesystem::remove(tempZip);

        g_logger<<"[INFO] 更新包 ("<<(i+1)<<"/"<<packagePaths.size()<<") 处理完成"<<std::endl;

        updateOrchestrator.OptimizeMemoryUsage();
    }

    g_logger<<"[INFO] 所有增量更新包应用完成"<<std::endl;

    return true;
}
bool IncrementalUpdatePlanner::ApplyUpdateFromManifest(const std::string& manifestPath,const std::string& tempDir) {
    std::ifstream manifestFile(manifestPath);
    if(!manifestFile.is_open()) {
        g_logger<<"[ERROR] 无法打开清单文件: "<<manifestPath<<std::endl;
        return false;
    }

    std::string line;
    int operationCount=0;
    int successCount=0;
    int failCount=0;

    while(std::getline(manifestFile,line)) {
        // 跳过注释行和空行
        if(line.empty()||line[0]=='#') continue;

        // 解析行格式：TYPE:PATH:OLD_PATH:HASH:SIZE
        std::istringstream lineStream(line);
        std::string token;
        std::vector<std::string> tokens;
        while(std::getline(lineStream,token,':')) {
            tokens.push_back(token);
        }
        if(tokens.size()<2) {
            g_logger<<"[WARN] 忽略无效行: "<<line<<std::endl;
            continue;
        }

        std::string type=tokens[0];
        std::string path=tokens[1];
        std::string oldPath=(tokens.size()>2)?tokens[2]:"";
        std::string hash=(tokens.size()>3)?tokens[3]:"";
        uint64_t size=(tokens.size()>4)?std::stoull(tokens[4]):0;

        // 根据类型执行操作
        if(type=="A"||type=="M") {
            std::string sourceFile=tempDir+"/"+path;
            std::string targetFile=updateOrchestrator.GetGameDirectory()+"/"+path;

            fsHelper.EnsureDirectoryExists(std::filesystem::path(targetFile).parent_path().string());

            if(std::filesystem::exists(sourceFile)) {
                std::error_code ec;
                std::filesystem::copy_file(sourceFile,targetFile,
                    std::filesystem::copy_options::overwrite_existing,ec);
                if(ec) {
                    g_logger<<"[ERROR] 复制文件失败: "<<sourceFile<<" -> "<<targetFile
                        <<" - "<<ec.message()<<std::endl;
                    failCount++;
                }
                else {
                    g_logger<<"[INFO] "<<(type=="A"?"新增":"修改")
                        <<"文件: "<<path<<std::endl;
                    successCount++;
                }
            }
            else {
                g_logger<<"[WARN] 源文件不存在: "<<sourceFile<<std::endl;
                failCount++;
            }
        }
        else if(type=="D") {
            // 删除文件
            std::string targetFile=updateOrchestrator.GetGameDirectory()+"/"+path;
            if(std::filesystem::exists(targetFile)) {
                try {
                    std::filesystem::remove(targetFile);
                    g_logger<<"[INFO] 删除文件: "<<path<<std::endl;
                    successCount++;
                }
                catch(const std::exception& e) {
                    g_logger<<"[ERROR] 删除文件失败: "<<targetFile
                        <<" - "<<e.what()<<std::endl;
                    failCount++;
                }
            }
            else {
                g_logger<<"[DEBUG] 文件不存在，无需删除: "<<path<<std::endl;
                // 不存在也算成功
                successCount++;
            }
        }
        else if(type=="R") {
            // 移动/重命名文件
            if(oldPath.empty()) {
                g_logger<<"[ERROR] 移动操作缺少 old_path: "<<line<<std::endl;
                failCount++;
                continue;
            }
            std::string sourceFile=tempDir+"/"+path;          // 新文件在临时目录中
            std::string targetFile=updateOrchestrator.GetGameDirectory()+"/"+path;    // 新位置
            std::string oldTargetFile=updateOrchestrator.GetGameDirectory()+"/"+oldPath; // 旧文件

            fsHelper.EnsureDirectoryExists(std::filesystem::path(targetFile).parent_path().string());

            // 先复制新文件到目标位置
            if(std::filesystem::exists(sourceFile)) {
                try {
                    std::filesystem::copy_file(sourceFile,targetFile,
                        std::filesystem::copy_options::overwrite_existing);
                    g_logger<<"[INFO] 移动文件: "<<oldPath<<" -> "<<path<<std::endl;
                }
                catch(const std::exception& e) {
                    g_logger<<"[ERROR] 复制文件失败 (移动操作): "<<sourceFile
                        <<" -> "<<targetFile<<" - "<<e.what()<<std::endl;
                    failCount++;
                    continue;
                }
            }
            else {
                g_logger<<"[ERROR] 移动操作的源文件不存在: "<<sourceFile<<std::endl;
                failCount++;
                continue;
            }

            // 删除旧文件
            if(std::filesystem::exists(oldTargetFile)) {
                try {
                    std::filesystem::remove(oldTargetFile);
                }
                catch(const std::exception& e) {
                    g_logger<<"[WARN] 移动后删除旧文件失败: "<<oldTargetFile
                        <<" - "<<e.what()<<std::endl;
                    // 不标记为失败，因为新文件已复制
                }
            }
            successCount++;
        }
        else if(type=="AD") {
            // 新增空目录
            std::string targetDir=updateOrchestrator.GetGameDirectory()+"/"+path;
            try {
                if(!std::filesystem::exists(targetDir)) {
                    std::filesystem::create_directories(targetDir);
                    g_logger<<"[INFO] 创建空目录: "<<path<<std::endl;
                }
                else {
                    g_logger<<"[DEBUG] 目录已存在: "<<path<<std::endl;
                }
                successCount++;
            }
            catch(const std::exception& e) {
                g_logger<<"[ERROR] 创建目录失败: "<<targetDir
                    <<" - "<<e.what()<<std::endl;
                failCount++;
            }
        }
        else if(type=="DD") {
            // 删除空目录
            std::string targetDir=updateOrchestrator.GetGameDirectory()+"/"+path;
            if(std::filesystem::exists(targetDir)&&std::filesystem::is_directory(targetDir)) {
                try {
                    // 仅删除空目录（如果目录非空，可能因为文件残留而失败）
                    std::filesystem::remove(targetDir);
                    g_logger<<"[INFO] 删除空目录: "<<path<<std::endl;
                    successCount++;
                }
                catch(const std::exception& e) {
                    g_logger<<"[WARN] 删除目录失败 (可能非空): "<<targetDir
                        <<" - "<<e.what()<<std::endl;
                    failCount++;
                }
            }
            else {
                g_logger<<"[DEBUG] 目录不存在或非目录，无需删除: "<<path<<std::endl;
                successCount++;
            }
        }
        else {
            g_logger<<"[WARN] 未知操作类型: "<<type<<" (行: "<<line<<")"<<std::endl;
            failCount++;
        }

        operationCount++;
        if(operationCount%50==0) {
            std::cout<<"\r处理清单: "<<operationCount<<" 项操作 (成功: "<<successCount
                <<", 失败: "<<failCount<<")     "<<std::flush;
        }
    }

    manifestFile.close();
    std::cout<<"\r清单处理完成: 总计 "<<operationCount<<" 项操作, 成功: "<<successCount
        <<", 失败: "<<failCount<<"                    "<<std::endl;

    g_logger<<"[INFO] 从清单执行了 "<<operationCount<<" 项操作, 成功: "<<successCount
        <<", 失败: "<<failCount<<std::endl;

    return failCount==0;
}
bool IncrementalUpdatePlanner::ApplyUpdateFromDirectory(const std::string& sourceDir) {
    int fileCount=0;
    int failedCount=0;
    const int BATCH_SIZE=50;

    try {
        std::wstring wideSourceDir=fsHelper.Utf8ToWide(sourceDir);
        std::wstring wideGameDir=fsHelper.Utf8ToWide(updateOrchestrator.GetGameDirectory());

        if(wideSourceDir.empty()||wideGameDir.empty()) {
            g_logger<<"[ERROR] 无法转换路径为宽字符"<<std::endl;
            return false;
        }

        for(const auto& entry:std::filesystem::recursive_directory_iterator(wideSourceDir)) {
            if(entry.is_regular_file()) {
                std::wstring wideRelativePath=entry.path().wstring().substr(wideSourceDir.size()+1);
                std::wstring wideTargetPath=wideGameDir+L"\\"+wideRelativePath;

                std::filesystem::path targetDir=std::filesystem::path(wideTargetPath).parent_path();
                if(!targetDir.empty()) {
                    std::filesystem::create_directories(targetDir);
                }

                bool copySuccess=fsHelper.CopyFileWithUnicode(entry.path().wstring(),wideTargetPath);

                if(copySuccess) {
                    fileCount++;

                    if(fileCount%BATCH_SIZE==0) {
                        updateOrchestrator.OptimizeMemoryUsage();
                        std::cout<<"\r应用更新: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"     ";
                        std::cout.flush();
                    }
                }
                else {
                    failedCount++;
                    g_logger<<"[WARN] 文件复制失败: "<<fsHelper.WideToUtf8(entry.path().wstring())<<std::endl;
                }
            }
        }

        std::cout<<"\r应用更新完成: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"                  "<<std::endl;
        g_logger<<"[INFO] 应用更新完成: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<std::endl;

        if(failedCount>0) {
            g_logger<<"[WARN] "<<failedCount<<" 个文件处理失败"<<std::endl;
            return false;
        }

        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 应用更新失败: "<<e.what()<<std::endl;
        return false;
    }
}
bool IncrementalUpdatePlanner::ApplyAllFilesFromUpdate(const std::string& tempDir) {
    int fileCount=0;
    int failedCount=0;

    std::wstring wideTempDir=fsHelper.Utf8ToWide(tempDir);
    std::wstring wideGameDir=fsHelper.Utf8ToWide(updateOrchestrator.GetGameDirectory());

    if(wideTempDir.empty()||wideGameDir.empty()) {
        g_logger<<"[ERROR] 无法转换路径为宽字符"<<std::endl;
        return false;
    }

    try {
        for(const auto& entry:std::filesystem::recursive_directory_iterator(wideTempDir)) {
            if(entry.is_regular_file()) {
                std::wstring wideRelativePath=entry.path().wstring().substr(wideTempDir.size()+1);
                std::wstring wideTargetPath=wideGameDir+L"\\"+wideRelativePath;
                std::filesystem::path targetDir=std::filesystem::path(wideTargetPath).parent_path();
                if(!targetDir.empty()) {
                    std::filesystem::create_directories(targetDir);
                }
                bool copySuccess=fsHelper.CopyFileWithUnicode(entry.path().wstring(),wideTargetPath);

                if(copySuccess) {
                    fileCount++;
                    if(fileCount%100==0) {
                        std::cout<<"\r更新进度: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"     ";
                        std::cout.flush();
                    }
                }
                else {
                    failedCount++;
                }
            }
        }
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 遍历临时目录失败: "<<e.what()<<std::endl;
        return false;
    }

    std::cout<<"\r更新完成: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"                  "<<std::endl;
    g_logger<<"[INFO] 更新了 "<<fileCount<<" 个文件，失败: "<<failedCount<<std::endl;

    return fileCount>0&&failedCount==0;
}