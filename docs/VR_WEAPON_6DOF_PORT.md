# Controller tracked weapons

Reference: https://github.com/fholger/farcry_vrmod

Reviewed VRManager::GetControllerTransform, CPlayer::ModifyWeaponPosition,
CWeaponClass::InitStaticTransforms and GetMuzzlePosAngles in the local checkout
under research/fholger_farcry_vrmod.

The OpenXR adapter locates left/right grip action spaces at predicted display
time and rejects poses without both valid position and orientation. The same
yaw reference used by the Vulkan camera maps controller poses into Cry world
space. Position is relative to the reference head position because this port's
game camera already supplies eye height.

First person weapons map the model's right hand grip onto the right controller,
using the reference mod's default palm offset and script RHOffset/RHOffsetAngles
when available. Shots from the local player use the transformed spitfire bone.
Missing tracking or grip bones retain the original camera weapon behavior.

This is the initial one handed integration. Two handed grip locking, physical
reload gestures, upper arm hiding, left handed model mirroring and per-weapon
grip tuning are not yet ported. Device behavior requires user validation.
# Weapon hands and size follow-up (2026-10-02)

The fholger implementation retains the hand meshes. `HideUpperArms` collapses
up to three parent bone matrices at the hand position after character animation
update, removing the forearms. Its weapon placement uses rigid transforms and
the inverse grip transform, without a uniform weapon size multiplier.
`SetScale(-1,1,1)` in that implementation mirrors left-handed weapons.

Our Vulkan variant now omits the first-person `hand-hero` material when a right
controller transform is available. This removes the separate hand/arm material
without changing the bones used for grip, reload or muzzle transforms. Materials
with hands baked into the weapon's own albedo are not covered by this filter.
Weapon asset scale remains unchanged. Release APK built and installed; headset
appearance remains to be confirmed.
# Reference grip and arm port (2026-10-02)

## DRAW_NEAR projection parity

The reference mod's VRRenderer::Hook_Renderer_SetCamera explicitly cancels
the desktop weapon FOV reduction for cameras with near=0.01 and far=40.
This renderer hook was missing from the initial gameplay-only port.
Vulkan still multiplied the weapon eye FOV by 0.6666, magnifying the model
and its apparent translations relative to real controller movement.

UpdateStockNearestCamera now preserves the world FOV. BuildOpenXrEyeMvp
and the matching fragment projection rays always use the full XR eye FOV.
The weapon near/far limits and depth range are retained. Geometry and grip
offsets are not rescaled to compensate for a projection mismatch.
Compilation succeeded; physical alignment needs headset confirmation.


## Controller camera basis and firing follow-up

During CSystem::Render, C3DEngine::SetCamera replaces the shared system
camera with the tracked visibility camera. The late weapon pose update
formerly read that camera and used its head yaw as the body yaw, applying
head rotation twice. CSystem now preserves the untracked render camera for
controller-to-world conversion until drawing ends. Outside drawing, the
current game camera remains the source of body position/yaw.

VR firing now has persistent held state and a latched release in IInput.
Gameplay commands use these states on every update, overriding synthetic
SDL mouse state while VR input is present. A missing mouse-button-up can no
longer leave semi-auto firing waiting for release. Menu mouse events are
still delivered through SDL. Weapon fire-mode rules remain in CPlayer.


The weapon grip now uses the reference mod's per-weapon Lua values even
when the installed stock scripts do not contain VR fields. This includes
MP5, Shotgun, RL, AG36, OICW, P90 and SniperRifle offsets/angles, and both
hand bone names. Explicit script values override the built-in profiles.
Grip matrices are initialized from the initial idle pose after model load,
including the reference correction for mirrored character models.

The tracked transform is controller * rotationY(-90 degrees) *
rotationZ(vr_weapon_pitch_offset) * rotationX(vr_weapon_yaw_offset) *
inverseGrip. Defaults match the reference: pitch 15 degrees, yaw 0.
No additional weapon size multiplier exists in the reference. The tracked
weapon uses render scale 1 and its original model units. Position updates
immediately before first-person drawing use the current tracked pose.

Forearm hiding now follows the reference bone algorithm: collapse up to
three ancestors of each hand onto the hand position after animation update.
The off hand is hidden through its relative bone matrix except during
reload. The previous renderer-side hand-hero material omission is removed;
the reference retains the main hand while suppressing long arms. These
changes do not add the reference mod's two-hand activation/input mode.

Build verification is compilation only; headset alignment remains to be
checked by the user.
