#version 450
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
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 0) uniform sampler2D secondaryTexture;
layout(set = 2, binding = 0) uniform sampler2D tertiaryTexture;
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
    vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2];
} textureStageTransforms;
#include "scene_lighting.glsl"
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
    vec4 sampled = textureGrad(baseColorTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
    return compareStockShadowStage(0u, baseColorTexture, sampled, objectPosition);
}
vec4 sampleSecondaryTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.y;
    vec4 sampled = textureGrad(secondaryTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
    return compareStockShadowStage(1u, secondaryTexture, sampled, objectPosition);
}
vec4 sampleTertiaryTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.z;
    vec4 sampled = textureGrad(tertiaryTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
    return compareStockShadowStage(2u, tertiaryTexture, sampled, objectPosition);
}
vec2 transformTertiaryUv(vec2 uv) {
    return stockTerrainStageTexCoord(2u, uv, objectPosition);
}
vec4 applyMaterialOverrides(vec4 c) {
    if (textureStageTransforms.materialParams.z > 0.5) c.rgb *= textureStageTransforms.materialParams.x;
    else c.a *= textureStageTransforms.materialParams.x;
    return c;
}
float radialEyeDistance(float eyeZ) {
    vec2 viewportSize = max(vec2(textureStageTransforms.uvRowQ[0].w,
                                 textureStageTransforms.uvRowQ[1].w), vec2(1.0));
    vec2 viewportOrigin = vec2(textureStageTransforms.linearControls[2].w,
                               textureStageTransforms.linearControls[3].w);
    vec2 ndc = 2.0 * (gl_FragCoord.xy - viewportOrigin) / viewportSize - 1.0;
    float tangentX = mix(textureStageTransforms.uvRow0[0].w,
                         textureStageTransforms.uvRow1[0].w, (ndc.x + 1.0) * 0.5);
    float tangentY = mix(textureStageTransforms.uvRow0[1].w,
                         textureStageTransforms.uvRow1[1].w, (ndc.y + 1.0) * 0.5);
    return eyeZ * sqrt(1.0 + tangentX * tangentX + tangentY * tangentY);
}
float sceneFogDepth() {
    float depthMin = textureStageTransforms.linearControls[0].w;
    float depthMax = textureStageTransforms.linearControls[1].w;
    return (gl_FragCoord.z - depthMin) / max(depthMax - depthMin, 1.0e-7);
}
vec4 applySceneFog(vec4 c) {
    if (textureStageTransforms.fogModeDensityStart.x < 0.5) return c;
    float n=textureStageTransforms.fogEndDepthRange.y, f=textureStageTransforms.fogEndDepthRange.z;
    float d=n*f/max(f-sceneFogDepth()*(f-n),1.0e-7);
    d=radialEyeDistance(d);
    int m=int(textureStageTransforms.fogModeDensityStart.y+0.5); float a;
    if(m==1) a=(d-textureStageTransforms.fogModeDensityStart.w)/(textureStageTransforms.fogEndDepthRange.x-textureStageTransforms.fogModeDensityStart.w);
    else if(m==2){float x=textureStageTransforms.fogModeDensityStart.z*d;a=1.0-exp(-min(x*x,80.0));}
    else a=1.0-exp(-min(textureStageTransforms.fogModeDensityStart.z*d,80.0));
    c.rgb=mix(c.rgb,textureStageTransforms.fogColor.rgb,clamp(a,0.0,1.0));return c;
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
void main() {
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5 && textureStageTransforms.materialParams.w < 1.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 primaryColor = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
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
    vec4 tertiary = sampleTertiaryTexture(transformTertiaryUv(
        stage2UsesTexCoord1 != 0u ? texCoord1 : texCoord0));
    vec4 env2 = unpackUnorm4x8(stage2Constant);
    vec3 s2a = sourceRgb(stage2ColorArg & 7u, tertiary, primaryColor, color, env2);
    vec3 s2b = sourceRgb((stage2ColorArg >> 3) & 7u, tertiary, primaryColor, color, env2);
    float s2aa = sourceAlpha(stage2AlphaArg & 7u, tertiary, primaryColor, color, env2);
    float s2ab = sourceAlpha((stage2AlphaArg >> 3) & 7u, tertiary, primaryColor, color, env2);
    vec3 s2c = sourceRgb((stage2ColorArg >> 6) & 7u, tertiary, primaryColor, color, env2);
    float s2ac = sourceAlpha((stage2AlphaArg >> 6) & 7u, tertiary, primaryColor, color, env2);
    float s2Arg1Alpha = sourceAlpha(stage2ColorArg & 7u, tertiary, primaryColor, color, env2);
    color = vec4(combineRgb(stage2ColorMode, s2a, s2b,
                            sourceAlpha((stage2ColorArg >> 6) & 7u, tertiary,
                                        primaryColor, color, env2),
                            s2c, s2Arg1Alpha),
                 combineAlpha(stage2AlphaMode, s2aa, s2ab, s2ac,
                              stage2AlphaMode == 6 ? primaryColor.a : tertiary.a));
    color = clamp(color, 0.0, 1.0);
    if (stockTerrainAmbientMode() == 4) {
        vec4 objectPosition4 = vec4(objectPosition, 1.0);
        vec2 fogEnterUv = vec2(dot(textureStageTransforms.terrainProjectionS[4], objectPosition4),
                               dot(textureStageTransforms.terrainProjectionT[4], objectPosition4));
        vec2 fogUv = vec2(dot(textureStageTransforms.terrainProjectionS[5], objectPosition4),
                          dot(textureStageTransforms.terrainProjectionT[5], objectPosition4));
        vec4 fogEnter = sampleSecondaryTexture(fogEnterUv);
        vec4 fog = sampleTertiaryTexture(fogUv);
        float fogAmount = clamp(fog.a * fogEnter.a, 0.0, 1.0);
        vec3 terrainColor = base.rgb * textureStageTransforms.materialAmbient.rgb *
                            vertexColor.rgb * 2.0;
        vec3 volumeColor = textureStageTransforms.terrainProjectionS[7].rgb;
        float alpha = base.a * textureStageTransforms.terrainProjectionS[7].w;
        outColor = vec4(mix(terrainColor, volumeColor, fogAmount), alpha);
        return;
    }
    if (stockTerrainOnlyCount() == 3) {
        vec3 detail0 = mix(vec3(0.5), base.rgb, vertexColor.b);
        vec3 detail1 = mix(vec3(0.5), layer.rgb, secondaryColor.b);
        vec3 detail2 = mix(vec3(0.5), tertiary.rgb, secondaryColor.g);
        color = vec4(detail0 * detail1 * detail2 * 4.0, 1.0);
    }
    if (textureStageTransforms.materialParams.w > 2.5 && stockTerrainOnlyCount() == 0) {
        vec3 detail0 = mix(vec3(0.5), base.rgb, secondaryColor.r);
        vec3 detail1 = mix(vec3(0.5), layer.rgb, secondaryColor.g);
        vec3 detail2 = mix(vec3(0.5), tertiary.rgb, secondaryColor.b);
        outColor = vec4(detail0 * detail1 * detail2 * 4.0, 1.0);
        return;
    }
    if (stockTerrainLayerCount() > 0) {
        color.rgb = base.rgb * primaryColor.rgb * 2.0 * stockTerrainDetail(layer.rgb, secondaryColor.r);
        if (stockTerrainLayerCount() > 1) color.rgb *= stockTerrainDetail(tertiary.rgb, secondaryColor.g);
        // CGRCTerrain_NLayers applies its CGPSParam Ambient to the base
        // material before compositing detail layers.
        color.rgb *= textureStageTransforms.materialAmbient.rgb;
        color.a = base.a;
    }
    color = applyMaterialOverrides(color);
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    color.rgb += stockSeparateSpecular;
    if (textureStageTransforms.materialAmbient.w > 1.5)
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    outColor = stockTerrainProgram() ? color : applySceneFog(color);
}
