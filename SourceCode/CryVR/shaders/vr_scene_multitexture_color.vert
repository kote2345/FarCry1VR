#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 0) out vec2 texCoord0;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec2 texCoord1;
layout(location = 3) out vec4 secondaryColor;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 uv1Row0; vec4 uv1Row1;
} transformData;
void main() {
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vec3 uv0 = vec3(inTexCoord0, 1.0);
    vec3 uv1 = vec3(inTexCoord1, 1.0);
    texCoord0 = vec2(dot(transformData.uv0Row0.xyz, uv0), dot(transformData.uv0Row1.xyz, uv0));
    texCoord1 = vec2(dot(transformData.uv1Row0.xyz, uv1), dot(transformData.uv1Row1.xyz, uv1));
    vertexColor = inColor;
    secondaryColor = inSecondaryColor;
}
