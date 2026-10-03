#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 60) const uint stage0UsesTexCoord1 = 0u;
layout(constant_id = 1) const int stage0ColorMode = 1;
layout(constant_id = 2) const int stage0AlphaMode = 1;
layout(constant_id = 3) const int stage1ColorMode = 1;
layout(constant_id = 4) const int stage1AlphaMode = 1;
layout(constant_id = 5) const uint stage0ColorArg = 0x0a1u;
layout(constant_id = 6) const uint stage0AlphaArg = 0x0a1u;
#define stage0Constant textureStageTransforms.textureConstants[0][0]
layout(constant_id = 8) const uint stage1ColorArg = 0x0a1u;
layout(constant_id = 9) const uint stage1AlphaArg = 0x0a1u;
#define stage1Constant textureStageTransforms.textureConstants[0][1]
layout(constant_id = 59) const uint stage1UsesTexCoord1 = 1u;
layout(constant_id = 11) const uint hasSecondaryColor = 0u;
layout(constant_id = 12) const int stage2ColorMode = 1;
layout(constant_id = 13) const int stage2AlphaMode = 1;
layout(constant_id = 14) const uint stage2ColorArg = 0x0a1u;
layout(constant_id = 15) const uint stage2AlphaArg = 0x0a1u;
#define stage2Constant textureStageTransforms.textureConstants[0][2]
layout(constant_id = 17) const uint hasSecondTexture = 0u;
layout(constant_id = 18) const uint stage2UsesTexCoord1 = 1u;
layout(constant_id = 19) const int stage3ColorMode = 1;
layout(constant_id = 20) const int stage3AlphaMode = 1;
layout(constant_id = 21) const uint stage3ColorArg = 0x0a1u;
layout(constant_id = 22) const uint stage3AlphaArg = 0x0a1u;
#define stage3Constant textureStageTransforms.textureConstants[0][3]
layout(constant_id = 24) const uint stage3UsesTexCoord1 = 1u;
layout(constant_id = 25) const uint hasFourthTexture = 0u;
layout(constant_id = 26) const uint hasThirdTexture = 0u;
layout(constant_id = 27) const int stage4ColorMode = 1;
layout(constant_id = 28) const int stage4AlphaMode = 1;
layout(constant_id = 29) const uint stage4ColorArg = 0x0a1u;
layout(constant_id = 30) const uint stage4AlphaArg = 0x0a1u;
#define stage4Constant textureStageTransforms.textureConstants[1][0]
layout(constant_id = 32) const uint stage4UsesTexCoord1 = 1u;
layout(constant_id = 33) const uint hasFifthTexture = 0u;
layout(constant_id = 34) const int stage5ColorMode = 1;
layout(constant_id = 35) const int stage5AlphaMode = 1;
layout(constant_id = 36) const uint stage5ColorArg = 0x0a1u;
layout(constant_id = 37) const uint stage5AlphaArg = 0x0a1u;
#define stage5Constant textureStageTransforms.textureConstants[1][1]
layout(constant_id = 39) const uint stage5UsesTexCoord1 = 1u;
layout(constant_id = 40) const uint hasSixthTexture = 0u;
layout(constant_id = 41) const int stage6ColorMode = 1;
layout(constant_id = 42) const int stage6AlphaMode = 1;
layout(constant_id = 43) const uint stage6ColorArg = 0x0a1u;
layout(constant_id = 44) const uint stage6AlphaArg = 0x0a1u;
#define stage6Constant textureStageTransforms.textureConstants[1][2]
layout(constant_id = 46) const uint stage6UsesTexCoord1 = 1u;
layout(constant_id = 47) const uint hasSeventhTexture = 0u;
layout(constant_id = 48) const int stage7ColorMode = 1;
layout(constant_id = 49) const int stage7AlphaMode = 1;
layout(constant_id = 50) const uint stage7ColorArg = 0x0a1u;
layout(constant_id = 51) const uint stage7AlphaArg = 0x0a1u;
#define stage7Constant textureStageTransforms.textureConstants[1][3]
layout(constant_id = 53) const uint stage7UsesTexCoord1 = 1u;
layout(constant_id = 54) const uint hasEighthTexture = 0u;
layout(constant_id = 55) const float stage4LodBias = 0.0;
layout(constant_id = 56) const float stage5LodBias = 0.0;
layout(constant_id = 57) const float stage6LodBias = 0.0;
layout(constant_id = 58) const float stage7LodBias = 0.0;
layout(constant_id = 61) const uint directionalLightmap = 0u;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 0) uniform sampler2D secondaryTexture;
layout(set = 2, binding = 0) uniform sampler2D tertiaryTexture;
layout(set = 3, binding = 0) uniform sampler2D fourthTexture;
layout(set = 4, binding = 0) uniform sampler2D fifthTexture;
layout(set = 5, binding = 0) uniform sampler2D sixthTexture;
layout(set = 6, binding = 0) uniform sampler2D seventhTexture;
layout(set = 7, binding = 0) uniform sampler2D eighthTexture;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8];
    vec4 uvRow1[8];
    vec4 uvRowQ[8];
    vec4 fogColor;
    vec4 fogModeDensityStart;
    vec4 fogEndDepthRange;
    vec4 materialParams;
    vec4 primaryColor;
    vec4 primaryColorMask;
    vec4 textureLodBias;
    vec4 clipPlane;
    vec4 objectLightPositionRadius;
    vec4 lightColorAmbient;
    vec4 materialAmbient;
    vec4 shadowRow0[8];
    vec4 shadowRow1[8];
    vec4 shadowRow2[8];
    vec4 shadowRow3[8];
    vec4 shadowMapStageMask[2];
    vec4 terrainProjectionS[8];
    vec4 terrainProjectionT[8];
    vec4 linearPlanes[32];
    vec4 linearMatrixRows[32];
    vec4 linearControls[8];
    vec4 fixedLights[32];
    vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2]; vec4 fogEye1Ray;
} textureStageTransforms;
#define VR_SCENE_SPECULAR_OCCLUSION_ALIAS
#include "scene_lighting.glsl"
#include "scene_fog.glsl"
vec4 applySceneFog(vec4 color) { return applyStockSceneFog(color); }
layout(location = 0) in vec2 texCoord0;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec2 texCoord1;
layout(location = 3) in vec4 secondaryColor;
layout(location = 9) in vec3 clipPosition;
layout(location = 12) in vec3 objectPosition;
layout(location = 13) in vec3 objectNormal;
layout(location = 14) flat in uint hasMaterialLighting;
layout(location = 15) in vec3 stockSeparateSpecular;
layout(location = 0) out vec4 outColor;
vec4 sampleBaseTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.x;
    return sampleStockTextureStage(0u, baseColorTexture, uv, scale, objectPosition);
}
vec4 sampleSecondaryTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.y;
    return sampleStockTextureStage(1u, secondaryTexture, uv, scale, objectPosition);
}
vec4 sampleTertiaryTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.z;
    return sampleStockTextureStage(2u, tertiaryTexture, uv, scale, objectPosition);
}
vec4 sampleFourthTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.w;
    return sampleStockTextureStage(3u, fourthTexture, uv, scale, objectPosition);
}
vec4 sampleFifthTexture(vec2 uv) {
    float scale = exp2(clamp(stage4LodBias, -16.0, 16.0));
    return sampleStockTextureStage(4u, fifthTexture, uv, scale, objectPosition);
}
vec4 sampleSixthTexture(vec2 uv) {
    float scale = exp2(clamp(stage5LodBias, -16.0, 16.0));
    return sampleStockTextureStage(5u, sixthTexture, uv, scale, objectPosition);
}
vec4 sampleSeventhTexture(vec2 uv) {
    float scale = exp2(clamp(stage6LodBias, -16.0, 16.0));
    return sampleStockTextureStage(6u, seventhTexture, uv, scale, objectPosition);
}
vec4 sampleEighthTexture(vec2 uv) {
    float scale = exp2(clamp(stage7LodBias, -16.0, 16.0));
    return sampleStockTextureStage(7u, eighthTexture, uv, scale, objectPosition);
}
vec2 transformStageUv(uint stage, vec2 uv) {
    return stockTerrainStageTexCoord(stage, uv, objectPosition);
}
vec4 applyMaterialOverrides(vec4 c) {
    if (textureStageTransforms.materialParams.z > 0.5) c.rgb *= textureStageTransforms.materialParams.x;
    else c.a *= textureStageTransforms.materialParams.x;
    return c;
}
vec3 sourceRgb(uint selector, vec4 texel, vec4 primary, vec4 previous, vec4 constantColor) {
    if (selector == 0u) return constantColor.rgb;
    if (selector == 2u) return primary.rgb;
    if (selector == 3u) return previous.rgb;
    if (selector == 4u) return constantColor.rgb;
    return texel.rgb;
}
float sourceAlpha(uint selector, vec4 texel, vec4 primary, vec4 previous, vec4 constantColor) {
    if (selector == 0u) return constantColor.a;
    if (selector == 2u) return primary.a;
    if (selector == 3u) return previous.a;
    if (selector == 4u) return constantColor.a;
    return texel.a;
}
vec3 combineRgb(int mode, vec3 a, vec3 b, float blendFactor, vec3 third, float arg1Alpha) {
    if (mode == 0) return a;
    if (mode == 2) return a * b * 2.0;
    if (mode == 3) return a * b * 4.0;
    if (mode == 4) return a + b;
    if (mode == 5) return a + b - vec3(0.5);
    if (mode == 17) return (a + b - vec3(0.5)) * 2.0;
    if (mode == 6 || mode == 7) return mix(b, a, blendFactor);
    if (mode == 8) return b;
    if (mode == 10) return a - b;
    if (mode == 9) return third + a * b;
    if (mode == 11) return a + b * arg1Alpha;
    if (mode == 12) return a * b + vec3(arg1Alpha);
    if (mode == 13) return a + b * (1.0 - arg1Alpha);
    if (mode == 14) return (vec3(1.0) - a) * b + vec3(arg1Alpha);
    if (mode == 15) return vec3(dot(a * 2.0 - 1.0, b * 2.0 - 1.0));
    if (mode == 16) return mix(b, a, third);
    return a * b;
}
float combineAlpha(int mode, float a, float b, float third, float blendFactor) {
    if (mode == 0) return a;
    // GL EF_SetColorOp scales RGB only; alpha 2X/4X remains MODULATE.
    if (mode == 2 || mode == 3) return a * b;
    if (mode == 6 || mode == 7) return mix(b, a, blendFactor);
    if (mode == 4) return a + b;
    if (mode == 5) return a + b - 0.5;
    if (mode == 8) return b;
    if (mode == 10) return a - b;
    if (mode == 9) return third + a * b;
    if (mode == 16) return mix(b, a, third);
    return a * b;
}
vec4 combineTextureStage(int colorMode, int alphaMode, uint colorArg, uint alphaArg,
                         uint constantColor, vec4 texel, vec4 primary,
                         vec4 previous, float blendFactor) {
    vec4 stageConstant = unpackUnorm4x8(constantColor);
    vec3 a = sourceRgb(colorArg & 7u, texel, primary, previous, stageConstant);
    vec3 b = sourceRgb((colorArg >> 3) & 7u, texel, primary, previous, stageConstant);
    vec3 c = sourceRgb((colorArg >> 6) & 7u, texel, primary, previous, stageConstant);
    float arg1Alpha = sourceAlpha(colorArg & 7u, texel, primary, previous, stageConstant);
    float alphaA = sourceAlpha(alphaArg & 7u, texel, primary, previous, stageConstant);
    float alphaB = sourceAlpha((alphaArg >> 3) & 7u, texel, primary, previous, stageConstant);
    float alphaC = sourceAlpha((alphaArg >> 6) & 7u, texel, primary, previous, stageConstant);
    float rgbBlendFactor = sourceAlpha((colorArg >> 6) & 7u, texel,
                                       primary, previous, stageConstant);
    return clamp(vec4(combineRgb(colorMode, a, b, rgbBlendFactor, c, arg1Alpha),
                      combineAlpha(alphaMode, alphaA, alphaB, alphaC, blendFactor)), 0.0, 1.0);
}
void main() {
    if (stockTerrainMarker() > -15.5 && stockTerrainMarker() < -14.5) {
        vec2 uv = stockTerrainStageTexCoord(0u, texCoord0, objectPosition);
        vec4 albedo = sampleBaseTexture(uv);
        float coverage = stockReceiverShadowCoverage(1u, secondaryTexture, objectPosition) + stockReceiverShadowCoverage(2u, tertiaryTexture, objectPosition) + stockReceiverShadowCoverage(3u, fourthTexture, objectPosition);
        outColor = applySceneFog(stockReceiverShadowColor(albedo, coverage));
        return;
    }
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5 && textureStageTransforms.materialParams.w < 1.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 primaryColor = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
    if (!stockFeatureDisabled(STOCK_NO_FRAGMENT_LIGHTING))
        primaryColor.rgb *= evaluateStockLighting(objectPosition, objectNormal,
        stockTerrainStageTexCoord(1u, texCoord1, objectPosition), hasMaterialLighting);
    vec2 stage0TexCoord = stockTerrainStageTexCoord(0u,
        stage0UsesTexCoord1 != 0u ? texCoord1 : texCoord0, objectPosition);
    vec4 base = sampleBaseTexture(stage0TexCoord);
    vec4 env0 = unpackUnorm4x8(stage0Constant);
    vec3 s0a = sourceRgb(stage0ColorArg & 7u, base, primaryColor, primaryColor, env0);
    vec3 s0b = sourceRgb((stage0ColorArg >> 3) & 7u, base, primaryColor, primaryColor, env0);
    float s0aa = sourceAlpha(stage0AlphaArg & 7u, base, primaryColor, primaryColor, env0);
    float s0ab = sourceAlpha((stage0AlphaArg >> 3) & 7u, base, primaryColor, primaryColor, env0);
    vec3 s0c = sourceRgb((stage0ColorArg >> 6) & 7u, base, primaryColor, primaryColor, env0);
    float s0ac = sourceAlpha((stage0AlphaArg >> 6) & 7u, base, primaryColor, primaryColor, env0);
    float s0Arg1Alpha = sourceAlpha(stage0ColorArg & 7u, base, primaryColor, primaryColor, env0);
    vec4 previous = vec4(combineRgb(stage0ColorMode, s0a, s0b,
                                    sourceAlpha((stage0ColorArg >> 6) & 7u, base,
                                                primaryColor, primaryColor, env0), s0c, s0Arg1Alpha),
                         combineAlpha(stage0AlphaMode, s0aa, s0ab, s0ac,
                                      stage0AlphaMode == 6 ? primaryColor.a : base.a));
    previous = clamp(previous, 0.0, 1.0);
    vec2 stage1TexCoord = stockTerrainStageTexCoord(1u,
        stage1UsesTexCoord1 != 0u ? texCoord1 : texCoord0, objectPosition);
    vec4 layer = sampleSecondaryTexture(stage1TexCoord);
    if (stockFeatureDisabled(STOCK_DIRECTIONAL_LIGHTMAP_FAST)) {
        // CGRCAmbientTempl TEMP_DOT3LM replaces all intermediate RGB
        // combiners. Retain stage 0's alpha and the original four samples.
        vec4 irradiance = sampleTertiaryTexture(transformStageUv(2u,
            stage2UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
        vec4 direction = sampleFourthTexture(transformStageUv(3u,
            stage3UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
        float ndotl = clamp(dot(direction.rgb * 2.0 - 1.0, layer.rgb * 2.0 - 1.0), 0.0, 1.0);
        float intensity = ndotl * irradiance.a + (1.0 - irradiance.a);
        vec3 bakedDiffuse = textureStageTransforms.fogEndDepthRange.w > 0.5 ?
            irradiance.rgb * direction.a * intensity * textureStageTransforms.fogEndDepthRange.w : vec3(0.0);
        vec4 result = vec4(base.rgb * (textureStageTransforms.materialAmbient.rgb *
            primaryColor.rgb + bakedDiffuse), previous.a);
        result = applyMaterialOverrides(result);
        if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(result.a,
            textureStageTransforms.materialParams.y, alphaTestMode)) discard;
        result.rgb += stockSeparateSpecular;
        if (textureStageTransforms.materialAmbient.w > 1.5)
            result.rgb = clamp(result.rgb, 0.0, 1.0);
        outColor = applySceneFog(result);
        return;
    }
    vec4 env1 = unpackUnorm4x8(stage1Constant);
    vec3 s1a = sourceRgb(stage1ColorArg & 7u, layer, primaryColor, previous, env1);
    vec3 s1b = sourceRgb((stage1ColorArg >> 3) & 7u, layer, primaryColor, previous, env1);
    float s1aa = sourceAlpha(stage1AlphaArg & 7u, layer, primaryColor, previous, env1);
    float s1ab = sourceAlpha((stage1AlphaArg >> 3) & 7u, layer, primaryColor, previous, env1);
    vec3 s1c = sourceRgb((stage1ColorArg >> 6) & 7u, layer, primaryColor, previous, env1);
    float s1ac = sourceAlpha((stage1AlphaArg >> 6) & 7u, layer, primaryColor, previous, env1);
    float s1Arg1Alpha = sourceAlpha(stage1ColorArg & 7u, layer, primaryColor, previous, env1);
    vec4 color = vec4(combineRgb(stage1ColorMode, s1a, s1b,
                                 sourceAlpha((stage1ColorArg >> 6) & 7u, layer,
                                             primaryColor, previous, env1), s1c, s1Arg1Alpha),
                      combineAlpha(stage1AlphaMode, s1aa, s1ab, s1ac,
                                   stage1AlphaMode == 6 ? primaryColor.a : layer.a));
    color = hasSecondTexture != 0u ? clamp(color, 0.0, 1.0) : previous;
    if (textureStageTransforms.fogEndDepthRange.w > 0.5 && hasSecondTexture != 0u)
    {
        // CGRCAmbientTempl's non-DOT3 lightmap path adds ambient and the
        // HDREncodeLM-scaled irradiance. The generic stage combiner clamps
        // each intermediate and cannot reproduce this additive result.
        color.rgb = base.rgb *
            (textureStageTransforms.materialAmbient.rgb * primaryColor.rgb +
             layer.rgb * textureStageTransforms.fogEndDepthRange.w);
        color.a = previous.a;
    }
    vec4 tertiary = sampleTertiaryTexture(transformStageUv(2u,
        stage2UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    vec4 env2 = unpackUnorm4x8(stage2Constant);
    vec3 s2a = sourceRgb(stage2ColorArg & 7u, tertiary, primaryColor, color, env2);
    vec3 s2b = sourceRgb((stage2ColorArg >> 3) & 7u, tertiary, primaryColor, color, env2);
    float s2aa = sourceAlpha(stage2AlphaArg & 7u, tertiary, primaryColor, color, env2);
    float s2ab = sourceAlpha((stage2AlphaArg >> 3) & 7u, tertiary, primaryColor, color, env2);
    vec3 s2c = sourceRgb((stage2ColorArg >> 6) & 7u, tertiary, primaryColor, color, env2);
    float s2ac = sourceAlpha((stage2AlphaArg >> 6) & 7u, tertiary, primaryColor, color, env2);
    float s2Arg1Alpha = sourceAlpha(stage2ColorArg & 7u, tertiary, primaryColor, color, env2);
    vec4 stage2Color = vec4(combineRgb(stage2ColorMode, s2a, s2b,
                            sourceAlpha((stage2ColorArg >> 6) & 7u, tertiary,
                                        primaryColor, color, env2),
                            s2c, s2Arg1Alpha),
                 combineAlpha(stage2AlphaMode, s2aa, s2ab, s2ac,
                              stage2AlphaMode == 6 ? primaryColor.a : tertiary.a));
    color = hasThirdTexture != 0u ? clamp(stage2Color, 0.0, 1.0) : color;
    vec4 fourth = sampleFourthTexture(transformStageUv(3u,
        stage3UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    vec4 env3 = unpackUnorm4x8(stage3Constant);
    vec3 s3a = sourceRgb(stage3ColorArg & 7u, fourth, primaryColor, color, env3);
    vec3 s3b = sourceRgb((stage3ColorArg >> 3) & 7u, fourth, primaryColor, color, env3);
    float s3aa = sourceAlpha(stage3AlphaArg & 7u, fourth, primaryColor, color, env3);
    float s3ab = sourceAlpha((stage3AlphaArg >> 3) & 7u, fourth, primaryColor, color, env3);
    vec3 s3c = sourceRgb((stage3ColorArg >> 6) & 7u, fourth, primaryColor, color, env3);
    float s3ac = sourceAlpha((stage3AlphaArg >> 6) & 7u, fourth, primaryColor, color, env3);
    float s3Arg1Alpha = sourceAlpha(stage3ColorArg & 7u, fourth, primaryColor, color, env3);
    vec4 stage3Color = vec4(combineRgb(stage3ColorMode, s3a, s3b,
                                       sourceAlpha((stage3ColorArg >> 6) & 7u, fourth,
                                                   primaryColor, color, env3),
                                       s3c, s3Arg1Alpha),
                            combineAlpha(stage3AlphaMode, s3aa, s3ab, s3ac,
                                         stage3AlphaMode == 6 ? primaryColor.a : fourth.a));
    color = hasFourthTexture != 0u ? clamp(stage3Color, 0.0, 1.0) : color;
    if (stockTerrainFogLayerCount() > 0) {
        vec4 objectPosition4 = vec4(objectPosition, 1.0);
        vec2 fogEnterUv = vec2(dot(textureStageTransforms.terrainProjectionS[4], objectPosition4),
                               dot(textureStageTransforms.terrainProjectionT[4], objectPosition4));
        vec2 fogUv = vec2(dot(textureStageTransforms.terrainProjectionS[5], objectPosition4),
                          dot(textureStageTransforms.terrainProjectionT[5], objectPosition4));
        vec4 fog;
        vec3 terrainColor = base.rgb * vertexColor.a *
                            textureStageTransforms.materialAmbient.rgb * 2.0;
        if (stockTerrainFogLayerCount() == 1) {
            vec4 fogEnter = sampleTertiaryTexture(fogEnterUv);
            fog = sampleFourthTexture(fogUv);
            vec3 detail = mix(vec3(0.5), layer.rgb, vertexColor.r);
            terrainColor *= detail * 2.0;
            fog.a *= fogEnter.a;
        } else {
            // CGVProgTerrain_2Layers_VF mirrors the fog texture horizontally
            // into the two halves selected by FogEnterMatrix.x.
            fogUv.x -= 0.5;
            fogUv.x = clamp(abs(fogUv.x), 0.0, 0.4921875);
            if (fogEnterUv.x > 0.5)
                fogUv.x += 0.5;
            fog = sampleFourthTexture(fogUv);
            vec3 detail0 = mix(vec3(0.5), layer.rgb, vertexColor.r);
            vec3 detail1 = mix(vec3(0.5), tertiary.rgb, secondaryColor.g);
            terrainColor *= detail0 * detail1 * 4.0;
        }
        vec3 volumeColor = textureStageTransforms.terrainProjectionS[7].rgb;
        float alpha = base.a * textureStageTransforms.terrainProjectionS[7].w;
        outColor = applySceneFog(vec4(mix(terrainColor, volumeColor, clamp(fog.a, 0.0, 1.0)), alpha));
        return;
    }
    if (!stockFeatureDisabled(STOCK_NO_TERRAIN) && textureStageTransforms.materialParams.w > 2.5 && stockTerrainOnlyCount() == 0) {
        vec3 detail0 = mix(vec3(0.5), base.rgb, secondaryColor.r);
        vec3 detail1 = mix(vec3(0.5), layer.rgb, secondaryColor.g);
        vec3 detail2 = mix(vec3(0.5), tertiary.rgb, secondaryColor.b);
        vec3 detail3 = mix(vec3(0.5), fourth.rgb, secondaryColor.a);
        // The 4-layer terrain-only variant exits before the shared material
        // tail. Preserve the global distance fog that OpenGL applies to far
        // terrain; without it distant islands keep their raw pale-blue color.
        outColor = applySceneFog(vec4(detail0 * detail1 * detail2 * detail3 * 8.0, 1.0));
        return;
    }
    if (directionalLightmap != 0u && hasSecondTexture != 0u &&
        hasThirdTexture != 0u && hasFourthTexture != 0u) {
        // CryEngine's CGRCAmbientTempl DOT3LM path: the bump normal and
        // light direction are both tangent-space vectors. The direction
        // map alpha scales the baked light; the color map alpha controls
        // the directional blend.
        vec3 bumpNormal = layer.rgb * 2.0 - 1.0;
        vec3 lightDirection = fourth.rgb * 2.0 - 1.0;
        float ndotl = clamp(dot(lightDirection, bumpNormal), 0.0, 1.0);
        float lightmapIntensity = ndotl * tertiary.a + (1.0 - tertiary.a);
        // Match the OpenGL HDREncodeLM variant selected by r_HDRFake.
        vec3 bakedDiffuse = textureStageTransforms.fogEndDepthRange.w > 0.5 ?
            tertiary.rgb * fourth.a * lightmapIntensity *
                textureStageTransforms.fogEndDepthRange.w : vec3(0.0);
        color = vec4(base.rgb *
                         (textureStageTransforms.materialAmbient.rgb *
                              primaryColor.rgb + bakedDiffuse),
                     previous.a);
    }
    vec4 fifth = sampleFifthTexture(transformStageUv(4u,
        stage4UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    if (hasFifthTexture != 0u)
        color = combineTextureStage(stage4ColorMode, stage4AlphaMode, stage4ColorArg,
            stage4AlphaArg, stage4Constant, fifth, primaryColor, color,
            sourceAlpha((stage4ColorArg >> 6) & 7u, fifth, primaryColor, color,
                        unpackUnorm4x8(stage4Constant)));
    vec4 sixth = sampleSixthTexture(transformStageUv(5u,
        stage5UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    if (hasSixthTexture != 0u)
        color = combineTextureStage(stage5ColorMode, stage5AlphaMode, stage5ColorArg,
            stage5AlphaArg, stage5Constant, sixth, primaryColor, color,
            sourceAlpha((stage5ColorArg >> 6) & 7u, sixth, primaryColor, color,
                        unpackUnorm4x8(stage5Constant)));
    vec4 seventh = sampleSeventhTexture(transformStageUv(6u,
        stage6UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    if (hasSeventhTexture != 0u)
        color = combineTextureStage(stage6ColorMode, stage6AlphaMode, stage6ColorArg,
            stage6AlphaArg, stage6Constant, seventh, primaryColor, color,
            sourceAlpha((stage6ColorArg >> 6) & 7u, seventh, primaryColor, color,
                        unpackUnorm4x8(stage6Constant)));
    vec4 eighth = sampleEighthTexture(transformStageUv(7u,
        stage7UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    if (hasEighthTexture != 0u)
        color = combineTextureStage(stage7ColorMode, stage7AlphaMode, stage7ColorArg,
            stage7AlphaArg, stage7Constant, eighth, primaryColor, color,
            sourceAlpha((stage7ColorArg >> 6) & 7u, eighth, primaryColor, color,
                        unpackUnorm4x8(stage7Constant)));
    if (stockTerrainOnlyCount() == 4) {
        // CGRCTerrain_4Layers_Only has no base map: all four bound maps are
        // details and the fourth weight comes from the original primary alpha.
        // The detail-only OpenGL program leaves the first weighted sample
        // unscaled, then multiplies the remaining three layers by 2.
        color.rgb = mix(vec3(0.5), base.rgb, primaryColor.b) *
                    stockTerrainDetail(layer.rgb, secondaryColor.b) *
                    stockTerrainDetail(tertiary.rgb, secondaryColor.g) *
                    stockTerrainDetail(fourth.rgb, primaryColor.a);
        color.a = 1.0;
    } else if (stockTerrainLayerCount() > 0) {
        color.rgb = base.rgb * primaryColor.rgb * 2.0 * stockTerrainDetail(layer.rgb, secondaryColor.r);
        if (stockTerrainLayerCount() > 1) color.rgb *= stockTerrainDetail(tertiary.rgb, secondaryColor.g);
        if (stockTerrainLayerCount() > 2) color.rgb *= stockTerrainDetail(fourth.rgb, secondaryColor.b);
        color.rgb *= textureStageTransforms.materialAmbient.rgb;
        color.a = base.a;
    }
    color = applyMaterialOverrides(color);
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    color.rgb += stockSeparateSpecular;
    if (textureStageTransforms.materialAmbient.w > 1.5)
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    outColor = applySceneFog(color);
}
