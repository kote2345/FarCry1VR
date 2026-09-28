#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 60) const uint stage0UsesTexCoord1 = 0u;
layout(constant_id = 1) const int stage0ColorMode = 1;
layout(constant_id = 2) const int stage0AlphaMode = 1;
layout(constant_id = 5) const uint stage0ColorArg = 0x0a1u;
layout(constant_id = 6) const uint stage0AlphaArg = 0x0a1u;
layout(constant_id = 7) const uint stage0Constant = 0xffffffffu;
layout(constant_id = 11) const uint hasSecondaryColor = 0u;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8];
    vec4 uvRow1[8];
    vec4 uvRowQ[8];
    vec4 fogColor;
    vec4 fogModeDensityStart;
    vec4 fogEndDepthRange;
    vec4 materialParams;
    vec4 primaryColor;
    vec4 primaryColorMask;
    vec4 textureLodBias;
    vec4 clipPlane;
} textureStageTransforms;
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
layout(location = 9) in vec3 clipPosition;
layout(location = 11) in vec3 projectorDirection;
layout(set = 0, binding = 2) uniform sampler2D projectorCookieTexture;
#ifndef VR_BUMP_MAP
layout(location = 10) in vec2 lightmapTexCoord;
#endif
layout(location = 0) out vec4 outColor;
vec4 sampleBaseTexture(vec2 uv) {
    float scale = exp2(clamp(textureStageTransforms.textureLodBias.x, -16.0, 16.0));
    return textureGrad(baseColorTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
}
#ifdef VR_BUMP_MAP
vec4 sampleBumpTexture(vec2 uv) {
    float scale = exp2(clamp(textureStageTransforms.textureLodBias.y, -16.0, 16.0));
    return textureGrad(bumpTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
}
#endif
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
    vec3 a = abs(d); int face; vec2 faceUv;
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
    vec2 localUv=clamp(faceUv*0.5+0.5,vec2(0.002),vec2(0.998));
    vec2 tile=vec2(float(face%3),float(face/3));
    return texture(projectorCookieTexture,(tile+localUv)/vec2(3.0,2.0)).rgb;
}
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
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
vec3 sourceRgb(uint selector, vec4 texel, vec4 primary, vec4 previous, vec4 constantColor) {
    if (selector == 0u) return constantColor.rgb;
    if (selector == 2u) return primary.rgb;
    if (selector == 3u) return previous.rgb;
    if (selector == 4u) return constantColor.rgb;
    return texel.rgb;
}
float sourceAlpha(uint selector, vec4 texel, vec4 primary, vec4 previous, vec4 constantColor) {
    if (selector == 0u) return constantColor.a;
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
    if (mode == 17) return (a + b - vec3(0.5)) * 2.0;
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
    if (dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec2 baseTexCoord = texCoord;
#ifndef VR_BUMP_MAP
    if (stage0UsesTexCoord1 != 0u) baseTexCoord = lightmapTexCoord;
#endif
    vec4 texel = sampleBaseTexture(baseTexCoord);
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 primaryColor = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
#ifdef VR_BUMP_MAP
    vec3 mapNormal = sampleBumpTexture(texCoord).xyz * 2.0 - 1.0;
    vec3 mappedNormal = safeNormalize(safeNormalize(tangent) * mapNormal.x +
                                      safeNormalize(binormal) * mapNormal.y +
                                      safeNormalize(tangentNormal) * mapNormal.z);
    vec3 toLight = objectLightPositionRadius.xyz;
    bool specularPass = objectLightPositionRadius.w < 0.0;
    float projectorMarker = textureStageTransforms.uvRowQ[7].w;
    bool projectedPass = projectorMarker < 0.0 && !specularPass;
    float attenuation = 1.0;
    if (!specularPass && objectLightPositionRadius.w > 0.0) {
        toLight -= objectPosition;
        float normalizedDistance = length(toLight) / objectLightPositionRadius.w;
        attenuation = normalizedDistance >= 1.0 ? 0.0 :
            2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                   3.0 * normalizedDistance * normalizedDistance + 1.0);
        if (projectedPass) {
            vec3 projectorDirection = safeNormalize(vec3(textureStageTransforms.uvRow0[7].w,
                textureStageTransforms.uvRow1[7].w, textureStageTransforms.uvRowQ[6].w));
            attenuation *= step(-projectorMarker,
                                dot(-safeNormalize(toLight), projectorDirection));
        }
    }
    float diffuse = specularPass ?
        pow(max(dot(mappedNormal, safeNormalize(toLight)), 0.0),
            max(-objectLightPositionRadius.w, 1.0)) :
        max(dot(mappedNormal, safeNormalize(toLight)), 0.0) * attenuation;
    vec3 ambientColor = projectedPass ? vec3(0.0) : vec3(textureStageTransforms.uvRow0[7].w,
                             textureStageTransforms.uvRow1[7].w,
                             textureStageTransforms.uvRowQ[6].w) * lightColorAmbient.w;
    primaryColor.rgb *= specularPass ? lightColorAmbient.rgb * diffuse :
        ambientColor + lightColorAmbient.rgb * diffuse;
#endif
    vec4 envColor = unpackUnorm4x8(stage0Constant);
    vec3 rgb0 = sourceRgb(stage0ColorArg & 7u, texel, primaryColor, primaryColor, envColor);
    vec3 rgb1 = sourceRgb((stage0ColorArg >> 3) & 7u, texel, primaryColor, primaryColor, envColor);
    float alpha0 = sourceAlpha(stage0AlphaArg & 7u, texel, primaryColor, primaryColor, envColor);
    float alpha1 = sourceAlpha((stage0AlphaArg >> 3) & 7u, texel, primaryColor, primaryColor, envColor);
    vec3 rgbThird = sourceRgb((stage0ColorArg >> 6) & 7u, texel, primaryColor, primaryColor, envColor);
    float alphaThird = sourceAlpha((stage0AlphaArg >> 6) & 7u, texel, primaryColor, primaryColor, envColor);
    float rgbArg1Alpha = sourceAlpha(stage0ColorArg & 7u, texel, primaryColor, primaryColor, envColor);
    float rgbFactor = sourceAlpha((stage0ColorArg >> 6) & 7u, texel,
                                  primaryColor, primaryColor, envColor);
    vec4 color = vec4(combineRgb(stage0ColorMode, rgb0, rgb1, rgbFactor, rgbThird, rgbArg1Alpha),
                      combineAlpha(stage0AlphaMode, alpha0, alpha1, alphaThird,
                                   stage0AlphaMode == 6 ? primaryColor.a : texel.a));
    color = clamp(color, 0.0, 1.0);
#ifndef VR_BUMP_MAP
    if (textureStageTransforms.uvRow1[5].w > 0.5)
        color.rgb *= sampleProjectorCookie(projectorDirection);
#endif
#ifdef VR_BUMP_MAP
    if (textureStageTransforms.uvRow1[5].w > 0.5) {
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
        vec3 d = vec3(dot(projectorDirection,right)*scale,
                      dot(projectorDirection,up)*scale,
                      dot(projectorDirection,forward));
        vec3 a = abs(d); int face; vec2 faceUv;
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
        vec2 localUv=clamp(faceUv*0.5+0.5,vec2(0.002),vec2(0.998));
        vec2 tile=vec2(float(face%3),float(face/3));
        color.rgb *= texture(projectorCookieTexture,(tile+localUv)/vec2(3.0,2.0)).rgb;
    }
#endif
    color = applyMaterialOverrides(color);
    if (textureStageTransforms.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(color.a > 0.0)) discard;
        if (alphaTestMode == 2 && !(color.a < 0.5)) discard;
        if (alphaTestMode == 3 && !(color.a >= 0.5)) discard;
        if (alphaTestMode == 4 && !(color.a >= 0.25)) discard;
    }
    outColor = applySceneFog(color);
}
