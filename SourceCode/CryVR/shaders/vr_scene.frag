#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform SceneFog {
    vec4 fogColor;
    vec4 fogModeDensityStart;
    vec4 fogEndDepthRange;
    vec4 materialParams;
} fogData;
vec4 applyMaterialOverrides(vec4 c) {
    if (fogData.materialParams.z > 0.5) c.rgb *= fogData.materialParams.x;
    else c.a *= fogData.materialParams.x;
    if (fogData.materialParams.y > 0.0 && c.a < fogData.materialParams.y) discard;
    return c;
}
vec4 applySceneFog(vec4 sourceColor) {
    if (fogData.fogModeDensityStart.x < 0.5) return sourceColor;
    float nearPlane = fogData.fogEndDepthRange.y;
    float farPlane = fogData.fogEndDepthRange.z;
    float denominator = farPlane - gl_FragCoord.z * (farPlane - nearPlane);
    float eyeDistance = (nearPlane * farPlane) / max(denominator, 1.0e-7);
    float amount;
    int mode = int(fogData.fogModeDensityStart.y + 0.5);
    if (mode == 1) {
        amount = (eyeDistance - fogData.fogModeDensityStart.w) /
                 max(fogData.fogEndDepthRange.x - fogData.fogModeDensityStart.w, 1.0e-6);
    } else if (mode == 2) {
        float d = fogData.fogModeDensityStart.z * eyeDistance;
        amount = 1.0 - exp(-min(d * d, 80.0));
    } else {
        float d = fogData.fogModeDensityStart.z * eyeDistance;
        amount = 1.0 - exp(-min(d, 80.0));
    }
    sourceColor.rgb = mix(sourceColor.rgb, fogData.fogColor.rgb, clamp(amount, 0.0, 1.0));
    return sourceColor;
}
void main() {
    vec4 color = applyMaterialOverrides(vec4(0.82, 0.86, 0.9, 1.0));
    float alpha = color.a;
    if (fogData.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(alpha > 0.0)) discard;
        if (alphaTestMode == 2 && !(alpha < 0.5)) discard;
        if (alphaTestMode == 3 && !(alpha >= 0.5)) discard;
        if (alphaTestMode == 4 && !(alpha >= 0.25)) discard;
    }
    color.a = alpha;
    outColor = applySceneFog(color);
}
