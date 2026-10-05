# Vulkan multiview migration

## Implemented

- Enable Vulkan 1.1 multiview on the device. Quest 3 reports multiview support
  and maxMultiviewViewCount = 6. Device initialization reports a clear error
  if multiview is unavailable; this version requires that feature.
- Create compatible clear/load stereo render passes with viewMask 3 and
  correlated view mask 3. Scene color and depth use two-layer images.
- Record the main scene once, in the first command buffer. All legacy material,
  generated texgen, terrain, caustic, water and character vertex variants select
  their projection through gl_ViewIndex and a per-draw stereo storage buffer.
  The first push-constant float carries the draw index only for stereo pipelines.
- Retain independent single-view pipelines for shadow maps and ocean reflections.
  Construct a complete material description even when the main pipeline is cached,
  so a later reflection variant can be created with valid shaders and state.
- Keep HUD and final output as separate passes using per-layer scene image views.
  This avoids changing the existing HUD transformations and minimap behavior.
- Snapshot refraction color for both eye layers before resuming the stereo scene.
  Water descriptors contain both separate eye images; fragment sampling chooses
  the eye with constant descriptor indices. Ordinary materials initialize both
  descriptor elements to the same image. Reflection projection follows each eye's
  actual cached reflection camera, including updates recorded this frame.
- Reset cached pipeline/index/descriptor bindings after the multiview water pass
  restart; restore viewport and scissor explicitly.
- Allocate two consecutive occlusion-query slots per multiview query and combine
  their results. Preserve nonblocking result reads during CPU/GPU frame overlap.
- Preserve CPU/GPU capture overlap, resource guards and fence-protected reuse.
  Retire grown stereo buffers with the frame and release array images once,
  with independently owned per-eye views and framebuffers.
- Add multiview and actual scene draw-call fields to the bounded timing CSV.
  Add shared stereo-shader dependencies to the native shader build.

## Status

Embedded shaders, native libraries and release APK compile successfully.
Runtime stereo correctness, water/HUD preservation and performance require
the next headset capture. No automated tests were added or run. Main scene
geometry is single pass; reflection, shadow, HUD and output passes remain separate.

## First headset capture

`research/vulkan_frame_timings_multiview.csv`, frames 4200-4920, reports
multiview = 1. Scene calls track submitted draws once: frame 4920 has 1477
queued entries and 1476 scene draw calls (one entry is not a geometry draw).
The first command buffer records the shared stereo scene; the second records
only its HUD/output. This confirms the active single-pass scene path.

Command recording is now about 5.0-5.1 ms versus 6.9-7.0 ms in the preceding
overlap capture. GPU scene time remains about 29.4-30.5 ms; there is no measured
GPU speedup in this scene. Total frame interval is roughly 35-38 ms, or
26-28 FPS estimated from phase timing. This is not an overlay FPS measurement.
The capture alone does not establish visual correctness of both eye images.

## References

- https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderPassMultiviewCreateInfo.html
- https://docs.vulkan.org/refpages/latest/refpages/source/ViewIndex.html
