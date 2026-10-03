#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
layout(location = 0) in vec4 vertexColor;
layout(location = 9) in vec3 clipPosition;
layout(location = 12) in vec3 objectPosition;
layout(location = 13) in vec3 objectNormal;
layout(location = 14) flat in uint hasMaterialLighting;
layout(location = 15) in vec3 stockSeparateSpecular;
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
layout(location = 0) out vec4 outColor;
vec4 applyMaterialOverrides(vec4 c) {
    if (textureStageTransforms.materialParams.z > 0.5) c.rgb *= textureStageTransforms.materialParams.x;
    else c.a *= textureStageTransforms.materialParams.x;
    return c;
}
void main() {
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5 && textureStageTransforms.materialParams.w < 1.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 color = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
    if (!stockFeatureDisabled(STOCK_NO_FRAGMENT_LIGHTING))
        color.rgb *= evaluateStockLighting(objectPosition, objectNormal, vec2(0.0), hasMaterialLighting);
    color = applyMaterialOverrides(color);
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    color.rgb += stockSeparateSpecular;
    if (textureStageTransforms.materialAmbient.w > 1.5)
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    outColor = applySceneFog(color);
}
