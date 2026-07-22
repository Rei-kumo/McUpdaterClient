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
		LOG_WARN("版本号格式不正确，跳过增量更新");
        return false;
    }

    int localMajor=std::stoi(localMatch[1]);
    int remoteMajor=std::stoi(remoteMatch[1]);

    if(localMajor!=remoteMajor) {
        LOG_INFO("检测到主要版本变更 ({} -> {})，建议使用全量更新", localVersion, remoteVersion);
    }

    return true;
}
std::vector<std::string> IncrementalUpdatePlanner::GetUpdatePackagePath(const Json::Value& packages,const std::string& fromVersion,const std::string& toVersion) {
    std::vector<std::string> result;

    LOG_INFO("寻找更新路径: {} -> {}", fromVersion, toVersion);

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")||!package.isMember("archive")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from==fromVersion&&to==toVersion) {
            LOG_INFO("找到直接合并包: {} ({} -> {})", archive, from, to);
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
            LOG_INFO("找到全量更新包: {} (0.0.0 -> {})", archive, to);
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

            LOG_INFO("找到增量更新路径，包含 {} 个包", archivePath.size());
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

	LOG_WARN("无法找到增量更新路径: {} -> {}",fromVersion,toVersion);
    return {};
}
bool IncrementalUpdatePlanner::ApplyIncrementalUpdate(const Json::Value& updateInfo,const std::string& localVersion,const std::string& remoteVersion) {
    updateOrchestrator.ClearCachedUpdateInfo();

    const Json::Value& packages=updateInfo["incremental_packages"];
    if(!packages.isArray()||packages.size()==0) {
        LOG_INFO("没有可用的增量更新包");
        return false;
    }

    LOG_INFO("开始处理增量更新: {} -> {}", localVersion, remoteVersion);

    std::vector<std::string> packagePaths=GetUpdatePackagePath(packages,localVersion,remoteVersion);

    if(packagePaths.empty()) {
        LOG_INFO("没有找到合适的增量更新包路径");
        return false;
    }

    LOG_INFO("需要应用 {} 个更新包", packagePaths.size());

    for(size_t i=0; i<packagePaths.size(); i++) {
        const std::string& packagePath=packagePaths[i];
        LOG_INFO("[INFO] ({}/{}) 处理更新包: {}", (i+1), packagePaths.size(), packagePath);

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
            LOG_ERROR("找不到包信息: {}", packagePath);
            continue;
        }

        std::string expectedHash=packageInfo["hash"].asString();
        long long expectedSize=packageInfo.isMember("size")?packageInfo["size"].asInt64():0;

        DWORD pid=GetCurrentProcessId();
        auto timestamp=std::chrono::steady_clock::now().time_since_epoch().count();
        std::string tempDir=std::filesystem::temp_directory_path().string();
        std::string tempZip=tempDir+"/mc_pkg_"+std::to_string(pid)+"_"+std::to_string(timestamp)+"_"+std::to_string(i)+".zip";

        LOG_INFO("开始下载更新包...");
        std::string progressMessage="下载更新包 "+std::to_string(i+1)+"/"+std::to_string(packagePaths.size());
        progressReporter.show(progressMessage,0,1);

        bool downloadSuccess=httpClient.DownloadFileWithProgress(
            packagePath,
            tempZip,
            [this,progressMessage,expectedSize](long long downloaded,long long total,void* userdata) {
                if(total<=0&&expectedSize>0) {
                    total=expectedSize;
                }
                progressReporter.show(progressMessage,downloaded,total);
            },
            nullptr
        );

        progressReporter.clear();

        if(!downloadSuccess) {
            LOG_ERROR("下载更新包失败: {}", packagePath);
            return false;
        }

        LOG_INFO("下载完成");

        if(expectedSize>0) {
            std::error_code ec;
            auto actualSize=std::filesystem::file_size(std::filesystem::u8path(tempZip),ec);
            if(!ec&&actualSize!=expectedSize) {
                LOG_WARN("文件大小不匹配: 期望 {}, 实际 {}", progressReporter.FormatBytes(expectedSize), progressReporter.FormatBytes(actualSize));
            }
        }

        if(!expectedHash.empty()) {
            LOG_INFO("验证文件哈希...");

            std::string actualHash=FileHasher::CalculateFileHashStream(tempZip,"md5");
            if(actualHash!=expectedHash) {
                LOG_ERROR("更新包哈希验证失败");
                LOG_ERROR("期望: {}", expectedHash);
                LOG_ERROR("实际: {}", actualHash);

                std::filesystem::remove(std::filesystem::u8path(tempZip));
                return false;
            }
            else {
                LOG_INFO("更新包哈希验证通过");
            }
        }

        std::string tempExtractDir=tempDir+"/mc_extract_"+std::to_string(pid)+"_"+std::to_string(timestamp)+"_"+std::to_string(i);
        fsHelper.EnsureDirectoryExists(tempExtractDir);

		LOG_INFO("解压更新包...");
        if(!zipExtractor.ExtractZipFromFile(tempZip,tempExtractDir)) {
            LOG_ERROR("解压更新包失败: {}", packagePath);
            std::filesystem::remove_all(std::filesystem::u8path(tempExtractDir));
            std::filesystem::remove(std::filesystem::u8path(tempZip));
            return false;
        }

        LOG_INFO("应用更新...");
        std::string manifestPath=tempExtractDir+"/update_manifest.txt";
        if(std::filesystem::exists(std::filesystem::u8path(manifestPath))) {
            if(!ApplyUpdateFromManifest(manifestPath,tempExtractDir)) {
                LOG_ERROR("应用清单更新失败");
                std::filesystem::remove_all(std::filesystem::u8path(tempExtractDir));
                std::filesystem::remove(std::filesystem::u8path(tempZip));
                return false;
            }
        }
        else {
            LOG_WARN("未找到清单文件，使用传统文件复制方式");
            if(!ApplyUpdateFromDirectory(tempExtractDir)) {
                LOG_ERROR("应用更新失败");
                std::filesystem::remove_all(std::filesystem::u8path(tempExtractDir));
                std::filesystem::remove(std::filesystem::u8path(tempZip));
                return false;
            }
        }

        std::filesystem::remove_all(std::filesystem::u8path(tempExtractDir));
        std::filesystem::remove(std::filesystem::u8path(tempZip));

        LOG_INFO("更新包 ({}/{}) 处理完成", (i+1), packagePaths.size());

        updateOrchestrator.OptimizeMemoryUsage();
    }

    LOG_INFO("所有增量更新包应用完成");

    return true;
}
bool IncrementalUpdatePlanner::ApplyUpdateFromManifest(const std::string& manifestPath,const std::string& tempDir) {
    std::ifstream manifestFile(std::filesystem::u8path(manifestPath));
    if(!manifestFile.is_open()) {
		LOG_ERROR("无法打开清单文件: {}",manifestPath);
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
            LOG_WARN("忽略无效行: {}", line);
            continue;
        }

        std::string type=tokens[0];
        std::string path=tokens[1];
        std::string oldPath=(tokens.size()>2)?tokens[2]:"";
        std::string hash=(tokens.size()>3)?tokens[3]:"";
        uint64_t size=(tokens.size()>4)?std::stoull(tokens[4]):0;

        // 根据类型执行操作
        if(type=="A"||type=="M") {
            std::string sourceFile;
            std::string targetFile;
            try {
                sourceFile=FileSystemHelper::SecureCombine(tempDir,path); 
                targetFile=FileSystemHelper::SecureCombine(updateOrchestrator.GetGameDirectory(),path);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {}", e.what());
                failCount++;
                continue;
            }

            fsHelper.EnsureDirectoryExists(std::filesystem::u8path(targetFile).parent_path().generic_u8string());

            if(std::filesystem::exists(std::filesystem::u8path(sourceFile))) {
                std::error_code ec;
                std::filesystem::copy_file(std::filesystem::u8path(sourceFile),std::filesystem::u8path(targetFile),
                    std::filesystem::copy_options::overwrite_existing,ec);
                if(ec) {
					LOG_ERROR("复制文件失败: {} -> {} - {}",sourceFile,targetFile,ec.message());
                    failCount++;
                }
                else {
					LOG_INFO("{}文件: {}",(type=="A"?"新增":"修改"),path);
                    successCount++;
                }
            }
            else {
                LOG_WARN("源文件不存在: {}", sourceFile);
                failCount++;
            }
        }
        else if(type=="D") {
            // 删除文件
            std::string targetFile;
            try {
                targetFile=FileSystemHelper::SecureCombine(updateOrchestrator.GetGameDirectory(),path);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {}", e.what());
                failCount++;
                continue;
            }
            if(std::filesystem::exists(std::filesystem::u8path(targetFile))) {
                try {
                    std::filesystem::remove(std::filesystem::u8path(targetFile));
                    LOG_INFO("删除文件: {}", path);
                    successCount++;
                }
                catch(const std::exception& e) {
                    LOG_ERROR("删除文件失败: {} - {}", targetFile, e.what());
                    failCount++;
                }
            }
            else {
                LOG_DEBUG("文件不存在，无需删除: {}", path);
                // 不存在也算成功
                successCount++;
            }
        }
        else if(type=="R") {
            // 移动/重命名文件
            if(oldPath.empty()) {
				LOG_ERROR("移动操作缺少 old_path: {}",line);
                failCount++;
                continue;
            }
            std::string sourceFile,targetFile,oldTargetFile;
            try {
                sourceFile=FileSystemHelper::SecureCombine(tempDir,path);
                targetFile=FileSystemHelper::SecureCombine(updateOrchestrator.GetGameDirectory(),path);
                oldTargetFile=FileSystemHelper::SecureCombine(updateOrchestrator.GetGameDirectory(),oldPath);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {}", e.what());
                failCount++;
                continue;
            }

            fsHelper.EnsureDirectoryExists(std::filesystem::u8path(targetFile).parent_path().generic_u8string());

            if(std::filesystem::exists(std::filesystem::u8path(sourceFile))) {
                try {
                    std::filesystem::copy_file(std::filesystem::u8path(sourceFile),std::filesystem::u8path(targetFile),std::filesystem::copy_options::overwrite_existing);
                    LOG_INFO("移动文件: {} -> {}", oldPath, path);
                }
                catch(const std::exception& e) {
                    LOG_ERROR("复制文件失败 (移动操作): {} -> {} - {}", sourceFile, targetFile, e.what());
                    failCount++;
                    continue;
                }
            }
            else {
                LOG_ERROR("移动操作的源文件不存在: {}", sourceFile);
                failCount++;
                continue;
            }

            // 删除旧文件
            if(std::filesystem::exists(std::filesystem::u8path(oldTargetFile))) {
                try {
                    std::filesystem::remove(std::filesystem::u8path(oldTargetFile));
                }
                catch(const std::exception& e) {
					LOG_WARN("移动后删除旧文件失败: {} - {}",oldTargetFile,e.what());
                    // 不标记为失败，因为新文件已复制
                }
            }
            successCount++;
        }
        else if(type=="AD") {
            std::string targetDir;
            try {
                targetDir=FileSystemHelper::SecureCombine(updateOrchestrator.GetGameDirectory(),path);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {}", e.what());
                failCount++;
                continue;
            }
            try {
                if(!std::filesystem::exists(std::filesystem::u8path(targetDir))) {
                    std::filesystem::create_directories(std::filesystem::u8path(targetDir));
                    LOG_INFO("创建空目录: {}", path);
                }
                else {
                    LOG_DEBUG("目录已存在: {}", path);
                }
                successCount++;
            }
            catch(const std::exception& e) {
				LOG_ERROR("创建目录失败: {} - {}",targetDir,e.what());
                failCount++;
            }
        }
        else if(type=="DD") {
            // 删除空目录
            std::string targetDir;
            try {
                targetDir=FileSystemHelper::SecureCombine(updateOrchestrator.GetGameDirectory(),path);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {}", e.what());
                failCount++;
                continue;
            }
            auto targetDirPath=std::filesystem::u8path(targetDir);
            if(std::filesystem::exists(targetDirPath)&&std::filesystem::is_directory(targetDirPath)) {
                try {
                    // 仅删除空目录（如果目录非空，可能因为文件残留而失败）
                    std::filesystem::remove(targetDirPath);
					LOG_INFO("删除空目录: {}",path);
                    successCount++;
                }
                catch(const std::exception& e) {
					LOG_WARN("删除目录失败 (可能非空): {} - {}",targetDir,e.what());  
                    failCount++;
                }
            }
            else {
                LOG_DEBUG("目录不存在或非目录，无需删除: {}", path);
                successCount++;
            }
        }
        else {
            LOG_WARN("未知操作类型: {} (行: {})", type, line);
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

    LOG_INFO("从清单执行了 {} 项操作, 成功: {}, 失败: {}", operationCount, successCount, failCount);

    return failCount==0;
}
bool IncrementalUpdatePlanner::ApplyUpdateFromDirectory(const std::string& sourceDir) {
    int fileCount=0;
    int failedCount=0;
    const int BATCH_SIZE=50;

    try {
        const std::string gameDir=updateOrchestrator.GetGameDirectory();
        std::filesystem::path sourcePath=std::filesystem::u8path(sourceDir);

        for(const auto& entry:
            std::filesystem::recursive_directory_iterator(sourcePath)) {
            if(entry.is_regular_file()) {
                std::error_code relativeEc;
                std::string relativePath=
                    std::filesystem::relative(entry.path(),sourcePath,relativeEc)
                    .generic_string();
                if(relativeEc) {
                    LOG_ERROR("计算相对路径失败: {} - {}",
                        entry.path().string(),relativeEc.message());
                    failedCount++;
                    continue;
                }

                std::string targetPath;
                try {
                    targetPath=FileSystemHelper::SecureCombine(gameDir,relativePath);
                }
                catch(const std::exception& e) {
                    LOG_ERROR("路径遍历被阻止: {} - {}",relativePath,e.what());
                    failedCount++;
                    continue;
                }

                // 确保目标目录存在
                std::filesystem::path targetFilePath=std::filesystem::u8path(targetPath);
                std::filesystem::path targetDir=targetFilePath.parent_path();
                if(!targetDir.empty()) {
                    std::error_code dirEc;
                    std::filesystem::create_directories(targetDir,dirEc);
                    // 目录创建失败可记录日志，但继续尝试复制
                }

                bool copySuccess=fsHelper.CopySingleFile(
                    entry.path().string(),targetPath);

                if(copySuccess) {
                    fileCount++;
                    if(fileCount%BATCH_SIZE==0) {
                        updateOrchestrator.OptimizeMemoryUsage();
                        std::cout<<"\r应用更新: "<<fileCount
                            <<" 个文件已处理，失败: "<<failedCount
                            <<"     ";
                        std::cout.flush();
                    }
                }
                else {
                    failedCount++;
                    LOG_WARN("文件复制失败: {}",entry.path().string());
                }
            }
        }

        std::cout<<"\r应用更新完成: "<<fileCount
            <<" 个文件已处理，失败: "<<failedCount
            <<"                  "<<std::endl;
        LOG_INFO("应用更新完成: {} 个文件已处理，失败: {}",fileCount,failedCount);

        if(failedCount>0) {
            LOG_WARN("{} 个文件处理失败",failedCount);
            return false;
        }
        return true;
    }
    catch(const std::exception& e) {
        LOG_ERROR("应用更新失败: {}",e.what());
        return false;
    }
}
bool IncrementalUpdatePlanner::ApplyAllFilesFromUpdate(const std::string& tempDir) {
    int fileCount=0;
    int failedCount=0;

    const std::string gameDir=updateOrchestrator.GetGameDirectory();
    std::filesystem::path tempPath=std::filesystem::u8path(tempDir);

    try {
        for(const auto& entry:
            std::filesystem::recursive_directory_iterator(tempPath)) {
            if(entry.is_regular_file()) {
                std::error_code relativeEc;
                std::string relativePath=
                    std::filesystem::relative(entry.path(),tempPath,relativeEc)
                    .generic_string();
                if(relativeEc) {
                    LOG_ERROR("计算相对路径失败: {} - {}",
                        entry.path().string(),relativeEc.message());
                    failedCount++;
                    continue;
                }

                std::string targetPath;
                try {
                    targetPath=FileSystemHelper::SecureCombine(gameDir,relativePath);
                }
                catch(const std::exception& e) {
                    LOG_ERROR("路径遍历被阻止: {} - {}",
                        relativePath,e.what());
                    failedCount++;
                    continue;
                }

                std::filesystem::path targetFilePath=std::filesystem::u8path(targetPath);
                std::filesystem::path targetDir=targetFilePath.parent_path();
                if(!targetDir.empty()) {
                    std::filesystem::create_directories(targetDir);
                }

                bool copySuccess=fsHelper.CopySingleFile(
                    entry.path().string(),targetPath);

                if(copySuccess) {
                    fileCount++;
                    if(fileCount%100==0) {
                        std::cout<<"\r更新进度: "<<fileCount
                            <<" 个文件已处理，失败: "<<failedCount
                            <<"     ";
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
        LOG_ERROR("遍历临时目录失败: {}",e.what());
        return false;
    }

    std::cout<<"\r更新完成: "<<fileCount
        <<" 个文件已处理，失败: "<<failedCount
        <<"                  "<<std::endl;
    LOG_INFO("更新了 {} 个文件，失败: {}",fileCount,failedCount);

    return fileCount>0&&failedCount==0;
}