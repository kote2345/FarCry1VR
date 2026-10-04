#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
// Multipass depth equality requires identical clip positions across variants.
invariant gl_Position;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec4 vertexColor;
layout(location = 9) out vec3 clipPosition;
layout(location = 11) out vec3 projectorDirection;
layout(location = 12) out vec3 objectPosition;
layout(location = 13) out vec3 objectNormal;
layout(location = 14) flat out uint hasMaterialLighting;
layout(location = 15) out vec3 stockSeparateSpecular;
layout(push_constant) uniform SceneTransform { mat4 mvp; mat4 modelView; } transformData;
#include "scene_stereo.glsl"
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8];
    vec4 uvRow1[8];
    vec4 uvRowQ[8];
    vec4 fogColor; vec4 fogModeDensityStart; vec4 fogEndDepthRange; vec4 materialParams;
    vec4 primaryColor; vec4 primaryColorMask; vec4 textureLodBias; vec4 clipPlane;
    vec4 objectLightPositionRadius; vec4 lightColorAmbient; vec4 materialAmbient;
    vec4 shadowRow0[8]; vec4 shadowRow1[8]; vec4 shadowRow2[8]; vec4 shadowRow3[8];
    vec4 shadowMapStageMask[2];
    vec4 terrainProjectionS[8]; vec4 terrainProjectionT[8];
    vec4 linearPlanes[32]; vec4 linearMatrixRows[32]; vec4 linearControls[8];
    vec4 fixedLights[32]; vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2]; vec4 fogEye1Ray;
} textureStageTransforms;
#include "scene_vertex_lighting.glsl"
void main() {
    clipPosition = inPosition;
    objectPosition = inPosition;
    objectNormal = inNormal;
    bool vertexLighting = stockVertexLightingMode() > 1.5;
    hasMaterialLighting = vertexLighting ? 0u : 1u;
    projectorDirection = inPosition - vec3(transformData.modelView[0][3],
                                            transformData.modelView[1][3],
                                            transformData.modelView[2][3]);
    gl_Position = stockStereoMvp() * vec4(inPosition, 1.0);
    vec3 separateSpecular;
    vec3 diffuseAmbient = vertexLighting ?
        evaluateStockVertexLighting(inPosition, inNormal, separateSpecular) : vec3(1.0);
    if (!vertexLighting) separateSpecular = vec3(0.0);
    vertexColor = vec4(diffuseAmbient, 1.0);
    stockSeparateSpecular = separateSpecular;
}
