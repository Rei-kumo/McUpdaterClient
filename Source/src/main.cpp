#include <iostream>
#include <string>
#include "ConfigManager.h"
#include "UpdateOrchestrator.h"
int main(int argc,char* argv[]) {
    if(argc==4&&strcmp(argv[1],"--elevated-replace")==0) {
        std::wstring newExe=FileSystemHelper::Utf8ToWide(argv[2]);
        std::wstring targetExe=FileSystemHelper::Utf8ToWide(argv[3]);

        wchar_t curExe[MAX_PATH];
        GetModuleFileNameW(NULL,curExe,MAX_PATH);

        if(_wcsicmp(targetExe.c_str(),curExe)!=0) {
            g_logger<<"[ERROR] 提权替换目标不是当前程序，拒绝"<<std::endl;
            return 1;
        }

        wchar_t tempPath[MAX_PATH];

        GetTempPathW(MAX_PATH,tempPath);
        std::filesystem::path tempDirPath(tempPath);
        std::filesystem::path newExePath(newExe);
        tempDirPath=std::filesystem::weakly_canonical(tempDirPath);
        newExePath=std::filesystem::weakly_canonical(newExePath);
        if(newExePath.wstring().find(tempDirPath.wstring())!=0) {
            g_logger<<"[ERROR] 新文件不在临时目录（规范化后），拒绝"<<std::endl;
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

    g_logger<<"[INFO] 当前启动器版本: v"<<currentVersion<<std::endl;

    if(!configManager.ConfigExists()) {
        std::cout<<"[INFO] 未找到配置文件，正在生成默认配置文件..."<<std::endl;

        if(!configManager.InitializeDefaultConfig()) {
            std::cerr<<"[ERROR] 生成默认配置文件失败!"<<std::endl;
            return 1;
        }

        std::cout<<"[INFO] 默认配置文件已生成，请编辑 "<<cfg<<" 文件来配置更新服务器地址和游戏目录！"<<std::endl;
        std::cout<<"[INFO] 按回车键退出..."<<std::endl;
        std::cin.get();
        return 0;
    }

    std::string logFile=configManager.ReadLogFile();
    if(!g_logger.Initialize(logFile)) {
        std::cerr<<"[ERROR] 无法初始化日志文件，将继续使用控制台输出"<<std::endl;
    }
    else {
        std::cout<<"[INFO] 日志文件: "<<logFile<<std::endl;
    }

    std::string apiUrl=configManager.ReadUpdateUrl();
    std::string gameDir=configManager.ReadGameDirectory();

    if(apiUrl.empty()) {
        g_logger<<"[ERROR] 配置文件中未设置更新api(update_url)！"<<std::endl;
        return 1;
    }

    if(gameDir.empty()) {
        g_logger<<"[ERROR] 配置文件中未设置游戏目录(game_directory)！"<<std::endl;
        return 1;
    }

    g_logger<<"[INFO] Made by Reikumo."<<std::endl;
    g_logger<<"[INFO] 配置加载成功："<<std::endl;
    g_logger<<"[INFO]  游戏目录: "<<gameDir<<std::endl;
    g_logger<<"[INFO]  更新服务器api: "<<apiUrl<<std::endl;
    g_logger<<"[INFO]  自动更新状态: "<<(configManager.ReadAutoUpdate()?"开启":"关闭")<<std::endl;
    g_logger<<"[INFO]  日志文件地址: "<<logFile<<std::endl;
    g_logger<<"[INFO]  客户端更新模式: "<<configManager.ReadUpdateMode()<<" (可能被服务端覆盖)"<<std::endl;
    g_logger<<"[INFO]  哈希算法: "<<configManager.ReadHashAlgorithm()<<std::endl;
    g_logger<<"[INFO]  文件删除功能: "<<(configManager.ReadEnableFileDeletion()?"开启":"关闭")<<std::endl;
    g_logger<<"[INFO]  API超时时间: "<<configManager.ReadApiTimeout()<<"秒"<<std::endl;
    g_logger<<std::endl;

    {
        UpdateOrchestrator updater(cfg,apiUrl,gameDir);

        if(updater.CheckForUpdates()) {
            if(configManager.ReadAutoUpdate()) {
                g_logger<<"[INFO] 自动更新已开启，开始更新..."<<std::endl;
                if(updater.ForceUpdate(false)) {
                    g_logger<<"[INFO] 自动更新成功！"<<std::endl;
                }
                else {
                    g_logger<<"[ERROR] 自动更新失败"<<std::endl;
                    return 1;
                }
            }
            else {
                std::cout<<"[INFO] 是否立即更新？ (y/n): ";
                char choice;
                std::cin>>choice;

                if(choice=='y'||choice=='Y') {
                    std::cout<<"[INFO] 是否强制同步(y/n，强制同步会在更新失败时中止): ";
                    std::cin>>choice;
                    bool forceSync=(choice=='y'||choice=='Y');

                    if(updater.ForceUpdate(forceSync)) {
                        g_logger<<"[INFO] 更新成功！"<<std::endl;
                    }
                    else {
                        g_logger<<"[ERROR] 更新失败！"<<std::endl;
                        return 1;
                    }
                }
                else {
                    g_logger<<"[INFO] 已取消更新。"<<std::endl;
                }
            }
        }
    }

    g_logger<<"[INFO] === McUpdaterClient 日志结束 ==="<<std::endl;

    if(!configManager.ReadAutoUpdate()) {
        std::cout<<"按回车键退出..."<<std::endl;
        std::cin.ignore();
        std::cin.get();
    }

}