#version 450

layout(set = 0, binding = 0) uniform sampler2D gameFrame;
layout(push_constant) uniform PanelStyle
{
    mat4 mvp;
    vec4 uvTransform;
    vec4 tint;
} style;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(gameFrame, uv) * style.tint;
}
