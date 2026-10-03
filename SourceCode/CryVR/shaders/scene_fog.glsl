// Shared OpenGL distance fog. CPU preparation removes viewport divisions and
// depth-range arithmetic from every fragment, without approximating the ray.
#ifndef STOCK_FOG_UNIFORMS
#define STOCK_FOG_UNIFORMS textureStageTransforms
#endif
layout(constant_id = 69) const uint stockFogMode = 0u;
layout(constant_id = 63) const bool stockFogMultiview = false;

vec2 stockFogEyeRay()
{
    // Ordinary multiview draws share one UBO, but the two OpenXR FOVs
    // need not be symmetric. Preserve the independent right-eye ray.
    if (stockFogMultiview && gl_ViewIndex == 1)
        return vec2(gl_FragCoord.x * STOCK_FOG_UNIFORMS.fogEye1Ray.x + STOCK_FOG_UNIFORMS.fogEye1Ray.y,
                    gl_FragCoord.y * STOCK_FOG_UNIFORMS.fogEye1Ray.z + STOCK_FOG_UNIFORMS.fogEye1Ray.w);
    vec2 ray = vec2(
        gl_FragCoord.x * STOCK_FOG_UNIFORMS.linearControls[4].w + STOCK_FOG_UNIFORMS.linearControls[5].w,
        gl_FragCoord.y * STOCK_FOG_UNIFORMS.linearControls[6].w + STOCK_FOG_UNIFORMS.linearControls[7].w);
    return ray;
}

float stockFogEyeDepth()
{
    return STOCK_FOG_UNIFORMS.fixedLightInfo.y /
        max(STOCK_FOG_UNIFORMS.fixedLightInfo.z -
            gl_FragCoord.z * STOCK_FOG_UNIFORMS.fixedLightInfo.w, 1.0e-7);
}

vec4 applyStockSceneFog(vec4 color)
{
    // 0 is the uniform-driven fallback; 4 is a known disabled fog pass.
    if (stockFogMode == 4u ||
        (stockFogMode == 0u && STOCK_FOG_UNIFORMS.fogModeDensityStart.x < 0.5))
        return color;
    uint mode = stockFogMode != 0u ? stockFogMode : uint(STOCK_FOG_UNIFORMS.fogModeDensityStart.y + 0.5);
    vec2 ray = stockFogEyeRay();
    float eyeZ = stockFogEyeDepth();
    float rayLengthSquared = 1.0 + dot(ray, ray);
    float amount;
    if (mode == 2u)
    {
        // EXP2 fog uses distance squared: sqrt followed by squaring cancels.
        float d = STOCK_FOG_UNIFORMS.fogModeDensityStart.z * eyeZ;
        amount = 1.0 - exp(-min(d * d * rayLengthSquared, 80.0));
    }
    else
    {
        float distance = eyeZ * sqrt(rayLengthSquared);
        if (mode == 1u)
            // Density is unused by GL_LINEAR; the CPU supplies inverse range.
            amount = (distance - STOCK_FOG_UNIFORMS.fogModeDensityStart.w) * STOCK_FOG_UNIFORMS.fogModeDensityStart.z;
        else
            amount = 1.0 - exp(-min(STOCK_FOG_UNIFORMS.fogModeDensityStart.z * distance, 80.0));
    }
    color.rgb = mix(color.rgb, STOCK_FOG_UNIFORMS.fogColor.rgb, clamp(amount, 0.0, 1.0));
    return color;
}
