#ifndef FILEHASHER_H
#define FILEHASHER_H

#include <string>
#include <vector>
#include <openssl/md5.h>
#include <openssl/sha.h>

class FileHasher {
public:
	static std::string CalculateFileHash(const std::string& filePath,const std::string& algorithm);
	static std::string CalculateMemoryHash(const std::vector<unsigned char>& data,const std::string& algorithm);
	static std::string CalculateDirectoryHash(const std::string& directoryPath,const std::string& algorithm);

private:
	static std::string MD5Hash(const std::vector<unsigned char>& data);
	static std::string SHA1Hash(const std::vector<unsigned char>& data);
	static std::string SHA256Hash(const std::vector<unsigned char>& data);
};

#endif