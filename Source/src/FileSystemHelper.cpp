#include "FileSystemHelper.h"
#include <set>
#include "SelfUpdater.h"
void FileSystemHelper::EnsureDirectoryExists(const std::string& path) {
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
    catch(const std::filesystem::filesystem_error& e) {
        g_logger<<"[ERROR] 创建目录失败: "<<path
            <<" - 错误码: "<<e.code().message()
            <<" (路径1: "<<e.path1()<<", 路径2: "<<e.path2()<<")"<<std::endl;
    }
    catch(const std::exception& e) {
        g_logger<<"[ERROR] 创建目录失败: "<<path<<" - "<<e.what()<<std::endl;
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
void FileSystemHelper::CleanupOrphanedFiles(const std::string& directoryPath,const Json::Value& expectedContents) {
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
    std::error_code ec;
    auto it=std::filesystem::recursive_directory_iterator(directoryPath,ec);
    if(ec) {
        g_logger<<"[ERROR] 无法打开目录迭代器: "<<directoryPath<<" - "<<ec.message()<<std::endl;
        return;
    }

    const auto end=std::filesystem::recursive_directory_iterator();
    while(it!=end) {
        if(ec) {
            g_logger<<"[ERROR] 迭代器状态无效: "<<ec.message()<<std::endl;
            break;
        }

        const auto& entry=*it;
        if(entry.is_regular_file()) {
            std::string relativePath=std::filesystem::relative(entry.path(),directoryPath,ec).string();
            if(ec) {
                g_logger<<"[ERROR] 计算相对路径失败: "<<entry.path().string()<<" - "<<ec.message()<<std::endl;
                it.increment(ec);
                continue;
            }
            std::replace(relativePath.begin(),relativePath.end(),'\\','/');

            g_logger<<"[DEBUG] 检查文件: "<<relativePath<<std::endl;

            if(expectedFiles.find(relativePath)==expectedFiles.end()) {
                std::error_code remove_ec;
                std::filesystem::remove(entry.path(),remove_ec);
                if(!remove_ec) {
                    g_logger<<"[INFO] 删除孤儿文件: "<<relativePath<<std::endl;
                }
                else {
                    g_logger<<"[ERROR] 删除孤儿文件失败: "<<relativePath<<" - "<<remove_ec.message()<<std::endl;
                }
            }
            else {
                g_logger<<"[DEBUG] 文件在期望列表中，保留: "<<relativePath<<std::endl;
            }
        }
        it.increment(ec);
        if(ec) {
            g_logger<<"[ERROR] 迭代目录时出错: "<<ec.message()<<std::endl;
            break;
        }
    }
}
std::wstring FileSystemHelper::Utf8ToWide(const std::string& utf8Str) {
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

std::string FileSystemHelper::WideToUtf8(const std::wstring& wideStr) {
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
bool FileSystemHelper::CopyFileWithUnicode(const std::wstring& sourcePath,const std::wstring& targetPath) {
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
void FileSystemHelper::CleanupTempExtractDir(const std::string& extractPath) {
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

void FileSystemHelper::CleanupTempFiles(const std::string& zipFilePath,const std::string& extractPath) {
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

bool FileSystemHelper::ValidateExtraction(const std::string& extractPath) {
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