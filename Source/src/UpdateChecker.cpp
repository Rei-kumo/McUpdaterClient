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

	LOG_INFO("本地游戏版本: {}",localVersion);
    LOG_INFO("远程游戏版本: {}",remoteVersion);

    if(remoteVersion>localVersion) {
        LOG_INFO("发现新版本: {}",remoteVersion);
        DisplayChangelog(updateInfo["changelog"]);
        return true;
    }
    else {
        LOG_INFO("当前已是最新版本");
        return false;
    }
}

Json::Value UpdateChecker::FetchUpdateInfo() {
    LOG_INFO("正在从服务器获取更新信息: {}", updateUrl);
    LOG_DEBUG("当前缓存状态: {}", (enableApiCache ? "启用API缓存" : "禁用API缓存"));

    Json::CharReaderBuilder reader;
    reader.settings_["maxDocumentSize"]=10*1024*1024;
    reader.settings_["maxDepth"]=100;

    std::string jsonResponse=httpClient.Get(updateUrl);
    if(jsonResponse.empty()) {
		LOG_ERROR("获取更新信息返回为空");
        return Json::Value();
    }

    if(jsonResponse.size()>10*1024*1024) {
        LOG_WARN("警告: JSON响应过大 ({}MB)，可能影响性能", (jsonResponse.size()/1024/1024));
    }

    Json::Value updateInfo;
    if(!ParseUpdateInfo(jsonResponse,updateInfo)) {
        LOG_ERROR("错误: 解析更新信息失败");
        return Json::Value();
    }

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
        LOG_ERROR("JSON解析错误: {}", errors);
        return false;
    }
}

void UpdateChecker::DisplayChangelog(const Json::Value& changelog) {
    if(changelog.isNull()||!changelog.isArray()) {
        LOG_INFO("暂无更新日志");
        return;
    }

    std::cout<<"\n=== 更新内容 ==="<<std::endl;
    for(const auto& change:changelog) {
        std::cout<<"- "<<change.asString()<<std::endl;
    }
    std::cout<<"================\n"<<std::endl;

	LOG_INFO("更新内容:");
    for(const auto& change:changelog) {
        LOG_INFO("  - {}", change.asString());
    }
}