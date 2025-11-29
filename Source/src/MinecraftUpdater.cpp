#include "MinecraftUpdater.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <zip.h>
#include "FileHasher.h"
#include <regex>
#include <cmath>
#include <set>
#include "Logger.h"

MinecraftUpdater::MinecraftUpdater(const std::string& config, const std::string& url, const std::string& gameDir)
    : gameDirectory(gameDir),
      configManager(config),
      httpClient(configManager.ReadApiTimeout()),
      updateChecker(url, httpClient, configManager),
      hasCachedUpdateInfo(false),
      enableApiCache(configManager.ReadEnableApiCache()) {
}

bool MinecraftUpdater::CheckForUpdates() {
	g_logger<<"[INFO]开始检查更新..."<<std::endl;

	if(!enableApiCache) {
		g_logger<<"[INFO]API缓存已禁用，强制重新获取更新信息"<<std::endl;
		hasCachedUpdateInfo=false;
		cachedUpdateInfo=Json::Value();
	}

	Json::Value updateInfo;
	if(hasCachedUpdateInfo) {
		updateInfo=cachedUpdateInfo;
		g_logger<<"[INFO]使用缓存的更新信息"<<std::endl;
	}
	else {
		updateInfo=updateChecker.FetchUpdateInfo();
		if(!updateInfo.isNull()) {
			cachedUpdateInfo=updateInfo;
			hasCachedUpdateInfo=true;
		}
	}

	if(updateInfo.isNull()) {
		g_logger<<"[ERROR]错误: 无法获取更新信息"<<std::endl;
		return false;
	}

	std::string serverUpdateMode;
	if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
		serverUpdateMode=updateInfo["update_mode"].asString();
		g_logger<<"[INFO]服务端强制使用更新模式: "<<serverUpdateMode<<std::endl;
	}
	else {
		serverUpdateMode=configManager.ReadUpdateMode();
		g_logger<<"[INFO]使用客户端配置的更新模式: "<<serverUpdateMode<<std::endl;
	}

	if(serverUpdateMode=="hash") {
		return CheckForUpdatesByHash();
	}
	else {
		return updateChecker.CheckForUpdates();
	}
}

bool MinecraftUpdater::CheckForUpdatesByHash() {
	if(!enableApiCache) {
		g_logger<<"[INFO]API缓存已禁用，强制重新获取更新信息"<<std::endl;
		hasCachedUpdateInfo=false;
		cachedUpdateInfo=Json::Value();
	}

	Json::Value updateInfo;

	if(hasCachedUpdateInfo) {
		updateInfo=cachedUpdateInfo;
		g_logger<<"[INFO]使用缓存的更新信息进行哈希检查"<<std::endl;
	}
	else {
		updateInfo=updateChecker.FetchUpdateInfo();
		if(updateInfo.isNull()) {
			g_logger<<"[ERROR]错误: 无法获取更新信息"<<std::endl;
			return false;
		}
		cachedUpdateInfo=updateInfo;
		hasCachedUpdateInfo=true;
	}

	std::string localVersion=configManager.ReadVersion();
	std::string remoteVersion=updateInfo["version"].asString();

	g_logger<<"[INFO]本地版本: "<<localVersion<<std::endl;
	g_logger<<"[INFO]远程版本: "<<remoteVersion<<std::endl;

	bool isConsistent=CheckFileConsistency(updateInfo["files"],updateInfo["directories"]);

	if(remoteVersion>localVersion) {
		std::cout<<"[INFO]发现新版本: "<<remoteVersion<<std::endl;

		if(ShouldForceHashUpdate(localVersion,remoteVersion)) {
			g_logger<<"[INFO]检测到跨越多个版本更新"<<std::endl;
		}

		if(!isConsistent) {
			g_logger<<"[INFO]文件一致性检查失败，需要更新"<<std::endl;
			return true;
		}
		else {
			g_logger<<"[INFO]版本号更新但文件已是最新，无需更新"<<std::endl;
			return false;
		}
	}
	else if(remoteVersion==localVersion) {
		if(!isConsistent) {
			g_logger<<"[INFO]版本号相同但文件不一致，需要修复"<<std::endl;
			return true;
		}
		else {
			g_logger<<"[INFO]当前已是最新版本且文件完整"<<std::endl;
			return false;
		}
	}
	else {
		if(!isConsistent) {
			g_logger<<"[WARN]本地版本较新但文件不一致，建议修复"<<std::endl;
			std::cout<<"[WARN]本地版本较新但文件可能损坏，是否修复？(y/n): ";
			char choice;
			std::cin>>choice;
			return (choice=='y'||choice=='Y');
		}
		else {
			g_logger<<"[INFO]本地版本较新且文件完整"<<std::endl;
			return false;
		}
	}
}

bool MinecraftUpdater::ShouldForceHashUpdate(const std::string& localVersion,const std::string& remoteVersion) {
	if(configManager.ReadSkipMajorVersionCheck()) {
		return false;
	}

	// 这里的版本号目前来说是1.1.1这种，如果自定义的话可能会有bug，但是1应该会有自定义的需求，之后修改
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

	g_logger<<"[DEBUG]开始文件一致性检查..."<<std::endl;

	for(const auto& fileInfo:fileManifest) {
		std::string relativePath=fileInfo["path"].asString();
		std::string expectedHash=fileInfo["hash"].asString();
		std::string fullPath=gameDirectory+"/"+relativePath;

		totalChecked++;

		if(!std::filesystem::exists(fullPath)) {
			g_logger<<"[INFO]文件不存在: "<<relativePath<<std::endl;
			allFilesConsistent=false;
			missingFiles++;
			continue;
		}

		std::string actualHash=FileHasher::CalculateFileHash(fullPath,hashAlgorithm);
		if(actualHash.empty()) {
			g_logger<<"[WARN]无法计算文件哈希: "<<relativePath<<std::endl;
			allFilesConsistent=false;
			mismatchedFiles++;
		}
		else if(actualHash!=expectedHash) {
			g_logger<<"[INFO]文件哈希不匹配: "<<relativePath<<std::endl;
			g_logger<<"[INFO]期望: "<<expectedHash<<std::endl;
			g_logger<<"[INFO]实际: "<<actualHash<<std::endl;
			allFilesConsistent=false;
			mismatchedFiles++;
		}
		else {
			g_logger<<"[DEBUG]文件一致: "<<relativePath<<std::endl;
		}
	}

	for(const auto& dirInfo:directoryManifest) {
		std::string relativePath=dirInfo["path"].asString();
		std::string fullPath=gameDirectory+"/"+relativePath;

		if(!std::filesystem::exists(fullPath)) {
			g_logger<<"[INFO]目录不存在: "<<relativePath<<std::endl;
			allFilesConsistent=false;
			missingFiles++;
			continue;
		}

		// 检查目录内每个文件
		const Json::Value& contents=dirInfo["contents"];
		for(const auto& contentInfo:contents) {
			std::string fileRelativePath=contentInfo["path"].asString();
			std::string expectedHash=contentInfo["hash"].asString();
			std::string fileFullPath=fullPath+"/"+fileRelativePath;

			totalChecked++;

			if(!std::filesystem::exists(fileFullPath)) {
				g_logger<<"[INFO]目录内文件不存在: "<<fileRelativePath<<std::endl;
				allFilesConsistent=false;
				missingFiles++;
				continue;
			}

			std::string actualHash=FileHasher::CalculateFileHash(fileFullPath,hashAlgorithm);
			if(actualHash.empty()) {
				g_logger<<"[WARN]无法计算目录内文件哈希: "<<fileRelativePath<<std::endl;
				allFilesConsistent=false;
				mismatchedFiles++;
			}
			else if(actualHash!=expectedHash) {
				g_logger<<"[INFO]目录内文件哈希不匹配: "<<fileRelativePath<<std::endl;
				allFilesConsistent=false;
				mismatchedFiles++;
			}
			else {
				g_logger<<"[DEBUG]目录内文件一致: "<<fileRelativePath<<std::endl;
			}
		}
	}

	g_logger<<"[INFO]文件一致性检查完成:"<<std::endl;
	g_logger<<"[INFO]  总共检查: "<<totalChecked<<" 个文件"<<std::endl;
	g_logger<<"[INFO]  缺失文件: "<<missingFiles<<" 个"<<std::endl;
	g_logger<<"[INFO]  不匹配文件: "<<mismatchedFiles<<" 个"<<std::endl;
	g_logger<<"[INFO]  文件一致性: "<<(allFilesConsistent?"通过":"失败")<<std::endl;

	return allFilesConsistent;
}

bool MinecraftUpdater::ForceUpdate(bool forceSync) {
	if(!enableApiCache) {
		g_logger<<"[INFO]API缓存已禁用，强制重新获取更新信息"<<std::endl;
		hasCachedUpdateInfo=false;
		cachedUpdateInfo=Json::Value();
	}

	Json::Value updateInfo;
	if(hasCachedUpdateInfo) {
		updateInfo=cachedUpdateInfo;
		g_logger<<"[INFO]使用缓存的更新信息进行更新"<<std::endl;
	}
	else {
		updateInfo=updateChecker.FetchUpdateInfo();
	}

	if(updateInfo.isNull()) {
		g_logger<<"[ERROR]错误: 无法获取更新信息"<<std::endl;
		return false;
	}

	std::string serverUpdateMode;
	if(updateInfo.isMember("update_mode")&&!updateInfo["update_mode"].asString().empty()) {
		serverUpdateMode=updateInfo["update_mode"].asString();
		g_logger<<"[INFO]服务端强制使用更新模式: "<<serverUpdateMode<<std::endl;
	}
	else {
		serverUpdateMode=configManager.ReadUpdateMode();
		g_logger<<"[INFO]使用客户端配置的更新模式: "<<serverUpdateMode<<std::endl;
	}

	std::string newVersion=updateInfo["version"].asString();

	if(serverUpdateMode=="hash") {
		g_logger<<"[INFO]开始更新到版本: "<<newVersion<<" (哈希模式)"<<std::endl;
		if(SyncFilesByHash(updateInfo)) {
			g_logger<<"[INFO]文件同步完成，更新版本信息..."<<std::endl;
			UpdateLocalVersion(newVersion);
			g_logger<<"[INFO]更新完成！"<<std::endl;
			return true;
		}
		else {
			g_logger<<"[ERROR]错误: 更新过程中出现错误！"<<std::endl;
			return false;
		}
	}
	else {
		g_logger<<"[INFO]开始更新到版本: "<<newVersion<<" (版本号模式)"<<std::endl;

		bool allSuccess=true;

		Json::Value fileList=updateInfo["files"];
		if(fileList.isArray()&&fileList.size()>0) {
			g_logger<<"[INFO]处理文件更新..."<<std::endl;
			if(!SyncFiles(fileList,forceSync)) {
				g_logger<<"[ERROR]错误: 文件更新失败"<<std::endl;
				if(forceSync) return false;
				allSuccess=false;
			}
		}

		Json::Value directoryList=updateInfo["directories"];
		if(directoryList.isArray()&&directoryList.size()>0) {
			g_logger<<"[INFO]处理目录更新..."<<std::endl;
			for(const auto& dirInfo:directoryList) {
				if(!dirInfo.isObject()) continue;

				std::string path=dirInfo["path"].asString();
				std::string url=dirInfo["url"].asString();

				if(path.empty()||url.empty()) {
					g_logger<<"[ERROR]错误: 目录信息不完整: path="<<path<<", url="<<url<<std::endl;
					if(forceSync) return false;
					allSuccess=false;
					continue;
				}

				g_logger<<"[INFO]更新目录: "<<path<<std::endl;
				if(!DownloadAndExtract(url,path)) {
					g_logger<<"[ERROR]错误: 目录更新失败: "<<path<<std::endl;
					if(forceSync) return false;
					allSuccess=false;
				}
				else {
					g_logger<<"[INFO]目录更新成功: "<<path<<std::endl;
				}
			}
		}

		if(allSuccess) {
			g_logger<<"[INFO]文件同步完成，更新版本信息..."<<std::endl;
			UpdateLocalVersion(newVersion);
			g_logger<<"[INFO]更新完成！"<<std::endl;
			return true;
		}
		else {
			g_logger<<"[ERROR]错误: 更新过程中出现错误！"<<std::endl;
			return false;
		}
	}
}

bool MinecraftUpdater::SyncFilesByHash(const Json::Value& updateInfo) {
	g_logger<<"[INFO]开始哈希模式同步..."<<std::endl;
	g_logger<<"[DEBUG]更新信息包含files字段: "<<updateInfo.isMember("files")<<std::endl;
	g_logger<<"[DEBUG]更新信息包含directories字段: "<<updateInfo.isMember("directories")<<std::endl;
	g_logger<<"[DEBUG]更新信息包含file_manifest字段: "<<updateInfo.isMember("file_manifest")<<std::endl;
	g_logger<<"[DEBUG]更新信息包含directory_manifest字段: "<<updateInfo.isMember("directory_manifest")<<std::endl;

	if(configManager.ReadEnableFileDeletion()) {
		ProcessDeleteList(updateInfo["delete_list"]);
	}

	Json::Value fileManifest=updateInfo["files"];
	Json::Value directoryManifest=updateInfo["directories"];

	g_logger<<"[DEBUG]文件清单数量: "<<fileManifest.size()<<std::endl;
	g_logger<<"[DEBUG]目录清单数量: "<<directoryManifest.size()<<std::endl;

	if(!UpdateFilesByHash(fileManifest,directoryManifest)) {
		return false;
	}

	g_logger<<"[INFO]哈希模式同步完成"<<std::endl;
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
					g_logger<<"[INFO]删除目录: "<<path<<std::endl;
				}
				else {
					std::filesystem::remove(fullPath);
					g_logger<<"[INFO]删除文件: "<<path<<std::endl;
				}
			}
		}
		catch(const std::exception& e) {
			g_logger<<"[WARN]删除失败: "<<path<<" - "<<e.what()<<std::endl;
		}
	}
	return true;
}

bool MinecraftUpdater::UpdateFilesByHash(const Json::Value& fileManifest,const Json::Value& directoryManifest) {
	std::string hashAlgorithm=configManager.ReadHashAlgorithm();
	bool allSuccess=true;

	g_logger<<"[DEBUG]开始更新文件，文件数量: "<<fileManifest.size()<<std::endl;
	g_logger<<"[DEBUG]游戏目录: "<<gameDirectory<<std::endl;

	for(const auto& fileInfo:fileManifest) {
		std::string relativePath=fileInfo["path"].asString();
		std::string expectedHash=fileInfo["hash"].asString();
		std::string url=fileInfo["url"].asString();

		std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
		std::filesystem::path fullPath=gameDirPath/relativePath;
		std::string fullPathStr=fullPath.string();

		g_logger<<"[DEBUG]处理文件: "<<relativePath<<std::endl;
		g_logger<<"[DEBUG]完整路径: "<<fullPathStr<<std::endl;
		g_logger<<"[DEBUG]文件URL: "<<url<<std::endl;

		std::filesystem::path parentDir=fullPath.parent_path();
		g_logger<<"[DEBUG]父目录: "<<parentDir.string()<<std::endl;
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
				g_logger<<"[DEBUG]目录写入权限检查通过: "<<parentDir.string()<<std::endl;
			}
		}
		catch(const std::exception& e) {
			g_logger<<"[DEBUG]目录写入权限检查失败: "<<e.what()<<std::endl;
		}

		if(!canWrite) {
			g_logger<<"[ERROR]错误: 目录没有写入权限: "<<parentDir.string()<<std::endl;
			allSuccess=false;
			continue;
		}

		if(std::filesystem::exists(fullPath)) {
			std::string actualHash=FileHasher::CalculateFileHash(fullPathStr,hashAlgorithm);
			if(!actualHash.empty()&&actualHash==expectedHash) {
				g_logger<<"[INFO]文件已是最新: "<<relativePath<<std::endl;
				continue;
			}
		}

		g_logger<<"[INFO]下载文件: "<<relativePath<<std::endl;
		if(!httpClient.DownloadFile(url,fullPathStr)) {
			g_logger<<"[ERROR]文件下载失败: "<<relativePath<<std::endl;

			g_logger<<"[DEBUG]目标路径: "<<fullPathStr<<std::endl;
			g_logger<<"[DEBUG]父目录存在: "<<std::filesystem::exists(parentDir)<<std::endl;
			g_logger<<"[DEBUG]父目录可写: "<<canWrite<<std::endl;

			allSuccess=false;
		}
		else {
			g_logger<<"[INFO]文件下载成功: "<<relativePath<<std::endl;

			if(!expectedHash.empty()) {
				std::string downloadedHash=FileHasher::CalculateFileHash(fullPathStr,hashAlgorithm);
				if(downloadedHash!=expectedHash) {
					g_logger<<"[WARN]文件哈希验证失败: "<<relativePath<<std::endl;
					g_logger<<"[WARN]期望: "<<expectedHash<<std::endl;
					g_logger<<"[WARN]实际: "<<downloadedHash<<std::endl;
				}
				else {
					g_logger<<"[INFO]文件哈希验证成功: "<<relativePath<<std::endl;
				}
			}
		}
	}

	g_logger<<"[DEBUG]开始更新目录，目录数量: "<<directoryManifest.size()<<std::endl;
	for(const auto& dirInfo:directoryManifest) {
		if(!SyncDirectoryByHash(dirInfo)) {
			allSuccess=false;
		}
	}

	return allSuccess;
}

bool MinecraftUpdater::SyncDirectoryByHash(const Json::Value& dirInfo) {
	std::string relativePath=dirInfo["path"].asString();
	std::string url=dirInfo["url"].asString();
	std::string hashAlgorithm=configManager.ReadHashAlgorithm();

	g_logger<<"[INFO]同步目录: "<<relativePath<<std::endl;

	std::vector<unsigned char> zipData;
	if(!httpClient.DownloadToMemory(url,zipData)) {
		g_logger<<"[ERROR]目录下载失败: "<<relativePath<<std::endl;
		return false;
	}

	std::string tempDir=std::filesystem::temp_directory_path().string()+"/mc_update_temp";
	EnsureDirectoryExists(tempDir);

	if(!ExtractZip(zipData,tempDir)) {
		g_logger<<"[ERROR]解压失败: "<<relativePath<<std::endl;
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
			g_logger<<"[WARN]解压文件中不存在: "<<fileRelativePath<<std::endl;
			dirSuccess=false;
			continue;
		}

		if(!expectedHash.empty()) {
			std::string actualHash=FileHasher::CalculateFileHash(tempFilePath,hashAlgorithm);
			if(actualHash!=expectedHash) {
				g_logger<<"[WARN]解压文件哈希验证失败: "<<fileRelativePath<<std::endl;
				g_logger<<"[WARN]期望: "<<expectedHash<<std::endl;
				g_logger<<"[WARN]实际: "<<actualHash<<std::endl;
			}
			else {
				g_logger<<"[DEBUG]解压文件哈希验证成功: "<<fileRelativePath<<std::endl;
			}
		}

		EnsureDirectoryExists(std::filesystem::path(targetFilePath).parent_path().string());

		try {
			std::filesystem::copy(tempFilePath,targetFilePath,
				std::filesystem::copy_options::overwrite_existing);
			g_logger<<"[INFO]更新文件: "<<fileRelativePath<<std::endl;
		}
		catch(const std::exception& e) {
			g_logger<<"[ERROR]文件复制失败: "<<fileRelativePath<<" - "<<e.what()<<std::endl;
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
		g_logger<<"[WARN]清理临时目录失败: "<<e.what()<<std::endl;
	}

	return dirSuccess;
}

void MinecraftUpdater::CleanupOrphanedFiles(const std::string& directoryPath,const Json::Value& expectedContents) {
	if(!expectedContents.isArray()) {
		g_logger<<"[WARN]警告: 预期内容不是数组，跳过清理孤儿文件"<<std::endl;
		return;
	}
	std::set<std::string> expectedFiles;
	for(const auto& contentInfo:expectedContents) {
		std::string path=contentInfo["path"].asString();
		std::replace(path.begin(),path.end(),'\\','/');
		expectedFiles.insert(path);
	}

	g_logger<<"[DEBUG]期望文件列表:"<<std::endl;
	for(const auto& file:expectedFiles) {
		g_logger<<"[DEBUG]  - "<<file<<std::endl;
	}

	try {
		for(const auto& entry:std::filesystem::recursive_directory_iterator(directoryPath)) {
			if(entry.is_regular_file()) {
				std::string relativePath=std::filesystem::relative(entry.path(),directoryPath).string();
				std::replace(relativePath.begin(),relativePath.end(),'\\','/');

				g_logger<<"[DEBUG]检查文件: "<<relativePath<<std::endl;

				if(expectedFiles.find(relativePath)==expectedFiles.end()) {
					try {
						std::filesystem::remove(entry.path());
						g_logger<<"[INFO]删除孤儿文件: "<<relativePath<<std::endl;
					}
					catch(const std::exception& e) {
						g_logger<<"[ERROR]删除孤儿文件失败: "<<relativePath<<" - "<<e.what()<<std::endl;
					}
				}
				else {
					g_logger<<"[DEBUG]文件在期望列表中，保留: "<<relativePath<<std::endl;
				}
			}
		}
	}
	catch(const std::exception& e) {
		g_logger<<"[ERROR]遍历目录失败: "<<directoryPath<<" - "<<e.what()<<std::endl;
	}
}


void MinecraftUpdater::EnsureDirectoryExists(const std::string& path) {
	try {
		if(path.empty()) {
			g_logger<<"[WARN]警告: 路径为空"<<std::endl;
			return;
		}

		std::filesystem::path dirPath(path);

		dirPath=std::filesystem::absolute(dirPath);

		if(!std::filesystem::exists(dirPath)) {
			g_logger<<"[INFO]创建目录: "<<dirPath.string()<<std::endl;
			bool created=std::filesystem::create_directories(dirPath);

			if(created) {
				g_logger<<"[INFO]目录创建成功: "<<dirPath.string()<<std::endl;
			}
			else {
				g_logger<<"[WARN]目录可能已存在: "<<dirPath.string()<<std::endl;
			}
			if(!std::filesystem::exists(dirPath)) {
				g_logger<<"[ERROR]错误: 目录创建后仍然不存在: "<<dirPath.string()<<std::endl;
			}
			else if(!std::filesystem::is_directory(dirPath)) {
				g_logger<<"[ERROR]错误: 路径存在但不是目录: "<<dirPath.string()<<std::endl;
			}
		}
		else {
			if(!std::filesystem::is_directory(dirPath)) {
				g_logger<<"[ERROR]错误: 路径存在但不是目录: "<<dirPath.string()<<std::endl;
			}
		}
	}
	catch(const std::exception& e) {
		g_logger<<"[ERROR]错误: 创建目录失败: "<<path<<" - "<<e.what()<<std::endl;
		g_logger<<"[ERROR]详细错误: "<<e.what()<<std::endl;
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
			g_logger<<"[INFO]备份目录: "<<filePath<<" -> "<<backupPath<<std::endl;
		}
		else {
			std::filesystem::copy_file(filePath,backupPath,
				std::filesystem::copy_options::overwrite_existing);
			g_logger<<"[INFO]备份文件: "<<filePath<<" -> "<<backupPath<<std::endl;
		}
		return true;
	}
	catch(const std::exception& e) {
		g_logger<<"[WARN]警告: 备份失败: "<<filePath<<" - "<<e.what()<<std::endl;
		g_logger<<"[WARN]警告: 备份失败，但继续更新过程..."<<std::endl;
		return false;
	}
}

bool MinecraftUpdater::ExtractZip(const std::vector<unsigned char>& zipData,const std::string& extractPath) {
	EnsureDirectoryExists(extractPath);

	std::string tempDir=std::filesystem::temp_directory_path().string();
	std::string tempZip=tempDir+"/minecraft_update_temp.zip";

	g_logger<<"[INFO]创建临时文件: "<<tempZip<<std::endl;

	std::ofstream file(tempZip,std::ios::binary);
	if(!file) {
		g_logger<<"[ERROR]错误: 无法创建临时文件: "<<tempZip<<std::endl;
		return false;
	}
	file.write(reinterpret_cast<const char*>(zipData.data()),zipData.size());
	file.close();

	int err=0;
	zip_t* zip=zip_open(tempZip.c_str(),0,&err);
	if(!zip) {
		g_logger<<"[ERROR]错误: 无法打开ZIP文件: "<<tempZip<<"，错误码: "<<err<<std::endl;
		std::filesystem::remove(tempZip);
		return false;
	}

	zip_int64_t numEntries=zip_get_num_entries(zip,0);
	g_logger<<"[INFO]开始解压 "<<numEntries<<" 个文件到: "<<extractPath<<std::endl;

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
			g_logger<<"[ERROR]错误: 无法解压文件: "<<name<<std::endl;
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
			g_logger<<"[INFO]解压文件: "<<name<<std::endl;
		}
		else {
			g_logger<<"[ERROR]错误: 无法创建文件: "<<fullPath<<std::endl;
		}

		zip_fclose(zfile);
	}

	zip_close(zip);
	std::filesystem::remove(tempZip);
	g_logger<<"[INFO]解压完成: "<<extractPath<<std::endl;
	return true;
}

bool MinecraftUpdater::DownloadAndExtract(const std::string& url,const std::string& relativePath) {
	std::vector<unsigned char> zipData;

	g_logger<<"[INFO]下载目录: "<<url<<" -> "<<relativePath<<std::endl;

	if(!httpClient.DownloadToMemory(url,zipData)) {
		g_logger<<"[ERROR]错误: 下载失败: "<<url<<std::endl;
		return false;
	}

	if(zipData.empty()) {
		g_logger<<"[ERROR]错误: 下载的文件为空: "<<url<<std::endl;
		return false;
	}

	std::filesystem::path gameDirPath=std::filesystem::absolute(gameDirectory);
	std::string fullPath=(gameDirPath/relativePath).string();

	g_logger<<"[DEBUG]完整目标路径: "<<fullPath<<std::endl;

	std::filesystem::path pathObj(fullPath);
	if(pathObj.has_parent_path()) {
		EnsureDirectoryExists(pathObj.parent_path().string());
	}

	if(std::filesystem::exists(fullPath)) {
		g_logger<<"[INFO]备份原有目录: "<<fullPath<<std::endl;
		if(!BackupFile(fullPath)) {
			g_logger<<"[WARN]警告: 目录备份失败，但继续更新..."<<std::endl;
		}
	}

	return ExtractZip(zipData,fullPath);
}

bool MinecraftUpdater::SyncFiles(const Json::Value& fileList,bool forceSync) {
	if(!fileList.isArray()) {
		g_logger<<"[ERROR]错误: 文件列表格式错误"<<std::endl;
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
			g_logger<<"[ERROR]错误: 文件信息不完整: path="<<path<<", url="<<url<<std::endl;
			if(forceSync) return false;
			allSuccess=false;
			continue;
		}

		std::string fullPath=gameDirectory+"/"+path;

		if(type=="directory") {
			g_logger<<"[INFO]更新目录: "<<path<<std::endl;
			if(!DownloadAndExtract(url,path)) {
				g_logger<<"[ERROR]错误: 目录更新失败: "<<path<<std::endl;
				if(forceSync) return false;
				allSuccess=false;
			}
		}
		else {
			std::string outputDir=std::filesystem::path(fullPath).parent_path().string();
			EnsureDirectoryExists(outputDir);

			if(std::filesystem::exists(fullPath)) {
				g_logger<<"[INFO]备份原有文件: "<<fullPath<<std::endl;
				if(!BackupFile(fullPath)) {
					g_logger<<"[WARN]警告: 文件备份失败，但继续更新..."<<std::endl;
				}
			}

			g_logger<<"[INFO]下载文件: "<<url<<" -> "<<fullPath<<std::endl;
			if(!httpClient.DownloadFile(url,fullPath)) {
				g_logger<<"[ERROR]错误: 文件更新失败: "<<path<<std::endl;
				if(forceSync) return false;
				allSuccess=false;
			}
			else {
				g_logger<<"[INFO]文件下载成功: "<<path<<std::endl;
			}
		}
	}

	return allSuccess;
}

void MinecraftUpdater::UpdateLocalVersion(const std::string& newVersion) {
	if(configManager.WriteVersion(newVersion)) {
		g_logger<<"[INFO]版本信息已更新为: "<<newVersion<<std::endl;
		hasCachedUpdateInfo=false;
		cachedUpdateInfo=Json::Value();
	}
	else {
		g_logger<<"[ERROR]错误: 更新版本信息失败"<<std::endl;
	}
}