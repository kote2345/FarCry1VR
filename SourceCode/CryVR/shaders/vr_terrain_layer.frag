#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene_water.glsl"
#include "scene_alpha_test.glsl"
layout(constant_id=0) const int alphaTestMode=0;
layout(set=0,binding=0) uniform sampler2D layerTexture;
layout(set=1,binding=0) uniform sampler2D bumpTexture;
layout(location=0) in vec2 baseUv;
layout(location=1) in float layerWeight;
layout(location=2) in vec3 tangentLight;
layout(location=3) in vec3 tangentView;
layout(location=9) in vec3 clipPosition;
layout(location=0) out vec4 outColor;
void main() {
    if(stockFragmentDiscardEnabled && dot(vec4(clipPosition,1.0),scene.clipPlane)<0.0) discard;
    vec2 scale=vec2(scene.textureLodBias.x);
    uint mask=uint(scene.terrainProjectionS[7].x+0.5);
    vec2 uv=baseUv;
    if ((mask&0x1000u)!=0u) {
        float height=texture(bumpTexture,uv).a*2.0-1.0;
        uv+=normalize(tangentView).xy*height*scene.terrainProjectionS[7].y;
    }
    vec4 base=textureGrad(layerTexture,uv,dFdx(uv)*scale,dFdy(uv)*scale);
    vec3 normal=vec3(0.0,0.0,1.0);
    if ((mask&4u)!=0u) {
        normal=texture(bumpTexture,uv).rgb*2.0-1.0;
        if ((mask&0x20u)!=0u) normal=normalize(normal);
    }
    vec3 rgb=base.rgb;
    if ((mask&1u)!=0u)
        rgb+=(dot(tangentLight,normal)-tangentLight.z)*scene.terrainProjectionS[5].xyz;
    if ((mask&8u)!=0u) rgb+=base.a*base.rgb;
    // CGRCTerrainLayerTempl, AffectMask=0, DST_COLOR/SRC_COLOR pass.
    // 0.5 is neutral under this blend; do not multiply ambient a second time.
    bool alphaBlend=(mask&0x8000u)!=0u;
    float alpha=alphaBlend ? layerWeight :
        ((mask&0x18u)==0x10u ? base.a : 1.0);
    vec4 color=vec4(alphaBlend ? rgb :
        rgb*layerWeight+vec3(0.5)*(1.0-layerWeight),alpha);
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a,scene.materialParams.y,alphaTestMode)) discard;
    outColor=color;
}
