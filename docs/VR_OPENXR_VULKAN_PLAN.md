# Far Cry VR rendering boundary

The VR path is Vulkan-only. OpenGL/Zink is not part of the VR pipeline and remains only for the legacy Android screen renderer until the Vulkan renderer replaces it.

## Current milestone

`SourceCode/CryVR` owns the API-independent OpenXR lifecycle:

- runtime loading without a link-time dependency (`libopenxr_loader.so` / `openxr_loader.dll`);
- instance and HMD system selection;
- `XR_KHR_vulkan_enable2` negotiation;
- session lifecycle and event processing;
- predicted frame timing and stereo view location;
- Vulkan XR swapchain creation/acquire/wait/release;
- Vulkan image views, framebuffers, command pool, descriptors and panel render pass;
- device-local/host-visible buffer allocation and staging uploads;
- RGBA8 texture creation and per-frame update with explicit Vulkan image layout transitions;
- SPIR-V module loading from memory or file through a platform-neutral API;
- the initial platform-neutral gameplay action set and controller state.
- platform-specific bindings for Meta Touch Plus, legacy Oculus Touch, Khronos Simple, HTC Vive and Valve Index profiles, normalized to select/trigger and move actions.

`CryVR::VulkanContext` now performs the common Vulkan bootstrap through the OpenXR `enable2` callbacks: it negotiates the API version, creates the OpenXR-compatible instance/device, selects the runtime physical device and exposes the graphics queue. `CSystem` creates the Vulkan XR session on `-vr` and passes both objects through `SCryRenderInterface` (`pVRRuntime` and `pVulkanContext`).

The current transitional frame path captures the legacy renderer's completed RGBA backbuffer before swap, uploads it to a Vulkan texture, and draws it onto a shared head-locked panel at 2.5 m. The quad is transformed independently using each OpenXR eye pose and asymmetric FOV, so the panel has binocular depth. Vulkan performs the panel rendering and OpenXR composition; OpenGL still produces the game image and must be removed by the native scene-renderer port. This readback/upload path is a bring-up path, not a performance target.

The normalized controller actions currently feed existing game bindings through SDL: left stick to WASD, left select to Enter, right select/trigger to Space, and right stick to relative mouse look. OpenXR action state is sampled per frame and cleared when session tracking is not active.

## Backend boundary

`VulkanFrameRenderer` is the temporary common frame and flat-panel renderer. `VulkanResourceManager` and `VulkanShaderLibrary` define the resource/shader boundary that `XRenderVulkan` will consume. The next renderer milestone is replacing the legacy OpenGL scene producer with native Vulkan scene submission. Its only platform-dependent code should be:

1. Vulkan loader/instance/device setup and queue selection;
2. Android/Quest surface and lifecycle integration;
3. PC window/surface integration;
4. Vulkan image views, render passes, framebuffers and synchronization.

Scene traversal, stereo views, input actions, frame timing and composition-layer assembly stay in `CryVR`/engine code and must not be duplicated for Quest and PCVR.

The Quest `GameActivity` launches with `-vr` and is registered as a VR activity; the Khronos Android OpenXR loader is packaged and initialized with the Java VM and Activity before other OpenXR calls. Until `XRenderVulkan` and native scene submission are implemented, game frames are mirrored to the VR panel through a synchronous GPU readback. Native Vulkan stereo scene rendering, menus as world-space UI, and video playback as a textured panel remain to be implemented.
