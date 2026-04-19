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
    g_logger<<"[DEBUG] McUpdaterClient配置: "<<config<<std::endl;
}
UpdateOrchestrator::~UpdateOrchestrator() {
}
bool UpdateOrchestrator::CheckForUpdatesByHash() {
    if(!enableApiCache) {
        g_logger<<"[INFO] API缓存已禁用，强制重新获取更新信息"<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;

    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        g_logger<<"[INFO] 使用缓存的更新信息进行哈希检查"<<std::endl;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
        if(updateInfo.isNull()) {
            g_logger<<"[ERROR] 错误: 无法获取更新信息"<<std::endl;
            return false;
        }
        cachedUpdateInfo=updateInfo;
        hasCachedUpdateInfo=true;
    }

    std::string localVersion=configManager.ReadVersion();
    std::string remoteVersion=updateInfo["version"].asString();

    g_logger<<"[INFO] 本地版本: "<<localVersion<<std::endl;
    g_logger<<"[INFO] 远程版本: "<<remoteVersion<<std::endl;

    bool isConsistent=hashSyncer.CheckFileConsistency(updateInfo["files"],updateInfo["directories"]);

    if(IsNewerVersion(localVersion,remoteVersion)) {
        std::cout<<"[INFO] 发现新版本: "<<remoteVersion<<std::endl;

        if(hashSyncer.ShouldForceHashUpdate(localVersion,remoteVersion)) {
            g_logger<<"[INFO] 检测到跨越多个版本更新"<<std::endl;
        }

        if(!isConsistent) {
            g_logger<<"[INFO] 文件一致性检查失败，需要更新"<<std::endl;
            return true;
        }
        else {
            g_logger<<"[INFO] 版本号更新但文件已是最新，无需更新"<<std::endl;
            return false;
        }
    }
    else if(localVersion==remoteVersion) {
        if(!isConsistent) {
            g_logger<<"[INFO] 版本号相同但文件不一致，需要修复"<<std::endl;
            return true;
        }
        else {
            g_logger<<"[INFO] 当前已是最新版本且文件完整"<<std::endl;
            return false;
        }
    }
    else {
        if(!isConsistent) {
            g_logger<<"[WARN] 本地版本较新但文件不一致，建议修复"<<std::endl;
            std::cout<<"[WARN] 本地版本较新但文件可能损坏，是否修复？(y/n): ";
            char choice;
            std::cin>>choice;
            return (choice=='y'||choice=='Y');
        }
        else {
            g_logger<<"[INFO] 本地版本较新且文件完整"<<std::endl;
            return false;
        }
    }
}
bool UpdateOrchestrator::CheckForUpdates() {
    g_logger<<"[INFO] 开始检查更新..."<<std::endl;

    if(!enableApiCache) {
        g_logger<<"[INFO] API缓存已禁用，强制重新获取更新信息"<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        g_logger<<"[INFO] 使用缓存的更新信息"<<std::endl;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
        if(!updateInfo.isNull()) {
            cachedUpdateInfo=updateInfo;
            hasCachedUpdateInfo=true;
        }
    }

    if(updateInfo.isNull()) {
        g_logger<<"[ERROR] 错误: 无法获取更新信息"<<std::endl;
        return false;
    }

    bool launcherNeedsUpdate=ProcessLauncherUpdate(updateInfo);

    if(launcherNeedsUpdate) {
        std::string remoteLauncherVersion=updateInfo["launcher"]["version"].asString();
        std::string localLauncherVersion=configManager.ReadLauncherVersion();

        g_logger<<"[INFO] 检测到启动器更新："<<localLauncherVersion<<" -> "<<remoteLauncherVersion<<std::endl;

        std::string currentVersionBackup=localLauncherVersion;

        if(configManager.ReadAutoUpdate()) {
            g_logger<<"[INFO] 自动更新已开启，开始更新启动器..."<<std::endl;
        }
        else {
            std::cout<<"\n[INFO] 发现启动器更新："<<localLauncherVersion<<" -> "<<remoteLauncherVersion<<std::endl;
            std::cout<<"[INFO] 是否立即更新启动器？ (y/n): ";
            char choice;
            std::cin>>choice;

            if(!(choice=='y'||choice=='Y')) {
                g_logger<<"[INFO] 用户取消启动器更新"<<std::endl;
                launcherNeedsUpdate=false;
            }
        }

        if(launcherNeedsUpdate) {
            if(CheckAndApplyLauncherUpdate()) {
                g_logger<<"[INFO] 启动器更新流程已启动，程序即将退出..."<<std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::exit(0);
            }
            else {
                g_logger<<"[ERROR] 启动器更新失败"<<std::endl;
                if(configManager.ReadLauncherVersion()!=currentVersionBackup) {
                    configManager.WriteLauncherVersion(currentVersionBackup);
                    g_logger<<"[INFO] 已恢复启动器版本号为原值："<<currentVersionBackup<<std::endl;
                }
            }
        }
    }

    std::string serverUpdateMode;
    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        serverUpdateMode=updateInfo["update_mode"].asString();
        g_logger<<"[INFO] 服务端强制使用更新模式: "<<serverUpdateMode<<std::endl;
    }
    else {
        serverUpdateMode=configManager.ReadUpdateMode();
        g_logger<<"[INFO] 使用客户端配置的更新模式: "<<serverUpdateMode<<std::endl;
    }

    if(serverUpdateMode=="hash") {
        return CheckForUpdatesByHash();
    }
    else {
        std::string localVersion=configManager.ReadVersion();
        std::string remoteVersion=updateInfo["version"].asString();

        if(IsNewerVersion(localVersion,remoteVersion)) {
            g_logger<<"[INFO] 发现新版本: "<<remoteVersion<<std::endl;
            updateChecker.DisplayChangelog(updateInfo["changelog"]);
            return true;
        }
        else {
            g_logger<<"[INFO] 当前已是最新版本"<<std::endl;
            return false;
        }
    }
}
bool UpdateOrchestrator::ForceUpdate(bool forceSync) {
    if(!enableApiCache) {
        g_logger<<"[INFO] API缓存已禁用，强制重新获取更新信息"<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        g_logger<<"[INFO] 使用缓存的更新信息进行更新"<<std::endl;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
    }

    if(updateInfo.isNull()) {
        g_logger<<"[ERROR] 错误: 无法获取更新信息"<<std::endl;
        return false;
    }

    std::string serverUpdateMode;
    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        serverUpdateMode=updateInfo["update_mode"].asString();
        g_logger<<"[INFO] 服务端强制使用更新模式: "<<serverUpdateMode<<std::endl;
    }
    else {
        serverUpdateMode=configManager.ReadUpdateMode();
        g_logger<<"[INFO] 使用客户端配置的更新模式: "<<serverUpdateMode<<std::endl;
    }

    std::string newVersion=updateInfo["version"].asString();
    std::string localVersion=configManager.ReadVersion();

    if(serverUpdateMode=="hash") {
        g_logger<<"[INFO] 开始更新到版本: "<<newVersion<<" (哈希模式)"<<std::endl;
        if(hashSyncer.SyncFilesByHash(updateInfo)) {
            g_logger<<"[INFO] 文件同步完成，更新版本信息..."<<std::endl;
            UpdateLocalVersion(newVersion);
            return true;
        }
        else {
            g_logger<<"[ERROR] 错误: 更新过程中出现错误!"<<std::endl;
            return false;
        }
    }
    else {
        g_logger<<"[INFO] 开始更新到版本: "<<newVersion<<" (版本号模式)"<<std::endl;
        bool useIncremental=false;
        if(updateInfo.isMember("incremental_packages")&&
            updateInfo["incremental_packages"].isArray()&&
            updateInfo["incremental_packages"].size()>0) {

            if(incrementalPlanner.ShouldUseIncrementalUpdate(localVersion,newVersion)) {
                useIncremental=true;
                g_logger<<"[INFO] 检测到增量更新包，使用增量更新模式"<<std::endl;

                if(incrementalPlanner.ApplyIncrementalUpdate(updateInfo,localVersion,newVersion)) {
                    g_logger<<"[INFO] 增量更新完成，更新版本信息..."<<std::endl;
                    UpdateLocalVersion(newVersion);
                    return true;
                }
                else {
                    g_logger<<"[WARN] 增量更新失败，回退到全量更新"<<std::endl;
                }
            }
        }

        bool allSuccess=true;

        Json::Value fileList=updateInfo["files"];
        if(fileList.isArray()&&fileList.size()>0) {
            g_logger<<"[INFO] 处理文件更新..."<<std::endl;
            if(!SyncFiles(fileList,forceSync)) {
                g_logger<<"[ERROR] 错误: 文件更新失败"<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
            }
        }

        Json::Value directoryList=updateInfo["directories"];
        if(directoryList.isArray()&&directoryList.size()>0) {
            g_logger<<"[INFO] 处理目录更新..."<<std::endl;
            for(const auto& dirInfo:directoryList) {
                if(!dirInfo.isObject()) continue;

                std::string path=dirInfo["path"].asString();
                std::string url=dirInfo["url"].asString();

                if(path.empty()||url.empty()) {
                    g_logger<<"[ERROR] 错误: 目录信息不完整: path="<<path<<", url="<<url<<std::endl;
                    if(forceSync) return false;
                    allSuccess=false;
                    continue;
                }

                g_logger<<"[INFO] 更新目录: "<<path<<std::endl;
                if(!zipExtractor.DownloadAndExtract(url,path,gameDirectory)) {
                    g_logger<<"[ERROR] 错误: 目录更新失败: "<<path<<std::endl;
                    if(forceSync) return false;
                    allSuccess=false;
                }
                else {
                    g_logger<<"[INFO] 目录更新成功: "<<path<<std::endl;
                }
            }
        }

        if(allSuccess) {
            g_logger<<"[INFO] 文件同步完成，更新版本信息..."<<std::endl;
            UpdateLocalVersion(newVersion);
            return true;
        }
        else {
            g_logger<<"[ERROR] 错误: 更新过程中出现错误！"<<std::endl;
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
        g_logger<<"[ERROR] 无法获取启动器更新信息"<<std::endl;
        return false;
    }

    const Json::Value& launcherInfo=updateInfo["launcher"];
    std::string remoteVersion=launcherInfo["version"].asString();
    std::string downloadUrl=launcherInfo["url"].asString();
    std::string expectedHash=launcherInfo["hash"].asString();

    if(downloadUrl.empty()) {
        g_logger<<"[ERROR] 启动器下载URL为空"<<std::endl;
        return false;
    }

    g_logger<<"[INFO] 开始下载新启动器版本："<<remoteVersion<<std::endl;
    g_logger<<"[INFO] 下载URL："<<downloadUrl<<std::endl;

    std::string currentVersion=configManager.ReadLauncherVersion();

    if(!selfUpdater.DownloadNewLauncher(downloadUrl,expectedHash,remoteVersion)) {
        g_logger<<"[ERROR] 下载或验证启动器失败"<<std::endl;
        if(configManager.ReadLauncherVersion()!=currentVersion) {
            configManager.WriteLauncherVersion(currentVersion);
            g_logger<<"[INFO] 已恢复启动器版本号为："<<currentVersion<<std::endl;
        }
        return false;
    }

    if(!configManager.WriteLauncherVersion(remoteVersion)) {
        g_logger<<"[ERROR] 无法更新配置中的启动器版本号，更新中止"<<std::endl;
        return false;
    }
    else {
        g_logger<<"[INFO] 已更新配置中的启动器版本号："<<remoteVersion<<std::endl;
    }

    g_logger<<"[INFO] 启动器下载完成，准备应用更新..."<<std::endl;

    if(selfUpdater.ApplyUpdate()) {
        g_logger<<"[INFO] 启动器更新已启动，程序将退出"<<std::endl;
        return true;
    }
    else {
        g_logger<<"[ERROR] 应用启动器更新失败"<<std::endl;
        configManager.WriteLauncherVersion(currentVersion);
        g_logger<<"[INFO] 已回滚启动器版本号为："<<currentVersion<<std::endl;
        return false;
    }
}
bool UpdateOrchestrator::SyncFiles(const Json::Value& fileList,bool forceSync) {
    if(!fileList.isArray()) {
        g_logger<<"[ERROR] 错误: 文件列表格式错误"<<std::endl;
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
            g_logger<<"[ERROR] 错误: 文件信息不完整: path="<<path<<", url="<<url<<std::endl;
            if(forceSync) return false;
            allSuccess=false;
            continue;
        }

        g_logger<<"[DEBUG] 检查URL: "<<url<<std::endl;

        if(type=="directory") {
            g_logger<<"[INFO] 更新目录: "<<path<<std::endl;
            if(fileInfo.isMember("hash")) {
                g_logger<<"[DEBUG] 目录哈希: "<<fileInfo["hash"].asString()<<std::endl;
            }
            if(fileInfo.isMember("size")) {
                g_logger<<"[DEBUG] 期望大小: "<<progressReporter.FormatBytes(fileInfo["size"].asInt64())<<std::endl;
            }

            std::string safeFullPath;
            try {
                safeFullPath=FileSystemHelper::SecureCombine(gameDirectory,path);
            }
            catch(const std::exception& e) {
                g_logger<<"[ERROR] 路径遍历被阻止: "<<e.what()<<" (目录: "<<path<<")"<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
                continue;
            }

            if(!zipExtractor.DownloadAndExtract(url,path,gameDirectory)) {
                g_logger<<"[ERROR] 错误: 目录更新失败: "<<path<<std::endl;

                if(forceSync) {
                    g_logger<<"[ERROR] 强制同步模式，更新失败"<<std::endl;
                    return false;
                }

                allSuccess=false;

                g_logger<<"[WARN] 尝试创建空目录作为后备: "<<safeFullPath<<std::endl;

                try {
                    std::filesystem::create_directories(safeFullPath);
                    g_logger<<"[INFO] 已创建空目录: "<<safeFullPath<<std::endl;
                }
                catch(const std::exception& e) {
                    g_logger<<"[ERROR] 创建空目录失败: "<<e.what()<<std::endl;
                }
            }
            else {
                g_logger<<"[INFO] 目录更新成功: "<<path<<std::endl;
            }
        }
        else {
            std::string fullPath;
            try {
                fullPath=FileSystemHelper::SecureCombine(gameDirectory,path);
            }
            catch(const std::exception& e) {
                g_logger<<"[ERROR] 路径遍历被阻止: "<<e.what()<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
                continue;
            }
            std::string outputDir=std::filesystem::path(fullPath).parent_path().string();
            fsHelper.EnsureDirectoryExists(outputDir);

            if(std::filesystem::exists(fullPath)) {
                g_logger<<"[INFO] 备份原有文件: "<<fullPath<<std::endl;
                if(!fsHelper.BackupFile(fullPath)) {
                    g_logger<<"[WARN] 警告: 文件备份失败，但继续更新..."<<std::endl;
                }
            }

            g_logger<<"[INFO] 下载文件: "<<url<<" -> "<<fullPath<<std::endl;

            long long expectedSize=0;
            if(fileInfo.isMember("size")) {
                expectedSize=fileInfo["size"].asInt64();
                g_logger<<"[DEBUG] 期望文件大小: "<<progressReporter.FormatBytes(expectedSize)<<std::endl;
            }

            std::string progressMessage="下载 "+path;
            if(expectedSize>0) {
                progressReporter.ShowProgressBar(progressMessage,0,expectedSize);
            }
            else {
                progressReporter.ShowProgressBar(progressMessage,0,1);
            }

            if(!httpClient.DownloadFileWithProgress(url,fullPath,
                [this,progressMessage,expectedSize](long long downloaded,long long total,void* userdata) {
                    if(total<=0&&expectedSize>0) {
                        total=expectedSize;
                    }
                    progressReporter.ShowProgressBar(progressMessage,downloaded,total);
                },nullptr)) {

                progressReporter.ClearProgressLine();
                g_logger<<"[ERROR] 错误: 文件下载失败: "<<path<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
            }
            else {
                progressReporter.ClearProgressLine();
                g_logger<<"[INFO] 文件下载成功: "<<path<<std::endl;
            }
        }
    }

    return allSuccess;
}
void UpdateOrchestrator::UpdateLocalVersion(const std::string& newVersion) {
    if(configManager.WriteVersion(newVersion)) {
        g_logger<<"[INFO] 版本信息已更新为: "<<newVersion<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }
    else {
        g_logger<<"[ERROR] 错误: 更新版本信息失败"<<std::endl;
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
        g_logger<<"[INFO] 检测到启动器更新："<<localVersion<<" -> "<<remoteVersion<<std::endl;
    }
    else {
        g_logger<<"[DEBUG] 启动器已是最新版本："<<localVersion<<std::endl;
    }

    return needsUpdate;
}