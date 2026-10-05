# OpenGL/Vulkan pipeline audit, 2026-09-30

This is a source audit and an inventory of the latest Training launch, not a
claim of visual parity. OpenGL is the reference. The scene still needs a headset
check after the fixes described below. Earlier entries in VULKAN_OPENGL_PARITY.md
include historical states and must not be read as current completeness claims.

## Confirmed water failure and changes

The latest `vulkan_water_record_probe.txt` contains real indexed WaterVolume
commands in both eyes, with 5,376 indices, but `texture=0,0`. Queue acceptance
alone was insufficient evidence that the original material reached the GPU.

`CTexMan::LoadFromImage` changes `eIF_DDS_DSDT` to `eIF_DDS_RGBA8`, retaining
`FIM_DSDT`. `GenerateNormalMap` preserves already generated DSDT bytes.
Consequently, `UploadImage` passes `eTF_8888` with `eTT_DSDTBump`.
OpenGL's GLTextures.cpp chooses DSDT upload semantics from the texture type.
Vulkan's decoder rejected this combination, accepting only `eTF_DSDT_MAG`.
The animated caustic normal map was therefore absent from the Vulkan image map.
`QueueStockIndexedDraw` cleared the primary ID and, because the secondary stage
depends on the primary, also cleared the reference texture ID. The water shader
sampled fallback descriptors rather than its two declared samplers.

Changes made:

- Accept the shared loader's 8888/RGBA format labels for signed DSDT data, for
  both the base level and mip-chain byte strides; do not apply BGRA swizzling.
- Match GL's signed-byte /127.0 conversion in the shader.
- Exempt dedicated water shaders from fixed-function interpolation validation.
  `TexColorOp=NoSet` keeps earlier combiner state in OpenGL, but CG water does not
  execute that combiner. A cached interpolation state must not reject water.
- Bind CGRCWater's actual Fresnel and base-water samplers in slots 2 and 3;
  restore its `fresnel.b + (base.b - 0.5)` opacity expression.
- Capture queue diagnostics only after successful queuing, so level precache
  draws cannot exhaust the 32-entry bound before actual XR rendering starts.

Planar reflection remains missing: the renderer still sets reflection RGB
weight to zero and substitutes a base map for unavailable `$WaterMap`.
This is explicitly not the complete OpenGL sea appearance.

## Pipeline stages

| Stage | OpenGL reference | Vulkan implementation / remaining difference |
|---|---|---|
| Engine visibility | Shared Cry3DEngine portal/sector/object selection | Same engine code; Vulkan maintains separate visibility-query results. Sharing engine code does not establish parity of query feedback or recursive cameras. |
| List preparation | GLRendPipeline.cpp, EF_RenderPipeLine: preprocess/general/last shader sorting, distance and stencil sorting | EF_EndEf3D implements these list types and restores frame/object state. Ordering exists; offscreen preprocess operations are not equivalent. |
| Technique selection | Hardware feature flags, resource/object/light conditions, expanded shader macros | NULL_Shaders parses conditions and program names/masks. Vulkan advertises NV4X, VS, PS20, PS30, bump and depth maps, despite having only selected translations. A selected program name does not mean its program executes. |
| Program compilation | GLCGVProgram/GLCGPShader compile CG scripts, includes, generated variants and parameters; legacy NVParse paths also exist | No general CG/NVParse compiler/translator. Vulkan selects a finite set of embedded GLSL modules and reproduces selected stock formulas on CPU/GPU. Unknown programs can use generic material/fixed-function fallback. |
| Hardware pass parsing | Full Layer, Array, Matrix, deformation, sampler and program parameter grammar | NULL_Shaders deliberately enters `parametersOnly` for many CG declarations. Full layers are retained for decals and selected plants/terrain/water programs. Untranslated programs can lose their original sampler/state declarations. |
| Vertex/index submission | GL leaf/chunk ranges, transient meshes, pointer arrays, deform/texgen programs | Formats 1–16 have translated layouts, with non-world streams rejected. Indexed lists/strips/fans and triangulated quads exist. Some vertex programs are CPU substitutions; arbitrary program deformation is absent. |
| Camera/projection | Stock model/view/projection, recursive reflection/portal camera, GL clip depth | Vulkan uses stock modelview plus an XR eye projection and Vulkan depth range. Ordinary stereo draws exist. Recursive reflection/portal cameras are not represented by the same scene-target pipeline. |
| Texture creation | GLTextures: image formats, bump/DSDT types, cube and generated textures | DXT decoding, ordinary uncompressed images, selected signed formats and mip chains exist. DSDT format-label mismatch fixed in this change. Cube images use a face atlas in supported paths, not a general native cube sampler. 3D textures and arbitrary generated render textures remain incomplete. |
| Binding and residency | STexPic::Set/mfSetTexture uploads/streams/resolves special texture names | NULL implementations are no-ops; Vulkan separately resolves and mirrors IDs. Missing-image fallback can preserve geometry while dropping material stages. File-backed recovery originally covered primary texture only. Animated/special/secondary texture residency needs continued auditing. |
| Fixed-function materials | GL color/alpha combine, RGBA generation, texgen and texture matrices | Up to eight translated texture stages, selected generators/combines and material RGBA exist. They cannot automatically reproduce arbitrary CG programs. Programmable passes must not inherit fixed-function validation constraints. |
| Surface lighting | Ambient, light, multilights, diffuse/specular, lightmaps and shadows dispatched by hardware technique | Selected ambient/light template masks, baked/directional lightmaps, bump lighting, attenuation and projector cookies are translated. No proof of complete generated-variant parity. Environment/specular/gloss combinations and terrain-specific shadow/light programs need separate translations. |
| Blend/depth/stencil/cull | GL GS-state mapping and ordered multipass rendering | VulkanPipelineFactory decodes corresponding state and uses pipeline cache keys. Dedicated stencil/shadow-volume draws exist. Matching state does not establish matching fragment alpha/depth or offscreen pass ordering. |
| Fog | Ordinary fog plus programmable HDR/fog-volume paths | Ordinary/radial fog exists. Original volumetric fog and all program-specific fog/HDR composition are not fully implemented. |
| Transparent geometry | Distance lists, material alpha, original specialized shaders and multipass blending | Lists/blending/alpha test exist; specialized decals/plants/godray/water translations are finite. Environment/refraction materials can still select a shader whose original inputs are unavailable. |
| Special render elements | Dedicated GL rendering for each CRE type | Vulkan handles leaf/temp mesh/beam/poly/client poly/polyblend/particles/glare/flashbang/flare, with shared or custom dispatch. Remaining NULL render-element methods can return success without drawing. Screen/HDR processing has no equivalent complete implementation. |
| Frame output | GL framebuffer, generated targets, screen/HDR processing | Vulkan renders stereo scene targets and submits XR projection layers. Panel/HUD/overlay paths are separate. Successful XR output or HUD does not establish scene shader parity. |

## Offscreen preprocess operations

OpenGL `EF_Preprocess` in GLRendPipeline.cpp dispatches all the following. Vulkan
consumes the leading preprocess records and implements shadow-caster preparation,
but has no matching executor for the other operations listed here.

| OpenGL operation | Missing Vulkan work |
|---|---|
| SPRID_SCANCM | Generate/update reflected environment cube maps. |
| SPRID_SCANLCM | Generate/update environment lighting cube maps. |
| SPRID_SCANSCR | Environment-screen target capture and matching sampling. |
| SPRID_SCANTEXWATER | Reflected camera across water plane, scene render into `$WaterMap`, original update cadence and projective texture matrix. |
| SPRID_SCANTEX | Object reflection/refraction target generation and state restoration. |
| SPRID_RAINOVERLAY | Rain render texture production. |
| SPRID_REFRACTED | Original refracted-object marking and separate flushing. |
| SPRID_SCREENTEXMAP | Ordered refracted-object flush and screen-texture capture. Dedicated water scene snapshots cover only selected modes, not this general operation. |
| SPRID_SHADOWMAPGEN | Partial implementation: depth targets/caster submission/comparison exist; all GL shadow composition is not reproduced. |
| SPRID_CORONA | Specialized Vulkan flare/query handling exists, with differences in query cadence/coverage and stereo feedback. |
| SPRID_PORTAL | Recursive portal rendering and target/camera management. |

## Actually selected Training programs

Inventory source: `build/material_program_latest.txt`, pulled from the latest
Quest run. Records include precache rejected draws as well as accepted draws.
Neither a selected name nor `queued=1` establishes pixel correctness.

| Selected vertex / fragment program family | Current handling |
|---|---|
| CGVProgAmbientTempl / CGRCAmbientTempl | Explicit mask-driven ambient/alpha/lightmap translation; many materials share this family. Full generated variant comparison remains necessary. |
| CGVProgLightTempl / CGRCLightTempl | Explicit light template translation and light batching, attenuation, bump/projector paths. Environment/specular combinations remain partial. |
| CGVProgShadowTempl / CGRCShadowTempl | Generic translated shadow/depth receiver paths; no general execution of this CG program. |
| CGVProgSimple_Plant and _Bended / CGRCPlants | Explicit plant base-pass color/alpha handling. Bending is not established as equivalent for every variant. |
| CGVProgSimple_Plant_Bump / CGRCPlants_Bump | No dedicated program-name translation in Vulkan; generic material lighting is insufficient evidence of matching the stock shader. |
| CGVProgTerrain / CGRCTerrain | Explicit terrain base color and projected UV handling. |
| CGVProgTerrainLayerTempl / CGRCTerrainLayerTempl | Selected in this run, but no dedicated program-name translation. Generic stages do not establish original terrain layer semantics. |
| CGVProgTerrain_LowLod / CGRCTerrain_NoCol | Low-resolution terrain geometry/UV/color adaptations exist, but no dedicated fragment-program execution. |
| CGVProgTerrainShadow / CGRCTerrainShadow | Selected in this run; original terrain-specific shadow program is not translated by name. |
| CGVProgWater / CGRCWater | Dedicated vertex/fragment modules; DSDT upload and Fresnel/base binding repaired. `$WaterMap` reflection still missing. |
| CGVProgLowMedWater / CGRCLowMedWater | Dedicated modules; real commands recorded with missing textures before this fix. No planar reflection dependency in this stock low-spec program. |
| CGVProgWater_Beach_Refr / CGRCWater_Beach_Refr | Dedicated shore-wave UV/color/alpha translation. |
| CGVProgWater_Beach_Shift / no named fragment | No dedicated vertex-program selection; generic pass fallback does not reproduce the shift program automatically. |
| CGVProgTexGen_1Unit / no named fragment | Selected water-bottom texgen has a CPU projection adaptation. |
| CGVProgDecalGeom / no named fragment | Surface-decal fixed-combiner translation. |
| CGVProgMuzzleFlash / no named fragment | CPU view/normal angular fading and additive material pass for Training rays. |
| Empty program names in a hardware pass | Not proof of a fixed-function reference program. Parser limitations or legacy hardware declarations can also leave names empty. These need a declared fallback/unsupported classification. |

Additional stock water programs present in the reference scripts but not all
translated: CGRCWater_PS20, _CM_PS20, _Lake, _Refr, _Ripples, _Tank,
_DuckWeed, CGRCWaterReflectCMap, CGWaterVolumeReflCM, CGRCOceanWater,
their associated vertex programs and bump sun-glow passes. They must not be
reported as implemented merely because another water program is supported.

## Priorities after the texture fix

1. Confirm visible LowMed surface in Training. Device probes after DSDT repair
   confirm resident animated samplers and 1.5–2.2 million passing samples in
   several scene frames, but water was source draw zero with depth writes off.
   Deferred recording now moves early water behind opaque depth-writing draws
   within each depth/stencil-clear scope. Build and installation passed;
   visibility after this scheduling change still requires device confirmation.
2. Implement offscreen reflected water scene target and its projective matrix,
   preserving original material samplers and reflection coefficient.
3. Replace silent unknown-program fallback with a bounded supported/partial/
   unsupported inventory; finish the actually selected terrain/plant/shadow
   programs before claiming baseline material parity.
4. Audit remaining sampler types and generated textures, including environment
   reflection/lighting and screen/refraction targets.
5. Complete volumetric fog, postprocessing/HDR and remaining special CRE paths.

Building the source and installing an APK verifies packaging, not appearance.
