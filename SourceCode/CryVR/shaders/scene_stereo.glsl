#extension GL_EXT_multiview : require
layout(constant_id = 63) const bool stockMultiview = false;
struct StockStereoTransform {
    mat4 mvp[2]; mat4 reflectionMvp[2];
    vec4 plantsAmbient; vec4 plantsBend;
};
layout(set = 0, binding = 3, std430) readonly buffer StockStereoTransforms {
    StockStereoTransform draws[];
} stockStereoTransforms;
mat4 stockStereoMvp() {
    if (stockMultiview)
        return stockStereoTransforms.draws[uint(transformData.mvp[0][0])].mvp[gl_ViewIndex];
    return transformData.mvp;
}
#ifdef STOCK_STEREO_WATER
mat4 stockStereoReflectionMvp() {
    if (stockMultiview)
        return stockStereoTransforms.draws[uint(transformData.mvp[0][0])].reflectionMvp[gl_ViewIndex];
    return transformData.reflectionMvp;
}
#endif
