#pragma once

namespace CryVR {
// Temporary comparison build: keep albedo, baked lighting, geometric
// diffuse shading and grass; omit normal maps and specular contributions.
// Original graphics settings and material technique selection stay intact.
constexpr bool kVulkanBasicRenderTest = true;
}
