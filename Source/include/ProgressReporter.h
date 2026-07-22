#ifndef PROGRESSREPORTER_H
#define PROGRESSREPORTER_H

#include <string>
#include <chrono>
#include <mutex>

class ProgressReporter {
public:
    void show(const std::string& operation,long long current,long long total);
    void clear();    
    static std::string FormatBytes(long long bytes);
private:
    std::mutex mtx_;
    std::chrono::steady_clock::time_point lastUpdate_{};
    bool dualMode_=false;
    long long lastCurrent_=-1;
    static constexpr int BAR_WIDTH=40;

};

#endif