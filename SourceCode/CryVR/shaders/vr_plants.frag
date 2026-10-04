#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#ifdef PLANT_EARLY_FRAGMENT_TESTS
layout(early_fragment_tests) in;
#endif
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 70) const bool stockZeroAlphaBlendNoOp = false;
layout(constant_id = 78) const bool stockPlantImplicitLod = false;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
#include "scene_plants_uniforms.glsl"
#include "scene_alpha_test.glsl"
#include "scene_fog.glsl"
vec4 applySceneFog(vec4 color) { return applyStockSceneFog(color); }
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 9) in vec3 clipPosition;
layout(location = 10) in float fogAmount;
layout(location = 0) out vec4 outColor;
void main() {
    float scale = textureStageTransforms.textureLodBias.x;
    // The unbiased path can use hardware implicit gradients; this fetch
    // precedes all discard branches, so coverage and mip selection are intact.
    vec4 texel = stockPlantImplicitLod ? texture(baseColorTexture, texCoord) :
        textureGrad(baseColorTexture, texCoord, dFdx(texCoord) * scale, dFdy(texCoord) * scale);
    if (stockFragmentDiscardEnabled &&
        dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    // CGRCPlants opacity is texture alpha * the vertex program's Ambient.w.
    // Reject empty texels before shading/fog; all derivatives precede discard.
    float alpha = clamp(texel.a * vertexColor.a, 0.0, 1.0);
    if (stockZeroAlphaBlendNoOp && alpha == 0.0) {
        // This variant forces early fragment tests and has neither depth nor
        // stencil writes. Discard is therefore safe after early depth and
        // avoids issuing a blend/store for every empty texel of a leaf card.
        discard;
    }
    if (stockFragmentDiscardEnabled &&
        !stockAlphaTestPasses(alpha, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    // CGRCPlants: tex2D(baseMap) * IN.Color, HDREncode RGB (LDR x2).
    // Ambient and vertex color have already been multiplied in the vertex stage.
    // RGB is clamped to the LDR attachment range. Relaxed precision allows
    // mobile drivers to pack color/fog arithmetic; positions, texture gradients
    // and alpha coverage deliberately remain full precision.
    mediump vec3 albedo = texel.rgb;
    mediump vec3 lightColor = vertexColor.rgb;
    mediump vec3 shadedColor = clamp(albedo * lightColor * 2.0, 0.0, 1.0);
    mediump vec3 fogColor = textureStageTransforms.fogColor.rgb;
    mediump float fogFactor = clamp(fogAmount, 0.0, 1.0);
    mediump vec3 rgb = mix(shadedColor, fogColor, fogFactor);
    outColor = vec4(rgb, alpha);
}
