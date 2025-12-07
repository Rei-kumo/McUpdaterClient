#include "ConfigManager.h"
#include <iostream>
#include <fstream>
#include <filesystem>

ConfigManager::ConfigManager(const std::string& configPath):configPath(configPath),configLoaded(false) {
    if(configPath.empty()) {
        g_logger<<"[ERROR]配置文件路径为空!"<<std::endl;
        return;
    }

    if(!EnsureConfigDirectory()) {
        g_logger<<"[ERROR]无法创建配置目录"<<std::endl;
        return;
    }

    LoadConfig();
}

ConfigManager::~ConfigManager() {
    cachedConfig.clear();
    configLoaded=false;
}

bool ConfigManager::LoadConfig() {
    if(configPath.empty()) {
        g_logger<<"[ERROR]配置文件路径为空"<<std::endl;
        return false;
    }

    std::ifstream file(configPath);
    if(!file.is_open()) {
        g_logger<<"[WARN]无法打开配置文件: "<<configPath<<std::endl;
        return false;
    }

    Json::CharReaderBuilder reader;
    std::string errors;

    if(!Json::parseFromStream(reader,file,&cachedConfig,&errors)) {
        g_logger<<"[ERROR]配置解析错误: "<<errors<<std::endl;
        file.close();
        return false;
    }

    file.close();
    configLoaded=true;
    return true;
}

Json::Value ConfigManager::ReadConfig(){
    if(configLoaded) {
        return cachedConfig;
    }

    if(LoadConfig()) {
        return cachedConfig;
    }

    return Json::Value();
}

std::string ConfigManager::ReadVersion(){
    Json::Value config=ReadConfig();
    if(config.isMember("version")){
        return config["version"].asString();
    }
    return "1.0.0";
}

std::string ConfigManager::ReadUpdateUrl(){
    Json::Value config=ReadConfig();
    if(config.isMember("update_url")){
        return config["update_url"].asString();
    }
    return "";
}

std::string ConfigManager::ReadGameDirectory(){
    Json::Value config=ReadConfig();
    if(config.isMember("game_directory")){
        return config["game_directory"].asString();
    }
    return "./.minecraft";
}

bool ConfigManager::ReadAutoUpdate(){
    Json::Value config=ReadConfig();
    if(config.isMember("auto_update")){
        return config["auto_update"].asBool();
    }
    return true;
}

std::string ConfigManager::ReadLogFile(){
    Json::Value config=ReadConfig();
    if(config.isMember("log_file")){
        return config["log_file"].asString();
    }
    return "./logs/updater.log";
}

std::string ConfigManager::ReadUpdateMode() {
    Json::Value config=ReadConfig();
    if(config.isMember("update_mode")) {
        return config["update_mode"].asString();
    }
    return "hash";
}

std::string ConfigManager::ReadHashAlgorithm() {
    Json::Value config=ReadConfig();
    if(config.isMember("hash_algorithm")) {
        return config["hash_algorithm"].asString();
    }
    return "md5";
}

bool ConfigManager::ReadEnableFileDeletion() {
    Json::Value config=ReadConfig();
    if(config.isMember("enable_file_deletion")) {
        return config["enable_file_deletion"].asBool();
    }
    return true;
}

bool ConfigManager::ReadSkipMajorVersionCheck() {
    Json::Value config=ReadConfig();
    if(config.isMember("skip_major_version_check")) {
        return config["skip_major_version_check"].asBool();
    }
    return false;
}

bool ConfigManager::WriteVersion(const std::string& version){
    Json::Value config=ReadConfig();
    config["version"]=version;
    return WriteConfig(config);
}

bool ConfigManager::WriteUpdateUrl(const std::string& url){
    Json::Value config=ReadConfig();
    config["update_url"]=url;
    return WriteConfig(config);
}

bool ConfigManager::WriteGameDirectory(const std::string& dir){
    Json::Value config=ReadConfig();
    config["game_directory"]=dir;
    return WriteConfig(config);
}

bool ConfigManager::WriteAutoUpdate(bool autoUpdate){
    Json::Value config=ReadConfig();
    config["auto_update"]=autoUpdate;
    return WriteConfig(config);
}

bool ConfigManager::WriteLogFile(const std::string& logPath){
    Json::Value config=ReadConfig();
    config["log_file"]=logPath;
    return WriteConfig(config);
}

bool ConfigManager::WriteUpdateMode(const std::string& mode) {
    Json::Value config=ReadConfig();
    config["update_mode"]=mode;
    return WriteConfig(config);
}

bool ConfigManager::WriteHashAlgorithm(const std::string& algorithm) {
    Json::Value config=ReadConfig();
    config["hash_algorithm"]=algorithm;
    return WriteConfig(config);
}

bool ConfigManager::WriteEnableFileDeletion(bool enable) {
    Json::Value config=ReadConfig();
    config["enable_file_deletion"]=enable;
    return WriteConfig(config);
}

bool ConfigManager::WriteSkipMajorVersionCheck(bool skip) {
    Json::Value config=ReadConfig();
    config["skip_major_version_check"]=skip;
    return WriteConfig(config);
}

bool ConfigManager::ConfigExists(){
    return std::filesystem::exists(configPath);
}

bool ConfigManager::InitializeDefaultConfig(){
    if(!EnsureConfigDirectory()){
        g_logger<<"[ERROR]无法创建配置目录"<<std::endl;
        return false;
    }

    Json::Value defaultConfig=CreateDefaultConfig();
    bool result=WriteConfig(defaultConfig);
    if(result){
        g_logger<<"[信息]已创建默认配置文件:"<<configPath<<std::endl;
    }
    else{
        g_logger<<"[ERROR]创建默认配置文件失败"<<std::endl;
    }
    return result;
}

Json::Value ConfigManager::CreateDefaultConfig(){
    Json::Value config;
    config["version"]="1.0.0";
    config["launcher_version"]="0.0.1";
    config["update_url"]="https://your-server.com/updates/version.json";
    config["game_directory"]="./.minecraft";
    config["auto_update"]=true;
    config["log_file"]="./logs/updater.log";
    config["update_mode"]="version";
    config["hash_algorithm"]="md5";
    config["enable_file_deletion"]=true;
    config["skip_major_version_check"]=false;
    config["enable_api_cache"]=true;
    config["api_timeout"]=600;
    return config;
}

std::string ConfigManager::ReadLauncherVersion() {
    Json::Value config=ReadConfig();
    if(config.isMember("launcher_version")) {
        return config["launcher_version"].asString();
    }
    return "0.0.1";
}

bool ConfigManager::WriteLauncherVersion(const std::string& version) {
    Json::Value config=ReadConfig();
    config["launcher_version"]=version;
    return WriteConfig(config);
}

bool ConfigManager::WriteConfig(const Json::Value& config){
    if(!EnsureConfigDirectory()){
        return false;
    }

    if(configPath.empty()) {
        g_logger<<"[ERROR]配置文件路径为空"<<std::endl;
        return false;
    }

    std::ofstream file(configPath);
    if(!file.is_open()){
        g_logger<<"[ERROR]无法打开配置文件进行写入: "<<configPath<<std::endl;
        return false;
    }

    Json::StreamWriterBuilder writer;
    std::string jsonString=Json::writeString(writer,config);
    file<<jsonString;
    file.close();

    cachedConfig=config;
    configLoaded=true;

    return true;
}

bool ConfigManager::EnsureConfigDirectory(){
    std::filesystem::path path(configPath);
    std::filesystem::path dir=path.parent_path();

    if(!dir.empty()&&!std::filesystem::exists(dir)){
        return std::filesystem::create_directories(dir);
    }

    return true;
}

bool ConfigManager::ReadEnableApiCache() {
    Json::Value config=ReadConfig();
    if(config.isMember("enable_api_cache")) {
        return config["enable_api_cache"].asBool();
    }
    return true;
}

int ConfigManager::ReadApiTimeout() {
    Json::Value config=ReadConfig();
    if(config.isMember("api_timeout")) {
        return config["api_timeout"].asInt();
    }
    return 60;
}

bool ConfigManager::WriteEnableApiCache(bool enable) {
    Json::Value config=ReadConfig();
    config["enable_api_cache"]=enable;
    return WriteConfig(config);
}

bool ConfigManager::WriteApiTimeout(int timeout) {
    Json::Value config=ReadConfig();
    config["api_timeout"]=timeout;
    return WriteConfig(config);
}