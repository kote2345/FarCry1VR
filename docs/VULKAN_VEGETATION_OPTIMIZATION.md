# Vegetation optimization — Quest 3, 2026-10-02

## Evidence

The paired whole-scene GPU timestamps in `research/vulkan_gpu_ab.csv`, pair 4,
have identical queued draw count and inventory hash. Omitting 385 blended
CGRCPlants draws changes scene time from 20.909 to 11.723 ms. This measures
9.185 ms of net vegetation cost, not the expected saving of an optimization.
It includes rasterization, shading, blending and their interactions.

## OpenGL reference

Compared the original Shaders.pak sources:

- `CGVProgSimple_Plant`: vertex RGB * Ambient.rgb; output alpha = Ambient.w.
- `CGVProgSimple_Plant_Bended`: the same color calculation after PosBending.
- `CGRCPlants`: one albedo sample * vertex output; LDR HDREncode scales RGB x2.
- `AmbPass_Plants_VP.csi`: Ambient uses WorldObjColor and Opacity parameters.
- `SParamComp_Opacity::mfGet`: current opacity * object opacity.

The previous generic Vulkan fragment path evaluated material lighting per
fragment and carried unrelated terrain, projector and specular branches.

## Implemented

1. Dedicated `vr_plants.vert` / `vr_plants.frag` for those two exact vertex
   programs with CGRCPlants. Evaluate the original Ambient/Bend parameters,
   multiply color in the vertex shader, preserve the bending formula and RGB x2.
   Opacity is evaluated once through Ambient.w, including object fade.
2. One texture lookup and early alpha rejection before RGB/fog processing.
   Texture derivatives precede every discard. Existing alpha thresholds,
   sampler LOD bias, clip plane, radial eye fog and blend/depth states remain.
3. Adjacent identical meshes merge into instanced draws, up to 256 instances.
   Each instance reads its own two eye matrices, ambient/opacity and bending
   from the existing frame storage buffer. The storage stride is now 288 bytes
   on CPU and in all shaders. Reflection matrices retain their original fields.
4. Preserve submission order. No material sorting across transparent objects.
   Verify mesh bytes (including UVs and vertex colors), index bytes, pipeline,
   texture/sampler, fog, alpha threshold, viewport, scissor and clip state.
   Query-instrumented draws and single-view rendering do not batch.
5. Shader dependencies include the shared stereo layout for multitexture
   variants, ensuring incremental builds update all users of the storage stride.

Density, visibility distances, LODs, textures, resolution and MSAA are unchanged.
Bump, sprite, texgen and other Cg variants retain their existing paths. No
replacement of alpha blending with dithering or alpha-to-coverage is applied.

## Measurement

`/sdcard/FarCry/vulkan_plants_batches.csv` records frame, original plant draws,
submitted calls and maximum instance count every 120 frames, bounded to 15360
frames. The material census adds `vp` and `fastPlants` to identify program
coverage. There are no new GPU queries or waits in the vegetation path.

Build success does not establish FPS improvement or visual correctness. Inspect
foliage in both eyes at the previous outdoor scene, then compare whole-scene GPU
timestamps and batch counts. Per-draw pipeline statistics on this driver are
not valid cost attribution (see the hotspot report).

## Research and applicability

- Meta's Quest porting case study recommends combining draws and instancing:
  https://developers.meta.com/vr/blog/showdown-on-quest-part-2-how-we-optimized-the-pc-vr-demo-for-meta-quest-2/
- Meta describes Quest transparency/overdraw costs:
  https://developers.meta.com/vr/documentation/web/webxr-perf-bp/
- Android recommends calculating appropriate work in the vertex stage and
  measuring shader/transparency costs:
  https://developer.android.com/games/optimize/materials
- Meta multiview reduces stereo submission overhead; it is already enabled:
  https://developers.meta.com/vr/documentation/unreal/unreal-multi-view/
- NVIDIA's SpeedTree treatment discusses vegetation alpha coverage and its
  interaction with LOD; these change coverage and require separate VR visual
  evaluation rather than an automatic substitution:
  https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering
- Meta symmetric projection is a further multiview geometry optimization, with
  swapchain/sub-image/foveation implications. It is not part of this change:
  https://developers.meta.com/vr/documentation/native/android/os-symmetric-projection/

Instancing primarily reduces submission/state overhead. It does not remove
overlapping transparent fragments in either eye; the dedicated shader addresses
that separate GPU cost while keeping the existing coverage and blend result.

## First device run and dispatch correction

The first installed implementation did not activate in the outdoor scene:
`research/vulkan_plants_batches_latest.csv` has zero fast-path draws throughout
frames 2280–4440, and the material census reports `fastPlants=0` with the expected
CGVProgSimple_Plant_Bended program. Whole-scene GPU timings remain approximately
20–21 ms; no speedup is established by this run.

Corrected dispatch to depend on the actual Cg program and required vertex
attributes. Other bound TMUs/fixed-function texgen do not participate in
CGRCPlants and no longer veto its dedicated pipeline. Default Cg parameters
are evaluated through the existing SParamComp_WorldColor, SParamComp_Opacity and
SParamComp_ObjWave implementations; explicit technique parameters still override
them. The previous code required an explicit technique Ambient parameter and
otherwise left the dedicated program disabled. Rebuilt and installed the
correction; activation, batching and GPU cost still require a new device run.
