#version 450
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
layout(location = 0) out vec4 outColor;
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
    vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2];
} textureStageTransforms;
#include "scene_lighting.glsl"
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
vec4 applySceneFog(vec4 sourceColor) {
    if (textureStageTransforms.fogModeDensityStart.x < 0.5) return sourceColor;
    float nearPlane = textureStageTransforms.fogEndDepthRange.y;
    float farPlane = textureStageTransforms.fogEndDepthRange.z;
    float denominator = farPlane - sceneFogDepth() * (farPlane - nearPlane);
    float eyeDistance = (nearPlane * farPlane) / max(denominator, 1.0e-7);
    eyeDistance = radialEyeDistance(eyeDistance);
    float amount;
    int mode = int(textureStageTransforms.fogModeDensityStart.y + 0.5);
    if (mode == 1) {
        amount = (eyeDistance - textureStageTransforms.fogModeDensityStart.w) /
                 (textureStageTransforms.fogEndDepthRange.x - textureStageTransforms.fogModeDensityStart.w);
    } else if (mode == 2) {
        float d = textureStageTransforms.fogModeDensityStart.z * eyeDistance;
        amount = 1.0 - exp(-min(d * d, 80.0));
    } else {
        float d = textureStageTransforms.fogModeDensityStart.z * eyeDistance;
        amount = 1.0 - exp(-min(d, 80.0));
    }
    sourceColor.rgb = mix(sourceColor.rgb, textureStageTransforms.fogColor.rgb, clamp(amount, 0.0, 1.0));
    return sourceColor;
}
void main() {
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vec4(0.82, 0.86, 0.9, 1.0);
    vec4 color = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
    color.rgb *= evaluateStockLighting(objectPosition, objectNormal, vec2(0.0), hasMaterialLighting);
    color = applyMaterialOverrides(color);
    float alpha = color.a;
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(alpha, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    color.a = alpha;
    color.rgb += stockSeparateSpecular;
    if (textureStageTransforms.materialAmbient.w > 1.5)
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    outColor = applySceneFog(color);
}
