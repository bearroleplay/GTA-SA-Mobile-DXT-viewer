#include "dxt.h"
#include "logger.h"
#include <cstring>
#include <algorithm>

static void DecodeRGB565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
    g = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
    b = (uint8_t)(((c      ) & 0x1F) * 255 / 31);
}

static void DecodeDXT1Block(const uint8_t* src,
                             uint8_t* dst,      
                             int rowStride)     
{
    uint16_t c0 = (uint16_t)(src[0] | (src[1] << 8));
    uint16_t c1 = (uint16_t)(src[2] | (src[3] << 8));
    uint32_t lut = (uint32_t)(src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24));

    uint8_t r[4], g[4], b[4], a[4];

    DecodeRGB565(c0, r[0], g[0], b[0]); a[0] = 255;
    DecodeRGB565(c1, r[1], g[1], b[1]); a[1] = 255;

    if (c0 > c1)
    {
        r[2] = (uint8_t)((2 * r[0] + r[1] + 1) / 3);
        g[2] = (uint8_t)((2 * g[0] + g[1] + 1) / 3);
        b[2] = (uint8_t)((2 * b[0] + b[1] + 1) / 3);
        a[2] = 255;
        r[3] = (uint8_t)((r[0] + 2 * r[1] + 1) / 3);
        g[3] = (uint8_t)((g[0] + 2 * g[1] + 1) / 3);
        b[3] = (uint8_t)((b[0] + 2 * b[1] + 1) / 3);
        a[3] = 255;
    }
    else
    {
        r[2] = (uint8_t)((r[0] + r[1]) / 2);
        g[2] = (uint8_t)((g[0] + g[1]) / 2);
        b[2] = (uint8_t)((b[0] + b[1]) / 2);
        a[2] = 255;
        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0; 
    }

    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            int idx = (lut >> ((row * 4 + col) * 2)) & 0x3;
            uint8_t* p = dst + row * rowStride + col * 4;
            p[0] = r[idx]; p[1] = g[idx]; p[2] = b[idx]; p[3] = a[idx];
        }
    }
}

static void DecodeDXT3Block(const uint8_t* src, uint8_t* dst, int rowStride)
{
    uint8_t alpha[16];
    for (int i = 0; i < 8; ++i)
    {
        alpha[i * 2 + 0] = (uint8_t)((src[i] & 0x0F) * 17);
        alpha[i * 2 + 1] = (uint8_t)(((src[i] >> 4) & 0x0F) * 17);
    }


    uint8_t colBlock[64];
    memset(colBlock, 255, sizeof(colBlock));
    uint8_t fakeBlock[8];

    uint16_t c0 = (uint16_t)(src[8] | (src[9] << 8));
    uint16_t c1 = (uint16_t)(src[10] | (src[11] << 8));
    if (c0 <= c1)
    {
        fakeBlock[0] = src[10]; fakeBlock[1] = src[11];
        fakeBlock[2] = src[8];  fakeBlock[3] = src[9];
        uint32_t lut = (uint32_t)(src[12] | (src[13] << 8) |
                                   (src[14] << 16) | (src[15] << 24));
        uint32_t newLut = 0;
        for (int i = 0; i < 16; ++i)
        {
            uint32_t v = (lut >> (i * 2)) & 0x3;
            if (v == 0) v = 1; else if (v == 1) v = 0;
            newLut |= (v << (i * 2));
        }
        fakeBlock[4] = (uint8_t)(newLut & 0xFF);
        fakeBlock[5] = (uint8_t)((newLut >> 8) & 0xFF);
        fakeBlock[6] = (uint8_t)((newLut >> 16) & 0xFF);
        fakeBlock[7] = (uint8_t)((newLut >> 24) & 0xFF);
    }
    else
    {
        memcpy(fakeBlock, src + 8, 8);
    }

    DecodeDXT1Block(fakeBlock, colBlock, 16);

    // Write out with explicit alpha
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
        {
            uint8_t* p = dst + row * rowStride + col * 4;
            uint8_t* c = colBlock + (row * 4 + col) * 4;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
            p[3] = alpha[row * 4 + col];
        }
}

static void DecodeDXT5Block(const uint8_t* src, uint8_t* dst, int rowStride)
{
    uint8_t a0 = src[0], a1 = src[1];


    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= ((uint64_t)src[2 + i]) << (i * 8);

    uint8_t aTable[8];
    aTable[0] = a0; aTable[1] = a1;
    if (a0 > a1)
    {
        aTable[2] = (uint8_t)((6 * a0 + 1 * a1 + 3) / 7);
        aTable[3] = (uint8_t)((5 * a0 + 2 * a1 + 3) / 7);
        aTable[4] = (uint8_t)((4 * a0 + 3 * a1 + 3) / 7);
        aTable[5] = (uint8_t)((3 * a0 + 4 * a1 + 3) / 7);
        aTable[6] = (uint8_t)((2 * a0 + 5 * a1 + 3) / 7);
        aTable[7] = (uint8_t)((1 * a0 + 6 * a1 + 3) / 7);
    }
    else
    {
        aTable[2] = (uint8_t)((4 * a0 + 1 * a1 + 2) / 5);
        aTable[3] = (uint8_t)((3 * a0 + 2 * a1 + 2) / 5);
        aTable[4] = (uint8_t)((2 * a0 + 3 * a1 + 2) / 5);
        aTable[5] = (uint8_t)((1 * a0 + 4 * a1 + 2) / 5);
        aTable[6] = 0;
        aTable[7] = 255;
    }

    uint8_t alpha[16];
    for (int i = 0; i < 16; ++i)
        alpha[i] = aTable[(bits >> (i * 3)) & 0x7];

    // Decode colour
    uint8_t colBlock[64];
    DecodeDXT1Block(src + 8, colBlock, 16);

    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
        {
            uint8_t* p = dst + row * rowStride + col * 4;
            uint8_t* c = colBlock + (row * 4 + col) * 4;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
            p[3] = alpha[row * 4 + col];
        }
}

DXTFormat DXTGuessFormat(int alphaMode, int defaultFormat)
{
    (void)defaultFormat;
    if (alphaMode == 2)
        return DXTFormat::DXT5;
    if (alphaMode == 3)
        return DXTFormat::DXT3;
    return DXTFormat::DXT1;
}

std::vector<uint8_t> DXTDecode(DXTFormat fmt,
                                int width, int height,
                                const uint8_t* srcData, size_t srcSize)
{
    if (width <= 0 || height <= 0 || !srcData || srcSize == 0)
        return {};

    int bw = (width  + 3) / 4;
    int bh = (height + 3) / 4;
    int blockSize = (fmt == DXTFormat::DXT1) ? 8 : 16;
    size_t needed = (size_t)(bw * bh * blockSize);

    if (srcSize < needed)
    {
        LOG_WARN("DXTDecode: srcSize %zu < needed %zu for %dx%d fmt=%d",
            srcSize, needed, width, height, (int)fmt);
    }

    std::vector<uint8_t> rgba(width * height * 4, 0);
    int rowStride = width * 4;

    size_t srcOff = 0;
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            if (srcOff + blockSize > srcSize)
                goto done;

            int px = bx * 4;
            int py = by * 4;

            uint8_t tmp[4 * 4 * 4] = {};
            const uint8_t* src = srcData + srcOff;

            switch (fmt)
            {
            case DXTFormat::DXT1: DecodeDXT1Block(src, tmp, 16); break;
            case DXTFormat::DXT3: DecodeDXT3Block(src, tmp, 16); break;
            case DXTFormat::DXT5: DecodeDXT5Block(src, tmp, 16); break;
            default: break;
            }

            for (int dy = 0; dy < 4 && py + dy < height; ++dy)
                for (int dx = 0; dx < 4 && px + dx < width; ++dx)
                {
                    uint8_t* dst = rgba.data() + (py + dy) * rowStride + (px + dx) * 4;
                    uint8_t* src2 = tmp + dy * 16 + dx * 4;
                    dst[0] = src2[0]; dst[1] = src2[1];
                    dst[2] = src2[2]; dst[3] = src2[3];
                }

            srcOff += blockSize;
        }
    }

done:
    LOG_DEBUG("DXTDecode: %dx%d fmt=%d decoded %zu blocks",
        width, height, (int)fmt, srcOff / blockSize);
    return rgba;
}

std::vector<uint8_t> DXTApplyChannelMode(const std::vector<uint8_t>& rgba,
                                          int width, int height,
                                          ChannelMode mode)
{
    int n = width * height;
    std::vector<uint8_t> out(n * 4);

    switch (mode)
    {
    case ChannelMode::RGBA:
        memcpy(out.data(), rgba.data(), n * 4);
        break;

    case ChannelMode::RGB:
        for (int i = 0; i < n; ++i)
        {
            out[i*4+0] = rgba[i*4+0];
            out[i*4+1] = rgba[i*4+1];
            out[i*4+2] = rgba[i*4+2];
            out[i*4+3] = 255;
        }
        break;

    case ChannelMode::Alpha:
        for (int i = 0; i < n; ++i)
        {
            uint8_t a = rgba[i*4+3];
            out[i*4+0] = a;
            out[i*4+1] = a;
            out[i*4+2] = a;
            out[i*4+3] = 255;
        }
        break;
    }

    return out;
}
