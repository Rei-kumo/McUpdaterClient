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
    void CancelUpdate();

    long long GetDownloadedBytes() const { return downloadedBytes; }
    long long GetTotalBytes() const { return totalBytes; }
    bool IsDownloading() const { return downloading; }

    static std::string GetCurrentExePath();

private:
    static size_t DownloadProgressCallback(void* ptr,size_t size,size_t nmemb,void* userdata);
};

#endif