#include "archive.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

static std::string ExtractQuotedName(const std::string& line, size_t& pos)
{
    while (pos < line.size() && line[pos] == ' ') ++pos;
    if (pos >= line.size() || line[pos] != '"') return {};
    ++pos;
    std::string name;
    while (pos < line.size() && line[pos] != '"')
        name += line[pos++];
    if (pos < line.size()) ++pos; // skip closing "
    return name;
}

static bool GetIntValue(const std::string& line, const std::string& key, int& out)
{
    std::string search = key + "=";
    size_t p = line.find(search);
    if (p == std::string::npos) return false;
    out = std::stoi(line.substr(p + search.size()));
    return true;
}

static bool GetHexValue(const std::string& line, const std::string& key, uint32_t& out)
{
    std::string search = key + "=";
    size_t p = line.find(search);
    if (p == std::string::npos) return false;
    out = (uint32_t)std::stoul(line.substr(p + search.size()), nullptr, 16);
    return true;
}

static bool GetStringValue(const std::string& line, const std::string& key, std::string& out)
{
    std::string search = key + "=";
    size_t p = line.find(search);
    if (p == std::string::npos) return false;
    size_t start = p + search.size();
    size_t end = line.find_first_of(" \t\r\n", start);
    out = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    return true;
}

static bool ParseTxt(const std::wstring& path,
                     ArchiveHeader& hdr,
                     std::vector<TexEntry>& entries,
                     std::string& errorMsg)
{
    std::ifstream f(path.c_str());
    if (!f.is_open())
    {
        errorMsg = "Cannot open txt file";
        return false;
    }

    std::string line;
    bool firstLine = true;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        if (firstLine)
        {
            firstLine = false;
            // Header line: cat=0 name=Default ...
            GetIntValue(line, "cat", hdr.cat);
            GetStringValue(line, "name", hdr.name);
            GetIntValue(line, "onfoot", hdr.onFoot);
            GetIntValue(line, "slow", hdr.slow);
            GetIntValue(line, "fast", hdr.fast);
            GetIntValue(line, "defaultformat", hdr.defaultFormat);
            GetIntValue(line, "defaultstream", hdr.defaultStream);
            LOG_DEBUG("Archive header: name=%s format=%d stream=%d",
                hdr.name.c_str(), hdr.defaultFormat, hdr.defaultStream);
            continue;
        }

        TexEntry e{};
        e.mipMode   = -1;
        e.alphaMode = -1;
        e.datOffset = 0xFFFFFFFF;
        e.thumbRGBA = 0;

        size_t pos = 0;
        e.name = ExtractQuotedName(line, pos);
        if (e.name.empty())
            continue;
        while (pos < line.size() && line[pos] == ' ') ++pos;
        if (pos < line.size() && line[pos] == '"')
        {
            std::string second = ExtractQuotedName(line, pos);
            // second may be "affiliate=xxx"
            std::string affKey = "affiliate=";
            if (second.find(affKey) == 0)
            {
                e.isAffiliate  = true;
                e.affiliateOf  = second.substr(affKey.size());
            }
        }

        if (!e.isAffiliate)
        {
            GetIntValue(line, "width",  e.width);
            GetIntValue(line, "height", e.height);
            GetHexValue(line, "png",    e.pngHash);
            GetHexValue(line, "img",    e.imgHash);
            GetIntValue(line, "mipmode",   e.mipMode);
            GetIntValue(line, "alphamode", e.alphaMode);
            GetIntValue(line, "isdetail",  e.isDetail);
            LOG_DEBUG("Texture: %s %dx%d alpha=%d mip=%d",
                e.name.c_str(), e.width, e.height, e.alphaMode, e.mipMode);
        }

        entries.push_back(e);
    }

    if (entries.empty())
    {
        errorMsg = "No texture entries found in txt";
        return false;
    }
    return true;
}

static bool ParseToc(const std::wstring& path,
                     std::vector<TexEntry>& entries,
                     std::string& errorMsg)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open())
    {
        errorMsg = "Cannot open toc file";
        return false;
    }

    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    if (sz < 4)
    {
        errorMsg = "TOC file too small";
        return false;
    }

    std::vector<uint32_t> toc(sz / 4);
    f.read(reinterpret_cast<char*>(toc.data()), sz);

    uint32_t totalDatSize = toc[0];
    LOG_DEBUG("TOC: total dat size = %u, toc entries (incl header) = %zu",
        totalDatSize, toc.size());
    size_t tocIdx = 1; // skip the first (dat size)
    for (auto& e : entries)
    {
        if (tocIdx >= toc.size())
            break;
        uint32_t val = toc[tocIdx++];
        if (val == 0xFFFFFFFF)
        {
            e.datOffset = 0xFFFFFFFF;
            e.datSize   = 0;
        }
        else
        {
            e.datOffset = val;
            uint32_t nextOff = totalDatSize;
            for (size_t j = tocIdx; j < toc.size(); ++j)
            {
                if (toc[j] != 0xFFFFFFFF)
                {
                    nextOff = toc[j];
                    break;
                }
            }
            e.datSize = (nextOff > val) ? (nextOff - val) : 0;
            LOG_DEBUG("  %s: offset=0x%08X size=%u", e.name.c_str(), val, e.datSize);
        }
    }
    return true;
}

static bool ParseTmb(const std::wstring& path,
                     std::vector<TexEntry>& entries,
                     std::string& errorMsg)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open())
    {
        LOG_WARN("Cannot open tmb file (thumbnails will be unavailable)");
        return true;
    }

    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    if (sz < 16)
        return true;

    std::vector<uint8_t> raw(sz);
    f.read(reinterpret_cast<char*>(raw.data()), sz);

    size_t numThumb = sz / 16;
    LOG_DEBUG("TMB: %zu thumbnail records", numThumb);

    size_t tmbIdx = 0;
    for (auto& e : entries)
    {
        if (e.isAffiliate || e.datOffset == 0xFFFFFFFF)
            continue;
        if (tmbIdx >= numThumb)
            break;
        const uint8_t* rec = raw.data() + tmbIdx * 16;
        // RGBA pixel is at [12-15]
        uint8_t r = rec[12], g = rec[13], b = rec[14], a = rec[15];
        e.thumbRGBA = ((uint32_t)r) | ((uint32_t)g << 8) |
                      ((uint32_t)b << 16) | ((uint32_t)a << 24);
        LOG_DEBUG("  TMB[%zu] for '%s': RGBA=%02X%02X%02X%02X",
            tmbIdx, e.name.c_str(), r, g, b, a);
        ++tmbIdx;
    }

    (void)errorMsg;
    return true;
}

bool ArchiveOpen(const std::wstring& txtPath,
                 Archive& out,
                 std::string& errorMsg)
{
    out = Archive{};
    out.txtPath = txtPath;

    size_t slash = txtPath.rfind(L'\\');
    if (slash == std::wstring::npos) slash = txtPath.rfind(L'/');
    if (slash != std::wstring::npos)
    {
        out.basePath = txtPath.substr(0, slash + 1);
        out.baseName = txtPath.substr(slash + 1);
    }
    else
    {
        out.baseName = txtPath;
    }

    {
        size_t dot = out.baseName.rfind(L'.');
        if (dot != std::wstring::npos)
            out.baseName = out.baseName.substr(0, dot);
        size_t us = out.baseName.rfind(L'_');
        if (us != std::wstring::npos)
        {
            std::wstring suf = out.baseName.substr(us + 1);
            bool allDig = !suf.empty();
            for (wchar_t c : suf) if (!iswdigit(c)) { allDig = false; break; }
            if (allDig) out.baseName = out.baseName.substr(0, us);
        }
    }

    auto FindSibling = [&](const std::wstring& ext) -> std::wstring
    {
        WIN32_FIND_DATAW fd;
        std::wstring pattern = out.basePath + out.baseName + L".dxt*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return {};
        do
        {
            std::wstring name = fd.cFileName;
            if (name.size() >= ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
            {
                FindClose(h);
                return out.basePath + name;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return {};
    };

    out.tocPath = FindSibling(L".toc");
    out.tmbPath = FindSibling(L".tmb");
    out.datPath = FindSibling(L".dat");

    LOG_INFO("Archive open: txt=%ls", txtPath.c_str());
    LOG_INFO("  toc=%ls", out.tocPath.c_str());
    LOG_INFO("  tmb=%ls", out.tmbPath.c_str());
    LOG_INFO("  dat=%ls", out.datPath.c_str());

    if (out.tocPath.empty())
    {
        errorMsg = "Cannot find .toc file next to the selected .txt";
        return false;
    }
    if (out.datPath.empty())
    {
        errorMsg = "Cannot find .dat file next to the selected .txt";
        return false;
    }

    if (!ParseTxt(txtPath, out.header, out.entries, errorMsg))
        return false;

    if (!ParseToc(out.tocPath, out.entries, errorMsg))
        return false;

    if (!out.tmbPath.empty())
    {
        std::string tmbErr;
        ParseTmb(out.tmbPath, out.entries, tmbErr);
    }

    LOG_INFO("Archive loaded: %zu entries", out.entries.size());
    return true;
}

std::vector<uint8_t> ArchiveReadTexData(const Archive& arc,
                                        const TexEntry& entry)
{
    if (entry.datOffset == 0xFFFFFFFF || entry.datSize == 0)
        return {};

    std::ifstream f(arc.datPath.c_str(), std::ios::binary);
    if (!f.is_open())
    {
        LOG_ERR("Cannot open dat file: %ls", arc.datPath.c_str());
        return {};
    }

    f.seekg(entry.datOffset);
    if (!f)
    {
        LOG_ERR("Seek failed to offset 0x%08X", entry.datOffset);
        return {};
    }

    std::vector<uint8_t> buf(entry.datSize);
    f.read(reinterpret_cast<char*>(buf.data()), entry.datSize);
    size_t got = (size_t)f.gcount();
    if (got < entry.datSize)
    {
        LOG_WARN("Short read: expected %u got %zu for '%s'",
            entry.datSize, got, entry.name.c_str());
        buf.resize(got);
    }

    LOG_DEBUG("ReadTexData: '%s' offset=0x%08X size=%u",
        entry.name.c_str(), entry.datOffset, entry.datSize);
    return buf;
}

bool ArchiveParseBlockHeader(const std::vector<uint8_t>& raw,
                             TexBlockHeader& hdr)
{
    if (raw.size() < 16)
        return false;

    hdr.magic0    = (uint16_t)(raw[0] | (raw[1] << 8));
    hdr.magic1    = (uint16_t)(raw[2] | (raw[3] << 8));
    hdr.width     = (uint16_t)(raw[4] | (raw[5] << 8));
    uint16_t hr   = (uint16_t)(raw[6] | (raw[7] << 8));
    hdr.hasMips   = (hr & 0x8000) != 0;
    hdr.height    = hr & 0x7FFF;
    hdr.dataSize  = (uint32_t)(raw[8]  | (raw[9]  << 8) |
                               (raw[10] << 16) | (raw[11] << 24));
    hdr.extra     = (uint32_t)(raw[12] | (raw[13] << 8) |
                               (raw[14] << 16) | (raw[15] << 24));

    LOG_DEBUG("BlockHeader: %ux%u hasMips=%d dataSize=%u extra=0x%08X",
        hdr.width, hdr.height, (int)hdr.hasMips, hdr.dataSize, hdr.extra);
    return true;
}

bool ArchiveReplaceTexData(const Archive& arc,
                           const TexEntry& entry,
                           const std::vector<uint8_t>& newRaw,
                           std::string& errorMsg)
{
    if (entry.datOffset == 0xFFFFFFFF)
    {
        errorMsg = "Cannot replace affiliate texture";
        return false;
    }
    if (newRaw.size() != entry.datSize)
    {
        errorMsg = "New data size does not match original — cannot replace";
        return false;
    }

    std::fstream f(arc.datPath.c_str(), std::ios::binary | std::ios::in | std::ios::out);
    if (!f.is_open())
    {
        errorMsg = "Cannot open dat file for writing";
        return false;
    }

    f.seekp(entry.datOffset);
    f.write(reinterpret_cast<const char*>(newRaw.data()), newRaw.size());
    if (!f)
    {
        errorMsg = "Write failed";
        return false;
    }

    LOG_INFO("Replaced texture '%s' at offset 0x%08X (%zu bytes)",
        entry.name.c_str(), entry.datOffset, newRaw.size());
    return true;
}
