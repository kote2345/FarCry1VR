#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec4 vertexColor;
layout(push_constant) uniform SceneTransform { mat4 mvp; mat4 modelView; } transformData;
vec3 safeNormalize(vec3 value) {
    float magnitude = length(value);
    return magnitude > 1.0e-6 ? value / magnitude : vec3(0.0);
}
void main() {
    vec3 n = safeNormalize(transpose(inverse(mat3(transformData.modelView))) * inNormal);
    vec3 objectLightPosition = vec3(transformData.modelView[0][3],
                                    transformData.modelView[1][3],
                                    transformData.modelView[2][3]);
    float lightRadius = transformData.modelView[3][3];
    vec3 objectToLight = objectLightPosition;
    float attenuation = 1.0;
    if (lightRadius > 0.0) {
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
    }
    vec3 lightDirectionEye = safeNormalize(mat3(transformData.modelView) * objectToLight);
    float diffuse = max(dot(n, lightDirectionEye), 0.0) * attenuation;
    float ambient = 0.22;
    vec3 diffuseColor = vec3(transformData.modelView[3][0], transformData.modelView[3][1],
                             transformData.modelView[3][2]);
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vertexColor = vec4(vec3(ambient) + diffuseColor * diffuse, 1.0);
}
