# Vulkan renderer parity with OpenGL

## Surface decal parser correction and regression boundary

Quest's bounded decal capture identified `templdecalmodulate` drawing Training
wall dirt textures with state `0x100` and MODULATE operations. Its original
script requires `DST_COLOR ZERO` blending and REPLACE. `shGetObject` returns
zero on unknown CG program tokens, ending the NULL hardware-pass parser before
it reaches the Layer containing those settings.

The parser now consumes CG declarations/parameters for `eS_Decal` surface
materials and retains their Layer states. Multiplicative decals receive wall
lighting through destination-color blending; they do not require texture alpha.
Ambient parameters are evaluated for those decal programs that declare them.

A broad application to all HW shaders produced gray geometry and missing
transparent objects in headset feedback, since their newly exposed passes lack
complete Vulkan program translations. That application was rolled back: other
sort categories retain their established fallback. Full transparent-material
shader parity remains unfinished and must not be inferred from this decal fix.

This file tracks the renderer port against the stock `XRenderOGL` path. “Partial” means that Vulkan has a translation for the basic case, but not the OpenGL behavior for every shader, state combination, or render-element type.

## Frame and scene pipeline

| Subsystem | Status | Current Vulkan behavior / remaining work |
|---|---|---|
| OpenXR frame, stereo views, swapchain | Implemented | Owns one XR frame and records one eye layer per view. Runtime/device paths still need in-headset validation after renderer changes. |
| Main render-item lists | Partial | `EF_EndEf3D` sorts and traverses the stock lists. Vulkan now runs the CPU-visible `EF_PreRender(1)` camera/frame setup once per non-empty list, establishes `RBF_3D` for item draws, tracks current/previous objects, and restores the saved object/list state. It honors `RBPF_IGNORERENDERING` like OpenGL (preprocess only, then clears the flag) and skips `LAST` in nested render recursion. It also maintains the legacy GL projection/model-view matrices and matrix getters used by CPU render helpers. It still does not run batching/flush callbacks or all `EF_ObjectChange` side effects. |
| Shader/pass setup | Partial | Common and hardware material scripts are loaded. The NULL-backed Vulkan renderer now parses hardware technique conditions, fixed-function layers, cull/render state, and pass kinds into the stock pass structures. `General`, per-light `Light`/`DiffuseLight`/`SpecularLight`, and a multi-light approximation are translated. Vulkan approximates OpenGL `MultiLights` by additive per-light stock lighting draws with pass light-type/light-map/occlusion filtering. Parsed `LMNoAmbient`, `LMNoSpecular`, `LMNoAddSpecular`, and `LMOnlyMaterialAmbient` flags now affect the stock lighting approximation. Basic `RFT_DEPTHMAPS` support keeps `DEPTHMAP` branches active and supplies caster targets, per-stage receiver matrices, and receiver-side `LEQUAL` comparison. `MultiShadows` dispatches consecutive shadow/light passes per dynamic-light caster group, separates alpha-blended DOT3 light-map casters, excludes casters flagged `ERF_CASTSHADOWINTOLIGHTMAP`, sorts regular light groups by caster count, and batches declared shadow samples. Exact OpenGL light-map shadow composition, self-shadowing, terrain shadows, fur, and remaining non-`General` behavior are incomplete. When a selected technique has no translated pass, Vulkan falls back to the shader's fixed-function passes if present. Cg program declarations and execution, custom parameters, and register combiners are not translated by this parser. |
| Preprocess | Partial | Each eye now renders the scene/UI into an internal color target and samples it in a fullscreen output pass into the OpenXR color attachment. This creates a Vulkan-owned sampled source without requiring transfer usage from runtime-owned swapchain images. `EF_Preprocess` / `EF_DrawREPreprocess` are still not ported, so portal, reflection/refraction, screen texture, heat-map, and related off-screen passes have no Vulkan equivalent. The ordinary `eS_Sky` draw is still queued after the leading preprocess records and reaches `CRESky`; full sky texture/pass parity remains incomplete. |
| Fixed-function indexed geometry | Partial | Triangle lists, strips, fans, quads, and grouped leaf geometry are queued. `CREPolyMesh` now translates its CPU positions, plane normal, white vertex colors, base/light-map UVs and indices into the selected Vulkan stock layout; tangent-only layouts remain unsupported. If a legacy base/light/normal texture has no Vulkan mirror, the Vulkan path now keeps the geometry and falls back to the supported untextured/base stage rather than dropping the whole draw. Full primitive-group and render-element coverage remains open. |
| Dynamic/client geometry | Partial | A number of immediate and transient draw paths are routed to Vulkan; GL-only draw helpers and some render elements still have no equivalent. |
| Viewport/scissor | Partial | Object scissor and SetViewport are captured per stock draw; logical window viewport coordinates are scaled to each XR eye target and submitted as dynamic Vulkan viewport state. Off-screen target viewports and runtime parity still need validation. |

### Call-path audit

The Vulkan renderer is not a port of `CGLRenderer::EF_PipeLine`; it is a separate executor called from `CVulkanRenderer::EF_EndEf3D`. It snapshots the six stock item-list ends, sorts selected lists, decodes sort keys, then calls each element's `mfDraw` directly for each selected shader pass. By comparison, OpenGL runs `EF_RenderPipeLine` -> `EF_PipeLine`, calls `EF_PreRender`, handles preprocess items, changes objects through `EF_ObjectChange`, starts shader batches with `EF_Start`, prepares render elements with `mfPrepare`, flushes state/batches, restores clip/depth/list state, and runs per-list cleanup.

This difference has concrete consequences:

- Vulkan still does not run the full `mfPrepare`/batch/flush sequence before each item. The ordinary leaf path now ports `CREOcLeaf::mfPrepare`'s clip-plane bounds cull, modified secondary-stream upload, and chunk range setup while avoiding its GL pipe-array writes. Character skinning and `mfCheckUpdate` are handled at object transitions and leaf submission; other render elements' prepare-time updates still need Vulkan equivalents.
- `CREOcLeaf` with a fixed-function shader reaches Vulkan `DrawBuffer`, including its primitive-group expansion, and now runs OpenGL's `mfCheckUpdate` first. Hardware techniques parsed by `XRenderNULL/NULL_Shaders.cpp` take priority over fixed-function passes, matching `EF_FlushShader`; their general leaf pass also updates and submits the indexed section directly because `CNULLRenderer::EF_DrawIndexedMesh` is empty. The hardware pass pointer is the selected `SShaderPassHW`. This translates the parsed fixed-function layer data; Cg/NVParse program bodies, program parameters and original custom vertex streams still do not execute.
- OpenGL has dedicated pass executors for diffuse/light-map, shadow, fog, screen, and other hardware pass types. Vulkan translates common `Light`/`DiffuseLight`/`SpecularLight` behavior and approximates `MultiLights` by splitting directional, point, and projected light sources into additive stock draws. Projected diffuse passes now apply the light orientation, frustum-angle cone cutoff, and sampled cubemap cookie in standard-normal and bump-map lighting; the cubemap projection is an atlas-based approximation. The Vulkan renderer advertises `RFT_DEPTHMAPS`; caster collection, ordinary `Shadow` caster grouping, per-stage receiver transforms, and receiver-side SGIX `LEQUAL` comparison are implemented. `MultiShadows` dispatches the contiguous shadow/light pass group, separates the alpha-blended DOT3 caster list, filters `ERF_CASTSHADOWINTOLIGHTMAP`, orders regular light groups by caster count, and batches declared samples. Exact OpenGL light-map shadow composition, terrain shadows, and self-shadowing remain incomplete. This path has not yet been visually validated in the headset. Fur, volumetric-fog, and screen-pass semantics also remain incomplete. When a selected technique has no translated pass, Vulkan uses the shader's fixed-function passes as a visibility fallback when available; shaders without either usable representation remain unsupported.
- `EF_Preprocess` is absent. The Vulkan list walker consumes the leading `eS_PreProcess` run without executing it, matching OpenGL's removal of those records after its off-screen work. The ordinary `eS_Sky` record follows that run in the same list and reaches `CRESky`/`DrawTriStrip`; portal, reflection/refraction, screen-space effects, and generated render textures remain unavailable. This list handling alone does not explain an all-black world.
- `EF_PreRender(1)` / `EF_SetCameraInfo` now update the state read by CPU render elements before each non-empty list: render/frame-object/transform counters, previous-object reset, camera world position and basis vectors, world-space/matrix flags, and the sun direction from the active recursion level's dynamic lights. Vulkan builds the legacy GL perspective matrix from `CCamera`, maintains the combined/inverse camera-projection matrices, returns GL-compatible model-view/projection matrix data, and updates the current object model-view on object transitions. Camera bytes now match OpenGL's direct `glLoadMatrixf(GetVCMatrixD3D9().GetData())` followed by `glGetFloatv`; an extra Vulkan-only transpose was removed because it changed the whole-world view transform. The item walker establishes `RBF_3D`, tracks object transitions, and performs per-list cleanup/restoration. `EF_PreRender(3)` depth-range/clear behavior is still not ported; XR eye projection is built from each runtime view instead.
- Vulkan now rebuilds `m_pActiveDLights` with `EF_BuildLightsList` and sets resource opacity before `EF_SelectHWTechnique`, matching the inputs OpenGL uses for light-count and opacity conditions. `FOB_NEAREST` draws now use OpenGL's narrowed FOV, 0.01/40 near/far planes, and 0..0.1 depth range independently for each eye. Other `EF_ObjectChange` behavior remains incomplete, including cube-map capture transitions, and the complete object/batch lifecycle.
- Pass state is now seeded into the NULL renderer's current-state cache before `mfDraw`; state changes made by the render element through `EF_SetState`, `SetCullMode`, and `SetColorOp` are carried into queued Vulkan draws. This covers these local overrides, but does not replace the full GL state cache and per-flush rules.
- The Vulkan renderer inherits the rest of `CNULLRenderer`. Its default `DrawBuffer`, `DrawTriStrip`, `EF_DrawIndexedMesh`, and most renderer state methods are no-ops; only methods explicitly overridden or queued by `CVulkanRenderer` produce Vulkan work. A render element that issues GL calls directly, or expects one of those null methods to submit geometry, is not covered.

### Geometry and shader coverage at the submission point

| Path | Vulkan implementation | OpenGL parity gap |
|---|---|---|
| Stock indexed leaf geometry | Vertex formats 1-16 have input layouts; world submissions reject pre-projected font format 5, tangent/binormal-only format 14, and UV-only format 15. Triangle lists, strips, fans, and quads are handled; quads are triangulated and grouped leaf sections are submitted separately. Fixed-function leaves run `mfCheckUpdate`; selected hardware general passes update and submit `CREOcLeaf` chunks through the Vulkan fixed-function translation. Enabled single-light passes consume the CREOcLeaf selected index stream. | The original hardware-program semantics remain unsupported. Unsupported formats/topologies or invalid buffers are rejected. |
| Transient/client indexed geometry | `DrawBuffer`, client indexed draws, both 3D `DrawDynVB` overloads, `CRETempMesh`, `CREPolyMesh`, `CREClientPoly`, and `CREPolyBlend` / `CREAnimPolyBlend` route supported data into queued indexed draws. The non-indexed dynamic overload expands its triangle vertices into sequential 16-bit indices. Each draw now binds its byte range directly, so mixed vertex strides cannot corrupt `vertexOffset` or shift a separate tangent stream. | No general translation for each OpenGL helper or transient render element; `mfPrepare` and index-stream mutations are not reproduced generally. `CREPolyMesh` uses its explicit CPU-data adapter because its legacy vertex-copy code in `mfPrepare` is commented out. PolyBlend supports its common oriented quad path but not the original pipe batching behavior or every specialized flare/program combination. |
| Scene vertex shaders | Hand-authored shaders cover untextured, vertex-color, simple-lit, textured, textured-lit, bump, and multi-texture variants, including color and no-color variants. Multi-texture draws with normals now apply the queued material/light terms instead of bypassing lighting; vertex formats without colors no longer read an absent color attribute. | No generic translation of Cg/NVParse or loaded vertex/pixel programs; custom attributes, skinning, and per-program constants are not reflected. |
| Scene fragment shaders | Fixed-function texture combine through eight stages, selected alpha tests, material opacity, simple diffuse/bump lighting, basic fog, independent LOD bias, and independent projective 3×3 UV transforms for stages 3–8 are represented in the shader set, including homogeneous q division. | Stages 3–8 can transform either stock UV set independently; custom generated coordinates and composition with the bump-map shader remain unsupported. Many combine/rgbgen/alphagen cases, specular/projector/lightmap paths, exact multi-pass lighting, volumetric fog, and special pass composition also remain incomplete. |
| Per-object preparation | Active dynamic-light list and opacity now feed hardware-technique selection; nearest/refraction filtering and character skinning are handled. `FOB_NEAREST` also applies the OpenGL narrow-FOV projection, near/far planes, and depth range per draw. | Full OpenGL `EF_ObjectChange` / `EF_Start` lifecycle, cube-map transitions, ordinary `EF_PreRender(3)` depth-range behavior, batching, and several animation/stream updates remain incomplete. |
| Render-element state | Per-pass state is seeded for `mfDraw`; local `EF_SetState`, cull-mode, and texture-combine changes are reflected in the Vulkan draw. With the Y-flipped OpenXR projection and positive-height viewport, Vulkan uses CCW front faces to match OpenGL's visible faces with back-face culling. Resource alpha/opacity overrides honor `EF2_IGNORERESOURCESTATES`, and `MTLFLAG_2SIDED` disables culling unless resource states are ignored. | Remaining GL state cache behavior, scoped resets, and interactions across multiple OpenGL flushes are incomplete. |
| Texture sampling | Common image data is mirrored to Vulkan RGBA textures; six cubemap faces are collected and mirrored in a 3×2 atlas; projected-light stock shaders select atlas faces from the object-space projector basis. Nearest/linear/mip filters, supported anisotropy, per-stage animated UV matrices for all eight stages, selected CPU texgen modes, and per-stage LOD bias are represented. | The atlas lookup is an approximation of OpenGL cubemap orientation/filtering and is currently used for projected-light cookies only. Ordinary `samplerCube` material stages, render targets, all legacy formats/mips, sampler changes after runtime cvar updates, separate DOT3 shadow/light-map caster sampling, and other GL texture-target semantics remain missing. |

The reported runtime state is: the HUD is visible, most 3D content is black, and some long white triangles appear. The HUD uses a separate panel draw path, so its visibility confirms the OpenXR frame/compositor and panel path, not the scene renderer. The triangles confirm that at least some indexed scene submissions reach rasterization; they do not prove that the scene vertex/index data, eye/model-view transform, texture bindings, or material shader are correct. Source inspection found and removed an extra Vulkan-only transpose of the camera matrix: OpenGL loads the original `GetVCMatrixD3D9().GetData()` and reads the same matrix bytes back, whereas Vulkan had been transposing them before all world draws. This is a concrete candidate for the distorted triangles/black world, but the running game must confirm the visual fix. Other gaps remain: `QueueStockIndexedDraw` can reject missing texture mirrors and unsupported material/pipeline combinations, while selected hardware-technique paths substitute hand-authored fixed-function shaders for the original pass programs. The hardware-technique selection/leaf submission gap is partly closed, but non-General pass semantics and original Cg/NVParse execution are absent. Vulkan reports per-frame queue rejections and per-eye record skips every 120 frames; those counters can identify a remaining dominant rejected path after the matrix fix is installed.

## Shader and material behavior

| Subsystem | Status | Current Vulkan behavior / remaining work |
|---|---|---|
| Stock vertex formats | Partial | Formats 1–16 have layouts, but screen-font and tangent-only/UV-only layouts are intentionally excluded from world draws. Wave, vertical-wave, bulge, squeeze, from-center and beam deforms operate on the Vulkan upload copy for indexed `DrawBuffer` submissions. `eDT_Flare` builds OpenGL-style 16-vertex/54-index geometry after preceding source-space deforms; geometry-changing stages after the flare and lightmap UV flares still fall back. Custom normal generation remains incomplete. |
| Texture combine | Partial | Eight texture stages and the operations that `CGLRenderer::EF_SetColorOp` actually implements are represented in Vulkan shaders. Color `ADD`, `ADDSIGNED`, diffuse-alpha blend, texture-alpha blend, replace/disable, and modulate variants follow the OpenGL dispatch; unsupported operations fall through to `MODULATE`. Vulkan retains effective color/alpha combine modes and source arguments per texture unit when OpenGL marks them unchanged or executes an explicit no-op, including OpenGL's `eCA_Specular` no-op selector, GL default source 2 as `CONSTANT`, and blend-operation source 2 initialization; interpolation now uses the effective source-2 alpha. The stock hardware-pass path now carries OpenGL current global color into each texture stage constant; utility-only state changes and some bump/environment combinations remain. |
| RGB/alpha generation | Partial | Fixed, identity, object, render-element, world, components, wave, noise, and light-style values are translated into shader constants. RGB `OneMinusFromClient` now inverts each vertex RGB in the fragment stage while preserving alpha. Portal/beam, client color fetch, and exact client-array mutation behavior remain. |
| Texture coordinate generation | Partial | Base/lightmap UVs, two UV channels, animated matrices, environment, projection, sphere-map, sphere-map-environment, and ordinary shadow-map projection are translated for stock indexed meshes using the OpenGL CPU formulas. Shadow receiver coordinates use the active caster group and per-stage light/view/object matrix with fragment-side depth comparison. Environment and sphere mapping use leaf meshes' source `SPipTangents::m_TNormal` where available. Quad UV generation follows OpenGL and only fills coordinates when no render element is active. Shader-object `HW_ObjectLinear` / `HW_EyeLinear` S/T/Q plane generation and `HW_NormalMap` / `HW_ReflectionMap` / `HW_SphereMap` vector generation are translated for the stock 2D texture path. `HW_EmbossMap` survives NULL/Vulkan parsing and copying, but Vulkan retains fallback: OpenGL's implementation enables only S/T/R, while `GL_NV_texgen_emboss` requires S/T/R/Q in emboss mode and enabled for defined output. Its previous-stage/`GL_LIGHT0` result is therefore undefined by that extension for this caller. 3D/cube coordinate sampling, other shader-object `m_GTC` generators, separate DOT3 shadow/light-map coordinates, and render-to-texture remain unsupported. |
| Baked lightmaps | Partial | Ordinary color lightmaps use the OpenGL-selected object/material texture and the object's separate LM-coordinate stream; explicit `eSrcPointer_TexLM` without a stream uses the same base-vertex X/Y fallback as `CREOcLeaf::mfGetPointer`. Ambient lightmap passes retain the OpenGL blend equation `base * (material/object ambient + LM * 4)`. DOT3 uses diffuse, bump, color LM and direction LM in the OpenGL sampler order, and samples both LM maps from the same UV set, with `max(dot(direction,bump),0) * LM.a + (1-LM.a)` and direction-map alpha scaling the baked diffuse. General `HasDOT3LM` passes can use the material direction map; `MultiLights` follows its stricter object-map condition. The low-quality CPU bake reads the original RGBA/RGB source bytes because NULL/Vulkan has no OpenGL texture readback. `DLF_LM` `MultiLights` specular draws sample the matching object occlusion-map RGBA channel from `m_OcclLights`. Vulkan increments `m_RendPass` before entering `DrawBuffer` (GL checks the initial pass before incrementing), so the synthesized first hardware lightmap pass accepts counter values 0 and 1. | Exact Cg variant behavior (HDR/HDR fake, offset bump, environment/gloss and all alpha modes) and headset visual parity remain incomplete. |
| OpenGL Matrix operations | Partial | Hardware-technique and pass-level `GL_TEXTURE` identity, translate, scale, rotate, and explicit matrix operations are parsed and composed after the resource UV matrix for all eight stock Vulkan texture stages. NV matrix registers and `LightCMProject` still request fallback; `Projected` and `Coords` remain parser no-ops as they are in the OpenGL compiler. |
| Lighting | Partial | Directional/point diffuse lighting and a bump-map path exist. Hardware `Light` / `DiffuseLight` passes select matching active directional, point, or projected lights individually, filter light-map-only passes against both light flags and receiver light-map data, and leave ambient in the technique's general pass. Enabled r_CullGeometryForLights leaf passes submit the per-light indices selected by CREOcLeaf::mfGetIndiciesForLight. Projected diffuse lights apply position/radius attenuation, an orientation/frustum-angle cone cutoff, and a sampled cubemap cookie in standard-normal and bump-map lighting; atlas orientation/filtering remains approximate. `MultiLights` follows OpenGL's receiver light-map suppression, per-light occlusion-list eligibility, and `LMF_HASAMBIENT` pass eligibility, including ambient-only draws when no dynamic light matches; it applies `LMF_DIVIDEAMB2/4` and `LMF_DIVIDEDIFF2/4` with OpenGL's override order. `DLF_LM` specular-only lights now retain their specular contribution in both `Light` and `MultiLights` translations, subject to `LMF_NOSPECULAR` and `LMF_NOADDSPECULAR`; `MultiLights` also multiplies specular by the matching R/G/B/A visibility channel from the object occlusion map. Ordinary light-pass groups also preserve OpenGL's `LMF_NOBUMP` sequence break per light. Ambient retains RGB material/object factors, including `LMF_NOAMBIENT`, `LMF_ONLYMATERIALAMBIENT`, and `FOB_IGNOREMATERIALAMBIENT`; `LMF_NOAMBIENT` suppresses the ambient term while retaining the baked lightmap on the base pass. `RBPF_DRAWNIGHTMAP` matches `SParamComp_AmbLightColor` by forcing white ambient before those factors and flags. `LMF_ONLYMATERIALAMBIENT` does not suppress direct diffuse light. The separate `eSrcPointer_TexLM` UV stream is now uploaded as Vulkan vertex binding 2 and routed to stock texture stages 0–7; stages 0 and 1 can select either base or lightmap UVs. Stage 0 lightmap UVs are routed through the secondary UV varying for stock texture shaders; the stage 0 bump-map combination still falls back, and runtime parity is not yet verified. Single-light passes apply parsed light-type, light-map, occlusion-map eligibility, `LMF_IGNOREPROJLIGHTS`, and `LMF_NOBUMP` filters. Accepted directional, point, and projected lights are still additive stock approximations. `SpecularLight` has a stock-normal half-vector approximation with material shininess and light specular tint. Programmable pass shaders, per-vertex varying point-light response, and exact attenuation/material combinations are not equivalent. |
| Fog | Partial | Per-fragment linear/exp/exp2 fog reconstructs radial eye distance from fragment depth, each eye's asymmetric OpenXR FOV, and viewport coordinates across every stock scene fragment shader. Vulkan enables it for NVIDIA devices to match OpenGL's `GL_EYE_RADIAL_NV` path and retains eye-space Z on other vendors. `SetFog` preserves OpenGL's effective `GL_EXP`/`GL_LINEAR`/`GL_EXP2` mode when the caller passes a mode that does not issue `glFogi`. Leaf draws also snapshot `CREOcLeaf::m_fFogScale` into their own fog start/end values, matching `CREOcLeaf::mfDraw`'s temporary GL fog-range override. Volumetric fog and GL's fog blend/pass composition are not complete. |
| Raster/depth/blend/stencil | Partial | Common depth, blend, cull, alpha-test and stencil state is decoded. The scene target preserves OpenGL's zero-alpha color clear for destination-alpha blend operations. The NULL-backed Vulkan state path resolves OpenGL per-flush inheritance masks for blend, depth write/test/function, stencil and alpha test before selecting an immutable pipeline. Parsed `SStencil` front/back compare and op bits, reference and mask now feed the Vulkan immutable/dynamic stencil state decoder; `SEfState` also applies stencil clears, fixed RGB/alpha, arbitrary color mask, polyline, cull and depth-test/function overrides. Vulkan now snapshots `EF_PreRender(3)`'s depth range per item list, including the 0..0.5 base range and portal partitioning; nearest objects still override it with OpenGL's 0..0.1 range. Shader/pass polygon-offset flags use the same `GL_OffsetFactor` / `GL_OffsetUnits` console variables and defaults as OpenGL. `GL_ClipPlanes` is translated through a per-draw object-space plane and fragment discard for the stock Vulkan shader family. Exact GL clip-plane behavior in programmable shaders, non-stock render elements, and exact per-flush reset/restore rules remain. |
| Programmable shaders | Missing | OpenGL's Cg/NVParse and hardware-program paths have no general shader translation/reflection system in Vulkan. The current shaders are hand-authored fixed-function translations. |
| Screen processing | Partial | Vulkan translates OpenGL's low-spec `CREScreenProcess` flash-bang countdown/brightness curve and timed color fade to stereo overlays, using separate additive and alpha-blend Vulkan pipelines. Cg-based screen capture, glare, blur, cartoon, motion blur, cryvision/heatvision and depth-of-field chains still need per-eye offscreen targets and shader ports. |

## Textures and render elements

| Subsystem | Status | Current Vulkan behavior / remaining work |
|---|---|---|
| 2D texture upload | Partial | Common base/bump image formats are converted to RGBA and registered. `eTF_RGBA` now follows OpenGL's actual `GL_BGRA_EXT` upload order, including object lightmap and direction-map uploads/updates. Signed HILO/V8U8 maps are decoded as tangent-space normals for the stock bump shader; signed HILO reconstructs positive Z because that shader consumes RGB normals. DXT1/3/5 and supported uncompressed DDS formats now preserve supplied mip levels instead of regenerating the chain from mip 0. Nearest, linear, bilinear, trilinear and anisotropic filtering are represented; Vulkan enables anisotropy only when the runtime device supports it and clamps it to `r_texture_anisotropic_level`. The `DownLoadToVideoMemory` adapter maps OpenGL `FILTER_NONE` to nearest and follows `nummipmap == 0` base-level/autogenerated-mipmap behavior for bilinear/trilinear filters. `SetTexClampMode` snapshots repeat/clamp sampler selection per queued scene draw without mutating earlier draws. Per-stage shader LOD bias now scales explicit UV gradients. Missing base or auxiliary texture mirrors now preserve geometry through an untextured/base-stage fallback and increment `textureFallbacks`; other authored-mip formats and sampler refresh after runtime cvar updates remain open. |
| Cubemaps and render targets | Partial | Vulkan gathers six loaded cubemap face images and mirrors a 3×2 RGBA atlas under the positive-X face's texture ID. Projected-light stock shaders sample that atlas using object-space light basis and frustum scale; ordinary material `samplerCube` lookup is not wired. OpenGL render targets, p-buffers, dynamic render targets, and render-to-texture effects are not ported. |
| Sky, shadows, sprites, particles | Partial | `CREOcLeaf`, transient meshes, a basic stencil shadow-volume path, `CREPolyBlend`, and textured `CREGlare` are handled. Terrain sector base-cover UVs and per-stage detail projections come from the sector's OpenGL custom texgen payload; the base pass now uses the vertex alpha as the grayscale terrain-lighting weight (`CGVProgTerrain` writes `IN.Color.www`) and multiplies the engine world color. The P3F low-resolution terrain path also carries world color. Vulkan keeps packed terrain surface masks and evaluates texture stages against their own object-space projection. The same base-cover coordinates are supplied to terrain dynamic-light and fog draws. Terrain-sort water draws with a real UV stream and first texture now use the per-eye scene-color refraction shader. Multi-layer terrain blends, exact terrain DOT3 lighting and shadows, terrain fog composition, OpenGL water bump/flow/reflection parameters, and color-only ocean strips still need ports. `CREFlare` builds Vulkan triangle-fan coronas and textured lens-flare quads, translates its fixed-function hardware layer through the stock stage path, and records separate occlusion and full-coverage queries. Its visibility fraction is thresholded at 0.05 (sun rays floor to 0.75) and fed through the same eight-value history used by OpenGL; remaining differences include a rasterized query-area denominator instead of OpenGL's projected-area computation, left-eye visibility driving both eyes, and Vulkan sampling every rendered frame instead of `m_fLightFadeTime`. For sun `CREFlareGeom`, Vulkan records an occlusion query around its indexed quad draw and uses the previous frame's sample result to reproduce OpenGL's `CV_r_coronafade` alpha transition. Sun-ray rendering and custom flare shader programs are not translated. `CREFlashBang` advances its OpenGL fade timer and queues its active pass texture as an 800x600 stereo overlay. `CREParticleSpray` emits Vulkan billboard/streak geometry for Poly, PolySegs, and Beam particle lists. OpenGL's Fur/SimulatedFur dispatcher is currently a stub that submits no geometry; Vulkan now preserves that no-op rather than forcing a panel fallback. Point/line particle systems, sky environment, object sprites, and specialized shadow paths still need individual audits/ports. Glare and flash-bang overlays are composited through the XR panel-image path, so exact OpenGL depth/blend ordering is not yet matched. |
| 2D/UI | Partial | The XR panel path renders UI images, text, timed fade and flash-bang overlays. Screen-space `DrawDynVB` batches, `DrawTriStrip`, `Draw2dImage` and glyph quads pass the exact current OpenGL blend-factor pair to Vulkan. Opaque, alpha and additive pipelines are prebuilt; other valid legacy factor pairs are created lazily and cached for both image and indexed-geometry draws. Each queued panel item snapshots the active OpenGL scissor rectangle and applies it when recorded. Geometry, texture coordinates, vertex tint, and OpenGL translate/rotate/scale matrix-stack transforms remain in submission order. Other 2D state and the rest of the OpenGL 2D renderer are still incomplete; temporal post-process inputs remain unavailable. |
| Debug/auxiliary drawing | Missing/partial | Lines, debug primitives, auxiliary geometry, and other CNULL inherited no-ops need Vulkan implementations where used by the game. |

## Work completed in this iteration

- Added lit and no-color vertex variants for multi-texture scene draws and expanded the dynamic uniform range so their lighting constants are visible to all descriptor paths.
- Preserved the exact legacy camera matrix bytes OpenGL reads back in `EF_SetCameraInfo`, removing a Vulkan-only transpose that changed the whole-world view transform.
- Added the missing `ADDSIGNED2X` texture-combine operation for the two translated texture stages.
- Added translation for common `SShaderPass` RGB/alpha generators and passed generated primary color/masks to the scene fragment shaders.
- Implemented persistent immediate texture IDs and color/alpha combine state for the two supported stages.
- Applied the common `SEfState` overrides in Vulkan: stencil clear/setup, fixed RGB/alpha, arbitrary color-channel mask, polyline, cull selection, and depth-test/function override.
- Added OpenGL-formula CPU UV generation for quad, environment, projection, sphere-map, and sphere-map-environment modes on stock indexed meshes. Environment mapping and projection read the undeformed source positions selected by OpenGL's `FGP_SRC` path; environment mapping uses the same object-translation-minus-position vector as `SEvalFuncs_RE::ETC_Environment`.
- Added OpenGL `Normal Custom` generation to the private stock vertex stream before deformation and texture-coordinate generation, matching `EF_EvalNormalsRB` and feeding the generated normal into Vulkan lighting paths.
- Added CPU translations for the common wave, vertical-wave, bulge, squeeze, and from-center vertex deforms before UV generation. `Wave`, `Squeeze`, and `Bulge` now read leaf meshes' source `SPipTangents::m_TNormal`, matching OpenGL's `FGP_SRC` tangent-normal stream when it exists. `Flare` now accepts preceding source-space deforms and builds from their result; geometry-changing stages after the generated flare still require fallback. Routed `CREBeam` through its OpenGL model/material prepare state into the stock indexed Vulkan path, making the existing beam vertex-deform translation reachable.
- Added Vulkan anisotropic sampler support gated by the physical-device feature and the OpenGL anisotropy level cvar.
- Passed each shader texture unit's OpenGL LOD bias into Vulkan scene sampling with bias-scaled texture gradients.
- Matched OpenGL's hardware-technique priority, populated active dynamic lights and current opacity before technique selection, and submitted general `CREOcLeaf` hardware passes through the Vulkan stock path.
- Added object-transition filtering and the stock character skinning update before scene submission.
- Applied render-element-local `EF_SetState`, cull-mode, and texture-combine changes to Vulkan submissions.
- Extended render-element-local `SetColorOp` overrides through TMU 7, so OpenGL pass-time color/alpha operations and operands reach every translated Vulkan texture stage.
- Ran `CREOcLeaf::mfCheckUpdate` for fixed-function leaf draws using OpenGL's resource-driven normal-update flag.
- Routed both 3D `DrawDynVB` overloads to Vulkan client-indexed draws while keeping the screen-space panel/font paths separate.
- Kept indexed scene geometry visible when base or auxiliary Vulkan texture mirrors are unavailable, with a per-frame `textureFallbacks` diagnostic counter instead of rejecting the draw.
- Added a fixed-function visibility fallback for selected hardware techniques that contain no `General` pass, preventing those objects from being discarded when the shader retains stock fixed-function passes.
- Added a third stock texture-combine stage for materials using either existing UV set, with its own sampler, combine operands, constant color, and LOD bias.
- Added a fourth stock texture-combine stage end to end, including pass-unit extraction, per-stage sampler and LOD bias, combine specialization constants, and a SPIR-V shader variant. It uses one of the existing UV sets and requires a matching UV transform.
- Added fifth through eighth stock texture-combine stages with independent combine arguments/constants, descriptor sets and UV-set selection; moved clip-plane equations into the fragment push constants so eight texture descriptor sets fit the scene pipeline layout. Stages 5–8 currently have no individual LOD bias.
- Added per-light dispatch for hardware `Light` / `DiffuseLight` passes and prevented those techniques' general pass from double-counting the same dynamic lights. Single-light passes now also honor `LMF_IGNOREPROJLIGHTS`, matching OpenGL light-pass rejection.
- Matched OpenGL's `DLF_LM` diffuse-light rejection scope: it is an early per-light check keyed to the first light pass and only checks `LIGHTMAP_DIR` or the object's LM ID. Vulkan no longer suppresses every later diffuse pass merely because any lightmap resource exists; the ordinary and `MultiShadows` paths share the same filter.
- Preserved every nonzero light specular color in ordinary `SpecularLight` and translated `Light` specular contributions; the 0.01 cutoff remains only on OpenGL's light-map-only pass filter.
- Restricted dynamic-light translation for parsed hardware techniques to OpenGL's `Light`, `DiffuseLight`, `SpecularLight`, and `MultiLights` pass types. `General` and `Shadow` passes no longer receive Vulkan-only dynamic-light contributions; fixed-function passes retain their separate stock-light translation.
- Kept dynamic-light submissions for position/UV-only mesh formats; the scene fragment shader already reconstructs their face normal from screen-space derivatives, so the CPU light-list builder no longer drops their light terms for lacking a normal attribute.
- Honored `LMF_IGNORELIGHTS` on hardware `Light`, `DiffuseLight`, and `SpecularLight` translations, matching OpenGL's `SLightMaterial::mfApply` / `EF_LightMaterial` path that skips `EF_SetLights`; the separate `MultiLights` PS30 translation remains independent.
- Split `MultiLights` ambient+baked-lightmap output into its own first draw before additive direct-light draws. This preserves ambient when the first light is projected and needs the material-ambient channels for its projector direction.
- Matched OpenGL DOT3 ambient eligibility by pass type: the `MultiLights` path requires active shader resources and a nonzero object `m_nLMDirId`; a `General` `HasDOT3LM` technique follows technique selection and accepts either the object's direction-map ID or a material `EFTT_LIGHTMAP_DIR` resource.
- Kept `HASAMBIENT` `MultiLights` passes eligible under `LMF_NOAMBIENT`, matching OpenGL's pass traversal so the baked lightmap still renders; when filtering leaves no direct light, the fallback draw now contributes no fabricated sun diffuse term.
- Capped ordinary fixed-function and per-light hardware lighting at OpenGL's eight `EF_SetLights` slots; the independent `MultiLights` PS30 path retains its separate multi-pass light list.
- Matched the fixed-function no-light fallback: materials with `EF_NEEDNORMALS` keep OpenGL's object/material ambient color when no Vulkan light survives filtering, with direct diffuse set to zero.
- Added stock fixed-function light and material specular colors plus `GL_SHININESS` to the Vulkan lighting payload, using OpenGL's default infinite-viewer direction.
- Moved fixed-function diffuse and specular evaluation to the vertex stage for normal-bearing stock shaders, matching OpenGL's smooth per-vertex interpolation instead of fragment evaluation. Specular is now carried separately and added after texture stages, matching the renderer's `GL_SEPARATE_SPECULAR_COLOR` state. Shader-based light passes retain their existing fragment lighting path.
- Added a Vulkan `MultiLights` approximation using additive per-light submissions with the OpenGL pass's light-type, LM, occlusion-map, and bump filters.
- Preserved direct fixed-function texture bindings and combine state for Vulkan TMU stages 2 and 3 on render elements that draw without an `SShaderPass`.
- Added a Blinn-Phong-style half-vector approximation for hardware `SpecularLight` passes and the combined specular term in `Light`/`MultiLights` translations, including material shininess, specular light tint, point-light attenuation at the object origin, and the bump-map normal path.
- Matched OpenGL's persistent per-TMU color/alpha combine modes and effective source arguments for `255` unchanged markers and explicit `MULTIPLYADD` / `BUMPENVMAP` / alpha-blend no-ops; `eCA_Specular` keeps the existing operand source as the GL implementation does.
- Preserved independent OpenGL LOD bias for texture stages 5–8 through Vulkan pipeline specialization and explicit-gradient sampling.
- Added per-draw dynamic uniform transforms for stages 3–8 so animated texture matrices apply independently while texture units share either stock UV set.
- Matched OpenGL resource-state suppression and the two-sided-material cull override for stock Vulkan geometry.
- Added periodic Vulkan scene diagnostics separating queued draws, unsupported inputs/features, missing textures, unsupported texture combines/states, pipeline creation failures, and per-eye record skips such as invalid XR transforms or empty scissors.
- Translated OpenGL shader-level `EF_POLYGONOFFSET` and hardware-pass `LMF_POLYOFFSET` flags to Vulkan raster depth bias, sharing the `GL_OffsetFactor` and `GL_OffsetUnits` console values with defaults of -1 and -4.
- Translated `EF_SetClipPlane` state to per-draw object-space plane equations and fragment clipping for the stock Vulkan shader family, honoring `GL_ClipPlanes` and preserving clip state across `CRESky` disable/re-enable transitions.
- Captured OpenGL `SetViewport` changes in each queued Vulkan scene draw and scaled the logical viewport to the XR eye target.
- Added Vulkan cubemap face collection, 3×2 atlas mirroring, object-space projector basis transfer, and projected-cookie atlas sampling in standard-normal and bump-map stock light shaders. Ordinary material cubemap sampling and exact OpenGL face orientation/filtering remain pending.
- Fixed mixed-format client geometry addressing: the shared Vulkan vertex buffer now binds each draw at its byte offset with base vertex zero, keeping positions and independently bound tangent streams indexed consistently.
- Mirrored the CPU-visible `EF_PreRender(1)` camera/frame state in Vulkan for particles, billboards, and other render elements that read `m_ViewOrg`, `m_CamVecs`, frame markers, or `m_SunDir`; also added the `RBF_3D` list scope, object transition markers, per-list state restoration, GL-compatible projection/model-view getters, and combined/inverse camera-projection matrices.
- Ported the non-batching `CREOcLeaf::mfPrepare` work used by ordinary scene items: clip-plane AABB rejection, secondary vertex stream refresh for modified leaves, and the prepared chunk range metadata.
- Added a `CREPolyMesh` Vulkan submission adapter that validates its index bounds and packs source positions, plane normals, stock white colors, and both UV channels into the selected Vulkan vertex layout before routing the indexed triangles through `DrawBuffer`.
- Added a `CREClientPoly` adapter that copies its local position/color/UV data, validates byte indices, expands them to 16-bit indices, and submits its triangle list through Vulkan `DrawBuffer`.
- Added `CREPolyBlend` and `CREAnimPolyBlend` billboard generation for their oriented quad path, including camera-facing orientations, mirror handling, distance/explicit scaling, generated UVs/normals, light-style or decay color, and world-space submission without reapplying the object transform.
- Added a Vulkan `CREGlare` path that resolves the active shader pass texture and queues its stock 800x600 textured quad as a stereo panel image with resource opacity and material tint.
- Added the stock `CREFlashBang` fade timer update and routed its active pass texture into the same stereo overlay path as OpenGL's 800x600 flash quad.
- Added OpenGL low-spec `CREScreenProcess` flash-bang and fade overlays, including separate additive/alpha Vulkan panel pipelines, frame-time countdowns, cubic flash brightness, fade delay/duration, fade color and `r_fadeamount` update.
- Ported OpenGL `eDT_Beam` vertex remapping, interpolated radius/length, start/end color gradient and view-angle alpha fade onto the private Vulkan upload copy for ordinary indexed `DrawBuffer` submissions.
- Ported OpenGL `eDT_Flare` plane-facing test, style/material intensity, corner expansion, ray clipping, and 16-vertex/54-index mesh generation for leaf draws with supported deforms before the flare. Geometry-changing deforms after the flare and lightmap UV flares request fallback.
- Ported `CREFlare` corona fans and lens-flare quads through Vulkan world draws, including camera-facing placement, brightness, texture, additive blend, occlusion and full-coverage queries, OpenGL's 0.05 intensity cutoff / 0.75 sun-ray floor, and the eight-value visibility history. Projected-area math, per-eye visibility, and `m_fLightFadeTime` query cadence still differ. Sun rays and custom hardware flare passes remain unsupported.
- Added an indexed XR panel pipeline for screen-space triangle batches so non-rectangular `DrawDynVB` geometry is preserved with per-vertex color and UV data instead of being rejected unless it forms axis-aligned quads.
- Passed the exact OpenGL `GS_BLEND_MASK` factors to XR panel images and indexed UI geometry, with common opaque/alpha/additive pipelines and lazy cached Vulkan pipelines for other valid factor pairs.
- Snapshotted the active OpenGL scissor rectangle per queued XR panel item and applied it while recording UI and overlay draws.
- Added `CREParticleSpray` geometry submission for its Poly, PolySegs, and Beam types, including camera-facing quads, legacy segment stepping, particle color/alpha, UVs, sparks, and 16-bit-safe batches.
- Added an OpenGL-compatible 2D matrix stack for Vulkan UI submission, including nested `Set2DMode`, push/pop, matrix load/multiply, translate, rotate, and scale; screen-space `DrawTriStrip` and dynamic batches now use the XR panel path.
- Translated `SetTexClampMode` into per-draw repeat/clamp sampler variants for all eight fixed-function texture stages, preserving queued draw state with immutable descriptor sets.
- Matched the fixed-function color and alpha combiner dispatch to `CGLRenderer::EF_SetColorOp`, removing D3D-style interpretations of legacy operations that OpenGL treats as `MODULATE` or leaves unchanged.
- Enabled common material-script discovery for the Vulkan renderer while leaving the pure NULL renderer's shader-free initialization intact. Vulkan advertises its translated high-end material capabilities and basic `RFT_DEPTHMAPS` path so the engine selects NV4X/PS3-era definitions and retains depth-map shader branches. Self-shadow capability remains disabled. The stock material translator can now receive parsed OpenGL-compatible pass/texture/state descriptions; Cg/NVParse execution is still a separate missing layer.
- Replaced the NULL renderer's empty hardware-technique stubs with parsing for technique conditions, fixed-function `Layer` state, culling, render state, pass kinds, light filters, and array-pointer declarations. This makes the existing Vulkan General/Light/MultiLights translations reachable from loaded hardware shader scripts; Cg program compilation and custom shader execution remain unimplemented.
- Ported OpenGL hardware-technique and pass-level `Matrix` block parsing to the NULL-backed Vulkan path and applied supported GL texture-matrix operations to stock UV matrices after animated texture-resource transforms. Unsupported NV matrix registers and light-dependent `LightCMProject` operations are flagged for fallback.
- Matched OpenGL's per-flush render-state inheritance when a shader pass leaves blend, depth, stencil, or alpha-test state under the surrounding flush's control; Vulkan now queues the resolved legacy state used to key its immutable pipelines.
- Applied OpenGL `LMF_DIVIDEAMB2/4` and `LMF_DIVIDEDIFF2/4` scaling to translated hardware lighting passes, including the source's `DIVIDE2` override when both flags are set.
- Added OpenGL `MultiLights` ambient-pass eligibility, including zero-direct-light ambient draws and the `LMF_HASDOT3LM` prerequisite; passed RGB material/object ambient factors through unused Vulkan texture-transform components for textured and untextured lit shader variants.
- Ported OpenGL's separate per-object `eSrcPointer_TexLM` stream into a third Vulkan vertex binding; hardware lightmap UVs now feed stock secondary texture stages, including selectable base/lightmap UV sources for texture stages 0 and 1.
- Preserved OpenGL's terrain-shadow caster rule: `eS_TerrainShadowPass` does not skip the first caster as a self-shadow, in either the direct shadow pass or the `MultiShadows` pass group.
- Matched the OpenGL `Terrain` vertex-color channel contract from `CGVProgTerrain`: sector terrain lighting comes from alpha replicated into RGB, multiplied by `m_WorldColor`; low-resolution P3F terrain now carries the same world-color modulation.
- Ported `TerrainDetailLayers` projection blocks at `FromRE[28..58]`, its camera-distance fade on secondary-color layer masks, and the one-to-four-layer multiplicative detail shader used before the OpenGL `DST_COLOR, SRC_COLOR` blend.

- Parsed OpenGL `HW_ObjectLinear` and `HW_EyeLinear` texture generators in the NULL-backed Vulkan shader loader and evaluated S/T/Q plane equations on the private vertex copy. Q is applied with a homogeneous divide before the Vulkan texture matrix, and eye-linear coordinates use the current object-to-eye transform. R is unused by the currently supported 2D sampler path.
- Parsed `HW_NormalMap`, `HW_ReflectionMap`, and `HW_SphereMap` generators and translated the OpenGL eye-space normal/reflection and sphere-map equations for Vulkan's S/T channels. Cube-map sampling and `HW_EmbossMap` previous-stage/light inputs remain separate unsupported paths.
- Added the missing NULL/Vulkan `HW_EmbossMap` parser, stable generator type, and copy operation. Audited `GL_NV_texgen_emboss`: OpenGL sets S/T/R but leaves Q outside emboss mode, while the extension requires all four coordinates enabled in emboss mode for defined output. The Vulkan path therefore keeps fallback instead of inventing a supposedly matching formula for an undefined OpenGL result. Extension specification: https://registry.khronos.org/OpenGL/extensions/NV/NV_texgen_emboss.txt.

- Added OpenGL `eERGB_OneMinusFromClient` behavior to the stock fragment shaders using a per-draw flag; RGB is inverted per vertex while client alpha is preserved.

- Ported OpenGL current global color to Vulkan texture-environment constants across stages 0–7, preserving state when a pass uses vertex/client color and updating byte channels when its RGB/alpha generator installs global color.

- Matched GL texture-combine argument state more closely: texture units start with GL's `TEXTURE/PREVIOUS/CONSTANT` sources, diffuse/texture-alpha blend operations install the matching source 2, and `eCA_Specular` keeps that installed source. Interpolation reads the effective source-2 alpha in every stock fragment variant.

- Added per-eye sampled color targets and a fullscreen output pass, keeping the OpenXR swapchain at color-attachment-only usage while establishing a Vulkan-owned source for future render-to-texture/preprocess effects.

- Corrected texture-stage disable semantics: OpenGL `eCO_DISABLE` maps to `GL_REPLACE`, so Vulkan now keeps stages 1–8 active and samples their texture/arg0 instead of skipping the stage or substituting PREVIOUS. Normal-map bindings remain on the dedicated bump path.

- Matched OpenGL radial fog distance in all stock scene fragment variants by reconstructing the per-pixel view ray from that eye's asymmetric OpenXR FOV and the linearized depth. Each eye gets an independent aligned texture-transform UBO slot, including untextured draws.

- Made `XRenderNULL` export the shared functions and data used by `XRenderVulkan`, replaced the OpenGL-only `SStencil::mfSet` call with the Vulkan pipeline's existing FSS decoder, and used Win32 loader APIs for OpenXR/Vulkan DLLs so PCVR no longer needs SDL3 binaries. The Vulkan DLL now links; the SDL dependency remains for Android OpenXR platform integration.

The Vulkan/OpenGL parity goal remains open until the missing systems above are implemented and validated against the OpenGL renderer in-game.
### Vulkan lightmap path

- Dynamic-light passes now run alongside material/base passes and baked-lightmap sampling. Their stock Vulkan translation covers the common directional, point, projected, and `DLF_LM` specular cases; exact programmable-light behavior and headset parity remain to be checked against OpenGL.
- Each translated hardware `Light` / `DiffuseLight` / `SpecularLight` draw now uses additive RGB blending and disables depth writes. These passes are submitted individually, so treating their first Vulkan draw as an opaque base draw replaced the preceding baked-lightmap result.
- Baked lightmap stages 2 and 3 can use the base UV stream when the selected pass has no separate lightmap coordinate buffer; these stages no longer reject otherwise valid stock vertex formats in that case. An explicit `eSrcPointer_TexLM` without a separate buffer instead reads vertex position X/Y, matching `CREOcLeaf::mfGetPointer` in OpenGL.
- Low-quality lightmap baking now reads the original color and direction bytes that were uploaded to the renderer. This preserves OpenGL's CPU bake formula and works with Vulkan, whose `STexPic::GetData32()` has no texture readback implementation. The temporary source images are released through the renderer so their Vulkan mirrors are retired, and the baked CPU image is freed after upload.
- The base-UV fallback also applies to stock texture stages 4–7. Directional lightmap specialization now requires the bump, color-lightmap, and direction-map stages to be available together, and its pipeline cache key uses the same condition.
- Object lightmap IDs replace only the OpenGL `$Lightmap` / `$LightmapDirection` templates or an unbound explicit lightmap-coordinate stage. Other explicit resource binds on that UV stage, including the HDR lightmap resource slot, now keep their own texture ID.
- `EFTT_LIGHTMAP`, `EFTT_LIGHTMAP_DIR` and `EFTT_OCCLUSION` now select the lightmap UV source on every supported fixed-function texture stage, matching their role as baked-map samplers.
- All stock texture vertex variants now forward raw UV sets. The fragment-stage transform path selects the stage's UBO S/T/Q matrix for both ordinary and terrain-generated coordinates, performs the homogeneous Q divide, and handles stages 0–7 uniformly. This matches OpenGL when stages select either UV set and avoids per-vertex stage-0/1 assumptions. The same transformed stage-1 coordinate feeds specular-occlusion lighting.
- `MultiLights` occlusion passes now require the same condition as OpenGL's `bUseOccl`: an object-owned lightmap, a `DLF_LM` light, and that light's encoded ID in one of the object's four occlusion slots.
- `DLF_LM` specular draws carry the matching `m_OcclLights` channel into the fragment shader and multiply specular by that RGBA occlusion value. The `eTF_4444` decoder now restores the same R/G/B/A nibble order and normalized channel values as OpenGL's `GL_BGRA` to `GL_RGBA4` conversion.
- Ported the baked-lightmap equations and sampler roles represented by OpenGL's `HasLM` / `HasDOT3LM` technique path. The non-DOT3 and DOT3 ambient terms use the translated pass ambient RGB, and the DOT3 diffuse term includes the direction map alpha multiplier before stock lightmap encoding.
- `AmbPassTempl` is an ordinary OpenGL `General` pass selected by the technique's `HasLM` or `HasDOT3LM` condition. Vulkan now recognizes those technique conditions for its baked-lightmap base pass instead of requiring `LMF_HASAMBIENT`, which OpenGL uses for `MultiLights` passes.
- In the DOT3 ambient pass, the bump image occupies the secondary sampler as a normal texture input, not Vulkan's tangent-basis normal-map route. The DOT3 specialization now rejects incomplete base/bump/color/direction texture sets and only enables baked-lightmap math when the requested variant is fully bound.
- Exact `CGRCAmbientTempl` parity still needs its optional environment cubemap/gloss, offset bump, HDR lightmap, and alpha-output variants. Those combinations are selected by shader macros in Far Cry's Cg script and are outside the current stock 2D fragment shader path.

### Native packaging correction (2026-09-29)

`CryVR` is a static library linked into both `CrySystem` and `XRenderVulkan`.
`CSystem` owns the frame renderer and supplies its address to `XRenderVulkan`.
Updating only `libXRenderVulkan.so` leaves an old frame renderer and embedded
SPIR-V in the APK. The staged `libCrySystem.so` was from 21:30, while the native
build containing the UV and UBO corrections was from 23:28. Its exported Vulkan
frame-renderer symbols were confirmed with `llvm-nm`.

Gradle now copies the complete available `bin/arm64-Release/*.so` output before
packaging. Build native sources first; Gradle still does not build the engine.
The rebuilt APK was installed on Quest 3, with matching native/staged hashes,
matching stripped/APK library hashes for both modules, and a matching hash of
the APK pulled back from the device. The visual outcome awaits a user launch;
the packaging correction alone does not establish that the stripes are gone.

### Bounded lightmap investigation and detail overlay corrections

- Detail overlays now inspect the final submitted vertex format. Hardware P3F meshes can become P3F_TEX2F before submission; reading those records with the former stride misread the fog attenuation coordinates.
- `CGRCDetailAtten` mixes the original detail RGB toward 0.5 using the fog texture alpha. The Vulkan stage now replaces RGB with the detail texel before interpolation; multiplying it by 0.5 incorrectly darkened each multiplicative overlay layer.
- Separate lightmap streams are validated against the indices of the current chunk, rather than requiring coordinates for every unused vertex in the main VBO. Existing short streams no longer silently fall through to position X/Y as atlas coordinates.
- Android saves at most one frame of opaque directional lightmap inputs (16 draws, less than 4 MiB) to `/sdcard/FarCry/vulkan_lightmap_probe.bin`. This captures source texture names, final vertex records, exact indexed UVs, material parameters and texture matrices without enabling the continuous audit. The snapshot is for investigation; the remaining striped lighting has not been visually confirmed fixed.
- Inspect a pulled snapshot with `python buildscripts/analyze-vulkan-lightmap-probe.py snapshot.bin --pak levellm.pak`. The script compares captured UV bytes to original level UV sets, checks their selected atlas, and reports triangle spans and black atlas samples. A black sample alone does not establish a rendering error.
# Confirmed mesh/lightmap vertex order mismatch (Quest capture)

## LDR lightmap brightness correction

## Follow-up lighting and decal state corrections

- OpenGL `SShaderTexUnit::mfSetTexture` resolves the material image through
  `pSTU`, but takes combiner operations/arguments from the original pass.
  Vulkan now does the same for all eight texture stages.
- Baked-lightmap fragment branches already add evaluated material ambient.
  Disable generic primary-color lighting for that first baked draw, avoiding
  a second multiplication by ambient.
- MultiLights `LMF_HASAMBIENT` must retain ambient on objects without baked
  lightmaps too; it is no longer dependent on lightmap base-pass eligibility.
- Install/reset `m_pCurLightMaterial` per draw following `EF_FlushHW` before
  evaluating color/alpha shader components.
- Bounded diagnostics: `vulkan_decal_probe.txt` records at most 32 distinct
  decal states; `vulkan_lighting_probe.txt` records at most 256 draw summaries
  sampled every 600 frames in baked/unbaked categories. No continuous audit
  or full texture hashing is enabled. Headset confirmation remains pending.

The original `CGVProgramms.csl` CommonSubroutines implementation of
`HDREncodeLM` returns `a * 4` unless both `_HDR` and `_HDR_FAKE` are defined.
The latter branch returns `a * HDR_OVERBRIGHT * 2` (16), not 2.
The Vulkan LDR scene previously selected 2 merely because `r_HDRFake` is
enabled by default. It now uses 4, matching the ordinary OpenGL pass.
`HDREncodeAmb` remains unscaled in that same LDR variant.

The bounded capture contains 16 opaque directional-lightmap draws. Their raw
lightmap UV bytes match the original Training `dot3lm.dat` records exactly,
and the selected color/direction atlases match those records. However,
`Common/pip_addons.cpp::CompactBuffer` was rewritten to emit vertices sorted
by position X, including `evs_NoSharing` meshes. The original implementation
emitted vertices in first-occurrence order. Separately loaded lightmap UVs
therefore referred to different vertices despite matching counts and valid UVs.

Using the captured index slices, current triangle UV spans have medians of
17–293 atlas pixels. Restoring the original first-occurrence order gives
medians of 0–6.74 pixels across all 16 draws (maximum span 16.99 pixels).
The fix retains sorted references for duplicate lookup but emits the final
vertex/tangent buffers and indices in first-occurrence order. Material ID
masking also follows the original 255 mask rather than truncating to 127.
This shared source is rebuilt into OpenGL and NULL, with Vulkan relinked
against NULL. Visual confirmation on the headset remains required.


## Model ambient and secondary normals (2026-09-30)

The skeleton draw capture identifies TemplBumpSpec and TemplBumpDiffuse. Their
General passes had no baked map and no translated Ambient parameter, leaving
unlit albedo before additive light passes. AmbPass_VP and AmbPassTempl explicitly
supply CGPSParam Ambient, including in techniques with no dynamic-light pass.
The NULL parser now retains pixel parameters after the CG declaration while
preserving the existing non-decal sampler/state fallback. Vulkan evaluates the
actual Ambient parameter for General passes instead of guessing from Light
passes or object lightmap presence.

OcLeaf lit submissions now copy the original secondary-stream normals into a
normal-bearing Vulkan layout, preserving position, UV and optional color. This
avoids replacing smooth model normals with fragment-derived triangle normals.
The angular attenuation belongs only to TemplMuzzleFlash_Auto; the _FP variant
uses texture replacement in the original Templates.csl and has no normal input.

The updated APK builds and installs successfully. Bone/ray visual parity remains
pending a new headset run. Ray pass capture already confirms ONE/ONE blending,
no baked map and no stock lighting; decoded texture snapshots are bounded to
one upload per reported texture. Full parity of all CG programs is not claimed.


## Programmable light audit (2026-09-30)

Compared CGVPMacro.csi, CGVProgLightTempl.crycg, CGRCLightTempl.crycg,
CGRCAmbientTempl.crycg and Templates.csl with the stock Vulkan GLSL paths.
Fixed differences:

- Programmable point attenuation follows saturate(1 - normalizedDistance^2)
  from PROC_ATTENPIX and the attenuation-map fragment expression. The vertex
  projector path follows linear PROC_ATTENVERT. Fixed GL light attenuation
  remains reciprocal constant/linear attenuation in its separate mode.
- Dynamic model passes resolve EFTT_BUMP, its sampler transform and tangent
  basis instead of using only diffuse albedo when the parsed units are empty.
- Specular half vectors and distance attenuation are evaluated at each surface
  position. Previously the object origin supplied one half vector and one
  attenuation for every vertex; an origin outside the light radius also
  discarded the whole object's specular pass.
- A NULL loaded flag is no longer sufficient proof of a Vulkan image. A
  missing file-backed base image gets one explicit reload with its original
  texture ID, avoiding the untextured fallback when an upload was missed.
- Safe normalization prevents zero-length normal/light/view vectors from
  producing NaNs. Fixed-viewer payload slots cannot masquerade as a projector
  cookie flag in programmable shaders.

Shader compilation, native relinking and APK installation succeeded. New
headset captures are needed to confirm the missing-image diagnosis for rays
and visual changes to bones. Remaining differences include per-pixel specular
power/gloss variants, environment reflections, exact fog-volume sampling and
other specialized CG programs; a complete port of every program is not claimed.

## Missing material image capture (2026-09-30)

The Quest capture reports gpuTexture=0 for both sun_rays_textureB.dds and
skeleton.dds. Their material passes therefore have no resident base image.
RegisterLegacyRgbaTexture silently rejected uploads beyond a shared 192 MiB
limit; Vulkan expands compressed DDS images to RGBA, increasing residency.
Removed that artificial limit from material uploads and shadow targets.
Actual allocation/upload and descriptor failures now record an error, and the
bounded ray/bone capture includes residentBytes and uploadError.

The APK built and installed successfully. A new headset run is required to
confirm texture residency and visual results; the old capture does not establish
that budget saturation was the only cause of missing images.

## First light and surface ordering (2026-09-30)

The user confirmed skeleton and ray textures now render correctly. Remaining
reports concern surfaces covering rays from behind, unlit translucent moss and
missing ceiling models in Training.

Compared EF_DrawLightPasses/EF_DrawGeneralPasses and GLShaders.cpp with Vulkan:

- Retain the material's first light pass blend/depth-write state. Vulkan used
  to force every translated material light draw to additive/no-depth-write,
  including techniques with no preceding General pass.
- Select first/second state from the number of executed passes, matching GL,
  instead of the technique array index or repeatedly treating light zero as
  a first pass.
- Preserve evaluated CG Ambient for the first combined diffuse/ambient pass;
  it was previously cleared from all non-MultiLights light passes.

An initial build also parsed state/light metadata after CG declarations and
installed GL's template second-state defaults. The user reported darker
skeletons with no improvement to other symptoms. The GPU capture still shows
resident skeleton images. Reverted those two parser changes to the previously
working fallback; the first-light state/ambient fixes remain for evaluation.

Native compilation and APK build/install succeeded. A bounded 192-record
vulkan_surface_probe.txt captures ceiling/moss/ray material selection, buffer
preparation and queue failures. The missing ceiling cause and headset visual
results remain pending the next run; no visibility fix is claimed yet.
StatObj buffers have generic source names, so the capture identifies ceiling
and moss candidates through their original material texture paths as well.

## Material/program variant audit (2026-09-30)

The audit now preserves the original CG vertex/fragment program names and
expanded preprocessor mask in each hardware pass. These fields are plain
arrays/scalars because the legacy pass array allocates entries with memset.
Known CGRCAmbientTempl variants use TEMP_LM/TEMP_DOT3LM/BUMP_MAP instead of
inferring the sampled lightmap program from object texture IDs alone.

Compared original program expressions and GL state setters; corrected:

- Regular lightmap RGB does not multiply the material alpha. Ambient/light
  template alpha now follows DIFFUSEALPHA, ALPHAGLOW and GLOSS_DIFFUSEALPHA.
- Vertex-program Ambient parameters are retained/evaluated alongside pixel
  parameters. CGRCPlants uses vertex RGB times Ambient and non-HDR x2;
  its layer, alpha-test and blend declarations are parsed completely.
- CGRCTerrain and its 1/2/3-layer variants preserve source lighting alpha,
  x2 encoding and distance-weighted detail blending toward neutral 0.5.
  The camera transform is calculated once per converted terrain buffer.
- SetMaterialColor installs the GL-equivalent global combiner constant and
  arguments. SelectTMU now changes CTexMan::m_CurStage rather than inheriting
  the empty NULL method. Direct render-element draws retain their explicit
  material constant instead of reevaluating the enclosing pass RGBGen.
- Scene vertex programs declare invariant gl_Position for equal-depth
  multipass rendering.

Native code, GLSL/SPIR-V and release APK compile successfully. Added a bounded
256-record vulkan_material_program_probe.txt inventory of all encountered
material/program/state variants, including queue status, texture bindings,
lightmap mode and ambient. No per-frame engine logging was added.

These changes are source-level corrections, not proof of complete visual
parity. Specialized environment, gloss, fog-volume and other CG programs
still require individual expression/state comparison. The next scene capture
must establish the exact variants used by the reported dark/missing surfaces.

### Startup crash correction

Quest crash stack identifies mfScriptPreprocessorMask -> CLog::LogV -> strlen
during C3DEngine initialization. The missing-snapshot warning had a %d/%s
format but supplied only the shader name, making %s dereference invalid data.
Fixed its arguments and the duplicate-mask warning format. The Vulkan parser
now looks up masks only for declarations with an existing macro snapshot;
programs without one keep the zero mask. Native rebuild and APK packaging
succeeded; startup confirmation remains with the user's next launch.

### Training capture follow-up

The user reports unchanged dark skeletons, bright moss, missing ceiling and
surfaces appearing over rays. Material inventory includes combined projector
variants A800xxxx with first-pass depth-writing state and RGB ambient replaced
by projector direction. Split first combined projector ambient into an
independent draw before repurposing that payload; exclude the projector cookie
from ambient. Subsequent direct/specular draws stay additive.

Restored GLShaders.cpp's EF_TEMPLNAMES second-pass default ONE/ONE with EQUAL
depth. Unlike the earlier attempt, vertex shader position invariance is now
enabled. Also retain DOT3 lightmap intensity/direction alpha for variants with
no bump resource, using a flat tangent normal instead of treating the directional
map as an ordinary color lightmap. Native build/APK succeeded.

The surface capture was exhausted by loading-time queued=0/rejectedInput draws;
it now starts only after geometry is successfully queued in the current frame.
The ceiling's actual scene-time rejection and new visual results remain pending.

### Separate model lighting

The user confirms the ceiling now renders. Some separate models still have
incorrect shading. Skeleton captures select non-lightmapped CGRCAmbientTempl
variants; StatObjRend assigns atlas IDs/UV streams only when render parameters
contain pLightMapInfo. No level atlas is fabricated for unbaked models.

Corrections compared to source:

- TexLM reads the primary vertex buffer used by CREOcLeaf::mfGetPointer,
  falling back to its CPU shadow when primary data is unavailable.
- Preserve whether the expanded program declares VERTCOLORS. Ambient/light
  template passes without that feature ignore incoming mesh RGB, as the CG
  expressions do; fixed-function vertex color must not tint them implicitly.
- Evaluate the CG Diffuse parameter against the selected light, preserving
  LightColor component clamping and shader/material parameter scales.
- DIFFUSE/SPECULAR generation bits gate the corresponding light contributions;
  do not add a specular draw merely because CDLight has a nonzero specular color.
- Bone/ray probe distinguishes pass types even when texture/state are equal.

All affected native targets rebuilt and APK packaging succeeded. New headset
results remain necessary to confirm improvement of the separate models.


## Water surface translation (2026-09-30)

- Training's actual sea surface is `terrainwater_onlysky` / `CGVProgWater` /
  `CGRCWater` (confirmed by the bounded material inventory). It is a separate
  path from the FFT `CREOcean` client effect. Both paths are now submitted.
- The NULL hardware parser retains water texture layers and their blend/depth
  states after CG declarations. Previously all WaterVolume/ocean layers were
  lost; even the seafloor's `CGVProgTexGen_1Unit` pass was untextured.
- Dedicated Vulkan water vertex programs generate ripple and reflection UVs,
  LowMed opacity, IndoorSpec alpha and shore wave coordinates from the original
  CG expressions. Fragment programs translate LowMed, Indoor final/spec,
  Outdoor refraction, CGRCWater base/Fresnel and beach color/alpha expressions.
- DSDT is decoded as biased signed XY, without treating it as a normalized XYZ
  normal. CGPSParmRect applies `r_embm` and the original screen-dimension scaling
  before conversion to normalized Vulkan screen coordinates.
- The scene snapshot overrides sampler 1 only for actual refraction programs.
  LowMed and IndoorSpec keep `waterbump.dds`; beach passes keep `wave1`.
  Generic lighting, generated primary RGB and resource opacity do not overwrite
  the program's own water color/alpha output.
- CGVProgWater surface deformation uses the original 32-entry gradient /
  permutation Perlin algorithm and WaveAmplitude with the original time shifts.
  Ocean bottom texgen uses transformed world positions and BaseTexGen planes.
- CREOcean's former empty NULL draw now prepares the shared FFT data, updates
  its signed XY ripple texture, culls sectors and uses original stitched LOD
  indices. CPU displacement implements water-level, depth fade and curvature;
  the Vulkan vertex stage evaluates the original Fresnel expression. Stitched
  strips become one triangle list per sector to avoid repeated vertex uploads.
- **Remaining differences:** FFT ocean now has a separate mirrored-scene
  reflection target and samples it in the `CGRCOcean` pass; its reflected
  capture is kept separate from the scene-color snapshot used by indoor-water
  refraction. This is source/build verified but still needs Quest visual
  verification. Capture triggers now use OpenGL's water-distance time factor,
  camera position/angle thresholds, and exact FOV-change check. The target
  still uses per-eye swapchain resolution instead of OpenGL's 512x512
  `$WaterMap`. CGRCWater
  still uses the available `water_lm` base-water texture with a Fresnel-based
  alpha approximation. The projected-grid optimization falls back to sectors.
  Other CG water variants and sun-glint lighting still need translation.
- Android native build and release APK assembly succeeded. Visual appearance
  is not confirmed until the user runs both Training water scenes on Quest 3.


### Quest launch regression after water build

The first water APK could not load `libXRenderVulkan.so`: its dependency
`libXRenderNULL.so` had an unresolved `BoxSides` reference introduced by the
restored ocean sector frustum culling. The symbol table is now defined in the
NULL ocean backend using the same face ordering as `GLREOcean.cpp`. Verified
that the rebuilt NULL shared library exports `BoxSides`; release build and APK
installation on Quest 3 succeeded.
