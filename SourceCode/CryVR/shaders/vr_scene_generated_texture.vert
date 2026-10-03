#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
invariant gl_Position;
layout(location = 0) in vec3 inPosition;
#ifdef VR_GENERATED_NORMAL
layout(location = 1) in vec3 inNormal;
#endif
#ifdef VR_GENERATED_COLOR
layout(location = 2) in vec4 inColor;
#endif
#ifdef VR_GENERATED_SECONDARY
layout(location = 4) in vec4 inSecondaryColor;
#endif
layout(location = 0) out vec2 texCoord;
#ifdef VR_GENERATED_EXTERNAL_UV
layout(location = 5) in vec2 inLightmapTexCoord;
#endif
layout(location = 1) out vec4 vertexColor;
#ifdef VR_GENERATED_MULTI
layout(location = 2) out vec2 lightmapTexCoord;
layout(location = 3) out vec4 secondaryColor;
#else
layout(location = 2) out vec4 secondaryColor;
layout(location = 10) out vec2 lightmapTexCoord;
#endif
layout(location = 9) out vec3 clipPosition;
layout(location = 11) out vec3 projectorDirection;
layout(location = 12) out vec3 objectPosition;
layout(location = 13) out vec3 objectNormal;
layout(location = 14) flat out uint hasMaterialLighting;
layout(location = 15) out vec3 stockSeparateSpecular;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8]; vec4 uvRow1[8]; vec4 uvRowQ[8];
    vec4 fogColor; vec4 fogModeDensityStart; vec4 fogEndDepthRange; vec4 materialParams;
    vec4 primaryColor; vec4 primaryColorMask; vec4 textureLodBias; vec4 clipPlane;
    vec4 objectLightPositionRadius; vec4 lightColorAmbient; vec4 materialAmbient;
    vec4 shadowRow0[8]; vec4 shadowRow1[8]; vec4 shadowRow2[8]; vec4 shadowRow3[8];
    vec4 shadowMapStageMask[2];
    vec4 terrainProjectionS[8]; vec4 terrainProjectionT[8];
    vec4 linearPlanes[32]; vec4 linearMatrixRows[32]; vec4 linearControls[8];
    vec4 fixedLights[32]; vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2]; vec4 fogEye1Ray;
} textureStageTransforms;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 objectLightPositionRadius; vec4 lightColorAmbient;
} transformData;
#include "scene_stereo.glsl"
#include "scene_vertex_lighting.glsl"
void main() {
    gl_Position = stockStereoMvp() * vec4(inPosition, 1.0);
    clipPosition = objectPosition = inPosition;
    texCoord = lightmapTexCoord = vec2(0.0);
#ifdef VR_GENERATED_EXTERNAL_UV
    lightmapTexCoord = inLightmapTexCoord;
#endif
    secondaryColor = vec4(0.0);
#ifdef VR_GENERATED_SECONDARY
    secondaryColor = inSecondaryColor;
#endif
    // Single- and multi-stage draws pack the trailing push constants
    // differently. The shared UBO carries the light position in both paths.
    projectorDirection = inPosition - textureStageTransforms.objectLightPositionRadius.xyz;
#ifdef VR_GENERATED_COLOR
    vertexColor = inColor;
#else
    vertexColor = vec4(1.0);
#endif
    stockSeparateSpecular = vec3(0.0);
#ifdef VR_GENERATED_NORMAL
    objectNormal = inNormal;
    bool vertexLighting = textureStageTransforms.materialAmbient.w > 1.5;
    hasMaterialLighting = vertexLighting ? 0u : 1u;
    if (vertexLighting) {
        vertexColor.rgb = evaluateStockVertexLighting(inPosition, inNormal, stockSeparateSpecular);
        vertexColor.a = 1.0;
    }
#else
    objectNormal = vec3(0.0);
    hasMaterialLighting = 2u;
#endif
}
