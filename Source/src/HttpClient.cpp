#include "HttpClient.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include "Logger.h"

HttpClient::HttpClient(int timeout)
    : curl(nullptr),timeoutSeconds(timeout),downloadTimeoutSeconds(0) {
    curl=curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"MinecraftUpdater/1.0");
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeoutSeconds);
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);
        curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,1024L);
        curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,30L);
        curl_easy_setopt(curl,CURLOPT_TCP_KEEPALIVE,1L);
        curl_easy_setopt(curl,CURLOPT_TCP_KEEPIDLE,10L);
        curl_easy_setopt(curl,CURLOPT_TCP_KEEPINTVL,5L);
    }
}

HttpClient::~HttpClient() {
    if(curl) {
        curl_easy_cleanup(curl);
    }
}

void HttpClient::SetTimeout(int timeout) {
    this->timeoutSeconds=timeout;
    if(curl) {
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeoutSeconds);
    }
}

void HttpClient::SetDownloadTimeout(int timeout) {
    this->downloadTimeoutSeconds=timeout;
    if(curl) {
        if(timeout>0) {
            curl_easy_setopt(curl,CURLOPT_TIMEOUT,downloadTimeoutSeconds);
        }
    }
}

std::string HttpClient::Get(const std::string& url) {
    std::string response;

    if(!curl) {
        g_logger<<"[ERROR] CURL初始化失败"<<std::endl;
        return response;
    }

    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,WriteCallback);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);

    CURLcode res=curl_easy_perform(curl);
    if(res!=CURLE_OK) {
        g_logger<<"[ERROR] HTTP请求失败: "<<curl_easy_strerror(res)<<std::endl;
        return "";
    }

    return response;
}

bool HttpClient::DownloadFile(const std::string& url,const std::string& outputPath) {
    return DownloadFileWithProgress(url,outputPath);
}
bool HttpClient::DownloadFileWithProgress(const std::string& url,const std::string& outputPath,
    DownloadProgressCallback progressCallback,void* userdata) {
    if(!curl) return false;
    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"MinecraftUpdater/1.0");
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);
    if(downloadTimeoutSeconds>0) {
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,downloadTimeoutSeconds);
    }
    else {
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeoutSeconds);
    }
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,1024L);
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,30L);

    FILE* file=nullptr;
    errno_t err=fopen_s(&file,outputPath.c_str(),"wb");

    if(err!=0||!file) {
        g_logger<<"[ERROR] 无法创建文件: "<<outputPath<<std::endl;
        return false;
    }

    DownloadProgressData progressData;
    progressData.callback=progressCallback;
    progressData.userdata=userdata;
    progressData.lastUpdateTime=0;
    progressData.totalBytes=0;
    progressData.downloadedBytes=0;

    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,WriteFileCallback);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,file);

    if(progressCallback) {
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl,CURLOPT_PROGRESSFUNCTION,CurlProgressCallback);
        curl_easy_setopt(curl,CURLOPT_PROGRESSDATA,&progressData);
    }
    else {
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,1L);
    }
    CURLcode res=curl_easy_perform(curl);
    fclose(file);

    if(res!=CURLE_OK) {
        g_logger<<"[ERROR] 下载失败: "<<curl_easy_strerror(res);
        if(res==CURLE_OPERATION_TIMEDOUT) {
            g_logger<<" (超时)";
        }
        g_logger<<std::endl;
        std::remove(outputPath.c_str());
        return false;
    }

    return true;
}

bool HttpClient::DownloadToMemory(const std::string& url,std::vector<unsigned char>& buffer) {
    return DownloadToMemoryWithProgress(url,buffer);
}

bool HttpClient::DownloadToMemoryWithProgress(const std::string& url,std::vector<unsigned char>& buffer,
    DownloadProgressCallback progressCallback,void* userdata) {
    if(!curl) return false;

    buffer.clear();

    DownloadProgressData progressData;
    progressData.callback=progressCallback;
    progressData.userdata=userdata;
    progressData.lastUpdateTime=0;
    progressData.totalBytes=0;
    progressData.downloadedBytes=0;

    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,WriteMemoryCallback);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);

    if(progressCallback) {
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,CurlProgressCallback);
        curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&progressData);
    }
    else {
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,1L);
    }
    if(downloadTimeoutSeconds>0) {
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,downloadTimeoutSeconds);
    }

    CURLcode res=curl_easy_perform(curl);
    if(downloadTimeoutSeconds>0) {
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeoutSeconds);
    }

    if(res!=CURLE_OK) {
        g_logger<<"[ERROR] 下载到内存失败: "<<curl_easy_strerror(res);
        if(res==CURLE_OPERATION_TIMEDOUT) {
            g_logger<<" (超时)";
        }
        g_logger<<std::endl;
        return false;
    }
    return true;
}

int HttpClient::CurlProgressCallback(void* clientp,double dltotal,double dlnow,double ultotal,double ulnow) {
    DownloadProgressData* progressData=static_cast<DownloadProgressData*>(clientp);

    if(progressData->callback) {
        long long totalBytes=static_cast<long long>(dltotal);
        long long downloadedBytes=static_cast<long long>(dlnow);
        progressData->callback(downloadedBytes,totalBytes,progressData->userdata);
    }

    return 0;
}

size_t HttpClient::WriteCallback(void* contents,size_t size,size_t nmemb,std::string* data) {
    size_t totalSize=size*nmemb;
    data->append((char*)contents,totalSize);
    return totalSize;
}

size_t HttpClient::WriteFileCallback(void* contents,size_t size,size_t nmemb,FILE* file) {
    return fwrite(contents,size,nmemb,file);
}

size_t HttpClient::WriteMemoryCallback(void* contents,size_t size,size_t nmemb,std::vector<unsigned char>* buffer) {
    size_t totalSize=size*nmemb;
    size_t oldSize=buffer->size();
    buffer->resize(oldSize+totalSize);
    memcpy(buffer->data()+oldSize,contents,totalSize);
    return totalSize;
}