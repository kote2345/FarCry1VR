vec3 evaluateStockVertexLighting(vec3 objectPosition, vec3 objectNormal,
                                 out vec3 separateSpecular)
{
    separateSpecular = vec3(0.0);
    if (textureStageTransforms.materialAmbient.w < 1.5)
        return vec3(1.0);

    // GL_Renderer disables GL_NORMALIZE. Preserve the supplied normal's
    // magnitude rather than silently changing fixed-function diffuse/specular.
    vec3 normal = objectNormal;
    int fixedLightCount = clamp(int(textureStageTransforms.fixedLightInfo.x), 0, 8);
    if (fixedLightCount > 0)
    {
        vec3 primary = textureStageTransforms.materialAmbient.rgb;
        vec3 specular = vec3(0.0);
        vec3 eyePosition = (textureStageTransforms.fixedMatrices[0] * vec4(objectPosition, 1.0)).xyz;
        normal = (textureStageTransforms.fixedMatrices[1] * vec4(objectNormal, 0.0)).xyz;
        vec3 viewer = vec3(0.0, 0.0, 1.0);
        for (int light = 0; light < fixedLightCount; ++light)
        {
            int base = light * 4;
            vec4 position = textureStageTransforms.fixedLights[base];
            vec4 diffuse = textureStageTransforms.fixedLights[base + 1];
            vec4 specularColor = textureStageTransforms.fixedLights[base + 2];
            vec4 coefficients = textureStageTransforms.fixedLights[base + 3];
            vec3 eyeLight = (textureStageTransforms.fixedMatrices[0] *
                vec4(position.xyz, position.w > 0.0 ? 1.0 : 0.0)).xyz;
            vec3 direction = eyeLight - (position.w > 0.0 ? eyePosition : vec3(0.0));
            float distanceToLight = length(direction);
            float attenuation = position.w > 0.0 ?
                1.0 / max(coefficients.x + coefficients.y * distanceToLight +
                          coefficients.z * distanceToLight * distanceToLight, 1.0e-6) : 1.0;
            direction = normalize(direction);
            float nDotL = dot(normal, direction);
            primary += diffuse.rgb * max(nDotL, 0.0) * attenuation;
            if (nDotL > 0.0 && any(notEqual(specularColor.rgb, vec3(0.0))))
                specular += specularColor.rgb * attenuation *
                    pow(max(dot(normal, normalize(direction + viewer)), 0.0),
                        clamp(specularColor.w, 0.0, 128.0));
        }
        separateSpecular = clamp(specular, 0.0, 1.0);
        return clamp(primary, 0.0, 1.0);
    }
    vec4 lightPositionRadius = textureStageTransforms.objectLightPositionRadius;
    vec3 lightVector = lightPositionRadius.xyz;
    float attenuation = 1.0;
    if (lightPositionRadius.w > 0.0)
    {
        lightVector -= objectPosition;
        float distanceToLight = length(lightVector);
        attenuation = 1.0 / max(textureStageTransforms.uvRow0[6].w +
                                textureStageTransforms.uvRow1[6].w * distanceToLight,
                                1.0e-6);
    }
    lightVector = normalize(lightVector);

    vec3 ambient = max(textureStageTransforms.materialAmbient.rgb, vec3(0.0));
    float normalDotLight = dot(normal, lightVector);
    vec3 diffuse = max(textureStageTransforms.lightColorAmbient.rgb, vec3(0.0)) *
                   max(normalDotLight, 0.0) * attenuation;
    vec3 specularColor = vec3(textureStageTransforms.uvRow0[4].w,
                              textureStageTransforms.uvRow1[4].w,
                              textureStageTransforms.uvRowQ[4].w);
    if (normalDotLight > 0.0 && any(notEqual(specularColor, vec3(0.0))))
    {
        vec3 viewDirection = normalize(vec3(textureStageTransforms.uvRow0[5].w,
                                            textureStageTransforms.uvRow1[5].w,
                                            textureStageTransforms.uvRowQ[5].w));
        vec3 halfVector = normalize(lightVector + viewDirection);
        float shininess = clamp(textureStageTransforms.lightColorAmbient.w, 0.0, 128.0);
        float specularFactor = pow(max(dot(normal, halfVector), 0.0), shininess);
        separateSpecular = clamp(specularColor * specularFactor * attenuation, 0.0, 1.0);
    }
    return clamp(ambient + diffuse, 0.0, 1.0);
}
