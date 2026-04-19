#ifndef SELFUPDATER_H
#define SELFUPDATER_H

#include <string>
#include <filesystem>
#include <atomic>
#include "HttpClient.h"
#include "Logger.h"
#include "FileHasher.h"
#include "ConfigManager.h"

class SelfUpdater {
private:
    HttpClient& httpClient;
    ConfigManager& configManager;
    std::string currentExePath;
    std::string tempExePath;
    std::atomic<bool> downloading;
    std::atomic<long long> downloadedBytes;
    std::atomic<long long> totalBytes;

public:
    SelfUpdater(HttpClient& httpClient,ConfigManager& configManager);

    bool DownloadNewLauncher(const std::string& downloadUrl,
        const std::string& expectedHash="",
        const std::string& expectedVersion="");
    bool ApplyUpdate();
    static std::wstring GetCurrentExePathW();
    long long GetDownloadedBytes() const { return downloadedBytes; }
    long long GetTotalBytes() const { return totalBytes; }
    bool IsDownloading() const { return downloading; }

    static std::string GetCurrentExePath();

private:
    bool TryNormalReplace(const std::wstring& newExe,const std::wstring& targetExe);
    bool RunElevatedReplace(const std::wstring& newExe,const std::wstring& targetExe);
    void CleanupOldBackup(const std::wstring& currentExePath);
    bool LaunchNewProcess(const std::wstring& exePath);
    static std::wstring GetShortPathNameSafe(const std::wstring& longPath);
};

#endif