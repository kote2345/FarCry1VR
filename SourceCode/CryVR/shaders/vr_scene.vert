#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 9) out vec3 clipPosition;
layout(push_constant) uniform SceneTransform { mat4 mvp; } transformData;
void main() { clipPosition = inPosition; gl_Position = transformData.mvp * vec4(inPosition, 1.0); }
