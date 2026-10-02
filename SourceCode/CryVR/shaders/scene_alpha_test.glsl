// Defaults to the original behavior for pipelines without specialization.
layout(constant_id = 62) const bool stockFragmentDiscardEnabled = true;

bool stockAlphaTestPasses(float alpha, float materialAlphaRef, int stateMode)
{
    // CGLRenderer::EF_SetResourcesState replaces the pass alpha function when
    // a material AlphaRef is set, and uses GL_GEQUAL. Do not also apply the
    // alpha-test bit from the pass in that case.
    if (materialAlphaRef > 0.0)
        return alpha >= materialAlphaRef;
    if (stateMode == 1) return alpha > 0.0;   // GL_GREATER, 0
    if (stateMode == 2) return alpha < 0.5;   // GL_LESS, 0.5
    if (stateMode == 3) return alpha >= 0.5;  // GL_GEQUAL, 0.5
    if (stateMode == 4) return alpha >= 0.25; // GL_GEQUAL, 0.25
    return true;
}
