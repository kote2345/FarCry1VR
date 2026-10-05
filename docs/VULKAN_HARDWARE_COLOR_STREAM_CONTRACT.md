# Hardware color-stream parity contract

OpenGL sources: GLRendPipeline.cpp::EF_Start, EF_DrawGeneralPasses;
GL_Renderer.h::EF_CommitStreams; GLShaders.cpp::SArrayPointer_Color::mfSet.

## Required state and ordering

1. Reset the batch pointer set at EF_Start, after installing the object/material.
2. Apply selected technique pointers to that batch set.
3. Reset the pass pointer set before each pass executor; apply its pointers.
4. Resolve destination Color independently of the buffer's physical layout.
5. Resolve the selected source (`ePT`), type, component count and stride.
6. Commit the array enable/disable transitions with the OpenGL executor's order.
7. When disabled, use current GL color rather than texture-environment color.
8. Preserve texture-environment Need/CurGlobalColor independently.

Current stock water checks only whether a technique/pass destination Color
exists, and uses the primary vertex-buffer color stream in that case. It uses
white otherwise. This does not implement source remapping, pointer inheritance,
current-color lifetime or all component/type conversions.

## Evidence needed before completion

- Source-level correspondence for each transition above, including light,
  general, shadow and immediate draw executors.
- Captured selected technique/pass pointers and resolved GPU source for water,
  colored models, uncolored models and custom render elements.
- Runtime comparison with OpenGL for the same scene and materials.

GL current color after enabled color-array draws is not safely inferred from
the last vertex or from NeedGlobalColor. That behavior requires explicit evidence;
do not invent a last-vertex assignment to conceal the missing contract.

Next implementation: represent resolved Color input with source/type/components
and ownership at technique/pass commit, then feed that input to water and shared
stock programs. Keep unsupported mappings visible in existing diagnostics.

## Implemented water source resolution

The last Color destination pointer in technique/pass order now supplies ePT,
Type and NumComponents. Packed unsigned-byte primary or secondary color sources
are resolved through physical attribute offsets. Secondary color is copied to
the submitted primary attribute; three-component color supplies alpha one.
Copies are private and remain alive through submission, with the input pointer
restored afterward. Unsupported float/generated/missing-color mappings increment
the existing fallback diagnostic. They are not implemented or claimed equivalent.
Native/release builds succeed; runtime evidence remains pending.

Direct unsigned-byte source resolution also covers general-stream position,
normal and base texcoord offsets, with per-stride bounds validation. It copies
the bytes as declared, matching glColorPointer interpretation rather than
normalizing the original float/vector values. CREOcLeaf::mfGetPointer returns
Color/SecColor offsets directly without converting them for a FLOAT request;
float interpretation must therefore preserve source memory and precision, not
reinterpret packed color as normalized floats. Tangent/derived streams, float
attributes and destination layouts without color remain unimplemented here.

Unsigned-byte color pointers targeting Tangent, Binormal or TNormal now read
the separate SPipTangents stream at source offsets 0/12/24, as CREOcLeaf does.
Source stride is independent of the general vertex stride. Stream availability,
vertex count and both source/destination bounds are checked before private copy.
Float and derived-vector generation remain unimplemented. Release build succeeds;
actual pointer selection/current-color runtime parity is still unverified.
