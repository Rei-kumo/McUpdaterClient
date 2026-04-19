#ifndef UPDATEORCHESTRATOR_H
#define UPDATEORCHESTRATOR_H

#include <string>
#include <json/json.h>
#include "ConfigManager.h"
#include "HttpClient.h"
#include "UpdateChecker.h"
#include "SelfUpdater.h"
#include "ProgressReporter.h"
#include "FileSystemHelper.h"
#include "ZipExtractor.h"
#include "IncrementalUpdatePlanner.h"
#include "HashBasedFileSyncer.h"

class UpdateOrchestrator {
public:
    UpdateOrchestrator(const std::string& config,const std::string& url,const std::string& gameDir);
    ~UpdateOrchestrator();
    bool CheckForUpdates();
    bool ForceUpdate(bool forceSync=false);
    bool SyncFiles(const Json::Value& fileList,bool forceSync);
    void OptimizeMemoryUsage();

    const std::string& GetGameDirectory() const { return gameDirectory; }
    Json::Value GetCachedUpdateInfo() const { return cachedUpdateInfo; }
    bool HasCachedUpdateInfo() const { return hasCachedUpdateInfo; }
    void SetCachedUpdateInfo(const Json::Value& info) { cachedUpdateInfo=info; hasCachedUpdateInfo=true; }
    void ClearCachedUpdateInfo() { cachedUpdateInfo=Json::Value(); hasCachedUpdateInfo=false; }
private:
    bool ProcessLauncherUpdate(const Json::Value& updateInfo);
    bool CheckAndApplyLauncherUpdate();
    bool CheckForUpdatesByHash();
    void UpdateLocalVersion(const std::string& newVersion);
    Json::Value cachedUpdateInfo;
    bool hasCachedUpdateInfo;
    std::string gameDirectory;
    ConfigManager configManager;

    HttpClient httpClient;
    UpdateChecker updateChecker;
    SelfUpdater selfUpdater;
    ProgressReporter progressReporter;
    FileSystemHelper fsHelper;
    ZipExtractor zipExtractor;
    HashBasedFileSyncer hashSyncer;
    IncrementalUpdatePlanner incrementalPlanner;


    bool enableApiCache;
};

#endif