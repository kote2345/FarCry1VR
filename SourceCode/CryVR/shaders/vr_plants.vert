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
layout(location = 10) out float fogAmount;
layout(constant_id = 69) const uint stockFogMode = 0u;
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
        float originalLengthSquared = dot(position, position);
        position.xy += bend.xy * factor;
        float bentLengthSquared = dot(position, position);
        if (bentLengthSquared > 0.0)
            position *= sqrt(originalLengthSquared / bentLengthSquared);
    }
    clipPosition = position;
    mat4 mvp = stockMultiview ? stockStereoTransforms.draws[instanceDraw].mvp[gl_ViewIndex] :
                               transformData.mvp;
    gl_Position = mvp * vec4(position, 1.0);
    fogAmount = 0.0;
    if (stockFogMode != 4u && (stockFogMode != 0u || textureStageTransforms.fogModeDensityStart.x > 0.5)) {
        // Foliage uses interpolated vertex fog, as the stock Cg plant path.
        // Derive each eye's radial distance using the same FOV as scene fog.
        vec2 pixel = (gl_Position.xy / max(gl_Position.w, 1.0e-7) * 0.5 + 0.5) *
            vec2(textureStageTransforms.uvRowQ[0].w, textureStageTransforms.uvRowQ[1].w) +
            vec2(textureStageTransforms.linearControls[2].w, textureStageTransforms.linearControls[3].w);
        vec4 rayCoefficients = vec4(textureStageTransforms.linearControls[4].w,
            textureStageTransforms.linearControls[5].w, textureStageTransforms.linearControls[6].w,
            textureStageTransforms.linearControls[7].w);
        if (stockMultiview && gl_ViewIndex == 1) rayCoefficients = textureStageTransforms.fogEye1Ray;
        vec2 ray = pixel * rayCoefficients.xz + rayCoefficients.yw;
        float distance = max(gl_Position.w, 0.0) * sqrt(1.0 + dot(ray, ray));
        vec4 fog = textureStageTransforms.fogModeDensityStart;
        uint mode = stockFogMode != 0u ? stockFogMode : uint(fog.y + 0.5);
        float d = fog.z * distance;
        fogAmount = clamp(mode == 1u ? (distance - fog.w) * fog.z :
            1.0 - exp(-min(mode == 2u ? d*d : d, 80.0)), 0.0, 1.0);
    }
    texCoord = inTexCoord;
    vertexColor = vec4(inColor.rgb * ambient.rgb, ambient.a);
}
