#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 1) const int stage0ColorMode = 1;
layout(constant_id = 2) const int stage0AlphaMode = 1;
layout(constant_id = 3) const int stage1ColorMode = 1;
layout(constant_id = 4) const int stage1AlphaMode = 1;
layout(constant_id = 5) const uint stage0ColorArg = 0x0a1u;
layout(constant_id = 6) const uint stage0AlphaArg = 0x0a1u;
layout(constant_id = 7) const uint stage0Constant = 0xffffffffu;
layout(constant_id = 8) const uint stage1ColorArg = 0x0a1u;
layout(constant_id = 9) const uint stage1AlphaArg = 0x0a1u;
layout(constant_id = 10) const uint stage1Constant = 0xffffffffu;
layout(constant_id = 11) const uint hasSecondaryColor = 0u;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 0) uniform sampler2D secondaryTexture;
layout(location = 0) in vec2 texCoord0;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec2 texCoord1;
layout(location = 3) in vec4 secondaryColor;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform SceneFog { vec4 fogColor; vec4 fogModeDensityStart; vec4 fogEndDepthRange; vec4 materialParams; } fogData;
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
    if (mode == 6 || mode == 7) return mix(b, a, blendFactor);
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
    vec4 base = texture(baseColorTexture, texCoord0);
    vec4 env0 = unpackUnorm4x8(stage0Constant);
    vec3 s0a = sourceRgb(stage0ColorArg & 7u, base, vertexColor, vertexColor, env0);
    vec3 s0b = sourceRgb((stage0ColorArg >> 3) & 7u, base, vertexColor, vertexColor, env0);
    float s0aa = sourceAlpha(stage0AlphaArg & 7u, base, vertexColor, vertexColor, env0);
    float s0ab = sourceAlpha((stage0AlphaArg >> 3) & 7u, base, vertexColor, vertexColor, env0);
    vec3 s0c = sourceRgb((stage0ColorArg >> 6) & 7u, base, vertexColor, vertexColor, env0);
    float s0ac = sourceAlpha((stage0AlphaArg >> 6) & 7u, base, vertexColor, vertexColor, env0);
    float s0Arg1Alpha = sourceAlpha(stage0ColorArg & 7u, base, vertexColor, vertexColor, env0);
    vec4 previous = vec4(combineRgb(stage0ColorMode, s0a, s0b,
                                    stage0ColorMode == 6 ? vertexColor.a : base.a, s0c, s0Arg1Alpha),
                         combineAlpha(stage0AlphaMode, s0aa, s0ab, s0ac,
                                      stage0AlphaMode == 6 ? vertexColor.a : base.a));
    previous = clamp(previous, 0.0, 1.0);
    vec4 layer = texture(secondaryTexture, texCoord1);
    vec4 env1 = unpackUnorm4x8(stage1Constant);
    vec3 s1a = sourceRgb(stage1ColorArg & 7u, layer, vertexColor, previous, env1);
    vec3 s1b = sourceRgb((stage1ColorArg >> 3) & 7u, layer, vertexColor, previous, env1);
    float s1aa = sourceAlpha(stage1AlphaArg & 7u, layer, vertexColor, previous, env1);
    float s1ab = sourceAlpha((stage1AlphaArg >> 3) & 7u, layer, vertexColor, previous, env1);
    vec3 s1c = sourceRgb((stage1ColorArg >> 6) & 7u, layer, vertexColor, previous, env1);
    float s1ac = sourceAlpha((stage1AlphaArg >> 6) & 7u, layer, vertexColor, previous, env1);
    float s1Arg1Alpha = sourceAlpha(stage1ColorArg & 7u, layer, vertexColor, previous, env1);
    vec4 color = vec4(combineRgb(stage1ColorMode, s1a, s1b,
                                 stage1ColorMode == 6 ? vertexColor.a : layer.a, s1c, s1Arg1Alpha),
                      combineAlpha(stage1AlphaMode, s1aa, s1ab, s1ac,
                                   stage1AlphaMode == 6 ? vertexColor.a : layer.a));
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
