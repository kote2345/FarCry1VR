#version 450
layout(location = 0) in vec3 inPosition;
layout(push_constant) uniform SceneTransform { mat4 mvp; } transformData;
void main() { gl_Position = transformData.mvp * vec4(inPosition, 1.0); }
