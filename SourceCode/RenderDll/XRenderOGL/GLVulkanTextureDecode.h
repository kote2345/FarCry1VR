#ifndef CRY_GL_VULKAN_TEXTURE_DECODE_H
#define CRY_GL_VULKAN_TEXTURE_DECODE_H

#include <vector>

class STexPic;
typedef unsigned char byte;

bool DecodeDxtBaseLevel(const byte* compressed, int width, int height,
                        bool dxt1, bool dxt3, bool dxt5, std::vector<byte>& rgba);

#endif
