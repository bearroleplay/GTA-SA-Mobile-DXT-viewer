#include "gui.h"
#include "logger.h"
#include "updater.h"
#include <objbase.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <string>
#include <thread>

static void RLEDecompress(uint8_t* pDest,       uint32_t uiDestSize,
                           const uint8_t* pSrc,  uint32_t uiSrcSize,
                           uint32_t uiSegSize,   uint8_t  uiEscape)
{
    if (uiDestSize == 0 || uiSegSize == 0 || uiSrcSize == 0) return;
    const uint8_t* pDestEnd = pDest + uiDestSize;
    const uint8_t* pSrcEnd  = pSrc  + uiSrcSize;

    while (pDest < pDestEnd)
    {
        if (pSrc >= pSrcEnd) break;

        if (pSrc[0] == uiEscape)
        {
            if (pSrc + 2 + (ptrdiff_t)uiSegSize > pSrcEnd) break;

            uint8_t count = pSrc[1];
            const uint8_t* pBlock = pSrc + 2;
            pSrc += 2 + uiSegSize;            

            if (count != 0)
            {
                uint8_t cnt = count;
                do {
                    size_t avail = (size_t)(pDestEnd - pDest);
                    size_t copy  = avail < uiSegSize ? avail : uiSegSize;
                    memcpy(pDest, pBlock, copy);
                    pDest += copy;
                    if (copy < uiSegSize) return; // dest full
                    cnt--;
                } while (cnt != 0);
            }
        }
        else
        {
            if (pSrc + (ptrdiff_t)uiSegSize > pSrcEnd) break;

            size_t avail = (size_t)(pDestEnd - pDest);
            size_t copy  = avail < uiSegSize ? avail : uiSegSize;
            memcpy(pDest, pSrc, copy);
            pDest += copy;
            pSrc  += uiSegSize;
            if (copy < uiSegSize) return; // dest full
        }
    }
}

// -------------------------------------------------------------------------
// Global state
// -------------------------------------------------------------------------
static GuiState g;
static ULONG_PTR g_gdiplusToken;
static const wchar_t* WC_MAIN    = L"DXTViewerMain";
static const wchar_t* WC_PREVIEW = L"DXTViewerPreview";

// Checkerboard tile size (pixels) for alpha transparency display
static const int CHECKER_SZ = 8;

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------
static std::wstring Narrow2Wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void GuiSetStatus(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list a; va_start(a, fmt);
    vswprintf(buf, 512, fmt, a);
    va_end(a);
    if (g.hwndStatus) SetWindowTextW(g.hwndStatus, buf);
    LOG_DEBUG("Status: %ls", buf);
}

// -------------------------------------------------------------------------
// HBITMAP creation from RGBA top-down pixels
// -------------------------------------------------------------------------
static HBITMAP CreateDIBFromRGBA(const uint8_t* rgba, int w, int h)
{
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbm || !bits) return nullptr;

    // Convert RGBA -> BGRA for GDI
    uint8_t* dst = (uint8_t*)bits;
    for (int i = 0; i < w * h; ++i)
    {
        dst[i*4+0] = rgba[i*4+2]; // B
        dst[i*4+1] = rgba[i*4+1]; // G
        dst[i*4+2] = rgba[i*4+0]; // R
        dst[i*4+3] = rgba[i*4+3]; // A
    }
    return hbm;
}

// -------------------------------------------------------------------------
// ListView refresh
// -------------------------------------------------------------------------
void GuiRefreshList(const std::wstring& filter)
{
    ListView_DeleteAllItems(g.hwndList);
    if (!g.arcLoaded) return;

    std::wstring lf = filter;
    std::transform(lf.begin(), lf.end(), lf.begin(), ::towlower);

    int visIdx = 0;
    for (size_t i = 0; i < g.arc.entries.size(); ++i)
    {
        const TexEntry& e = g.arc.entries[i];
        std::wstring wname = Narrow2Wide(e.name);
        if (!lf.empty())
        {
            std::wstring lname = wname;
            std::transform(lname.begin(), lname.end(), lname.begin(), ::towlower);
            if (lname.find(lf) == std::wstring::npos) continue;
        }

        LVITEMW lvi{};
        lvi.mask    = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem   = visIdx++;
        lvi.lParam  = (LPARAM)i;
        lvi.pszText = (LPWSTR)wname.c_str();
        int row = ListView_InsertItem(g.hwndList, &lvi);

        // Width column
        wchar_t tmp[32];
        if (!e.isAffiliate && e.width)
        {
            swprintf(tmp, 32, L"%d", e.width);
            ListView_SetItemText(g.hwndList, row, 1, tmp);
            swprintf(tmp, 32, L"%d", e.height);
            ListView_SetItemText(g.hwndList, row, 2, tmp);
        }
        else if (e.isAffiliate)
        {
            ListView_SetItemText(g.hwndList, row, 1, (LPWSTR)L"—");
            ListView_SetItemText(g.hwndList, row, 2, (LPWSTR)L"—");
        }

        // Format
        const wchar_t* fmt = L"?";
        if (e.isAffiliate) fmt = L"alias";
        else if (e.alphaMode == 2) fmt = L"DXT5";
        else if (e.alphaMode == 3) fmt = L"DXT3";
        else                       fmt = L"DXT1";
        ListView_SetItemText(g.hwndList, row, 3, (LPWSTR)fmt);
    }
    LOG_DEBUG("List refreshed: %d items visible", visIdx);
}

// -------------------------------------------------------------------------
// Texture selection
// -------------------------------------------------------------------------
void GuiSelectTexture(int entryIdx)
{
    g.selectedIdx = entryIdx;

    if (g.hbmPreview) { DeleteObject(g.hbmPreview); g.hbmPreview = nullptr; }
    g.texW = g.texH = 0;

    if (entryIdx < 0 || entryIdx >= (int)g.arc.entries.size())
    {
        SetWindowTextW(g.hwndInfo, L"No texture selected");
        InvalidateRect(g.hwndPreview, nullptr, TRUE);
        return;
    }

    const TexEntry& e = g.arc.entries[entryIdx];
    LOG_DEBUG("Select texture: %s", e.name.c_str());

    if (e.isAffiliate)
    {
        wchar_t buf[256];
        swprintf(buf, 256, L"%hs  [alias of %hs]",
            e.name.c_str(), e.affiliateOf.c_str());
        SetWindowTextW(g.hwndInfo, buf);
        InvalidateRect(g.hwndPreview, nullptr, TRUE);
        GuiSetStatus(L"Affiliate texture — no data stored");
        return;
    }

    if (e.datOffset == 0xFFFFFFFF)
    {
        SetWindowTextW(g.hwndInfo, L"No .dat data for this entry");
        InvalidateRect(g.hwndPreview, nullptr, TRUE);
        return;
    }

    GuiSetStatus(L"Loading %hs ...", e.name.c_str());

    std::vector<uint8_t> raw = ArchiveReadTexData(g.arc, e);
    if (raw.empty())
    {
        GuiSetStatus(L"Failed to read texture data");
        return;
    }

    // Parse block header
    TexBlockHeader bh{};
    bool hdrOk = ArchiveParseBlockHeader(raw, bh);

    int txtW = e.width, txtH = e.height;

    // -----------------------------------------------------------------------
    // RGBA32 fast path: rawSize - 16 == w * h * 4
    // Some textures (typically mipMode=0) are stored as raw uncompressed
    // RGBA8888 with a 16-byte header prefix. Detect and display directly.
    // -----------------------------------------------------------------------
    const wchar_t* fmtName = L"DXT1";
    if (raw.size() > 16 && txtW > 0 && txtH > 0 &&
        (raw.size() - 16) == (size_t)txtW * txtH * 4)
    {
        LOG_DEBUG("Detect RGBA32: %s %dx%d (%zu bytes)",
            e.name.c_str(), txtW, txtH, raw.size());

        std::vector<uint8_t> rgba(raw.begin() + 16, raw.end());
        std::vector<uint8_t> view = DXTApplyChannelMode(rgba, txtW, txtH, g.chanMode);
        g.hbmPreview = CreateDIBFromRGBA(view.data(), txtW, txtH);
        g.texW = txtW; g.texH = txtH;

        wchar_t info[512];
        swprintf(info, 512,
            L"%hs   %d × %d   RGBA32   mipMode=%d  alphaMode=%d  offset=0x%08X  size=%u B",
            e.name.c_str(), txtW, txtH,
            e.mipMode, e.alphaMode, e.datOffset, e.datSize);
        SetWindowTextW(g.hwndInfo, info);
        GuiSetStatus(L"Loaded %hs  (%d×%d  RGBA32)", e.name.c_str(), txtW, txtH);
        InvalidateRect(g.hwndPreview, nullptr, TRUE);
        return;
    }

    DXTFormat fmt = DXTGuessFormat(e.alphaMode, g.arc.header.defaultFormat);
    if (fmt == DXTFormat::DXT5) fmtName = L"DXT5";
    if (fmt == DXTFormat::DXT3) fmtName = L"DXT3";
    int bpb = (fmt == DXTFormat::DXT1) ? 8 : 16; // bytes per 4×4 block

    // Returns minimum bytes needed to decode one mip level (w,h) in current format
    auto needBytes = [&](int tw, int th) -> size_t {
        return (size_t)((tw+3)/4) * ((th+3)/4) * bpb;
    };

    // Returns total bytes for all mip levels from (tw,th) down to 1×1
    auto needAllMipBytes = [&](int tw, int th) -> size_t {
        size_t total = 0;
        while (tw > 0 || th > 0)
        {
            int mw = std::max(tw, 1), mh = std::max(th, 1);
            total += (size_t)((mw+3)/4) * ((mh+3)/4) * bpb;
            tw >>= 1; th >>= 1;
        }
        return total;
    };

    // Same as needAllMipBytes but stops at the 4×4 mip level.
    // Rockstar mobile archives do NOT store sub-4×4 mip levels (2×2, 1×1),
    // so the bytes stored on disk match this count, not needAllMipBytes.
    // Binary verification: 256×256 DXT5 → 87376 B (not 87408 from needAllMipBytes).
    auto needDXTChainBytes = [&](int tw, int th) -> size_t {
        size_t total = 0;
        while (tw > 0 || th > 0)
        {
            int mw = std::max(tw, 1), mh = std::max(th, 1);
            if (mw < 4 && mh < 4) break;   // Rockstar stops here; no sub-4×4 mips
            total += (size_t)((mw+3)/4) * ((mh+3)/4) * bpb;
            tw >>= 1; th >>= 1;
        }
        return total;
    };

    // -----------------------------------------------------------------------
    // Decompression detection and inflate
    //
    // SAMP Mobile archives compress DXT data for larger textures using raw
    // deflate (no zlib wrapper — do NOT use uncompress(), use inflateInit2
    // with -MAX_WBITS for raw deflate).
    // The 16-byte block header stores the dimensions; the data that follows
    // is either the raw DXT stream (small textures) or a raw-deflate stream.
    //
    // Detection: if stored bytes (raw.size()-16) are less than the minimum
    // bytes needed just for the base mip level, the data must be compressed.
    // -----------------------------------------------------------------------
    size_t storedDataBytes = raw.size() > 16 ? raw.size() - 16 : 0;

    // Use block-header dims if available, fall back to txt dims for the check
    int refW = (hdrOk && bh.width  > 0) ? (int)bh.width  : txtW;
    int refH = (hdrOk && bh.height > 0) ? (int)bh.height : txtH;

    size_t baseMipBytes    = (refW > 0 && refH > 0) ? needBytes(refW, refH)       : 0;
    size_t allMipBytes     = (refW > 0 && refH > 0) ? needAllMipBytes(refW, refH) : 0;

    // Compute expected DXT chain size from the .txt dimensions (e.g. 256×256).
    // Two reasons to keep this separate from allMipBytes:
    //   1. Block headers on broken textures carry garbage dims (e.g. 59640×32819),
    //      making allMipBytes enormous and useless for layout detection.
    //   2. needDXTChainBytes stops at 4×4 (matching what Rockstar actually stores),
    //      whereas needAllMipBytes goes down to 1×1 (giving 87408 vs correct 87376).
    size_t txtDXTChainBytes = (txtW > 0 && txtH > 0) ? needDXTChainBytes(txtW, txtH) : 0;

    // The .dat file allocates slots aligned to sector boundaries; the slot size
    // (storedDataBytes = nextOffset - thisOffset) can be much larger than the
    // actual data.  bh.dataSize (block-header bytes 8–11) is the true byte count
    // of the payload that follows the 16-byte header — use it for detection and
    // as the source-length limit passed to RLEDecompress.
    size_t actualSrcBytes =
        (hdrOk && bh.dataSize > 0 && (size_t)bh.dataSize <= storedDataBytes)
        ? (size_t)bh.dataSize
        : storedDataBytes;

    // Buffer holding the decompressed DXT stream (filled only when compressed)
    std::vector<uint8_t> decompressedBuf;
    bool wasDecompressed = false;

    // Escape byte: stored in the low byte of the block-header `extra` field.
    // Must be computed BEFORE the RLE detection check because escape == 0x00
    // means no RLE was applied (an escape byte of 0x00 would corrupt any DXT5
    // stream, since the alpha endpoint byte 0x00 is extremely common and would
    // be misidentified as an RLE run marker on every block).
    uint8_t escape = hdrOk ? (uint8_t)(bh.extra & 0xFF) : 0x00;

    // RLE detection using actualSrcBytes (true payload length, not slot size):
    //   escape == 0x00              → raw texture, never RLE (would corrupt DXT5)
    //   actualSrcBytes == baseMipBytes  → raw mip0 (menu_down, etc.) — skip RLE
    //   actualSrcBytes == allMipBytes   → raw all-mip stack          — skip RLE
    //   anything else                  → Rockstar block-level RLE
    //
    // Note: the RLE stream CAN be larger than baseMipBytes when the escape byte
    // appears frequently in the DXT data (every such block becomes an 18-byte
    // RUN record instead of a 16-byte literal), so we do NOT apply an upper bound.
    if (escape != 0x00 &&
        actualSrcBytes > 2 && baseMipBytes > 0 &&
        actualSrcBytes != baseMipBytes &&
        actualSrcBytes != allMipBytes)
    {
        // Stored data is Rockstar block-level RLE.
        //
        // Confirmed via disassembly of libGTASA.so RLEDecompress @ 0x283030:
        //   - uiSegSize = bytes per one DXT block: 8 (DXT1) or 16 (DXT5)
        //   - uiEscape  = escape byte from block-header extra[0]
        //   - SAMP forces mip=1, so we only need baseMipBytes of output.
        size_t outCapacity = baseMipBytes + baseMipBytes / 4 + 1024;
        decompressedBuf.assign(outCapacity, 0);

        // segSize = bytes per 4×4 DXT block in the chosen format
        uint32_t segSize = (uint32_t)bpb; // 8 for DXT1, 16 for DXT5/DXT3

        RLEDecompress(
            decompressedBuf.data(),              // pDest
            (uint32_t)baseMipBytes,              // uiDestSize  (mip0 only — SAMP mip=1)
            raw.data() + 16,                     // pSrc  (skip 16-byte block header)
            (uint32_t)actualSrcBytes,            // uiSrcSize   (true payload, not slot size)
            segSize,                             // uiSegSize
            escape);                             // uiEscape (from block header extra[0])

        decompressedBuf.resize(baseMipBytes);
        wasDecompressed = true;
        LOG_DEBUG("Rockstar RLE decompressed '%s': %zu (slot=%zu) → %zu bytes (segSize=%u escape=0x%02X)",
            e.name.c_str(), actualSrcBytes, storedDataBytes, baseMipBytes, segSize, (unsigned)escape);
    }

    // The effective DXT source: decompressed stream, or the raw bytes after the header.
    // dxtOffset is now always 0 relative to effData (the header was already skipped).
    const uint8_t* effData = wasDecompressed
        ? decompressedBuf.data()
        : (raw.data() + 16);
    size_t effSize = wasDecompressed ? decompressedBuf.size() : storedDataBytes;

    // -----------------------------------------------------------------------
    // Diagnostic hex dump for raw textures with escape == 0x00
    // This helps identify what format the payload is actually in.
    // -----------------------------------------------------------------------
    if (!wasDecompressed && escape == 0x00)
    {
        char hexBuf[256] = {};
        size_t dumpBytes = effSize < 64 ? effSize : 64;
        int pos = 0;
        for (size_t i = 0; i < dumpBytes && pos < (int)sizeof(hexBuf) - 3; ++i)
            pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", effData[i]);
        LOG_DEBUG("Raw escape=0x00 payload[0..%zu]: %s", dumpBytes, hexBuf);
        LOG_DEBUG("  storedDataBytes=%zu  allMipBytes=%zu  baseMipBytes=%zu  ratio=%.4f",
            storedDataBytes, allMipBytes, baseMipBytes,
            allMipBytes > 0 ? (double)effSize / (double)allMipBytes : 0.0);
    }

    // -----------------------------------------------------------------------
    // Double-chain raw texture layout (escape == 0x00 only):
    //
    // Binary analysis of SAMP Mobile archives reveals that several
    // 256×256 DXT5 textures (e.g. hud_dive, hud_bicycle, BJStand) use
    // this exact payload layout:
    //
    //   [ 10-byte sub-header (all zeros) ]
    //   [ allMipBytes: DXT5 mip chain #1  ]  ← the real, live texture
    //   [ allMipBytes: DXT5 mip chain #2  ]  ← near-fully transparent copy
    //
    // Total: 2 × allMipBytes + 10  (e.g. 2×87376+10 = 174762)
    //
    // Chain #2 consists almost entirely of 0x01 0x00 alpha-block headers
    // (alpha0=1, alpha1=0 → transparent); chain #1 has real pixel data.
    //
    // Detection: effSize is in [2×allMipBytes + 6 .. 2×allMipBytes + 16].
    // Action:    skip the sub-header (effSize − 2×allMipBytes bytes) and
    //            set effSize = allMipBytes so chain #1 is decoded.
    // -----------------------------------------------------------------------
    // Two reasons we use txtDXTChainBytes instead of allMipBytes here:
    //   1. Block header dims are garbage on these textures → allMipBytes is huge.
    //   2. needDXTChainBytes stops at 4×4 mips (87376 B for 256×256 DXT5),
    //      matching what is actually stored. needAllMipBytes gives 87408 (adds
    //      2×2 and 1×1 mips that Rockstar does not write), which breaks the check.
    //
    // Binary-verified layout for 256×256 DXT5 slots (size=174778, stored=174762):
    //   [ 10-byte sub-header (all zeros) ]
    //   [ 87376-byte DXT5 mip chain #1  ]  ← real icon data (3503/4096 blocks)
    //   [ 87376-byte DXT5 mip chain #2  ]  ← transparent filler (all 0x01 0x00)
    //   Total: 10 + 87376 + 87376 = 174762  ✓
    if (!wasDecompressed && escape == 0x00 && txtDXTChainBytes > 0 &&
        effSize >= txtDXTChainBytes * 2 + 6  &&   // sub-header ≥ 6 bytes
        effSize <= txtDXTChainBytes * 2 + 16)     // sub-header ≤ 16 bytes
    {
        size_t subHdrSize = effSize - txtDXTChainBytes * 2;
        LOG_DEBUG("Double-chain layout: effSize=%zu dxtChain=%zu subHdr=%zu → chain#1",
            effSize, txtDXTChainBytes, subHdrSize);
        effData += subHdrSize;
        effSize  = txtDXTChainBytes;
    }

    // -----------------------------------------------------------------------
    // Pick (w, h) — dimensions to decode:
    //
    // 1. Block-header dims fit in effSize → use header dims.
    // 2. Txt dims fit in effSize           → use txt dims.
    // 3. Fallback: use txt dims (or header dims), pad data with zeros.
    // -----------------------------------------------------------------------
    int w = txtW, h = txtH;
    bool decided = false;

    // Option 1
    if (hdrOk && bh.width > 0 && bh.height > 0)
    {
        if (effSize >= needBytes((int)bh.width, (int)bh.height))
        {
            w = (int)bh.width; h = (int)bh.height;
            decided = true;
            LOG_DEBUG("Strategy 1: header dims %dx%d effSize=%zu", w, h, effSize);
        }
    }

    // Option 2
    if (!decided && txtW > 0 && txtH > 0 && effSize >= needBytes(txtW, txtH))
    {
        w = txtW; h = txtH; decided = true;
        LOG_DEBUG("Strategy 2: txt dims %dx%d effSize=%zu", w, h, effSize);
    }

    // Option 3: fallback
    if (!decided)
    {
        w = txtW; h = txtH;
        LOG_DEBUG("Strategy 3 (fallback/pad): %dx%d effSize=%zu", w, h, effSize);
    }

    if (w <= 0 || h <= 0)
    {
        GuiSetStatus(L"Unknown texture dimensions");
        return;
    }

    // Build a buffer of exactly needBytes(w,h), padded with zeros if needed.
    size_t need  = needBytes(w, h);
    size_t avail = effSize; // effData already starts at DXT data, no offset needed

    std::vector<uint8_t> dxtBuf;
    const uint8_t* dxtData;
    size_t dxtSize;

    if (avail >= need)
    {
        // Enough data — use effData directly
        dxtData = effData;
        dxtSize = avail;
        dxtBuf.clear(); // not used
    }
    else
    {
        // Pad with zeros so the block layout stays correct
        dxtBuf.assign(need, 0);
        if (avail > 0)
            std::copy(effData, effData + avail, dxtBuf.data());
        dxtData = dxtBuf.data();
        dxtSize = need;
        LOG_WARN("Padded %dx%d DXT data: have %zu need %zu (partial texture)",
            w, h, avail, need);
    }

    LOG_DEBUG("Decode: %s %dx%d fmt=%d compressed=%d dxtSize=%zu",
        e.name.c_str(), w, h, (int)fmt, (int)wasDecompressed, dxtSize);

    std::vector<uint8_t> rgba = DXTDecode(fmt, w, h, dxtData, dxtSize);
    if (rgba.empty())
    {
        GuiSetStatus(L"DXT decode failed for %hs", e.name.c_str());
        return;
    }

    std::vector<uint8_t> view = DXTApplyChannelMode(rgba, w, h, g.chanMode);
    g.hbmPreview = CreateDIBFromRGBA(view.data(), w, h);
    g.texW = w; g.texH = h;

    wchar_t info[512];
    const wchar_t* compFlag = wasDecompressed ? L" [RLE]" : L"";
    if (w != txtW || h != txtH)
        swprintf(info, 512,
            L"%hs   decoded %d×%d  (txt: %d×%d)   %ls%ls   mipMode=%d  alphaMode=%d  offset=0x%08X  size=%u B",
            e.name.c_str(), w, h, txtW, txtH, fmtName, compFlag,
            e.mipMode, e.alphaMode, e.datOffset, e.datSize);
    else
        swprintf(info, 512,
            L"%hs   %d × %d   %ls%ls   mipMode=%d  alphaMode=%d  offset=0x%08X  size=%u B",
            e.name.c_str(), w, h, fmtName, compFlag,
            e.mipMode, e.alphaMode, e.datOffset, e.datSize);
    SetWindowTextW(g.hwndInfo, info);

    InvalidateRect(g.hwndPreview, nullptr, TRUE);
    GuiSetStatus(L"Loaded %hs  (%d×%d  %ls%ls)", e.name.c_str(), w, h, fmtName, compFlag);
}

// -------------------------------------------------------------------------
// Replace texture
// -------------------------------------------------------------------------
void GuiReplaceTexture(HWND hwnd)
{
    if (g.selectedIdx < 0 || g.selectedIdx >= (int)g.arc.entries.size())
    {
        MessageBoxW(hwnd, L"No texture selected.", L"Replace", MB_ICONWARNING);
        return;
    }
    const TexEntry& e = g.arc.entries[g.selectedIdx];
    if (e.isAffiliate || e.datOffset == 0xFFFFFFFF)
    {
        MessageBoxW(hwnd, L"Cannot replace affiliate texture.", L"Replace", MB_ICONWARNING);
        return;
    }

    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Image files\0*.png;*.bmp;*.jpg;*.tga\0All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    // Load with GDI+
    Gdiplus::Bitmap bmp(path);
    if (bmp.GetLastStatus() != Gdiplus::Ok)
    {
        MessageBoxW(hwnd, L"Failed to load image.", L"Replace", MB_ICONERROR);
        return;
    }

    int nw = (int)bmp.GetWidth();
    int nh = (int)bmp.GetHeight();

    // Warn if resolution differs
    if ((e.width && nw != e.width) || (e.height && nh != e.height))
    {
        wchar_t msg[256];
        swprintf(msg, 256,
            L"Image resolution %d×%d does not match original %d×%d.\n"
            L"This may cause display errors in-game.\n\nContinue anyway?",
            nw, nh, e.width, e.height);
        if (MessageBoxW(hwnd, msg, L"Resolution Mismatch", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }

    // Read original raw block to preserve the 16-byte header
    std::vector<uint8_t> origRaw = ArchiveReadTexData(g.arc, e);
    if (origRaw.size() < 16)
    {
        MessageBoxW(hwnd, L"Cannot read original texture block.", L"Replace", MB_ICONERROR);
        return;
    }

    // Encode the loaded image to the same DXT format
    // We do a simple 1:1 copy by decoding the PNG and re-encoding to DXT
    // (full DXT encoder implementation is out of scope; we store RGBA raw
    //  and replace only if original was uncompressed, otherwise warn)
    // For now: if the source image matches dimensions and the original data
    // size equals what we expect for that DXT format, encode it.
    // Simplified: write RGBA directly into a temporary raw block matching size.
    // A full DXT encoder would go here — for correctness, we encode
    // the bitmap to a BMP in memory and replace the dat block with the
    // re-encoded DXT data using a soft DXT encoder.

    // Lock bits for RGBA access
    Gdiplus::Rect rect(0, 0, nw, nh);
    Gdiplus::BitmapData bdata;
    bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                 PixelFormat32bppARGB, &bdata);

    // Build DXT1 or DXT5 data (minimal software encoder)
    DXTFormat fmt = DXTGuessFormat(e.alphaMode, g.arc.header.defaultFormat);
    int blockSize  = (fmt == DXTFormat::DXT1) ? 8 : 16;
    int bw = (nw + 3) / 4, bh = (nh + 3) / 4;
    size_t dxtDataSize = (size_t)(bw * bh * blockSize);
    size_t newTotalSize = 16 + dxtDataSize;

    if (newTotalSize != origRaw.size())
    {
        bmp.UnlockBits(&bdata);
        wchar_t msg[256];
        swprintf(msg, 256,
            L"Encoded size (%zu bytes) differs from original (%zu bytes).\n"
            L"In-place replacement is not possible.",
            newTotalSize, origRaw.size());
        MessageBoxW(hwnd, msg, L"Replace", MB_ICONERROR);
        return;
    }

    // Simple DXT1/DXT5 encoder: for each 4×4 block find min/max colours
    std::vector<uint8_t> newRaw = origRaw; // copy header
    uint8_t* dstBlock = newRaw.data() + 16;

    const uint8_t* src = (const uint8_t*)bdata.Scan0;
    int stride = bdata.Stride;

    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            // Collect 4×4 pixels (BGRA from GDI+)
            uint8_t pixels[4][4][4] = {};
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                {
                    int px = bx*4+dx, py = by*4+dy;
                    if (px < nw && py < nh)
                    {
                        const uint8_t* p = src + py*stride + px*4;
                        pixels[dy][dx][0] = p[2]; // R
                        pixels[dy][dx][1] = p[1]; // G
                        pixels[dy][dx][2] = p[0]; // B
                        pixels[dy][dx][3] = p[3]; // A
                    }
                }

            if (fmt == DXTFormat::DXT5)
            {
                // Alpha block: min/max alpha
                uint8_t aMin=255, aMax=0;
                for (int i=0;i<4;++i) for(int j=0;j<4;++j)
                { aMin=std::min(aMin,pixels[i][j][3]); aMax=std::max(aMax,pixels[i][j][3]); }
                dstBlock[0] = aMax; dstBlock[1] = aMin;
                // Simple: all pixels use interpolated; encode as 3-bit indices
                uint64_t aBits = 0;
                for (int i = 0; i < 16; ++i)
                {
                    int ai = i/4, aj = i%4;
                    uint8_t a = pixels[ai][aj][3];
                    int idx = (aMax != aMin) ? (int)((aMax - a) * 6 / (aMax - aMin)) : 0;
                    aBits |= ((uint64_t)(idx & 7)) << (i*3);
                }
                for (int i = 0; i < 6; ++i) dstBlock[2+i] = (uint8_t)(aBits >> (i*8));
                dstBlock += 8; // advance, colour block follows
            }

            // Colour block (DXT1 part, shared by DXT1/DXT5)
            uint8_t rMin=255,rMax=0,gMin=255,gMax=0,bMin=255,bMax=0;
            for(int i=0;i<4;++i) for(int j=0;j<4;++j)
            {
                rMin=std::min(rMin,pixels[i][j][0]); rMax=std::max(rMax,pixels[i][j][0]);
                gMin=std::min(gMin,pixels[i][j][1]); gMax=std::max(gMax,pixels[i][j][1]);
                bMin=std::min(bMin,pixels[i][j][2]); bMax=std::max(bMax,pixels[i][j][2]);
            }
            uint16_t c0 = (uint16_t)(((rMax>>3)<<11)|((gMax>>2)<<5)|(bMax>>3));
            uint16_t c1 = (uint16_t)(((rMin>>3)<<11)|((gMin>>2)<<5)|(bMin>>3));
            if (fmt == DXTFormat::DXT1 && c0 <= c1) std::swap(c0, c1);

            dstBlock[0] = c0 & 0xFF; dstBlock[1] = (c0>>8) & 0xFF;
            dstBlock[2] = c1 & 0xFF; dstBlock[3] = (c1>>8) & 0xFF;

            uint32_t indices = 0;
            for (int i = 0; i < 16; ++i)
            {
                int ai=i/4, aj=i%4;
                uint8_t r2=pixels[ai][aj][0], g2=pixels[ai][aj][1], b2=pixels[ai][aj][2];
                // Pick closest of c0/c1
                int d0 = abs(r2-rMax)+abs(g2-gMax)+abs(b2-bMax);
                int d1 = abs(r2-rMin)+abs(g2-gMin)+abs(b2-bMin);
                indices |= ((d0<=d1 ? 0 : 1) << (i*2));
            }
            dstBlock[4]=(uint8_t)(indices&0xFF);
            dstBlock[5]=(uint8_t)((indices>>8)&0xFF);
            dstBlock[6]=(uint8_t)((indices>>16)&0xFF);
            dstBlock[7]=(uint8_t)((indices>>24)&0xFF);
            dstBlock += 8;
        }
    }

    bmp.UnlockBits(&bdata);

    std::string err;
    if (!ArchiveReplaceTexData(g.arc, e, newRaw, err))
    {
        MessageBoxW(hwnd, Narrow2Wide(err).c_str(), L"Replace Error", MB_ICONERROR);
        return;
    }

    // Reload preview
    GuiSelectTexture(g.selectedIdx);
    GuiSetStatus(L"Texture '%hs' replaced successfully", e.name.c_str());
}

// -------------------------------------------------------------------------
// Export texture as PNG
// -------------------------------------------------------------------------
void GuiExportTexture(HWND hwnd)
{
    if (!g.hbmPreview)
    {
        MessageBoxW(hwnd, L"No texture loaded.", L"Export", MB_ICONWARNING);
        return;
    }

    wchar_t path[MAX_PATH] = {};
    if (g.selectedIdx >= 0 && g.selectedIdx < (int)g.arc.entries.size())
        swprintf(path, MAX_PATH, L"%hs.png",
            g.arc.entries[g.selectedIdx].name.c_str());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"PNG image\0*.png\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags       = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    Gdiplus::Bitmap bmp(g.hbmPreview, nullptr);
    CLSID pngClsid;
    // Get PNG encoder CLSID
    UINT num=0, sz=0;
    Gdiplus::GetImageEncodersSize(&num, &sz);
    std::vector<uint8_t> buf(sz);
    Gdiplus::GetImageEncoders(num, sz, (Gdiplus::ImageCodecInfo*)buf.data());
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    for (UINT i = 0; i < num; ++i)
    {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
        {
            pngClsid = codecs[i].Clsid;
            break;
        }
    }
    Gdiplus::Status st = bmp.Save(path, &pngClsid, nullptr);
    if (st == Gdiplus::Ok)
        GuiSetStatus(L"Exported to %ls", path);
    else
        MessageBoxW(hwnd, L"Failed to save PNG.", L"Export", MB_ICONERROR);
}

// -------------------------------------------------------------------------
// Open archive dialog
// -------------------------------------------------------------------------
void GuiOpenArchive(HWND hwnd, const std::wstring& hint)
{
    std::wstring path = hint;
    if (path.empty())
    {
        wchar_t buf[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hwnd;
        ofn.lpstrFilter = L"Texture archive index (*.txt)\0*.txt\0All files\0*.*\0";
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.Flags       = OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        path = buf;
    }

    Archive arc;
    std::string err;
    if (!ArchiveOpen(path, arc, err))
    {
        MessageBoxW(hwnd, Narrow2Wide(err).c_str(), L"Open Archive", MB_ICONERROR);
        return;
    }

    if (g.hbmPreview) { DeleteObject(g.hbmPreview); g.hbmPreview = nullptr; }
    g.arc      = std::move(arc);
    g.arcLoaded = true;
    g.selectedIdx = -1;
    g.texW = g.texH = 0;

    SetWindowTextW(g.hwndInfo, L"");
    GuiRefreshList();

    wchar_t title[256];
    swprintf(title, 256, L"DXTViewer — %ls", g.arc.baseName.c_str());
    SetWindowTextW(hwnd, title);
    GuiSetStatus(L"Loaded archive: %ls  (%zu textures)",
        g.arc.baseName.c_str(), g.arc.entries.size());
    InvalidateRect(g.hwndPreview, nullptr, TRUE);
}

// -------------------------------------------------------------------------
// About dialog
// -------------------------------------------------------------------------
static void ShowAbout(HWND hwnd)
{
    wchar_t msg[512];
    swprintf(msg, 512,
        L"DXTViewer  v" L"" APP_VERSION L"\n\n"
        L"Viewer and editor for SAMP Mobile DXT texture archives.\n\n"
        L"Supported formats: DXT1, DXT3, DXT5\n\n"
        L"Repository:\nhttps://github.com/" GITHUB_OWNER L"/" GITHUB_REPO L"\n\n"
        L"© 2025");
    MessageBoxW(hwnd, msg, L"About DXTViewer", MB_OK | MB_ICONINFORMATION);
}

// -------------------------------------------------------------------------
// Check for updates (runs in background thread, shows result in main thread)
// catch(...) works here because x86_64-posix-seh MinGW uses Win64 SEH for
// both C++ and native exceptions, so it does catch 0x6BA and similar.
// The VEH in main.cpp logs them first but returns CONTINUE_SEARCH, so
// catch(...) still gets to handle them — the thread and process stay alive.
// -------------------------------------------------------------------------
static void CheckForUpdates(HWND hwnd)
{
    GuiSetStatus(L"Checking for updates...");
    std::thread([hwnd]()
    {
        try
        {
            UpdateInfo info;
            std::string err;
            bool ok = UpdateCheck(info, err);

            if (!ok)
            {
                std::wstring we = Narrow2Wide(err);
                MessageBoxW(hwnd,
                    (L"Update check failed:\n" + we).c_str(),
                    L"Update", MB_ICONWARNING);
                SendMessageW(hwnd, WM_USER + 1, 0, 0);
                return;
            }

            if (!info.available)
            {
                MessageBoxW(hwnd,
                    L"You are running the latest version.",
                    L"Update", MB_ICONINFORMATION);
                SendMessageW(hwnd, WM_USER + 1, 0, 0);
                return;
            }

            std::wstring tag = Narrow2Wide(info.tagName);
            wchar_t msg[512];
            swprintf(msg, 512,
                L"New version available: %ls\n\n"
                L"Download and install now?\n"
                L"(The application will restart automatically)",
                tag.c_str());
            if (MessageBoxW(hwnd, msg, L"Update Available",
                            MB_YESNO | MB_ICONINFORMATION) == IDYES)
            {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                std::string dlErr;
                if (!UpdateDownloadAndReplace(info.downloadUrl, exePath, dlErr))
                {
                    std::wstring we2 = Narrow2Wide(dlErr);
                    MessageBoxW(hwnd,
                        (L"Download failed:\n" + we2).c_str(),
                        L"Update", MB_ICONERROR);
                }
                else
                {
                    MessageBoxW(hwnd,
                        L"Update downloaded. The application will now close and restart.",
                        L"Update", MB_ICONINFORMATION);
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
            }
            SendMessageW(hwnd, WM_USER + 1, 0, 0);
        }
        catch (...)
        {
            // Catches SEH exceptions (e.g. 0x6BA from WinHTTP when network is
            // unavailable). The VEH already logged the code; just show a dialog.
            LOG_WARN("Update thread: network exception caught — update skipped");
            MessageBoxW(hwnd,
                L"Update check failed: network error.\n"
                L"Please check your internet connection and try again.",
                L"Update", MB_ICONWARNING);
            SendMessageW(hwnd, WM_USER + 1, 0, 0);
        }
    }).detach();
}

// -------------------------------------------------------------------------
// Preview window procedure (right panel)
// -------------------------------------------------------------------------
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;

        // Background
        HBRUSH bgBrush = CreateSolidBrush(RGB(40, 40, 40));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        if (g.hbmPreview && g.texW > 0 && g.texH > 0)
        {
            // Draw checkerboard under the texture (for alpha)
            for (int y = 0; y < ch; y += CHECKER_SZ)
                for (int x = 0; x < cw; x += CHECKER_SZ)
                {
                    bool odd = ((x / CHECKER_SZ + y / CHECKER_SZ) & 1) != 0;
                    RECT cr = { x, y,
                                std::min(x + CHECKER_SZ, cw),
                                std::min(y + CHECKER_SZ, ch) };
                    HBRUSH cb = CreateSolidBrush(odd ? RGB(180,180,180) : RGB(255,255,255));
                    FillRect(hdc, &cr, cb);
                    DeleteObject(cb);
                }

            // Fit-to-window scaling
            float scaleX = (float)cw / g.texW;
            float scaleY = (float)ch / g.texH;
            float scale  = std::min(scaleX, scaleY);
            if (scale > 1.0f && g.texW < 64 && g.texH < 64)
                scale = std::min(scale, 8.0f); // cap upscale for tiny textures

            int dw = (int)(g.texW * scale);
            int dh = (int)(g.texH * scale);
            int dx = (cw - dw) / 2;
            int dy = (ch - dh) / 2;

            HDC memDC = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(memDC, g.hbmPreview);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, dx, dy, dw, dh,
                       memDC, 0, 0, g.texW, g.texH, SRCCOPY);

            SelectObject(memDC, old);
            DeleteDC(memDC);
        }
        else
        {
            const wchar_t* hint = g.arcLoaded
                ? L"Select a texture from the list"
                : L"File → Open Archive (Ctrl+O)";
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(160, 160, 160));
            DrawTextW(hdc, hint, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -------------------------------------------------------------------------
// Layout helper
// -------------------------------------------------------------------------
static void LayoutChildren(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int statusH = 22;
    int infoH   = 22;
    int searchH = 24;

    int lw = g.splitterX;
    int rw = w - lw - SPLITTER_W;
    int contentH = h - statusH;

    // Left panel: search at top, list below
    MoveWindow(g.hwndSearch, 0, 0, lw, searchH, TRUE);
    MoveWindow(g.hwndList,   0, searchH, lw, contentH - searchH, TRUE);

    // Splitter (handled by painting in MainWndProc)
    // Right panel: info bar at bottom, preview above
    int rx = lw + SPLITTER_W;
    MoveWindow(g.hwndInfo,    rx, contentH - infoH, rw, infoH, TRUE);
    MoveWindow(g.hwndPreview, rx, 0, rw, contentH - infoH, TRUE);

    // Status bar
    MoveWindow(g.hwndStatus, 0, h - statusH, w, statusH, TRUE);
}

// -------------------------------------------------------------------------
// Main window procedure
// -------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    // ---- WM_CREATE -------------------------------------------------------
    case WM_CREATE:
    {
        // Menu bar
        HMENU hMenu    = CreateMenu();
        HMENU hFile    = CreatePopupMenu();
        HMENU hView    = CreatePopupMenu();
        HMENU hHelp    = CreatePopupMenu();

        AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN, L"&Open Archive\tCtrl+O");
        AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit");

        AppendMenuW(hView, MF_STRING | MF_CHECKED, IDM_VIEW_RGBA,  L"&RGBA\tF1");
        AppendMenuW(hView, MF_STRING, IDM_VIEW_RGB,   L"R&GB only\tF2");
        AppendMenuW(hView, MF_STRING, IDM_VIEW_ALPHA, L"&Alpha channel\tF3");

        AppendMenuW(hHelp, MF_STRING, IDM_HELP_UPDATE, L"Check for &Updates");
        AppendMenuW(hHelp, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT,  L"&About");

        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&File");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"&View");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"&Help");
        SetMenu(hwnd, hMenu);

        // Search box
        g.hwndSearch = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 100, 24, hwnd, (HMENU)IDC_SEARCH, nullptr, nullptr);
        SendMessageW(g.hwndSearch, EM_SETCUEBANNER, TRUE,
            (LPARAM)L"Search textures...");

        // Texture list (ListView)
        g.hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            0, 24, 100, 100, hwnd, (HMENU)IDC_LISTVIEW, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g.hwndList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 160; col.pszText = (LPWSTR)L"Name";
        ListView_InsertColumn(g.hwndList, 0, &col);
        col.cx   = 52;  col.pszText = (LPWSTR)L"W";
        ListView_InsertColumn(g.hwndList, 1, &col);
        col.cx   = 52;  col.pszText = (LPWSTR)L"H";
        ListView_InsertColumn(g.hwndList, 2, &col);
        col.cx   = 48;  col.pszText = (LPWSTR)L"Fmt";
        ListView_InsertColumn(g.hwndList, 3, &col);

        // Preview panel (custom window)
        g.hwndPreview = CreateWindowExW(WS_EX_CLIENTEDGE, WC_PREVIEW, L"",
            WS_CHILD | WS_VISIBLE,
            200, 0, 100, 100, hwnd, (HMENU)IDC_PREVIEW_PANEL, nullptr, nullptr);

        // Info label
        g.hwndInfo = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            200, 400, 100, 22, hwnd, (HMENU)IDC_INFO_LABEL, nullptr, nullptr);
        SendMessageW(g.hwndInfo, WM_SETFONT,
            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        // Status bar
        g.hwndStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_STATUS_BAR, nullptr, nullptr);

        g.splitterX    = 320;
        g.chanMode     = ChannelMode::RGBA;
        g.arcLoaded    = false;
        g.selectedIdx  = -1;

        LayoutChildren(hwnd);
        return 0;
    }

    // ---- WM_SIZE ---------------------------------------------------------
    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    // ---- WM_PAINT (splitter) ---------------------------------------------
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int statusH = 22;
        int h = rc.bottom - statusH;
        RECT sr = { g.splitterX, 0, g.splitterX + SPLITTER_W, h };
        HBRUSH sb = CreateSolidBrush(RGB(60, 60, 60));
        FillRect(hdc, &sr, sb);
        DeleteObject(sb);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ---- Splitter drag ---------------------------------------------------
    case WM_LBUTTONDOWN:
    {
        int mx = LOWORD(lp);
        if (mx >= g.splitterX && mx < g.splitterX + SPLITTER_W)
        {
            g.draggingSplit = true;
            SetCapture(hwnd);
        }
        break;
    }
    case WM_MOUSEMOVE:
    {
        int mx = LOWORD(lp);
        if (mx >= g.splitterX && mx < g.splitterX + SPLITTER_W)
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        if (g.draggingSplit)
        {
            RECT rc; GetClientRect(hwnd, &rc);
            int newX = std::max(100, std::min(mx, (int)rc.right - 200));
            if (newX != g.splitterX)
            {
                g.splitterX = newX;
                LayoutChildren(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        break;
    }
    case WM_LBUTTONUP:
        if (g.draggingSplit) { g.draggingSplit = false; ReleaseCapture(); }
        break;

    // ---- Keyboard shortcuts ----------------------------------------------
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000)
        {
            if (wp == 'O') { GuiOpenArchive(hwnd); return 0; }
            if (wp == 'E') { GuiExportTexture(hwnd); return 0; }
        }
        if (wp == VK_F1) PostMessageW(hwnd, WM_COMMAND, IDM_VIEW_RGBA,  0);
        if (wp == VK_F2) PostMessageW(hwnd, WM_COMMAND, IDM_VIEW_RGB,   0);
        if (wp == VK_F3) PostMessageW(hwnd, WM_COMMAND, IDM_VIEW_ALPHA, 0);
        break;

    // ---- WM_COMMAND (menu) -----------------------------------------------
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_FILE_OPEN:
            GuiOpenArchive(hwnd);
            break;
        case IDM_FILE_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case IDM_VIEW_RGBA:
            g.chanMode = ChannelMode::RGBA;
            GuiSelectTexture(g.selectedIdx);
            {
                HMENU hm = GetMenu(hwnd);
                HMENU hv = GetSubMenu(hm, 1);
                CheckMenuRadioItem(hv, IDM_VIEW_RGBA, IDM_VIEW_ALPHA, IDM_VIEW_RGBA, MF_BYCOMMAND);
            }
            break;
        case IDM_VIEW_RGB:
            g.chanMode = ChannelMode::RGB;
            GuiSelectTexture(g.selectedIdx);
            {
                HMENU hm = GetMenu(hwnd);
                HMENU hv = GetSubMenu(hm, 1);
                CheckMenuRadioItem(hv, IDM_VIEW_RGBA, IDM_VIEW_ALPHA, IDM_VIEW_RGB, MF_BYCOMMAND);
            }
            break;
        case IDM_VIEW_ALPHA:
            g.chanMode = ChannelMode::Alpha;
            GuiSelectTexture(g.selectedIdx);
            {
                HMENU hm = GetMenu(hwnd);
                HMENU hv = GetSubMenu(hm, 1);
                CheckMenuRadioItem(hv, IDM_VIEW_RGBA, IDM_VIEW_ALPHA, IDM_VIEW_ALPHA, MF_BYCOMMAND);
            }
            break;
        case IDM_TEX_REPLACE:
            GuiReplaceTexture(hwnd);
            break;
        case IDM_TEX_EXPORT:
            GuiExportTexture(hwnd);
            break;
        case IDM_HELP_ABOUT:
            ShowAbout(hwnd);
            break;
        case IDM_HELP_UPDATE:
            CheckForUpdates(hwnd);
            break;
        // Search box change
        case IDC_SEARCH:
            if (HIWORD(wp) == EN_CHANGE)
            {
                wchar_t buf[256];
                GetWindowTextW(g.hwndSearch, buf, 256);
                GuiRefreshList(buf);
            }
            break;
        }
        break;

    // ---- WM_NOTIFY (ListView) --------------------------------------------
    case WM_NOTIFY:
    {
        NMHDR* hdr2 = (NMHDR*)lp;
        if (hdr2->idFrom == IDC_LISTVIEW)
        {
            if (hdr2->code == NM_CLICK || hdr2->code == LVN_ITEMCHANGED)
            {
                int sel = ListView_GetNextItem(g.hwndList, -1, LVNI_SELECTED);
                if (sel >= 0)
                {
                    LVITEMW lvi{};
                    lvi.mask  = LVIF_PARAM;
                    lvi.iItem = sel;
                    ListView_GetItem(g.hwndList, &lvi);
                    GuiSelectTexture((int)lvi.lParam);
                }
            }
            if (hdr2->code == NM_RCLICK)
            {
                // Context menu: Replace / Export
                int sel = ListView_GetNextItem(g.hwndList, -1, LVNI_SELECTED);
                if (sel >= 0)
                {
                    HMENU hCtx = CreatePopupMenu();
                    AppendMenuW(hCtx, MF_STRING, IDM_TEX_REPLACE, L"Replace texture...");
                    AppendMenuW(hCtx, MF_STRING, IDM_TEX_EXPORT,  L"Export as PNG...");
                    POINT pt; GetCursorPos(&pt);
                    TrackPopupMenu(hCtx, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(hCtx);
                }
            }
        }
        break;
    }

    // ---- Background thread result ----------------------------------------
    case WM_USER + 1:
        GuiSetStatus(L"Ready");
        break;

    // ---- Drop file -------------------------------------------------------
    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        DragQueryFileW(hDrop, 0, path, MAX_PATH);
        DragFinish(hDrop);
        // Open if it's a .txt file
        std::wstring p = path;
        if (p.size() >= 4 &&
            _wcsicmp(p.substr(p.size()-4).c_str(), L".txt") == 0)
            GuiOpenArchive(hwnd, p);
        break;
    }

    // ---- Close -----------------------------------------------------------
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        if (g.hbmPreview) DeleteObject(g.hbmPreview);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -------------------------------------------------------------------------
// GuiInit
// -------------------------------------------------------------------------
bool GuiInit(HINSTANCE hInst, int nCmdShow)
{
    // GDI+
    Gdiplus::GdiplusStartupInput gdipSI;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdipSI, nullptr);

    // Common controls
    INITCOMMONCONTROLSEX icce{};
    icce.dwSize = sizeof(icce);
    icce.dwICC  = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icce);

    // Register preview window class
    WNDCLASSEXW wcPrev{};
    wcPrev.cbSize        = sizeof(wcPrev);
    wcPrev.lpfnWndProc   = PreviewWndProc;
    wcPrev.hInstance     = hInst;
    wcPrev.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wcPrev.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcPrev.lpszClassName = WC_PREVIEW;
    RegisterClassExW(&wcPrev);

    // Register main window class
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = WC_MAIN;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc))
    {
        LOG_ERR("RegisterClassExW failed: %lu", GetLastError());
        return false;
    }

    g.hwndMain = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WC_MAIN, L"DXTViewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 720,
        nullptr, nullptr, hInst, nullptr);

    if (!g.hwndMain)
    {
        LOG_ERR("CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    ShowWindow(g.hwndMain, nCmdShow);
    UpdateWindow(g.hwndMain);
    LOG_INFO("GUI initialised");
    return true;
}

HWND GuiGetMainHwnd()
{
    return g.hwndMain;
}

int GuiRun()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    LOG_INFO("GUI message loop ended");
    return (int)msg.wParam;
}
