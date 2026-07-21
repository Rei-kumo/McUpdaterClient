#include "UpdateOrchestrator.h"
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
#include "ZipExtractor.h"
#include "HashBasedFileSyncer.h"
#include "IncrementalUpdatePlanner.h"
#include "VersionCompare.h"

UpdateOrchestrator::UpdateOrchestrator(const std::string& config,const std::string& url,const std::string& gameDir)
    : configManager(config),
    httpClient(configManager.ReadApiTimeout()),
    updateChecker(url,httpClient,configManager,configManager.ReadEnableApiCache()),
    selfUpdater(httpClient,configManager),
    progressReporter(),
    fsHelper(),
    zipExtractor(httpClient,progressReporter),
    hashSyncer(httpClient,*this,progressReporter,fsHelper,zipExtractor,configManager),
    incrementalPlanner(httpClient,fsHelper,progressReporter,configManager,*this,zipExtractor),
    enableApiCache(configManager.ReadEnableApiCache()),
    hasCachedUpdateInfo(false),
    gameDirectory(gameDir)
{
    LOG_DEBUG("McUpdaterClient配置: {}", config);
}
UpdateOrchestrator::~UpdateOrchestrator() {
}
bool UpdateOrchestrator::CheckForUpdatesByHash() {
    if(!enableApiCache) {
        LOG_INFO("API缓存已禁用，强制重新获取更新信息");
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;

    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        LOG_INFO("使用缓存的更新信息进行哈希检查");
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
        if(updateInfo.isNull()) {
            LOG_ERROR("错误: 无法获取更新信息");
            return false;
        }
        cachedUpdateInfo=updateInfo;
        hasCachedUpdateInfo=true;
    }

    std::string localVersion=configManager.ReadVersion();
    std::string remoteVersion=updateInfo["version"].asString();

    LOG_INFO("本地版本: {}", localVersion);
    LOG_INFO("远程版本: {}", remoteVersion);

    bool isConsistent=hashSyncer.CheckFileConsistency(updateInfo["files"],updateInfo["directories"]);

    if(IsNewerVersion(localVersion,remoteVersion)) {
        LOG_INFO("发现新版本: {}", remoteVersion);

        if(hashSyncer.ShouldForceHashUpdate(localVersion,remoteVersion)) {
            LOG_INFO("检测到跨越多个版本更新");
        }

        if(!isConsistent) {
            LOG_INFO("文件一致性检查失败，需要更新");
            return true;
        }
        else {
            LOG_INFO("版本号更新但文件已是最新，无需更新");
            return false;
        }
    }
    else if(localVersion==remoteVersion) {
        if(!isConsistent) {
			LOG_INFO("版本号相同但文件不一致，需要修复");
            return true;
        }
        else {
            LOG_INFO("当前已是最新版本且文件完整");
            return false;
        }
    }
    else {
        if(!isConsistent) {
            LOG_WARN("本地版本较新但文件不一致，建议修复");
            std::cout<<"本地版本较新但文件可能损坏，是否修复？(y/n): ";
            char choice;
            std::cin>>choice;
            return (choice=='y'||choice=='Y');
        }
        else {
            LOG_INFO("本地版本较新且文件完整");
            return false;
        }
    }
}
bool UpdateOrchestrator::CheckForUpdates() {
    LOG_INFO("开始检查更新...");

    if(!enableApiCache) {
        LOG_INFO("API缓存已禁用，强制重新获取更新信息");
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        LOG_INFO("使用缓存的更新信息");
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
        if(!updateInfo.isNull()) {
            cachedUpdateInfo=updateInfo;
            hasCachedUpdateInfo=true;
        }
    }

    if(updateInfo.isNull()) {
        LOG_ERROR("错误: 无法获取更新信息");
        return false;
    }

    bool launcherNeedsUpdate=ProcessLauncherUpdate(updateInfo);

    if(launcherNeedsUpdate) {
        std::string remoteLauncherVersion=updateInfo["launcher"]["version"].asString();
        std::string localLauncherVersion=configManager.ReadLauncherVersion();

        LOG_INFO("检测到启动器更新：{} -> {}", localLauncherVersion, remoteLauncherVersion);

        std::string currentVersionBackup=localLauncherVersion;

        if(configManager.ReadAutoUpdate()) {
            LOG_INFO("自动更新已开启，开始更新启动器...");
        }
        else {
            std::cout<<"\n[INFO] 发现启动器更新："<<localLauncherVersion<<" -> "<<remoteLauncherVersion<<std::endl;
            std::cout<<"[INFO] 是否立即更新启动器？ (y/n): ";
            char choice;
            std::cin>>choice;

            if(!(choice=='y'||choice=='Y')) {
                LOG_INFO("用户取消启动器更新");
                launcherNeedsUpdate=false;
            }
        }

        if(launcherNeedsUpdate) {
            if(CheckAndApplyLauncherUpdate()) {
                LOG_INFO("启动器更新流程已启动，程序即将退出...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::exit(0);
            }
            else {
                LOG_ERROR("启动器更新失败");
                if(configManager.ReadLauncherVersion()!=currentVersionBackup) {
                    configManager.WriteLauncherVersion(currentVersionBackup);
                    LOG_INFO("已恢复启动器版本号为原值：{}", currentVersionBackup);
                }
            }
        }
    }

    std::string serverUpdateMode;
    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        serverUpdateMode=updateInfo["update_mode"].asString();
        LOG_INFO("服务端强制使用更新模式: {}", serverUpdateMode);
    }
    else {
        serverUpdateMode=configManager.ReadUpdateMode();
        LOG_INFO("使用客户端配置的更新模式: {}", serverUpdateMode);
    }

    if(serverUpdateMode=="hash") {
        return CheckForUpdatesByHash();
    }
    else {
        std::string localVersion=configManager.ReadVersion();
        std::string remoteVersion=updateInfo["version"].asString();

        if(IsNewerVersion(localVersion,remoteVersion)) {
            LOG_INFO("发现新版本: {}", remoteVersion);
            updateChecker.DisplayChangelog(updateInfo["changelog"]);
            return true;
        }
        else {
            LOG_INFO("当前已是最新版本");
            return false;
        }
    }
}
bool UpdateOrchestrator::ForceUpdate(bool forceSync) {
    if(!enableApiCache) {
        LOG_INFO("API缓存已禁用，强制重新获取更新信息");
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        LOG_INFO("使用缓存的更新信息进行更新");
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
    }

    if(updateInfo.isNull()) {
        LOG_ERROR("错误: 无法获取更新信息");
        return false;
    }

    std::string serverUpdateMode;
    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        serverUpdateMode=updateInfo["update_mode"].asString();
        LOG_INFO("服务端强制使用更新模式: {}", serverUpdateMode);
    }
    else {
        serverUpdateMode=configManager.ReadUpdateMode();
        LOG_INFO("使用客户端配置的更新模式: {}", serverUpdateMode);
    }

    std::string newVersion=updateInfo["version"].asString();
    std::string localVersion=configManager.ReadVersion();

    if(serverUpdateMode=="hash") {
        LOG_INFO("开始更新到版本: {} (哈希模式)", newVersion);
        if(hashSyncer.SyncFilesByHash(updateInfo)) {
            LOG_INFO("文件同步完成，更新版本信息...");
            UpdateLocalVersion(newVersion);
            return true;
        }
        else {
            LOG_ERROR("错误: 更新过程中出现错误!");
            return false;
        }
    }
    else {
        LOG_INFO("开始更新到版本: {} (版本号模式)", newVersion);
        bool useIncremental=false;
        if(updateInfo.isMember("incremental_packages")&&
            updateInfo["incremental_packages"].isArray()&&
            updateInfo["incremental_packages"].size()>0) {

            if(incrementalPlanner.ShouldUseIncrementalUpdate(localVersion,newVersion)) {
                useIncremental=true;
                LOG_INFO("检测到增量更新包，使用增量更新模式");

                if(incrementalPlanner.ApplyIncrementalUpdate(updateInfo,localVersion,newVersion)) {
                    LOG_INFO("增量更新完成，更新版本信息...");
                    UpdateLocalVersion(newVersion);
                    return true;
                }
                else {
                    LOG_WARN("增量更新失败，回退到全量更新");
                }
            }
        }

        bool allSuccess=true;

        Json::Value fileList=updateInfo["files"];
        if(fileList.isArray()&&fileList.size()>0) {
            LOG_INFO("处理文件更新...");
            if(!SyncFiles(fileList,forceSync)) {
                LOG_ERROR("错误: 文件更新失败");
                if(forceSync) return false;
                allSuccess=false;
            }
        }

        Json::Value directoryList=updateInfo["directories"];
        if(directoryList.isArray()&&directoryList.size()>0) {
            LOG_INFO("处理目录更新...");
            for(const auto& dirInfo:directoryList) {
                if(!dirInfo.isObject()) continue;

                std::string path=dirInfo["path"].asString();
                std::string url=dirInfo["url"].asString();

                if(path.empty()||url.empty()) {
                    LOG_ERROR("错误: 目录信息不完整: path={}, url={}", path, url);
                    if(forceSync) return false;
                    allSuccess=false;
                    continue;
                }

                LOG_INFO("更新目录: {}", path);
                if(!zipExtractor.DownloadAndExtract(url,path,gameDirectory)) {
                    LOG_ERROR("错误: 目录更新失败: {}", path);
                    if(forceSync) return false;
                    allSuccess=false;
                }
                else {
                    LOG_INFO("目录更新成功: {}", path);
                }
            }
        }

        if(allSuccess) {
            LOG_INFO("文件同步完成，更新版本信息...");
            UpdateLocalVersion(newVersion);
            return true;
        }
        else {
            LOG_ERROR("错误: 更新过程中出现错误！");
            return false;
        }
    }
}
bool UpdateOrchestrator::CheckAndApplyLauncherUpdate() {
    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
    }

    if(updateInfo.isNull()||!updateInfo.isMember("launcher")) {
        LOG_ERROR("无法获取启动器更新信息");
        return false;
    }

    const Json::Value& launcherInfo=updateInfo["launcher"];
    std::string remoteVersion=launcherInfo["version"].asString();
    std::string downloadUrl=launcherInfo["url"].asString();
    std::string expectedHash=launcherInfo["hash"].asString();

    if(downloadUrl.empty()) {
        LOG_ERROR("启动器下载URL为空");
        return false;
    }

    LOG_INFO("开始下载新启动器版本：{}", remoteVersion);
    LOG_INFO("下载URL：{}", downloadUrl);

    std::string currentVersion=configManager.ReadLauncherVersion();

    if(!selfUpdater.DownloadNewLauncher(downloadUrl,expectedHash,remoteVersion)) {
        LOG_ERROR("下载或验证启动器失败");
        if(configManager.ReadLauncherVersion()!=currentVersion) {
            configManager.WriteLauncherVersion(currentVersion);
            LOG_INFO("已恢复启动器版本号为：{}", currentVersion);
        }
        return false;
    }

    if(!configManager.WriteLauncherVersion(remoteVersion)) {
        LOG_ERROR("无法更新配置中的启动器版本号，更新中止");
        return false;
    }
    else {
        LOG_INFO("已更新配置中的启动器版本号：{}", remoteVersion);
    }

    LOG_INFO("启动器下载完成，准备应用更新...");

    if(selfUpdater.ApplyUpdate()) {
        LOG_INFO("启动器更新已启动，程序将退出");
        return true;
    }
    else {
        LOG_ERROR("应用启动器更新失败");
        configManager.WriteLauncherVersion(currentVersion);
        LOG_INFO("已回滚启动器版本号为：{}", currentVersion);
        return false;
    }
}
bool UpdateOrchestrator::SyncFiles(const Json::Value& fileList,bool forceSync) {
    if(!fileList.isArray()) {
        LOG_ERROR("错误: 文件列表格式错误");
        return false;
    }

    fsHelper.EnsureDirectoryExists(gameDirectory);

    bool allSuccess=true;

    for(const auto& fileInfo:fileList) {
        if(!fileInfo.isObject()) continue;

        std::string path=fileInfo["path"].asString();
        std::string url=fileInfo["url"].asString();
        std::string type=fileInfo.isMember("type")?fileInfo["type"].asString():"file";

        if(path.empty()||url.empty()) {
            LOG_ERROR("错误: 文件信息不完整: path={}, url={}", path, url);
            if(forceSync) return false;
            allSuccess=false;
            continue;
        }

        LOG_DEBUG("检查URL: {}", url);

        if(type=="directory") {
            LOG_INFO("更新目录: {}", path);
            if(fileInfo.isMember("hash")) {
                LOG_DEBUG("目录哈希: {}", fileInfo["hash"].asString());
            }
            if(fileInfo.isMember("size")) {
                LOG_DEBUG("期望大小: {}", progressReporter.FormatBytes(fileInfo["size"].asInt64()));
            }

            std::string safeFullPath;
            try {
                safeFullPath=FileSystemHelper::SecureCombine(gameDirectory,path);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {} (目录: {})", e.what(), path);
                if(forceSync) return false;
                allSuccess=false;
                continue;
            }

            if(!zipExtractor.DownloadAndExtract(url,path,gameDirectory)) {
                LOG_ERROR("错误: 目录更新失败: {}", path);

                if(forceSync) {
                    LOG_ERROR("强制同步模式，更新失败");
                    return false;
                }

                allSuccess=false;

                LOG_WARN("尝试创建空目录作为后备: {}", safeFullPath);

                try {
                    std::filesystem::create_directories(safeFullPath);
                    LOG_INFO("已创建空目录: {}", safeFullPath);
                }
                catch(const std::exception& e) {
                    LOG_ERROR("创建空目录失败: {}", e.what());
                }
            }
            else {
                LOG_INFO("目录更新成功: {}", path);
            }
        }
        else {
            std::string fullPath;
            try {
                fullPath=FileSystemHelper::SecureCombine(gameDirectory,path);
            }
            catch(const std::exception& e) {
                LOG_ERROR("路径遍历被阻止: {}", e.what());
                if(forceSync) return false;
                allSuccess=false;
                continue;
            }
            std::string outputDir=std::filesystem::path(fullPath).parent_path().string();
            fsHelper.EnsureDirectoryExists(outputDir);

            if(std::filesystem::exists(fullPath)) {
                LOG_INFO("备份原有文件: {}", fullPath);
                if(!fsHelper.BackupFile(fullPath)) {
                    LOG_WARN("警告: 文件备份失败，但继续更新...");
                }
            }

            LOG_INFO("下载文件: {} -> {}", url, fullPath);

            long long expectedSize=0;
            if(fileInfo.isMember("size")) {
                expectedSize=fileInfo["size"].asInt64();
                LOG_DEBUG("期望文件大小: {}", progressReporter.FormatBytes(expectedSize));
            }

            std::string progressMessage="下载 "+path;
            if(expectedSize>0) {
                progressReporter.show(progressMessage,0,expectedSize);
            }
            else {
                progressReporter.show(progressMessage,0,1);
            }

            if(!httpClient.DownloadFileWithProgress(url,fullPath,
                [this,progressMessage,expectedSize](long long downloaded,long long total,void* userdata) {
                    if(total<=0&&expectedSize>0) {
                        total=expectedSize;
                    }
                    progressReporter.show(progressMessage,downloaded,total);
                },nullptr)) {

                progressReporter.clear();
                LOG_ERROR("错误: 文件下载失败: {}", path);
                if(forceSync) return false;
                allSuccess=false;
            }
            else {
                progressReporter.clear();
                LOG_INFO("文件下载成功: {}", path);
            }
        }
    }

    return allSuccess;
}
void UpdateOrchestrator::UpdateLocalVersion(const std::string& newVersion) {
    if(configManager.WriteVersion(newVersion)) {
        LOG_INFO("版本信息已更新为: {}", newVersion);
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }
    else {
        LOG_ERROR("错误: 更新版本信息失败");
    }
}
void UpdateOrchestrator::OptimizeMemoryUsage() {
    static int callCount=0;
    callCount++;
    if(callCount%10==0) {
        SetProcessWorkingSetSize(GetCurrentProcess(),(SIZE_T)-1,(SIZE_T)-1);
        HANDLE heap=GetProcessHeap();
        if(heap) {
            HeapCompact(heap,HEAP_NO_SERIALIZE);
        }
    }
}
bool UpdateOrchestrator::ProcessLauncherUpdate(const Json::Value& updateInfo) {
    if(!updateInfo.isMember("launcher")||!updateInfo["launcher"].isObject()) {
        return false;
    }

    const Json::Value& launcherInfo=updateInfo["launcher"];
    if(!launcherInfo.isMember("version")||!launcherInfo.isMember("url")) {
        return false;
    }

    std::string remoteVersion=launcherInfo["version"].asString();
    std::string localVersion=configManager.ReadLauncherVersion();

    bool needsUpdate=(IsNewerVersion(localVersion,remoteVersion));

    if(needsUpdate) {
        LOG_INFO("检测到启动器更新：{} -> {}", localVersion, remoteVersion);
    }
    else {
        LOG_DEBUG("启动器已是最新版本：{}", localVersion);
    }

    return needsUpdate;
}