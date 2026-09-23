#version 450

layout(push_constant) uniform PanelTransform
{
    mat4 mvp;
    vec4 uvTransform;
    vec4 tint;
} transform;

layout(location = 0) out vec2 uv;

const vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

void main()
{
    vec2 p = positions[gl_VertexIndex];
    gl_Position = transform.mvp * vec4(p, 0.0, 1.0);
    // OpenGL readback starts at the bottom row; flip V when sampling it.
    vec2 panelUv = vec2(p.x * 0.5 + 0.5, p.y * 0.5 + 0.5);
    uv = panelUv * transform.uvTransform.xy + transform.uvTransform.zw;
}
