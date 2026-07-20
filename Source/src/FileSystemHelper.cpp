#include "FileSystemHelper.h"
#include <set>
#include "SelfUpdater.h"
void FileSystemHelper::EnsureDirectoryExists(const std::string& path) {
    try {
        if(path.empty()) {
			LOG_WARN("警告: 路径为空");
            return;
        }

        std::filesystem::path dirPath(path);
        dirPath=std::filesystem::absolute(dirPath);

        if(!std::filesystem::exists(dirPath)) {
            LOG_INFO("创建目录: {}", dirPath.string());
            bool created=std::filesystem::create_directories(dirPath);

            if(created) {
                LOG_INFO("目录创建成功: {}", dirPath.string());
            }
            else {
                LOG_WARN("目录可能已存在: {}", dirPath.string());
            }
            if(!std::filesystem::exists(dirPath)) {
                LOG_ERROR("错误: 目录创建后仍然不存在: {}", dirPath.string());
            }
            else if(!std::filesystem::is_directory(dirPath)) {
                LOG_ERROR("错误: 路径存在但不是目录: {}", dirPath.string());
            }
        }
        else {
            if(!std::filesystem::is_directory(dirPath)) {
                LOG_ERROR("错误: 路径存在但不是目录: {}", dirPath.string());
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

        LOG_INFO("备份完成: {} -> {}", filePath, backupPath);
        return true;
    }
    catch(const std::exception& e) {
        LOG_WARN("备份失败: {} - {}", filePath, e.what());
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

    std::error_code ec;
    auto it=std::filesystem::recursive_directory_iterator(
        fullDirPath,
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
            std::string relativePath=std::filesystem::relative(entry.path(),fullDirPath,ec).string();
            if(ec) {
                LOG_ERROR("计算相对路径失败: {} - {}", entry.path().string(), ec.message());
                it.increment(ec);
                continue;
            }
            std::replace(relativePath.begin(),relativePath.end(),'\\','/');

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
std::wstring FileSystemHelper::Utf8ToWide(const std::string& utf8Str) {
    if(utf8Str.empty()) return L"";

    int requiredSize=MultiByteToWideChar(CP_UTF8,0,utf8Str.c_str(),-1,NULL,0);
    if(requiredSize==0) {
        DWORD error=GetLastError();
        LOG_ERROR("MultiByteToWideChar failed, error: {}", error);
        return L"";
    }

    std::wstring wideStr(requiredSize,0);
    if(MultiByteToWideChar(CP_UTF8,0,utf8Str.c_str(),-1,&wideStr[0],requiredSize)==0) {
        DWORD error=GetLastError();
        LOG_ERROR("MultiByteToWideChar failed, error: {}", error);
        return L"";
    }
    wideStr.pop_back();
    return wideStr;
}

std::string FileSystemHelper::WideToUtf8(const std::wstring& wideStr) {
    if(wideStr.empty()) return "";

    int requiredSize=WideCharToMultiByte(CP_UTF8,0,wideStr.c_str(),-1,NULL,0,NULL,NULL);
    if(requiredSize==0) {
        DWORD error=GetLastError();
        LOG_ERROR("WideCharToMultiByte failed, error: {}", error);
        return "";
    }

    std::string utf8Str(requiredSize,0);
    if(WideCharToMultiByte(CP_UTF8,0,wideStr.c_str(),-1,&utf8Str[0],requiredSize,NULL,NULL)==0) {
        DWORD error=GetLastError();
        LOG_ERROR("WideCharToMultiByte failed, error: {}", error);
        return "";
    }
    utf8Str.pop_back();
    return utf8Str;
}
bool FileSystemHelper::CopyFileWithUnicode(const std::wstring& sourcePath,const std::wstring& targetPath) {
    BOOL result=CopyFileW(sourcePath.c_str(),targetPath.c_str(),FALSE);

    if(!result) {
        DWORD error=GetLastError();

        if(error==ERROR_ACCESS_DENIED) {
            DeleteFileW(targetPath.c_str());
            result=CopyFileW(sourcePath.c_str(),targetPath.c_str(),FALSE);

            if(!result) {
                error=GetLastError();
                LOG_ERROR("复制文件失败 (删除后重试): {} -> {}，错误码: {}", WideToUtf8(sourcePath), WideToUtf8(targetPath), error);
                return false;
            }
        }
        else {
            LOG_ERROR("复制文件失败: {} -> {}，错误码: {}", WideToUtf8(sourcePath), WideToUtf8(targetPath), error);
            return false;
        }
    }

    return true;
}
void FileSystemHelper::CleanupTempExtractDir(const std::string& extractPath) {
    LOG_INFO("清理临时解压目录...");
    if(!extractPath.empty()&&std::filesystem::exists(extractPath)) {
        try {
            std::string tempDir=std::filesystem::temp_directory_path().string();
            if(extractPath.find(tempDir)==0) {
                std::filesystem::remove_all(extractPath);
                LOG_INFO("已清理临时解压目录: {}", extractPath);
            }
            else {
                LOG_INFO("保留非临时目录: {}", extractPath);
            }
        }
        catch(const std::exception& e) {
            LOG_WARN("无法清理解压目录: {}", e.what());
        }
    }
    else {
        LOG_INFO("解压目录不存在或为空，无需清理");
    }
}

void FileSystemHelper::CleanupTempFiles(const std::string& zipFilePath,const std::string& extractPath) {
    LOG_INFO("清理所有临时文件...");
    if(!zipFilePath.empty()&&std::filesystem::exists(zipFilePath)) {
        try {
            std::filesystem::remove(zipFilePath);
            LOG_INFO("已清理临时 ZIP 文件: {}", zipFilePath);
        }
        catch(const std::exception& e) {
            LOG_WARN("无法删除临时 ZIP 文件: {}", e.what());
        }
    }
    CleanupTempExtractDir(extractPath);
}

bool FileSystemHelper::ValidateExtraction(const std::string& extractPath) {
    LOG_INFO("验证解压结果...");

    if(!std::filesystem::exists(extractPath)) {
        LOG_ERROR("解压目录不存在: {}", extractPath);
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
                        LOG_WARN("发现空文件: {}", entry.path().string());
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
    std::filesystem::path base=std::filesystem::absolute(baseDir,ec);
    if(ec) {
        throw std::runtime_error("SecureCombine: cannot resolve base path: "+baseDir);
    }
    base=std::filesystem::weakly_canonical(base,ec);
    if(ec) {
        base=std::filesystem::absolute(baseDir,ec);
        if(ec) {
            throw std::runtime_error("SecureCombine: base path does not exist and cannot be resolved: "+baseDir);
        }
    }
    std::string cleanUserPath=userPath;
    std::replace(cleanUserPath.begin(),cleanUserPath.end(),'\\','/');
    std::filesystem::path full=base/cleanUserPath;
    full=std::filesystem::weakly_canonical(full,ec);
    if(ec) {
        std::filesystem::path parent=full.parent_path();
        parent=std::filesystem::weakly_canonical(parent,ec);
        if(ec) {
            throw std::runtime_error("SecureCombine: cannot resolve path: "+userPath);
        }
        full=parent/full.filename();
    }
    std::string fullStr=full.string();
    std::string baseStr=base.string();
    std::replace(fullStr.begin(),fullStr.end(),'\\','/');
    std::replace(baseStr.begin(),baseStr.end(),'\\','/');
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
    if(fullStr.find("/../")!=std::string::npos||
        fullStr.find("\\..\\")!=std::string::npos) {
        throw std::runtime_error("Path traversal attempt (..) in resolved path: "+userPath);
    }
    std::string result=full.string();
    if(!result.empty()&&result.back()=='/') result.pop_back();
    return result;
}

std::wstring FileSystemHelper::SecureCombineW(const std::wstring& baseDir,const std::wstring& userPath) {
    if(baseDir.empty()) {
        throw std::runtime_error("SecureCombineW: base directory is empty");
    }

    std::error_code ec;
    std::filesystem::path base(baseDir);
    base=std::filesystem::absolute(base,ec);
    if(ec) {
        throw std::runtime_error("SecureCombineW: cannot resolve base path");
    }
    base=std::filesystem::weakly_canonical(base,ec);
    if(ec) {
        base=std::filesystem::absolute(baseDir,ec);
        if(ec) {
            throw std::runtime_error("SecureCombineW: base path does not exist");
        }
    }

    std::wstring cleanUserPath=userPath;
    std::replace(cleanUserPath.begin(),cleanUserPath.end(),L'\\',L'/');

    std::filesystem::path full=base/cleanUserPath;
    full=std::filesystem::weakly_canonical(full,ec);
    if(ec) {
        std::filesystem::path parent=full.parent_path();
        parent=std::filesystem::weakly_canonical(parent,ec);
        if(ec) {
            throw std::runtime_error("SecureCombineW: cannot resolve path");
        }
        full=parent/full.filename();
    }

    std::wstring fullStr=full.wstring();
    std::wstring baseStr=base.wstring();

    std::replace(fullStr.begin(),fullStr.end(),L'\\',L'/');
    std::replace(baseStr.begin(),baseStr.end(),L'\\',L'/');

    if(baseStr.back()!=L'/') baseStr+=L'/';
    if(fullStr.back()!=L'/') fullStr+=L'/';

    if(fullStr.size()<baseStr.size()||
        fullStr.compare(0,baseStr.size(),baseStr)!=0) {
        throw std::runtime_error("Path traversal detected (wide)");
    }

    if(fullStr.find(L"/../")!=std::wstring::npos) {
        throw std::runtime_error("Path traversal attempt (..) in resolved wide path");
    }

    std::wstring result=full.wstring();
    if(!result.empty()&&result.back()==L'/') result.pop_back();
    return result;
}