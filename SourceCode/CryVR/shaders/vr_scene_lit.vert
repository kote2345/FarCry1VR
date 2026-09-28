#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec4 vertexColor;
layout(location = 9) out vec3 clipPosition;
layout(location = 11) out vec3 projectorDirection;
layout(push_constant) uniform SceneTransform { mat4 mvp; mat4 modelView; } transformData;
layout(set = 0, binding = 1, std140) uniform TextureStageTransforms {
    vec4 uvRow0[8];
    vec4 uvRow1[8];
    vec4 uvRowQ[8];
} textureStageTransforms;
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
}
void main() {
    clipPosition = inPosition;
    projectorDirection = inPosition - vec3(transformData.modelView[0][3],
                                            transformData.modelView[1][3],
                                            transformData.modelView[2][3]);
    vec3 n = safeNormalize(transpose(inverse(mat3(transformData.modelView))) * inNormal);
    vec3 objectLightPosition = vec3(transformData.modelView[0][3],
                                    transformData.modelView[1][3],
                                    transformData.modelView[2][3]);
    float lightRadius = transformData.modelView[3][3];
    bool specularPass = lightRadius < 0.0;
    float projectorMarker = textureStageTransforms.uvRowQ[7].w;
    bool projectedPass = projectorMarker < 0.0 && !specularPass;
    vec3 objectToLight = objectLightPosition;
    float attenuation = 1.0;
    if (!specularPass && lightRadius > 0.0) {
        objectToLight -= inPosition;
        float distanceToLight = length(objectToLight);
        float normalizedDistance = distanceToLight / lightRadius;
        if (normalizedDistance >= 1.0) {
            attenuation = 0.0;
        } else {
            // Match GLRendPipeline.cpp's stock sAttenuation curve:
            // 2 * (2*x^3 - 3*x^2 + 1), with its continuous value at x=0.
            attenuation = 2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                                 3.0 * normalizedDistance * normalizedDistance + 1.0);
        }
        if (projectedPass) {
            vec3 projectorDirection = safeNormalize(vec3(textureStageTransforms.uvRow0[7].w,
                textureStageTransforms.uvRow1[7].w, textureStageTransforms.uvRowQ[6].w));
            attenuation *= step(-projectorMarker,
                                dot(-safeNormalize(objectToLight), projectorDirection));
        }
    }
    vec3 lightDirectionEye = safeNormalize(mat3(transformData.modelView) * objectToLight);
    float diffuse = specularPass ?
        pow(max(dot(n, lightDirectionEye), 0.0), max(-lightRadius, 1.0)) :
        max(dot(n, lightDirectionEye), 0.0) * attenuation;
    float ambient = (specularPass || projectedPass) ? 0.0 : projectorMarker;
    vec3 ambientColor = projectedPass ? vec3(0.0) : vec3(textureStageTransforms.uvRow0[7].w,
                             textureStageTransforms.uvRow1[7].w,
                             textureStageTransforms.uvRowQ[6].w);
    vec3 diffuseColor = vec3(transformData.modelView[3][0], transformData.modelView[3][1],
                             transformData.modelView[3][2]);
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vertexColor = vec4(ambientColor * ambient + diffuseColor * diffuse, 1.0);
}
