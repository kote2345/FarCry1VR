#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
// Multipass depth equality requires identical clip positions across variants.
invariant gl_Position;
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;
layout(location = 4) in vec4 inSecondaryColor;
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
void main() {
    stockSeparateSpecular = vec3(0.0);
    clipPosition = inPosition; objectPosition = inPosition; objectNormal = vec3(0.0); hasMaterialLighting = 2u;
    projectorDirection = vec3(0.0);
    gl_Position = stockStereoMvp() * vec4(inPosition, 1.0);
    texCoord0 = inTexCoord0;
    texCoord1 = inTexCoord1;
    vertexColor = inColor;
    secondaryColor = inSecondaryColor;
}
