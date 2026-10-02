#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "scene_water.glsl"
layout(location=0) in vec3 inPosition;
layout(push_constant) uniform Transform { mat4 mvp; } transformData;
#include "scene_stereo.glsl"
layout(location=0) out vec2 causticsUv;
layout(location=1) out float causticsFade;
layout(location=9) out vec3 clipPosition;
invariant gl_Position;
void main() {
    vec4 position=vec4(inPosition,1.0);
    gl_Position=stockStereoMvp()*position;
    clipPosition=inPosition;
    // CGVProgCaust: ObjMatrix, CameraPos and WaterLevel. Keep the fade
    // floating point and interpolate it after evaluating it at each vertex.
    vec3 world=vec3(dot(scene.terrainProjectionS[0],position),
                    dot(scene.terrainProjectionS[1],position),
                    dot(scene.terrainProjectionS[2],position));
    vec4 camera=scene.terrainProjectionS[4];
    vec4 waterTime=scene.terrainProjectionS[5];
    causticsUv=world.xy*0.2+waterTime.yz;
    causticsFade=(1.0-length(world-camera.xyz)*camera.w)*
        float(waterTime.x-world.z>=1.0)*0.75;
}
