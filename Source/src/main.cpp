#include <iostream>
#include <string>
#include "MinecraftUpdater.h"

int main() {
    std::string cfg="config/updater.json";

    ConfigManager configManager(cfg);

    if(!configManager.ConfigExists()){
        std::cout<<"[INFO]未找到配置文件，正在生成默认配置文件..."<<std::endl;

        if(!configManager.InitializeDefaultConfig()){
            std::cerr<<"[ERROR]生成默认配置文件失败!"<<std::endl;
            return 1;
        }

        std::cout<<"[INFO]默认配置文件已生成，请编辑"<<cfg<<"文件来配置更新服务器地址和游戏目录！"<<std::endl;
        std::cout<<"[INFO]按回车键退出..."<<std::endl;
        std::cin.get();
        return 0;
    }

    std::string logFile=configManager.ReadLogFile();
    if(!g_logger.Initialize(logFile)){
        std::cerr<<"[ERROR]无法初始化日志文件，将继续使用控制台输出"<<std::endl;
    }
    else{
        std::cout<<"[INFO]日志文件:"<<logFile<<std::endl;
    }

    std::string apiUrl=configManager.ReadUpdateUrl();
    std::string gameDir=configManager.ReadGameDirectory();

    if(apiUrl.empty()){
        g_logger<<"[ERROR]配置文件中未设置更新api(update_url)！"<<std::endl;
        return 1;
    }

    if(gameDir.empty()){
        g_logger<<"[ERROR]配置文件中未设置游戏目录(game_directory)！"<<std::endl;
        return 1;
    }

    g_logger<<"[INFO]当前McUpdaterClient版本:v0.0.4"<<std::endl;
    g_logger<<"[INFO]Made by Reikumo."<<std::endl;
    g_logger<<"[INFO]配置加载成功："<<std::endl;
    g_logger<<"[INFO]  游戏目录: "<<gameDir<<std::endl;
    g_logger<<"[INFO]  更新服务器api: "<<apiUrl<<std::endl;
    g_logger<<"[INFO]  自动更新状态: "<<(configManager.ReadAutoUpdate()?"开启":"关闭")<<std::endl;
    g_logger<<"[INFO]  日志文件地址: "<<logFile<<std::endl;
    g_logger<<"[INFO]  客户端更新模式: "<<configManager.ReadUpdateMode()<<" (可能被服务端覆盖)"<<std::endl;
    g_logger<<"[INFO]  哈希算法: "<<configManager.ReadHashAlgorithm()<<std::endl;
    g_logger<<"[INFO]  文件删除功能: "<<(configManager.ReadEnableFileDeletion()?"开启":"关闭")<<std::endl;
    g_logger<<"[INFO]  跳过主版本检查: "<<(configManager.ReadSkipMajorVersionCheck()?"是":"否")<<std::endl;
    g_logger<<"[INFO]  API缓存: "<<(configManager.ReadEnableApiCache()?"启用":"禁用")<<std::endl;
    g_logger<<"[INFO]  API超时时间: "<<configManager.ReadApiTimeout()<<"秒"<<std::endl;
    g_logger<<std::endl;

    {
        MinecraftUpdater updater(cfg,apiUrl,gameDir);

        if(updater.CheckForUpdates()){
            if(configManager.ReadAutoUpdate()){
                g_logger<<"[INFO]自动更新已开启，开始更新..."<<std::endl;
                if(updater.ForceUpdate(false)){
                    g_logger<<"[INFO]自动更新成功！"<<std::endl;
                }
                else{
                    g_logger<<"[ERROR]自动更新失败"<<std::endl;
                    return 1;
                }
            }
            else{
                std::cout<<"[INFO]是否立即更新？ (y/n): ";
                char choice;
                std::cin>>choice;

                if(choice=='y'||choice=='Y'){
                    std::cout<<"[INFO]是否强制同步(y/n，强制同步会在更新失败时中止):";
                    std::cin>>choice;
                    bool forceSync=(choice=='y'||choice=='Y');

                    if(updater.ForceUpdate(forceSync)){
                        g_logger<<"[INFO]更新成功！"<<std::endl;
                    }
                    else{
                        g_logger<<"[ERROR]更新失败！"<<std::endl;
                        return 1;
                    }
                }
                else{
                    g_logger<<"[INFO]已取消更新。"<<std::endl;
                }
            }
        }
    }

    g_logger<<"[INFO]=== McUpdaterClient 日志结束 ==="<<std::endl;
    return 0;
}