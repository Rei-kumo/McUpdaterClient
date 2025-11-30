#include "HttpClient.h"
#include <iostream>
#include <fstream>
#include "Logger.h"

HttpClient::HttpClient(int timeout) {
	curl=curl_easy_init();
	if(curl) {
		curl_easy_setopt(curl,CURLOPT_USERAGENT,"MinecraftUpdater/1.0");
		curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeout);
		curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
	}
}

HttpClient::~HttpClient() {
	if(curl) {
		curl_easy_cleanup(curl);
	}
}

std::string HttpClient::Get(const std::string& url) {
	std::string response;

	if(!curl) {
		g_logger<<"[ERROR]CURL初始化失败"<<std::endl;
		return response;
	}

	curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
	curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,WriteCallback);
	curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);

	CURLcode res=curl_easy_perform(curl);
	if(res!=CURLE_OK) {
		g_logger<<"[ERROR]HTTP请求失败: "<<curl_easy_strerror(res)<<std::endl;
		return "";
	}

	return response;
}

bool HttpClient::DownloadFile(const std::string& url,const std::string& outputPath) {
	if(!curl) return false;

	FILE* file=nullptr;
	errno_t err=fopen_s(&file,outputPath.c_str(),"wb");
	if(err!=0||!file) {
		g_logger<<"[ERROR]无法创建文件: "<<outputPath<<std::endl;
		return false;
	}

	curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
	curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,WriteFileCallback);
	curl_easy_setopt(curl,CURLOPT_WRITEDATA,file);

	CURLcode res=curl_easy_perform(curl);
	fclose(file);

	if(res!=CURLE_OK) {
		g_logger<<"[ERROR]下载失败:"<<curl_easy_strerror(res)<<std::endl;
		return false;
	}

	return true;
}

bool HttpClient::DownloadToMemory(const std::string& url,std::vector<unsigned char>& buffer) {
	if(!curl) return false;

	buffer.clear();
	curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
	curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,WriteMemoryCallback);
	curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);

	CURLcode res=curl_easy_perform(curl);
	if(res!=CURLE_OK) {
		g_logger<<"[ERROR]下载到内存失败: "<<curl_easy_strerror(res)<<std::endl;
		return false;
	}
	return true;
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