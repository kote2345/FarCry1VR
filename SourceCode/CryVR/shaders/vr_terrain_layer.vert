#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "scene_water.glsl"
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec4 inColor;
layout(push_constant) uniform Transform { mat4 mvp; } transformData;
#include "scene_stereo.glsl"
layout(location=0) out vec2 baseUv;
layout(location=1) out float layerWeight;
layout(location=2) out vec3 tangentLight;
layout(location=3) out vec3 tangentView;
layout(location=9) out vec3 clipPosition;
invariant gl_Position;
void main() {
    // CGVProgramms.csl::PosTerrainLayerOverlay.
    vec4 position=vec4(inPosition+inNormal*0.05,1.0);
    gl_Position=stockStereoMvp()*position;
    // The stock program adds 0.005 to GL clip W before GL's [-W,W]
    // depth is converted to Vulkan [0,W]. MVP already contains that
    // conversion, so also add half the W change to clip Z.
    gl_Position.z+=0.0025;
    gl_Position.w+=0.005;
    clipPosition=position.xyz;
    baseUv=vec2(dot(scene.terrainProjectionS[0],position),
                dot(scene.terrainProjectionS[1],position));
    float distanceRatio=min(length(scene.terrainProjectionS[2].xyz-position.xyz)/
                            max(scene.terrainProjectionS[3].x,1.e-6),1.0);
    distanceRatio*=distanceRatio;
    distanceRatio*=distanceRatio;
    layerWeight=inColor.a*(1.0-distanceRatio);
    // CGVProgTerrainLayerTempl uses Tangent=(0,1,0).
    vec3 normal=length(inNormal)>1.e-6 ? normalize(inNormal) : vec3(0.0,0.0,1.0);
    vec3 binormal=cross(vec3(0.0,1.0,0.0),normal);
    binormal=length(binormal)>1.e-6 ? normalize(binormal) : vec3(1.0,0.0,0.0);
    vec3 tangent=cross(normal,binormal);
    vec3 light=scene.terrainProjectionS[4].xyz;
    tangentLight=normalize(vec3(dot(tangent,light),dot(binormal,light),dot(normal,light)));
    vec3 view=scene.terrainProjectionS[2].xyz-position.xyz;
    tangentView=vec3(dot(tangent,view),dot(binormal,view),dot(normal,view));
}
