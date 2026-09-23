#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 1) const int stage0ColorMode = 1;
layout(constant_id = 2) const int stage0AlphaMode = 1;
layout(constant_id = 5) const uint stage0ColorArg = 0x0a1u;
layout(constant_id = 6) const uint stage0AlphaArg = 0x0a1u;
layout(constant_id = 7) const uint stage0Constant = 0xffffffffu;
layout(constant_id = 11) const uint hasSecondaryColor = 0u;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
#ifdef VR_BUMP_MAP
layout(set = 1, binding = 0) uniform sampler2D bumpTexture;
layout(location = 3) in vec3 objectPosition;
layout(location = 4) in vec3 tangent;
layout(location = 5) in vec3 binormal;
layout(location = 6) in vec3 tangentNormal;
layout(location = 7) in vec4 objectLightPositionRadius;
layout(location = 8) in vec4 lightColorAmbient;
#endif
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec4 secondaryColor;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform SceneFog { vec4 fogColor; vec4 fogModeDensityStart; vec4 fogEndDepthRange; vec4 materialParams; } fogData;
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
}
vec4 applyMaterialOverrides(vec4 c) {
    if (fogData.materialParams.z > 0.5) c.rgb *= fogData.materialParams.x;
    else c.a *= fogData.materialParams.x;
    if (fogData.materialParams.y > 0.0 && c.a < fogData.materialParams.y) discard;
    return c;
}
vec4 applySceneFog(vec4 c) {
    if (fogData.fogModeDensityStart.x < 0.5) return c;
    float n=fogData.fogEndDepthRange.y, f=fogData.fogEndDepthRange.z;
    float d=n*f/max(f-gl_FragCoord.z*(f-n),1.0e-7);
    int m=int(fogData.fogModeDensityStart.y+0.5); float a;
    if(m==1) a=(d-fogData.fogModeDensityStart.w)/max(fogData.fogEndDepthRange.x-fogData.fogModeDensityStart.w,1.0e-6);
    else if(m==2){float x=fogData.fogModeDensityStart.z*d;a=1.0-exp(-min(x*x,80.0));}
    else a=1.0-exp(-min(fogData.fogModeDensityStart.z*d,80.0));
    c.rgb=mix(c.rgb,fogData.fogColor.rgb,clamp(a,0.0,1.0));return c;
}
vec3 sourceRgb(uint selector, vec4 texel, vec4 primary, vec4 previous, vec4 constantColor) {
    if (selector == 0u) return hasSecondaryColor != 0u ? secondaryColor.rgb : vec3(0.0);
    if (selector == 2u) return primary.rgb;
    if (selector == 3u) return previous.rgb;
    if (selector == 4u) return constantColor.rgb;
    return texel.rgb;
}
float sourceAlpha(uint selector, vec4 texel, vec4 primary, vec4 previous, vec4 constantColor) {
    if (selector == 0u) return hasSecondaryColor != 0u ? secondaryColor.a : 1.0;
    if (selector == 2u) return primary.a;
    if (selector == 3u) return previous.a;
    if (selector == 4u) return constantColor.a;
    return texel.a;
}
vec3 combineRgb(int mode, vec3 a, vec3 b, float blendFactor, vec3 third, float arg1Alpha) {
    if (mode == 0) return a;
    if (mode == 2) return a * b * 2.0;
    if (mode == 3) return a * b * 4.0;
    if (mode == 4) return a + b;
    if (mode == 5) return a + b - vec3(0.5);
    if (mode == 6) return mix(b, a, blendFactor);
    if (mode == 7) return mix(b, a, blendFactor);
    if (mode == 8) return b;
    if (mode == 10) return a - b;
    if (mode == 9) return third + a * b;
    if (mode == 11) return a + b * arg1Alpha;
    if (mode == 12) return a * b + vec3(arg1Alpha);
    if (mode == 13) return a + b * (1.0 - arg1Alpha);
    if (mode == 14) return (vec3(1.0) - a) * b + vec3(arg1Alpha);
    if (mode == 15) return vec3(dot(a * 2.0 - 1.0, b * 2.0 - 1.0));
    if (mode == 16) return mix(b, a, third);
    return a * b;
}
float combineAlpha(int mode, float a, float b, float third, float blendFactor) {
    if (mode == 0) return a;
    if (mode == 2) return a * b * 2.0;
    if (mode == 3) return a * b * 4.0;
    if (mode == 6 || mode == 7) return mix(b, a, blendFactor);
    if (mode == 4) return a + b;
    if (mode == 5) return a + b - 0.5;
    if (mode == 8) return b;
    if (mode == 10) return a - b;
    if (mode == 9) return third + a * b;
    if (mode == 16) return mix(b, a, third);
    return a * b;
}
void main() {
    vec4 texel = texture(baseColorTexture, texCoord);
    vec4 primaryColor = vertexColor;
#ifdef VR_BUMP_MAP
    vec3 mapNormal = texture(bumpTexture, texCoord).xyz * 2.0 - 1.0;
    vec3 mappedNormal = safeNormalize(safeNormalize(tangent) * mapNormal.x +
                                      safeNormalize(binormal) * mapNormal.y +
                                      safeNormalize(tangentNormal) * mapNormal.z);
    vec3 toLight = objectLightPositionRadius.xyz;
    float attenuation = 1.0;
    if (objectLightPositionRadius.w > 0.0) {
        toLight -= objectPosition;
        float normalizedDistance = length(toLight) / objectLightPositionRadius.w;
        attenuation = normalizedDistance >= 1.0 ? 0.0 :
            2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                   3.0 * normalizedDistance * normalizedDistance + 1.0);
    }
    float diffuse = max(dot(mappedNormal, safeNormalize(toLight)), 0.0) * attenuation;
    primaryColor.rgb *= vec3(lightColorAmbient.w) + lightColorAmbient.rgb * diffuse;
#endif
    vec4 envColor = unpackUnorm4x8(stage0Constant);
    vec3 rgb0 = sourceRgb(stage0ColorArg & 7u, texel, primaryColor, primaryColor, envColor);
    vec3 rgb1 = sourceRgb((stage0ColorArg >> 3) & 7u, texel, primaryColor, primaryColor, envColor);
    float alpha0 = sourceAlpha(stage0AlphaArg & 7u, texel, primaryColor, primaryColor, envColor);
    float alpha1 = sourceAlpha((stage0AlphaArg >> 3) & 7u, texel, primaryColor, primaryColor, envColor);
    vec3 rgbThird = sourceRgb((stage0ColorArg >> 6) & 7u, texel, primaryColor, primaryColor, envColor);
    float alphaThird = sourceAlpha((stage0AlphaArg >> 6) & 7u, texel, primaryColor, primaryColor, envColor);
    float rgbArg1Alpha = sourceAlpha(stage0ColorArg & 7u, texel, primaryColor, primaryColor, envColor);
    float rgbFactor = stage0ColorMode == 6 ? primaryColor.a : texel.a;
    vec4 color = vec4(combineRgb(stage0ColorMode, rgb0, rgb1, rgbFactor, rgbThird, rgbArg1Alpha),
                      combineAlpha(stage0AlphaMode, alpha0, alpha1, alphaThird,
                                   stage0AlphaMode == 6 ? primaryColor.a : texel.a));
    color = clamp(color, 0.0, 1.0);
    color = applyMaterialOverrides(color);
    if (fogData.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(color.a > 0.0)) discard;
        if (alphaTestMode == 2 && !(color.a < 0.5)) discard;
        if (alphaTestMode == 3 && !(color.a >= 0.5)) discard;
        if (alphaTestMode == 4 && !(color.a >= 0.25)) discard;
    }
    outColor = applySceneFog(color);
}
