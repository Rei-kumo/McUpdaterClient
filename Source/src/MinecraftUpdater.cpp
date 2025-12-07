#include "MinecraftUpdater.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <zip.h>
#include <regex>
#include <cmath>
#include <set>
#include "SelfUpdater.h"
#include <thread>
#include <iomanip>
#include <sstream>
#include <queue>
#include <map>
#include <algorithm>
#include <mutex>
static void ClearConsoleLine() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hConsole=GetStdHandle(STD_OUTPUT_HANDLE);

    if(GetConsoleScreenBufferInfo(hConsole,&csbi)) {
        int columns=csbi.srWindow.Right-csbi.srWindow.Left+1;
        DWORD charsWritten;

        COORD cursorPos=csbi.dwCursorPosition;
        cursorPos.X=0;

        FillConsoleOutputCharacter(hConsole,' ',columns,cursorPos,&charsWritten);
        SetConsoleCursorPosition(hConsole,cursorPos);
    }
}

static std::string FormatBytes(long long bytes) {
    if(bytes<0) bytes=0;

    const char* units[]={"B","KB","MB","GB","TB"};
    int unitIndex=0;
    double size=static_cast<double>(bytes);

    while(size>=1024.0&&unitIndex<4) {
        size/=1024.0;
        unitIndex++;
    }

    std::stringstream ss;

    if(bytes==0) {
        ss<<"0.0 B";
    }
    else if(bytes==1) {
        ss<<"1.0 B";
    }
    else {
        if(unitIndex==0) {
            ss<<std::fixed<<std::setprecision(0)<<size<<" "<<units[unitIndex];
        }
        else if(size<10.0) {
            ss<<std::fixed<<std::setprecision(1)<<size<<" "<<units[unitIndex];
        }
        else {
            ss<<std::fixed<<std::setprecision(0)<<size<<" "<<units[unitIndex];
        }
    }

    return ss.str();
}

void MinecraftUpdater::DownloadProgressCallback(long long downloaded,long long total,void* userdata) {
    MinecraftUpdater* updater=static_cast<MinecraftUpdater*>(userdata);
    if(updater) {
        updater->ShowProgressBar("下载",downloaded,total);
    }
}

void MinecraftUpdater::ShowProgressBar(const std::string& operation,long long current,long long total) {
    static std::mutex progressMutex;
    std::lock_guard<std::mutex> lock(progressMutex);

    static auto lastUpdateTime=std::chrono::steady_clock::now();
    static long long lastCurrent=0;
    auto now=std::chrono::steady_clock::now();
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-lastUpdateTime).count();

    bool shouldUpdate=false;

    if(elapsed>=200) {
        shouldUpdate=true;
    }
    else if(total>0) {
        float lastProgress=static_cast<float>(lastCurrent)/total;
        float currentProgress=static_cast<float>(current)/total;
        if(fabs(currentProgress-lastProgress)>=0.01f) {
            shouldUpdate=true;
        }
    }
    else if(current!=lastCurrent) {
        shouldUpdate=true;
    }

    if(!shouldUpdate&&current<total) {
        return;
    }

    lastUpdateTime=now;
    lastCurrent=current;


    std::cout<<"\r  ";

    const int barWidth=40;

    if(total<=0) {
        static int dotCount=0;
        dotCount=(dotCount+1)%4;
        std::string dots(dotCount,'.');

        std::string currentStr=FormatBytes(current);

        std::string line="进度: "+currentStr+" 已下载"+dots;

        if(line.length()<60) {
            line.append(60-line.length(),' ');
        }

        std::cout<<line;
    }
    else {

        float progress=static_cast<float>(current)/total;
        if(progress<0.0f) progress=0.0f;
        if(progress>1.0f) progress=1.0f;

        int pos=static_cast<int>(barWidth*progress);


        std::string bar="[";
        for(int i=0; i<barWidth; ++i) {
            if(i<pos) bar+="=";
            else if(i==pos) bar+=">";
            else bar+=" ";
        }
        bar+="]";

        std::stringstream ss;
        ss<<"进度: "<<bar<<" "
            <<std::fixed<<std::setprecision(1)<<(progress*100.0)<<"%";

        ss<<" ("<<FormatBytes(current)<<"/"<<FormatBytes(total)<<")";

        std::string line=ss.str();

        if(line.length()<70) {
            line.append(70-line.length(),' ');
        }

        std::cout<<line;
    }

    std::cout.flush();
}
void MinecraftUpdater::ClearProgressLine() {
    const int consoleWidth=80;
    std::cout<<"\r";
    for(int i=0; i<consoleWidth; i++) {
        std::cout<<" ";
    }
    std::cout<<"\r";
    std::cout.flush();
}
int MinecraftUpdater::GetDownloadTimeoutForSize(long long fileSize) {
    int baseTimeout=60;
    int additionalTime=static_cast<int>((fileSize/(10*1024*1024))*30);
    int totalTimeout=baseTimeout+additionalTime;
    return (totalTimeout>600)?600:totalTimeout;
}

MinecraftUpdater::MinecraftUpdater(const std::string& config,const std::string& url,const std::string& gameDir)
    : configManager(config),
    gameDirectory(gameDir),
    httpClient(configManager.ReadApiTimeout()),
    updateChecker(url,httpClient,configManager,configManager.ReadEnableApiCache()),
    selfUpdater(httpClient,configManager),
    hasCachedUpdateInfo(false),
    enableApiCache(configManager.ReadEnableApiCache()){
    g_logger<<"[DEBUG] McUpdater配置: "<<config<<std::endl;
}

bool MinecraftUpdater::CheckForUpdates() {
    g_logger<<"[INFO] 开始检查更新..."<<std::endl;

    if(!enableApiCache) {
        g_logger<<"[INFO] API缓存已禁用，强制重新获取更新信息"<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        g_logger<<"[INFO] 使用缓存的更新信息"<<std::endl;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
        if(!updateInfo.isNull()) {
            cachedUpdateInfo=updateInfo;
            hasCachedUpdateInfo=true;
        }
    }

    if(updateInfo.isNull()) {
        g_logger<<"[ERROR] 错误: 无法获取更新信息"<<std::endl;
        return false;
    }

    bool launcherNeedsUpdate=ProcessLauncherUpdate(updateInfo);

    if(launcherNeedsUpdate) {
        std::string remoteLauncherVersion=updateInfo["launcher"]["version"].asString();
        std::string localLauncherVersion=configManager.ReadLauncherVersion();

        g_logger<<"[INFO] 检测到启动器更新："<<localLauncherVersion<<" -> "<<remoteLauncherVersion<<std::endl;

        std::string currentVersionBackup=localLauncherVersion;

        if(configManager.ReadAutoUpdate()) {
            g_logger<<"[INFO] 自动更新已开启，开始更新启动器..."<<std::endl;
        }
        else {
            std::cout<<"\n[INFO] 发现启动器更新："<<localLauncherVersion<<" -> "<<remoteLauncherVersion<<std::endl;
            std::cout<<"[INFO] 是否立即更新启动器？ (y/n): ";
            char choice;
            std::cin>>choice;

            if(!(choice=='y'||choice=='Y')) {
                g_logger<<"[INFO] 用户取消启动器更新"<<std::endl;
                launcherNeedsUpdate=false;
            }
        }

        if(launcherNeedsUpdate) {
            if(CheckAndApplyLauncherUpdate()) {
                g_logger<<"[INFO] 启动器更新流程已启动，程序即将退出..."<<std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::exit(0);
            }
            else {
                g_logger<<"[ERROR] 启动器更新失败"<<std::endl;
                if(configManager.ReadLauncherVersion()!=currentVersionBackup) {
                    configManager.WriteLauncherVersion(currentVersionBackup);
                    g_logger<<"[INFO] 已恢复启动器版本号为原值："<<currentVersionBackup<<std::endl;
                }
            }
        }
    }

    std::string serverUpdateMode;
    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        serverUpdateMode=updateInfo["update_mode"].asString();
        g_logger<<"[INFO] 服务端强制使用更新模式: "<<serverUpdateMode<<std::endl;
    }
    else {
        serverUpdateMode=configManager.ReadUpdateMode();
        g_logger<<"[INFO] 使用客户端配置的更新模式: "<<serverUpdateMode<<std::endl;
    }

    if(serverUpdateMode=="hash") {
        return CheckForUpdatesByHash();
    }
    else {
        std::string localVersion=configManager.ReadVersion();
        std::string remoteVersion=updateInfo["version"].asString();

        if(remoteVersion>localVersion) {
            g_logger<<"[INFO] 发现新版本: "<<remoteVersion<<std::endl;
            updateChecker.DisplayChangelog(updateInfo["changelog"]);
            return true;
        }
        else {
            g_logger<<"[INFO] 当前已是最新版本"<<std::endl;
            return false;
        }
    }
}

bool MinecraftUpdater::ProcessLauncherUpdate(const Json::Value& updateInfo) {
    if(!updateInfo.isMember("launcher")||!updateInfo["launcher"].isObject()) {
        return false;
    }

    const Json::Value& launcherInfo=updateInfo["launcher"];
    if(!launcherInfo.isMember("version")||!launcherInfo.isMember("url")) {
        return false;
    }

    std::string remoteVersion=launcherInfo["version"].asString();
    std::string localVersion=configManager.ReadLauncherVersion();

    bool needsUpdate=(remoteVersion>localVersion);

    if(needsUpdate) {
        g_logger<<"[INFO] 检测到启动器更新："<<localVersion<<" -> "<<remoteVersion<<std::endl;
    }
    else {
        g_logger<<"[DEBUG] 启动器已是最新版本："<<localVersion<<std::endl;
    }

    return needsUpdate;
}

bool MinecraftUpdater::CheckAndApplyLauncherUpdate() {
    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
    }

    if(updateInfo.isNull()||!updateInfo.isMember("launcher")) {
        g_logger<<"[ERROR] 无法获取启动器更新信息"<<std::endl;
        return false;
    }

    const Json::Value& launcherInfo=updateInfo["launcher"];
    std::string remoteVersion=launcherInfo["version"].asString();
    std::string downloadUrl=launcherInfo["url"].asString();
    std::string expectedHash=launcherInfo["hash"].asString();

    if(downloadUrl.empty()) {
        g_logger<<"[ERROR] 启动器下载URL为空"<<std::endl;
        return false;
    }

    g_logger<<"[INFO] 开始下载新启动器版本："<<remoteVersion<<std::endl;
    g_logger<<"[INFO] 下载URL："<<downloadUrl<<std::endl;

    std::string currentVersion=configManager.ReadLauncherVersion();

    if(!selfUpdater.DownloadNewLauncher(downloadUrl,expectedHash,remoteVersion)) {
        g_logger<<"[ERROR] 下载或验证启动器失败"<<std::endl;
        if(configManager.ReadLauncherVersion()!=currentVersion) {
            configManager.WriteLauncherVersion(currentVersion);
            g_logger<<"[INFO] 已恢复启动器版本号为："<<currentVersion<<std::endl;
        }
        return false;
    }

    if(!configManager.WriteLauncherVersion(remoteVersion)) {
        g_logger<<"[ERROR] 无法更新配置中的启动器版本号，更新中止"<<std::endl;
        return false;
    }
    else {
        g_logger<<"[INFO] 已更新配置中的启动器版本号："<<remoteVersion<<std::endl;
    }

    g_logger<<"[INFO] 启动器下载完成，准备应用更新..."<<std::endl;

    if(selfUpdater.ApplyUpdate()) {
        g_logger<<"[INFO] 启动器更新已启动，程序将退出"<<std::endl;
        return true;
    }
    else {
        g_logger<<"[ERROR] 应用启动器更新失败"<<std::endl;
        configManager.WriteLauncherVersion(currentVersion);
        g_logger<<"[INFO] 已回滚启动器版本号为："<<currentVersion<<std::endl;
        return false;
    }
}

bool MinecraftUpdater::CheckForUpdatesByHash() {
    if(!enableApiCache) {
        g_logger<<"[INFO] API缓存已禁用，强制重新获取更新信息"<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;

    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        g_logger<<"[INFO] 使用缓存的更新信息进行哈希检查"<<std::endl;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
        if(updateInfo.isNull()) {
            g_logger<<"[ERROR] 错误: 无法获取更新信息"<<std::endl;
            return false;
        }
        cachedUpdateInfo=updateInfo;
        hasCachedUpdateInfo=true;
    }

    std::string localVersion=configManager.ReadVersion();
    std::string remoteVersion=updateInfo["version"].asString();

    g_logger<<"[INFO] 本地版本: "<<localVersion<<std::endl;
    g_logger<<"[INFO] 远程版本: "<<remoteVersion<<std::endl;

    bool isConsistent=CheckFileConsistency(updateInfo["files"],updateInfo["directories"]);

    if(remoteVersion>localVersion) {
        std::cout<<"[INFO] 发现新版本: "<<remoteVersion<<std::endl;

        if(ShouldForceHashUpdate(localVersion,remoteVersion)) {
            g_logger<<"[INFO] 检测到跨越多个版本更新"<<std::endl;
        }

        if(!isConsistent) {
            g_logger<<"[INFO] 文件一致性检查失败，需要更新"<<std::endl;
            return true;
        }
        else {
            g_logger<<"[INFO] 版本号更新但文件已是最新，无需更新"<<std::endl;
            return false;
        }
    }
    else if(remoteVersion==localVersion) {
        if(!isConsistent) {
            g_logger<<"[INFO] 版本号相同但文件不一致，需要修复"<<std::endl;
            return true;
        }
        else {
            g_logger<<"[INFO] 当前已是最新版本且文件完整"<<std::endl;
            return false;
        }
    }
    else {
        if(!isConsistent) {
            g_logger<<"[WARN] 本地版本较新但文件不一致，建议修复"<<std::endl;
            std::cout<<"[WARN] 本地版本较新但文件可能损坏，是否修复？(y/n): ";
            char choice;
            std::cin>>choice;
            return (choice=='y'||choice=='Y');
        }
        else {
            g_logger<<"[INFO] 本地版本较新且文件完整"<<std::endl;
            return false;
        }
    }
}

bool MinecraftUpdater::ShouldForceHashUpdate(const std::string& localVersion,const std::string& remoteVersion) {
    if(configManager.ReadSkipMajorVersionCheck()) {
        return false;
    }

    std::regex versionRegex(R"((\d+)\.(\d+)\.(\d+))");
    std::smatch localMatch,remoteMatch;

    if(std::regex_match(localVersion,localMatch,versionRegex)&&
        std::regex_match(remoteVersion,remoteMatch,versionRegex)) {

        int localMajor=std::stoi(localMatch[1]);
        int remoteMajor=std::stoi(remoteMatch[1]);

        if(localMajor!=remoteMajor) {
            return true;
        }

        int localMinor=std::stoi(localMatch[2]);
        int remoteMinor=std::stoi(remoteMatch[2]);

        if(std::abs(remoteMinor-localMinor)>=3) {
            return true;
        }
    }

    return false;
}

bool MinecraftUpdater::CheckFileConsistency(const Json::Value& fileManifest,const Json::Value& directoryManifest) {
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();
    bool allFilesConsistent=true;
    int missingFiles=0;
    int mismatchedFiles=0;
    int totalChecked=0;

    g_logger<<"[DEBUG] 开始文件一致性检查..."<<std::endl;

    for(const auto& fileInfo:fileManifest) {
        std::string relativePath=fileInfo["path"].asString();
        std::string expectedHash=fileInfo["hash"].asString();
        std::string fullPath=gameDirectory+"/"+relativePath;

        totalChecked++;

        if(!std::filesystem::exists(fullPath)) {
            g_logger<<"[INFO] 文件不存在: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            missingFiles++;
            continue;
        }

        std::string actualHash=FileHasher::CalculateFileHash(fullPath,hashAlgorithm);
        if(actualHash.empty()) {
            g_logger<<"[WARN] 无法计算文件哈希: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            mismatchedFiles++;
        }
        else if(actualHash!=expectedHash) {
            g_logger<<"[INFO] 文件哈希不匹配: "<<relativePath<<std::endl;
            g_logger<<"[INFO] 期望: "<<expectedHash<<std::endl;
            g_logger<<"[INFO] 实际: "<<actualHash<<std::endl;
            allFilesConsistent=false;
            mismatchedFiles++;
        }
        else {
            g_logger<<"[DEBUG] 文件一致: "<<relativePath<<std::endl;
        }
    }

    for(const auto& dirInfo:directoryManifest) {
        std::string relativePath=dirInfo["path"].asString();
        std::string fullPath=gameDirectory+"/"+relativePath;

        if(!std::filesystem::exists(fullPath)) {
            g_logger<<"[INFO] 目录不存在: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            missingFiles++;
            continue;
        }

        const Json::Value& contents=dirInfo["contents"];
        for(const auto& contentInfo:contents) {
            std::string fileRelativePath=contentInfo["path"].asString();
            std::string expectedHash=contentInfo["hash"].asString();
            std::string fileFullPath=fullPath+"/"+fileRelativePath;

            totalChecked++;

            if(!std::filesystem::exists(fileFullPath)) {
                g_logger<<"[INFO] 目录内文件不存在: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                missingFiles++;
                continue;
            }

            std::string actualHash=FileHasher::CalculateFileHash(fileFullPath,hashAlgorithm);
            if(actualHash.empty()) {
                g_logger<<"[WARN] 无法计算目录内文件哈希: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                mismatchedFiles++;
            }
            else if(actualHash!=expectedHash) {
                g_logger<<"[INFO] 目录内文件哈希不匹配: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                mismatchedFiles++;
            }
            else {
                g_logger<<"[DEBUG] 目录内文件一致: "<<fileRelativePath<<std::endl;
            }
        }
    }

    g_logger<<"[INFO] 文件一致性检查完成:"<<std::endl;
    g_logger<<"[INFO]   总共检查: "<<totalChecked<<" 个文件"<<std::endl;
    g_logger<<"[INFO]   缺失文件: "<<missingFiles<<" 个"<<std::endl;
    g_logger<<"[INFO]   不匹配文件: "<<mismatchedFiles<<" 个"<<std::endl;
    g_logger<<"[INFO]   文件一致性: "<<(allFilesConsistent?"通过":"失败")<<std::endl;

    return allFilesConsistent;
}

bool MinecraftUpdater::ForceUpdate(bool forceSync) {
    if(!enableApiCache) {
        g_logger<<"[INFO] API缓存已禁用，强制重新获取更新信息"<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }

    Json::Value updateInfo;
    if(hasCachedUpdateInfo) {
        updateInfo=cachedUpdateInfo;
        g_logger<<"[INFO] 使用缓存的更新信息进行更新"<<std::endl;
    }
    else {
        updateInfo=updateChecker.FetchUpdateInfo();
    }

    if(updateInfo.isNull()) {
        g_logger<<"[ERROR] 错误: 无法获取更新信息"<<std::endl;
        return false;
    }

    std::string serverUpdateMode;
    if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
        serverUpdateMode=updateInfo["update_mode"].asString();
        g_logger<<"[INFO] 服务端强制使用更新模式: "<<serverUpdateMode<<std::endl;
    }
    else {
        serverUpdateMode=configManager.ReadUpdateMode();
        g_logger<<"[INFO] 使用客户端配置的更新模式: "<<serverUpdateMode<<std::endl;
    }

    std::string newVersion=updateInfo["version"].asString();
    std::string localVersion=configManager.ReadVersion();

    if(serverUpdateMode=="hash") {
        g_logger<<"[INFO] 开始更新到版本: "<<newVersion<<" (哈希模式)"<<std::endl;
        if(SyncFilesByHash(updateInfo)) {
            g_logger<<"[INFO] 文件同步完成，更新版本信息..."<<std::endl;
            UpdateLocalVersion(newVersion);
            return true;
        }
        else {
            g_logger<<"[ERROR] 错误: 更新过程中出现错误!"<<std::endl;
            return false;
        }
    }
    else {
        g_logger<<"[INFO] 开始更新到版本: "<<newVersion<<" (版本号模式)"<<std::endl;
        bool useIncremental=false;
        if(updateInfo.isMember("incremental_packages")&&
            updateInfo["incremental_packages"].isArray()&&
            updateInfo["incremental_packages"].size()>0) {

            if(ShouldUseIncrementalUpdate(localVersion,newVersion)) {
                useIncremental=true;
                g_logger<<"[INFO] 检测到增量更新包，使用增量更新模式"<<std::endl;

                if(ApplyIncrementalUpdate(updateInfo,localVersion,newVersion)) {
                    g_logger<<"[INFO] 增量更新完成，更新版本信息..."<<std::endl;
                    UpdateLocalVersion(newVersion);
                    return true;
                }
                else {
                    g_logger<<"[WARN] 增量更新失败，回退到全量更新"<<std::endl;
                }
            }
        }

        bool allSuccess=true;

        Json::Value fileList=updateInfo["files"];
        if(fileList.isArray()&&fileList.size()>0) {
            g_logger<<"[INFO] 处理文件更新..."<<std::endl;
            if(!SyncFiles(fileList,forceSync)) {
                g_logger<<"[ERROR] 错误: 文件更新失败"<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
            }
        }

        Json::Value directoryList=updateInfo["directories"];
        if(directoryList.isArray()&&directoryList.size()>0) {
            g_logger<<"[INFO] 处理目录更新..."<<std::endl;
            for(const auto& dirInfo:directoryList) {
                if(!dirInfo.isObject()) continue;

                std::string path=dirInfo["path"].asString();
                std::string url=dirInfo["url"].asString();

                if(path.empty()||url.empty()) {
                    g_logger<<"[ERROR] 错误: 目录信息不完整: path="<<path<<", url="<<url<<std::endl;
                    if(forceSync) return false;
                    allSuccess=false;
                    continue;
                }

                g_logger<<"[INFO] 更新目录: "<<path<<std::endl;
                if(!DownloadAndExtract(url,path)) {
                    g_logger<<"[ERROR] 错误: 目录更新失败: "<<path<<std::endl;
                    if(forceSync) return false;
                    allSuccess=false;
                }
                else {
                    g_logger<<"[INFO] 目录更新成功: "<<path<<std::endl;
                }
            }
        }

        if(allSuccess) {
            g_logger<<"[INFO] 文件同步完成，更新版本信息..."<<std::endl;
            UpdateLocalVersion(newVersion);
            return true;
        }
        else {
            g_logger<<"[ERROR] 错误: 更新过程中出现错误！"<<std::endl;
            return false;
        }
    }
}

bool MinecraftUpdater::SyncFilesByHash(const Json::Value& updateInfo) {
    g_logger<<"[INFO] 开始哈希模式同步..."<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含files字段: "<<updateInfo.isMember("files")<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含directories字段: "<<updateInfo.isMember("directories")<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含file_manifest字段: "<<updateInfo.isMember("file_manifest")<<std::endl;
    g_logger<<"[DEBUG] 更新信息包含directory_manifest字段: "<<updateInfo.isMember("directory_manifest")<<std::endl;

    if(configManager.ReadEnableFileDeletion()) {
        ProcessDeleteList(updateInfo["delete_list"]);
    }

    Json::Value fileManifest=updateInfo["files"];
    Json::Value directoryManifest=updateInfo["directories"];

    g_logger<<"[DEBUG] 文件清单数量: "<<fileManifest.size()<<std::endl;
    g_logger<<"[DEBUG] 目录清单数量: "<<directoryManifest.size()<<std::endl;

    if(!UpdateFilesByHash(fileManifest,directoryManifest)) {
        return false;
    }

    g_logger<<"[INFO] 哈希模式同步完成"<<std::endl;
    return true;
}

bool MinecraftUpdater::ProcessDeleteList(const Json::Value& deleteList) {
    if(!deleteList.isArray()) {
        return true;
    }

    for(const auto& item:deleteList) {
        std::string path=item.asString();
        std::string fullPath=gameDirectory+"/"+path;

        try {
            if(std::filesystem::exists(fullPath)) {
                if(std::filesystem::is_directory(fullPath)) {
                    std::filesystem::remove_all(fullPath);
                    g_logger<<"[INFO] 删除目录: "<<path<<std::endl;
                }
                else {
                    std::filesystem::remove(fullPath);
                    g_logger<<"[INFO] 删除文件: "<<path<<std::endl;
                }
            }
        }
        catch(const std::exception& e) {
            g_logger<<"[WARN] 删除失败: "<<path<<" - "<<e.what()<<std::endl;
        }
    }
    return true;
}

bool MinecraftUpdater::UpdateFilesByHash(const Json::Value& fileManifest,const Json::Value& directoryManifest) {
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();
    bool allSuccess=true;

    int totalFiles=fileManifest.size();
    int currentFile=0;

    std::cout<<std::endl;

    for(const auto& fileInfo:fileManifest) {
        currentFile++;
        std::string relativePath=fileInfo["path"].asString();
        std::string expectedHash=fileInfo["hash"].asString();
        std::string url=fileInfo["url"].asString();

        std::cout<<"["<<currentFile<<"/"<<totalFiles<<"] 下载: "<<relativePath<<std::endl;
        std::string fileInfoStr="["+std::to_string(currentFile)+"/"+
            std::to_string(totalFiles)+"] 下载: "+relativePath;
        int padding=80-(int)fileInfoStr.length();
        if(padding>0) {
            std::cout<<std::string(padding,' ');
        }
        std::cout<<std::endl;
        std::cout.flush();

        std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
        std::filesystem::path fullPath=gameDirPath/relativePath;
        std::string fullPathStr=fullPath.string();

        std::filesystem::path parentDir=fullPath.parent_path();
        EnsureDirectoryExists(parentDir.string());

        bool canWrite=false;
        try {
            std::filesystem::path testFile=parentDir/"write_test.tmp";
            std::ofstream testStream(testFile);
            if(testStream) {
                testStream<<"test";
                testStream.close();
                std::filesystem::remove(testFile);
                canWrite=true;
                g_logger<<"[DEBUG] 目录写入权限检查通过: "<<parentDir.string()<<std::endl;
            }
        }
        catch(const std::exception& e) {
            g_logger<<"[DEBUG] 目录写入权限检查失败: "<<e.what()<<std::endl;
        }

        if(!canWrite) {
            g_logger<<"[ERROR] 错误: 目录没有写入权限: "<<parentDir.string()<<std::endl;
            std::cout<<"  [权限错误]"<<std::endl;
            allSuccess=false;
            continue;
        }

        if(std::filesystem::exists(fullPath)) {
            std::string actualHash=FileHasher::CalculateFileHash(fullPathStr,hashAlgorithm);
            if(!actualHash.empty()&&actualHash==expectedHash) {
                g_logger<<"[INFO] 文件已是最新: "<<relativePath<<std::endl;
                std::cout<<"  [已是最新]"<<std::endl;
                continue;
            }
        }

        if(fileInfo.isMember("size")) {
            long long fileSize=fileInfo["size"].asInt64();
            int timeout=GetDownloadTimeoutForSize(fileSize);
            httpClient.SetDownloadTimeout(timeout);
            g_logger<<"[DEBUG] 设置文件下载超时: "<<timeout<<"秒 (大小: "<<FormatBytes(fileSize)<<")"<<std::endl;
        }

        std::string progressMessage="进度";

        ShowProgressBar(progressMessage,0,1);

        bool downloadSuccess=false;
        long long fileSize=0;

        if(fileInfo.isMember("size")) {
            fileSize=fileInfo["size"].asInt64();
        }

        auto progressCallback=[this,progressMessage,fileSize](long long downloaded,long long total,void* userdata) {
            if(total<=0&&fileSize>0) {
                ShowProgressBar(progressMessage,downloaded,fileSize);
            }
            else {
                ShowProgressBar(progressMessage,downloaded,total);
            }
            };

        if(fileSize>0) {
            int timeout=GetDownloadTimeoutForSize(fileSize);
            httpClient.SetDownloadTimeout(timeout);
        }

        downloadSuccess=httpClient.DownloadFileWithProgress(
            url,
            fullPathStr,
            progressCallback,
            nullptr
        );

        ClearProgressLine();

        httpClient.SetDownloadTimeout(0);

        if(!downloadSuccess) {
            std::cout<<"  [失败]"<<std::endl;
            allSuccess=false;
            continue;
        }

        std::error_code ec;
        auto actualSize=std::filesystem::file_size(fullPathStr,ec);
        std::string sizeStr=ec?"未知大小":FormatBytes(actualSize);

        if(!expectedHash.empty()) {
            std::string downloadedHash=FileHasher::CalculateFileHash(fullPathStr,hashAlgorithm);
            if(downloadedHash!=expectedHash) {
                std::cout<<"  [完成，大小: "<<sizeStr<<"，但哈希不匹配]"<<std::endl;
            }
            else {
                std::cout<<"  [完成，大小: "<<sizeStr<<"，已验证]"<<std::endl;
            }
        }
        else {
            std::cout<<"  [完成，大小: "<<sizeStr<<"]"<<std::endl;
        }
    }

    return allSuccess;
}

bool MinecraftUpdater::SyncDirectoryByHash(const Json::Value& dirInfo) {
    std::string relativePath=dirInfo["path"].asString();
    std::string url=dirInfo["url"].asString();
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();

    g_logger<<"[INFO] 同步目录: "<<relativePath<<std::endl;

    std::vector<unsigned char> zipData;
    if(!httpClient.DownloadToMemory(url,zipData)) {
        g_logger<<"[ERROR] 目录下载失败: "<<relativePath<<std::endl;
        return false;
    }

    std::string tempDir=std::filesystem::temp_directory_path().string()+"/mc_update_temp";
    EnsureDirectoryExists(tempDir);

    if(!ExtractZip(zipData,tempDir)) {
        g_logger<<"[ERROR] 解压失败: "<<relativePath<<std::endl;
        return false;
    }

    std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
    std::string targetDir=(gameDirPath/relativePath).string();
    EnsureDirectoryExists(targetDir);

    const Json::Value& contents=dirInfo["contents"];
    bool dirSuccess=true;

    for(const auto& contentInfo:contents) {
        std::string fileRelativePath=contentInfo["path"].asString();
        std::string expectedHash=contentInfo["hash"].asString();

        std::string tempFilePath=tempDir+"/"+fileRelativePath;
        std::string targetFilePath=targetDir+"/"+fileRelativePath;

        if(!std::filesystem::exists(tempFilePath)) {
            g_logger<<"[WARN] 解压文件中不存在: "<<fileRelativePath<<std::endl;
            dirSuccess=false;
            continue;
        }

        if(!expectedHash.empty()) {
            std::string actualHash=FileHasher::CalculateFileHash(tempFilePath,hashAlgorithm);
            if(actualHash!=expectedHash) {
                g_logger<<"[WARN] 解压文件哈希验证失败: "<<fileRelativePath<<std::endl;
                g_logger<<"[WARN] 期望: "<<expectedHash<<std::endl;
                g_logger<<"[WARN] 实际: "<<actualHash<<std::endl;
            }
            else {
                g_logger<<"[DEBUG] 解压文件哈希验证成功: "<<fileRelativePath<<std::endl;
            }
        }

        EnsureDirectoryExists(std::filesystem::path(targetFilePath).parent_path().string());

        try {
            std::filesystem::copy(tempFilePath,targetFilePath,
                std::filesystem::copy_options::overwrite_existing);
            g_logger<<"[INFO] 更新文件: "<<fileRelativePath<<std::endl;
        }
        catch(const std::exception& e) {
            g_logger<<"[ERROR] 文件复制失败: "<<fileRelativePath<<" - "<<e.what()<<std::endl;
            dirSuccess=false;
        }
    }

    if(configManager.ReadEnableFileDeletion()) {
        CleanupOrphanedFiles(targetDir,contents);
    }

    try {
        std::filesystem::remove_all(tempDir);
    }
    catch(const std::exception& e) {
        g_logger<<"[WARN] 清理临时目录失败: "<<e.what()<<std::endl;
    }

    return dirSuccess;
}

void MinecraftUpdater::CleanupOrphanedFiles(const std::string& directoryPath,const Json::Value& expectedContents) {
    if(!expectedContents.isArray()) {
        g_logger<<"[WARN] 警告: 预期内容不是数组，跳过清理孤儿文件"<<std::endl;
        return;
    }
    std::set<std::string> expectedFiles;
    for(const auto& contentInfo:expectedContents) {
        std::string path=contentInfo["path"].asString();
        std::replace(path.begin(),path.end(),'\\','/');
        expectedFiles.insert(path);
    }

    g_logger<<"[DEBUG] 期望文件列表:"<<std::endl;
    for(const auto& file:expectedFiles) {
        g_logger<<"[DEBUG]   - "<<file<<std::endl;
    }

    try {
        for(const auto& entry:std::filesystem::recursive_directory_iterator(directoryPath)) {
            if(entry.is_regular_file()) {
                std::string relativePath=std::filesystem::relative(entry.path(),directoryPath).string();
                std::replace(relativePath.begin(),relativePath.end(),'\\','/');

                g_logger<<"[DEBUG] 检查文件: "<<relativePath<<std::endl;

                if(expectedFiles.find(relativePath)==expectedFiles.end()) {
                    try {
                        std::filesystem::remove(entry.path());
                        g_logger<<"[INFO] 删除孤儿文件: "<<relativePath<<std::endl;
                    }
                    catch(const std::exception& e) {
                        g_logger<<"[ERROR] 删除孤儿文件失败: "<<relativePath<<" - "<<e.what()<<std::endl;
                    }
                }
                else {
                    g_logger<<"[DEBUG] 文件在期望列表中，保留: "<<relativePath<<std::endl;
                }
            }
        }
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 遍历目录失败: "<<directoryPath<<" - "<<e.what()<<std::endl;
    }
}

void MinecraftUpdater::EnsureDirectoryExists(const std::string& path) {
    try {
        if(path.empty()) {
            g_logger<<"[WARN] 警告: 路径为空"<<std::endl;
            return;
        }

        std::filesystem::path dirPath(path);
        dirPath=std::filesystem::absolute(dirPath);

        if(!std::filesystem::exists(dirPath)) {
            g_logger<<"[INFO] 创建目录: "<<dirPath.string()<<std::endl;
            bool created=std::filesystem::create_directories(dirPath);

            if(created) {
                g_logger<<"[INFO] 目录创建成功: "<<dirPath.string()<<std::endl;
            }
            else {
                g_logger<<"[WARN] 目录可能已存在: "<<dirPath.string()<<std::endl;
            }
            if(!std::filesystem::exists(dirPath)) {
                g_logger<<"[ERROR] 错误: 目录创建后仍然不存在: "<<dirPath.string()<<std::endl;
            }
            else if(!std::filesystem::is_directory(dirPath)) {
                g_logger<<"[ERROR] 错误: 路径存在但不是目录: "<<dirPath.string()<<std::endl;
            }
        }
        else {
            if(!std::filesystem::is_directory(dirPath)) {
                g_logger<<"[ERROR] 错误: 路径存在但不是目录: "<<dirPath.string()<<std::endl;
            }
        }
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 错误: 创建目录失败: "<<path<<" - "<<e.what()<<std::endl;
        g_logger<<"[ERROR] 详细错误: "<<e.what()<<std::endl;
    }
}

bool MinecraftUpdater::BackupFile(const std::string& filePath) {
    if(!std::filesystem::exists(filePath)) {
        return true;
    }

    std::string backupPath=filePath+".backup";

    try {
        if(std::filesystem::exists(backupPath)) {
            std::filesystem::remove_all(backupPath);
        }
        if(std::filesystem::is_directory(filePath)) {
            std::filesystem::copy(filePath,backupPath,
                std::filesystem::copy_options::recursive|
                std::filesystem::copy_options::overwrite_existing);
            g_logger<<"[INFO] 备份目录: "<<filePath<<" -> "<<backupPath<<std::endl;
        }
        else {
            std::filesystem::copy_file(filePath,backupPath,
                std::filesystem::copy_options::overwrite_existing);
            g_logger<<"[INFO] 备份文件: "<<filePath<<" -> "<<backupPath<<std::endl;
        }
        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[WARN] 警告: 备份失败: "<<filePath<<" - "<<e.what()<<std::endl;
        g_logger<<"[WARN] 警告: 备份失败，但继续更新过程..."<<std::endl;
        return false;
    }
}

bool MinecraftUpdater::ExtractZip(const std::vector<unsigned char>& zipData,const std::string& extractPath) {
    EnsureDirectoryExists(extractPath);

    std::string tempDir=std::filesystem::temp_directory_path().string();
    std::string tempZip=tempDir+"/minecraft_update_temp.zip";

    g_logger<<"[INFO] 创建临时文件: "<<tempZip<<std::endl;

    std::ofstream file(tempZip,std::ios::binary);
    if(!file) {
        g_logger<<"[ERROR] 错误: 无法创建临时文件: "<<tempZip<<std::endl;
        return false;
    }
    file.write(reinterpret_cast<const char*>(zipData.data()),zipData.size());
    file.close();

    int err=0;
    zip_t* zip=zip_open(tempZip.c_str(),0,&err);
    if(!zip) {
        g_logger<<"[ERROR] 错误: 无法打开ZIP文件: "<<tempZip<<"，错误码: "<<err<<std::endl;
        std::filesystem::remove(tempZip);
        return false;
    }

    zip_int64_t numEntries=zip_get_num_entries(zip,0);
    g_logger<<"[INFO] 开始解压 "<<numEntries<<" 个文件到: "<<extractPath<<std::endl;

    for(zip_int64_t i=0; i<numEntries; i++) {
        const char* name=zip_get_name(zip,i,0);
        if(!name) continue;

        std::string fullPath=extractPath+"/"+name;

        if(name[strlen(name)-1]=='/') {
            EnsureDirectoryExists(fullPath);
            continue;
        }

        zip_file_t* zfile=zip_fopen_index(zip,i,0);
        if(!zfile) {
            g_logger<<"[ERROR] 错误: 无法解压文件: "<<name<<std::endl;
            continue;
        }

        std::filesystem::path filePath(fullPath);
        EnsureDirectoryExists(filePath.parent_path().string());

        std::ofstream outFile(fullPath,std::ios::binary);
        if(outFile) {
            char buffer[8192];
            zip_int64_t bytesRead;
            while((bytesRead=zip_fread(zfile,buffer,sizeof(buffer)))>0) {
                outFile.write(buffer,bytesRead);
            }
            outFile.close();
            g_logger<<"[INFO] 解压文件: "<<name<<std::endl;
        }
        else {
            g_logger<<"[ERROR] 错误: 无法创建文件: "<<fullPath<<std::endl;
        }

        zip_fclose(zfile);
    }

    zip_close(zip);
    std::filesystem::remove(tempZip);
    g_logger<<"[INFO] 解压完成: "<<extractPath<<std::endl;
    return true;
}

bool MinecraftUpdater::DownloadAndExtract(const std::string& url,const std::string& relativePath) {
    std::vector<unsigned char> zipData;

    g_logger<<"[INFO] 下载目录: "<<url<<" -> "<<relativePath<<std::endl;

    std::string progressMessage="下载目录 "+relativePath;
    ShowProgressBar(progressMessage,0,1);

    if(!httpClient.DownloadToMemoryWithProgress(url,zipData,
        [this,progressMessage](long long downloaded,long long total,void* userdata){
            ShowProgressBar(progressMessage,downloaded,total);
        },nullptr)) {

        ClearProgressLine();
        g_logger<<"[ERROR] 错误: 下载失败: "<<url<<std::endl;
        return false;
    }

    ClearProgressLine();

    if(zipData.empty()) {
        g_logger<<"[ERROR] 错误: 下载的文件为空: "<<url<<std::endl;
        return false;
    }

    std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
    std::string fullPath=(gameDirPath/relativePath).string();

    g_logger<<"[DEBUG] 完整目标路径: "<<fullPath<<std::endl;

    std::filesystem::path pathObj(fullPath);
    if(pathObj.has_parent_path()) {
        EnsureDirectoryExists(pathObj.parent_path().string());
    }

    if(std::filesystem::exists(fullPath)) {
        g_logger<<"[INFO] 备份原有目录: "<<fullPath<<std::endl;
        if(!BackupFile(fullPath)) {
            g_logger<<"[WARN] 警告: 目录备份失败，但继续更新..."<<std::endl;
        }
    }

    return ExtractZip(zipData,fullPath);
}

bool MinecraftUpdater::SyncFiles(const Json::Value& fileList,bool forceSync) {
    if(!fileList.isArray()) {
        g_logger<<"[ERROR] 错误: 文件列表格式错误"<<std::endl;
        return false;
    }

    EnsureDirectoryExists(gameDirectory);

    bool allSuccess=true;

    for(const auto& fileInfo:fileList) {
        if(!fileInfo.isObject()) continue;

        std::string path=fileInfo["path"].asString();
        std::string url=fileInfo["url"].asString();
        std::string type=fileInfo.isMember("type")?fileInfo["type"].asString():"file";

        if(path.empty()||url.empty()) {
            g_logger<<"[ERROR] 错误: 文件信息不完整: path="<<path<<", url="<<url<<std::endl;
            if(forceSync) return false;
            allSuccess=false;
            continue;
        }

        std::string fullPath=gameDirectory+"/"+path;

        if(type=="directory") {
            g_logger<<"[INFO] 更新目录: "<<path<<std::endl;
            if(!DownloadAndExtract(url,path)) {
                g_logger<<"[ERROR] 错误: 目录更新失败: "<<path<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
            }
        }
        else {
            std::string outputDir=std::filesystem::path(fullPath).parent_path().string();
            EnsureDirectoryExists(outputDir);

            if(std::filesystem::exists(fullPath)) {
                g_logger<<"[INFO] 备份原有文件: "<<fullPath<<std::endl;
                if(!BackupFile(fullPath)) {
                    g_logger<<"[WARN] 警告: 文件备份失败，但继续更新..."<<std::endl;
                }
            }

            g_logger<<"[INFO] 下载文件: "<<url<<" -> "<<fullPath<<std::endl;

            long long expectedSize=0;
            if(fileInfo.isMember("size")) {
                expectedSize=fileInfo["size"].asInt64();
                g_logger<<"[DEBUG] 期望文件大小: "<<::FormatBytes(expectedSize)<<std::endl;
            }

            std::string progressMessage="下载 "+path;
            if(expectedSize>0) {
                ShowProgressBar(progressMessage,0,expectedSize);
            }
            else {
                ShowProgressBar(progressMessage,0,1);
            }

            if(!httpClient.DownloadFileWithProgress(url,fullPath,
                [this,progressMessage,expectedSize](long long downloaded,long long total,void* userdata) {
                    if(total<=0&&expectedSize>0) {
                        total=expectedSize;
                    }
                    ShowProgressBar(progressMessage,downloaded,total);
                },nullptr)) {

                ClearProgressLine();
                g_logger<<"[ERROR] 错误: 文件下载失败: "<<path<<std::endl;
                if(forceSync) return false;
                allSuccess=false;
            }
            else {
                ClearProgressLine();
                g_logger<<"[INFO] 文件下载成功: "<<path<<std::endl;
            }
        }
    }

    return allSuccess;
}

void MinecraftUpdater::UpdateLocalVersion(const std::string& newVersion) {
    if(configManager.WriteVersion(newVersion)) {
        g_logger<<"[INFO] 版本信息已更新为: "<<newVersion<<std::endl;
        hasCachedUpdateInfo=false;
        cachedUpdateInfo=Json::Value();
    }
    else {
        g_logger<<"[ERROR] 错误: 更新版本信息失败"<<std::endl;
    }
}

bool MinecraftUpdater::ShouldUseIncrementalUpdate(const std::string& localVersion,const std::string& remoteVersion) {
    if(localVersion>=remoteVersion) {
        return false;
    }

    std::regex versionRegex(R"((\d+)\.(\d+)\.(\d+))");
    std::smatch localMatch,remoteMatch;

    if(!std::regex_match(localVersion,localMatch,versionRegex)||
        !std::regex_match(remoteVersion,remoteMatch,versionRegex)) {
        g_logger<<"[WARN] 版本号格式不正确，跳过增量更新"<<std::endl;
        return false;
    }

    int localMajor=std::stoi(localMatch[1]);
    int remoteMajor=std::stoi(remoteMatch[1]);

    if(localMajor!=remoteMajor) {
        g_logger<<"[INFO] 检测到主要版本变更 ("<<localVersion<<" -> "<<remoteVersion<<")，建议使用全量更新"<<std::endl;
    }

    return true;
}

bool MinecraftUpdater::ApplyIncrementalUpdate(const Json::Value& updateInfo,const std::string& localVersion,const std::string& remoteVersion) {
    const Json::Value& packages=updateInfo["incremental_packages"];
    std::string hashAlgorithm=configManager.ReadHashAlgorithm();

    if(!packages.isArray()||packages.size()==0) {
        g_logger<<"[INFO] 没有可用的增量更新包"<<std::endl;
        return false;
    }

    g_logger<<"[INFO] 开始处理增量更新: "<<localVersion<<" -> "<<remoteVersion<<std::endl;

    std::vector<std::string> packagePaths=GetUpdatePackagePath(packages,localVersion,remoteVersion);

    if(packagePaths.empty()) {
        g_logger<<"[INFO] 没有找到合适的增量更新包路径"<<std::endl;
        return false;
    }

    g_logger<<"[INFO] 需要应用 "<<packagePaths.size()<<" 个更新包"<<std::endl;

    for(size_t i=0; i<packagePaths.size(); i++) {
        const std::string& packagePath=packagePaths[i];
        g_logger<<"[INFO] ("<<(i+1)<<"/"<<packagePaths.size()<<") 处理更新包: "<<packagePath<<std::endl;

        std::string expectedHash;
        long long expectedSize=0;
        for(const auto& package:packages) {
            if(package["archive"].asString()==packagePath) {
                if(package.isMember("hash")) {
                    expectedHash=package["hash"].asString();
                }
                if(package.isMember("size")) {
                    expectedSize=package["size"].asInt64();
                }
                break;
            }
        }
        if(expectedSize>0) {
            int downloadTimeout=GetDownloadTimeoutForSize(expectedSize);
            httpClient.SetDownloadTimeout(downloadTimeout);
            g_logger<<"[DEBUG] 设置下载超时: "<<downloadTimeout<<"秒 (文件大小: "<<FormatBytes(expectedSize)<<")"<<std::endl;
        }
        else {
            httpClient.SetDownloadTimeout(300);
        }
        std::string tempZip=std::filesystem::temp_directory_path().string()+"/mc_update_"+std::to_string(i)+".zip";

        g_logger<<"[INFO] 开始下载更新包..."<<std::endl;

        if(!httpClient.DownloadFileWithProgress(packagePath,tempZip,
            DownloadProgressCallback,this)) {
            g_logger<<std::endl<<"[ERROR] 下载更新包失败: "<<packagePath<<std::endl;

            if(std::filesystem::exists(tempZip)) {
                std::filesystem::remove(tempZip);
            }

            httpClient.SetDownloadTimeout(0);
            return false;
        }
        ClearProgressLine();
        g_logger<<"[INFO] 下载完成"<<std::endl;
        if(expectedSize>0) {
            std::error_code ec;
            auto actualSize=std::filesystem::file_size(tempZip,ec);
            if(!ec&&actualSize!=expectedSize) {
                g_logger<<"[WARN] 文件大小不匹配: 期望 "<<FormatBytes(expectedSize)<<", 实际 "<<FormatBytes(actualSize)<<std::endl;
            }
        }
        if(!expectedHash.empty()) {
            g_logger<<"[INFO] 验证文件哈希..."<<std::endl;

            std::string actualHash=FileHasher::CalculateFileHash(tempZip,hashAlgorithm);
            if(actualHash!=expectedHash) {
                g_logger<<"[ERROR] 更新包哈希验证失败: "<<packagePath<<std::endl;
                g_logger<<"[ERROR] 期望: "<<expectedHash<<std::endl;
                g_logger<<"[ERROR] 实际: "<<actualHash<<std::endl;

                std::filesystem::remove(tempZip);
                httpClient.SetDownloadTimeout(0);
                return false;
            }
            else {
                g_logger<<"[INFO] 更新包哈希验证通过"<<std::endl;
            }
        }

        std::ifstream file(tempZip,std::ios::binary);
        std::vector<unsigned char> zipData((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        file.close();
        std::filesystem::remove(tempZip);
        httpClient.SetDownloadTimeout(0);
        std::string tempDir=std::filesystem::temp_directory_path().string()+"/mc_update_temp_"+std::to_string(i);
        EnsureDirectoryExists(tempDir);

        g_logger<<"[INFO] 解压更新包..."<<std::endl;
        if(!ExtractZip(zipData,tempDir)) {
            g_logger<<"[ERROR] 解压更新包失败: "<<packagePath<<std::endl;
            return false;
        }
        bool manifestFound=false;
        std::string manifestPath;
        std::vector<std::string> possibleManifestNames={
            "update_manifest.txt",
            "changelog.txt",
            "file_list.txt",
            "manifest.txt"
        };

        for(const auto& name:possibleManifestNames) {
            std::string testPath=tempDir+"/"+name;
            if(std::filesystem::exists(testPath)) {
                manifestPath=testPath;
                manifestFound=true;
                g_logger<<"[INFO] 找到更新清单文件: "<<name<<std::endl;
                break;
            }
        }

        if(manifestFound) {
            if(!ApplyUpdateFromManifest(manifestPath,tempDir)) {
                g_logger<<"[ERROR] 应用更新清单失败"<<std::endl;
                return false;
            }
        }
        else {
            g_logger<<"[WARN] 更新包中没有找到清单文件，将更新所有文件"<<std::endl;
            if(!ApplyAllFilesFromUpdate(tempDir)) {
                g_logger<<"[ERROR] 更新所有文件失败"<<std::endl;
                return false;
            }
        }
        try {
            std::filesystem::remove_all(tempDir);
        }
        catch(const std::exception& e) {
            g_logger<<"[WARN] 清理临时目录失败: "<<e.what()<<std::endl;
        }

        g_logger<<"[INFO] 更新包 ("<<(i+1)<<"/"<<packagePaths.size()<<") 处理完成"<<std::endl;
    }

    g_logger<<"[INFO] 所有增量更新包应用完成"<<std::endl;
    return true;
}

std::vector<std::string> MinecraftUpdater::GetUpdatePackagePath(const Json::Value& packages,const std::string& fromVersion,const std::string& toVersion) {
    std::vector<std::string> result;

    g_logger<<"[INFO] 寻找更新路径: "<<fromVersion<<" -> "<<toVersion<<std::endl;

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")||!package.isMember("archive")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from==fromVersion&&to==toVersion) {
            g_logger<<"[INFO] 找到直接合并包: "<<archive<<" ("<<from<<" -> "<<to<<")"<<std::endl;
            return {archive};
        }
    }

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from=="0.0.0"&&to==toVersion) {
            g_logger<<"[INFO] 找到全量更新包: "<<archive<<" (0.0.0 -> "<<to<<")"<<std::endl;
            return {archive};
        }
    }

    std::map<std::string,std::vector<std::string>> graph;
    std::map<std::string,std::string> archiveMap;

    for(const auto& package:packages) {
        if(!package.isMember("from_version")||!package.isMember("to_version")||!package.isMember("archive")) {
            continue;
        }

        std::string from=package["from_version"].asString();
        std::string to=package["to_version"].asString();
        std::string archive=package["archive"].asString();

        if(from=="0.0.1") {
            continue;
        }

        graph[from].push_back(to);
        std::string edgeKey=from+"->"+to;
        archiveMap[edgeKey]=archive;
    }

    std::queue<std::pair<std::string,std::vector<std::string>>> q;
    std::set<std::string> visited;

    q.push({fromVersion,{}});
    visited.insert(fromVersion);

    while(!q.empty()) {
        auto [current,path]=q.front();
        q.pop();

        if(current==toVersion) {
            std::vector<std::string> archivePath;
            for(size_t i=0; i<path.size(); i++) {
                std::string from=(i==0)?fromVersion:path[i-1];
                std::string to=path[i];
                std::string edgeKey=from+"->"+to;

                if(archiveMap.find(edgeKey)!=archiveMap.end()) {
                    archivePath.push_back(archiveMap[edgeKey]);
                }
            }

            g_logger<<"[INFO] 找到增量更新路径，包含 "<<archivePath.size()<<" 个包"<<std::endl;
            return archivePath;
        }

        if(graph.find(current)!=graph.end()) {
            for(const auto& next:graph[current]) {
                if(visited.find(next)==visited.end()) {
                    visited.insert(next);
                    std::vector<std::string> newPath=path;
                    newPath.push_back(next);
                    q.push({next,newPath});
                }
            }
        }
    }

    g_logger<<"[WARN] 无法找到增量更新路径: "<<fromVersion<<" -> "<<toVersion<<std::endl;
    return {};
}
bool MinecraftUpdater::ApplyUpdateFromManifest(const std::string& manifestPath,const std::string& tempDir) {
    std::ifstream manifestFile(manifestPath);
    if(!manifestFile.is_open()) {
        g_logger<<"[ERROR] 无法打开清单文件: "<<manifestPath<<std::endl;
        return false;
    }

    std::string line;
    int operationCount=0;

    while(std::getline(manifestFile,line)) {
        if(line.empty()||line[0]=='#') continue;

        size_t colonPos=line.find(':');
        if(colonPos!=std::string::npos) {
            std::string operation=line.substr(0,colonPos);
            std::string filePath=line.substr(colonPos+1);

            operation.erase(0,operation.find_first_not_of(" \t"));
            operation.erase(operation.find_last_not_of(" \t")+1);
            filePath.erase(0,filePath.find_first_not_of(" \t"));
            filePath.erase(filePath.find_last_not_of(" \t")+1);

            if(operation=="A"||operation=="M") {
                std::string sourceFile=tempDir+"/"+filePath;
                std::string targetFile=gameDirectory+"/"+filePath;

                EnsureDirectoryExists(std::filesystem::path(targetFile).parent_path().string());

                if(std::filesystem::exists(sourceFile)) {
                    std::filesystem::copy(sourceFile,targetFile,
                        std::filesystem::copy_options::overwrite_existing);
                    g_logger<<"[INFO] "<<(operation=="A"?"新增":"修改")<<"文件: "<<filePath<<std::endl;
                    operationCount++;
                }
                else {
                    g_logger<<"[WARN] 源文件不存在: "<<sourceFile<<std::endl;
                }
            }
            else if(operation=="D") {
                std::string targetFile=gameDirectory+"/"+filePath;
                if(std::filesystem::exists(targetFile)) {
                    std::filesystem::remove(targetFile);
                    g_logger<<"[INFO] 删除文件: "<<filePath<<std::endl;
                    operationCount++;
                }
            }
            else {
                g_logger<<"[WARN] 未知的操作类型: "<<operation<<" (行: "<<line<<")"<<std::endl;
            }
        }
    }

    manifestFile.close();
    g_logger<<"[INFO] 从清单文件执行了 "<<operationCount<<" 个操作"<<std::endl;
    return true;
}

bool MinecraftUpdater::ApplyAllFilesFromUpdate(const std::string& tempDir) {
    int fileCount=0;

    for(const auto& entry:std::filesystem::recursive_directory_iterator(tempDir)) {
        if(entry.is_regular_file()) {
            std::string relativePath=std::filesystem::relative(entry.path(),tempDir).string();
            std::string targetPath=gameDirectory+"/"+relativePath;

            EnsureDirectoryExists(std::filesystem::path(targetPath).parent_path().string());

            try {
                if(std::filesystem::exists(targetPath)) {
                    BackupFile(targetPath);
                }

                std::filesystem::copy(entry.path(),targetPath,
                    std::filesystem::copy_options::overwrite_existing);

                g_logger<<"[INFO] 更新文件: "<<relativePath<<std::endl;
                fileCount++;
            }
            catch(const std::exception& e) {
                g_logger<<"[ERROR] 更新文件失败: "<<relativePath<<" - "<<e.what()<<std::endl;
                return false;
            }
        }
    }

    g_logger<<"[INFO] 更新了 "<<fileCount<<" 个文件"<<std::endl;
    return true;
}