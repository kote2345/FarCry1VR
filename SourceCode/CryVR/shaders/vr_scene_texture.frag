#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 60) const uint stage0UsesTexCoord1 = 0u;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 9) in vec3 clipPosition;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8]; vec4 uvRow1[8]; vec4 uvRowQ[8];
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
#include "scene_lighting.glsl"
#include "scene_fog.glsl"
vec4 applySceneFog(vec4 color) { return applyStockSceneFog(color); }
layout(location = 12) in vec3 objectPosition;
layout(location = 13) in vec3 objectNormal;
layout(location = 14) flat in uint hasMaterialLighting;
layout(location = 15) in vec3 stockSeparateSpecular;
layout(location = 10) in vec2 lightmapTexCoord;
layout(location = 0) out vec4 outColor;
vec4 sampleBaseTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.x;
    return sampleStockTextureStage(0u, baseColorTexture, uv, scale, objectPosition);
}
vec4 applyMaterialOverrides(vec4 c) {
    if (textureStageTransforms.materialParams.z > 0.5) c.rgb *= textureStageTransforms.materialParams.x;
    else c.a *= textureStageTransforms.materialParams.x;
    return c;
}
void main() {
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vertexColor;
    if (stockMaterialColorMode() > 0.5 && stockMaterialColorMode() < 1.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 primaryColor = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
    vec2 baseTexCoord = stage0UsesTexCoord1 != 0u ? lightmapTexCoord : texCoord;
    baseTexCoord = stockTerrainStageTexCoord(0u, baseTexCoord, objectPosition);
    vec4 texel = sampleBaseTexture(baseTexCoord);
    float specularOcclusionChannel = textureStageTransforms.fogColor.w;
    bool derivativeSensitiveLighting = !stockFeatureDisabled(STOCK_NO_FRAGMENT_LIGHTING) &&
        (stockMaterialNormalMode(hasMaterialLighting) == 2u ||
         (specularOcclusionChannel > 0.5 && specularOcclusionChannel < 4.5));
    bool alphaCheckedEarly = stockFragmentDiscardEnabled && !derivativeSensitiveLighting;
    if (alphaCheckedEarly) {
        float alpha = texel.a * primaryColor.a;
        if (textureStageTransforms.materialParams.z <= 0.5)
            alpha *= textureStageTransforms.materialParams.x;
        if (!stockAlphaTestPasses(alpha, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    }
    if (!stockFeatureDisabled(STOCK_NO_FRAGMENT_LIGHTING))
        primaryColor.rgb *= evaluateStockLighting(objectPosition, objectNormal,
        stockTerrainStageTexCoord(1u, lightmapTexCoord, objectPosition), hasMaterialLighting);
    vec4 color = applyMaterialOverrides(texel * primaryColor);
    if (stockFragmentDiscardEnabled && !alphaCheckedEarly &&
        !stockAlphaTestPasses(color.a, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    color.rgb += stockSeparateSpecular;
    if (stockMaterialLightingMode() > 1.5)
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    outColor = applySceneFog(color);
}
