#version 450
layout(constant_id = 0) const int alphaTestMode = 0;
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform SceneFog { vec4 fogColor; vec4 fogModeDensityStart; vec4 fogEndDepthRange; vec4 materialParams; } fogData;
vec4 applyMaterialOverrides(vec4 c) {
    if (fogData.materialParams.z > 0.5) c.rgb *= fogData.materialParams.x;
    else c.a *= fogData.materialParams.x;
    if (fogData.materialParams.y > 0.0 && c.a < fogData.materialParams.y) discard;
    return c;
}
vec4 applySceneFog(vec4 c) {
    if (fogData.fogModeDensityStart.x < 0.5) return c;
    float n=fogData.fogEndDepthRange.y, f=fogData.fogEndDepthRange.z;
    float d=n*f/max(f-gl_FragCoord.z*(f-n),1.0e-7);
    int m=int(fogData.fogModeDensityStart.y+0.5); float a;
    if(m==1) a=(d-fogData.fogModeDensityStart.w)/max(fogData.fogEndDepthRange.x-fogData.fogModeDensityStart.w,1.0e-6);
    else if(m==2){float x=fogData.fogModeDensityStart.z*d;a=1.0-exp(-min(x*x,80.0));}
    else a=1.0-exp(-min(fogData.fogModeDensityStart.z*d,80.0));
    c.rgb=mix(c.rgb,fogData.fogColor.rgb,clamp(a,0.0,1.0));return c;
}
void main() {
    vec4 color = applyMaterialOverrides(texture(baseColorTexture, texCoord) * vertexColor);
    if (fogData.materialParams.y <= 0.0) {
        if (alphaTestMode == 1 && !(color.a > 0.0)) discard;
        if (alphaTestMode == 2 && !(color.a < 0.5)) discard;
        if (alphaTestMode == 3 && !(color.a >= 0.5)) discard;
        if (alphaTestMode == 4 && !(color.a >= 0.25)) discard;
    }
    outColor = applySceneFog(color);
}
