#include "VulkanTextureDecode.h"

#include <cstddef>

namespace
{
void DecodeColorBlock(const uint8_t* block, bool allowTransparent, uint8_t colors[4][4])
{
    const uint32_t c0 = block[0] | (static_cast<uint32_t>(block[1]) << 8);
    const uint32_t c1 = block[2] | (static_cast<uint32_t>(block[3]) << 8);
    const uint32_t endpoints[2] = { c0, c1 };
    for (int endpoint = 0; endpoint < 2; ++endpoint)
    {
        const uint32_t c = endpoints[endpoint];
        colors[endpoint][0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        colors[endpoint][1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        colors[endpoint][2] = static_cast<uint8_t>((c & 31) * 255 / 31);
        colors[endpoint][3] = 255;
    }

    if (allowTransparent && c0 <= c1)
    {
        for (int channel = 0; channel < 3; ++channel)
            colors[2][channel] = static_cast<uint8_t>((colors[0][channel] + colors[1][channel]) / 2);
        colors[2][3] = 255;
        colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
    }
    else
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            colors[2][channel] = static_cast<uint8_t>((2 * colors[0][channel] + colors[1][channel]) / 3);
            colors[3][channel] = static_cast<uint8_t>((colors[0][channel] + 2 * colors[1][channel]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    }
}
}

bool DecodeVulkanDxtBaseLevel(const uint8_t* compressed, int width, int height,
                              bool dxt1, bool dxt3, bool dxt5,
                              std::vector<uint8_t>& rgba)
{
    if (!compressed || width <= 0 || height <= 0 || (!dxt1 && !dxt3 && !dxt5))
        return false;
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount > static_cast<size_t>(-1) / 4)
        return false;

    rgba.resize(pixelCount * 4);
    const int blockBytes = dxt1 ? 8 : 16;
    const int blocksWide = (width + 3) / 4;
    const int blocksHigh = (height + 3) / 4;
    for (int by = 0; by < blocksHigh; ++by)
    {
        for (int bx = 0; bx < blocksWide; ++bx)
        {
            const uint8_t* block = compressed + (static_cast<size_t>(by) * blocksWide + bx) * blockBytes;
            const uint8_t* colorBlock = block + (dxt1 ? 0 : 8);
            uint8_t colors[4][4];
            DecodeColorBlock(colorBlock, dxt1, colors);
            const uint32_t selectors = colorBlock[4] | (static_cast<uint32_t>(colorBlock[5]) << 8) |
                (static_cast<uint32_t>(colorBlock[6]) << 16) | (static_cast<uint32_t>(colorBlock[7]) << 24);
            uint8_t alpha[16];
            for (uint8_t& value : alpha) value = 255;

            if (dxt3)
            {
                for (int i = 0; i < 16; ++i)
                {
                    const uint8_t packed = block[i / 2];
                    alpha[i] = static_cast<uint8_t>(((i & 1) ? packed >> 4 : packed & 15) * 17);
                }
            }
            else if (dxt5)
            {
                uint8_t table[8] = { block[0], block[1] };
                if (table[0] > table[1])
                {
                    for (int i = 1; i <= 6; ++i)
                        table[i + 1] = static_cast<uint8_t>(((7 - i) * table[0] + i * table[1]) / 7);
                }
                else
                {
                    for (int i = 1; i <= 4; ++i)
                        table[i + 1] = static_cast<uint8_t>(((5 - i) * table[0] + i * table[1]) / 5);
                    table[6] = 0;
                    table[7] = 255;
                }
                uint64_t selectorsAlpha = 0;
                for (int i = 0; i < 6; ++i)
                    selectorsAlpha |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i)
                    alpha[i] = table[(selectorsAlpha >> (3 * i)) & 7];
            }

            for (int py = 0; py < 4; ++py)
            {
                const int y = by * 4 + py;
                if (y >= height) continue;
                for (int px = 0; px < 4; ++px)
                {
                    const int x = bx * 4 + px;
                    if (x >= width) continue;
                    const int pixel = py * 4 + px;
                    const uint8_t* color = colors[(selectors >> (2 * pixel)) & 3];
                    uint8_t* out = &rgba[(static_cast<size_t>(y) * width + x) * 4];
                    out[0] = color[0]; out[1] = color[1]; out[2] = color[2];
                    out[3] = dxt1 ? color[3] : alpha[pixel];
                }
            }
        }
    }
    return true;
}
