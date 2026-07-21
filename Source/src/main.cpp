#include <iostream>
#include <string>
#include "ConfigManager.h"
#include "UpdateOrchestrator.h"
#include "logger.h"
struct LoggerGuard {
    ~LoggerGuard() {
        Logger::Instance().Shutdown();
    }
};
int main(int argc,char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    if(argc==4&&strcmp(argv[1],"--elevated-replace")==0) {
        std::wstring newExe=FileSystemHelper::Utf8ToWide(argv[2]);
        std::wstring targetExe=FileSystemHelper::Utf8ToWide(argv[3]);

        wchar_t curExe[MAX_PATH];
        GetModuleFileNameW(NULL,curExe,MAX_PATH);

        if(_wcsicmp(targetExe.c_str(),curExe)!=0) {
            LOG_ERROR("提权替换目标不是当前程序，拒绝");
            return 1;
        }

        wchar_t tempPath[MAX_PATH];

        GetTempPathW(MAX_PATH,tempPath);
        std::filesystem::path tempDirPath(tempPath);
        std::filesystem::path newExePath(newExe);
        tempDirPath=std::filesystem::weakly_canonical(tempDirPath);
        newExePath=std::filesystem::weakly_canonical(newExePath);
        if(newExePath.wstring().find(tempDirPath.wstring())!=0) {
			LOG_ERROR("新文件不在临时目录（规范化后），拒绝");
            return 1;
        }

        for(int i=0; i<30; ++i) {
            HANDLE h=CreateFileW(targetExe.c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
            if(h!=INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                break;
            }
            Sleep(1000);
        }

        std::wstring backup=targetExe+L".old";
        DeleteFileW(backup.c_str());
        MoveFileW(targetExe.c_str(),backup.c_str());
        if(MoveFileExW(newExe.c_str(),targetExe.c_str(),MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(backup.c_str());
            STARTUPINFOW si={sizeof(si)};
            PROCESS_INFORMATION pi;
            CreateProcessW(targetExe.c_str(),NULL,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 0;
        }
        else {
            MoveFileW(backup.c_str(),targetExe.c_str());
            return 1;
        }
    }
    std::string cfg="config/updater.json";

    ConfigManager configManager(cfg);

    std::string currentVersion=configManager.ReadLauncherVersion();
    if(currentVersion.empty()) {
        currentVersion="0.0.1";
        configManager.WriteLauncherVersion(currentVersion);
    }

	LOG_INFO("当前启动器版本: v{}",currentVersion);

    if(!configManager.ConfigExists()) {
		LOG_INFO("未找到配置文件，正在生成默认配置文件...");

        if(!configManager.InitializeDefaultConfig()) {
			LOG_ERROR("生成默认配置文件失败!");
            return 1;
        }

		LOG_INFO("默认配置文件已生成，请编辑 {} 文件来配置更新服务器地址和游戏目录！",cfg);
		LOG_INFO("按回车键退出...");

        std::cin.get();
        return 0;
    }

    std::string logFile=configManager.ReadLogFile();
    if(!Logger::Instance().Initialize(logFile)) {
        LOG_ERROR("无法初始化日志文件，将继续使用控制台输出");
    }
    else {
        LOG_INFO("日志文件: {}",logFile);
    }
    LoggerGuard guard;

    std::string apiUrl=configManager.ReadUpdateUrl();
    std::string gameDir=configManager.ReadGameDirectory();

    if(apiUrl.empty()) {
        LOG_ERROR("配置文件中未设置更新api(update_url)！");
        return 1;
    }

    if(gameDir.empty()) {
        LOG_ERROR("配置文件中未设置游戏目录(game_directory)！");
        return 1;
    }

    LOG_INFO("Made by Reikumo.");
    LOG_INFO("配置加载成功：");
    LOG_INFO("  游戏目录: {}", gameDir);
    LOG_INFO("  更新服务器api: {}", apiUrl);
    LOG_INFO("  自动更新状态: {}", (configManager.ReadAutoUpdate() ? "开启" : "关闭"));
    LOG_INFO("  日志文件地址: {}", logFile);
    LOG_INFO("  客户端更新模式: {} (可能被服务端覆盖)", configManager.ReadUpdateMode());
    LOG_INFO("  哈希算法: {}", configManager.ReadHashAlgorithm());
    LOG_INFO("  文件删除功能: {}", (configManager.ReadEnableFileDeletion() ? "开启" : "关闭"));
    LOG_INFO("  API超时时间: {}秒", configManager.ReadApiTimeout());

    {
        UpdateOrchestrator updater(cfg,apiUrl,gameDir);

        if(updater.CheckForUpdates()) {
            if(configManager.ReadAutoUpdate()) {
                LOG_INFO("自动更新已开启，开始更新...");
                if(updater.ForceUpdate(false)) {
                    LOG_INFO("自动更新成功！");
                }
                else {
                    LOG_ERROR("自动更新失败");
                    return 1;
                }
            }
            else {
				LOG_INFO("是否立即更新？ (y/n): ");
                char choice;
                std::cin>>choice;

                if(choice=='y'||choice=='Y') {
                    std::cout<<"[INFO] 是否强制同步(y/n，强制同步会在更新失败时中止): ";
                    std::cin>>choice;
                    bool forceSync=(choice=='y'||choice=='Y');

                    if(updater.ForceUpdate(forceSync)) {
                        LOG_INFO("更新成功！");
                    }
                    else {
                        LOG_ERROR("更新失败！");
                        return 1;
                    }
                }
                else {
                    LOG_INFO("已取消更新。");
                }
            }
        }
    }

    LOG_INFO("=== McUpdaterClient 日志结束 ===");

    if(!configManager.ReadAutoUpdate()) {
        LOG_INFO("按回车键退出...");
        std::cin.ignore();
        std::cin.get();
    }

}