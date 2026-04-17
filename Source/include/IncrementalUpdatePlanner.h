#ifndef INCREMENTALUPDATEPLANNER_H
#define INCREMENTALUPDATEPLANNER_H

#include <string>
#include <vector>
#include <json/json.h>
#include "HttpClient.h"
#include "ConfigManager.h"
#include "ProgressReporter.h"
#include "ZipExtractor.h"
#include "FileSystemHelper.h"

class UpdateOrchestrator;
class IncrementalUpdatePlanner {
public:
    IncrementalUpdatePlanner(HttpClient& http,
        FileSystemHelper& fs,
        ProgressReporter& reporter,
        ConfigManager& config,
        UpdateOrchestrator& orc,
        ZipExtractor& zip);
    bool ShouldUseIncrementalUpdate(const std::string& localVersion,const std::string& remoteVersion);
    std::vector<std::string> GetUpdatePackagePath(const Json::Value& packages,
        const std::string& fromVersion,
        const std::string& toVersion);
    bool ApplyIncrementalUpdate(const Json::Value& updateInfo,
        const std::string& localVersion,
        const std::string& remoteVersion);

private:
    bool ApplyUpdateFromManifest(const std::string& manifestPath,const std::string& tempDir);
    bool ApplyUpdateFromDirectory(const std::string& sourceDir);
    bool ApplyAllFilesFromUpdate(const std::string& tempDir);

    // 依赖
    HttpClient& httpClient;
    FileSystemHelper& fsHelper;
    ProgressReporter& progressReporter;
    ConfigManager& configManager;
    UpdateOrchestrator& updateOrchestrator;
    ZipExtractor& zipExtractor;
};

#endif