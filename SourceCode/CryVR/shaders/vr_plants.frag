#version 450
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
#include "scene_plants_uniforms.glsl"
#include "scene_alpha_test.glsl"
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 9) in vec3 clipPosition;
layout(location = 0) out vec4 outColor;
float radialEyeDistance(float eyeZ) {
    vec2 viewportSize = max(vec2(textureStageTransforms.uvRowQ[0].w,
                                 textureStageTransforms.uvRowQ[1].w), vec2(1.0));
    vec2 viewportOrigin = vec2(textureStageTransforms.linearControls[2].w,
                               textureStageTransforms.linearControls[3].w);
    vec2 ndc = 2.0 * (gl_FragCoord.xy - viewportOrigin) / viewportSize - 1.0;
    float tangentX = mix(textureStageTransforms.uvRow0[0].w,
                         textureStageTransforms.uvRow1[0].w, (ndc.x + 1.0) * 0.5);
    float tangentY = mix(textureStageTransforms.uvRow0[1].w,
                         textureStageTransforms.uvRow1[1].w, (ndc.y + 1.0) * 0.5);
    return eyeZ * sqrt(1.0 + tangentX * tangentX + tangentY * tangentY);
}
float sceneFogDepth() {
    float depthMin = textureStageTransforms.linearControls[0].w;
    float depthMax = textureStageTransforms.linearControls[1].w;
    return (gl_FragCoord.z - depthMin) / max(depthMax - depthMin, 1.0e-7);
}
vec4 applySceneFog(vec4 c) {
    if (textureStageTransforms.fogModeDensityStart.x < 0.5) return c;
    float n=textureStageTransforms.fogEndDepthRange.y, f=textureStageTransforms.fogEndDepthRange.z;
    float d=n*f/max(f-sceneFogDepth()*(f-n),1.0e-7);
    d=radialEyeDistance(d);
    int m=int(textureStageTransforms.fogModeDensityStart.y+0.5); float a;
    if(m==1) a=(d-textureStageTransforms.fogModeDensityStart.w)/(textureStageTransforms.fogEndDepthRange.x-textureStageTransforms.fogModeDensityStart.w);
    else if(m==2){float x=textureStageTransforms.fogModeDensityStart.z*d;a=1.0-exp(-min(x*x,80.0));}
    else a=1.0-exp(-min(textureStageTransforms.fogModeDensityStart.z*d,80.0));
    c.rgb=mix(c.rgb,textureStageTransforms.fogColor.rgb,clamp(a,0.0,1.0));return c;
}
void main() {
    float scale = textureStageTransforms.textureLodBias.x;
    vec4 texel = textureGrad(baseColorTexture, texCoord,
                            dFdx(texCoord) * scale, dFdy(texCoord) * scale);
    if (stockFragmentDiscardEnabled &&
        dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    // CGRCPlants opacity is texture alpha * the vertex program's Ambient.w.
    // Reject empty texels before shading/fog; all derivatives precede discard.
    float alpha = clamp(texel.a * vertexColor.a, 0.0, 1.0);
    if (stockFragmentDiscardEnabled &&
        !stockAlphaTestPasses(alpha, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    // CGRCPlants: tex2D(baseMap) * IN.Color, HDREncode RGB (LDR x2).
    // Ambient and vertex color have already been multiplied in the vertex stage.
    outColor = applySceneFog(vec4(clamp(texel.rgb * vertexColor.rgb * 2.0, 0.0, 1.0), alpha));
}
