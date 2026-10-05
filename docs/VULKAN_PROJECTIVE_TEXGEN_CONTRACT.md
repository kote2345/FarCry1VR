# Projective texgen: remaining implementation contract

Generated no-UV modules now have external-lightmap-UV variants (24 total).
When a lightmap coordinate buffer is bound, the selected module reads input
location 5 and emits those UVs through the matching single/multi-stage output.
The existing compact key buffer bit distinguishes this module choice.
Secondary color and vertex lighting remain supported. This resolves external
UV transport, not stateful GL current-coordinate inheritance. Visual parity
and specialized programs are still open.

Generated no-UV projector direction now reads objectLightPositionRadius from
the shared UBO. Trailing push constants carry light data in the single-stage
normal path but texture matrices in multi-stage paths, so using those slots
for every generated variant was incorrect. Current-coordinate inspection
also confirmed many GL immediate draws leave nonzero current UV state;
zero defaults are not a complete state mirror. External lightmap UV input
for generated no-UV variants remains open.

Secondary-color no-UV formats are now wired into generated-coordinate
selection. Four additional variants read the real attribute at location 4,
preserve secondary color, and support single/multi-stage interfaces with
or without normals. The renderer creates and destroys twelve modules and
selects secondary variants from the actual vertex format. Current-coordinate
defaults and specialized programs remain incomplete; visual parity pending.

Runtime selection now enables generated no-UV single/multi-stage vertex
modules for linear texgen formats without secondary color. Texture selection
and UV1 validation accept the generated stages. Compact pipeline key bit 7
(below topology bits, above cull bits) distinguishes this module choice.
Native libraries and APK built. Secondary-color formats, current-coordinate
defaults beyond zero, specialized programs, and visual verification remain
open; this is not full no-UV parity.

No-UV vertex interface now has eight compiled/embedded variants: four
normal/color combinations for each single- and multi-stage fragment layout.
Multi-stage outputs place UV1 at location 2 and secondary color at location 3.
All eight modules share creation/destruction. Native libraries and APK built.
Pipeline selection and cache identity still need wiring; secondary-color
input for formats carrying it remains an explicit open requirement.

The four no-UV vertex SPIR-V variants are now embedded in
VulkanGeneratedTexgenShaders.h, loaded as modules during scene setup, and
destroyed/reset with the other scene shader modules. Native build and APK
succeeded. Runtime selection remains pending: multi-stage fragments require
UV1 at location 2 and secondary color at location 3, while the single-stage
interface uses secondary color at 2 and UV1 at 10. Add matching multi-stage
variants before selecting these modules for all no-UV generated draws.

No-UV vertex foundation: vr_scene_generated_texture.vert compiles four
position/color/normal combinations into SPIR-V via CryVRGeneratedTexgenShaders.
It emits the textured fragment interface, zero default UVs, and shared
vertex lighting when normals are present. SPIR-V compilation and APK build
succeeded. Embedding, module lifecycle, shader selection, and secondary-color
handling are still pending; these new variants are not selected at runtime.

Linear generator collection no longer requires a resolved m_TexPic: GL
enables pass m_GTC outside the image-binding branch. Optional projection
matrix access is now null-safe. Native libraries and APK built successfully.

Remaining shader-selection gap: VulkanFrameRenderer textureExpected and
secondTextureHasUvSet still depend on vertex UV streams. A fully generated
linear stage should work without mesh UVs, as GL texgen does. Current
position-only scene vertex shaders do not emit the textured fragment
interface, so enabling sampling alone is insufficient. Implement compatible
no-UV textured vertex variants (or a uniform generated-coordinate interface),
including material lighting and every relevant multi-stage variant, then
adjust selection for enabled generated S/T and disabled-component defaults.

Removed the unreachable CPU linear-plane division branch after confirming
the shared fragment coordinate function is used by the textured single,
two-, three-, and four-to-eight-stage scene shader variants. This cleanup
does not establish specialized water/terrain or cubemap texgen equivalence.
The native build and release APK succeeded. The checkpoint APK was installed
on Quest 3; the application was not running and was not launched by the agent.
Runtime visual verification is pending.

## Higher-stage linear collection implemented

All eight stage units now collect valid ObjectLinear/EyeLinear S/T/R/Q
planes and their complete material/operation matrix. Each light draw submits
these records independently. Higher stages no longer reject a translated
linear generator merely because their software eGenTC mode is different.
Other hardware generators on stages 2-7 still record an unsupported-path
diagnostic. The former stage-0/1 limit below is historical; specialized
shader consumers, remapping, and runtime visual parity remain open.

## Active migration of stages 0/1

ObjectLinear/EyeLinear planes are now collected from pass generators and
snapshotted with the final full material matrix into each light draw. The
CPU path leaves mesh UVs unchanged for these generators. Valid R planes are
preserved too. The shared fragment coordinate function consumes the records
for stages 0/1; detail overlays reset the generator state before submission.
Other coordinate modes and higher-stage collection remain incomplete.
Lightmap-stage-0 matrix remapping and specialized shader consumers require
further source comparison. Visual parity has not been verified.

## Capture foundation implemented

The SceneUniformBlock now appends 32 plane rows, 32 texture-matrix rows and
eight control vectors. Descriptor ranges and all nine matching GLSL block
declarations include these fields. Both eye uploads and buffer-growth
reuploads populate the records. The shared fragment coordinate function
evaluates enabled linear components from interpolated objectPosition, applies
the full matrix including generated R, and divides by transformed Q.
Renderer plane collection/enabling is still pending; CPU generation remains
active until that migration is wired.

VulkanStockLinearTexgen holds four plane rows, the full texture matrix,
component mask, enable flag and input UV selection. VulkanFrameRenderer
stores eight independent stage records and snapshots them into StockDraw.
DrawRenderItem resets this state for each pass. Native libraries and APK
built after adding this interface. Plane collection and UBO/GLSL consumption
are still pending; this foundation does not change projective sampling yet.

Source references: XRenderOGL/GLShaders.cpp SGenTC_ObjectLinear::mfSet and
SGenTC_EyeLinear::mfSet; GLTextures.cpp SShaderTexUnit::mfSetTexture.

Both linear generators reset the current element's m_nCountCustomData before
reading valid plane parameters. Vulkan now does the same.

## Unresolved coordinate transport

VulkanRenderer.cpp generateLegacyTexCoords currently divides generated S/T
by Q on the CPU and writes two floats into UV0/UV1. This loses homogeneous Q
before raster interpolation. Stages above 1 also share UV1 and therefore
cannot carry independent generated coordinates through this path.

The scene fragment shaders already receive interpolated objectPosition.
Linear planes can instead be evaluated from that position in the fragment,
then transformed as a homogeneous four-vector by the full texture matrix,
and finally divided by transformed Q. This preserves interpolation for
linear object/eye planes on the current direct-item path. Normal/reflection/
sphere generators require their own coordinate transport and sampler type.

Implementation must carry four planes, enabled-component mask, and full
texture matrix per stage. Disabled components retain the selected input
coordinate stream. Apply generated R before the matrix too: a 2D sampler
does not consume R directly, but the texture matrix can mix it into S/T/Q.

Extend the C++ draw capture, SceneUniformBlock, descriptor size, all matching
GLSL uniform declarations, and shared stage-coordinate function together.
Account for stage remapping (lightmap stage 0), per-eye copies, resumed passes,
buffer growth, and specialized water/terrain programs. Remove CPU generation
for migrated modes only after the new data reaches the selected shader.

This is an open requirement. Builds and ordinary 2D UV sampling do not prove
projective texgen parity.
