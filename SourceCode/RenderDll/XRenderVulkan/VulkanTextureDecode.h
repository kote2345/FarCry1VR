#pragma once

#include <cstdint>
#include <vector>

bool DecodeVulkanDxtBaseLevel(const uint8_t* compressed, int width, int height,
                              bool dxt1, bool dxt3, bool dxt5,
                              std::vector<uint8_t>& rgba);
