#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_water.glsl"
#include "scene_alpha_test.glsl"
layout(constant_id=0) const int alphaTestMode=0;
layout(set=0,binding=0) uniform sampler2D causticsTexture;
layout(location=0) in vec2 causticsUv;
layout(location=1) in float causticsFade;
layout(location=9) in vec3 clipPosition;
layout(location=0) out vec4 outColor;
void main() {
    if (stockFragmentDiscardEnabled && dot(scene.clipPlane,vec4(clipPosition,1.0))<0.0) discard;
    // CGRCCaust multiplies ALL sampled channels by IN.Color.a and Color.
    // ONE/ONE blending requires the fade in RGB as well as alpha.
    float scale=scene.textureLodBias.x;
    vec4 color=causticsFade*textureGrad(causticsTexture,causticsUv,
        dFdx(causticsUv)*scale,dFdy(causticsUv)*scale)*scene.primaryColor;
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a,scene.materialParams.y,alphaTestMode)) discard;
    // Source program is NoFog; no fixed lighting or material-opacity pass.
    outColor=color;
}
