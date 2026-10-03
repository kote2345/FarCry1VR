#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
// Multipass depth equality requires identical clip positions across variants.
invariant gl_Position;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 5) in vec2 inLightmapTexCoord;
layout(location = 10) out vec2 lightmapTexCoord;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 6) in vec3 inTangent;
layout(location = 7) in vec3 inBinormal;
layout(location = 8) in vec3 inTangentNormal;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec4 secondaryColor;
layout(location = 3) out vec3 objectPosition;
layout(location = 4) out vec3 tangent;
layout(location = 5) out vec3 binormal;
layout(location = 6) out vec3 tangentNormal;
layout(location = 7) out vec4 objectLightPositionRadius;
layout(location = 8) out vec4 lightColorAmbient;
layout(location = 9) out vec3 clipPosition;
layout(location = 11) out vec3 projectorDirection;
layout(location = 15) out vec3 stockSeparateSpecular;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0;
    vec4 uv0Row1;
    vec4 objectLightPositionRadius;
    vec4 lightColorAmbient;
} transformData;
#include "scene_stereo.glsl"
void main() {
    stockSeparateSpecular = vec3(0.0);
    clipPosition = inPosition;
    projectorDirection = inPosition - transformData.objectLightPositionRadius.xyz;
    texCoord = inTexCoord;
    lightmapTexCoord = inLightmapTexCoord;
    gl_Position = stockStereoMvp() * vec4(inPosition, 1.0);
    vertexColor = inColor;
    secondaryColor = inSecondaryColor;
    objectPosition = inPosition;
    tangent = inTangent;
    binormal = inBinormal;
    tangentNormal = inTangentNormal;
    objectLightPositionRadius = transformData.objectLightPositionRadius;
    lightColorAmbient = transformData.lightColorAmbient;
}
