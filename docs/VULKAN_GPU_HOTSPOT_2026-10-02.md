# Quest 3 GPU hotspot measurement

Captured results: `research/vulkan_gpu_ab.csv`.

The strongest validated result is pair 4. Baseline frame 4559 and diagnostic
frame 4560 contain the same 1,333 queued commands and the same inventory hash
11510331014369972777. The diagnostic frame omits 385 CGRCPlants commands with
alpha blending and no depth writes. Main scene GPU time falls from 20.908906 ms
to 11.723489 ms: a 9.185417 ms reduction, or 43.93% of baseline scene time.
Output-pass time is measured separately and is not part of this reduction.

This is a measured net cost of the blended vegetation group, not a prediction
that an equivalent-looking optimization can recover all 9.19 ms. The texture
census identifies `templplants1 / CGRCPlants`, including
`Objects/Natural/coastal_objects/cst_mall_grass_yellow`, as the principal
candidate from the earlier draw-level statistics.

Other pairs have different inventory hashes and are preliminary: omitting all
alpha-tested geometry reduces scene time by 11.65–11.91 ms; this group overlaps
the blended vegetation group, so these reductions cannot be added. Omitting
terrain changes time by about 1.18–1.33 ms. The water tests vary between a
0.04 ms reduction and a 0.48 ms increase, which does not establish a benefit.

Per-draw pipeline-statistics queries on this Adreno driver are unsuitable for
absolute attribution: their summed fragment counts were 4.249 times the
independent whole-scene count. Earlier percentages based on those counters are
not validated. Paired scene timestamps outside render passes provide the
evidence above.

The diagnostic omits geometry for only eight isolated frames after explicit
arming. It stops automatically. Normal rendering includes all groups.

Next investigation: compare CGRCPlants and its vertex lighting, alpha test,
fog, texture sampling and depth/blend state with OpenGL. Preserve foliage
coverage, density and opacity when optimizing the normal rendering path.
