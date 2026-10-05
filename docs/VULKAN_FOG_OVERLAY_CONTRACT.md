# Fog overlay source contract

Reference: XRenderOGL/GLRendPipeline.cpp EF_DrawFogOverlayPasses and
Common/Shaders/ShaderCore.cpp system shader loading.

## Dispatch after material

OpenGL calls the overlay after detail passes when a fog volume is present and
r_VolumetricFog is enabled. The overlay returns for FOB_ZPASS or RBSI_FOGVOLUME.
This is distinct from ordinary distance fog; implementing one does not prove
the other is present.

Shader selection:

- Without hardware vertex shaders, or at bump quality zero, select TemplFog_FP
  unless the material owns a vertex shader.
- Otherwise, select TemplFogCaustics for non-water materials whose volume
  distance is within 0.1 of water level, at outer recursion, and within 40 units
  of the camera. Select TemplFog for the remaining cases.
- For a water material, temporarily add 0.1 to the volume distance around the
  overlay draw and subtract it afterward.
- Execute every pass of the selected shader's first hardware technique through
  EF_DrawGeneralPasses with fog-pass semantics.

## Missing Vulkan initialization and execution

The NULL_RENDERER build excludes system fog shader declarations/definitions
and loading in ShaderCore.cpp. Vulkan inherits this backend. The current Vulkan
item walker stores the selected fog volume but has no equivalent auxiliary fog
overlay dispatch. Its existing distance-fog uniforms cannot supply these passes.

File discovery, including ignored files, found no fog shader scripts in this
workspace. The system loader names external TemplFog/TemplFog_FP/
TemplFogCaustics templates. Their actual pass scripts and Cg/NVParse programs
must be read before implementing the fragment formulas; choosing a guessed
exponential function would not reproduce the source rendering.

## Implementation dependencies

1. Obtain the exact source shader scripts loaded by the working OpenGL backend.
2. Initialize the selected templates for Vulkan without enabling unrelated
   NULL backend system programs indiscriminately.
3. Transfer vertex fog/enter-plane coordinates from the shared parameter
   evaluators, blend/depth state, all selected fragment passes and samplers.
4. Queue the overlay at the material boundary after detail, preserving fog-pass
   treatment and temporary water-volume distance adjustment.
5. Compare output with OpenGL including water caustics and volume entry surfaces.

Status: source control flow reviewed; initialization, shader translation and
runtime parity incomplete. Other pipeline work remains available independently.

## Source assets located subsequently

Located the uncompiled source scripts in the local reference archive
`D:/games/Far Cry/FCData/Shaders.pak`. Selected templates, Fog/Caust Cg source
and their macro include are copied into `research/opengl-fog` for inspection.
This resolves local source discovery; exact runtime archive precedence still
needs comparison with the game configuration.

`TemplFog` has one pass: CGVProgFog generates FogEnterMatrix UV0 and
FogMatrix UV1; CGRCFog computes `tex2D(Fog, UV1) * tex2D(FogEnter, UV0)`,
then multiplies RGB by HDREncodeAmb(volumeColor). Blend is SRC_ALPHA /
ONE_MINUS_SRC_ALPHA with depth writes disabled and NoFog on the program.
`TemplFogCaustics` first draws CGVProgCaust/CGRCCaust with an animated
causq sequence, ONE/ONE blending and no depth write, then the ordinary fog
pass. `TemplFog_FP` expresses the two texture multiplication and volume color
with object-linear texgen and fixed combiners. These source assets allow an
explicit transfer; no Vulkan fog overlay implementation is claimed yet.

## System fog texture initialization

Found both CTexMan fog pointers uninitialized in the Vulkan/NULL backend.
Added lazy frame-entry initialization through the Vulkan texture manager:
`$Fog` uses the exact 128x128 RGBA generation from CGLTexMan::GenerateFogMaps
(0.982 attenuation table, radial index truncation, opaque borders), and
`Textures/FogEnter` loads with the same clamp/no-mips/no-remove/alpha flags.
The existing detail overlay previously could not enter its texture-dependent
branch because m_Text_Fog was null. This supplies that prerequisite as well as
the samplers for the future fog overlay. Release build succeeded; neither
runtime texture residency nor visual output has been verified.

## Shared coordinate generation

Added `BuildStockFogTexgen` using the exact shared SParamComp_FogMatrix and
SParamComp_FogEnterMatrix evaluators for S and T. It retains their wave,
object-transform, boundary smoothing and FOGVOLUME flag semantics. Detail
attenuation now consumes the FogMatrix branch with a temporary no-wave volume,
then restores the material's original volume. FogEnter support is ready for
overlay submission but is not yet dispatched as an auxiliary pass. Release
build succeeded; complete fog and caustics shading remains unfinished.

## Procedural fog GPU residency

CVulkanTexMan::CreateTexture can return a valid STexPic without a mirrored GPU
image when callbacks/upload are unavailable. Fog initialization now checks
HasLegacyTexture as well as the pointer and regenerates/reuploads into the same
STexPic when the GPU image is missing. It preserves the binding and avoids
allocating another texture object on retry. FogEnter file reload and full
auxiliary pass dispatch remain separate unfinished requirements.

## Ordinary fog overlay submission implemented

DrawBuffer now queues a two-texture auxiliary draw after the final material
pass and detail overlay, respecting volume presence, r_VolumetricFog, ZPASS
and FOGVOLUME suppression. It uses the exact shared FogEnter/Fog texgen planes,
volume RGB, texture alpha product, source-alpha blending, opaque-material
depth equality and protected depth/blend state. Lighting and ordinary distance
fog are disabled for this draw; main fog state is restored afterward. Shadow
comparison texgen from the preceding material is cleared for the fog samplers.
Water's temporary +0.1 volume distance is applied during coordinate generation.

Located HDREncodeAmb in CGVProgramms.csl: ordinary mode returns the input,
fake HDR uses HDR_OVERBRIGHT/2. The ordinary-mode formula is represented by
the two-stage multiplication here. HDR is still explicitly accounted as
untranslated; caustics remains missing. Auxiliary dispatch after techniques
ending with other special pass kinds and full batching require further work.
Release build succeeds, but texture residency and visual parity are unverified.

## Caustics auxiliary pass implemented

Added dedicated vr_caustics vertex/fragment programs and module lifecycle.
Pipeline selection uses program marker -12 in the existing programmable uniform
payload, isolated from terrain (-11) and water program keys. Vertex position uses
the ordinary object MVP. World P uses the shared TranspObjMatrix evaluator's exact
legacy columns; camera comes from GetCamera, and scrolling time uses shared
SParamComp_Time, preserving pause semantics. Vertex fade is the source expression
(1-distance*0.025)*(waterLevel-worldZ>=1)*0.75, without byte quantization or
fragment-side distance evaluation. The fragment multiplies every sampled channel
by interpolated fade and volume color and does not apply ordinary fog/lighting.

The parsed TemplFogCaustics first texture unit supplies the animation. Enabled
Layer parsing for CGRCCaust and CGRCFog in the NULL shader parser; mfUpdate selects
the source sequence frame with its existing timing/pause rules. Submission before
ordinary fog matches water-height, shader-sort, recursion, capability/quality and
minimum-camera-distance eligibility. ONE/ONE, opaque depth equality, protected
states, alpha threshold, object scissor and clip plane are carried to the draw.
Missing parsed sequence or GPU image is counted as untranslated, not fabricated.

Build succeeded before the final matrix-evaluator correction; rebuild required.
Runtime appearance, asset residency and batching remain unverified. Fog/HDR and
techniques ending in unsupported special passes still prevent full parity.
Final shared-matrix-evaluator revision also builds successfully; APK installation follows.
