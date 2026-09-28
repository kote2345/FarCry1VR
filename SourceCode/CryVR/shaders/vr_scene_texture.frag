#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 60) const uint stage0UsesTexCoord1 = 0u;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 9) in vec3 clipPosition;
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
layout(location = 10) in vec2 lightmapTexCoord;
layout(location = 0) out vec4 outColor;
vec4 sampleBaseTexture(vec2 uv) {
    float scale = exp2(clamp(textureStageTransforms.textureLodBias.x, -16.0, 16.0));
    return textureGrad(baseColorTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
}
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
    float n=textureStageTransforms.fogEndDepthRange.y, f=textureStageTransforms.fogEndDepthRange.z;
    float d=n*f/max(f-gl_FragCoord.z*(f-n),1.0e-7);
    d=radialEyeDistance(d);
    int m=int(textureStageTransforms.fogModeDensityStart.y+0.5); float a;
    if(m==1) a=(d-textureStageTransforms.fogModeDensityStart.w)/max(textureStageTransforms.fogEndDepthRange.x-textureStageTransforms.fogModeDensityStart.w,1.0e-6);
    else if(m==2){float x=textureStageTransforms.fogModeDensityStart.z*d;a=1.0-exp(-min(x*x,80.0));}
    else a=1.0-exp(-min(textureStageTransforms.fogModeDensityStart.z*d,80.0));
    c.rgb=mix(c.rgb,textureStageTransforms.fogColor.rgb,clamp(a,0.0,1.0));return c;
}
void main() {
    if (dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 primaryColor = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
    vec2 baseTexCoord = stage0UsesTexCoord1 != 0u ? lightmapTexCoord : texCoord;
    vec4 color = applyMaterialOverrides(sampleBaseTexture(baseTexCoord) * primaryColor);
    if (textureStageTransforms.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(color.a > 0.0)) discard;
        if (alphaTestMode == 2 && !(color.a < 0.5)) discard;
        if (alphaTestMode == 3 && !(color.a >= 0.5)) discard;
        if (alphaTestMode == 4 && !(color.a >= 0.25)) discard;
    }
    outColor = applySceneFog(color);
}
