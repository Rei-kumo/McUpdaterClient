#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <string>
#include <vector>
#include <functional>
#include <curl/curl.h>

class HttpClient {
public:
    using DownloadProgressCallback=std::function<void(long long downloaded,long long total,void* userdata)>;

    HttpClient(int timeout=60);
    ~HttpClient();

    std::string Get(const std::string& url);
    bool DownloadFile(const std::string& url,const std::string& outputPath);
    bool DownloadFileWithProgress(const std::string& url,const std::string& outputPath,
        DownloadProgressCallback progressCallback=nullptr,void* userdata=nullptr);
    bool DownloadToMemory(const std::string& url,std::vector<unsigned char>& buffer);
    bool DownloadToMemoryWithProgress(const std::string& url,std::vector<unsigned char>& buffer,
        DownloadProgressCallback progressCallback=nullptr,void* userdata=nullptr);
    void SetTimeout(int timeout);
    void SetDownloadTimeout(int timeout);

private:
    static size_t WriteCallback(void* contents,size_t size,size_t nmemb,std::string* data);
    static size_t WriteFileCallback(void* contents,size_t size,size_t nmemb,FILE* file);
    static size_t WriteMemoryCallback(void* contents,size_t size,size_t nmemb,std::vector<unsigned char>* buffer);
    static int CurlProgressCallback(void* clientp,double dltotal,double dlnow,double ultotal,double ulnow);

    struct DownloadProgressData {
        DownloadProgressCallback callback;
        void* userdata;
        long long lastUpdateTime;
        long long totalBytes;
        long long downloadedBytes;
    };

    CURL* curl;
    int timeoutSeconds;
    int downloadTimeoutSeconds;
};

#endif