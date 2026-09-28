#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(location = 0) out vec4 outColor;
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
vec4 applySceneFog(vec4 sourceColor) {
    if (textureStageTransforms.fogModeDensityStart.x < 0.5) return sourceColor;
    float nearPlane = textureStageTransforms.fogEndDepthRange.y;
    float farPlane = textureStageTransforms.fogEndDepthRange.z;
    float denominator = farPlane - gl_FragCoord.z * (farPlane - nearPlane);
    float eyeDistance = (nearPlane * farPlane) / max(denominator, 1.0e-7);
    eyeDistance = radialEyeDistance(eyeDistance);
    float amount;
    int mode = int(textureStageTransforms.fogModeDensityStart.y + 0.5);
    if (mode == 1) {
        amount = (eyeDistance - textureStageTransforms.fogModeDensityStart.w) /
                 max(textureStageTransforms.fogEndDepthRange.x - textureStageTransforms.fogModeDensityStart.w, 1.0e-6);
    } else if (mode == 2) {
        float d = textureStageTransforms.fogModeDensityStart.z * eyeDistance;
        amount = 1.0 - exp(-min(d * d, 80.0));
    } else {
        float d = textureStageTransforms.fogModeDensityStart.z * eyeDistance;
        amount = 1.0 - exp(-min(d, 80.0));
    }
    sourceColor.rgb = mix(sourceColor.rgb, textureStageTransforms.fogColor.rgb, clamp(amount, 0.0, 1.0));
    return sourceColor;
}
void main() {
    if (dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec4 color = applyMaterialOverrides(mix(vec4(0.82, 0.86, 0.9, 1.0), textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask));
    float alpha = color.a;
    if (textureStageTransforms.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(alpha > 0.0)) discard;
        if (alphaTestMode == 2 && !(alpha < 0.5)) discard;
        if (alphaTestMode == 3 && !(alpha >= 0.5)) discard;
        if (alphaTestMode == 4 && !(alpha >= 0.25)) discard;
    }
    color.a = alpha;
    outColor = applySceneFog(color);
}
