#pragma once
#include <cstdint>
#include <vector>


enum class DXTFormat
{
    Unknown,
    DXT1,   // BC1 — RGB + 1-bit alpha
    DXT3,   // BC2 — RGB + 4-bit explicit alpha
    DXT5,   // BC3 — RGB + 8-bit interpolated alpha
};

// Channel view mode for the preview window
enum class ChannelMode
{
    RGBA, 
    RGB,  
    Alpha, 
};

DXTFormat DXTGuessFormat(int alphaMode, int defaultFormat);

std::vector<uint8_t> DXTDecode(DXTFormat fmt,
                                int width, int height,
                                const uint8_t* srcData, size_t srcSize);

std::vector<uint8_t> DXTApplyChannelMode(const std::vector<uint8_t>& rgbaTopDown,
                                         int width, int height,
                                         ChannelMode mode);
