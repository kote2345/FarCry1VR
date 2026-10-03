// Shared object-space lighting for the stock CryEngine material shaders.
// The renderer uploads one light/material tuple per draw (and per light pass).
#ifdef VR_SCENE_SPECULAR_OCCLUSION_ALIAS
#define stockSpecularOcclusionTexture eighthTexture
#else
layout(set = 7, binding = 0) uniform sampler2D stockSpecularOcclusionTexture;
#endif

#include "scene_alpha_test.glsl"

// Zero preserves the complete legacy path. Bits are set only when the CPU
// knows a feature is absent for this pipeline, allowing dead-code elimination.
layout(constant_id = 66) const uint stockShaderDisabledFeatures = 0u;
layout(constant_id = 71) const float stockTerrainMarkerValue = -1.0e30;
layout(constant_id = 72) const float stockMaterialLightingModeValue = -1.0;
layout(constant_id = 73) const float stockMaterialColorModeValue = -1.0;
layout(constant_id = 74) const uint stockMaterialNormalModeValue = 0xffffffffu;
uint stockMaterialNormalMode(uint varyingMode) {
    return stockMaterialNormalModeValue != 0xffffffffu ? stockMaterialNormalModeValue : varyingMode;
}
float stockMaterialColorMode() {
    return stockMaterialColorModeValue >= 0.0 ? stockMaterialColorModeValue :
        textureStageTransforms.materialParams.w;
}
float stockTerrainMarker() {
    return stockTerrainMarkerValue > -1.0e29 ? stockTerrainMarkerValue :
        textureStageTransforms.terrainProjectionT[7].w;
}
float stockMaterialLightingMode() {
    return stockMaterialLightingModeValue >= 0.0 ? stockMaterialLightingModeValue :
        textureStageTransforms.materialAmbient.w;
}
const uint STOCK_NO_FRAGMENT_LIGHTING = 1u;
const uint STOCK_NO_TERRAIN = 2u;
const uint STOCK_NO_LINEAR_TEXGEN = 4u;
const uint STOCK_NO_SHADOW_COMPARE = 8u;
const uint STOCK_NO_FOG = 16u;
const uint STOCK_NO_PROJECTOR = 32u;
const uint STOCK_DIRECTIONAL_LIGHTMAP_FAST = 64u;
bool stockFeatureDisabled(uint feature)
{
    return (stockShaderDisabledFeatures & feature) != 0u;
}

int stockTerrainLayerCount()
{
    if (stockFeatureDisabled(STOCK_NO_TERRAIN)) return 0;
    float marker = stockTerrainMarker();
    return marker > 100.5 && marker < 104.5 ? int(marker - 100.0 + 0.5) : 0;
}

bool stockTerrainProgram()
{
    if (stockFeatureDisabled(STOCK_NO_TERRAIN)) return false;
    float marker = stockTerrainMarker();
    return (marker > 99.5 && marker < 104.5) ||
           (marker > 200.5 && marker < 204.5) ||
           (marker > 300.5 && marker < 304.5) ||
           (marker > 410.5 && marker < 412.5) ||
           (marker > 399.5 && marker < 400.5);
}

bool stockTerrainShadowProgram()
{
    if (stockFeatureDisabled(STOCK_NO_TERRAIN)) return false;
    float marker = stockTerrainMarker();
    return marker > 399.5 && marker < 400.5;
}

int stockTerrainFogLayerCount()
{
    if (stockFeatureDisabled(STOCK_NO_TERRAIN)) return 0;
    float marker = stockTerrainMarker();
    return marker > 410.5 && marker < 412.5 ? int(marker - 410.0 + 0.5) : 0;
}

int stockTerrainOnlyCount()
{
    if (stockFeatureDisabled(STOCK_NO_TERRAIN)) return 0;
    float marker = stockTerrainMarker();
    return marker > 200.5 && marker < 204.5 ? int(marker - 200.0 + 0.5) : 0;
}

int stockTerrainAmbientMode()
{
    if (stockFeatureDisabled(STOCK_NO_TERRAIN)) return 0;
    float marker = stockTerrainMarker();
    return marker > 300.5 && marker < 304.5 ? int(marker - 300.0 + 0.5) : 0;
}

vec3 stockTerrainDetail(vec3 texel, float weight)
{
    // CGRCTerrain_NLayers: detail * fade + 0.5 * (1 - fade), then x2.
    return mix(vec3(1.0), texel * 2.0, weight);
}

vec4 compareStockShadowStage(uint stage, sampler2D depthTexture,
                             vec4 sampledColor, vec3 objectPositionForShadow)
{
    if (stockFeatureDisabled(STOCK_NO_SHADOW_COMPARE)) return sampledColor;
    float enabled = stage < 4u ?
        textureStageTransforms.shadowMapStageMask[0][stage] :
        textureStageTransforms.shadowMapStageMask[1][stage - 4u];
    if (enabled < 0.5)
        return sampledColor;

    vec4 objectPosition4 = vec4(objectPositionForShadow, 1.0);
    vec4 shadowCoordinate = vec4(
        dot(textureStageTransforms.shadowRow0[stage], objectPosition4),
        dot(textureStageTransforms.shadowRow1[stage], objectPosition4),
        dot(textureStageTransforms.shadowRow2[stage], objectPosition4),
        dot(textureStageTransforms.shadowRow3[stage], objectPosition4));
    if (abs(shadowCoordinate.w) <= 1.0e-7 || shadowCoordinate.w < 0.0)
        return vec4(1.0);

    vec3 projected = shadowCoordinate.xyz / shadowCoordinate.w;
    vec2 projectedUv = projected.xy;
    float storedDepth = textureGrad(depthTexture, projectedUv,
                                    dFdx(projectedUv), dFdy(projectedUv)).r;
    // SGIX GL_TEXTURE_COMPARE_OPERATOR_SGIX with GL_TEXTURE_LEQUAL_R_SGIX.
    float visible = projected.z <= storedDepth ? 1.0 : 0.0;
    return vec4(visible, visible, visible, 1.0);
}

// CGRCShadowTempl blends ambient albedo over the lit surface, with shadow
// coverage in alpha. A visible sample must leave the existing lighting intact.
float stockReceiverShadowCoverage(uint stage, sampler2D map, vec3 position)
{
    float enabled = textureStageTransforms.shadowMapStageMask[stage / 4u][stage % 4u];
    if (enabled < 0.5) return 0.0;
    return (1.0 - compareStockShadowStage(stage, map, vec4(1.0), position).r) *
        textureStageTransforms.terrainProjectionT[6][stage - 1u];
}
vec4 stockReceiverShadowColor(vec4 albedo, float coverage)
{
    return vec4(albedo.rgb * textureStageTransforms.terrainProjectionS[6].rgb * 2.0,
                clamp(coverage, 0.0, 1.0));
}

vec4 sampleStockTextureStage(uint stage, sampler2D stageTexture, vec2 uv,
                            float gradientScale, vec3 objectPositionForShadow)
{
    if (!stockFeatureDisabled(STOCK_NO_SHADOW_COMPARE))
    {
        float enabled = textureStageTransforms.shadowMapStageMask[stage / 4u][stage % 4u];
        if (enabled >= 0.5)
            return compareStockShadowStage(stage, stageTexture, vec4(1.0), objectPositionForShadow);
    }
    // A projected depth comparison replaces the color sample entirely.
    // Do not fetch the same texture first at the unrelated material UV.
    return textureGrad(stageTexture, uv, dFdx(uv) * gradientScale, dFdy(uv) * gradientScale);
}

vec2 stockTerrainRawTexCoord(uint stage, vec2 fallbackUv, vec3 objectPositionForTexgen)
{
    // Receiver shadows and character decals use this payload for program
    // constants; their albedo keeps the mesh UV, not object-linear texgen.
    float programMarker = stockTerrainMarker();
    if (programMarker > -16.5 && programMarker < -14.5) return fallbackUv;
    if (stockFeatureDisabled(STOCK_NO_TERRAIN) || textureStageTransforms.materialParams.w < 1.5)
        return fallbackUv;
    vec4 position = vec4(objectPositionForTexgen, 1.0);
    return vec2(dot(textureStageTransforms.terrainProjectionS[stage], position),
                dot(textureStageTransforms.terrainProjectionT[stage], position));
}

vec2 stockTerrainStageTexCoord(uint stage, vec2 fallbackUv,
                               vec3 objectPositionForTexgen)
{
    if (!stockFeatureDisabled(STOCK_NO_LINEAR_TEXGEN) && textureStageTransforms.linearControls[stage].x > 0.5)
    {
        uint mask = uint(textureStageTransforms.linearControls[stage].y + 0.5);
        uint base = stage * 4u;
        vec4 coordinate = vec4(fallbackUv, 0.0, 1.0);
        vec4 position = vec4(objectPositionForTexgen, 1.0);
        for (uint component = 0u; component < 4u; ++component)
            if ((mask & (1u << component)) != 0u)
                coordinate[component] = dot(textureStageTransforms.linearPlanes[base + component], position);
        vec4 transformed = vec4(
            dot(textureStageTransforms.linearMatrixRows[base], coordinate),
            dot(textureStageTransforms.linearMatrixRows[base + 1u], coordinate),
            dot(textureStageTransforms.linearMatrixRows[base + 2u], coordinate),
            dot(textureStageTransforms.linearMatrixRows[base + 3u], coordinate));
        return transformed.xy / transformed.w;
    }
    vec2 projectedUv = !stockFeatureDisabled(STOCK_NO_TERRAIN) && textureStageTransforms.materialParams.w >= 1.5 ?
        stockTerrainRawTexCoord(stage, fallbackUv, objectPositionForTexgen) : fallbackUv;
    vec3 projectedUv3 = vec3(projectedUv, 1.0);
    float q = dot(textureStageTransforms.uvRowQ[stage].xyz, projectedUv3);
    float divisor = abs(q) > 1.0e-7 ? q : (q < 0.0 ? -1.0e-7 : 1.0e-7);
    return vec2(dot(textureStageTransforms.uvRow0[stage].xyz, projectedUv3),
                dot(textureStageTransforms.uvRow1[stage].xyz, projectedUv3)) / divisor;
}

vec3 stockLightingNormalize(vec3 value)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * inversesqrt(lengthSquared) : vec3(0.0);
}

// CGVPMacro PROC_ATTENPIX and CGRCLightTempl's point-light branch:
// saturate(1 - x*x - y*y - z*z). Projector vertex attenuation is linear.
float stockProgramAttenuation(float normalizedDistance, bool projected)
{
    return clamp(1.0 - (projected ? normalizedDistance :
                       normalizedDistance * normalizedDistance), 0.0, 1.0);
}

float stockProgramVectorAttenuation(vec3 toLight, float radius, bool projected)
{
    float squaredDistance = dot(toLight, toLight) / (radius * radius);
    // Point attenuation needs squared distance, not sqrt followed by square.
    return clamp(1.0 - (projected ? sqrt(squaredDistance) : squaredDistance), 0.0, 1.0);
}

layout(set = 6, binding = 0) uniform sampler2D stockSpecularGlossTexture;
vec4 stockProgramGloss(vec2 uv) {
    uint flags = uint(textureStageTransforms.terrainProjectionS[6].x + 0.5);
    return (flags & 16u) != 0u ? textureGrad(stockSpecularGlossTexture, uv, dFdx(uv), dFdy(uv)) : vec4(1.0);
}
float stockProgramSpecular(vec3 position, vec3 normal, vec3 legacyHalfVector, float glossAlpha)
{
    vec4 camera = textureStageTransforms.terrainProjectionS[7];
    vec4 light = textureStageTransforms.terrainProjectionT[7];
    vec3 halfVector = stockLightingNormalize(legacyHalfVector);
    float attenuation = 1.0;
    if (camera.w > 0.5)
    {
        vec3 toLight = camera.w > 1.5 ? light.xyz - position : light.xyz;
        halfVector = stockLightingNormalize(stockLightingNormalize(toLight) +
            stockLightingNormalize(camera.xyz - position));
        if (camera.w > 1.5)
            attenuation = stockProgramVectorAttenuation(toLight, max(light.w, 1.0e-6), camera.w > 2.5);
        uint flags = uint(textureStageTransforms.terrainProjectionS[6].x + 0.5);
        if (textureStageTransforms.terrainProjectionS[6].w > 0.5 && (flags & 1u) != 0u) {
            vec3 reflectedLight = reflect(-stockLightingNormalize(toLight), normal);
            float power = textureStageTransforms.terrainProjectionS[6].y;
            if ((flags & 2u) != 0u) power *= glossAlpha;
            return pow(max(dot(reflectedLight, stockLightingNormalize(camera.xyz-position)), 0.0),
                       max(power, 0.0)) * attenuation;
        }
    }
    float specular = clamp((dot(normal, halfVector) - 0.75) * 4.0, 0.0, 1.0);
    return specular * specular * attenuation;
}
float stockProgramSpecular(vec3 position, vec3 normal, vec3 legacyHalfVector) {
    return stockProgramSpecular(position, normal, legacyHalfVector, 1.0);
}

vec3 evaluateStockLighting(vec3 objectPositionForLighting,
                           vec3 objectNormalForLighting,
                           vec2 lightmapUvForLighting,
                           uint hasLightingForMaterial)
{
    hasLightingForMaterial = stockMaterialNormalMode(hasLightingForMaterial);
    // OpenGL only evaluates this lighting term in an active ambient/light
    // pass. Base albedo and baked-lightmap passes stay unlit here.
    if (stockFeatureDisabled(STOCK_NO_FRAGMENT_LIGHTING) || stockMaterialLightingMode() < 0.5 ||
        hasLightingForMaterial == 0u)
        return vec3(1.0);

    vec3 normal = objectNormalForLighting;
    if (hasLightingForMaterial == 2u)
    {
        // Several stock CryEngine leaf buffers contain position/UV only.
        // Reconstruct a face normal from the interpolated object position so
        // these common static meshes still receive directional and point light.
        normal = stockLightingNormalize(cross(dFdx(objectPositionForLighting),
                                 dFdy(objectPositionForLighting)));
        if (!gl_FrontFacing)
            normal = -normal;
    }
    else
        normal = stockLightingNormalize(normal);
    vec4 lightPositionRadius = textureStageTransforms.objectLightPositionRadius;
    vec4 lightColorAmbient = textureStageTransforms.lightColorAmbient;
    bool fixedFunctionLighting = stockMaterialLightingMode() > 1.5;
    vec3 lightVector = lightPositionRadius.xyz;
    float radius = lightPositionRadius.w;
    bool specularPass = radius < 0.0;
    bool projectedPass = lightColorAmbient.w < 0.0 && !specularPass;
    float attenuation = 1.0;

    if (!specularPass && radius > 0.0)
    {
        lightVector -= objectPositionForLighting;
        if (fixedFunctionLighting)
        {
            float distanceToLight = length(lightVector);
            float constantAttenuation = max(textureStageTransforms.uvRow0[6].w, 1.0e-6);
            float linearAttenuation = max(textureStageTransforms.uvRow1[6].w, 0.0);
            attenuation = 1.0 / max(constantAttenuation +
                                    linearAttenuation * distanceToLight, 1.0e-6);
        }
        else
            attenuation = stockProgramVectorAttenuation(lightVector, radius, projectedPass);

        if (projectedPass)
        {
            vec3 projectorDirection = stockLightingNormalize(textureStageTransforms.materialAmbient.xyz);
            attenuation *= step(-lightColorAmbient.w,
                                dot(-stockLightingNormalize(lightVector), projectorDirection));
        }
    }

    float lambert = max(dot(normal, stockLightingNormalize(lightVector)), 0.0);
    float specularOcclusion = 1.0;
    float specularOcclusionChannel = textureStageTransforms.fogColor.w;
    if (specularOcclusionChannel > 0.5 && specularOcclusionChannel < 4.5)
    {
        vec4 visibility = textureGrad(stockSpecularOcclusionTexture,
            lightmapUvForLighting, dFdx(lightmapUvForLighting),
            dFdy(lightmapUvForLighting));
        int channel = clamp(int(specularOcclusionChannel + 0.5) - 1, 0, 3);
        specularOcclusion = visibility[channel];
    }
    if (specularPass)
    {
        // CGRCLightTempl's non-per-pixel specular branch computes
        // saturate((NdotH - 0.75) * 4), then squares that term. The renderer
        // supplies the half-angle vector in lightVector for this pass.
        float specular = stockProgramSpecular(objectPositionForLighting, normal, lightVector) * specularOcclusion;
        // CGVProgramms.csl HDREncode is x2 with the stock non-HDR settings.
        return max(lightColorAmbient.rgb * specular * 2.0, vec3(0.0));
    }

    // OpenGL's AmbLightColor component already contains the evaluated RGB
    // ambient term. Its alpha is object opacity, not an ambient multiplier.
    vec3 ambient = projectedPass ? vec3(0.0) :
        max(textureStageTransforms.materialAmbient.rgb, vec3(0.0));
    float diffuseEncoding = fixedFunctionLighting ? 1.0 : 2.0;
    vec3 diffuse = max(lightColorAmbient.rgb, vec3(0.0)) * lambert * attenuation * diffuseEncoding;
    vec3 specular = vec3(0.0);
    if (fixedFunctionLighting)
    {
        // CGLRenderer::EF_LightMaterial installs the material/light specular
        // colors and shininess in GL. GL_LIGHT_MODEL_LOCAL_VIEWER defaults to
        // false, so the carried object-space view direction is constant.
        vec3 viewDirection = stockLightingNormalize(vec3(textureStageTransforms.uvRow0[5].w,
                                            textureStageTransforms.uvRow1[5].w,
                                            textureStageTransforms.uvRowQ[5].w));
        vec3 halfVector = stockLightingNormalize(stockLightingNormalize(lightVector) + viewDirection);
        float shininess = clamp(lightColorAmbient.w, 0.0, 128.0);
        float specularFactor = pow(max(dot(normal, halfVector), 0.0), shininess);
        vec3 specularColor = vec3(textureStageTransforms.uvRow0[4].w,
                                  textureStageTransforms.uvRow1[4].w,
                                  textureStageTransforms.uvRowQ[4].w);
        specular = max(specularColor, vec3(0.0)) * specularFactor * attenuation *
                   specularOcclusion;
    }
    return max(ambient + diffuse + specular, vec3(0.0));
}
