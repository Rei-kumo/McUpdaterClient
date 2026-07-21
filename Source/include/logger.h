#ifndef LOGGER_H
#define LOGGER_H

#ifdef ERROR
#undef ERROR
#endif

#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <chrono>
#include <fmt/core.h>
#include <filesystem>
#include <utility>
#include <iostream>
#include <vector>
#include <algorithm>

enum class LogLevel
{
    DEBUG =0,
    INFO,
    WARN,
    ERROR,
    OFF
};

class Logger {
public:
    //定义一个全局访问点，这样子用户访问的就是一个唯一的地方,迈耶斯单例
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    bool Initialize(const std::string& filename){
        std::lock_guard<std::mutex> lock(mtx_);
        if(initialized_)return true;

        std::filesystem::path inputPath(filename);
		std::filesystem::path logDir = inputPath.has_parent_path()?inputPath.parent_path():".";
        std::string latestFile=(logDir/"latest.log").string();

        std::filesystem::create_directories(logDir);

        if(std::filesystem::exists(latestFile)) {
            auto now=std::chrono::system_clock::now();
            auto tt=std::chrono::system_clock::to_time_t(now);
            std::tm tm;
#if defined(_WIN32) || defined(_WIN64)
            localtime_s(&tm,&tt);
#else
            localtime_r(&tt,&tm);
#endif
            std::ostringstream crashNameStream;
            crashNameStream<<logDir.string()<<"/crash_"<<std::put_time(&tm,"%Y%m%d_%H%M%S")<<".log";
            std::string crashName=crashNameStream.str();
            std::filesystem::rename(latestFile,crashName);
            std::cerr<<"[WARN] 检测到上次运行异常退出，日志已保存为 "<<crashName<<std::endl;
        }

        logFile_.open(latestFile,std::ios::trunc);
        if(!logFile_.is_open()) {
            std::cerr<<"[ERROR]无法打开日志文件: "<<latestFile<<std::endl;
            return false;
        }
        initialized_=true;
        logFileName_=latestFile;
        writelmpl(LogLevel::INFO,"=== McUpdaterClient 日志开始 ===");
        CleanOldLogs(logDir.string(),5);
        return true;
    }

    void Shutdown(size_t keepVersions=5) {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!initialized_) return;
        if(logFile_.is_open()) {
            logFile_.close();
        }

        std::filesystem::path latestPath(logFileName_);
        if(std::filesystem::exists(latestPath)) {
            auto now=std::chrono::system_clock::now();
            auto tt=std::chrono::system_clock::to_time_t(now);
            std::tm tm;
#if defined(_WIN32) || defined(_WIN64)
            localtime_s(&tm,&tt);
#else
            localtime_r(&tt,&tm);
#endif
            std::ostringstream archiveName;
            archiveName<<"updater_"<<std::put_time(&tm,"%Y%m%d_%H%M%S")<<".log";
            std::filesystem::path archivePath=latestPath.parent_path()/archiveName.str();
            std::filesystem::rename(latestPath,archivePath);

            CleanOldLogs(latestPath.parent_path().string(),keepVersions);
        }
        initialized_=false;
    }

    void SetLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mtx_);
        minLevel_=level;
    }

    void Enable(bool enable) {
        SetLevel(enable?LogLevel::DEBUG:LogLevel::OFF);
    }

    template<typename... Args>
    void log(LogLevel level,fmt::format_string<Args...> fmt,Args&&... args) {
        if(level<minLevel_) return;
        std::string content=fmt::format(fmt,std::forward<Args>(args)...);
        write(level,content);
    }

    template<typename... Args>
    void info(fmt::format_string<Args...> fmt,Args&&... args) {
        log(LogLevel::INFO,fmt,std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(fmt::format_string<Args...> fmt,Args&&... args) {
        log(LogLevel::DEBUG,fmt,std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(fmt::format_string<Args...> fmt,Args&&... args) {
        log(LogLevel::WARN,fmt,std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(fmt::format_string<Args...> fmt,Args&&... args) {
        log(LogLevel::ERROR,fmt,std::forward<Args>(args)...);
    }

private:
    Logger()=default;
    ~Logger() {
        if(logFile_.is_open()) {
            logFile_.close();
        }
    }

    Logger(const Logger&)=delete;
    Logger& operator=(const Logger&)=delete;

    void write(LogLevel level,const std::string& content) {
        if(!initialized_)return;
        std::lock_guard<std::mutex> lock(mtx_);
        writelmpl(level,content);
    }

    void writelmpl(LogLevel level,const std::string& content) {
        static const char* levelStr[]={"DEBUG","INFO","WARN","ERROR"};
        std::string timestamp=GetTimestamp();
        std::string line=timestamp+" ["+levelStr[static_cast<int>(level)]+"] "+content;
        logFile_<<line<<std::endl;
        std::cout<<line<<std::endl;
    }

    static std::string GetTimestamp() {
        auto now=std::chrono::system_clock::now();

        auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())%1000;
        std::time_t tt=std::chrono::system_clock::to_time_t(now);
        std::tm tm;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tm,&tt);
#else
        localtime_r(&tt,&tm);
#endif
        std::ostringstream ss;
        ss<<std::put_time(&tm,"%Y-%m-%d %H:%M:%S")<<"."<<std::setfill('0')<<std::setw(3)<<ms.count();
        return "["+ss.str()+"]";
    }

    void CleanOldLogs(const std::string& directory,size_t keepCount) {
        namespace fs=std::filesystem;
        std::vector<fs::path> logFiles;
        for(auto& entry:fs::directory_iterator(directory)) {
            if(!entry.is_regular_file()) continue;
            std::string fname=entry.path().filename().string();
            // 匹配 updater_*.log 或 crash_*.log,两个的计数算在一起
            if((fname.find("updater_")==0||fname.find("crash_")==0)&&
                fname.size()>4&&fname.compare(fname.size()-4,4,".log")==0) {
                logFiles.push_back(entry.path());
            }
        }
        if(logFiles.size()<=keepCount) return;

        std::sort(logFiles.begin(),logFiles.end(),[](const fs::path& a,const fs::path& b) {
            return fs::last_write_time(a)>fs::last_write_time(b);
            });
        for(size_t i=keepCount; i<logFiles.size(); ++i) {
            fs::remove(logFiles[i]);
        }
    }

    std::ofstream logFile_;
    std::string logFileName_;
    bool initialized_=false;
    LogLevel minLevel_=LogLevel::DEBUG;
    std::mutex mtx_;
};
#define LOG_DEBUG(fmt, ...)   Logger::Instance().debug(FMT_STRING(fmt), ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    Logger::Instance().info(FMT_STRING(fmt), ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    Logger::Instance().warn(FMT_STRING(fmt), ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   Logger::Instance().error(FMT_STRING(fmt), ##__VA_ARGS__)

#endif