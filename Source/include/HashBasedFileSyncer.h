#ifndef HASHBASEDFILESYNCER_H
#define HASHBASEDFILESYNCER_H

#include <string>
#include <json/json.h>
#include "HttpClient.h"
#include "ConfigManager.h"
#include "ProgressReporter.h"

#include "FileSystemHelper.h"
class UpdateOrchestrator;
class ZipExtractor;
class HashBasedFileSyncer {
public:
    HashBasedFileSyncer(HttpClient& http,
        UpdateOrchestrator& orc,
        ProgressReporter& reporter,
        FileSystemHelper& fs,
        ZipExtractor& zip,
        ConfigManager& config);
    bool CheckFileConsistency(const Json::Value& fileManifest,const Json::Value& directoryManifest);
    bool SyncFilesByHash(const Json::Value& updateInfo);
    bool ProcessDeleteList(const Json::Value& deleteList);
    bool ShouldForceHashUpdate(const std::string& localVersion,const std::string& remoteVersion);
private:
    bool UpdateFilesByHash(const Json::Value& fileManifest,const Json::Value& directoryManifest);
    bool SyncDirectoryByHash(const Json::Value& dirInfo);
    int GetDownloadTimeoutForSize(long long fileSize);
    HttpClient& httpClient;
    UpdateOrchestrator& updateOrchestrator;
    ProgressReporter& progressReporter;
    FileSystemHelper& fsHelper;
    ZipExtractor& zipExtractor;
    ConfigManager& configManager;
};
#endif