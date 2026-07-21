#include "ProgressReporter.h"
#include <iostream>
#include <iomanip>
#include <sstream>

void ProgressReporter::show(const std::string& operation,
    long long current,long long total) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto now=std::chrono::steady_clock::now();
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-lastUpdate_).count();

    bool shouldUpdate=(elapsed>=200);
    if(!shouldUpdate&&total>0) {
        float prev=static_cast<float>(lastCurrent_)/total;
        float curr=static_cast<float>(current)/total;
        if(std::abs(curr-prev)>=0.01f) shouldUpdate=true;
    }
    else if(!shouldUpdate&&current!=lastCurrent_) {
        shouldUpdate=true;
    }

    if(!shouldUpdate&&current<total) return;

    lastUpdate_=now;
    lastCurrent_=current;

    std::ostringstream line;

    line<<operation<<": ";

    if(total>0) {
        float progress=(current<=0)?0.0f:static_cast<float>(current)/total;
        if(progress>1.0f) progress=1.0f;
        int pos=static_cast<int>(BAR_WIDTH*progress);

        line<<'['
            <<std::string(pos,'=')
            <<'>'
            <<std::string(BAR_WIDTH-pos-1,' ')
            <<"] ";

        line<<std::fixed<<std::setprecision(1)<<(progress*100.0f)<<"%  "
            <<FormatBytes(current)<<'/'<<FormatBytes(total);
    }
    else {
        static int dots=0;
        dots=(dots+1)%4;
        line<<"已处理 "<<FormatBytes(current)<<std::string(dots,'.');
    }

    std::cout<<"\r"<<line.str();
    std::cout<<std::string(80-std::min<size_t>(80,line.str().size()),' ');
    std::cout.flush();
}

void ProgressReporter::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::cout<<"\r"<<std::string(80,' ')<<"\r";
    std::cout.flush();
}

std::string ProgressReporter::FormatBytes(long long bytes) {
    if(bytes<0) bytes=0;
    const char* units[]={"B","KB","MB","GB","TB"};
    int i=0;
    double size=static_cast<double>(bytes);
    while(size>=1024.0&&i<4) {
        size/=1024.0;
        ++i;
    }
    std::ostringstream oss;
    if(i==0)
        oss<<static_cast<long long>(size)<<' '<<units[i];
    else
        oss<<std::fixed<<std::setprecision(1)<<size<<' '<<units[i];
    return oss.str();
}