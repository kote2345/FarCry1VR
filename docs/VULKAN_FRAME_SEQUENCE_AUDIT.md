# Последовательное сопоставление кадра OpenGL и Vulkan

Эталон — исходный XRenderOGL. Статусы относятся к конкретным операциям,
а не ко всему рендеру. Пользователь подтвердил наличие воды после изменения
порядка; полное соответствие её проходов ещё не установлено.

## 1. Вход в кадр и подготовка списков

| Операция OpenGL | Соответствие Vulkan | Результат |
|---|---|---|
| `BeginFrame`: `mfBeginFrame`, frame/update IDs | `CVulkanRenderer::BeginFrame` | Уже выполнялось |
| Сброс polygon counters, clear marker, real time | Там же | Перенесено |
| Освобождение alternating `m_TempMeshes`, выбор `m_CurTempMeshes` | Там же | Перенесено |
| `BeginFrame`: `ResetToDefault` | Новый Vulkan override | Перенесены scissor, clip override, texture operations/target selection, depth/blend/cull defaults, global color и shader-state flags; GL-only client-array bookkeeping пока отдельно не сопоставлено |
| `BeginFrame`: reload/filter/gamma/polygon-mode | Частичное соответствие | Перенесены reload с очисткой флага, выбор polygon mode, runtime filter и CPU gamma table/delta; графическое применение gamma соответствует Android GL-пути без device-ramp |
| `EF_StartEf`: диапазоны списков, vis objects, recursion, clear lights | Общий `CRenderer::EF_StartEf` | Одна исходная реализация для обоих backend |
| `EF_EndEf3D`: сброс NIGHT/HEAT flags | Vulkan override | Перенесено |
| `EF_UpdateSplashes`, `EF_AddClientPolys3D/2D` до фиксации EndRI | Vulkan override | Перенесён вызов общих функций; выполнение всех типов полигонов ещё не подтверждено |
| `EF_EndEf3D`: no-draw/fullbrightness/exclude/show-only/debug | Vulkan override | Перенесены guard незапущенной рекурсии, no-draw ветка и получение exclude/show-only; fullbrightness/debug ещё не сопоставлены |
| `EF_Flush`: exclude/show-only | `DrawRenderItem` | Перенесены исходные strcmp/strstr условия до отправки материальных проходов |
| `STexPic::SetFilter`: default FT_NOMIPS | Upload + sampler/wrap sampler | Перенесён default LINEAR без анизотропии |
| `CGLTexMan::SetFilter`: шесть GL filter modes | Vulkan sampler + wrap variants | Добавлены nearest без mips и nearest с linear mip interpolation; режимы различают min/mag/mipmap/LOD |
| `BeginFrame`: изменение global filter/aniso | Vulkan BeginFrame + `SetLegacyTextureFilter` | Обновляются busy mipmapped textures без переupload; сбрасываются wrap/projector descriptors, FT_NOMIPS сохраняет свой режим; aniso ограничено device cap, выбирает trilinear как GL |
| PREPROCESS → STENCIL → GENERAL → UNSORTED → DISTSORT → LAST | `stockListOrder` | Исходный порядок уже совпадал; поздний `sceneOrder` ещё требует отдельного разбора |
| `IGNORERENDERING` после preprocess | Vulkan list walker | Убрана фиксация флага до preprocess; значение читается после preprocess |
| `EF_PreRender(1)` и `EF_PreRender(3)` выполняют Stage & 1 | `PrepareStockFrameState` до/после сортировки | Добавлена вторая подготовка |
| Sun direction использует DLights[current recursion] | `PrepareStockFrameState` | Исправлен индекс: диапазоны RI используют level−1, свет — level |
| `EF_PreRender(3)`: portal depth range | List walker + `SetStockDepthRange` | Диапазон теперь записывается также в поля `m_RP` |
| `EF_PreRender(3)`: условия clear/NOCLEARBUF/startPipeline | `ClearStockSceneBuffers` + ordered attachment clear | Перенесены условия и color/depth/stencil-команда; после первичной инициализации scene render pass использует LOAD |
| `ClearColorBuffer` | Ordered color attachment clear | Исправлены момент выполнения, scissor, alpha=1 и wasCleared; исходная `SetClearColor` остаётся отдельной операцией |
| `ClearDepthBuffer` | Ordered depth attachment clear | Дополнено обновление wasCleared |
| Удаление client polygons и уменьшение recursion | `CNULLRenderer::EF_EndEf3D` | Общие вызовы уже выполнялись |

Сборка native-библиотек и APK после первого пакета изменений прошла.
Визуальная проверка этого пакета пока не выполнена. Этап 1 остаётся открытым.

Очистки сохраняют границу в `sceneOrder`: перенос воды не пересекает
color/depth/stencil clear. Color-clear инвалидирует snapshot воды.
Дополнительно требуется сопоставить write masks и режим measureoverdraw:
`vkCmdClearAttachments` не наследует GL color/stencil write masks.

Буферы сцены color/depth теперь общие для сменяемых OpenXR output images,
по отдельной паре для каждого глаза. Это сохраняет последний отрисованный
кадр, а не устаревшее содержимое очередного swapchain slot. Начальная CLEAR
используется только при первом выполнении для глаза; последующие проходы
используют LOAD и ordered clear-команды. Stencil store изменён с DONT_CARE
на STORE: refraction snapshot/resume больше не разрешает потерю stencil.
Синхронизация между кадрами остаётся через существующий frame fence;
общие attachments освобождаются один раз после удаления framebuffers.
Сборка прошла; визуальная проверка сохранения сцены ещё не выполнена.

## Следующий порядок работы

## 2. Вход в preprocess

`ProcessStockPreprocess` заменяет inline-обход только shadow receivers.
Перенесены сбор всех preprocess bits с исходными границами FSPR_MAX,
сортировка по SPRID, source index/объект/шейдер/resources/RE и consumed count.
При пустом списке операций возвращается 0, как в OpenGL. NOPREPROCESS
потребляет исходные элементы без выполнения операций. Генерация shadow maps
сохраняет проверки draw-to-texture и типов RE. REFRACTED маркирует объект
FOB_REFRACTED при исходном условии draw-to-texture.

SCANCM, SCANLCM, SCANSCR, SCANTEXWATER, SCANTEX, RAINOVERLAY, SCREENTEXMAP,
CORONA и PORTAL остаются отдельными незавершёнными операциями. Теперь их
пропуск учитывается RequirePanelFallback; это диагностический счётчик,
а не реализация или переход на другой renderer. REFRACTED-маркировка сама
по себе не реализует flush/capture преломлённых объектов.
Сборка прошла, визуальная проверка нового диспетчера пока не выполнена.

Gamma: `GLSystem.cpp::SetGamma` вычисляет 256 ushort значений с clamp gamma
0.5–3.0 и исходной формулой contrast/brightness. Это перенесено, включая
`SetGammaDelta`, который NULL backend ранее игнорировал. На Android исходный
`SetDeviceGamma` не применяет системную ramp (`#ifndef __linux`), поэтому
выходной Vulkan shader не получает дополнительную коррекцию. Перенос на
платформу с действующей системной ramp потребует отдельного соответствия вывода.

1. Закончить оставшиеся `BeginFrame` операции и clear semantics.
2. Сопоставить оставшуюся подготовку `EF_EndEf3D`.
3. Разобрать preprocess по вызываемым операциям и границам render targets.
4. Сопоставить list walker, смену объекта/материала и flush.
5. Перейти к последовательности и формулам материальных проходов.

Исходники: `XRenderOGL/GL_Renderer.cpp`, `XRenderOGL/GLRendPipeline.cpp`,
`Common/Renderer.cpp`, `XRenderVulkan/VulkanRenderer.cpp`,
`CryVR/VulkanFrameRenderer.cpp`.
# Shadow-pass clear ownership

EyeLinear direct-item coordinates no longer dot the original plane with
MV-transformed vertices. GLShaders.cpp installs GL_EYE_PLANE under the current
model-view; GL stores plane * inverse(MV), which cancels the same MV used to
draw this item. Vulkan now evaluates the equivalent object-coordinate dot.
Merged objects with a different draw MV need a separate plane-installation
lifetime implementation. Projective Q interpolation remains incomplete.
Native libraries and APK built; visual parity not verified.

Pass eCO_NOSET now preserves the entire TMU combiner (RGB/alpha operations
and both argument packs), matching GLTextures.cpp mfSetTexture's single
color-op guard. Previously alpha/argument fields could still change. Explicit
element overrides remain higher priority. Applied to all eight stages.
Native libraries and APK built successfully; visual parity not verified.

Texgen enable/source audit: the supported stage-0/1 CPU translation now
evaluates pass-owned m_GTC even for Base/None/NoFill/LightMap software modes.
GLTextures.cpp mfSetTexture enables this->m_GTC independently of resolved
pSTU image/resource; Vulkan previously returned early and could read the
resource generator instead. Higher-stage independent generated coordinates
and full cube sampling remain incomplete. Native libraries and APK built;
no runtime visual verification.

Translated normal-map alias now tracks the TMU from which the bump image
was selected. Final explicit element replacement/unbinding of that stage
also updates the normal-map image and its LOD; previously the dedicated
sampler retained the old image while the ordinary stage changed. This
preserves the GL ordering of resource binding before element drawing.
Native libraries and APK built. Programmable sampler layouts and generated
coordinate parity remain incomplete; no visual check of this change.

Explicitly bound stages 2-7 now receive final element combiner operations,
arguments and LOD. A newly added stage inherits cached TMU state rather than
VulkanStockTextureStage defaults; existing stages retain pass values except
for explicit element overrides. Stages 4-7 are also exposed to submission
when added by the element. Coordinate inheritance and normal-map aliases
remain open. Native libraries and APK built; not visually verified.

Final explicit sampler bindings are now applied after implicit hardware
resource/bump preparation for all eight stages, including ID 0 unbinding.
mfUpdate only changes the animation frame and does not bind a texture.
This prevents late AmbPassTempl binding from overwriting an element choice.
For stages absent from the pass, coordinate/combiner inheritance remains
under audit; translated normal-map aliases also require comparison.
Native libraries and APK built successfully; not visually verified.

Explicit element texture binding: SetTexture during an active pass now marks
the selected TMU as overridden. resolveStageTextureId gives that binding
precedence over pass/resource/object tokens, matching mfSetTextures followed
by mfDraw in GL. Stages 0/1 also explicitly propagate unbinding (ID 0).
Binding new stages absent from the pass and implicit programmable samplers
still require comparison. Native libraries and APK built; no visual check.

Fixed-pass texture boundary: before element drawing, cached TMUs at and above
the pass texture count are now disabled and m_nCurStages updated. This follows
GLTextures.cpp SShaderPass::mfSetTextures -> BindNULL(i), retaining combiner
state while dropping excess textures. Hardware passes with implicit resource
samplers still need a separate binding audit. Native libraries and APK built;
runtime parity not verified.

State-shader stencil restoration now reapplies packed compare/op, reference,
and compare mask at every item flush even when the state-shader pointer is
unchanged. GL calls SStencil::mfSet at every EF_SetStateShaderState. This
prevents an element's stencil changes leaking into the next item. Stencil
clear frequency still depends on source batching boundaries and is not
claimed equivalent. Native libraries and APK built; no visual verification.

Hardware resource-opacity dispatch: after technique selection, non-opaque
resource opacity now clears cached m_RP.m_ObjFlags FOB_LIGHTPASS, matching
EF_SetResourcesState. Technique selection still sees original object flags.
AlphaRef and IGNORERESOURCESTATES suppress this transition as in GL. Fixed
fallback selection reads the object's flag, matching GL fixed-pass selection.
This does not complete programmable-pass translation or verify visual parity.

Resource alpha-test audit: `stockAlphaTestPasses` already gives nonzero
material AlphaRef precedence over pass bits and uses >=, matching GL_GEQUAL.
`DrawRenderItem` now sets RBSI_ALPHATEST before pass/element state evaluation,
matching EF_SetResourcesState. AlphaRef also suppresses resource-opacity
blending, as in the source. Built native libraries and APK successfully.
Resource/state-shader culling now follows resource first, state shader second;
depth overrides enter the cached state before their protection flags apply.
These changes are not visually verified.

Generated-color precision: masked primary channels now consume the same
truncated UCol bytes as the texture environment constant. Previously only
the constant was quantized; primary-color modulation retained float precision.
This follows the byte storage in GL `EF_Eval_RGBAGen` and `EF_SetGlobalColor`.
Native libraries and release APK built successfully; not visually verified.

RGBA generator audit: source `EF_Eval_RGBAGen` initializes its local color
to white RGBA. Vulkan `EvaluatePassColor` now sets all four channels for
`eERGB_Identity`, carries `CLightStyle::m_Color.a` for `eERGB_StyleColor`, and
preserves fixed alpha while scaling RGB for `eERGB_StyleIntens`. Previously
these branches retained vertex alpha. Native libraries and release APK built
successfully; no visual verification yet. The generator audit remains open
for protected generation flags, software client color paths, and byte conversion.

State-shader protection audit: `GLRendPipeline.cpp::EF_SetStateShaderState`
sets `RBSI_NOCULL`, `RBSI_DEPTHTEST`, and `RBSI_DEPTHFUNC` when it overrides
culling or depth. Vulkan `DrawRenderItem` now sets those same flags alongside
the existing state overrides, so render-element callbacks see the source
contract. Native libraries and release APK built successfully. This does not
complete the protected-state audit: RGB/alpha generation and every downstream
state consumer still require comparison; visual parity is not established.

Runtime checkpoint: the accumulated APK was installed successfully on Quest
2G0YC1ZG5G069Z via adb install -r. The application was not launched by the agent.
User scene confirmation and runtime visual evidence are pending. This checkpoint
does not close the phase or prove whole-renderer parity.

Further source comparison confirms ordinary blend factors, alpha comparisons
and depth Equal/Greater/Lequal mapping match the inspected GL state switch.
Mirror culling and 2D cull disable are now resolved at draw submission.
Polygon offset applies to fill only; fixed-function detail retains shader offset,
hardware detail runs after it is disabled. Detail scissor is disabled and TMU
LOD bias retained, including late bump bindings. Detail begins from its own
blend/depth state and preserves explicit state-shader depth/color-mask overrides
and blend/write protections from resource/object opacity. Full stencil lifecycle,
protected alpha state and all capture passes remain open.

LOD bias: Vulkan now overrides the NULL SetLodBias stub with per-TMU state,
initializes it from r_maxtexlod_bias during ResetToDefault, passes it to direct
draws and records shader-unit bias for later draws. Render-element SetLodBias
uses a per-pass override bit so material unit resolution does not erase the
call. Initial combiner requested-argument caches are invalidated by reset while
effective sources persist, and overridden clip planes are restored as in GL.
Native/APK builds succeed; specialized bump/water sampling and runtime mip
selection still require comparison.

RGB operation cache now distinguishes the requested EColorOp from its effective
combine function. MULTIPLYADD/BUMPENVMAP preserve the function but reset RGB
scale to one; 255 preserves both. Entering interpolation after a no-op reinstalls
SOURCE2/OPERAND2 even if the effective combine function was already interpolate.
All eight TMUs use this rule. Native/APK compilation succeeds; runtime comparison
is still pending.

Texture interpolation now tracks third-operand RGB versus alpha per TMU.
Entering BLENDDIFFUSEALPHA/BLENDTEXTUREALPHA installs SRC_ALPHA; a changed
non-Specular third source installs SRC_COLOR. RGB-vector interpolation uses
existing GLSL mode16 while retaining the original operation in the GL state
cache. Argument updates compare individual fields rather than rewriting all
sources whenever any field changes. Native/APK build succeeds; runtime parity
and remaining retained combiner operations still need verification.

Texture combine comparison: the one/two/three/four-texture color fragment
programs no longer apply RGB_SCALE 2/4 to alpha MODULATE2X/MODULATE4X. OpenGL
EF_SetColorOp sets GL_RGB_SCALE and GL_COMBINE_ALPHA=GL_MODULATE, without alpha
scaling. Advanced operations and retained combiner state still require further
source comparison; this fixes only the demonstrated alpha scale discrepancy.

Opacity follow-up: technique selection observes EF_Start opacity=1; resource
opacity is applied after selecting the technique, respecting AlphaRef and
IGNORERESOURCESTATES. ResourceState is reset per material and records resource
blending. Object-alpha handling is restricted to OcLeaf, clears depth-equal when
blending already exists, and overrides stage-zero alpha with texture*constant
for fixed-function ordinary blending. Additive/multiplicative branches execute
even at opacity=1 as in SetAlphaState. Fixed-function resource opacity uses the
source truncation to an eight-bit color. Native/APK build succeeds; this does not
establish parity of all programmable alpha paths or final rendered results.

EF_Start vertex-layout initialization now sets shader format, stride,
color/UV/normal offsets and NextPtr, clears geometry counts and merged RE/object
arrays, and selects SysRendIndices at the item batch boundary before preparation.
This uses the common gBufInfoTable/m_VertexSize definitions. Native/APK build
succeeds; renderer buffer allocation and actual geometry preparation still need
full source and runtime verification.

Material entry now sets m_pStateShader, resets per-flush flags/modifiers,
light-material and technique caches and advances m_Frame at the direct-item batch
boundary. Fog-volume selection now observes CV_r_VolumetricFog, matching EF_Start.
Vulkan still emits an individual item per batch; full source batching parity,
opacity evaluation and vertex-layout initialization require further comparison.
Native/APK build succeeds; runtime equivalence remains unverified.

Nearest CPU camera transitions now follow EF_ObjectChange: save previous camera,
use zMin=.01/zMax=40 and FOV*.6666 for nonzero nearest objects, then restore the
camera/depth range when leaving nearest or completing a list. The existing XR
eye projection independently applies the same nearest FOV factor; CPU camera
projection is used for legacy APIs, not multiplied into that eye projection.
Native/APK build succeeds; nearest geometry and subsequent world passes require
runtime comparison.

Object transition now compares the candidate against m_pPrevObject and stores
the newly loaded object there, matching EF_ObjectChange. Previously it compared
against m_pCurObject and stored the replaced object; the first identity object
could skip its transition after PreRender invalidated the cache. Native/APK build
succeeds. Nearest draw viewport/z planes already have dedicated Vulkan handling;
CPU nearest camera/FOV parity remains to be audited.

BindNULL(1) list cleanup: higher stage texture IDs are now zeroed while combiner
operations/arguments remain intact. SetTexture tracks m_nCurStages when binding
a nonzero texture. This prevents direct-TMU render elements without an active
shader pass from inheriting higher stages from the preceding list. Native/APK
build succeeds; visible behavior has not yet been compared.

List exit now disables RBPF_SETCLIPPLANE, restores the list depth range, clears
MATRIXNOTLOADED/PS1NEEDSET/PS2NEEDSET/TSNEEDSET/VSNEEDSET, selects TMU0 and
disables scissor, following the tail of OpenGL EF_PipeLine. Object-change-to-zero,
nearest camera restoration and BindNULL(1) still require further comparison.
Native/APK compilation succeeds; visual equivalence remains unverified.

PreRender camera boundary: PrepareStockFrameState resets m_ViewMatrix to the
camera matrix before camera-info evaluation. The later item-loop entry no longer
overwrites the NULL previous-object value established by PreRender(3). This
matches EF_PreRender's cache invalidation before the first object transition.
Native/APK build succeeds; runtime evidence is still required.

List-entry comparison against GLRendPipeline.cpp::EF_PipeLine found another
ordering difference: identity current/previous object and m_nCurLightParam=-1
are now established before PreRender(1) and preprocess. Previously only the later
item-loop initialization set the identity object. IGNORERENDERING is checked
after preprocess only for shader-sorted lists, matching the source branch.
Native libraries and APK build; rendered parity remains unverified.

Follow-up source comparison with XRenderOGL/GL_Shadows.cpp::PrepareDepthMap:
fog is now disabled during caster collection and restored afterwards; capture
size is halved until it fits renderer dimensions; makeNewTexture allocates a
fresh ID. HasQueuedShadowMapDraws excludes clear commands, so a clear alone
cannot falsely satisfy the caster update check. Native/APK build succeeds.
The post-capture main-buffer color/depth clear is now queued after viewport and
camera restoration, with scissor disabled, GS_DEPTHWRITE and default clear RGB /
alpha zero as in GL_Shadows. Stencil is preserved. Build succeeds; runtime
comparison is still pending.

Shadow-slot reuse, penumbra/blur and complete
nested renderer-state parity are still pending.

QueueStockClear and QueueStockClearStencil now preserve the active shadow target
ID, just like geometry draws. Previously these commands had ID zero and could
execute against the main eye attachments after preprocess. RecordShadowMapDraws
now executes the clears in sequence within their shadow framebuffer, with a
target-sized rectangle intersected with the captured scissor. Drawing viewport
does not restrict clearing. Color/stencil write-mask parity remains pending.

Native libraries and APK compile successfully; runtime visual parity is unverified.
