# Camera, weapon, particle and shoreline corrections

## Evidence from the user's run

`research/vulkan_material_trace_latest.txt` records the DE weapon and hand
materials with resident albedo textures but vertex format 1 (positions only).
The particle pass uses texture ID 4096, whose image is `Textures/red`.

## Corrections

- Update the flattened body camera matrix after changing its angles. The engine
  updated its visibility camera, but the Vulkan renderer received a copied matrix
  with the original game pitch/roll. Use the inverse Cry camera view matrix for
  controller world coordinates, including its neutral rotation and yaw convention.
- Restore UV stream requirements for CGVProgAmbientTempl/CGVProgLightTempl in
  shader construction, and tangent requirements for the latter. OpenGL obtains
  these from the compiled vertex program's mfHasPointer calls.
- Use fholger's stock weapon grip bone names when the original Lua scripts have
  no BoneRightHand field. Clear cached grip data on weapon reset and remove the
  desktop weapon sway angles from the tracked orientation.
- Override NULL renderer LoadTexture, LoadAnimatedTexture and
  GenerateAlphaGlowTexture. The inherited implementations returned the red error
  image, zero animation IDs and zero glow texture IDs, respectively.
- Execute CGRCAmbient_Particle's texture * (vertex light + ambient) RGB and
  texture alpha * vertex alpha * ambient opacity before the normal opacity and
  alpha-test operations. This preserves ambient for particles whose vertex light
  is intentionally zero.
- Preserve layers/states for CGRCWater_Beach and beach shift programs. Translate
  their original UV animation, vertex color and sampled alpha, using the beach UV
  vertex shader for both refractive and non-refractive variants. The latter also
  evaluates PosWaterDeform noise and its UV perturbation on the CPU.
- Guard normalization of zero normals in terrain detail shading.

Native libraries and release APK compiled successfully. Visual results require
the next headset run; build success does not establish rendering parity.
