#ifndef CRY_VR_VULKAN_FRAME_CAPTURE_H
#define CRY_VR_VULKAN_FRAME_CAPTURE_H

#include "CryVR.h"

namespace CryVR
{
// Build a projection for a head-locked flat image panel. The panel center is
// shared by both views; each eye uses its own OpenXR pose and asymmetric FOV.
bool BuildFlatPanelMvp(const XrView& leftView, const XrView& rightView,
                       uint32_t eyeIndex, float aspectRatio, float distanceMeters,
                       float mvp[16]);

// OpenXR eye pose + asymmetric FOV to a column-major Vulkan clip transform
// (right-handed view, -Z forward, Y-flipped framebuffer, depth 0..1).
bool BuildOpenXrViewProjection(const XrView& view, float nearPlane,
                               float farPlane, float viewProjection[16]);

// Reprojects a legacy GL modelview captured at the reference head pose into
// the current OpenXR eye. This preserves the game's camera as the world-space
// reference while adding per-eye IPD and tracked head motion.
bool BuildOpenXrEyeMvp(const XrView& eyeView, const XrPosef& referenceHeadPose,
                       const float referenceModelView[16],
                       float nearPlane, float farPlane, float mvp[16],
                       float eyeModelView[16] = 0);
}

#endif
