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

// OpenXR orientation delta from the captured origin to the current eye pose.
// This exact delta is also used to build the rendered camera transform.
bool GetOpenXrRelativeOrientation(const XrQuaternionf& origin,
                                  const XrQuaternionf& current,
                                  XrQuaternionf& relative);

// Reprojects a legacy GL modelview captured at the reference head pose into
// the current OpenXR eye. The caller may anchor eye positions to the reference
// head position for 3DoF while preserving current orientation and per-eye IPD.
bool BuildOpenXrEyeMvp(const XrView& eyeView, const XrPosef& referenceHeadPose,
                       const float referenceModelView[16],
                       float nearPlane, float farPlane, float mvp[16],
                       float eyeModelView[16] = 0, bool nearestObject = false);
}

#endif
