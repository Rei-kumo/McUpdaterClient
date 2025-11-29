#ifndef MINECRAFTUPDATER_H
#define MINECRAFTUPDATER_H

#include <string>
#include <vector>
#include "HttpClient.h"
#include "ConfigManager.h"
#include "UpdateChecker.h"

class MinecraftUpdater {
private:
	std::string gameDirectory;
	HttpClient httpClient;
	ConfigManager configManager;
	UpdateChecker updateChecker;
	Json::Value cachedUpdateInfo;
public:
	MinecraftUpdater(const std::string& config,const std::string& url,const std::string& gameDir);

	bool CheckForUpdates();
	bool ForceUpdate(bool forceSync=false);

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
	bool hasCachedUpdateInfo;
	bool enableApiCache;
	void CleanupOrphanedFiles(const std::string& directoryPath,const Json::Value& expectedContents);
	
};

#endif