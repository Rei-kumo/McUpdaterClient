#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>

class Logger {
private:
    std::ofstream logFile;
    std::string logFileName;
    bool enabled;
    bool isNewLine;

public:
    Logger();
    ~Logger();

    bool Initialize(const std::string& filename);
    void Enable(bool enable);

    template<typename T>
    Logger& operator<<(const T& message) {
        std::ostringstream oss;
        oss<<message;
        std::string msgStr=oss.str();

        if(enabled&&logFile.is_open()) {
            if(isNewLine) {
                logFile<<GetTimestamp()<<" "<<msgStr;
                isNewLine=false;
            }
            else {
                logFile<<msgStr;
            }
            logFile.flush();
        }

        std::cout<<msgStr;

        return *this;
    }

    Logger& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if(enabled&&logFile.is_open()) {
            manip(logFile);
            logFile.flush();
        }

        if(manip==static_cast<std::ostream&(*)(std::ostream&)>(std::endl)) {
            isNewLine=true;
        }

        manip(std::cout);
        return *this;
    }

private:
    std::string GetTimestamp();
};

extern Logger g_logger;

#endif