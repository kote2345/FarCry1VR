#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;
layout(location = 0) out vec2 texCoord0;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec2 texCoord1;
layout(location = 3) out vec4 secondaryColor;
layout(location = 9) out vec3 clipPosition;
layout(location = 11) out vec3 projectorDirection;
layout(push_constant) uniform SceneTransform {
    mat4 mvp;
    vec4 uv0Row0; vec4 uv0Row1;
    vec4 uv1Row0; vec4 uv1Row1;
} transformData;
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
} textureStageTransforms;
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
}
void main() {
    clipPosition = inPosition;
    vec3 toLight = textureStageTransforms.objectLightPositionRadius.xyz;
    float radius = textureStageTransforms.objectLightPositionRadius.w;
    bool specularPass = radius < 0.0;
    float ambientFactor = textureStageTransforms.lightColorAmbient.w;
    bool projectedPass = ambientFactor < 0.0 && !specularPass;
    float attenuation = 1.0;
    if (!specularPass && radius > 0.0) {
        toLight -= inPosition;
        float normalizedDistance = length(toLight) / radius;
        attenuation = normalizedDistance >= 1.0 ? 0.0 :
            2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                   3.0 * normalizedDistance * normalizedDistance + 1.0);
        if (projectedPass) {
            vec3 projectorDirection = safeNormalize(textureStageTransforms.materialAmbient.xyz);
            attenuation *= step(-ambientFactor,
                                dot(-safeNormalize(toLight), projectorDirection));
        }
    }
    float diffuse = specularPass ?
        pow(max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0), max(-radius, 1.0)) :
        max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0) * attenuation;
    vec3 ambient = projectedPass || specularPass ? vec3(0.0) :
        textureStageTransforms.materialAmbient.rgb * ambientFactor;
    vec3 lighting = specularPass ? textureStageTransforms.lightColorAmbient.rgb * diffuse :
        ambient + textureStageTransforms.lightColorAmbient.rgb * diffuse;
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vec3 uv0 = vec3(inTexCoord0, 1.0);
    vec3 uv1 = vec3(inTexCoord1, 1.0);
    texCoord0 = vec2(dot(transformData.uv0Row0.xyz, uv0), dot(transformData.uv0Row1.xyz, uv0));
    texCoord1 = vec2(dot(transformData.uv1Row0.xyz, uv1), dot(transformData.uv1Row1.xyz, uv1));
    vertexColor = vec4(inColor.rgb * lighting, inColor.a);
    secondaryColor = inSecondaryColor;
    projectorDirection = inPosition - textureStageTransforms.objectLightPositionRadius.xyz;
}
