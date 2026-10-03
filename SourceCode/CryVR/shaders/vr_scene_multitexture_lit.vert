#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
// Multipass depth equality requires identical clip positions across variants.
invariant gl_Position;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;
layout(location = 0) out vec2 texCoord0;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec2 texCoord1;
layout(location = 3) out vec4 secondaryColor;
layout(location = 9) out vec3 clipPosition;
layout(location = 11) out vec3 projectorDirection;
layout(location = 12) out vec3 objectPosition;
layout(location = 13) out vec3 objectNormal;
layout(location = 14) flat out uint hasMaterialLighting;
layout(location = 15) out vec3 stockSeparateSpecular;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 uv1Row0; vec4 uv1Row1;
} transformData;
#include "scene_stereo.glsl"
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
    vec4 shadowRow0[8]; vec4 shadowRow1[8]; vec4 shadowRow2[8]; vec4 shadowRow3[8];
    vec4 shadowMapStageMask[2];
    vec4 terrainProjectionS[8]; vec4 terrainProjectionT[8];
    vec4 linearPlanes[32]; vec4 linearMatrixRows[32]; vec4 linearControls[8];
    vec4 fixedLights[32]; vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2]; vec4 fogEye1Ray;
} textureStageTransforms;
#include "scene_vertex_lighting.glsl"
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
}
void main() {
    bool terrainShadow = textureStageTransforms.terrainProjectionT[7].w > 399.5 &&
                         textureStageTransforms.terrainProjectionT[7].w < 400.5;
    vec3 drawPosition = inPosition;
    if (terrainShadow)
        drawPosition += inNormal * 0.05;
    clipPosition = drawPosition;
    objectPosition = drawPosition; objectNormal = inNormal;
    bool vertexLighting = textureStageTransforms.materialAmbient.w > 1.5;
    hasMaterialLighting = vertexLighting ? 0u : 1u;
    vec3 toLight = textureStageTransforms.objectLightPositionRadius.xyz;
    float radius = textureStageTransforms.objectLightPositionRadius.w;
    bool specularPass = radius < 0.0;
    float ambientFactor = textureStageTransforms.lightColorAmbient.w;
    bool projectedPass = ambientFactor < 0.0 && !specularPass;
    float attenuation = 1.0;
    if (!specularPass && radius > 0.0) {
        toLight -= inPosition;
        float normalizedDistance = length(toLight) / radius;
        attenuation = normalizedDistance >= 1.0 ? 0.0 :
            2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                   3.0 * normalizedDistance * normalizedDistance + 1.0);
        if (projectedPass) {
            vec3 projectorDirection = safeNormalize(textureStageTransforms.materialAmbient.xyz);
            attenuation *= step(-ambientFactor,
                                dot(-safeNormalize(toLight), projectorDirection));
        }
    }
    float halfAngle = max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0);
    float specular = clamp((halfAngle - 0.75) * 4.0, 0.0, 1.0);
    specular *= specular;
    float diffuse = specularPass ? specular :
        max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0) * attenuation;
    vec3 ambient = projectedPass || specularPass ? vec3(0.0) :
        textureStageTransforms.materialAmbient.rgb * ambientFactor;
    vec3 encodedDiffuse = textureStageTransforms.lightColorAmbient.rgb * diffuse * 2.0;
    vec3 lighting = specularPass ? encodedDiffuse : ambient + encodedDiffuse;
    gl_Position = stockStereoMvp() * vec4(drawPosition, 1.0);
    if (terrainShadow)
        gl_Position.w += 0.005;
    texCoord0 = inTexCoord0;
    texCoord1 = inTexCoord1;
    vertexColor = inColor;
    vec3 separateSpecular;
    vec3 diffuseAmbient = vertexLighting ?
        evaluateStockVertexLighting(inPosition, inNormal, separateSpecular) : vec3(1.0);
    if (!vertexLighting) separateSpecular = vec3(0.0);
    if (vertexLighting) vertexColor = vec4(diffuseAmbient, 1.0);
    stockSeparateSpecular = separateSpecular;
    secondaryColor = inSecondaryColor;
    projectorDirection = inPosition - textureStageTransforms.objectLightPositionRadius.xyz;
}
