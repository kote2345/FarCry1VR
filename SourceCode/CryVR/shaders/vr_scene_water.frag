#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
#include "scene_alpha_test.glsl"
layout(set = 0, binding = 0) uniform sampler2D waterNormalTexture;
layout(set = 1, binding = 0) uniform sampler2D sceneColorTextures[2];
vec4 sampleSceneColor(vec2 uv) {
    // Constant descriptor indices do not require sampled-image array dynamic indexing.
    if (gl_ViewIndex == 0) return texture(sceneColorTextures[0], uv);
    return texture(sceneColorTextures[1], uv);
}
#ifdef WATER_SEA
layout(set = 2, binding = 0) uniform sampler2D fresnelTexture;
layout(set = 3, binding = 0) uniform sampler2D baseWaterTexture;
layout(location=4) in vec2 baseUv;
#endif
#include "scene_water.glsl"
layout(location=0) in vec2 texCoord;
layout(location=1) in vec4 vertexColor;
layout(location=2) in vec2 reflectionUv;
layout(location=3) in vec3 fresnelColor;
layout(location=9) in vec3 clipPosition;
layout(location=0) out vec4 outColor;
float fogDistance(float eyeZ) {
    vec2 size = max(vec2(scene.uvRowQ[0].w, scene.uvRowQ[1].w), vec2(1.0));
    vec2 origin = vec2(scene.linearControls[2].w, scene.linearControls[3].w);
    vec2 ndc = 2.0 * (gl_FragCoord.xy - origin) / size - 1.0;
    float tx = mix(scene.uvRow0[0].w, scene.uvRow1[0].w, (ndc.x + 1.0) * 0.5);
    float ty = mix(scene.uvRow0[1].w, scene.uvRow1[1].w, (ndc.y + 1.0) * 0.5);
    return eyeZ * sqrt(1.0 + tx * tx + ty * ty);
}

void main() {
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), scene.clipPlane) < 0.0) discard;
    int mode=int(scene.terrainProjectionS[7].w+0.5);
    vec4 matrix=scene.terrainProjectionS[4];
    vec4 ambient=scene.terrainProjectionS[5];
    vec4 water=scene.terrainProjectionS[6];
    // DSDT is stored as signed bytes, mirrored to UNORM with a +128 bias.
    // GLTextures::BuildMips uploads signed DSDT bytes divided by 127.0f.
    vec2 bump=clamp((texture(waterNormalTexture,texCoord).xy*255.0-128.0)/127.0,
        vec2(-1.0),vec2(1.0));
    vec2 offset=matrix.xy*bump.x+matrix.zw*bump.y;
    vec4 color;
    if(mode==9 || mode==10) {
      vec4 foam=texture(waterNormalTexture,texCoord);
      color=foam*ambient*vertexColor;
    } else if(mode==2 || mode==4) {
      vec2 size=max(vec2(scene.uvRowQ[0].w,scene.uvRowQ[1].w),vec2(1.0));
      color=vec4(sampleSceneColor(clamp(gl_FragCoord.xy/size+offset,vec2(0),vec2(1))).rgb*water.rgb,1.0);
    } else {
      vec4 reflected=sampleSceneColor(reflectionUv+offset);
      float lum=dot(reflected.rgb,vec3(0.33,0.59,0.11));
#ifdef WATER_SEA
      if(mode==7) {
        vec4 amount=scene.terrainProjectionT[6];
        vec4 base=texture(baseWaterTexture,baseUv);
        vec4 fresnel=texture(fresnelTexture,vec2(fresnelColor.x));
        // CGRCWater: the stock Fresnel lookup and base map determine alpha.
        // Reflection weight is supplied by the renderer; it remains zero
        // until the planar mirrored-camera WaterMap pass is implemented.
        color=vec4(mix(base.rgb,reflected.rgb,amount.rgb),
            vertexColor.a*amount.a*(fresnel.b+(base.b-0.5)));
      } else
#endif
      if(mode==8) {
        color=vec4(reflected.rgb*ambient.rgb*vertexColor.rgb,reflected.a*ambient.a*vertexColor.a);
      } else if(mode==1) {
        vec3 surface=clamp(clamp(lum*0.7+0.3,0.0,1.0)*water.rgb,0.0,1.0);
        color=vec4(ambient.rgb*surface*1.25,clamp(vertexColor.a*water.a+0.5,0.0,1.0));
      } else if(mode==3) {
        float diffuse=2.0*(lum-0.5);
        float specular=clamp(2.0*(diffuse*diffuse-0.6),0.0,1.0);
        color=vec4(ambient.rgb*clamp(diffuse*water.rgb+specular*ambient.a,0.0,1.0),vertexColor.a*water.a);
      } else if(mode==6) {
        vec4 water1=scene.terrainProjectionT[5];
        vec4 delta=water1-water;
        color=vec4(water.rgb+delta.rgb*vertexColor.rgb+reflected.rgb*fresnelColor,
          (water1.a+delta.a*vertexColor.b)*vertexColor.a);
      } else {
        color=vec4(ambient.rgb*clamp(reflected.rgb*clamp(lum*1.75,0.0,1.0)*water.a+water.rgb,0.0,1.0),1.0);
      }
    }
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a, scene.materialParams.y, alphaTestMode)) discard;

    if (scene.fogModeDensityStart.x > 0.5) {
        float nearPlane = scene.fogEndDepthRange.y;
        float farPlane = scene.fogEndDepthRange.z;
        float depth = (gl_FragCoord.z - scene.linearControls[0].w) /
            max(scene.linearControls[1].w - scene.linearControls[0].w, 1.0e-7);
        float denominator = farPlane - depth * (farPlane - nearPlane);
        float eyeDistance = (nearPlane * farPlane) / max(denominator, 1.0e-7);
        eyeDistance = fogDistance(eyeDistance);
        int mode = int(scene.fogModeDensityStart.y + 0.5);
        float amount;
        if (mode == 1)
            amount = (eyeDistance - scene.fogModeDensityStart.w) /
                (scene.fogEndDepthRange.x - scene.fogModeDensityStart.w);
        else if (mode == 2) {
            float d = scene.fogModeDensityStart.z * eyeDistance;
            amount = 1.0 - exp(-min(d * d, 80.0));
        } else {
            float d = scene.fogModeDensityStart.z * eyeDistance;
            amount = 1.0 - exp(-min(d, 80.0));
        }
        color.rgb = mix(color.rgb, scene.fogColor.rgb, clamp(amount, 0.0, 1.0));
    }
    outColor = color;
}
