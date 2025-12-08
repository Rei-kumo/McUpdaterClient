#include "FileHasher.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>

std::string FileHasher::CalculateFileHash(const std::string& filePath,const std::string& algorithm) {
	std::ifstream file(filePath,std::ios::binary);
	if(!file) {
		return "";
	}

	std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(file),{});
	return CalculateMemoryHash(buffer,algorithm);
}

std::string FileHasher::CalculateMemoryHash(const std::vector<unsigned char>& data,const std::string& algorithm) {
	if(algorithm=="md5") {
		return MD5Hash(data);
	}
	else if(algorithm=="sha1") {
		return SHA1Hash(data);
	}
	else if(algorithm=="sha256") {
		return SHA256Hash(data);
	}
	return MD5Hash(data);
}

std::string FileHasher::CalculateDirectoryHash(const std::string& directoryPath,const std::string& algorithm) {
	std::string combinedContent;

	for(const auto& entry:std::filesystem::recursive_directory_iterator(directoryPath)) {
		if(entry.is_regular_file()) {
			std::string fileHash=CalculateFileHash(entry.path().string(),algorithm);
			combinedContent+=entry.path().filename().string()+":"+fileHash+";";
		}
	}

	std::vector<unsigned char> data(combinedContent.begin(),combinedContent.end());
	return CalculateMemoryHash(data,algorithm);
}

std::string FileHasher::MD5Hash(const std::vector<unsigned char>& data) {
	MD5_CTX context;
	MD5_Init(&context);
	MD5_Update(&context,data.data(),data.size());

	unsigned char digest[MD5_DIGEST_LENGTH];
	MD5_Final(digest,&context);

	std::stringstream ss;
	for(int i=0; i<MD5_DIGEST_LENGTH; ++i) {
		ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)digest[i];
	}
	return ss.str();
}

std::string FileHasher::SHA1Hash(const std::vector<unsigned char>& data) {
	SHA_CTX context;
	SHA1_Init(&context);
	SHA1_Update(&context,data.data(),data.size());

	unsigned char digest[SHA_DIGEST_LENGTH];
	SHA1_Final(digest,&context);

	std::stringstream ss;
	for(int i=0; i<SHA_DIGEST_LENGTH; ++i) {
		ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)digest[i];
	}
	return ss.str();
}

std::string FileHasher::SHA256Hash(const std::vector<unsigned char>& data) {
	SHA256_CTX context;
	SHA256_Init(&context);
	SHA256_Update(&context,data.data(),data.size());

	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256_Final(digest,&context);

	std::stringstream ss;
	for(int i=0; i<SHA256_DIGEST_LENGTH; ++i) {
		ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)digest[i];
	}
	return ss.str();
}
// 在FileHasher类中添加
std::string FileHasher::CalculateFileHashStream(const std::string& filePath,const std::string& algorithm) {
    std::ifstream file(filePath,std::ios::binary);
    if(!file) {
        return "";
    }

    const size_t bufferSize=8192;
    char buffer[bufferSize];

    if(algorithm=="md5") {
        MD5_CTX context;
        MD5_Init(&context);

        while(file.read(buffer,bufferSize)||file.gcount()>0) {
            MD5_Update(&context,buffer,file.gcount());
        }

        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5_Final(digest,&context);

        std::stringstream ss;
        for(int i=0; i<MD5_DIGEST_LENGTH; ++i) {
            ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)digest[i];
        }
        return ss.str();
    }
    else if(algorithm=="sha1") {
        SHA_CTX context;
        SHA1_Init(&context);

        while(file.read(buffer,bufferSize)||file.gcount()>0) {
            SHA1_Update(&context,buffer,file.gcount());
        }

        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA1_Final(digest,&context);

        std::stringstream ss;
        for(int i=0; i<SHA_DIGEST_LENGTH; ++i) {
            ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)digest[i];
        }
        return ss.str();
    }
    else if(algorithm=="sha256") {
        SHA256_CTX context;
        SHA256_Init(&context);

        while(file.read(buffer,bufferSize)||file.gcount()>0) {
            SHA256_Update(&context,buffer,file.gcount());
        }

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256_Final(digest,&context);

        std::stringstream ss;
        for(int i=0; i<SHA256_DIGEST_LENGTH; ++i) {
            ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)digest[i];
        }
        return ss.str();
    }

    return "";
}