#ifndef LOGGER_H
#define LOGGER_H

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

enum class LogLevel
{
    DEBUG =0,
    INFO,
    WARN,
	ERR,
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

        std::filesystem::path path(filename);
        std::filesystem::create_directories(path.parent_path());
        logFile_.open(filename,std::ios::app);
        if(!logFile_.is_open()) {
            std::cerr<<"[ERROR]无法打开日志文件: "<<filename<<std::endl;
            return false;
        }
        initialized_=true;
        logFileName_=filename;
        writelmpl(LogLevel::INFO,"=== McUpdaterClient 日志开始 ===");
        return true;
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
        log(LogLevel::ERR,fmt,std::forward<Args>(args)...);
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
        static const char* levelStr[]={"DEBUG","INFO","WARN","ERR"};
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