#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
invariant gl_Position;
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 9) out vec3 clipPosition;
#include "scene_plants_uniforms.glsl"
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 uv1Row0; vec4 uv1Row1;
} transformData;
#include "scene_stereo.glsl"
void main() {
    uint instanceDraw = uint(transformData.mvp[0][0]) + uint(gl_InstanceIndex);
    vec4 ambient = stockMultiview ? stockStereoTransforms.draws[instanceDraw].plantsAmbient :
                                   textureStageTransforms.terrainProjectionS[6];
    vec4 bend = stockMultiview ? stockStereoTransforms.draws[instanceDraw].plantsBend :
                                textureStageTransforms.terrainProjectionT[6];
    vec3 position = inPosition;
    if (any(notEqual(bend.xy, vec2(0.0)))) {
        float factor = max(position.z, 0.0) * bend.z + bend.w;
        factor *= factor;
        factor = factor * factor - bend.w;
        float originalLength = length(position);
        position.xy += bend.xy * factor;
        float bentLength = length(position);
        if (bentLength > 0.0) position *= originalLength / bentLength;
    }
    clipPosition = position;
    mat4 mvp = stockMultiview ? stockStereoTransforms.draws[instanceDraw].mvp[gl_ViewIndex] :
                               transformData.mvp;
    gl_Position = mvp * vec4(position, 1.0);
    texCoord = inTexCoord;
    vertexColor = vec4(inColor.rgb * ambient.rgb, ambient.a);
}
