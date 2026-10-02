// Shared object-space lighting for the stock CryEngine material shaders.
// The renderer uploads one light/material tuple per draw (and per light pass).
#ifdef VR_SCENE_SPECULAR_OCCLUSION_ALIAS
#define stockSpecularOcclusionTexture eighthTexture
#else
layout(set = 7, binding = 0) uniform sampler2D stockSpecularOcclusionTexture;
#endif

#include "scene_alpha_test.glsl"

int stockTerrainLayerCount()
{
    float marker = textureStageTransforms.terrainProjectionT[7].w;
    return marker > 100.5 && marker < 104.5 ? int(marker - 100.0 + 0.5) : 0;
}

bool stockTerrainProgram()
{
    float marker = textureStageTransforms.terrainProjectionT[7].w;
    return (marker > 99.5 && marker < 104.5) ||
           (marker > 200.5 && marker < 204.5) ||
           (marker > 300.5 && marker < 304.5) ||
           (marker > 410.5 && marker < 412.5) ||
           (marker > 399.5 && marker < 400.5);
}

bool stockTerrainShadowProgram()
{
    float marker = textureStageTransforms.terrainProjectionT[7].w;
    return marker > 399.5 && marker < 400.5;
}

int stockTerrainFogLayerCount()
{
    float marker = textureStageTransforms.terrainProjectionT[7].w;
    return marker > 410.5 && marker < 412.5 ? int(marker - 410.0 + 0.5) : 0;
}

int stockTerrainOnlyCount()
{
    float marker = textureStageTransforms.terrainProjectionT[7].w;
    return marker > 200.5 && marker < 204.5 ? int(marker - 200.0 + 0.5) : 0;
}

int stockTerrainAmbientMode()
{
    float marker = textureStageTransforms.terrainProjectionT[7].w;
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

vec2 stockTerrainRawTexCoord(uint stage, vec2 fallbackUv, vec3 objectPositionForTexgen)
{
    if (textureStageTransforms.materialParams.w < 1.5)
        return fallbackUv;
    vec4 position = vec4(objectPositionForTexgen, 1.0);
    return vec2(dot(textureStageTransforms.terrainProjectionS[stage], position),
                dot(textureStageTransforms.terrainProjectionT[stage], position));
}

vec2 stockTerrainStageTexCoord(uint stage, vec2 fallbackUv,
                               vec3 objectPositionForTexgen)
{
    if (textureStageTransforms.linearControls[stage].x > 0.5)
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
    vec2 projectedUv = textureStageTransforms.materialParams.w >= 1.5 ?
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

float stockProgramSpecular(vec3 position, vec3 normal, vec3 legacyHalfVector)
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
            attenuation = stockProgramAttenuation(length(toLight) / max(light.w, 1.0e-6), camera.w > 2.5);
    }
    float specular = clamp((dot(normal, halfVector) - 0.75) * 4.0, 0.0, 1.0);
    return specular * specular * attenuation;
}

vec3 evaluateStockLighting(vec3 objectPositionForLighting,
                           vec3 objectNormalForLighting,
                           vec2 lightmapUvForLighting,
                           uint hasLightingForMaterial)
{
    // OpenGL only evaluates this lighting term in an active ambient/light
    // pass. Base albedo and baked-lightmap passes stay unlit here.
    if (textureStageTransforms.materialAmbient.w < 0.5 ||
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
    bool fixedFunctionLighting = textureStageTransforms.materialAmbient.w > 1.5;
    vec3 lightVector = lightPositionRadius.xyz;
    float radius = lightPositionRadius.w;
    bool specularPass = radius < 0.0;
    bool projectedPass = lightColorAmbient.w < 0.0 && !specularPass;
    float attenuation = 1.0;

    if (!specularPass && radius > 0.0)
    {
        lightVector -= objectPositionForLighting;
        float distanceToLight = length(lightVector);
        if (fixedFunctionLighting)
        {
            float constantAttenuation = max(textureStageTransforms.uvRow0[6].w, 1.0e-6);
            float linearAttenuation = max(textureStageTransforms.uvRow1[6].w, 0.0);
            attenuation = 1.0 / max(constantAttenuation +
                                    linearAttenuation * distanceToLight, 1.0e-6);
        }
        else
            attenuation = stockProgramAttenuation(distanceToLight / radius, projectedPass);

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
