#include "ProgressReporter.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <zip.h>
#include <regex>
#include <cmath>
#include <set>
#include "SelfUpdater.h"
#include <thread>
#include <iomanip>
#include <sstream>
#include <queue>
#include <map>
#include <algorithm>
#include <mutex>
#include <memory>
#include "FileHasher.h"
#include <fcntl.h>
#include <io.h>
#include <windows.h>
void ProgressReporter::ShowProgressBar(const std::string& operation,long long current,long long total) {
    static std::mutex progressMutex;
    std::lock_guard<std::mutex> lock(progressMutex);

    static auto lastUpdateTime=std::chrono::steady_clock::now();
    static long long lastCurrent=0;
    auto now=std::chrono::steady_clock::now();
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-lastUpdateTime).count();

    bool shouldUpdate=false;

    if(elapsed>=200) {
        shouldUpdate=true;
    }
    else if(total>0) {
        float lastProgress=static_cast<float>(lastCurrent)/total;
        float currentProgress=static_cast<float>(current)/total;
        if(fabs(currentProgress-lastProgress)>=0.01f) {
            shouldUpdate=true;
        }
    }
    else if(current!=lastCurrent) {
        shouldUpdate=true;
    }

    if(!shouldUpdate&&current<total) {
        return;
    }

    lastUpdateTime=now;
    lastCurrent=current;


    std::cout<<"\r  ";

    const int barWidth=40;

    if(total<=0) {
        static int dotCount=0;
        dotCount=(dotCount+1)%4;
        std::string dots(dotCount,'.');

        std::string currentStr=FormatBytes(current);

        std::string line="进度: "+currentStr+" 已下载"+dots;

        if(line.length()<60) {
            line.append(60-line.length(),' ');
        }

        std::cout<<line;
    }
    else {

        float progress=static_cast<float>(current)/total;
        if(progress<0.0f) progress=0.0f;
        if(progress>1.0f) progress=1.0f;

        int pos=static_cast<int>(barWidth*progress);


        std::string bar="[";
        for(int i=0; i<barWidth; ++i) {
            if(i<pos) bar+="=";
            else if(i==pos) bar+=">";
            else bar+=" ";
        }
        bar+="]";

        std::stringstream ss;
        ss<<"进度: "<<bar<<" "
            <<std::fixed<<std::setprecision(1)<<(progress*100.0)<<"%";

        ss<<" ("<<FormatBytes(current)<<"/"<<FormatBytes(total)<<")";

        std::string line=ss.str();

        if(line.length()<70) {
            line.append(70-line.length(),' ');
        }

        std::cout<<line;
    }

    std::cout.flush();
}
void ProgressReporter::ClearProgressLine() {
    const int consoleWidth=80;
    std::cout<<"\r";
    for(int i=0; i<consoleWidth; i++) {
        std::cout<<" ";
    }
    std::cout<<"\r";
    std::cout.flush();
}
std::string ProgressReporter::FormatBytes(long long bytes) {
    if(bytes<0) bytes=0;

    const char* units[]={"B","KB","MB","GB","TB"};
    int unitIndex=0;
    double size=static_cast<double>(bytes);

    while(size>=1024.0&&unitIndex<4) {
        size/=1024.0;
        unitIndex++;
    }

    std::stringstream ss;

    if(bytes==0) {
        ss<<"0.0 B";
    }
    else if(bytes==1) {
        ss<<"1.0 B";
    }
    else {
        if(unitIndex==0) {
            ss<<std::fixed<<std::setprecision(0)<<size<<" "<<units[unitIndex];
        }
        else if(size<10.0) {
            ss<<std::fixed<<std::setprecision(1)<<size<<" "<<units[unitIndex];
        }
        else {
            ss<<std::fixed<<std::setprecision(0)<<size<<" "<<units[unitIndex];
        }
    }

    return ss.str();
}
void ProgressReporter::DownloadProgressCallback(long long downloaded,long long total,void* userdata) {
    ProgressReporter* updater=static_cast<ProgressReporter*>(userdata);
    if(updater) {
        updater->ShowProgressBar("下载",downloaded,total);
    }
}
static void ClearConsoleLine() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hConsole=GetStdHandle(STD_OUTPUT_HANDLE);

    if(GetConsoleScreenBufferInfo(hConsole,&csbi)) {
        int columns=csbi.srWindow.Right-csbi.srWindow.Left+1;
        DWORD charsWritten;

        COORD cursorPos=csbi.dwCursorPosition;
        cursorPos.X=0;

        FillConsoleOutputCharacter(hConsole,' ',columns,cursorPos,&charsWritten);
        SetConsoleCursorPosition(hConsole,cursorPos);
    }
}