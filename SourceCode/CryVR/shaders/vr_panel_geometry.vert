#version 450

layout(location = 0) in vec3 position;
layout(location = 2) in vec4 vertexColor;
layout(location = 3) in vec2 vertexUv;

layout(push_constant) uniform PanelTransform
{
    mat4 mvp;
    vec4 uvTransform;
    vec4 tint;
} transform;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;

void main()
{
    gl_Position = transform.mvp * vec4(position, 1.0);
    // These are the explicit UVs supplied to GL's vertex array. Callers
    // which need 1-V (DrawImage) have already applied it when building vertices.
    uv = vertexUv * transform.uvTransform.xy + transform.uvTransform.zw;
    color = vertexColor;
}
