#version 450
#extension GL_GOOGLE_include_directive : require
layout(constant_id = 0) const int alphaTestMode = 0;
layout(constant_id = 64) const uint stockDecalSimpleMode = 0u;
layout(constant_id = 60) const uint stage0UsesTexCoord1 = 0u;
layout(constant_id = 1) const int stage0ColorMode = 1;
layout(constant_id = 2) const int stage0AlphaMode = 1;
layout(constant_id = 5) const uint stage0ColorArg = 0x0a1u;
layout(constant_id = 6) const uint stage0AlphaArg = 0x0a1u;
#define stage0Constant textureStageTransforms.textureConstants[0][0]
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
    vec4 objectLightPositionRadius;
    vec4 lightColorAmbient;
    vec4 materialAmbient;
    vec4 shadowRow0[8];
    vec4 shadowRow1[8];
    vec4 shadowRow2[8];
    vec4 shadowRow3[8];
    vec4 shadowMapStageMask[2];
    vec4 terrainProjectionS[8];
    vec4 terrainProjectionT[8];
    vec4 linearPlanes[32];
    vec4 linearMatrixRows[32];
    vec4 linearControls[8];
    vec4 fixedLights[32];
    vec4 fixedLightInfo; mat4 fixedMatrices[2]; uvec4 textureConstants[2];
} textureStageTransforms;
#include "scene_lighting.glsl"
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
layout(location = 15) in vec3 stockSeparateSpecular;
#ifndef VR_BUMP_MAP
layout(location = 12) in vec3 objectPosition;
layout(location = 13) in vec3 objectNormal;
layout(location = 14) flat in uint hasMaterialLighting;
#endif
layout(location = 11) in vec3 projectorDirection;
layout(set = 0, binding = 2) uniform sampler2D projectorCookieTexture;
#ifndef VR_BUMP_MAP
layout(location = 10) in vec2 lightmapTexCoord;
#endif
layout(location = 0) out vec4 outColor;
vec4 sampleBaseTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.x;
    vec4 sampled = textureGrad(baseColorTexture, uv, dFdx(uv) * scale, dFdy(uv) * scale);
    return compareStockShadowStage(0u, baseColorTexture, sampled, objectPosition);
}
#ifdef VR_BUMP_MAP
vec4 sampleBumpTexture(vec2 uv) {
    float scale = textureStageTransforms.textureLodBias.y;
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
    return c;
}
float radialEyeDistance(float eyeZ) {
    vec2 viewportSize = max(vec2(textureStageTransforms.uvRowQ[0].w,
                                 textureStageTransforms.uvRowQ[1].w), vec2(1.0));
    vec2 viewportOrigin = vec2(textureStageTransforms.linearControls[2].w,
                               textureStageTransforms.linearControls[3].w);
    vec2 ndc = 2.0 * (gl_FragCoord.xy - viewportOrigin) / viewportSize - 1.0;
    float tangentX = mix(textureStageTransforms.uvRow0[0].w,
                         textureStageTransforms.uvRow1[0].w, (ndc.x + 1.0) * 0.5);
    float tangentY = mix(textureStageTransforms.uvRow0[1].w,
                         textureStageTransforms.uvRow1[1].w, (ndc.y + 1.0) * 0.5);
    return eyeZ * sqrt(1.0 + tangentX * tangentX + tangentY * tangentY);
}
float sceneFogDepth() {
    float depthMin = textureStageTransforms.linearControls[0].w;
    float depthMax = textureStageTransforms.linearControls[1].w;
    return (gl_FragCoord.z - depthMin) / max(depthMax - depthMin, 1.0e-7);
}
vec4 applySceneFog(vec4 c) {
    if (textureStageTransforms.fogModeDensityStart.x < 0.5) return c;
    float n=textureStageTransforms.fogEndDepthRange.y, f=textureStageTransforms.fogEndDepthRange.z;
    float d=n*f/max(f-sceneFogDepth()*(f-n),1.0e-7);
    d=radialEyeDistance(d);
    int m=int(textureStageTransforms.fogModeDensityStart.y+0.5); float a;
    if(m==1) a=(d-textureStageTransforms.fogModeDensityStart.w)/(textureStageTransforms.fogEndDepthRange.x-textureStageTransforms.fogModeDensityStart.w);
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
    // GL EF_SetColorOp scales RGB only; alpha 2X/4X remains MODULATE.
    if (mode == 2 || mode == 3) return a * b;
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
    if (stockFragmentDiscardEnabled && dot(vec4(clipPosition, 1.0), textureStageTransforms.clipPlane) < 0.0) discard;
    vec2 baseTexCoord = texCoord;
#ifndef VR_BUMP_MAP
    if (stage0UsesTexCoord1 != 0u) baseTexCoord = lightmapTexCoord;
#endif
    baseTexCoord = stockTerrainStageTexCoord(0u, baseTexCoord, objectPosition);
    vec4 texel = sampleBaseTexture(baseTexCoord);
    vec4 generatedVertexColor = vertexColor;
    if (textureStageTransforms.materialParams.w > 0.5 && textureStageTransforms.materialParams.w < 1.5) generatedVertexColor.rgb = vec3(1.0) - generatedVertexColor.rgb;
    vec4 primaryColor = mix(generatedVertexColor, textureStageTransforms.primaryColor, textureStageTransforms.primaryColorMask);
#ifndef VR_BUMP_MAP
    if (stockDecalSimpleMode == 0u)
        primaryColor.rgb *= evaluateStockLighting(objectPosition, objectNormal,
            stockTerrainStageTexCoord(1u, lightmapTexCoord, objectPosition), hasMaterialLighting);
#endif
#ifdef VR_BUMP_MAP
    vec3 mapNormal = sampleBumpTexture(baseTexCoord).xyz * 2.0 - 1.0;
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
        attenuation = stockProgramAttenuation(normalizedDistance, projectedPass);
        if (projectedPass) {
            vec3 projectorDirection = safeNormalize(vec3(textureStageTransforms.uvRow0[7].w,
                textureStageTransforms.uvRow1[7].w, textureStageTransforms.uvRowQ[6].w));
            attenuation *= step(-projectorMarker,
                                dot(-safeNormalize(toLight), projectorDirection));
        }
    }
    float halfAngle = max(dot(mappedNormal, safeNormalize(toLight)), 0.0);
    float specular = clamp((halfAngle - 0.75) * 4.0, 0.0, 1.0);
    specular *= specular;
    if (specularPass)
        specular = stockProgramSpecular(objectPosition, mappedNormal, toLight);
    float diffuse = specularPass ? specular :
        max(dot(mappedNormal, safeNormalize(toLight)), 0.0) * attenuation;
    vec3 ambientColor = projectedPass ? vec3(0.0) : vec3(textureStageTransforms.uvRow0[7].w,
                             textureStageTransforms.uvRow1[7].w,
                             textureStageTransforms.uvRowQ[6].w) * lightColorAmbient.w;
    vec3 encodedDiffuse = lightColorAmbient.rgb * diffuse * 2.0;
    primaryColor.rgb *= specularPass ? encodedDiffuse : ambientColor + encodedDiffuse;
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
    if (stockDecalSimpleMode != 0u) {
        color = applyMaterialOverrides(color);
        if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a,
                textureStageTransforms.materialParams.y, alphaTestMode)) discard;
        if (textureStageTransforms.materialAmbient.w > 1.5)
            color.rgb = clamp(color.rgb, 0.0, 1.0);
        outColor = applySceneFog(color);
        return;
    }
    if (stockTerrainAmbientMode() == 1)
        color = vec4(texel.rgb * textureStageTransforms.materialAmbient.rgb * 2.0, texel.a);
    if (stockTerrainOnlyCount() == 1) {
        color = vec4(mix(vec3(0.5), texel.rgb, vertexColor.b), 1.0);
    }
    // CGRCTerrain supplies WorldColor through its Ambient parameter and
    // encodes the base albedo/vertex-lighting product separately (x2).
    // This terrain shader does not pass through the generic lightmap branch.
    if (stockTerrainProgram() && stockTerrainLayerCount() == 0 &&
        stockTerrainOnlyCount() == 0 && stockTerrainAmbientMode() == 0)
        color.rgb *= textureStageTransforms.materialAmbient.rgb;
    if (textureStageTransforms.materialParams.w > 2.5 && stockTerrainOnlyCount() == 0) {
        color = vec4(mix(vec3(0.5), texel.rgb, secondaryColor.r), 1.0);
        outColor = color;
        return;
    }
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
    if (textureStageTransforms.terrainProjectionT[7].w < -11.5 &&
        textureStageTransforms.terrainProjectionT[7].w > -12.5) {
        // CGRCAmbient_Particle: ambient is added to the vertex light,
        // while its independent opacity multiplies texture and vertex alpha.
        vec4 ambient = textureStageTransforms.terrainProjectionS[6];
        color = vec4(texel.rgb * (vertexColor.rgb + ambient.rgb),
                     texel.a * vertexColor.a * ambient.a);
    }
    color = applyMaterialOverrides(color);
    if (stockFragmentDiscardEnabled && !stockAlphaTestPasses(color.a, textureStageTransforms.materialParams.y, alphaTestMode)) discard;
    color.rgb += stockSeparateSpecular;
    if (textureStageTransforms.materialAmbient.w > 1.5)
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    outColor = stockTerrainProgram() ? color : applySceneFog(color);
}
