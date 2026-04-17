#ifndef PROGRESSREPORTER_H
#define PROGRESSREPORTER_H

#include <string>
#include <mutex>

class ProgressReporter {
public:
    void ShowProgressBar(const std::string& operation,long long current,long long total);
    void ClearProgressLine();
    std::string FormatBytes(long long bytes);
    static void DownloadProgressCallback(long long downloaded,long long total,void* userdata);

private:
    std::mutex progressMutex;
};

#endif