 #include "updater.h"
#include "logger.h"
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <sstream>
#include <vector>

static std::string JsonString(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t p = json.find(search);
    if (p == std::string::npos) return {};
    p = json.find(':', p + search.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string val;
    while (p < json.size() && json[p] != '"')
    {
        if (json[p] == '\\') { ++p; }
        if (p < json.size()) val += json[p];
        ++p;
    }
    return val;
}

static bool HttpGet(const std::wstring& host,
                    const std::wstring& path,
                    std::string& response,
                    std::string& errorMsg)
{
    HINTERNET hSession = WinHttpOpen(
        L"DXTViewer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { errorMsg = "WinHttpOpen failed"; return false; }

    HINTERNET hConn = WinHttpConnect(hSession, host.c_str(),
                                     INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSession); errorMsg = "WinHttpConnect failed"; return false; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); errorMsg = "WinHttpOpenRequest failed"; return false; }

    WinHttpAddRequestHeaders(hReq,
        L"Accept: application/vnd.github.v3+json\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
    ok = ok && WinHttpReceiveResponse(hReq, nullptr) != FALSE;

    if (!ok)
    {
        errorMsg = "HTTP request failed";
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
    {
        std::vector<char> buf(avail + 1, 0);
        DWORD read = 0;
        WinHttpReadData(hReq, buf.data(), avail, &read);
        response.append(buf.data(), read);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    LOG_DEBUG("HttpGet %ls%ls -> %zu bytes", host.c_str(), path.c_str(), response.size());
    return true;
}

static bool IsNewerVersion(const std::string& remote)
{
    std::string r = remote;
    if (!r.empty() && r[0] == 'v') r = r.substr(1);
    std::string local = APP_VERSION;

    auto split = [](const std::string& s) {
        std::vector<int> v;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.'))
        {
            try { v.push_back(std::stoi(tok)); } catch (...) { v.push_back(0); }
        }
        return v;
    };

    auto rv = split(r), lv = split(local);
    size_t n = std::max(rv.size(), lv.size());
    for (size_t i = 0; i < n; ++i)
    {
        int ri = i < rv.size() ? rv[i] : 0;
        int li = i < lv.size() ? lv[i] : 0;
        if (ri > li) return true;
        if (ri < li) return false;
    }
    return false;
}

static std::string FindExeAsset(const std::string& json)
{

    size_t pos = 0;
    while (pos < json.size())
    {
        size_t p = json.find("browser_download_url", pos);
        if (p == std::string::npos) break;
        p = json.find('"', p + 20);
        if (p == std::string::npos) break;
        p = json.find('"', p + 1); 
        if (p == std::string::npos) break;
        ++p;
        std::string url;
        while (p < json.size() && json[p] != '"') url += json[p++];
        if (url.size() >= 4 &&
            url.substr(url.size() - 4) == ".exe")
            return url;
        pos = p;
    }
    return {};
}

bool UpdateCheck(UpdateInfo& out, std::string& errorMsg)
{
    out = {};
    std::wstring apiHost = L"api.github.com";
    std::wstring apiPath = L"/repos/" GITHUB_OWNER L"/" GITHUB_REPO L"/releases/latest";

    std::string body;
    if (!HttpGet(apiHost, apiPath, body, errorMsg))
        return false;

    out.tagName     = JsonString(body, "tag_name");
    out.releaseUrl  = JsonString(body, "html_url");
    out.downloadUrl = FindExeAsset(body);

    LOG_INFO("UpdateCheck: tag=%s url=%s exe=%s",
        out.tagName.c_str(), out.releaseUrl.c_str(), out.downloadUrl.c_str());

    out.available = !out.tagName.empty() && IsNewerVersion(out.tagName);
    return true;
}

bool UpdateDownloadAndReplace(const std::string& downloadUrl,
                               const std::wstring& exePath,
                               std::string& errorMsg)
{
    if (downloadUrl.empty())
    {
        errorMsg = "No direct download URL found in release assets";
        return false;
    }

    std::wstring wUrl(downloadUrl.begin(), downloadUrl.end());
    // Strip "https://"
    std::wstring host, path;
    size_t schemeEnd = wUrl.find(L"://");
    if (schemeEnd != std::wstring::npos)
    {
        size_t hostStart = schemeEnd + 3;
        size_t slash = wUrl.find(L'/', hostStart);
        if (slash == std::wstring::npos)
        {
            host = wUrl.substr(hostStart);
            path = L"/";
        }
        else
        {
            host = wUrl.substr(hostStart, slash - hostStart);
            path = wUrl.substr(slash);
        }
    }
    else
    {
        errorMsg = "Malformed download URL";
        return false;
    }

    LOG_INFO("Downloading update from %ls%ls", host.c_str(), path.c_str());

    std::string body;
    if (!HttpGet(host, path, body, errorMsg))
        return false;

    if (body.empty())
    {
        errorMsg = "Downloaded file is empty";
        return false;
    }

    std::wstring tmpPath = exePath + L".new";
    FILE* f = nullptr;
    _wfopen_s(&f, tmpPath.c_str(), L"wb");
    if (!f)
    {
        errorMsg = "Cannot write temp file for update";
        return false;
    }
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);

    std::wstring batPath = exePath + L"_update.bat";
    FILE* bat = nullptr;
    _wfopen_s(&bat, batPath.c_str(), L"w");
    if (!bat)
    {
        errorMsg = "Cannot write update batch file";
        return false;
    }

    fprintf(bat,
        "@echo off\r\n"
        "ping 127.0.0.1 -n 3 >nul\r\n"
        "move /Y \"%ls\" \"%ls\"\r\n"
        "start \"\" \"%ls\"\r\n"
        "del \"%%~f0\"\r\n",
        tmpPath.c_str(), exePath.c_str(), exePath.c_str());
    fclose(bat);

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmd = L"cmd.exe /C \"" + batPath + L"\"";
    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);

    LOG_INFO("Update batch launched: %ls", batPath.c_str());
    return true;
}
