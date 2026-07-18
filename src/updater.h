#pragma once
#include <string>

#define GITHUB_OWNER "bearroleplay"
#define GITHUB_REPO  "GTA-SA-Mobile-DXT-viewer"
#define APP_VERSION  "1.0"

struct UpdateInfo
{
    bool        available;
    std::string tagName; 
    std::string downloadUrl;  
    std::string releaseUrl;  
};

bool UpdateCheck(UpdateInfo& out, std::string& errorMsg);

bool UpdateDownloadAndReplace(const std::string& downloadUrl,
                              const std::wstring& exePath,
                              std::string& errorMsg);
