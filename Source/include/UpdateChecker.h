#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <string>
#include <json/json.h>
#include "HttpClient.h"
#include "ConfigManager.h"
#include "Logger.h"

class UpdateChecker {
private:
	std::string updateUrl;
	HttpClient& httpClient;
	ConfigManager& configManager;
	bool enableApiCache;
public:
	UpdateChecker(const std::string& url,HttpClient& http,ConfigManager& config,bool apiCache=false);

	bool CheckForUpdates();
	Json::Value FetchUpdateInfo();
	void DisplayChangelog(const Json::Value& changelog);

private:
	bool ParseUpdateInfo(const std::string& jsonData,Json::Value& updateInfo);
};

#endif