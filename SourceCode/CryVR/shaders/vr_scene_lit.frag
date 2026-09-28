#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(location = 0) in vec4 vertexColor;
layout(location = 9) in vec3 clipPosition;
layout(location = 11) in vec3 projectorDirection;
layout(set = 0, binding = 2) uniform sampler2D projectorCookieTexture;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8]; vec4 uvRow1[8]; vec4 uvRowQ[8];
    vec4 fogColor;
    vec4 fogModeDensityStart;
    vec4 fogEndDepthRange;
    vec4 materialParams;
    vec4 primaryColor;
    vec4 primaryColorMask;
    vec4 textureLodBias;
    vec4 clipPlane;
} textureStageTransforms;
layout(location = 0) out vec4 outColor;
vec4 applyMaterialOverrides(vec4 c) {
    if (textureStageTransforms.materialParams.z > 0.5) c.rgb *= textureStageTransforms.materialParams.x;
    else c.a *= textureStageTransforms.materialParams.x;
    if (textureStageTransforms.materialParams.y > 0.0 && c.a < textureStageTransforms.materialParams.y) discard;
    return c;
}
float radialEyeDistance(float eyeZ) {
    vec2 viewportSize = max(vec2(textureStageTransforms.uvRowQ[0].w,
                                 textureStageTransforms.uvRowQ[1].w), vec2(1.0));
    vec2 ndc = 2.0 * gl_FragCoord.xy / viewportSize - 1.0;
    float tangentX = mix(textureStageTransforms.uvRow0[0].w,
                         textureStageTransforms.uvRow1[0].w, (ndc.x + 1.0) * 0.5);
    float tangentY = mix(textureStageTransforms.uvRow0[1].w,
                         textureStageTransforms.uvRow1[1].w, (ndc.y + 1.0) * 0.5);
    return eyeZ * sqrt(1.0 + tangentX * tangentX + tangentY * tangentY);
}
vec4 applySceneFog(vec4 c) {
    if (textureStageTransforms.fogModeDensityStart.x < 0.5) return c;
    float n = textureStageTransforms.fogEndDepthRange.y, f = textureStageTransforms.fogEndDepthRange.z;
    float d = n*f/max(f-gl_FragCoord.z*(f-n), 1.0e-7);
    d = radialEyeDistance(d);
    int m = int(textureStageTransforms.fogModeDensityStart.y + 0.5);
    float a;
    if (m == 1) a = (d-textureStageTransforms.fogModeDensityStart.w)/max(textureStageTransforms.fogEndDepthRange.x-textureStageTransforms.fogModeDensityStart.w,1.0e-6);
    else if (m == 2) { float x=textureStageTransforms.fogModeDensityStart.z*d; a=1.0-exp(-min(x*x,80.0)); }
    else a=1.0-exp(-min(textureStageTransforms.fogModeDensityStart.z*d,80.0));
    c.rgb=mix(c.rgb,textureStageTransforms.fogColor.rgb,clamp(a,0.0,1.0)); return c;
}
vec3 sampleProjectorCookie(vec3 direction) {
    vec3 forward = vec3(textureStageTransforms.uvRow0[2].w,
                        textureStageTransforms.uvRow1[2].w,
                        textureStageTransforms.uvRowQ[2].w);
    vec3 right = vec3(textureStageTransforms.uvRow0[3].w,
                      textureStageTransforms.uvRow1[3].w,
                      textureStageTransforms.uvRowQ[3].w);
    vec3 up = vec3(textureStageTransforms.uvRow0[4].w,
                   textureStageTransforms.uvRow1[4].w,
                   textureStageTransforms.uvRowQ[4].w);
    float scale = max(textureStageTransforms.uvRow0[5].w, 1.0e-4);
    vec3 d = vec3(dot(direction, right) * scale,
                  dot(direction, up) * scale,
                  dot(direction, forward));
    vec3 a = abs(d);
    int face;
    vec2 faceUv;
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x >= 0.0) { face=0; faceUv=vec2(-d.z,-d.y)/a.x; }
        else { face=1; faceUv=vec2(d.z,-d.y)/a.x; }
    } else if (a.y >= a.x && a.y >= a.z) {
        if (d.y >= 0.0) { face=2; faceUv=vec2(d.x,d.z)/a.y; }
        else { face=3; faceUv=vec2(d.x,-d.z)/a.y; }
    } else {
        if (d.z >= 0.0) { face=4; faceUv=vec2(d.x,-d.y)/a.z; }
        else { face=5; faceUv=vec2(-d.x,-d.y)/a.z; }
    }
    vec2 localUv = clamp(faceUv * 0.5 + 0.5, vec2(0.002), vec2(0.998));
    vec2 tile = vec2(float(face % 3), float(face / 3));
    return texture(projectorCookieTexture, (tile + localUv) / vec2(3.0, 2.0)).rgb;
}
void main() {
    if (dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 color = applyMaterialOverrides(mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask));
    if (textureStageTransforms.uvRow1[5].w > 0.5)
        color.rgb *= sampleProjectorCookie(projectorDirection);
    if (textureStageTransforms.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(color.a > 0.0)) discard;
        if (alphaTestMode == 2 && !(color.a < 0.5)) discard;
        if (alphaTestMode == 3 && !(color.a >= 0.5)) discard;
        if (alphaTestMode == 4 && !(color.a >= 0.25)) discard;
    }
    outColor = applySceneFog(color);
}
