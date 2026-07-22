#include "FileSystemHelper.h"

void FileSystemHelper::EnsureDirectoryExists(const std::string& path) {
    try {
        if(path.empty()) {
			LOG_WARN("警告: 路径为空");
            return;
        }
            std::error_code ec;

        std::filesystem::path dirPath=std::filesystem::u8path(path);
        dirPath=std::filesystem::absolute(dirPath);

        if(!std::filesystem::exists(dirPath)) {
            bool created=std::filesystem::create_directories(dirPath);
            if(!created&&!std::filesystem::exists(dirPath)) {
                LOG_ERROR("目录创建失败: {}",std::string{dirPath.u8string()});
                return;
            }
            if(!std::filesystem::is_directory(dirPath)) {
                LOG_ERROR("路径存在但不是目录: {}",std::string{dirPath.u8string()});
            }
        }
    }
    catch(const std::filesystem::filesystem_error& e) {
        LOG_ERROR("创建目录失败: {} - 错误码: {} (路径1: {}, 路径2: {})",
            path,e.code().message(),
            e.path1().string(),
            e.path2().string());
    }
    catch(const std::exception& e) {
		LOG_ERROR("创建目录失败: {} - {}",path,e.what());
    }
}

bool FileSystemHelper::BackupFile(const std::string& filePath) {
    std::filesystem::path srcPath=std::filesystem::u8path(filePath);

    if(!std::filesystem::exists(srcPath)) {
        return true;
    }

    std::string backupPathStr=filePath+".backup";
    std::string timestamp=std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::string tempBackupPathStr=backupPathStr+"_"+timestamp;

    std::filesystem::path backupPath=std::filesystem::u8path(backupPathStr);
    std::filesystem::path tempBackupPath=std::filesystem::u8path(tempBackupPathStr);

    try {
        if(std::filesystem::is_directory(srcPath)) {
            std::filesystem::copy(srcPath,tempBackupPath,
                std::filesystem::copy_options::recursive|
                std::filesystem::copy_options::overwrite_existing);
        }
        else {
            std::filesystem::copy_file(srcPath,tempBackupPath,
                std::filesystem::copy_options::overwrite_existing);
        }

        if(std::filesystem::exists(backupPath)) {
            std::filesystem::remove_all(backupPath);
        }
        std::filesystem::rename(tempBackupPath,backupPath);

        LOG_INFO("备份完成: {} -> {}",filePath,backupPathStr);
        return true;
    }
    catch(const std::exception& e) {
        LOG_WARN("备份失败: {} - {}",filePath,e.what());
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

void FileSystemHelper::CleanupOrphanedFiles(const std::string& baseDir,
    const std::string& relativeDir,
    const Json::Value& expectedContents) {
    if(!expectedContents.isArray()) {
		LOG_WARN("预期内容不是数组，跳过清理孤儿文件");
        return;
    }
    std::string fullDirPath;
    try {
        fullDirPath=SecureCombine(baseDir,relativeDir);
    }
    catch(const std::exception& e) {
        LOG_ERROR("清理孤儿文件时路径遍历被阻止: {} - {}", relativeDir, e.what());
        return;
    }

    std::set<std::string> expectedFiles;
    for(const auto& contentInfo:expectedContents) {
        std::string path=contentInfo["path"].asString();
        std::replace(path.begin(),path.end(),'\\','/');
        expectedFiles.insert(path);
    }

    LOG_DEBUG("期望文件列表:");
    for(const auto& file:expectedFiles) {
        LOG_DEBUG("   - {}", file);
    }

    std::filesystem::path fullDirPathObj=std::filesystem::u8path(fullDirPath);

    std::error_code ec;
    auto it=std::filesystem::recursive_directory_iterator(
        fullDirPathObj,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    if(ec) {
        LOG_ERROR("无法打开目录迭代器: {} - {}", fullDirPath, ec.message());
        return;
    }

    const auto end=std::filesystem::recursive_directory_iterator();
    while(it!=end) {
        if(ec) {
            LOG_ERROR("迭代器状态无效: {}", ec.message());
            break;
        }

        const auto& entry=*it;
        if(entry.is_symlink()||entry.is_other()) {
            it.increment(ec);
            continue;
        }

        if(entry.is_regular_file()) {
            std::string relativePath=std::filesystem::relative(entry.path(),fullDirPathObj,ec).generic_u8string();
            if(ec) {
                LOG_ERROR("计算相对路径失败: {} - {}",
                    std::string{entry.path().u8string()},ec.message());
                it.increment(ec);
                continue;
            }

            LOG_DEBUG("检查文件: {}", relativePath);

            if(expectedFiles.find(relativePath)==expectedFiles.end()) {
                std::error_code remove_ec;
                std::filesystem::remove(entry.path(),remove_ec);
                if(!remove_ec) {
                    LOG_INFO("删除孤儿文件: {}", relativePath);
                }
                else {
                    LOG_ERROR("删除孤儿文件失败: {} - {}", relativePath, remove_ec.message());
                }
            }
            else {
                LOG_DEBUG("文件在期望列表中，保留: {}", relativePath);
            }
        }
        it.increment(ec);
        if(ec) {
            LOG_ERROR("迭代目录时出错: {}", ec.message());
            break;
        }
    }
}

bool FileSystemHelper::CopySingleFile(const std::string& source,const std::string& target) {
    std::error_code ec;
    std::filesystem::copy_file(
        std::filesystem::u8path(source),
        std::filesystem::u8path(target),
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
    if(ec) {
        LOG_ERROR("复制文件失败: {} -> {}，错误: {}",source,target,ec.message());
        return false;
    }
    return true;
}

void FileSystemHelper::CleanupTempExtractDir(const std::string& extractPath) {
    LOG_INFO("清理临时解压目录...");
    std::filesystem::path extractPathObj=std::filesystem::u8path(extractPath);
    if(!extractPath.empty()&&std::filesystem::exists(extractPathObj)) {
        try {
            std::string tempDir=std::filesystem::temp_directory_path().generic_u8string();
            std::string extractPathNormalized=extractPath;
            std::replace(extractPathNormalized.begin(),extractPathNormalized.end(),'\\','/');

            if(extractPathNormalized.size()>tempDir.size()&&
                extractPathNormalized.compare(0,tempDir.size(),tempDir)==0) {
                if(extractPathNormalized[tempDir.size()]=='/') {
                    std::filesystem::remove_all(extractPathObj);
                    LOG_INFO("已清理临时解压目录: {}",extractPath);
                    return;
                }
            }
            else {
                LOG_INFO("保留非临时目录: {}",extractPath);
            }
        }
        catch(const std::exception& e) {
            LOG_WARN("无法清理解压目录: {}",e.what());
        }
    }
    else {
        LOG_INFO("解压目录不存在或为空，无需清理");
    }
}

void FileSystemHelper::CleanupTempFiles(const std::string& zipFilePath,
    const std::string& extractPath) {
    LOG_INFO("清理所有临时文件...");

    std::filesystem::path zipPathObj=std::filesystem::u8path(zipFilePath);

    if(!zipFilePath.empty()&&std::filesystem::exists(zipPathObj)) {
        try {
            std::filesystem::remove(zipPathObj);
            LOG_INFO("已清理临时 ZIP 文件: {}",zipFilePath);
        }
        catch(const std::exception& e) {
            LOG_WARN("无法删除临时 ZIP 文件: {}",e.what());
        }
    }
    CleanupTempExtractDir(extractPath);
}

bool FileSystemHelper::ValidateExtraction(const std::string& extractPath) {
    LOG_INFO("验证解压结果...");
    std::filesystem::path extractPathObj=std::filesystem::u8path(extractPath);

    if(!std::filesystem::exists(extractPathObj)) {
        LOG_ERROR("解压目录不存在: {}",std::string{extractPathObj.u8string()});
        return false;
    }

    try {
        int fileCount=0;
        int dirCount=0;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(extractPathObj,std::filesystem::directory_options::skip_permission_denied)){
            if(entry.is_directory()) {
                dirCount++;
            }
            else if(entry.is_regular_file()) {
                fileCount++;
                try {
                    auto fileSize=std::filesystem::file_size(entry.path());
                    if(fileSize==0) {
                        LOG_WARN("发现空文件: {}",std::string{entry.path().u8string()});
                    }
                }
                catch(...) {
                }
            }
        }

		LOG_INFO("解压验证: 总共 {} 个条目 ({} 个文件, {} 个目录)",fileCount+dirCount,fileCount,dirCount);

        if(fileCount==0&&dirCount==0) {
            LOG_WARN("解压目录为空，可能解压失败");
            return false;
        }

        if(fileCount+dirCount<3) {
            LOG_WARN("解压条目数量较少，可能未完全解压");
        }

        return true;
    }
    catch(const std::exception& e) {
        LOG_ERROR("验证解压结果失败: {}", e.what());
        return false;
    }
}
std::string FileSystemHelper::SecureCombine(const std::string& baseDir,const std::string& userPath) {
    if(baseDir.empty()) {
        throw std::runtime_error("SecureCombine: base directory is empty");
    }

    std::error_code ec;
    std::filesystem::path base=std::filesystem::absolute(std::filesystem::u8path(baseDir),ec);
    if(ec) {
        throw std::runtime_error("SecureCombine: cannot resolve base path: "+baseDir);
    }
    base=std::filesystem::weakly_canonical(base,ec);
    if(ec) {
        base=std::filesystem::absolute(std::filesystem::u8path(baseDir),ec);
        if(ec) {
            throw std::runtime_error("SecureCombine: base path does not exist and cannot be resolved: "+baseDir);
        }
    }
    std::string cleanUserPath=userPath;
    std::replace(cleanUserPath.begin(),cleanUserPath.end(),'\\','/');
    std::filesystem::path full=base/std::filesystem::u8path(cleanUserPath);
    full=std::filesystem::weakly_canonical(full,ec);
    if(ec) {
        std::filesystem::path parent=full.parent_path();
        parent=std::filesystem::weakly_canonical(parent,ec);
        if(ec) {
            throw std::runtime_error("SecureCombine: cannot resolve path: "+userPath);
        }
        full=parent/full.filename();
    }
    std::string fullStr=full.generic_u8string();
    std::string baseStr=base.generic_u8string();
    if(baseStr.back()!='/') {
        baseStr+='/';
    }
    if(fullStr.back()!='/') {
        fullStr+='/';
    }

    if(fullStr.size()<baseStr.size()||
        fullStr.compare(0,baseStr.size(),baseStr)!=0) {
        throw std::runtime_error("Path traversal detected: "+userPath);
    }
    if(fullStr.find("/../")!=std::string::npos) {
        throw std::runtime_error("Path traversal attempt (..) in resolved path: "+userPath);
    }
    std::string result=full.generic_u8string();
    if(!result.empty()&&result.back()=='/') result.pop_back();
    return result;
}