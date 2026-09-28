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
    // Legacy 2D batches use a top-left origin and top-down texture V.
    uv = vec2(vertexUv.x, 1.0 - vertexUv.y) * transform.uvTransform.xy + transform.uvTransform.zw;
    color = vertexColor;
}
