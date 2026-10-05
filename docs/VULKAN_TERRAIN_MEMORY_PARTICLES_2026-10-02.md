# Terrain, native memory and particle follow-up (2026-10-02)

## Follow-up: measured terrain fade error

`research/terrain_detail_current.txt` shows world-space terrain vertices
around (300, 1000, 20), while the shader camera switches from real world
coordinates to values around (0, 0, 1). Fade distance is 52.15362. The
incorrect distance therefore clamps the detail weight to zero, even when
`research/terrain_coverage_current.txt` reports millions of passing samples.
Geometry visibility alone was not sufficient to diagnose the missing detail.

Vulkan now treats an object without FOB_TRANS_MASK as identity in
CCObject::GetInvMatrix, ignoring stale cached matrix indices. Both terrain
sector fading and terrain overlay parameters use the world camera directly
for such objects, and retain inverse transformation for transformed ones.
OpenGL's separate implementation is unchanged. The code and APK compile;
the resulting headset appearance still needs confirmation.

## Runtime evidence

Quest process `com.nearchuckle.farcry`, PID 28343: Android reported
5,984,109 KB total PSS including 1,806,243 KB swap PSS immediately before
the process disappeared. Native heap dominated the resident allocations;
graphics accounted for approximately 570 MB. This establishes memory
pressure, but does not attribute every allocation to a single subsystem.
The multiplayer material trace is saved in
`research/terrain_multiplayer_trace.txt` and includes queued terrain detail
layers with valid GPU images and vertex format 8.

## Confirmed allocation bug

`CLeafBuffer::CheckUpdate` asks `CVertexBuffer::GetStream(VSF_TANGENTS)`
whether a tangent stream exists. Vulkan inherited the NULL implementation,
which always returned null. Each check allocated another tangent array,
overwrote the previous pointer, then returned false because GetStream still
reported null. The old allocations could never be released.

The shared NULL implementation now returns the CPU stream for Vulkan only.
Replacement stream allocation also frees the old CPU array in Vulkan.
OpenGL has its own GetStream implementation and is unaffected.

## Resource lifetime and particles

Vulkan texture removal previously destroyed the GPU mirror before
STexPic::Release checked reference counts and FT_NOREMOVE. Shared images
could therefore disappear while still in use. Mirror destruction now runs
from the texture manager's RemoveFromHash callback at actual release.
This also covers texture releases initiated directly by shader resources,
which previously bypassed CVulkanRenderer::RemoveTexture.

CGRCAmbient_Particle already evaluates object/material opacity in Ambient.w.
Its translated pass no longer applies the generic opacity multiplier again.

## Terrain geometry

Vulkan detail geometry now covers the full layer fade radius around the
viewer instead of a directional footprint generated before the tracked eye
rotation. Grid regeneration depends on movement rather than head turns.
The CPU offset no longer embeds a camera direction which becomes stale
between regenerations. The existing terrain overlay vertex program still
applies its normal displacement and clip-W depth adjustment.
Layer textures, surface masks, projection planes and fade distances remain
the stock terrain inputs. OpenGL retains its original footprint generation.

## Verification status

All affected native libraries and the release APK compiled successfully.
Headset appearance and memory stability require another user-launched run.
The APK records at most 32 memory/resource snapshots, once per 240 frames,
in `/sdcard/FarCry/vulkan_memory_snapshots.txt`; it does not flood logcat.
