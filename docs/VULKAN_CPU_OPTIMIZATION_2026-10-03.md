# CPU draw preparation optimization, 2026-10-03

## Baseline

Ten-second simpleperf capture of the user-running outdoor scene:
`research/farcry_cpu_plants.data`, decoded report
`research/farcry_cpu_plants_report.txt`. This is not a bunker measurement.

The main game thread accounts for 88.96% of CPU samples. Render accounts for
about 67% of its Update call tree. Draw submission includes substantial time
in QueueStockIndexedDraw, StockDraw construction, memcpy/memmove and memset.
ResetStockLinearTexgen alone accounts for 1.12% of total samples.

After installing the first CPU changes, a 10-second bunker capture produced
33,945 samples in `research/farcry_cpu_bunker.data`. The game thread accounted
for 88.92% and C3DEngine::RenderScene for 61.42%. A bump texture load path in
DrawBuffer accounted for 18.53%: missing or not-yet-available bump textures
were searched first by material name and then by material directory each time
the draw path retried them. A follow-up capture had only 476 samples and showed
the Android activity idle, so it is not a valid gameplay comparison.

After adding the failed bump lookup cache, the next 10-second bunker capture
(`research/farcry_cpu_bunker_cached.data`, report
`research/farcry_cpu_bunker_cached_report.txt`) produced 40,415 samples. The
main game thread accounted for 88.9%; `C3DEngine::RenderScene` accounted for
53.77% of total samples. Texture loading no longer appeared in the renderer's
hot call tree or flat report at a 0.8% threshold. These profiles confirm the
repeated lookup path is gone; differing sample counts and CPU clock rates do
not establish an FPS or frame-time gain.

## Changes

- Move optional linear texgen, fixed lights and shadow matrices out of every
  StockDraw into an auxiliary snapshot allocated only when used. Copy active
  entries only. Retain auxiliary vector capacity between frames.
- Reset linear texgen validity flags instead of clearing its entire payload.
  Zero-light and zero-shadow updates invalidate their payload without copying it.
- Reuse cached pipeline handles before constructing pipeline descriptions.
  Reflection cache misses still construct their required description.
- In multiview, prepare one ordinary eye UBO. Keep both eye transforms in the
  stereo storage buffer and both reflection UBOs for separate reflection passes.
- Skip normal stream conversion when normals already occupy the required
  interleaved stream. The dedicated simple plants shader does not consume normals.
- Remove duplicate stage 4-7 snapshots and avoid exp2 for zero texture LOD bias.
- Cache failed Vulkan bump texture lookups by material resource, name and path.
  Successful loads immediately clear the entry. A later successful load made
  elsewhere is observed through `IsTextureLoaded()`. Changed material names or
  paths trigger a fresh lookup. Keep at most 2,048 entries and evict one when
  adding another, so changing levels cannot grow the cache without limit.
- Build the 16 immutable Vulkan vertex format descriptions once and copy the
  selected description for each draw instead of rebuilding its attributes.

These are common draw preparation paths, including indoor geometry. Material
and shader equations are unchanged. Auxiliary data is captured before worker
uniform preparation and consumed before the next BeginFrame clears its vector.

## Build and runtime

All affected native libraries and release APK build successfully with
`build-android.bat`. Runtime visual parity and CPU timing improvement require
a new user-launched capture, preferably inside the Training bunker. Do not
claim a measured CPU improvement from this build yet.

The next 10-second bunker capture (`research/farcry_cpu_bunker_vertexcache.data`,
report `research/farcry_cpu_bunker_vertexcache_report.txt`) produced 42,283
samples. `GetVulkanVertexFormat` no longer appears above the 0.7% flat-report
threshold (it was 1.20% in the preceding capture). This supports that the
repeated format construction was removed, but does not establish a frame-time
gain. The remaining repeated tree lookup on the 64-word pipeline key led to a
last-pipeline fast path in the latest source; its follow-up capture is below.

The next 10-second capture after the last-pipeline fast path
(`research/farcry_cpu_bunker_pipelinefast.data`, report
`research/farcry_cpu_bunker_pipelinefast_report.txt`) produced 42,253 samples
and 15.201 billion CPU cycles, close to the preceding 42,283 samples and
15.205 billion cycles. Pipeline tree-comparison symbols disappeared from the
report. Main-thread render share changed from 63.08% to 62.60%; this difference
is too small to call a measured frame-time improvement. QueueStockIndexedDraw
and upload-buffer copies remain the larger Vulkan-side costs.

The old simpleperf addresses belong to the pre-change libraries; the rebuilt
libraries must not be used to symbolize that old capture.

The current APK then also changed `StockDrawAux` to leave inactive fixed-light,
texgen and shadow matrix slots uninitialized, explicitly initializing only
texgen activity flags and copying only active data. Uniform blocks are zeroed
before active entries are copied. This removes a full auxiliary snapshot clear
per draw with no shader-layout or output changes; a new runtime capture is
needed to quantify its effect.
