# Follow-up: terrain detail and particle rectangles

The headset report after the preceding build still shows angle-dependent terrain
detail and black particle rectangles. The fresh material census contains a real
cloud2 texture instead of the previous red NULL-renderer placeholder.

Two further discrepancies were found:

1. `CTerrain::MakeSplatVertex` only initializes position and alpha. Normals are
   filled later only when a terrain layer has a material. Vulkan's translated
   `PosTerrainLayerOverlay` reads the normal for position displacement even for
   the mask-zero simple layers. Initialize that source to the up normal before
   material-specific normals are applied; otherwise reused stack data affects
   clip position and depth rejection as the head rotates.
2. OpenGL `EF_DrawGeneralPasses` and light passes select the object's nonzero
   `m_RenderState` before the pass state. Vulkan never read this field. The
   particle builder assigns additive, color-based, or alpha-based blending to
   this field. Restore the override for hardware draws, preserving stencil and
   keeping the dedicated fog pass state. This allows RGB particle textures to
   use their original blending rather than drawing their black background.

These are source-level corrections. Visual confirmation remains necessary.
