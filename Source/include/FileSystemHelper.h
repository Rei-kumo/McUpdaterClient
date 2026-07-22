#ifndef FILESYSTEMHELPER_H
#define FILESYSTEMHELPER_H
#include <set>
#include <string>
#include <filesystem>
#include <atomic>
#include "Logger.h"
#include <json/json.h>
class FileSystemHelper {
public:
    static void EnsureDirectoryExists(const std::string& path);
    static bool BackupFile(const std::string& filePath);
    static void CleanupOrphanedFiles(const std::string& baseDir,
        const std::string& relativeDir,
        const Json::Value& expectedContents);
    static bool CopySingleFile(const std::string& source,const std::string& target);

    static void CleanupTempExtractDir(const std::string& extractPath);
    static void CleanupTempFiles(const std::string& zipFilePath,
        const std::string& extractPath);
    static bool ValidateExtraction(const std::string& extractPath);
    static std::string SecureCombine(const std::string& baseDir,const std::string& userPath);
    

};
#endif