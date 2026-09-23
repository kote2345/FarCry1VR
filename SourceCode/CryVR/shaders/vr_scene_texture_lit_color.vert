#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 4) in vec4 inSecondaryColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec4 secondaryColor;
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
    vec3 toLight = transformData.objectLightPositionRadius.xyz;
    float attenuation = 1.0;
    if (transformData.objectLightPositionRadius.w > 0.0) {
        toLight -= inPosition;
        float distanceToLight = length(toLight);
        float normalizedDistance = distanceToLight / transformData.objectLightPositionRadius.w;
        if (normalizedDistance >= 1.0) {
            attenuation = 0.0;
        } else {
            attenuation = 2.0 * (2.0 * normalizedDistance * normalizedDistance * normalizedDistance -
                                 3.0 * normalizedDistance * normalizedDistance + 1.0);
        }
    }
    float diffuse = max(dot(safeNormalize(inNormal), safeNormalize(toLight)), 0.0) * attenuation;
    vec3 lighting = vec3(transformData.lightColorAmbient.w) + transformData.lightColorAmbient.rgb * diffuse;
    gl_Position = transformData.mvp * vec4(inPosition, 1.0);
    vec3 uv = vec3(inTexCoord, 1.0);
    texCoord = vec2(dot(transformData.uv0Row0.xyz, uv), dot(transformData.uv0Row1.xyz, uv));
    vertexColor = vec4(inColor.rgb * lighting, inColor.a);
    secondaryColor = inSecondaryColor;
}
