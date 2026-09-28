#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord;
layout(location = 5) in vec2 inLightmapTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec4 secondaryColor;
layout(location = 9) out vec3 clipPosition;
layout(location = 10) out vec2 lightmapTexCoord;
layout(location = 11) out vec3 projectorDirection;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 uv1Row0; vec4 uv1Row1;
} transformData;
void main() {
    clipPosition = inPosition;
    // Keep the varying interface compatible with the textured fragment
    // shader used by the renderer. This shader has no projector basis input,
    // matching vr_scene_texture_color.vert's default behavior.
    projectorDirection = vec3(0.0);
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vec3 uv = vec3(inTexCoord, 1.0);
    vec3 lightmapUv = vec3(inLightmapTexCoord, 1.0);
    texCoord = vec2(dot(transformData.uv0Row0.xyz, uv), dot(transformData.uv0Row1.xyz, uv));
    lightmapTexCoord = vec2(dot(transformData.uv1Row0.xyz, lightmapUv),
                            dot(transformData.uv1Row1.xyz, lightmapUv));
    vertexColor = vec4(1.0);
    secondaryColor = vec4(0.0);
}
