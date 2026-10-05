# CPU preparation optimizations

Baseline: `research/vulkan_frame_timings_latest.csv`, frames 3000–3120:
approximately 1500 queued draws, 32–33 ms scene capture, 7–8 ms command
recording, 32–33 ms submission fence wait. The latter includes queued GPU
work and is not a shader timestamp measurement.

Changes:

- Build scene UBOs once after capture rather than repeatedly during every
  submission and again whenever the uniform buffer grows.
- One persistent worker prepares half of the immutable draw snapshots when
  there are at least 256 entries; the render thread handles the other half.
  Each writes separate regions of coherent mapped memory. Join precedes
  submission and teardown. Engine state, descriptors and Vulkan commands
  remain on the render thread.
- Fill common uniforms once per draw and reuse them for both eyes. Generate
  reflection blocks only for draws with a reflection transform. Clear commands
  have no UBO and are skipped.
- Evaluate FOV tangents once per eye and frame. Cache eye matrices per near/far
  range during scene recording instead of recalculating tracking quaternions
  and projection for every draw.
- Skip consecutive redundant scene pipeline/index bindings. Preserve draw order.
- Create reflection pipeline variants only for eligible reflected materials.
- Allow release CPU sampling through Android `profileable`, without enabling
  a debug build. Previous shell simpleperf recording was denied by the device.

The CSV includes `uniform_avg`/`uniform_max` inside `record_avg`/`record_max`.
Compare total capture + record + end time; work moved from capture to record
must not be mistaken for a speedup. No multiview or general engine threading
was introduced. Runtime performance and visual parity need the same headset
scene after installation. Compilation succeeds; no automated tests were run.

## Second capture and CPU profile

`research/vulkan_frame_timings_parallel.csv`, frames 3000–3120: capture
30–32 ms, record including uniform preparation 8.1–8.3 ms, fence wait about
30 ms. Total remains approximately 72–73 ms. Uniform preparation is about
1.6–1.7 ms. This does not establish a large improvement, and the scene counts
are not identical to the baseline.

A 10-second simpleperf CPU capture now succeeds with profileable enabled.
Flat profile: memmove 13.60%, memset 2.40%. Call stacks identify whole-array
copies/allocations in DrawBuffer's ensureVertexCopy, invoked before evaluating
pass color even for generators which only change global material color.
Raw call profile: `research/farcry_perf_parallel_calls.data` (recorded before
the following source changes; use matching build symbols when analyzing).

Second changes:

- Evaluate explicitly recognized global-only RGB/alpha generators through
  the same NULL/OpenGL parity evaluator without allocating a vertex copy.
  Keep the full path for actual per-vertex generators and prior retained
  vertex colors, preserving the global-color commit and guard side effects.
- Reuse capacities for generated vertices, separate UV vertices, lightmap UVs
  and lit vertex arrays. A deque of scratch slots and a scoped depth counter
  keep nested DrawBuffer calls isolated.

These changes compile; the second changes still require a headset capture.

## Third capture and changes

`research/vulkan_frame_timings_scratch.csv`: heavy scene capture is now
24-25 ms, recording about 8 ms, fence wait about 29-30 ms. Full frame remains
approximately 65 ms; the CPU improvement has not fixed the low frame rate.
`research/farcry_perf_scratch.data` records the installed build before the
following changes. memmove remains 13.06% of CPU samples; indexed client
geometry upload and scene descriptor binding remain substantial costs.

- Upload only the vertex and lightmap UV range referenced by client indices.
  Preserve original indices with a negative base vertex. Keep the full range
  for separate tangent streams, whose offsets must remain aligned.
- Retain unchanged texture descriptor sets 1-7 between scene draws. Bind
  set 0's per-draw uniforms every draw; additional sets' uniform bindings are
  unused by all scene shaders and use dynamic offset zero.

Native libraries and release APK build successfully. Performance of these
latest changes still requires a matching headset scene capture.

## Fourth capture

`research/vulkan_frame_timings_ranges.csv`, stable frames 3120-3480:
capture 22.4-22.7 ms, recording 6.5-6.7 ms, fence wait 28.6-31.2 ms.
Full frame remains around 62-63 ms. The user reports no visible improvement.
`research/farcry_perf_ranges.data`: memmove 9.20% of CPU samples; copying
StockDraw into the command vector remains a measurable part of draw capture.

- Construct StockDraw directly in the command vector after all failure checks,
  retaining initialization and the original per-draw uniform offsets.
- Add optional GPU timestamp queries, guarded by queue timestamp support.
  Measure auxiliary uploads/shadows/reflections, scene plus HUD, and output
  across both eyes. Read only after the existing fence; no additional waits.
  Save aggregated GPU timings every 120 frames to vulkan_gpu_timings.csv,
  stop after 15360 frames, and release the query pool on shutdown.

The GPU timestamp measurements are needed to distinguish actual GPU work
from CPU fence-wait time before choosing the next GPU or scheduling change.

## GPU capture and opaque shader specialization

`research/vulkan_frame_timings_inplace.csv` and
`research/vulkan_gpu_timings_latest.csv`, frames 2760-2880:
CPU capture 22.2-22.7 ms, command recording 6.6-6.7 ms, fence wait
29.3-30.3 ms. GPU auxiliary work is about 0.001 ms, scene plus HUD
28.0-28.9 ms, output 1.1-1.2 ms. The fence wait is largely GPU scene work;
the latest CPU copy change did not materially improve the frame time.

All legacy scene fragment shaders contained uniform-dependent clip and
alpha discard even for opaque passes without these features. Add fragment
specialization ID 62 to remove both discard instructions for passes with
no material AlphaRef, no legacy alpha test and no regular/reflection clipping.
Preserve the complete original path for any pass requiring either feature.
Include the flag in the pipeline cache key; use the original discard behavior
by default for callers that do not opt into this specialization. No explicit
early_fragment_tests is forced on alpha-tested materials.

Shaders, native libraries and APK compile. Actual GPU time improvement and
visual parity of this change require the next headset capture.

## Discard specialization capture: no measured improvement

`research/vulkan_frame_timings_discard.csv` and
`research/vulkan_gpu_timings_discard.csv`, stable frames 2760-3000:
CPU capture 22.5-23.7 ms, recording 6.6-6.8 ms, GPU scene 29.9-30.9 ms,
output 1.2-1.3 ms. Total frame time is approximately 64-66 ms. This capture
does not demonstrate a benefit from discard specialization; scene GPU time
is slightly higher than the previous capture despite similar draw counts.
These separate captures do not establish that the specialization caused
the increase. No performance fix is claimed.

A live device sample reports GPU clock 545 MHz and busy/total counters
536974/1000232 (about 54%). Android thermal status is 0. These are a single
sample, not sustained measurements. CPU capture and GPU execution are
currently serialized by the end-of-frame fence. Low aggregate utilization
therefore does not exclude substantial costs on both CPU and GPU.

## Overlap CPU capture with submitted GPU rendering

Remove the CPU fence wait immediately after vkQueueSubmit. The Vulkan
binding's OpenXR image-release rules accept unfinished commands on the bound
graphics queue; return the image and submit its composition layer after queue
submission. Reference:
https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#vulkan2-swapchain-image-layout

Use two alternating client vertex/index arenas. BeginFrame no longer waits
for the previous submission before CPU draw capture. Capture therefore runs
while the GPU executes the previous frame. Bound the queue to one outstanding
graphics submission: complete it immediately before reusing the shared uniform
buffer, command buffers, query pools and descriptor updates at EndFrame.
This is partial CPU/GPU overlap, not fully independent recording of two frames.

Track submitted draw and panel vertex/index/tangent/lightmap buffers. A resource
manager guard synchronizes only when an upload or destruction touches a buffer
referenced by the pending submission. Protect image destruction and existing
sampler/descriptor invalidation as well. Retire grown geometry/uniform chunks
with their submission, rather than freeing them at BeginFrame. Read GPU timing
queries after completion using the saved frame number and draw count.
Synchronize teardown and detach resource callbacks before destroying the
renderer. Two geometry arenas remain bounded and are freed on shutdown.

The GPU-wait CSV field now measures resource-reuse fence waits, including
hazard-triggered waits, rather than a wait immediately following submission.
Record/uniform timers exclude the fence wait at EndFrame. Waits triggered
during draw capture also contribute to capture wall time; do not sum them twice.
Native libraries and APK build; runtime stability and frame overlap need the
next headset capture. FPS improvement has not yet been measured.

## First overlap capture

`research/vulkan_frame_timings_overlap.csv` and
`research/vulkan_gpu_timings_overlap.csv`, stable frames 3360-3720:
CPU capture 22.7-23.3 ms, record 6.9-7.0 ms, resource fence wait 5.2-6.5 ms,
EndFrame 12.5-13.8 ms, update gap 1.9-2.0 ms, BeginFrame about 0.17 ms.
Frame interval estimated from begin + capture + end + update is 38-39 ms,
approximately 26 FPS, versus 64-66 ms / 15-16 FPS before overlap.
This is a timing-derived estimate, not a headset FPS overlay measurement.
GPU scene remains 29.4-30.0 ms, output about 1 ms. GPU work was not reduced;
CPU capture now overlaps it instead of waiting immediately after submission.
The remaining shared-resource fence wait and about 7 ms of recording still
prevent fully independent CPU command recording while GPU rendering proceeds.

## 72 Hz preset CPU capture and recording overlap

Current baseline captures: `research/vulkan_frame_timings_72.csv` and
`research/vulkan_gpu_timings_72.csv`. Stable late rows show 1335-1376 queued
draws, CPU capture 15.8-18.2 ms, recording including uniform preparation
4.1-4.6 ms, resource fence waits 0.8-3.3 ms, and GPU scene 19.4-22.0 ms.
These measurements precede the changes below.

A 10-second simpleperf capture on the running release reports 89.6% of CPU
samples on the main game thread. Scene rendering consumes 70.7% of its update
samples, RenderEnd another 19.6%, and system update 7.1%. Queueing client
geometry, draw snapshot initialization, normal/terrain conversion and memory
copies are major contributors. This identifies renderer CPU work rather than
physics or AI as the primary CPU target; it does not imply GPU time is zero.

Uniform and stereo transform buffers now have two bounded frame regions.
Descriptors remain stable; dynamic UBO offsets and stereo draw indices select
the current region. Each OpenXR image has two sets of eye command buffers.
Prepare uniforms and record commands while the previous submission executes;
complete that submission immediately before recycling its fence and submitting
the new commands. Query results are collected before the new commands can
reset their pools. Allocation growth, texture staging reuse, descriptor/image
destruction and geometry hazards still synchronize when necessary.

Normal and terrain conversion now process the index range of the current
material group. Retain conversion scratch arrays for terrain, water, generated
UVs, muzzle flashes and triangulated quads. Retain normal-array size to avoid
zeroing unrelated vertices on every draw. Use the captured pipeline handle
instead of looking up the same 64-word cache key during command recording.

Release native libraries and APK build successfully. Runtime stability, visual
parity and FPS change for this revision require another user-launched capture.

## GPU-bound capture

`research/vulkan_gpu_timings_gpu97.csv` is the user's 72 Hz / reduced-resolution
scene. Stable rows show the main scene at 18.8-19.5 ms, output at 0.5-0.7 ms,
and roughly 1.3-1.8 ms of GPU setup work; one row reached 25 ms during a spike.
This exceeds the 13.9 ms 72 Hz frame interval in the scene itself. The observed
97% utilization agrees with GPU saturation. A matched per-draw GPU breakdown
has not yet been collected, so changing shader work or ordering speculatively
could alter lighting, transparency or water without a defensible win.

The latest APK adds six GPU timestamp slices across the ordered main-scene draw
list. This will point to expensive draw groups/material passes while retaining
all geometry and texture detail. The device confirms the requested 72 Hz mode.
