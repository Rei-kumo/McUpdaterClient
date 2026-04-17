#ifndef FILESYSTEMHELPER_H
#define FILESYSTEMHELPER_H
#include "SelfUpdater.h"
class FileSystemHelper {
public:
    void EnsureDirectoryExists(const std::string& path);
    bool BackupFile(const std::string& filePath);           
    void CleanupOrphanedFiles(const std::string& directoryPath,
        const Json::Value& expectedContents);
    bool CopyFileWithUnicode(const std::wstring& sourcePath,
        const std::wstring& targetPath);

    static std::wstring Utf8ToWide(const std::string& utf8Str);
    static std::string WideToUtf8(const std::wstring& wideStr);

    void CleanupTempExtractDir(const std::string& extractPath);
    void CleanupTempFiles(const std::string& zipFilePath,
        const std::string& extractPath);
    bool ValidateExtraction(const std::string& extractPath);
};
#endif