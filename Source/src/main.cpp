#include <iostream>
#include <string>
#include "MinecraftUpdater.h"

int main() {
	//默认的配置路径
	std::string cfg="config/updater.json";
	ConfigManager configManager(cfg);

	//看是不是第一次启动，如果是第一次启动，创建默认配置文件
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

	//初始化日志
	std::string logFile=configManager.ReadLogFile();
	if(!g_logger.Initialize(logFile)){
		std::cerr<<"[ERROR]无法初始化日志文件，将继续使用控制台输出"<<std::endl;
	}
	else{
		std::cout<<"[INFO]日志文件:"<<logFile<<std::endl;
	}

	//读取cfg
	std::string apiUrl=configManager.ReadUpdateUrl();
	std::string gameDir=configManager.ReadGameDirectory();
	bool autoUpdate=configManager.ReadAutoUpdate();
	bool enableApiCache=configManager.ReadEnableApiCache();
	int apiTimeout=configManager.ReadApiTimeout();
	std::string updateMode=configManager.ReadUpdateMode();
	std::string hashAlgorithm=configManager.ReadHashAlgorithm();
	bool enableFileDeletion=configManager.ReadEnableFileDeletion();
	bool skipMajorVersionCheck=configManager.ReadSkipMajorVersionCheck();

	//检查配置文件，防止有小天才没填
	if(apiUrl.empty()){
		g_logger<<"[ERROR]错误：配置文件中未设置更新api(update_url)！"<<std::endl;
		return 1;
	}

	if(gameDir.empty()){
		g_logger<<"[ERROR]错误：配置文件中未设置游戏目录(game_directory)！"<<std::endl;
		return 1;
	}
	g_logger<<"[INFO]当前McUpdater版本:v0.0.1"<<std::endl;
	g_logger<<"[INFO]Made by Reikumo."<<std::endl;
	g_logger<<"[INFO]配置加载成功："<<std::endl;
	g_logger<<"[INFO]  游戏目录: "<<gameDir<<std::endl;
	g_logger<<"[INFO]  更新服务器api: "<<apiUrl<<std::endl;
	g_logger<<"[INFO]  自动更新状态: "<<(autoUpdate?"开启":"关闭")<<std::endl;
	g_logger<<"[INFO]  日志文件地址: "<<logFile<<std::endl;
	g_logger<<"[INFO]  客户端更新模式: "<<updateMode<<" (可能被服务端覆盖)"<<std::endl;
	g_logger<<"[INFO]  哈希算法: "<<hashAlgorithm<<std::endl;
	g_logger<<"[INFO]  文件删除功能: "<<(enableFileDeletion?"开启":"关闭")<<std::endl;
	g_logger<<"[INFO]  跳过主版本检查: "<<(skipMajorVersionCheck?"是":"否")<<std::endl;
	g_logger<<"[INFO]  API缓存: "<<(enableApiCache?"启用":"禁用")<<std::endl;
	g_logger<<"[INFO]  API超时时间: "<<apiTimeout<<"秒"<<std::endl;
	g_logger<<std::endl;

	MinecraftUpdater updater(cfg,apiUrl,gameDir);

	if(updater.CheckForUpdates()){
		if(autoUpdate){
			g_logger<<"[INFO]自动更新已开启，开始更新..."<<std::endl;
			if(updater.ForceUpdate(false)){//不强制同步
				g_logger<<"[INFO]自动更新成功！"<<std::endl;
			}
			else{
				g_logger<<"[ERROR]错误: 自动更新失败"<<std::endl;
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
					g_logger<<"[ERROR]错误: 更新失败！"<<std::endl;
					return 1;
				}
			}
			else{
				g_logger<<"[INFO]已取消更新。"<<std::endl;
			}
		}
	}

	g_logger<<"[INFO]=== McUpdater 日志结束 ==="<<std::endl;
	return 0;
}