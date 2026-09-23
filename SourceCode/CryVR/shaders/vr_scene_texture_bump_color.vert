#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
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
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0;
    vec4 uv0Row1;
    vec4 objectLightPositionRadius;
    vec4 lightColorAmbient;
} transformData;
void main() {
    vec3 uv = vec3(inTexCoord, 1.0);
    texCoord = vec2(dot(transformData.uv0Row0.xyz, uv), dot(transformData.uv0Row1.xyz, uv));
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vertexColor = inColor;
    secondaryColor = inSecondaryColor;
    objectPosition = inPosition;
    tangent = inTangent;
    binormal = inBinormal;
    tangentNormal = inTangentNormal;
    objectLightPositionRadius = transformData.objectLightPositionRadius;
    lightColorAmbient = transformData.lightColorAmbient;
}
