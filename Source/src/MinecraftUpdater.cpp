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
#include <memory>
#include "FileHasher.h"
#include <fcntl.h>
#include <io.h>
#include <windows.h>
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
    enableApiCache(configManager.ReadEnableApiCache()) {

    g_logger<<"[DEBUG] McUpdaterClient配置: "<<config<<std::endl;
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
    const int BATCH_SIZE=50;
    int processedInBatch=0;

    auto processBatch=[&]() {
        if(processedInBatch>=BATCH_SIZE) {
            HANDLE heap=GetProcessHeap();
            if(heap!=NULL) {
                HeapCompact(heap,0);
            }

            std::cout<<"\r检查进度: "<<totalChecked<<" 文件 ("<<missingFiles<<" 缺失, "<<mismatchedFiles<<" 不匹配)      ";
            std::cout.flush();

            processedInBatch=0;
        }
        };

    for(const auto& fileInfo:fileManifest) {
        processBatch();

        std::string relativePath=fileInfo["path"].asString();
        std::string expectedHash=fileInfo["hash"].asString();
        std::string fullPath=gameDirectory+"/"+relativePath;

        totalChecked++;
        processedInBatch++;

        if(!std::filesystem::exists(fullPath)) {
            g_logger<<"[DEBUG] 文件不存在: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            missingFiles++;
            continue;
        }

        std::string actualHash=FileHasher::CalculateFileHash(fullPath,hashAlgorithm);
        if(actualHash.empty()) {
            g_logger<<"[DEBUG] 无法计算文件哈希: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            mismatchedFiles++;
        }
        else if(actualHash!=expectedHash) {
            g_logger<<"[DEBUG] 文件哈希不匹配: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            mismatchedFiles++;
        }
    }

    for(const auto& dirInfo:directoryManifest) {
        std::string relativePath=dirInfo["path"].asString();
        std::string fullPath=gameDirectory+"/"+relativePath;

        if(!std::filesystem::exists(fullPath)) {
            g_logger<<"[DEBUG] 目录不存在: "<<relativePath<<std::endl;
            allFilesConsistent=false;
            missingFiles++;
            continue;
        }

        const Json::Value& contents=dirInfo["contents"];
        for(const auto& contentInfo:contents) {
            processBatch();

            std::string fileRelativePath=contentInfo["path"].asString();
            std::string expectedHash=contentInfo["hash"].asString();
            std::string fileFullPath=fullPath+"/"+fileRelativePath;

            totalChecked++;
            processedInBatch++;

            if(!std::filesystem::exists(fileFullPath)) {
                g_logger<<"[DEBUG] 目录内文件不存在: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                missingFiles++;
                continue;
            }

            std::string actualHash=FileHasher::CalculateFileHash(fileFullPath,hashAlgorithm);
            if(actualHash.empty()) {
                g_logger<<"[DEBUG] 无法计算目录内文件哈希: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                mismatchedFiles++;
            }
            else if(actualHash!=expectedHash) {
                g_logger<<"[DEBUG] 目录内文件哈希不匹配: "<<fileRelativePath<<std::endl;
                allFilesConsistent=false;
                mismatchedFiles++;
            }
        }
    }

    std::cout<<"\r检查完成: "<<totalChecked<<" 文件 ("<<missingFiles<<" 缺失, "<<mismatchedFiles<<" 不匹配)      "<<std::endl;

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
    std::string timestamp=std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::string tempBackupPath=backupPath+"_"+timestamp;

    try {
        if(std::filesystem::is_directory(filePath)) {
            std::filesystem::copy(filePath,tempBackupPath,
                std::filesystem::copy_options::recursive|
                std::filesystem::copy_options::overwrite_existing);
        }
        else {
            std::filesystem::copy_file(filePath,tempBackupPath,
                std::filesystem::copy_options::overwrite_existing);
        }
        if(std::filesystem::exists(backupPath)) {
            std::filesystem::remove_all(backupPath);
        }
        std::filesystem::rename(tempBackupPath,backupPath);

        g_logger<<"[INFO] 备份完成: "<<filePath<<" -> "<<backupPath<<std::endl;
        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[WARN] 备份失败: "<<filePath<<" - "<<e.what()<<std::endl;
        try {
            if(std::filesystem::exists(tempBackupPath)) {
                std::filesystem::remove_all(tempBackupPath);
            }
        }
        catch(...) {
        }

        return false;
    }
}

bool MinecraftUpdater::ExtractZip(const std::vector<unsigned char>& zipData,const std::string& extractPath) {
    EnsureDirectoryExists(extractPath);
    std::string tempDir=std::filesystem::temp_directory_path().string();
    std::string tempZip=tempDir+"/minecraft_update_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".zip";

    g_logger<<"[INFO] 将ZIP数据写入临时文件: "<<tempZip<<std::endl;

    std::ofstream tempFile(tempZip,std::ios::binary);
    if(!tempFile) {
        g_logger<<"[ERROR] 无法创建临时ZIP文件: "<<tempZip<<std::endl;
        return false;
    }

    tempFile.write(reinterpret_cast<const char*>(zipData.data()),zipData.size());
    tempFile.close();
    bool result=ExtractZipFromFile(tempZip,extractPath);
    std::error_code ec;
    std::filesystem::remove(tempZip,ec);
    if(ec) {
        g_logger<<"[WARN] 无法删除临时文件: "<<tempZip<<" - "<<ec.message()<<std::endl;
    }

    return result;
}

bool MinecraftUpdater::ExtractZipFromFile(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 开始解压文件: "<<zipFilePath<<" 到 "<<extractPath<<std::endl;

    if(!std::filesystem::exists(zipFilePath)) {
        g_logger<<"[ERROR] ZIP 文件不存在: "<<zipFilePath<<std::endl;
        return false;
    }

    std::error_code ec;
    auto fileSize=std::filesystem::file_size(zipFilePath,ec);
    if(ec||fileSize==0) {
        g_logger<<"[ERROR] ZIP 文件无效或为空: "<<zipFilePath<<std::endl;
        return false;
    }

    g_logger<<"[INFO] ZIP 文件大小: "<<FormatBytes(fileSize)<<std::endl;

    return ExtractZipWithMiniz(zipFilePath,extractPath);
}

bool MinecraftUpdater::ExtractZipWithMiniz(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 调用 miniz 解压方法..."<<std::endl;
    return ExtractZipSimple(zipFilePath,extractPath);
}

bool MinecraftUpdater::ExtractZipSimple(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 使用简单解压方法..."<<std::endl;

    g_logger<<"[INFO] 尝试使用 Windows 系统命令解压..."<<std::endl;
    if(ExtractZipWithSystemCommand(zipFilePath,extractPath)) {
        g_logger<<"[INFO] Windows 系统命令解压成功"<<std::endl;
        if(ValidateExtraction(extractPath)) {
            return true;
        }
        else {
            g_logger<<"[WARN] Windows 系统命令解压验证失败，尝试备用方案"<<std::endl;
            CleanupTempExtractDir(extractPath);
        }
    }
    else {
        g_logger<<"[WARN] Windows 系统命令解压失败，尝试备用方案"<<std::endl;
    }

    g_logger<<"[INFO] 尝试使用原始 libzip 解压..."<<std::endl;
    if(ExtractZipOriginal(zipFilePath,extractPath)) {
        g_logger<<"[INFO] libzip 解压成功"<<std::endl;
        if(ValidateExtraction(extractPath)) {
            return true;
        }
        else {
            g_logger<<"[WARN] libzip 解压验证失败"<<std::endl;
            CleanupTempExtractDir(extractPath);
            return false;
        }
    }
    else {
        g_logger<<"[ERROR] libzip 解压失败"<<std::endl;
        CleanupTempExtractDir(extractPath);
        return false;
    }
}

bool MinecraftUpdater::ExtractZipWithSystemCommand(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 使用 Windows 系统命令解压..."<<std::endl;
    if(!std::filesystem::exists(zipFilePath)) {
        g_logger<<"[ERROR] ZIP 文件不存在: "<<zipFilePath<<std::endl;
        return false;
    }
    EnsureDirectoryExists(extractPath);

    std::string command;
    command="powershell -Command \"Expand-Archive -Path '"+zipFilePath+
        "' -DestinationPath '"+extractPath+"' -Force -ErrorAction Stop\"";

    g_logger<<"[DEBUG] 执行命令: "<<command<<std::endl;

    STARTUPINFOA si={0};
    PROCESS_INFORMATION pi={0};
    si.cb=sizeof(si);
    si.dwFlags=STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE;

    if(CreateProcessA(NULL,(LPSTR)command.c_str(),NULL,NULL,FALSE,
        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
        NULL,NULL,&si,&pi)) {
        DWORD waitResult=WaitForSingleObject(pi.hProcess,60000);

        DWORD exitCode=1;
        if(waitResult==WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess,&exitCode);
        }
        else if(waitResult==WAIT_TIMEOUT) {
            g_logger<<"[ERROR] 解压命令超时"<<std::endl;
            TerminateProcess(pi.hProcess,1);
            exitCode=1;
        }
        else {
            g_logger<<"[ERROR] 等待解压进程失败，等待结果: "<<waitResult<<std::endl;
            exitCode=1;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if(exitCode==0) {
            g_logger<<"[INFO] Windows 系统命令解压成功"<<std::endl;
            return true;
        }
        else {
            g_logger<<"[ERROR] Windows 系统命令解压失败，退出代码: "<<exitCode<<std::endl;
            return false;
        }
    }
    else {
        DWORD error=GetLastError();
        g_logger<<"[ERROR] 无法创建解压进程，错误代码: "<<error<<std::endl;
        return false;
    }
}
std::wstring MinecraftUpdater::Utf8ToWide(const std::string& utf8Str) {
    if(utf8Str.empty()) return L"";

    int requiredSize=MultiByteToWideChar(CP_UTF8,0,utf8Str.c_str(),-1,NULL,0);
    if(requiredSize==0) {
        DWORD error=GetLastError();
        g_logger<<"[ERROR] MultiByteToWideChar failed, error: "<<error<<std::endl;
        return L"";
    }

    std::wstring wideStr(requiredSize,0);
    if(MultiByteToWideChar(CP_UTF8,0,utf8Str.c_str(),-1,&wideStr[0],requiredSize)==0) {
        DWORD error=GetLastError();
        g_logger<<"[ERROR] MultiByteToWideChar failed, error: "<<error<<std::endl;
        return L"";
    }
    wideStr.pop_back();
    return wideStr;
}

std::string MinecraftUpdater::WideToUtf8(const std::wstring& wideStr) {
    if(wideStr.empty()) return "";

    int requiredSize=WideCharToMultiByte(CP_UTF8,0,wideStr.c_str(),-1,NULL,0,NULL,NULL);
    if(requiredSize==0) {
        DWORD error=GetLastError();
        g_logger<<"[ERROR] WideCharToMultiByte failed, error: "<<error<<std::endl;
        return "";
    }

    std::string utf8Str(requiredSize,0);
    if(WideCharToMultiByte(CP_UTF8,0,wideStr.c_str(),-1,&utf8Str[0],requiredSize,NULL,NULL)==0) {
        DWORD error=GetLastError();
        g_logger<<"[ERROR] WideCharToMultiByte failed, error: "<<error<<std::endl;
        return "";
    }
    utf8Str.pop_back();
    return utf8Str;
}
bool MinecraftUpdater::ExtractZipOriginal(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 使用原始libzip解压..."<<std::endl;

    int err=0;
    zip_t* zip=zip_open(zipFilePath.c_str(),0,&err);
    if(!zip) {
        g_logger<<"[ERROR] 无法打开ZIP文件: "<<zipFilePath<<"，错误码: "<<err<<std::endl;
        return false;
    }

    zip_int64_t numEntries=zip_get_num_entries(zip,0);
    g_logger<<"[INFO] 总共 "<<numEntries<<" 个条目需要解压"<<std::endl;

    if(numEntries<=0) {
        g_logger<<"[ERROR] ZIP文件为空"<<std::endl;
        zip_close(zip);
        return false;
    }
    g_logger<<"[INFO] 第一步：创建目录结构..."<<std::endl;

    std::vector<std::string> fileEntries;

    for(zip_int64_t i=0; i<numEntries; i++) {
        const char* name=zip_get_name(zip,i,ZIP_FL_ENC_UTF_8);
        if(!name) {
            name=zip_get_name(zip,i,0);
            if(!name) {
                g_logger<<"[WARN] 无法获取文件 "<<i<<" 的文件名"<<std::endl;
                continue;
            }
        }
        std::string originalName=name;
        std::string safeName=originalName;
        std::wstring wideName=Utf8ToWide(originalName);
        if(wideName.empty()) {
            g_logger<<"[WARN] 无法转换文件名: "<<originalName<<std::endl;
            safeName="file_"+std::to_string(i)+".dat";
            g_logger<<"[INFO] 使用替代文件名: "<<safeName<<std::endl;
        }
        std::string fullPath;
        std::wstring wideExtractPath=Utf8ToWide(extractPath);
        if(!wideExtractPath.empty()&&!wideName.empty()) {
            std::wstring wideFullPath=wideExtractPath+L"/"+wideName;
            fullPath=WideToUtf8(wideFullPath);
            if(!wideName.empty()&&wideName.back()==L'/') {
                try {
                    std::filesystem::path dirPath=wideFullPath;
                    std::filesystem::create_directories(dirPath);

                    if(i%50==0) {
                        g_logger<<"[DEBUG] 创建目录: "<<originalName<<std::endl;
                    }
                }
                catch(const std::exception& e) {
                    g_logger<<"[WARN] 无法创建目录 "<<originalName<<": "<<e.what()<<std::endl;
                }
                continue;
            }
        }
        else {
            fullPath=extractPath+"/"+safeName;
        }
        fullPath=extractPath+"/"+safeName;
        if(!safeName.empty()&&safeName.back()=='/') {
            try {
                std::filesystem::create_directories(fullPath);
                if(i%50==0) {
                    g_logger<<"[DEBUG] 创建目录: "<<originalName<<std::endl;
                }
            }
            catch(const std::exception& e) {
                g_logger<<"[WARN] 无法创建目录 "<<originalName<<": "<<e.what()<<std::endl;
            }
            continue;
        }
        fileEntries.push_back(originalName);
    }

    g_logger<<"[INFO] 第二步：解压 "<<fileEntries.size()<<" 个文件..."<<std::endl;
    const size_t bufferSize=65536;
    std::vector<char> buffer(bufferSize);
    int extractedFiles=0;
    int failedFiles=0;
    int unicodeFailedFiles=0;
    int totalFiles=static_cast<int>(fileEntries.size());

    for(size_t idx=0; idx<fileEntries.size(); idx++) {
        const auto& originalName=fileEntries[idx];
        zip_int64_t index=zip_name_locate(zip,originalName.c_str(),ZIP_FL_ENC_UTF_8);
        if(index<0) {
            index=zip_name_locate(zip,originalName.c_str(),0);
            if(index<0) {
                g_logger<<"[WARN] 无法找到文件索引: "<<originalName<<std::endl;
                failedFiles++;
                continue;
            }
        }

        zip_file_t* zfile=zip_fopen_index(zip,index,0);
        if(!zfile) {
            g_logger<<"[WARN] 无法打开文件: "<<originalName<<std::endl;
            failedFiles++;
            continue;
        }

        std::string safeName=originalName;
        std::string fullPath;
        std::wstring wideExtractPath=Utf8ToWide(extractPath);
        std::wstring wideName=Utf8ToWide(originalName);

        if(!wideExtractPath.empty()&&!wideName.empty()) {
            std::wstring wideFullPath=wideExtractPath+L"/"+wideName;
            fullPath=WideToUtf8(wideFullPath);
            std::filesystem::path filePath=wideFullPath;
            std::filesystem::create_directories(filePath.parent_path());
            FILE* outFile=_wfopen(wideFullPath.c_str(),L"wb");
            if(outFile) {
                zip_int64_t bytesRead;
                long long totalBytes=0;
                while((bytesRead=zip_fread(zfile,buffer.data(),bufferSize))>0) {
                    size_t written=fwrite(buffer.data(),1,(size_t)bytesRead,outFile);
                    totalBytes+=bytesRead;
                }

                fclose(outFile);
                extractedFiles++;
            }
            else {
                DWORD error=GetLastError();
                g_logger<<"[ERROR] 无法创建文件: "<<originalName<<" (错误码: "<<error<<")"<<std::endl;
                failedFiles++;
                unicodeFailedFiles++;
                std::string asciiName="file_"+std::to_string(extractedFiles+failedFiles)+".dat";
                std::string asciiFullPath=extractPath+"/"+asciiName;

                g_logger<<"[INFO] 尝试使用ASCII名称: "<<asciiName<<std::endl;

                std::ofstream asciiFile(asciiFullPath,std::ios::binary);
                if(asciiFile) {
                    zip_int64_t bytesRead;
                    long long totalBytes=0;
                    zip_fclose(zfile);
                    zfile=zip_fopen_index(zip,index,0);

                    if(zfile) {
                        while((bytesRead=zip_fread(zfile,buffer.data(),bufferSize))>0) {
                            asciiFile.write(buffer.data(),bytesRead);
                            totalBytes+=bytesRead;
                        }
                        asciiFile.close();
                        extractedFiles++;
                        g_logger<<"[INFO] 文件 "<<originalName<<" 保存为 "<<asciiName<<std::endl;
                    }
                }
            }
        }
        else {
            g_logger<<"[WARN] 无法处理Unicode文件名: "<<originalName<<std::endl;
            failedFiles++;
            unicodeFailedFiles++;
        }
        zip_fclose(zfile);
        if((extractedFiles+failedFiles)%100==0) {
            int processed=extractedFiles+failedFiles;
            int percent=static_cast<int>((processed*100)/(std::max)(totalFiles,1));
            std::cout<<"\r解压进度: "<<processed<<"/"<<totalFiles<<" 文件 ("<<percent<<"%)，成功: "<<extractedFiles<<"，失败: "<<failedFiles<<"      ";
            std::cout.flush();
            g_logger<<"[INFO] 已处理 "<<processed<<"/"<<totalFiles<<" 个文件 ("<<percent<<"%)"<<std::endl;
        }
        if(failedFiles>=20&&idx>100) {
            g_logger<<"[ERROR] 失败文件过多，停止解压 (总失败: "<<failedFiles<<", Unicode失败: "<<unicodeFailedFiles<<")"<<std::endl;
            break;
        }
    }

    zip_close(zip);

    std::cout<<"\r解压完成: "<<extractedFiles<<"/"<<totalFiles<<" 个文件已提取，失败: "<<failedFiles<<" (Unicode失败: "<<unicodeFailedFiles<<")                  "<<std::endl;
    g_logger<<"[INFO] 解压完成: "<<extractedFiles<<"/"<<totalFiles<<" 个文件已提取，失败: "<<failedFiles<<std::endl;

    if(unicodeFailedFiles>0) {
        g_logger<<"[WARN] "<<unicodeFailedFiles<<" 个文件因Unicode编码问题未能正确提取"<<std::endl;
        g_logger<<"[WARN] 建议检查系统区域设置或使用英文文件名"<<std::endl;
    }
    float successRate=(totalFiles>0)?(extractedFiles*100.0f/totalFiles):0.0f;
    g_logger<<"[INFO] 解压成功率: "<<std::fixed<<std::setprecision(1)<<successRate<<"%"<<std::endl;
    if(successRate<80.0f) {
        g_logger<<"[WARN] 解压成功率较低，可能需要手动检查"<<std::endl;
        return false;
    }

    return extractedFiles>0;
}
bool MinecraftUpdater::DownloadAndExtract(const std::string& url,const std::string& relativePath) {
    g_logger<<"[INFO] 下载并解压: "<<url<<" -> "<<relativePath<<std::endl;
    std::string tempDir=std::filesystem::temp_directory_path().string();
    std::string tempZip=tempDir+"/mc_temp_"+
        std::to_string(GetCurrentProcessId())+"_"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".zip";
    std::string progressMessage="下载 "+relativePath;
    ShowProgressBar(progressMessage,0,1);

    bool downloadSuccess=httpClient.DownloadFileWithProgress(
        url,
        tempZip,
        [this,progressMessage](long long downloaded,long long total,void* userdata) {
            ShowProgressBar(progressMessage,downloaded,total);
        },
        nullptr
    );

    ClearProgressLine();

    if(!downloadSuccess) {
        g_logger<<"[ERROR] 下载失败: "<<url<<std::endl;
        return false;
    }
    std::error_code ec;
    auto fileSize=std::filesystem::file_size(tempZip,ec);
    if(ec) {
        g_logger<<"[ERROR] 无法获取文件大小: "<<ec.message()<<std::endl;
        std::filesystem::remove(tempZip);
        return false;
    }

    g_logger<<"[INFO] 下载完成，文件大小: "<<FormatBytes(fileSize)<<std::endl;
    if(fileSize<1024) {
        std::ifstream file(tempZip,std::ios::binary);
        if(file) {
            std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            file.close();
            if(content.find("404")!=std::string::npos||
                content.find("Not Found")!=std::string::npos||
                content.find("Error")!=std::string::npos) {

                g_logger<<"[INFO] 服务器返回错误页面，可能是空文件夹，将创建空目录"<<std::endl;
                g_logger<<"[DEBUG] 服务器响应: "<<content<<std::endl;
                std::filesystem::remove(tempZip);
                std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
                std::string extractPath=(gameDirPath/relativePath).string();
                try {
                    if(!std::filesystem::exists(extractPath)) {
                        std::filesystem::create_directories(extractPath);
                        g_logger<<"[INFO] 已创建空目录: "<<extractPath<<std::endl;
                    }
                    else {
                        g_logger<<"[INFO] 目录已存在: "<<extractPath<<std::endl;
                    }
                    return true;
                }
                catch(const std::exception& e) {
                    g_logger<<"[ERROR] 创建目录失败: "<<e.what()<<std::endl;
                    return false;
                }
            }
        }
    }
    if(!IsValidZipFile(tempZip)) {
        g_logger<<"[ERROR] 下载的文件不是有效的ZIP文件，大小: "<<FormatBytes(fileSize)<<std::endl;
        if(fileSize<1024) {
            std::ifstream file(tempZip,std::ios::binary);
            if(file) {
                std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                g_logger<<"[DEBUG] 文件内容: "<<content<<std::endl;
            }
            file.close();
        }

        std::filesystem::remove(tempZip);
        return false;
    }
    std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
    std::string extractPath=(gameDirPath/relativePath).string();
    if(std::filesystem::exists(extractPath)) {
        g_logger<<"[INFO] 备份原有目录..."<<std::endl;
        BackupFile(extractPath);
    }
    bool extractSuccess=ExtractZipOriginal(tempZip,extractPath);
    std::filesystem::remove(tempZip);

    if(!extractSuccess) {
        g_logger<<"[ERROR] 解压失败"<<std::endl;
        return false;
    }

    return true;
}
bool MinecraftUpdater::CheckServerResponse(const std::string& url) {
    g_logger<<"[DEBUG] 检查服务器响应: "<<url<<std::endl;

    try {
        std::string tempFile=std::filesystem::temp_directory_path().string()+"/test_response.bin";

        if(!httpClient.DownloadFileWithProgress(url,tempFile,nullptr,nullptr)) {
            g_logger<<"[DEBUG] 服务器响应测试失败"<<std::endl;
            return false;
        }

        std::error_code ec;
        auto fileSize=std::filesystem::file_size(tempFile,ec);
        std::filesystem::remove(tempFile);

        if(ec||fileSize==0) {
            g_logger<<"[DEBUG] 服务器返回空文件或错误"<<std::endl;
            return false;
        }

        g_logger<<"[DEBUG] 服务器响应正常，文件大小: "<<FormatBytes(fileSize)<<std::endl;
        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[DEBUG] 检查服务器响应异常: "<<e.what()<<std::endl;
        return false;
    }
}

bool MinecraftUpdater::IsValidZipFile(const std::string& filePath) {
    std::ifstream file(filePath,std::ios::binary);
    if(!file) {
        g_logger<<"[DEBUG] 无法打开文件: "<<filePath<<std::endl;
        return false;
    }

    file.seekg(0,std::ios::end);
    size_t fileSize=file.tellg();
    file.seekg(0,std::ios::beg);

    if(fileSize<22) {
        g_logger<<"[DEBUG] 文件太小 ("<<fileSize<<" 字节)，可能是空ZIP文件"<<std::endl;

        if(fileSize==0) {
            return true;
        }

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(),fileSize);

        if(fileSize==22) {
            if(buffer[0]==0x50&&buffer[1]==0x4B&&
                buffer[2]==0x05&&buffer[3]==0x06) {
                g_logger<<"[DEBUG] 有效的空ZIP文件（只有目录结束标记）"<<std::endl;
                return true;
            }
        }

        return false;
    }

    char header[4];
    file.read(header,4);

    if(file.gcount()<4) {
        g_logger<<"[DEBUG] 无法读取文件头"<<std::endl;
        return false;
    }
    bool isZipSignature=(header[0]==0x50&&header[1]==0x4B&&
        header[2]==0x03&&header[3]==0x04);

    if(!isZipSignature) {
        g_logger<<"[DEBUG] 文件头不是有效的ZIP签名: ";
        for(int i=0; i<4; ++i) {
            g_logger<<std::hex<<(int)(unsigned char)header[i]<<" ";
        }
        g_logger<<std::dec<<std::endl;

        if(fileSize==0) {
            g_logger<<"[DEBUG] 空文件，可能是空目录"<<std::endl;
            return true;
        }

        return false;
    }

    g_logger<<"[DEBUG] 有效的ZIP文件签名，文件大小: "<<FormatBytes(fileSize)<<std::endl;
    return true;
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

        g_logger<<"[DEBUG] 检查URL: "<<url<<std::endl;

        if(type=="directory") {
            g_logger<<"[INFO] 更新目录: "<<path<<std::endl;
            if(fileInfo.isMember("hash")) {
                g_logger<<"[DEBUG] 目录哈希: "<<fileInfo["hash"].asString()<<std::endl;
            }
            if(fileInfo.isMember("size")) {
                g_logger<<"[DEBUG] 期望大小: "<<FormatBytes(fileInfo["size"].asInt64())<<std::endl;
            }

            if(!DownloadAndExtract(url,path)) {
                g_logger<<"[ERROR] 错误: 目录更新失败: "<<path<<std::endl;

                if(forceSync) {
                    g_logger<<"[ERROR] 强制同步模式，更新失败"<<std::endl;
                    return false;
                }

                allSuccess=false;
                std::string fullPath=gameDirectory+"/"+path;
                g_logger<<"[WARN] 尝试创建空目录作为后备: "<<fullPath<<std::endl;

                try {
                    std::filesystem::create_directories(fullPath);
                    g_logger<<"[INFO] 已创建空目录: "<<fullPath<<std::endl;
                }
                catch(const std::exception& e) {
                    g_logger<<"[ERROR] 创建空目录失败: "<<e.what()<<std::endl;
                }
            }
            else {
                g_logger<<"[INFO] 目录更新成功: "<<path<<std::endl;
            }
        }
        else {
            std::string fullPath=gameDirectory+"/"+path;
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
    cachedUpdateInfo=Json::Value();
    hasCachedUpdateInfo=false;

    Json::Value().swap(cachedUpdateInfo);

    const Json::Value& packages=updateInfo["incremental_packages"];
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

        if(i>0) {
            OptimizeMemoryUsage();
        }

        Json::Value packageInfo;
        for(const auto& package:packages) {
            if(package["archive"].asString()==packagePath) {
                packageInfo=package;
                break;
            }
        }

        if(packageInfo.isNull()) {
            g_logger<<"[ERROR] 找不到包信息: "<<packagePath<<std::endl;
            continue;
        }

        std::string expectedHash=packageInfo["hash"].asString();
        long long expectedSize=packageInfo.isMember("size")?packageInfo["size"].asInt64():0;

        std::string tempDir=std::filesystem::temp_directory_path().string();
        std::string tempZip=tempDir+"/mc_pkg_"+std::to_string(i)+".zip";

        g_logger<<"[INFO] 开始下载更新包..."<<std::endl;
        std::string progressMessage="下载更新包 "+std::to_string(i+1)+"/"+std::to_string(packagePaths.size());
        ShowProgressBar(progressMessage,0,1);

        bool downloadSuccess=httpClient.DownloadFileWithProgress(
            packagePath,
            tempZip,
            [this,progressMessage,expectedSize](long long downloaded,long long total,void* userdata) {
                if(total<=0&&expectedSize>0) {
                    total=expectedSize;
                }
                ShowProgressBar(progressMessage,downloaded,total);
            },
            nullptr
        );

        ClearProgressLine();

        if(!downloadSuccess) {
            g_logger<<"[ERROR] 下载更新包失败: "<<packagePath<<std::endl;
            return false;
        }

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

            std::string actualHash=FileHasher::CalculateFileHashStream(tempZip,"md5");
            if(actualHash!=expectedHash) {
                g_logger<<"[ERROR] 更新包哈希验证失败"<<std::endl;
                g_logger<<"[ERROR] 期望: "<<expectedHash<<std::endl;
                g_logger<<"[ERROR] 实际: "<<actualHash<<std::endl;

                std::filesystem::remove(tempZip);
                return false;
            }
            else {
                g_logger<<"[INFO] 更新包哈希验证通过"<<std::endl;
            }
        }

        std::string tempExtractDir=tempDir+"/mc_extract_"+std::to_string(i);
        EnsureDirectoryExists(tempExtractDir);

        g_logger<<"[INFO] 解压更新包..."<<std::endl;
        if(!ExtractZipFromFile(tempZip,tempExtractDir)) {
            g_logger<<"[ERROR] 解压更新包失败: "<<packagePath<<std::endl;
            std::filesystem::remove_all(tempExtractDir);
            std::filesystem::remove(tempZip);
            return false;
        }

        g_logger<<"[INFO] 应用更新..."<<std::endl;
        if(!ApplyUpdateFromDirectory(tempExtractDir)) {
            g_logger<<"[ERROR] 应用更新失败"<<std::endl;
            std::filesystem::remove_all(tempExtractDir);
            std::filesystem::remove(tempZip);
            return false;
        }

        std::filesystem::remove_all(tempExtractDir);
        std::filesystem::remove(tempZip);

        g_logger<<"[INFO] 更新包 ("<<(i+1)<<"/"<<packagePaths.size()<<") 处理完成"<<std::endl;

        OptimizeMemoryUsage();
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
    int failedCount=0;

    std::wstring wideTempDir=Utf8ToWide(tempDir);
    std::wstring wideGameDir=Utf8ToWide(gameDirectory);

    if(wideTempDir.empty()||wideGameDir.empty()) {
        g_logger<<"[ERROR] 无法转换路径为宽字符"<<std::endl;
        return false;
    }

    try {
        for(const auto& entry:std::filesystem::recursive_directory_iterator(wideTempDir)) {
            if(entry.is_regular_file()) {
                std::wstring wideRelativePath=entry.path().wstring().substr(wideTempDir.size()+1);
                std::wstring wideTargetPath=wideGameDir+L"\\"+wideRelativePath;
                std::filesystem::path targetDir=std::filesystem::path(wideTargetPath).parent_path();
                if(!targetDir.empty()) {
                    std::filesystem::create_directories(targetDir);
                }
                bool copySuccess=CopyFileWithUnicode(entry.path().wstring(),wideTargetPath);

                if(copySuccess) {
                    fileCount++;
                    if(fileCount%100==0) {
                        std::cout<<"\r更新进度: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"     ";
                        std::cout.flush();
                    }
                }
                else {
                    failedCount++;
                }
            }
        }
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 遍历临时目录失败: "<<e.what()<<std::endl;
        return false;
    }

    std::cout<<"\r更新完成: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"                  "<<std::endl;
    g_logger<<"[INFO] 更新了 "<<fileCount<<" 个文件，失败: "<<failedCount<<std::endl;

    return fileCount>0&&failedCount==0;
}
bool MinecraftUpdater::ApplyUpdateFromDirectory(const std::string& sourceDir) {
    int fileCount=0;
    int failedCount=0;
    const int BATCH_SIZE=50;

    try {
        std::wstring wideSourceDir=Utf8ToWide(sourceDir);
        std::wstring wideGameDir=Utf8ToWide(gameDirectory);

        if(wideSourceDir.empty()||wideGameDir.empty()) {
            g_logger<<"[ERROR] 无法转换路径为宽字符"<<std::endl;
            return false;
        }

        for(const auto& entry:std::filesystem::recursive_directory_iterator(wideSourceDir)) {
            if(entry.is_regular_file()) {
                std::wstring wideRelativePath=entry.path().wstring().substr(wideSourceDir.size()+1);
                std::wstring wideTargetPath=wideGameDir+L"\\"+wideRelativePath;

                std::filesystem::path targetDir=std::filesystem::path(wideTargetPath).parent_path();
                if(!targetDir.empty()) {
                    std::filesystem::create_directories(targetDir);
                }

                bool copySuccess=CopyFileWithUnicode(entry.path().wstring(),wideTargetPath);

                if(copySuccess) {
                    fileCount++;

                    if(fileCount%BATCH_SIZE==0) {
                        OptimizeMemoryUsage();
                        std::cout<<"\r应用更新: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"     ";
                        std::cout.flush();
                    }
                }
                else {
                    failedCount++;
                    g_logger<<"[WARN] 文件复制失败: "<<WideToUtf8(entry.path().wstring())<<std::endl;
                }
            }
        }

        std::cout<<"\r应用更新完成: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<"                  "<<std::endl;
        g_logger<<"[INFO] 应用更新完成: "<<fileCount<<" 个文件已处理，失败: "<<failedCount<<std::endl;

        if(failedCount>0) {
            g_logger<<"[WARN] "<<failedCount<<" 个文件处理失败"<<std::endl;
            return false;
        }

        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 应用更新失败: "<<e.what()<<std::endl;
        return false;
    }
}

bool MinecraftUpdater::CopyFileWithUnicode(const std::wstring& sourcePath,const std::wstring& targetPath) {
    BOOL result=CopyFileW(sourcePath.c_str(),targetPath.c_str(),FALSE);

    if(!result) {
        DWORD error=GetLastError();

        if(error==ERROR_ACCESS_DENIED) {
            DeleteFileW(targetPath.c_str());
            result=CopyFileW(sourcePath.c_str(),targetPath.c_str(),FALSE);

            if(!result) {
                error=GetLastError();
                g_logger<<"[ERROR] 复制文件失败 (删除后重试): "<<WideToUtf8(sourcePath)<<" -> "<<WideToUtf8(targetPath)<<"，错误码: "<<error<<std::endl;
                return false;
            }
        }
        else {
            g_logger<<"[ERROR] 复制文件失败: "<<WideToUtf8(sourcePath)<<" -> "<<WideToUtf8(targetPath)<<"，错误码: "<<error<<std::endl;
            return false;
        }
    }

    return true;
}

void MinecraftUpdater::OptimizeMemoryUsage() {
    static int callCount=0;
    callCount++;
    if(callCount%10==0) {
        SetProcessWorkingSetSize(GetCurrentProcess(),(SIZE_T)-1,(SIZE_T)-1);
        HANDLE heap=GetProcessHeap();
        if(heap) {
            HeapCompact(heap,HEAP_NO_SERIALIZE);
        }
    }
}

void MinecraftUpdater::CleanupTempExtractDir(const std::string& extractPath) {
    g_logger<<"[INFO] 清理临时解压目录..."<<std::endl;
    if(!extractPath.empty()&&std::filesystem::exists(extractPath)) {
        try {
            std::string tempDir=std::filesystem::temp_directory_path().string();
            if(extractPath.find(tempDir)==0) {
                std::filesystem::remove_all(extractPath);
                g_logger<<"[INFO] 已清理临时解压目录: "<<extractPath<<std::endl;
            }
            else {
                g_logger<<"[INFO] 保留非临时目录: "<<extractPath<<std::endl;
            }
        }
        catch(const std::exception& e) {
            g_logger<<"[WARN] 无法清理解压目录: "<<e.what()<<std::endl;
        }
    }
    else {
        g_logger<<"[INFO] 解压目录不存在或为空，无需清理"<<std::endl;
    }
}

void MinecraftUpdater::CleanupTempFiles(const std::string& zipFilePath,const std::string& extractPath) {
    g_logger<<"[INFO] 清理所有临时文件..."<<std::endl;
    if(!zipFilePath.empty()&&std::filesystem::exists(zipFilePath)) {
        try {
            std::filesystem::remove(zipFilePath);
            g_logger<<"[INFO] 已清理临时 ZIP 文件: "<<zipFilePath<<std::endl;
        }
        catch(const std::exception& e) {
            g_logger<<"[WARN] 无法删除临时 ZIP 文件: "<<e.what()<<std::endl;
        }
    }
    CleanupTempExtractDir(extractPath);
}

bool MinecraftUpdater::ValidateExtraction(const std::string& extractPath) {
    g_logger<<"[INFO] 验证解压结果..."<<std::endl;

    if(!std::filesystem::exists(extractPath)) {
        g_logger<<"[ERROR] 解压目录不存在: "<<extractPath<<std::endl;
        return false;
    }

    try {
        int fileCount=0;
        int dirCount=0;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(extractPath)) {
            if(entry.is_directory()) {
                dirCount++;
            }
            else if(entry.is_regular_file()) {
                fileCount++;
                try {
                    auto fileSize=std::filesystem::file_size(entry.path());
                    if(fileSize==0) {
                        g_logger<<"[WARN] 发现空文件: "<<entry.path().string()<<std::endl;
                    }
                }
                catch(...) {
                }
            }
        }

        g_logger<<"[INFO] 解压验证: 总共 "<<(fileCount+dirCount)<<" 个条目 ("
            <<fileCount<<" 个文件, "<<dirCount<<" 个目录)"<<std::endl;

        if(fileCount==0&&dirCount==0) {
            g_logger<<"[WARN] 解压目录为空，可能解压失败"<<std::endl;
            return false;
        }

        if(fileCount+dirCount<3) {
            g_logger<<"[WARN] 解压条目数量较少，可能未完全解压"<<std::endl;
        }

        return true;
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 验证解压结果失败: "<<e.what()<<std::endl;
        return false;
    }
}