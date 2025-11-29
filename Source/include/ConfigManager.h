#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <string>
#include <json/json.h>
#include "Logger.h"

class ConfigManager{
private:
    std::string configPath;

public:
    ConfigManager(const std::string& configPath);

    bool InitializeDefaultConfig();
    bool ConfigExists();

    std::string ReadVersion();
    bool WriteVersion(const std::string& version);
    std::string ReadUpdateUrl();
    bool WriteUpdateUrl(const std::string& url);
    std::string ReadGameDirectory();
    bool WriteGameDirectory(const std::string& dir);
    bool ReadAutoUpdate();
    bool WriteAutoUpdate(bool autoUpdate);
    std::string ReadLogFile();
    bool WriteLogFile(const std::string& logPath);

    Json::Value ReadConfig();
    bool WriteConfig(const Json::Value& config);

    std::string ReadUpdateMode();
    bool WriteUpdateMode(const std::string& mode);
    std::string ReadHashAlgorithm();
    bool WriteHashAlgorithm(const std::string& algorithm);
    bool ReadEnableFileDeletion();
    bool WriteEnableFileDeletion(bool enable);
    bool ReadSkipMajorVersionCheck();
    bool WriteSkipMajorVersionCheck(bool skip);
    bool ReadEnableApiCache();
    bool WriteEnableApiCache(bool enable);
    int ReadApiTimeout();
    bool WriteApiTimeout(int timeout);

private:
    bool EnsureConfigDirectory();
    Json::Value CreateDefaultConfig();
};

#endif