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
    Json::Value cachedUpdateInfo;
    bool hasCachedUpdateInfo; 
    std::string gameDirectory;
private:
    bool ProcessLauncherUpdate(const Json::Value& updateInfo);
    bool CheckAndApplyLauncherUpdate();
    bool CheckForUpdatesByHash();
    void UpdateLocalVersion(const std::string& newVersion);

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