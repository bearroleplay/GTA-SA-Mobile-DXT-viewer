#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct TexEntry
{
    std::string  name;
    int          width;
    int          height;
    uint32_t     pngHash;
    uint32_t     imgHash;  
    int          mipMode;
    int          alphaMode; 
    int          isDetail;  
    bool         isAffiliate;
    std::string  affiliateOf;

    uint32_t     datOffset;
    uint32_t     datSize;  

    uint32_t     thumbRGBA;
};

struct ArchiveHeader
{
    int         cat;
    std::string name;
    int         onFoot;
    int         slow;
    int         fast;
    int         defaultFormat;
    int         defaultStream;
};

struct Archive
{
    std::wstring   txtPath;
    std::wstring   basePath;
    std::wstring   baseName;
    std::wstring   tocPath;
    std::wstring   tmbPath;
    std::wstring   datPath;

    ArchiveHeader  header;
    std::vector<TexEntry> entries;
};

bool ArchiveOpen(const std::wstring& txtPath,
                 Archive& out,
                 std::string& errorMsg);

std::vector<uint8_t> ArchiveReadTexData(const Archive& arc,
                                        const TexEntry& entry);

struct TexBlockHeader
{
    uint16_t magic0;
    uint16_t magic1;
    uint16_t width;
    uint16_t height;
    bool     hasMips;
    uint32_t dataSize;
    uint32_t extra;
};
bool ArchiveParseBlockHeader(const std::vector<uint8_t>& raw,
                             TexBlockHeader& hdr);

bool ArchiveReplaceTexData(const Archive& arc,
                           const TexEntry& entry,
                           const std::vector<uint8_t>& newRaw,
                           std::string& errorMsg);
