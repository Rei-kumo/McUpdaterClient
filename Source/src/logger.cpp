#include "Logger.h"
#include <filesystem>

Logger g_logger;

Logger::Logger(): enabled(false),isNewLine(true){}

Logger::~Logger() {
    if(logFile.is_open()) {
        logFile.close();
    }
}

bool Logger::Initialize(const std::string& filename) {
    logFileName=filename;

    std::filesystem::path path(filename);
    std::filesystem::create_directories(path.parent_path());

    logFile.open(filename,std::ios::app);
    if(!logFile.is_open()) {
        std::cerr<<"[ERROR]无法打开日志文件: "<<filename<<std::endl;
        enabled=false;
        return false;
    }

    enabled=true;
    isNewLine=true;
    *this<<"[INFO]=== McUpdaterClient 日志开始 ==="<<std::endl;
    return true;
}

void Logger::Enable(bool enable) {
    enabled=enable;
}

std::string Logger::GetTimestamp() {
    auto now=std::chrono::system_clock::now();
    auto time_t=std::chrono::system_clock::to_time_t(now);
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch())%1000;

    std::tm local_tm;
    localtime_s(&local_tm,&time_t);

    std::stringstream ss;
    ss<<"["<<std::put_time(&local_tm,"%Y-%m-%d %H:%M:%S");
    ss<<"."<<std::setfill('0')<<std::setw(3)<<ms.count()<<"]";
    return ss.str();
}