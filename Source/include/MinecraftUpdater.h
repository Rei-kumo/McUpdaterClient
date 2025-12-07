#ifndef MINECRAFTUPDATER_H
#define MINECRAFTUPDATER_H

#include <string>
#include <vector>
#include "HttpClient.h"
#include "ConfigManager.h"
#include "UpdateChecker.h"
#include "SelfUpdater.h"
#include "FileHasher.h"
#include "Logger.h"
#include <algorithm>
#include <queue>
#include <map>
#include <iomanip>
#include <sstream>

class MinecraftUpdater {
private:
    ConfigManager configManager;
    std::string gameDirectory;
    HttpClient httpClient;
    UpdateChecker updateChecker;
    SelfUpdater selfUpdater;
    Json::Value cachedUpdateInfo;
    bool hasCachedUpdateInfo;
    bool enableApiCache;
public:
    MinecraftUpdater(const std::string& config,const std::string& url,const std::string& gameDir);

    bool CheckForUpdates();
    bool ForceUpdate(bool forceSync=false);
    bool CheckAndApplyLauncherUpdate();

private:
    bool DownloadAndExtract(const std::string& url,const std::string& relativePath);
    bool ExtractZip(const std::vector<unsigned char>& zipData,const std::string& extractPath);
    bool SyncFiles(const Json::Value& fileList,bool forceSync);
    bool BackupFile(const std::string& filePath);
    void EnsureDirectoryExists(const std::string& path);
    void UpdateLocalVersion(const std::string& newVersion);

    bool CheckForUpdatesByHash();
    bool ShouldForceHashUpdate(const std::string& localVersion,const std::string& remoteVersion);
    bool SyncFilesByHash(const Json::Value& updateInfo);
    bool CheckFileConsistency(const Json::Value& fileManifest,const Json::Value& directoryManifest);
    bool UpdateFilesByHash(const Json::Value& fileManifest,const Json::Value& directoryManifest);
    bool ProcessDeleteList(const Json::Value& deleteList);
    bool SyncDirectoryByHash(const Json::Value& dirInfo);
    void CleanupOrphanedFiles(const std::string& directoryPath,const Json::Value& expectedContents);

    bool ProcessLauncherUpdate(const Json::Value& updateInfo);
    bool ApplyIncrementalUpdate(const Json::Value& updateInfo,const std::string& localVersion,const std::string& remoteVersion);
    std::vector<std::string> GetUpdatePackagePath(const Json::Value& packages,const std::string& fromVersion,const std::string& toVersion);
    bool ShouldUseIncrementalUpdate(const std::string& localVersion,const std::string& remoteVersion);
    static void DownloadProgressCallback(long long downloaded,long long total,void* userdata);
    void ShowProgressBar(const std::string& operation,long long current,long long total);
    void ClearProgressLine();
    int GetDownloadTimeoutForSize(long long fileSize);
    bool ApplyUpdateFromManifest(const std::string& manifestPath,const std::string& tempDir);
    bool ApplyAllFilesFromUpdate(const std::string& tempDir);
};

#endif