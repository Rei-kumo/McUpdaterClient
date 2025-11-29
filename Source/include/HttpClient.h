#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <string>
#include <vector>
#include <curl/curl.h>

class HttpClient {
public:
	HttpClient(int timeout=60);
	~HttpClient();

	std::string Get(const std::string& url);
	bool DownloadFile(const std::string& url,const std::string& outputPath);
	bool DownloadToMemory(const std::string& url,std::vector<unsigned char>& buffer);

private:
	static size_t WriteCallback(void* contents,size_t size,size_t nmemb,std::string* data);
	static size_t WriteFileCallback(void* contents,size_t size,size_t nmemb,FILE* file);
	static size_t WriteMemoryCallback(void* contents,size_t size,size_t nmemb,std::vector<unsigned char>* buffer);

	CURL* curl;
};

#endif