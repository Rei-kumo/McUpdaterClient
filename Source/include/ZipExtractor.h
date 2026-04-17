#ifndef ZIPEXTRACTOR_H
#define ZIPEXTRACTOR_H
#include "HttpClient.h"
#include "ProgressReporter.h"
#include "FileSystemHelper.h"
class ZipExtractor {
public:
    bool IsValidZipFile(const std::string& filePath);
    bool ExtractZipFromFile(const std::string& zipFilePath,
        const std::string& extractPath);
    bool ExtractZip(const std::vector<unsigned char>& zipData,
        const std::string& extractPath);
    bool DownloadAndExtract(const std::string& url,const std::string& relativePath,const std::string& targetBaseDir);
    ZipExtractor(HttpClient& http,ProgressReporter& reporter);
private:
    bool ExtractZipWithMiniz(const std::string& zipFilePath,
        const std::string& extractPath);
    bool ExtractZipSimple(const std::string& zipFilePath,
        const std::string& extractPath);
    bool ExtractZipWithSystemCommand(const std::string& zipFilePath,
        const std::string& extractPath);
    bool ExtractZipOriginal(const std::string& zipFilePath,
        const std::string& extractPath);
    bool CheckServerResponse(const std::string& url);
    HttpClient& httpClient;
    FileSystemHelper fsHelper;
    ProgressReporter& pRepoter;
};
#endif