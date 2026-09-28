#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 5) in vec2 inLightmapTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec4 secondaryColor;
layout(location = 9) out vec3 clipPosition;
layout(location = 10) out vec2 lightmapTexCoord;
layout(location = 11) out vec3 projectorDirection;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8];
    vec4 uvRow1[8];
    vec4 uvRowQ[8];
} textureStageTransforms;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0;
    vec4 uv0Row1;
    vec4 objectLightPositionRadius;
    vec4 lightColorAmbient;
} transformData;
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
}
void main() {
    clipPosition = inPosition;
    projectorDirection = inPosition - transformData.objectLightPositionRadius.xyz;
    vec3 toLight = transformData.objectLightPositionRadius.xyz;
    bool specularPass = transformData.objectLightPositionRadius.w < 0.0;
    float projectorMarker = textureStageTransforms.uvRowQ[7].w;
    bool projectedPass = projectorMarker < 0.0 && !specularPass;
    float attenuation = 1.0;
    if (!specularPass && transformData.objectLightPositionRadius.w > 0.0) {
        toLight -= inPosition;
        float distanceToLight = length(toLight);
        float normalizedDistance = distanceToLight / transformData.objectLightPositionRadius.w;
        if (normalizedDistance >= 1.0) {
            attenuation = 0.0;
        } else {
            attenuation = 2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                                 3.0 * normalizedDistance * normalizedDistance + 1.0);
        }
        if (projectedPass) {
            vec3 projectorDirection = safeNormalize(vec3(textureStageTransforms.uvRow0[7].w,
                textureStageTransforms.uvRow1[7].w, textureStageTransforms.uvRowQ[6].w));
            attenuation *= step(-projectorMarker,
                                dot(-safeNormalize(toLight), projectorDirection));
        }
    }
    float diffuse = specularPass ?
        pow(max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0),
            max(-transformData.objectLightPositionRadius.w, 1.0)) :
        max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0) * attenuation;
    vec3 ambientColor = projectedPass ? vec3(0.0) : vec3(textureStageTransforms.uvRow0[7].w,
                             textureStageTransforms.uvRow1[7].w,
                             textureStageTransforms.uvRowQ[6].w) * transformData.lightColorAmbient.w;
    vec3 lighting = specularPass ? transformData.lightColorAmbient.rgb * diffuse :
        ambientColor + transformData.lightColorAmbient.rgb * diffuse;
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vec3 uv = vec3(inTexCoord, 1.0);
    texCoord = vec2(dot(transformData.uv0Row0.xyz, uv), dot(transformData.uv0Row1.xyz, uv));
    vec3 lightmapUv = vec3(inLightmapTexCoord, 1.0);
    lightmapTexCoord = vec2(dot(textureStageTransforms.uvRow0[1].xyz, lightmapUv),
                            dot(textureStageTransforms.uvRow1[1].xyz, lightmapUv));
    vertexColor = vec4(inColor.rgb * lighting, inColor.a);
    secondaryColor = inSecondaryColor;
}
