# OpenGL color evaluation and commit contract

Source reference: XRenderOGL/GLRendPipeline.cpp EF_Eval_RGBAGen,
CCObject::SetAlphaState, EF_SetResourcesState; GL_Renderer.h
EF_CommitTexStageState. Vulkan reference: EvaluatePassColor and DrawBuffer.

## Distinct state

OpenGL evaluates into m_NeedGlobalColor. EF_CommitTexStageState copies its
complete packed value into m_CurGlobalColor before applying the texture stages.
The cached combiner arguments and the committed effective arguments are not
always equal. Vulkan currently updates m_CurGlobalColor directly from its
generated primary color and uses separate object-alpha translations.

At stage zero only, RBSI_GLOBALRGB replaces the second RGB argument selector
(bits 3 through 5) with eCA_Constant. RBSI_GLOBALALPHA independently replaces
the second alpha argument selector. The other argument bits are preserved.
Stages above zero keep their cached arguments. These replacements are commit
state; they must not overwrite the stored material combiner arguments.

## Generator branches that must remain different

| RGB generator | OpenGL when a render element is active |
| --- | --- |
| Fixed, StyleIntens, StyleColor, Comps | Install global color without RGBGEN skip |
| Identity | Install global color only when RGBGEN is clear |
| Object, Wave, Noise | Install global color when RGBGEN is clear; otherwise use vertex evaluation |
| OneMinusObject, RE, OneMinusRE, World | Their guarded global branches do not universally run |
| FromClient, NoFill | Do not install a new global color in this branch |

The fallback vertex evaluators in Common/EvalFuncs_C.cpp write the pipeline
vertex color stream. They cannot all be implemented by reusing a uniform color
or by skipping the entire RGB evaluation when RGBGEN is set.

Alpha Identity tests RGBGEN and sets RGBGEN. Fixed tests ALPHAGEN, except
when RGB Fixed already supplied alpha. Object, OneMinusObject, RE,
OneMinusRE and World have ALPHAGEN guards. Style runs unconditionally.
Comps has the legacy assignment `m_FlagsPerFlush = RBSI_ALPHAGEN` in its
render-element branch. That assignment is part of the working source behavior.

## Remaining implementation requirements

1. Preserve the expected packed global color separately from the committed
   texture environment color through material passes.
2. Mirror generator-specific guards and updates, including their stream-writing
   branches and the earlier resource/state-shader flags.
3. Apply stage-zero global RGB/alpha selector substitutions at the actual commit
   boundary, retaining the original cached combiner arguments.
4. Reconcile the existing object-alpha texture environment translation with
   that commit, so the same operation is not applied twice.
5. Compare both the generated primary color and all effective texture-stage
   arguments with OpenGL before claiming multi-pass color parity.

This source review identifies missing behavior; it does not certify runtime
parity or complete the transfer.

## Implemented commit selector handling

DrawBuffer now applies both GLOBAL flags to its local stage-zero argument
copies before the explicit object-alpha texture-env translation. NoSet (255)
arguments are resolved from the stored stage argument before substitution.
The cached material arguments and stages 1 through 7 remain intact.

This provides the commit operation, but the complete resource/generator flag
producers and expected-global-color lifetime are still incomplete. Existing
opacity translations have not been declared equivalent by this change.

## Expected-color state foundation

Successful global generator output now writes the truncated packed bytes into
`m_NeedGlobalColor` and commits that value into `m_CurGlobalColor`. This
preserves the expected color separately for subsequent pass preparation.
It does not yet change skipped-generator or vertex-stream fallback semantics,
and it does not complete all flag producers. Release APK rebuilt successfully.

## Light-style packed color evaluation

Compared StyleColor/StyleIntens RGB and Style alpha with EF_Eval_RGBAGen.
StyleColor now uses the same CFColor::GetTrue packed conversion. StyleIntens
and Style alpha cast fixed-byte times intensity to byte before normalization,
as OpenGL does, rather than clamping the float before conversion. The existing
primary/environment commit still consumes packed bytes. Release build succeeds.
Generator-specific flags, skipped branches and vertex-stream fallbacks remain
unfinished; these three branch corrections do not establish multi-pass parity.

## CPU generator implementation transferred to NULL backend

Replaced the empty CNULLRenderer::EF_Eval_RGBAGen body with OpenGL's complete
CPU implementation, changing only the owning class name. This retains every
RGB/alpha branch, RGBGEN/ALPHAGEN guard, legacy flag assignment, packed color,
client/vertex write, shared evaluator call and final m_NeedGlobalColor update.
Native libraries and APK build successfully.

Vulkan DrawBuffer still calls EvaluatePassColor and does not yet call this
transferred routine. Integration must install valid scratch vertex pointers,
stride/color offset/count and the appropriate shared evaluator, then preserve
stream/global-color state for the flush. The transfer alone does not change
Vulkan material output and is not evidence that generator parity is complete.

## Generator initialization prerequisites

Inspection for vertex-stream integration found EF_InitWaveTables,
EF_InitRandTables and EF_InitEvalFuncs empty in XRenderNULL. Replaced all three
with their OpenGL CPU implementations. Existing NULL EF_PipelineInit already
calls these functions, so Vulkan's current wave/component/deformation consumers
now receive the same initialized sine, half-sine, cosine, hill, square, sawtooth,
inverse-sawtooth and triangle tables. Random initialization retains the source
formula, including its literal 32767 divisor. Shared evaluator selection now
sets m_pCurFuncs to the C or RE evaluator instead of leaving it unset.

Native libraries and release APK compile. Full EF_Eval_RGBAGen draw integration
remains pending: RE Beam additionally calls EF_GetPointer and cannot be enabled
safely with only a scratch RGB stream. Runtime visual parity remains unverified.

## Scoped CPU stream access foundation

EF_GetPointer is an inline common renderer entry point, not a Vulkan virtual
method. Its RE path delegates to CREOcLeaf::mfGetPointer/sGetBuf, whose original
streams cannot represent Vulkan's private generated upload copy. Added an
optional SCpuDrawStream context to the common entry point: source/destination
selection follows FGP_SRC, chunk offset follows FGP_REAL, and position, normal,
color, secondary color, UV0 and tangent-basis pointers carry their own strides.
A missing stream returns null instead of aliasing an OpenGL offset or the shared
leaf buffer. Without a context the existing RE/common delegation is preserved.

No Vulkan draw installs this context yet. Integration must scope/restore it,
provide scratch stream lifetime and range, and handle remaining derived pointer
kinds (including light vectors and lightmap coordinates). The whole native set
and APK rebuilt successfully after the common renderer layout change. This is
an accessor prerequisite; it does not establish active generator execution.

## Shared generator activated for color-bearing draw streams

DrawBuffer now calls the transferred CPU generator for formats exposing a
vertex-color attribute. It installs a scoped SCpuDrawStream over the private
upload copy, retains the original source stream and tangent basis, and supplies
the current chunk's first vertex, count, stride and color offset to the shared
evaluator. The context and legacy pointer fields are restored after evaluation.
A protected NULL helper selects C/RE evaluation and reports whether bSetCol
installed a global packed color, including installations with unchanged bytes.
Vulkan consumes that exact result and commits retained m_NeedGlobalColor even
when the current generator's global branch is guarded. Stream writes remain in
the private geometry uploaded by this draw.

Invalid ranges or Beam without its required tangent basis are accounted as
untranslated and retain the existing fallback. Formats without a color attribute
still use the prior evaluator. Persistent generated color streams across multiple
draws, missing resource/state-shader flag producers and no-color format conversion
are not yet transferred. Thus this activates the source branches for part of the
actual draw path but does not establish complete multi-pass color parity.
Build succeeded before the final Beam prerequisite guard; final rebuild follows.

## Generated color lifetime within a flush

Added a per-buffer packed-color cache scoped by m_RP.m_Frame (the direct Vulkan
item path increments this at its EF_Start-equivalent boundary). ensureVertexCopy
restores a compatible format/count's retained colors before deforms, texgen and
color generation. After the shared generator, the resulting packed stream is
retained for following passes/light draws using that buffer. A new flush clears
the cache, so another object sharing the leaf starts from its original colors.
Original source vertices remain separate for FGP_SRC consumers.

This corrects losing generated vertex colors when the next draw creates another
private copy. The actual Vulkan batching/merging boundary is still not fully
OpenGL-equivalent. Formats without color, missing flag producers and complete
program execution remain open. Native libraries and release APK build successfully;
runtime output has not been verified.

## State-shader color producer order

Moved packed state-shader color installation to the item/flush preparation
boundary, before pass generators. It now sets RGBGEN and ALPHAGEN independently,
constructs zero-initialized UCol with the same fixed-channel rules and byte 0/3
exchange, and assigns m_NeedGlobalColor once at that boundary. Draws consume
retained expected state color before evaluating the material generator.

Removed the former post-generator unconditional state-color reconstruction:
OpenGL's Fixed/Style/Comps generators can replace the state shader's initial
color, while guarded branches inspect its already-set flags. The old Vulkan
ordering overwrote those results and supplied no guard flags. Release build
succeeds. No-color fallback evaluation, resource/object opacity state producers,
primary-color array selection and complete batching still require comparison;
runtime parity is not verified by this correction.

## Opacity alpha-generation state producers

Compared resource opacity in EF_SetResourcesState and object fade in
CCObject::SetAlphaState. Vulkan set ALPHABLEND/DEPTHWRITE but omitted ALPHAGEN.
Added the same alpha-generator protection to both existing opacity paths,
updated m_fCurOpacity for resource/object state, and cleared cached FOB_LIGHTPASS
for resource opacity as OpenGL does. AlphaRef continues to take precedence.

Release native/APK build succeeds. GLOBALRGB/GLOBALALPHA resource producers
and avoiding duplicate fragment/combiner opacity remain to reconcile; resource
state setup still differs in dispatch timing and no-color formats retain the
fallback evaluator. These changes are not full opacity or rendering parity.

## Object-fade texture-environment persistence

Compared CCObject::SetAlphaState fixed-function branches. Blend classification
now prefers m_ResourceState's blend pair, falling back to the pass state only
when it is absent. The object-fade packed color is committed into both expected
and current global color, rather than supplied only as a temporary draw constant.
Multiplicative/additive RGB modes set GLOBALRGB; the ordinary alpha mode sets
GLOBALALPHA. Existing explicit object-fade combiner changes remain and fragment
opacity stays one in these modes, avoiding an additional fade multiplication.

Release build succeeds. Source EF_LightMaterial clears GLOBAL flags when actual
fixed lights are installed; Vulkan lighting ownership still needs reconciliation.
Resource opacity's GLOBAL producers and complete CG parameter semantics remain
open. No-color formats and runtime visual parity remain incomplete.
