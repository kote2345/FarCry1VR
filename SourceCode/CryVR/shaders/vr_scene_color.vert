#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec4 inColor;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 0) out vec4 vertexColor;
layout(location = 1) out vec4 secondaryColor;
layout(push_constant) uniform SceneTransform { mat4 mvp; } transformData;
void main() { gl_Position = transformData.mvp * vec4(inPosition, 1.0); vertexColor = inColor; secondaryColor = inSecondaryColor; }
