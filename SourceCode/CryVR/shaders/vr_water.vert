#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "scene_water.glsl"
invariant gl_Position;
layout(location=0) in vec3 inPosition;
#ifdef WATER_COLOR
layout(location=2) in vec4 inColor;
#endif
#ifdef WATER_OCEAN
layout(location=1) in vec3 inNormal;
#endif
layout(push_constant) uniform Transform { mat4 mvp; mat4 reflectionMvp; } transformData;
#define STOCK_STEREO_WATER
#include "scene_stereo.glsl"
#ifdef WATER_UV
layout(location=3) in vec2 inTexCoord;
#endif
layout(location=4) out vec2 baseUv;
layout(location=0) out vec2 rippleUv;
layout(location=1) out vec4 waterVertexColor;
layout(location=2) out vec2 reflectionUv;
layout(location=3) out vec3 fresnelColor;
layout(location=9) out vec3 clipPosition;
void main() {
 vec4 pos=vec4(inPosition,1.0);
 gl_Position=stockStereoMvp()*pos;
 clipPosition=inPosition;
 int mode=int(scene.terrainProjectionS[7].w+0.5);
 vec4 shift=scene.terrainProjectionS[2], detail=scene.terrainProjectionS[3];
 vec3 eye=normalize(scene.terrainProjectionS[7].xyz-inPosition);
 vec2 generated=vec2(dot(pos,scene.terrainProjectionS[0]),dot(pos,scene.terrainProjectionS[1]));
 vec4 inputColor=vec4(1.0);
#ifdef WATER_COLOR
 inputColor=inColor;
#endif
 inputColor=mix(inputColor,scene.primaryColor,scene.primaryColorMask);
 waterVertexColor=inputColor;
 fresnelColor=vec3(0.0);
 baseUv=vec2(0.0);
 vec3 reflected=normalize(2.0*eye.z*vec3(0,0,1)-eye);
 if(mode==7) {
  rippleUv=generated+shift.xy;
  baseUv=rippleUv*detail.xy;
  // Non-reflection fallback samples water_lm instead of an unavailable WaterMap.
  reflectionUv=baseUv;
  fresnelColor=vec3(abs(eye.z));
 } else if(mode==8 || mode==9 || mode==10) {
#ifdef WATER_UV
  vec4 wave=scene.terrainProjectionT[0];
  reflectionUv=vec2(inTexCoord.x*wave.w,(inTexCoord.y+wave.x)*wave.y);
  if(mode==10) reflectionUv=inTexCoord*scene.terrainProjectionT[1].xy+wave.xy;
#else
  reflectionUv=vec2(0.0);
#endif
  rippleUv=(generated+detail.w*shift.zw)*detail.xy;
  if(mode==9 || mode==10) rippleUv=reflectionUv;
 } else if(mode==1) {
  rippleUv=generated+shift.zw*shift.xy;
  reflectionUv=((reflected.xy+1.0)*0.5-shift.zw*shift.xy*0.2)*0.4;
  waterVertexColor.a=max(0.0,inputColor.a*1.8-1.0/max(gl_Position.w*gl_Position.w,1.e-8));
 } else if(mode==3) {
  vec2 shear=vec2(cos(shift.z*detail.w),sin(shift.w*detail.w));
  rippleUv=(generated+shear*0.1)*detail.xy;
  reflectionUv=reflected.xy-shear*0.1;
  waterVertexColor.a=gl_Position.w*0.15;
 } else {
  rippleUv=(generated+detail.w*shift.zw)*detail.xy;
  reflectionUv=vec2(0.0);
  waterVertexColor.a=1.0;
 }
#ifdef WATER_OCEAN
 // World positions and depth-scaled normals come from the FFT sector mesh.
 rippleUv=generated;
 vec4 reflectedClip=stockStereoReflectionMvp()*pos;
 float reflectedW=abs(reflectedClip.w)>1.0e-7?reflectedClip.w:1.0e-7;
 reflectionUv=vec2(reflectedClip.x/reflectedW*0.5+0.5,
                   0.5-reflectedClip.y/reflectedW*0.5);
 float facing=abs(dot(-eye,normalize(inNormal)));
 float fresnel=1.0/pow(facing+1.0,scene.terrainProjectionT[2].y);
 fresnelColor=mix(scene.terrainProjectionT[0].rgb,scene.terrainProjectionT[1].rgb,fresnel);
 waterVertexColor.rgb=vec3(facing);
 waterVertexColor.a=inputColor.a;
#endif
}
