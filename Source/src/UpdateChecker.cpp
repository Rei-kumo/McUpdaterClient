#include "UpdateChecker.h"
#include <iostream>
#include <sstream>

UpdateChecker::UpdateChecker(const std::string& url,HttpClient& http,ConfigManager& config,bool apiCache)
    : updateUrl(url),httpClient(http),configManager(config),enableApiCache(apiCache) {
}

bool UpdateChecker::CheckForUpdates() {
    Json::Value updateInfo=FetchUpdateInfo();
    if(updateInfo.isNull()) {
        return false;
    }

    std::string localVersion=configManager.ReadVersion();
    std::string remoteVersion=updateInfo["version"].asString();

    g_logger<<"[INFO] 本地游戏版本: "<<localVersion<<std::endl;
    g_logger<<"[INFO] 远程游戏版本: "<<remoteVersion<<std::endl;

    if(remoteVersion>localVersion) {
        g_logger<<"[INFO] 发现新版本: "<<remoteVersion<<std::endl;
        DisplayChangelog(updateInfo["changelog"]);
        return true;
    }
    else {
        g_logger<<"[INFO] 当前已是最新版本"<<std::endl;
        return false;
    }
}

Json::Value UpdateChecker::FetchUpdateInfo() {
    g_logger<<"[INFO]正在从服务器获取更新信息: "<<updateUrl<<std::endl;
    g_logger<<"[DEBUG]当前缓存状态: "<<(enableApiCache?"启用API缓存":"禁用API缓存")<<std::endl;

    std::string jsonResponse=httpClient.Get(updateUrl);
    if(jsonResponse.empty()) {
        g_logger<<"[ERROR]错误: 获取更新信息返回为空"<<std::endl;
        return Json::Value();
    }

    g_logger<<"[DEBUG]服务器响应: "<<jsonResponse<<std::endl;

    Json::Value updateInfo;
    if(!ParseUpdateInfo(jsonResponse,updateInfo)) {
        g_logger<<"[ERROR]错误: 解析更新信息失败"<<std::endl;
        return Json::Value();
    }

    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        std::string serverMode=updateInfo["update_mode"].asString();
        g_logger<<"[INFO]服务端指定更新模式: "<<serverMode<<std::endl;

        if(serverMode!="version"&&serverMode!="hash") {
            g_logger<<"[WARN]警告: 服务端指定了无效的更新模式: "<<serverMode<<", 将使用客户端配置"<<std::endl;
        }
    }
    else {
        g_logger<<"[INFO]服务端未指定更新模式，将使用客户端配置"<<std::endl;
    }

    if(!updateInfo.isMember("version")) {
        g_logger<<"[ERROR]错误: 更新信息缺少版本号(version)字段"<<std::endl;
        return Json::Value();
    }

    g_logger<<"[INFO]成功获取更新信息，版本: "<<updateInfo["version"].asString()<<std::endl;
    return updateInfo;
}

bool UpdateChecker::ParseUpdateInfo(const std::string& jsonData,Json::Value& updateInfo) {
    Json::CharReaderBuilder reader;
    std::stringstream ss(jsonData);
    std::string errors;

    if(Json::parseFromStream(reader,ss,&updateInfo,&errors)) {
        return true;
    }
    else {
        g_logger<<"[ERROR]JSON解析错误: "<<errors<<std::endl;
        return false;
    }
}

void UpdateChecker::DisplayChangelog(const Json::Value& changelog) {
    if(changelog.isNull()||!changelog.isArray()) {
        g_logger<<"[INFO]暂无更新日志"<<std::endl;
        return;
    }

    std::cout<<"\n=== 更新内容 ==="<<std::endl;
    for(const auto& change:changelog) {
        std::cout<<"- "<<change.asString()<<std::endl;
    }
    std::cout<<"================\n"<<std::endl;

    g_logger<<"[INFO]更新内容:"<<std::endl;
    for(const auto& change:changelog) {
        g_logger<<"[INFO]  - "<<change.asString()<<std::endl;
    }
}