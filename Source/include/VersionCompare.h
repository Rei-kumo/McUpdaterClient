#ifndef VERSIONCOMPARE_H
#define VERSIONCOMPARE_H

#include <string>
#include <vector>
#include <sstream>

class Version {
public:
    int major=0,minor=0,patch=0;

    explicit Version(const std::string& versionStr) {
        std::vector<int> parts;
        std::stringstream ss(versionStr);
        std::string part;
        while(std::getline(ss,part,'.')) {
            parts.push_back(std::stoi(part));
        }
        if(parts.size()>0) major=parts[0];
        if(parts.size()>1) minor=parts[1];
        if(parts.size()>2) patch=parts[2];
    }

    bool operator<(const Version& other) const {
        if(major!=other.major) return major<other.major;
        if(minor!=other.minor) return minor<other.minor;
        return patch<other.patch;
    }

    bool operator>(const Version& other) const { return other<*this; }
    bool operator==(const Version& other) const { return major==other.major&&minor==other.minor&&patch==other.patch; }
    bool operator!=(const Version& other) const { return !(*this==other); }
    bool operator<=(const Version& other) const { return !(*this>other); }
    bool operator>=(const Version& other) const { return !(*this<other); }
};

inline bool IsNewerVersion(const std::string& current,const std::string& latest) {
    return Version(latest)>Version(current);
}

#endif