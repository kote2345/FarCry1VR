# Sequential OpenGL pipeline checkpoint

## Ocean sector CPU geometry parity

Compared GLREOcean::mfDrawOceanSectors with NULL_REOcean::mfDrawOceanSectors,
which generates the mesh submitted to Vulkan. GL sorts visible sectors after
LinkVisSectors; NULL previously omitted this ordering. Added the same sort.
GL CGVProgOcean scales displaced wave height by seabed-depth fade, then applies
camera-distance curvature independently. NULL had multiplied curvature by the
seabed fade; corrected vertex Z to waveHeight*heightScale*fade - curvature +
waterLevel. NULL/Vulkan dependent libraries and Android APK build successfully.
Visual ocean parity and WaterMap reflection remain unverified/incomplete.


## Terrain base and layer fog routing

OpenGL terrain base CGRCTerrain and CGRCTerrain_NLayers write their explicit
program color; terrain fog is handled by separate terrain fog passes. Vulkan's
generic fragment fog was being applied on the base and layer material draws.
Mark all recognized stock terrain layer counts (including base count zero) in
the terrain projection UBO and bypass generic scene fog in textured, multitexture,
three-texture and four-texture fragments for those draws. TerrainFogPass remains
separately dispatched. Native/APK build succeeds; visual runtime parity pending.


## Four-layer detail scale and fog

CGRCTerrain_4Layers_Only forms layer zero as texel*weight + 0.5*(1-weight),
then multiplies each remaining detail term by two. Use the unscaled first blend
and the existing x2 detail helper for layers 1..3; the prior implementation
x2-scaled all four, doubling its result. The Cg program writes direct output and
does not call HDRFogBlend; bypass generic scene fog for this pass. APK/native
rebuild succeeds. Installed build and Quest appearance remain unverified.


## Terrain detail pass channel mapping correction

The OpenGL vertex source was checked directly: for the four detail-only pass,
OUT.Color.b = IN.Color1.r * fade; OUT.Color.a = IN.Color1.a * fade;
OUT.Color1.b = IN.Color1.g * fade; OUT.Color1.g = IN.Color1.b * fade.
Vulkan CPU preparation now writes those outputs to matching packed channels,
and fragment sampling consumes primary B/A and secondary B/G for detail weights
0..3. This corrects channel mismatch in the preceding four-layer implementation.
Native/APK build succeeded after the change; visual verification pending.

## OpenGL ocean reflection source contract and Vulkan port

GLRendPipeline EF_Preprocess handles SPRID_SCANTEXWATER: it updates the 512x512
WaterMap by DrawToTexture with a water-level clip plane and reflected camera,
subject to water reflection update controls. CGVProgOcean projects displaced
position through ReflectMatrix into TEXCOORD1; CGRCOcean samples that WaterMap
using DSDT offsets.

Vulkan records a mirrored scene into a dedicated per-eye color target and now
binds that target as the mode-6 CGRCOcean reflection sampler. The refraction
snapshot has its own target and descriptor, so an indoor-water copy cannot
overwrite the ocean reflection. Reflection projection is generated from the
mirrored eye MVP. Capture triggers now follow OpenGL's water-distance time
factor, camera-position and camera-angle thresholds, FOV-change check, and
backward-time reset. Source/build implementation is present; headset validation
is still pending. The target currently uses eye resolution instead of
OpenGL's 512x512 target.


## Terrain four-layer-only weights

Compared TerrainDetailLayers in terrain.csl and CGVProgTerrain_4Layers_Only /
CGRCTerrain_4Layers_Only. Recognize the fourth detail-only template, allow its
four-layer marker, and evaluate four detail textures without a base albedo.
Restore the vertex-program channel contract: detail weights are Color.b,
Color1.b, Color1.g and Color.a, each faded by pow4(distance/FadingDist).
Vulkan had populated unrelated channels and sampled weights from the wrong
components. Shader/native/APK rebuild succeeds; no runtime parity evidence.
Terrain shadows and other variants remain incomplete.

## Ocean reflection target separation

The mirrored-scene ocean capture and indoor-water refraction snapshot used to
share `waterCopy`. Added a distinct reflection color image, framebuffer, and
descriptor per eye; mode-6 ocean draws sample it, while modes 2/4 continue to
sample the scene-color snapshot. Android native build and release APK assembly
succeeded. Runtime image/layout behavior still requires Quest verification.


## Terrain texgen full planes

Terrain base/detail layer projection copies previously kept xyz only. Original
CGVProgTerrain_NLayers computes dot(LayerNTexGen, vPos), including the fourth
translation coefficient. Copy all four components for both FromRE layer ranges.
Base cover mapping already retained its offsets. Build succeeds; visual effect
unverified and APK not installed. Terrain ambient/HDR, far/no-color variants,
layer templates, shadows and light passes remain incomplete.


## Depth-state decoder comparison

Compared GLRendPipeline EF_SetState depth branches with VulkanPipelineState:
LEQUAL default, EQUAL priority over GREATER, independent depth write and test
flags match. NULL EF_SetState resolves RBSI_DEPTHFUNC/DEPTHWRITE/DEPTHTEST
inheritance before deferred submission. No confirmed decoder change is justified
by this comparison. Camera-dependent godray overlap remains unresolved: next
scope is pass submission order, clears and recursive target boundaries. This
source comparison does not prove complete depth/transparent rendering parity.


## Secondary-color normal materialization

Extend missing-normal conversion to P3F_COL4UB_COL4UB and its TEX2F variant,
using matching normal-bearing layouts and preserving secondary color bytes.
Existing normal-bearing layouts already preserve the entire record. This closes
a layout eligibility gap in resource/explicit normal commit for plant and other
secondary-color leaf streams. Format 16 lacks an equivalent legacy normal layout
and remains unresolved. Build succeeds; APK not installed or visually verified.


## Generated-coordinate shader selection

QueueStockClientIndexedDraw previously restricted no-UV generated variants to
nonprogrammable materials and required linear texgen even when stage zero used
a separately bound lightmap stream. Select existing normal/color/multistage
variants for either coordinate source and permit programmable materials. Water
keeps its dedicated shader selection. This prevents missing location-3 reads in
this path and retains committed normals. Other programmable coordinate semantics
remain incomplete. CryVR and dependent native libraries plus APK build successfully;
latest APK not installed or visually verified.


## Normal materialization without stored UV

Extend committed normal conversion to P3F and P3F_COL4UB, selecting P3F_N
and P3F_N_COL4UB respectively and preserving the absence of stored UV. GL normal
pointer commit is independent of texture-coordinate storage; generated-coordinate
materials must not lose lighting solely because their general stream has no UV.
Existing lit vertex helpers pass raw normals to fixed evaluation, consistent
with GL_NORMALIZE disabled. Build succeeds; latest APK not installed or visually
verified. Overall pipeline parity remains incomplete.


## Explicit general normal source

Resolve eSrcPointer_Normal from the current leaf primary general stream using
its actual format/stride, matching CREOcLeaf::mfGetPointer flags zero. This also
supplies normals when the translated destination layout omitted them; generated
vertex copies no longer implicitly stand in for the explicitly bound source.
Tangent-based copying is restricted to GL_FLOAT declarations (resource override
always uses float). Other declared normal types are still unsupported rather
than treated as float. Build succeeds; no runtime verification or installation.


## Explicit tangent-based normal destinations

Track normal destinations alongside texture pointers: technique declarations
then pass overrides. Resource normal commit remains the final TNormal override,
as in GL EF_CommitStreams. Supported tangent/binormal/TNormal sources now copy
from the current primary tangent stream into the deferred vertex snapshot.
An explicit pointer prevents the unrelated secondary-normal fallback. General
normal pointers retain the existing primary attribute; other derived source
kinds, pointer Type conversion, inheritance across passes and missing streams
remain incomplete. Native/APK build succeeds; this APK is not installed yet.


## Normal override preserves complete existing layouts

Extended resource TNormal override to every existing float3 normal layout:
copy the complete vertex record and replace only its normal. This preserves
secondary colors and additional UV attributes. Resource eligibility now applies
outside ambient/light pass classification, matching unconditional resource-normal
commit in GL EF_CommitStreams. Layouts without normal storage still use the
restricted conversion path; missing tangent residency and explicit normal pointer
state remain unresolved. Native libraries and release APK build successfully.
Runtime parity remains unverified.


## Resource normal stream override

GL_Renderer.h EF_CommitStreams installs eSrcPointer_TNormal whenever resources
require normals. CREOcLeaf sGetBuf with flags zero resolves the current primary
VSF_TANGENTS, not the secondary source general normal. Vulkan now copies this
current tangent normal into supported textured vertex layouts, overriding an
existing general normal as well as supplying a missing one. Secondary normals
remain the prior fallback only when the resource override does not apply.
Missing tangent residency, other vertex layouts, explicit pointer inheritance
and programmable derived attributes remain under audit. Native/APK build succeeds;
visual parity is not established.


## Fixed transparent material retains all accepted lights

Resource opacity handling previously truncated lightPasses to one entry before
the newly added fixed-light array was packed. Excluded fixedFunctionMaterialLighting
from that truncation: all accepted fixed lights reach the array and the later
single-draw collapse, with framebuffer opacity/blend applied once. Programmable
pass handling retains its prior behavior and remains separately under audit.
This corrects a source-level loss of lighting for transparent fixed materials;
runtime appearance still requires verification.

## Fixed projector payload separation

GL EF_SetLights installs projector lights as positional GL lights without the
programmable cookie/cone payload. Vulkan already guarded direction/cookie
reconstruction by eSHP_MAX, but a later cone-angle assignment still overwrote
the fixed shininess slot. That assignment is now programmable-only. Normal
matrix inversion is now restricted to draws with active fixed-light arrays;
other draws retain identity for the unused tail. Build and runtime evidence
must not be used to claim full projector/lighting parity.

## Fixed lighting eye-space evaluation

Added model-view and inverse-transpose normal matrices to the fixed lighting
UBO tail, with matching full vertex/fragment/water declarations and descriptor
size. Fixed vertex accumulation now transforms positions and accepted legacy
light coordinates to eye space, evaluates attenuation using eye-space distance,
and transforms normals without normalizing (GL_NORMALIZE is disabled). The
nonlocal viewer is constant eye +Z. Camera matrix bytes were compared with GL
SetCamera, which likewise loads GetVCMatrixD3D9 directly. All this is source
correspondence, not runtime proof. Projector dispatch and generated normal
streams remain under audit; overall renderer parity remains incomplete.

## Fixed light producer and single material draw connected

For fixedFunctionMaterialLighting with translated dynamic lights, pack all
accepted legacy light parameters into up to eight UBO records before draw
submission. Preserve material shininess independently of prior per-light
ambient-suppression payload edits. Collapse the fixed light draw vector to
one material submission, snapshot the array and clear transient array state
immediately afterward. Programmable light pass vectors retain their ordering.
The shader accumulation branch is now active for this fixed path. Build succeeds.
Object-space attenuation/normal transforms for scaled objects remain different
from GL eye-space evaluation; full fixed lighting parity and visual effect
are unverified. This does not establish programmable skeleton material parity.

## Fixed light vertex accumulation helper

Extended all vertex variants using scene_vertex_lighting.glsl to the complete
scene UBO layout. The helper now supports up to eight fixed lights, adds diffuse
to material ambient and accumulates separate specular, clamping only after the
sum. Each light carries point/directional position, diffuse, specular/shininess,
and constant/linear/quadratic attenuation. Count zero retains the existing path.
CPU producer and one-material-draw scheduling are still not connected, so the
new branch is not active yet. Eye-space transforms for scaled objects remain
open. Runtime lighting parity is not established.

## Fixed light array transport foundation

Added eight fixed-light records (four vec4 per light) and count to StockDraw
and the scene UBO tail. SetStockFixedLights snapshots data into deferred draws;
both normal and reuploaded eye blocks copy the array, with descriptor size
and CPU static_assert adjusted. Full fragment/water UBO declarations include
the new tail. Existing abbreviated vertex declarations still need extension
before the lighting helper can consume it. Producer selection, shader loop and
single-draw scheduling are not connected yet; this step changes the transport
contract, not rendered lighting. Build succeeds; not installed.

## Fixed-function lighting accumulation contract (next implementation)

GL EF_SetLights installs up to eight compact active lights simultaneously;
EF_SetHWLight sets zero per-light ambient and the exact diffuse/specular and
constant/linear/quadratic attenuation. Vertex lighting then produces one primary
and separate specular before the texture combiners and framebuffer blend.
Vulkan currently sends one StockDraw per entry in lightPasses, with only one
objectLightPositionRadius/color payload consumed by scene_vertex_lighting.glsl.
This is not equivalent: clamp(sum(diffuse)+ambient) differs from separate
clamped draws, and texture combiner/blend/alpha operations execute repeatedly.
This remains a confirmed architectural gap, including transparent materials.

Required implementation: carry all accepted fixed lights in a per-draw UBO;
preserve compact selection, DLF_LM/specular-only and LMF filters; accumulate
vertex primary/specular once; issue one material draw. Hardware programmable
light passes must retain their reference pass sequence. Eye-space position and
inverse-transpose normal handling must be carried alongside the lights for
scaled objects. Existing one-light helper is not evidence of full parity.

## Beam alpha generator CPU eligibility

SEvalFuncs_C inherits an empty EALPHA_Beam, whereas SEvalFuncs_RE needs tangent
normals and modifies beam positions/colors. Vulkan's tangent requirement now
applies only when an RE is active. Previously a no-RE Beam-alpha pass bypassed
the exact CPU evaluator (including its RGB generator) and used the approximate
uniform evaluator. Runtime behavior unverified; source/destination lifetime and
full material-lighting parity remain open.

## CPU evaluator destination offsets

Vulkan's CPU color evaluation previously installed actual stride/color offset
but left normal/UV offsets from the shader's requested format. Direct CPU
evaluators use m_Ptr plus those offsets, so a converted draw could read the
wrong attributes. Evaluation now installs actual destination normal/UV offsets
and restores them afterward alongside stride/color. The intercept also exposes
the actual secondary-color destination offset. Missing-attribute generator
dispatch remains an unresolved requirement. Build succeeds; not installed.

## CPU generator source format separated from destination

SCpuDrawStream now carries a distinct source stride and attribute offsets.
EF_GetPointer(FGP_SRC) uses that layout, while mutable pointers retain the draw
copy layout. Vulkan color evaluation supplies the available secondary general
stream and its resolved format, matching CREOcLeaf::sGetBuf source ownership.
Tangent reads retain their separate stream. Missing secondary source fallback
and unsupported generated vectors remain incomplete; this does not prove all
CPU generators or material lighting equivalent. Runtime verification pending.

## Software projection dispatch and texgen range

EF_Eval_TexGen dispatches Projection to the selected CPU/RE evaluator.
SEvalFuncs_C::ETC_Projection is empty, so Vulkan now preserves input UV for
Projection without a render element (hardware m_GTC generators remain separate).
CPU vertex-copy texgen now writes only the current RE range, matching FGP_REAL
and m_RendNumVerts in EvalFuncs_RE, with bounds validation. No-element geometry
retains its full local vertex range. Build succeeds; not installed or visually
verified. This does not resolve cross-pass pointer/array-enable persistence.

## Environment texgen CPU/RE split

SEvalFuncs_C::ETC_Environment uses ViewOrg minus current CPU position, physical
normal, and V=(reflectedZ+1)/2. SEvalFuncs_RE instead uses object translation
minus FGP_SRC position, source TNormal, and V=0.5-reflectedZ/2. Vulkan previously
used the RE expression in both paths. Environment generation now follows the
appropriate evaluator and tangent-normal eligibility requires an active RE.
Build succeeds; visual parity remains unverified. CPU Projection is empty in
the reference and requires a separate dispatch correction.

## FGP_SRC position stream for software texgen

EvalFuncs_RE environment/projection request source positions with FGP_SRC.
CREOcLeaf::sGetBuf selects the secondary general stream in that case. Vulkan
previously used the retained primary general stream as originalPosition.
It now reads available secondary float3 positions with their own vertex format,
offset and stride, retaining the old fallback when that source is unavailable.
That fallback is not full OpenGL missing-stream equivalence. Source normal/UV
selection and cross-pass enable lifetime still require work. Build succeeds;
no visual verification or installation performed.

## CPU versus render-element deform evaluator selection

SEvalFuncs_C::VerticalWaveDeform is empty, whereas SEvalFuncs_RE implements it.
Vulkan now skips VerticalWave for no-render-element geometry. Wave/Squeeze/Bulge
read the physical general normal in the CPU path, and TNormal in the RE path;
Vulkan now limits tangent-normal selection to the RE path and validates the
actual chosen normal source. Sin-table phase/scale and VerticalWave +Z direction
were compared with EvalFuncs_RE. Build succeeds; runtime parity remains open.

## Deform vertex range

EvalFuncs_RE obtains FGP_REAL vertex/tangent/UV pointers and processes
m_RendNumVerts; CREOcLeaf offsets real pointers by the current first vertex.
Vulkan's Beam and ordinary Wave/Squeeze/Bulge/FromCenter loop previously ran
over all m_NumVerts. These loops now use the current render-element range
m_FirstVertex/m_RendNumVerts, validated against the retained buffer; no-element
draws retain the full local stream. Flare replacement is a separate path and
remains under audit. Build succeeds; runtime effect not verified.

## Hardware pass CPU deforms

GL EF_DrawGeneralPasses and EF_DrawShadowPasses call EF_Eval_DeformVerts on
SShaderPassHW::m_Deforms after the material-level list. Vulkan previously only
evaluated SShader::m_Deforms. The private vertex-copy evaluator now receives
the material list followed by the current General/Shadow/MultiShadows list.
Existing unsupported deform and flare replacement restrictions still apply.
Cross-pass cumulative mutation of shared OpenGL streams remains unported; this
adds current-pass evaluation, not complete deformation lifetime parity.
Native libraries and release APK build successfully; not installed.

## Separate lightmap-stream upload and active shader inspection

Rechecked CREOcLeaf::mfGetPointer(TexLM): an object LMTC buffer supplies its
TEX2F stream at byte offset zero; without it the normal general stream is returned
at offset zero. Vulkan's explicit-pointer fallback copies position X/Y, agreeing
with the inspected float2 case. Upload validates referenced chunk indices when
the LM stream is shorter than the general vertex buffer. VulkanPipelineFactory
replaces location 5 with binding 2 when the separate stream exists, rather than
adding a duplicate attribute. Pointer type/component interpretation remains open.

Inspected actual fragment dispatch: ordinary textured single-stage draws select
m_sceneTextureColorFragmentShader even without a physical color attribute.
The simplified vr_scene_texture.frag therefore is not evidence of an active
missing-combiner regression; do not change it on that assumption. Stage UV
selection happens before stockTerrainStageTexCoord applies the stage matrix.
Next unresolved source contract: pass array enable masks and pointer bindings
have distinct lifetimes (GL_Renderer.h::EF_CommitStreams); retaining a pointer
alone is insufficient to reproduce cross-pass sampling.

## Texture matrix ownership independent of selected UV array

Removed the swap of stage-zero/stage-one matrices when stage zero selects
TexLM. Vulkan fragment variants select raw texCoord0/1 using specialization
flags and then stockTerrainStageTexCoord applies uvRow[stage]; QueueStockDraw
populates those rows from the corresponding destination-stage matrix. Swapping
these matrices therefore applies the other stage's transform. OpenGL's texture
matrix belongs to the active texture unit, independently of glTexCoordPointer.
Stage-zero and stage-one uploads now retain their own matrices. Native renderer
and release APK build successfully; runtime verification remains pending.

## Stage-zero lightmap array selection

The submitted stage-zero UV flag and linear-texgen UV input now follow that
stage's resolved pointer, instead of equality with the single last lightmapStage
marker. This preserves stage-zero TexLM when another stage declares TexLM later.
Stage-one explicit pointer selection is also committed immediately before draw
submission, including paths without a stage-one texture-unit declaration.
Build succeeds. Cross-pass binding/enable-state lifetime and exact stage-specific
matrix handling remain under audit; full simultaneous-stage parity is unproven.

## Explicit UV source precedes sampler resource classification

For stages 1 through 7, explicit technique/pass texture pointers now determine
whether the separate TexLM stream is selected. Previously sampler resource
slots LIGHTMAP/LIGHTMAP_DIR/OCCLUSION could re-enable lightmap UV after a pass
replaced its pointer with ordinary Tex. GL texture binding does not replace
glTexCoordPointer. Resource/generator heuristics remain only where no explicit
pointer is declared. Full stage-0, generated pointer and inherited state support
is still open. Build succeeds; runtime appearance remains unverified.

## Resolve technique/pass UV bindings by destination

The current draw now collects texture-coordinate pointers separately for all
eight destination stages, applying technique declarations then pass declarations.
A non-TexLM pass pointer replaces the technique's TexLM at the same stage
instead of leaving it classified as a lightmap array. Declaration sequence is
retained when selecting the surviving TexLM stage. This handles replacement
within the current technique/pass pair; cross-pass GL pointer/cache and enable
mask lifetime, simultaneous lightmap stages, and generated sources are still
incomplete. Native libraries and release APK build successfully. Not installed
and no visual result is claimed.

## Lightmap pointer destination classification

ShaderComponents.cpp creates SArrayPointer_Texture only for eDstPointer_Tex0;
only that class updates m_nLMStage in OpenGL. Vulkan's technique/pass scan
previously recognized TexLM solely by source, incorrectly treating TexLM routed
to color/normal/position as a UV-stage assignment. Both scans now also require
the texture destination. Native renderer and APK build successfully.
Still open: replacing an existing stage's TexLM with another source, tracking
multiple active arrays, and inherited pass pointer bindings. These require a
per-stage binding representation rather than the present single-stage marker.

## Hardware pass lightmap pointer precedence

GLShaders.cpp::SArrayPointer_Texture::mfSet updates m_nLMStage on every TexLM
pointer. EF_FlushHW applies technique pointers before each pass's pointers.
Vulkan previously stopped at the first technique TexLM and skipped pass TexLM
when lightmapStage was already set. Removed first-pointer breaks and the
already-set gate: the last declared pass TexLM now overrides the technique
stage, matching the OpenGL stage-selection order. Multiple simultaneously
enabled lightmap arrays and inherited non-LM replacement pointers still require
full per-stage stream tracking; this correction does not establish that coverage.
Native renderer and release APK build successfully; runtime verification pending.

## Object and immutable state comparison (follow-up)

Compared GL EF_ObjectChange nearest transition against UpdateStockNearestCamera:
both save/restore the camera, use near/far 0.01/40, FOV factor 0.6666 and depth
range 0/0.1, restoring the pipeline min/max on exit. Vulkan applies this on
object transitions. This establishes CPU parameter correspondence only, not
stereo projection identity.

Compared GL EF_SetState with NULL shared state resolution and Vulkan immutable
state decoding: depth EQUAL precedes GREATER, default LEQUAL; color-mask priority
is NONE, ALPHA, RGB, RGBA. Protected blend/depth/write/test/stencil/alpha flags
retain their prior cached values. MapStockCullMode honors RBF_2D as no culling.
No new difference established in these inspected branches.

Compared fixed EF_FlushShader mesh preparation with DrawStockElement: resource
normal requirement and failed mfCheckUpdate rejection agree for ordinary leaf
draws (distinct from the animated object preparation corrected above).
Custom normal generation exists on the Vulkan private vertex copy. Remaining
work is stream ownership across technique/pass pointers and the accumulated
lighting expressions, not adding another depth-state workaround without evidence.

## GPU submission order: remove water relocation

VulkanFrameRenderer previously moved waterEffect draws behind the last opaque
depth-writing draw in each clear-delimited scope. OpenGL consumes the engine's
submission sequence directly; deferred recording does not justify changing it.
Removed this extra relocation and record m_stockDraws in their original order,
including clears and capture points. This restores source ordering, but does not
prove that engine-side pass selection or transparent sorting is fully equivalent.
Native libraries and release APK build successfully. Water and godray runtime
behavior still need verification; do not claim this resolves godray occlusion.

## Animated object preparation: update result ownership

Compared GLRendPipeline.cpp::EF_ObjectChange skinning with
VulkanRenderer.cpp::PrepareStockSkinnedObject. OpenGL calls ProcessSkinning
after mfCheckUpdate without testing its boolean result, and accepts a missing
vertex buffer when computing the fence force-update condition. Vulkan previously
returned early in both cases. CREOcLeaf::mfCheckUpdate can return false for
missing indices after vertex preparation; that result must not suppress skinning.
Removed these two Vulkan gates while retaining object/container validation.
Native libraries and release APK build successfully. Visual lighting parity
is not established by this source correction; material passes remain open.

Reference: source XRenderOGL, not the installed game's shader binaries.
Water visibility was confirmed by the user; full rendering parity remains open.

## Frame entry and list execution

Reviewed `CGLRenderer::BeginFrame` against Vulkan `BeginFrame`: shader reload,
polygon mode, texture filtering, frame counters, temporary mesh pool recycling,
real time and default state reset are present. This is a source review, not a
visual verification of default state or frame attachment contents.

Reviewed `CGLRenderer::EF_RenderPipeLine` against Vulkan `EF_EndEf3D`:
PREPROCESS, STENCIL, GENERAL, UNSORTED, DISTSORT, LAST order is present;
LAST is restricted to the outer recursion level. Sorting and preparation calls
are present. Recursive preprocess, batching and final GPU command ordering
still require comparison; list order alone is insufficient.

## Texture stages: concrete correction

OpenGL `CGLTexMan::BindNULL` disables unused texture targets. Disabled fixed
texture units do not change the previous stage's color or alpha.
Vulkan's four/eight texture fragment calculated stage 2 directly into `color`
before checking `hasThirdTexture`; its false branch therefore retained the
incorrectly modified result. Changed it to calculate `stage2Color` separately
and retain the incoming `color` when the stage is absent. This matters when
stage 3 or a higher stage is active with a gap at stage 2.

Checked adjacent stages: optional stage 1 retains `previous`, optional stage 3
retains `color`, stages 4 through 7 update only inside their enabled conditions.
The three-texture fragment is selected only with an active third texture.

Runtime appearance has not yet been verified. Full shader/material parity is
not claimed by this correction.

## List suppression lifetime

`EF_RenderPipeLine` checks `RBPF_IGNORERENDERING` once after PREPROCESS.
If set at that boundary it clears the flag and omits the remaining list block.
If clear, it enters that block; a later flag change does not suppress entry to
every subsequent list. Sorted `EF_PipeLine` calls retain their own post-preprocess
checks, while unsorted and distance-sorted calls have no such check.

Vulkan previously checked the flag at every outer list entry and cleared it
unconditionally at the end. Changed the outer gate to run at the STENCIL
boundary (even if STENCIL is empty), clear and break only when suppression was
set there, and leave subsequent flag changes to the per-list source checks.
Removed unconditional final clearing. This preserves OpenGL's flag lifetime
through later preprocess operations and nested rendering.

Compared `EF_PreRender` Stage 1 CPU bookkeeping with `PrepareStockFrameState`:
render/frame-object counters, flags, camera information and sun direction are
present. Stage 2 clear and portal depth ranges are applied by the list walker.
Global programmable vertex parameters are still not a complete Cg translation.

## EF_Start object and material inputs

Compared the shader batch initialization with the Vulkan per-item boundary.
Added the missing `m_MergedObjects.SetUse(0)` alongside the existing resets of
`m_MergedREs` and `m_MergedObjs`. Common `CREOcLeaf::mfCheckUpdate` traverses
`m_MergedObjects` for `SHPF_LMTC`; a direct Vulkan item must not inherit another
batch's object list.

Changed the initial `m_DynLMask` source from the queued render item's mask to
the current object's `m_DynLMMask`, matching `EF_Start` before light-list
construction and hardware technique selection. The later `EF_Flush` mask
filtering (`r_hwlights`, `r_showlight`) remains to be transferred at its own
boundary. Batch merging and complete flush lifetime are still open.

## EF_Flush dynamic-light mask (subsequent implementation)

Added `ApplyStockFlushLightMask` for both hardware and fixed material dispatch.
It reloads the live object's mask, clears it when `r_hwlights` is disabled and
removes named lights that fail the `r_showlight` substring filter. The hardware
call occurs after technique selection, matching the EF_Start -> EF_Flush order.
As in OpenGL, this step does not rebuild `m_pActiveDLights` or reselect the
technique; those are different state with an earlier lifetime.

Native renderer and release APK rebuilt successfully. Runtime verification
of these options and the remaining light-pass dispatch is still open.

## EF_Flush material clip-plane ownership

Extended the flush preparation (now named `PrepareStockFlushState`) with the
missing `EF3_CLIPPLANE` handling. WATER_FRONT installs `(0,0,1,-waterLevel)`;
WATER_BACK installs `(0,0,-1,waterLevel)`. Installation requires no active
clip-plane and no material-owned plane flag, matching OpenGL. Existing capture
planes are therefore retained. A following shader without `EF3_CLIPPLANE`
disables only the material-owned plane and clears `RBPF_SETCLIPPLANE`.

Both hardware and fixed dispatch use the same preparation before their passes.
The source plane setter installs world-space planes using the camera matrix;
Vulkan already carries world-space clip position and the plane in its draw
uniforms. This change supplies the missing material ownership transition.
Renderer and APK build succeeded; runtime clipping and reflection parity remain
unverified.

## State-shader packed color

Compared `EF_SetStateShaderState` color installation with Vulkan DrawBuffer.
OpenGL initializes `Col.dcolor` to zero, writes fixed RGB (all four bytes) and/or
fixed alpha, exchanges byte 0 with byte 3, and installs the entire result.
Vulkan previously enabled only RGB for the RGB-only case and only alpha for
the alpha-only case, leaking channels from the preceding pass generator.
Replaced that with the same zero-initialized assembly and all-channel mask.
The legacy byte exchange is preserved, including its unusual alpha-only result.

Release build succeeded. Remaining generator work: per-flush RGBGEN/ALPHAGEN
guards and the lifetime of generated values across multiple material passes;
this packed-color correction does not prove those semantics complete.

## Global RGB generator alpha

`EF_Eval_RGBAGen` initializes its local packed color to `0xffffffff`.
Successful Object/RE/World/Wave/Noise RGB generation writes RGB and installs
the entire packed color, retaining alpha 255 unless a subsequent alpha
generator changes it. Vulkan previously left the vertex alpha active for these
RGB-only generators. Enabled the alpha override whenever all three global RGB
channels were generated; generators supplying explicit alpha retain it.

Source review also establishes that RGBGEN is not a universal skip condition:
Fixed/Style/Comps run unconditionally, Object/Wave/Noise can redirect to vertex
stream evaluation, while RE/World/Identity can skip. A blanket flag guard would
be incorrect. Full multi-pass stream and global-color lifetime remains open.

## Alpha-only global color installation

The final `bSetCol` branch of OpenGL `EF_Eval_RGBAGen` installs the complete
local UCol for successful alpha-only generators too. Its untouched RGB starts
white. Vulkan previously overrode only alpha and retained mesh RGB. Changed
the final evaluator mask to all four channels whenever any global channel was
generated. NoFill/client-only paths still retain the vertex stream. This fixes
the local packed-color installation; per-flush flags and stream-writing
branches remain an explicit unresolved part of the same pipeline stage.

## State-shader stencil clear boundary

`EF_SetStateShaderState` clears stencil for each flush whose state requests it.
Vulkan previously cleared only when the state-shader pointer changed, missing
new object/material/fog batches with the same state shader. Expanded its clear
key to object, shader, resources, fog and state shader. Adjacent items sharing
that key retain one clear rather than clearing between pieces of one material.

Full object merging is still absent and may alter actual flush boundaries;
this corrects the missing unmerged batch inputs, not all batching semantics.

## Stencil clear and overdraw ownership

Reviewed `QueueStockClearStencil` and the scene command writer: clear records
retain the current scissor and are executed at their ordered queue position.
The clear value is stencil zero. Vulkan lacked the source state-shader rule
that disables `r_measureoverdraw` when `m_bClearStencil` is requested. Added
that transition and the same one-time diagnostic (the option is then zero).
This does not implement the absent overdraw measurement/display passes or
prove masked clear parity.

## Detail overlay transformed attenuation plane

Compared the detail fog coordinates against `SParamComp_FogMatrix::mfGet4f`.
The shared source uses `TransformPlane2(objectMatrix, plane)` when the cached
object flags have FOB_TRANS_MASK. Vulkan's manual multiplication used matrix
columns, while that helper uses matrix rows. Replaced the manual multiplication
with the same helper and cached object flag test. The +0.5 S offset and constant
T=0.49 are retained. This changes attenuation on translated/rotated detail
models; release compilation succeeded, visual verification remains open.

## Detail overlay texture-target cleanup

OpenGL finishes `EF_DrawDetailOverlayPasses` with `CGLTexMan::BindNULL(0)`.
After a submitted eligible detail pass, Vulkan now clears all stage binding
IDs, sets the active-stage count to zero and clears texture matrix/texgen
modifier flags. Queued draws retain their captured samplers. Combiner and LOD
cache values remain available as in BindNULL. The next material must bind its
textures explicitly. Dedicated fog overlay passes still require transfer.

## Shared state transitions before auxiliary passes

Compared CRenderer::EF_SetState in GLRendPipeline.cpp with the NULL implementation
used by Vulkan. Added the missing pre-change SHOWLINES depth-test override,
WASDEPTHWRITE marking (including repeated states), and render-element LastVP
assignment. Stencil protection also includes RBPF_MEASUREOVERDRAW as in OpenGL.

Detail and ordinary fog now call the shared state resolver instead of duplicating
only selected protected-state masks. The effective main draw state becomes the
cache inherited by these subsequent passes. Protected alpha-test thresholds are
carried into both auxiliary draws. Fog reinstalls the object scissor after detail
or light passes, matching the general-pass object-scissor operation.

Native libraries and release APK compile successfully. Runtime output is not
verified. Caustics vertex/fragment formulas and sequence were reviewed against
CGVProgCaust/CGRCCaust and Templates.csl; their Vulkan pass remains unimplemented.
Full batching, special-pass auxiliary dispatch and general Cg execution remain
open. These changes do not establish complete OpenGL parity.

## Object inverse matrices and clip-plane conversion

The Vulkan backend links CCObject::GetInvMatrix from XRenderNULL. Inspection
found it returned identity unconditionally, despite Vulkan's stock lighting,
beam deformation, camera/specular parameters and shared shader components all
using it to convert world positions/directions into object space. Replaced that
stub with the OpenGL implementation: identity ID zero, cached positive IDs,
allocation for negative IDs and the same rotate/scale/translate branches.
Common EF_StartEf reserves matrix slot zero; CCObject initialization and light
object updates invalidate the inverse IDs as in the shared OpenGL engine path.

Also corrected DrawBuffer's world clip-plane conversion. Legacy Matrix44 uses
row vectors/last-row translation; object coefficients require each legacy row
dotted with the world plane (the TransformPlane2 formula). The old column-based
code omitted object translation from the plane's distance. The transformation
now uses cached m_RP.m_ObjFlags, matching shared shader-component semantics.

Native libraries and release APK build successfully. These are source-proven
mathematical/state differences, not proof of visual recovery. Runtime effects
on transformed models, reflections, fog and lighting still require verification.

## Complete legacy object-matrix cache interface

Continued the preparation-stage audit after GetInvMatrix. NULL pipeline init
omitted OpenGL's m_ObjMatrices.reinit(32); added the same initialization. Added
the absent CCObject::GetVPMatrix implementation directly from OpenGL: ID-zero
camera projection, positive cached IDs valid for the current TransformFrame,
and recomputation of camera-projection times object matrix otherwise.

Current Vulkan stock draw submission computes its MVP independently, so this
GetVPMatrix addition alone does not change those draw positions. It supplies the
source object/shader API required by the ongoing program transfer. Native and
APK builds succeed. Full program execution and runtime parity remain incomplete.

## Fixed-function material-light flags

OpenGL applies the shader's m_LMFlags through SLightMaterial::mfApply before
evaluating fixed-function passes. Vulkan previously supplied zero flags whenever
the hardware-pass type was eSHP_MAX. Fixed-function draws now use shader flags;
hardware draws retain their per-pass flags. Existing ambient/diffuse/specular
calculations consequently receive the fixed material flags as well.

The fixed-function dynamic-light path now honors LMF_IGNORELIGHTS. As in
EF_SetLights, it excludes projectors for LMF_IGNOREPROJLIGHTS and excludes baked
DLF_LM lights with LMF_NOSPECULAR when the surface already has a lightmap.

Native library and release APK build succeeded. Runtime appearance was not
verified. Active-light indexing, the timing of GLOBALRGB/GLOBALALPHA clearing,
constant-light fallback and complete material/pass ordering still require audit.

### Fixed-function active-light indexing

EF_SetLights iterates the first eight entries of the compact m_pActiveDLights
list prepared by EF_BuildLightsList. Vulkan instead limited the original scene
array to eight entries before testing its mask. Selected lights at scene indices
eight or greater were consequently lost even when they occupied a valid GL slot.
Fixed-function submission now reads the same compact active list and applies
LMF_LIGHT_MASK/LMF_LIGHT_SHIFT against its compact index. Programmable paths keep
their existing dispatch pending separate review. Release APK builds successfully;
runtime verification and GLOBAL color-state timing remain pending.

### Fixed-function ambient and diffuse formulas

Fixed-function material submission now calls the same EF_GetCurrentAmbient and
EF_GetCurrentDiffuse helpers as OpenGL's EF_LightMaterial. The programmable
AmbLightColor reconstruction used previously additionally multiplied ambient by
material diffuse and could darken fixed-function surfaces. Shared helpers also
preserve the precise ONLYMATERIALAMBIENT/IGNOREMATERIALAMBIENT interaction and
upper channel clamps for divided diffuse. Programmable calculations remain
separate. Native and APK builds succeed; visual parity is not yet verified.

### Constant-light color state

Fixed materials requiring normals now use EF_ConstantLightMaterial before the
first pass when no translated light slot exists, installing the exact packed
NeedGlobalColor and GLOBALRGB/GLOBALALPHA for stage-zero argument substitution.
The ordinary ambient vertex-light multiplier is disabled for this path; opacity
is carried by the constant instead of being multiplied again by fragment fade.
When lights are populated, first-pass preparation clears these global flags as
EF_LightMaterial does. Later passes retain generated color state. Fog objects
are excluded. Release builds succeed; no-color generator parity, exact batching
boundaries, active-light acceptance and runtime verification remain incomplete.

### Fixed-function lighting eligibility

Moved the EF_LightMaterial eligibility checks ahead of Vulkan light submission:
fixed lighting requires a material, EF_NEEDNORMALS and absence of FOB_FOGPASS.
Previously fog/unlit objects could still receive dynamic light draws, and the
ambient fallback could illuminate objects without a light material. Constant
fallback now uses the same eligibility; FOB_LIGHTPASS alone does not suppress
it because OpenGL's helper has no such guard. Native and release APK builds
succeed. Exact inherited GL-light state and full runtime comparison remain open.

### Fixed-function specular suppression

EF_LightMaterial clears material specular and shininess with LMF_NOSPECULAR;
EF_SetLights also clears the light specular color. Vulkan now clears the fixed
lighting payload's specular RGB and exponent in that case, instead of only
applying this flag to separate programmable specular draws. Vertex fixed
lighting also gates the half-vector specular term on a positive normal/light
dot product. These changes do not establish complete multi-light accumulation
or runtime parity; those remain under review.

### Light-material dispatch flags

Reviewed Common/LightMaterial.cpp::mfApply before proceeding to texture binding.
LMF_DISABLE returns without EF_LightMaterial; LMF_BUMPMATERIAL assigns the light
material to the separate bump executor instead. Vulkan fixed lighting eligibility
now excludes both cases, so they cannot install ordinary constant color or emit
ordinary fixed-light contributions. This corrects the newly integrated constant
fallback as well. Complete bump execution remains pending. Release APK builds.

### Material combiner cache installation

GLTextures.cpp::mfSetTexture installs CO/AO/CA/AA together when color operation
is not NOSET. Vulkan reconstructed the current draw but left its legacy stage
cache unchanged; global stage-zero argument substitution could then read stale
selectors during a later NOSET pass. Material binding now persists all four
fields for every declared stage, preserving explicit render-element overrides.
Effective GLOBAL argument replacement remains temporary, as in OpenGL. Release
APK builds successfully; inherited state across special passes remains to audit.

### NOSET restoration after global argument replacement

VulkanFrameRenderer retains effective submitted selectors, including temporary
GLOBALRGB/GLOBALALPHA substitutions. Passing argument sentinel 255 for NOSET
therefore retained that substitution even after the global flag was cleared.
NOSET now submits the cached material selectors explicitly, then applies only
the current draw's global flags. Operation sentinels still retain the effective
GL operation/scale state. Applies to all eight declared stages. Release builds
succeed; full visual and auxiliary-pass state verification remains pending.

### Fixed lighting replaces primary RGB

GL_Renderer.cpp disables GL_COLOR_MATERIAL; GLSystem.cpp enables separate
specular. Fixed lighting computes primary RGB from the applied light material,
independently of client vertex colors. Vulkan color-bearing lit vertex programs
previously multiplied that RGB by inColor, introducing extra vertex darkening.
They now replace primary RGB only when fixed vertex lighting is enabled.
The shared fixed-light helper clamps primary and separate specular RGB to [0,1]
before interpolation. Material alpha and complete multi-light summation still
require review; runtime parity has not been established.

### Lit primary alpha and generator ownership

Color-bearing fixed lit vertex programs now set primary alpha to one, matching
EF_GetCurrentDiffuse's material alpha with GL_COLOR_MATERIAL disabled. The draw
also disables primaryColorMask replacement when fixed material lighting has
active translated lights: a pass generator's glColor must not replace the lit
primary in that state. Packed texture-environment color still follows generator
and GLOBAL selector rules. Constant-light fallback remains separate. Multi-light
accumulation, formats without normals and runtime verification remain pending.

### Fixed-function normal magnitude

GL_Renderer.cpp explicitly disables GL_NORMALIZE. The fixed vertex-light helper
no longer normalizes input normals unconditionally; it preserves magnitude for
diffuse and half-vector specular. This removes one source discrepancy for rigid
transforms but is not complete normal parity: inverse-transpose eye-space normal
conversion and eye-space light distances for scaled objects remain unimplemented
in the stock object-space light payload. Runtime verification remains pending.

### Secondary normal stream for fixed light passes

Leaf normal-stream materialization previously included fixed ambient fallback
but excluded fixed-function draws with active dynamic lights. Those draws could
lose the leaf's smooth normals when their main buffer held position/UV only.
Eligibility now includes every fixed material-lighting draw, retaining the
existing leaf secondary-stream availability and vertex-format checks. Release
APK builds successfully; arbitrary missing-normal formats and scaled normal
transform parity remain pending.

### Fixed point-light attenuation branch order

EF_SetLights uses RE bounds only for its first accepted nondirectional light:
after bCalcDist is set, its else branch substitutes center zero/radius 1000.
Vulkan now preserves that exact branch order for fixed lights, while retaining
existing programmable attenuation behavior. Positive-radius light calculations
retain the source min/max clamp and constant/linear equations. Zero-radius
degenerate behavior remains to review. Release builds succeed.

Quest 3 installation succeeded before this attenuation edit, with the preceding
normal-stream, generator, material, lighting and combiner changes. The user was
asked to launch Training; the application was closed and was not auto-launched.
Visual evidence remains pending. The latest built APK includes attenuation too.

### Lit stage-zero source restoration

After enabling fixed lights, EF_LightMaterial assigns DEF_TEXARG0 to stage-zero
CA and AA. Vulkan now restores the same legacy selectors before first-pass
texture binding, unless the render element already supplied a later override.
A NOSET material inherits Texture/Primary, while an ordinary material bind
still replaces both selectors. Constant fallback retains its separate global
selector path. Release APK builds successfully; this edit is not installed yet.

### Separate specular before fog

All stock fragment variants now clamp the primary-texture plus separate-specular
RGB sum before scene fog when fixed vertex lighting is active. Previously fog
mixed an over-range specular sum before the render target clamped it, allowing
bright fixed highlights to alter fog visibility. Programmable light modes keep
their existing range behavior. Shader/native/APK build succeeded; runtime parity
remains unverified. This change has not yet been installed on Quest.

### Fog and effective viewport depth range

Nearest objects use viewport depth 0..0.1, matching glDepthRange. All stock fog
fragment variants now undo the effective viewport range before reconstructing
eye distance. The prior formula treated window depth as full-range normalized
depth and yielded incorrect distance for nearest/custom-range draws. The range
is stored in unused linearControls[0..1].w padding for each draw, including UBO
reuploads after buffer growth. Ordinary 0..1 behavior is retained. Projection
near/far and full eye-space fog parity still require review; runtime is pending.

### Fog projection near/far

Fog constants now use draw.nearPlane/draw.farPlane, the same pair passed to
BuildStockEyeTransform for its projection. Previously constants were always
0.05/1000 even when the camera clip range differed, including nearest objects
whose projection uses 0.01/40. Combined with viewport depth normalization this
corrects the depth-to-eye-distance formula's inputs. Release APK builds succeed;
viewport origin/size and full radial-distance parity still require audit.

### Radial fog viewport coordinates

Per-eye fog ray reconstruction now normalizes fragment coordinates against the
draw's viewport origin and extent, rather than the complete swapchain extent.
Unused linearControls[2..3].w carry origin; existing UV-row extent fields carry
draw width/height. Both initial uploads and buffer-growth reuploads use the same
draw state. Full-swapchain viewports retain the same ray coordinates. Actual
projection FOV selection and runtime visual parity remain under review.

### Nearest-object fog FOV

BuildOpenXrEyeMvp scales nearest-object eye FOV angles by 0.6666 before taking
tangents. Radial fog now applies the identical scale for those draws, including
buffer-growth reuploads, instead of reconstructing rays from ordinary world
FOV. Together with draw near/far, depth range and viewport this aligns the fog
ray inputs with current stock eye projection. Release APK builds succeed;
full OpenGL fog-mode/state lifecycle and runtime parity remain incomplete.

### Shared fog-state synchronization

Vulkan SetFog now updates m_FS density/start/end/requested mode and color, while
retaining the separate effective mode for unsupported public-mode requests as
OpenGL does. SetFog mirrors OpenGL's heat-vision black color; SetFogColor retains
its separate direct-color semantics. EnableFog synchronizes m_FS.m_bEnable.
Previously only private Vulkan values changed, leaving shared EF_PushFog/PopFog
and effects reading m_FS with stale state. Release APK builds succeed; runtime
and complete push/pop dispatch parity remain under review.

### Exponential fog range independence

SetStockFog no longer disables EXP/EXP2 when the linear start/end range is
equal, reversed or unused. Only linear mode uses that range for enable-time
validation; exponential modes require finite density. Shared EF_PopFog calls
the synchronized SetFog/EnableFog dispatch reviewed above. Reversed/equal linear
range behavior still differs and requires separate formula handling. Native
libraries and release APK build successfully; runtime comparison remains pending.

### Reversed linear fog ranges

Linear fog now retains the sign of end-start, including the water fragment
variant, and SetStockFog accepts finite reversed ranges. The prior positive
denominator clamp and end>start gate incorrectly prevented reversed linear
ramps. Equal start/end remains excluded pending evidence for degenerate GL
behavior. Full runtime parity remains unverified.

### Water fog viewport/depth normalization

The dedicated vr_scene_water fragment bypasses ordinary applySceneFog. Its
fogDistance now subtracts viewport origin, and its eye-distance reconstruction
undoes the draw's viewport depth range using the same control padding. Draw
near/far and nearest FOV already arrive through common UBO upload. This closes
the separate water path's missed viewport/depth inputs; water program selection,
reflection/refraction capture and runtime parity remain incomplete.

### Shared water alpha-test executor

Extracted stockAlphaTestPasses into scene_alpha_test.glsl, included by both
ordinary stock lighting fragments and dedicated water fragments. Water now uses
alpha-test specialization ID zero and evaluates pass GREATER/LESS/GEQUAL modes,
with material AlphaRef retaining GL_GEQUAL precedence. Previously it applied
only material threshold. Added the shared include to native shader dependencies
so future changes rebuild embedded programs. Runtime verification is pending.

### Special-pass alpha-test integration

Terrain-layer now evaluates the final output alpha against the shared material
and render-state alpha-test function; it previously ignored alpha testing.
Caustics now uses the same helper instead of its duplicated inverse-comparison
implementation. Existing template formulas, output alpha and blend state are
retained. Shared shader dependency tracking includes both embedded programs.
Runtime parity and complete terrain-layer program coverage remain incomplete.

### Alpha-test CPU/GPU dispatch audit

Confirmed QueueStockClientIndexedDraw sets supportsAlphaTest for dedicated
water, terrain-layer and caustics pipelines after selecting their modules.
VulkanPipelineFactory registers constant ID 0 from decoded render-state alpha
mode; new shader declarations therefore consume that value. This proves source
dispatch connectivity, not GPU/visual correctness. Water output does not consume
materialParams.x globalOpacity; its source program parameter ownership must be
reviewed before adding an unconditional output fade. No parity claim is made.

### Water output-opacity source audit

Saved original uncompiled CGRCIndoorWater_final, CGRCIndoorWaterSpec,
CGRCLowMedWater and CGVProgLowMedWater from Shaders.pak in research/opengl-water.
Indoor final explicitly outputs alpha one. LowMed outputs saturated vertex alpha
times WaterColor alpha plus 0.5; Spec outputs vertex alpha times WaterColor alpha.
Their stock fragment formulas match these expressions; adding materialParams.x
as unconditional fragment opacity would diverge. LowMed vertex alpha uses input
alpha*1.8 minus reciprocal clip-W squared, matching the current stock branch.
Opacity parity is still incomplete until CPU generation of that input color and
the template WaterColor parameter are traced. No shader change is justified by
this output-formula audit; remaining water programs and captures are unfinished.

### LowMed template opacity ownership

Saved authoritative terrainWater.csl. LOWSPEC_WATER_SHADER supplies WaterColor
RGB constants 0.5/0.7/1.0 and alpha User LowSpecOpacity, whose default is 1.0.
The Vulkan waterParameter resolver evaluates the actual shared CG parameter
components for WaterColor; the fragment multiplies that alpha in the source
formula. The template explicitly declares only the vertex-position array.
IN.Color ownership therefore requires reviewing the OpenGL array/current-color
commit path, not assuming the Vulkan general primaryColorMask is appropriate.
The water vertex program currently chooses inColor when its layout provides it,
otherwise white; correspondence of that choice remains unproven. Next audit is
the hardware array commit for water and formats lacking color attributes.

### Hardware water color-array selection evidence

GL_Renderer.h::EF_CommitStreams enables/disables color arrays from
SArrayPointer::m_CurEnabled and m_CurEnabledPass. GLShaders.cpp::
SArrayPointer_Color::mfSet sets those bits only when the declared pointer runs.
This is independent of whether a general VB layout contains a color field.
The stock WATER_COLOR layout branch uses inColor whenever the buffer format
provides color, so it does not by itself reproduce that declared-array selection.
When no color array is enabled OpenGL supplies current color; NeedGlobalColor
is committed as texture-environment color, not automatically glColor, in
EF_CommitTexStageState. Thus blindly applying primaryColorMask in vr_water.vert
is not a source-faithful fix. Required next work is an explicit active-pointer
selection/current-color contract across technique/pass commits and draw calls.

### Water declared color-array selection

Water submission now inspects destination Color pointers in the selected
technique and hardware pass. vr_water uses inColor only when that array is
declared, rather than whenever its buffer layout contains color. The ordinary
pass-generated uniform replacement mask is repurposed explicitly for water
input selection. Undeclared array currently supplies the initial GL white color;
cross-draw current-color tracking, immediate color updates, inherited pointer
state and non-Color sources targeting Color remain incomplete. This is a partial
array-selection implementation, not complete water/current-color parity. Native
libraries and release APK build successfully; runtime verification is pending.

### Terrain ambient and detail-only passes

Compared Vulkan stock-terrain fragment handling against the extracted OpenGL
`terrain.csl`, `CGVProgTerrain*` and `CGRCTerrain*` sources. The Vulkan general
dynamic-lighting evaluator was also applied to `CGRCTerrain` programs even
though their fragment shaders use the `Ambient` Cg parameter and per-vertex
terrain weights. Excluded base, layered and detail-only terrain programs from
that generic evaluator. Added the OpenGL `Ambient` factor to the Vulkan base
terrain and 1–3 detail-layer formulas; 4Layers_Only remains unlit as in Cg.
Also added dedicated formulas for the one-texture low-resolution `CGRCTerrain_NoCol`
and two-texture `CGRCTerrain_Far` paths, including their shader Ambient factor.
Added the two-texture default detail-texture formula and terrain volumetric-fog
blend. The volumetric-fog pass receives its shader-specific `FogColor` Cg
parameter through an unused terrain transform slot; Vulkan combines fog and
terrain albedo using the two sampled fog alpha channels and the original vertex
color. All terrain variants with an explicit Cg formula bypass generic dynamic
lighting. Runtime verification remains pending.

OpenGL declares separate `*_1Layers_Only` through `*_4Layers_Only` passes with
`DST_COLOR/SRC_COLOR`, `DepthFunc=Equal`, and per-detail vertex weights. Vulkan
now recognizes each program, preserves its terrain texgen mode, executes the
matching 1–4-texture weighted detail product, and leaves blending/depth state
with the translated render state. Runtime Quest verification is still pending.
