#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec4 secondaryColor;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 uv1Row0; vec4 uv1Row1;
} transformData;
void main() {
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vec3 uv = vec3(inTexCoord, 1.0);
    texCoord = vec2(dot(transformData.uv0Row0.xyz, uv), dot(transformData.uv0Row1.xyz, uv));
    vertexColor = vec4(1.0);
    secondaryColor = vec4(0.0);
}
