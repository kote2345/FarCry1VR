# Physical NPC implementation stages

## Six degree of freedom grip checkpoint

The point grip plus 12 Nm relative angular motor is replaced by separate solver
constraints for translation and all three relative rotation axes. CryPhysics
now supports an angular-only fixed orientation constraint using captured common
world frames and shortest-arc quaternion drift correction. Each constraint has
its own ID and is removed on release/body loss. Angular drift is excluded from
linear energy correction. Tracking torque remains finite (40 Nm), and hand
tracking preserves controller roll via the captured controller-to-hand frame;
the former single-axis capsule orientation lost that roll. Diagnostics report
position/orientation constraint IDs and relative angular error. Existing arm
chain and reach locomotion coupling remain; this does not yet implement finger
surface contacts or a fully dynamic player root. Headset acceptance is pending.

## Held arm motor target correction

User feedback reported very heavy small props after mass scaling. The physics
update had supplied held-contact IK to upper-arm/forearm motors while only the
hand target was remapped to tracking. This made finite arm drives oppose hand
motion and introduced resistance unrelated to prop mass. The update now supplies
raw tracked IK to the entire arm, with held contact used only for rendering.
The extra hand remapping is removed. Existing 0.5 rigid prop mass scaling remains.
Native compilation passed; perceived weight requires headset acceptance.

## Confirmed constraint creation blocker

The requested headset log recorded grip input and selected prop 5771, but no
constraint success or rejection. `CPhysicalEntity::GetParams(pe_params_part)`
filled the requested part data and fell through to return 0. The grip creator
therefore exited before submitting its constraint. The engine query now returns
1 after filling part data and rejects out-of-range slots before access. This
also unblocks the bullet part-ID fallback. Native build passed; headset
behavior remains to be checked. Logs are read only at the user's request.

## Grip update ownership correction

Headset feedback still reported immobile held objects and unrestricted walking;
the retrieved diagnostics contained no successful physical-grip records.
VR body creation and tracking drives previously depended on the avatar draw
callback. They now run from the active physical-interaction update before grip
constraint creation. A temporary tracked IK pose is prepared and restored
without feeding changes back into animation. Rendering resolves contacts with
zero drive time. Missing bodies/bones and rejected constraints have bounded
diagnostics. Native build passed; runtime cause and behavior need headset
confirmation rather than assuming the previous part-ID fix was sufficient.

## Anatomical reach and locomotion checkpoint

Held props/NPCs now limit stick locomotion by shoulder-to-wrist reach using
the physical upper-arm and forearm lengths, with a 2 cm extension reserve.
Shoulder offsets follow the tracked torso and are rebased onto the current
player root. The limiter predicts attachment motion (including angular
velocity) and tangential walking, and iterates both held hands. Outward
coasting momentum is transferred to the load; continued blocked stick input
adds equal/opposite pulling impulses capped at 350 N per hand. No root or
held-object transform is written. Native build passed; headset acceptance is
pending. This constrains stick locomotion; real-world HMD room-scale movement
and fully anatomical joint limits remain separate work.

## Physical hand constraint checkpoint

The first headset check found a pinned visible hand with no object movement.
`pe_status_pos.partid` is an input selector, not an output when querying by
part slot; its default -1 caused CryPhysics to reject the grasp constraint.
The corrected checkpoint resolves the selected part ID via `pe_params_part`,
addresses angular grip impulses by slot, and fixes the same ID lookup in the
bullet-hit fallback. Native compilation passed; headset acceptance is pending.

Generic prop/NPC grasps now create a persistent CryPhysics point constraint
between the selected rigid/articulated part and the existing dynamic VR hand
body. Each grasp captures both local attachment points. The held object's
position and velocity are never commanded directly. The previous spring NPC
drive and velocity-prescribed rigid prop drive have been removed.

While grasping, point constraints join hand, forearm, upper arm, clavicle and
torso segments. Each remains dynamic and driven by finite tracking/pose motors.
The hand motor targets the raw controller, independently of the rendered hold
pose, and is capped at 350 N. A bounded 12 Nm angular grip motor applies equal
and opposite torque to hand/object using their combined inverse inertia. This
keeps translation constrained while allowing a compliant wrist.

NPC living hosts consume accepted grasp-constraint impulses separately from
collision loads and internal skeletal constraints. Horizontal tensile hand
load also reacts on the player's living body; existing mass/slack-aware stick
limiting remains. Connected body segments never use tracking recenter position
writes. Palm calibration is frozen during a hold. Render IK preserves the
held contact even when the ordinary arm-reach clamp would miss it.

Release removes grasp/arm constraints before destroying any hand bodies.
Tracking/body loss also cleans them up. Distance alone does not release a hold.
Constraint frame initialization and zero-drift normalization were corrected
in the stock rigid-constraint code. Diagnostics: `[VRPhysicalGrip]` and bounded
`[VRPhysicalGripError]` samples measuring solver anchor separation/controller lag.

Native build passed; headset behavior is pending acceptance. Test one/two-hand
NPC dragging, controller rotation, a heavy prop against a wall, button release
and repeated grips. This is a physical grasp/upper-body checkpoint: locomotion
still uses PE_LIVING, arm joints are ball joints without anatomical angular
limits, and HMD/room-scale movement is not a fully dynamic character root.

## Feedback fixes: muzzle axis, hits, persistent NPC grip and weapon binding

The headset diagnostics showed accepted bullet impulses on individual bodies,
but the animation drives and canned pain pose dominated the visible response.
Bullet source 4 now weakens the hit body's motors for 0.55 s and its physical
parent for 0.30 s, smoothly returning from 15% strength. The damage callback
temporarily marks the actor so native animation dispatch suppresses only
`pain_*` clips; health and death processing still run. A shot can activate its
target rig for 1.2 s outside the normal 8 m contact radius.

CGF inspection of the installed Falcon and M4 TPV models showed that their
barrels point along object +X, while spitfire helper rotations are angled
effects transforms. Muzzle firing now cancels the helper rotation and uses
the bound object's +X axis, retaining the muzzle's world position.

The grab motor previously released any held body after 1 m of hand separation.
Active living NPC grips now stay latched until button release or tracking/body
loss. Stick movement that increases separation is limited by the NPC host
velocity, actual player/NPC mass ratio, and available 0.25 m hand slack.
Horizontal tensile hand impulses apply an opposing reaction to the player's
living body. A blocked/heavy NPC slows the player's locomotion while the hand
motor continues to pull. Other prop break rules remain. This is a bounded
force motor with locomotion load coupling, not a full two-body constraint.

Living AI actors in the held-weapon state restore missing TPV bindings after
legacy weapon transitions. Headset acceptance remains pending, especially
automatic-weapon visibility and barrel direction on both weapons.

## Test player protection and NPC muzzle firing checkpoint

`vr_test_player_invulnerable` defaults to 1 for this testing build. The local
non-AI player marks its entity, damage dispatch returns before Lua damage/death
callbacks, and player update maintains full health while alive. Set to 0 for
normal damage; this does not revive an already dead player or protect AI.

`vr_npc_muzzle_fire` defaults to 1. Handheld instant NPC weapons resolve the
actual TPV binding's spitfire helper and current bone pose, using the same
character angles/offset as rendering. The object's local +X barrel axis sets
the ray direction after cancelling helper rotation; weapon spread is preserved. There is no AI target
correction after resolving the muzzle. Vehicles, mounted guns, melee and
projectiles retain their existing firing path. Missing helpers/bindings retain
the original AI shot, and the helper matrix API now safely rejects absent
helpers instead of dereferencing null. Headset acceptance is pending: check
normal aim, then deflect the weapon hand and observe impacts on a nearby wall.

This follows the physical hand/weapon pose; it does not yet give NPC weapons
independent rigid bodies or implement physical recoil.

## Stage 2: bullet impulse checkpoint

Stage 1's second implementation passed the user's headset test: holding the
NPC's hand moves the actor through the world. Its accepted APK is preserved
as `research/physical_npc_stage1_accepted.apk`.

`CVRNPCDamageController` adds bullet impulses for live AI actors whose active
rig is attached to their living host. Instant weapon hits use damage, authored
weapon impact strength, normalized shot direction, hit position and actual
part mass. Resolve bone ids through their physical parents; living-collider
hits without a bone id use the nearest physical surface within 0.45 m.

Apply after damage callbacks, resolving the entity and physical body again:
callbacks may replace the rig with death physics or remove the target.
Recheck the selected body's mass limit. Accepted hits zero the three legacy
script impact multipliers so the same shot does not apply another impulse or
a large death kick. Zero entity impulses return before minimum-speed scaling.
Zero-damage repeated ray contacts do not add a second physical impulse.

Bullet source 4 bypasses the old impulse threshold and legacy AddImpact bone
exclusions. Alive active rigs report a separate damage load to the stage-one
root controller: 25% horizontal root contribution, independent of frame time,
without marking it as a grip. Fatal hits target the current articulated body
when it is available after callbacks. A deferred death-animation transition
can still discard momentum when replacing the rig later; recovery/death
handoff remains a later checkpoint.

Settings: `vr_npc_bullet_impulse` (1), `vr_npc_bullet_scale` (0.15 Ns per damage
unit), `vr_npc_bullet_max_impulse` (18 Ns),
`vr_npc_bullet_max_part_velocity` (2.5 m/s impulse-induced change limit).
Weapon weighting is clamped to 0.5–1.5. This is a configurable visual-response
approximation, not a ballistic momentum simulation. Diagnostics: `[VRNPCBullet]`.

Check shoulder/arm, chest/abdomen and leg shots, bursts and a fatal hit; verify
no excessive launch, no snap-back and preserved hand dragging. Melee,
projectile/explosion handling, procedural damage poses and balance loss are
not included in this checkpoint. Headset acceptance is pending.

## Current checkpoint: stage 1, second implementation

`CVRPhysicalNPCController` is a separate game-side module for live nearby AI
actors. The existing host-bound articulation, pose motors and animation
readback stay in their working coordinate system. No free pelvis is enabled.

The rig collects horizontal grip impulses (continuous interaction source 3)
and accepted solver impulses from contacts with dynamic rigid bodies,
including the player's VR body. Both are consumed once by the controller.
The contact recorder covers accepted initial CG, iterative impulses and
accepted final CG corrections; unprojection is excluded. Static ground,
angular/joint constraints and animation motors do not supply root loads.
The previous pre-solve Za/Ya transfer is removed to avoid double application
and animation feedback.

The controller applies bounded impulses to actual living velocity. During
interaction locomotion resistance is finite (1/s response, 1.5 m/s² maximum
horizontal AI acceleration); original AI requested velocity is preserved.
Normal locomotion regains control during the final 0.2 seconds of recovery.
Movement still uses the living collider's world collision solver and ordinary
entity/AI physics readback. There is no saved root position to return to.

Settings: `vr_npc_root` (1), `vr_npc_root_force` (450 N maximum combined root
force), `vr_npc_root_recovery` (0.45 s). Diagnostics: `[VRNPCRoot2]` includes
state, grip presence, root impulse, actual velocity and entity displacement.
The living root command times out after at most 0.2 seconds without refresh.
Disabling the controller, losing active physics or dying releases its state.

This is a compliant host-root approximation, not a free pelvis character:
vertical support, heading, balance and foot placement remain for subsequent
checkpoints. Headset acceptance must check pulling a living NPC several metres,
release without snap-back, shoulder/chest pushes, wall blocking and untouched
NPCs standing/walking without unintended drift. This attempt has not yet
passed headset acceptance.

## Headset result: stage 1 rejected and source changes reverted

The implementation described below is historical and is no longer enabled.
Headset testing reported teleporting NPCs, unstable ragdolls and a crash.
All six source files changed by stage 1 were restored to their pre-attempt
index versions, preserving the existing staged active ragdoll implementation.

`stage1_crash_log.txt` records SIGSEGV in `CTriMesh::PreparePolygon`, through
`CRigidEntity::GetMaxTimeStep` and `CPhysicalWorld::TimeStep`.
`stage1_physics_diagnostics.txt` contains repeated `broken-chain` resets;
`stage1_engine_log.txt` preserves the game log. The exact crash cause has not
been established. Stage 1 is not complete.

The next attempt needs a consistent coordinate-frame contract between host
pivot updates, free-root stepping, pose targets and physics readback. Record
pelvis/host transforms and recovery reasons, and isolate root authority to an
actual interaction before enabling it broadly for nearby AI actors.

## Stage 1: horizontal physical root

Active ragdolls now opt into `pe_params_articulated_body::bPhysicalRoot`.
The pelvis joint is ungrounded. Horizontal pelvis motion is not restored to
the animation root; AI requested velocity supplies a bounded traction drive
(2/s response, maximum 3 m/s² applied to the pelvis body). Other segment
position drives follow the physical pelvis horizontally. Their angular motors
continue to follow the animation pose.

The articulated solver publishes pelvis horizontal velocity plus root offset
correction (8/s, maximum 4 m/s) to its living host. The living collider resolves
that movement against the world, and existing entity physics readback updates
the entity and AI position. A scoped override preserves the original AI
requested velocity. Commands expire after 0.1 seconds without a solver update,
and are explicitly released when active ragdoll is disabled.

Ordinary host translations do not move physical bodies a second time. Existing
explicit teleport/heading handling remains. Large animation target error no
longer triggers broken-chain reset in this mode; separation of connected
joint pivots still does.

Vertical support remains animation driven relative to the living host. This
stage does not implement procedural stepping, balance failure, lifting whole
actors, or physics authority over heading. Physical root mode follows the
existing nearby/live AI activation policy, rather than a new interaction state
machine. Feet may slide. These are deliberate limits of the first headset
checkpoint, not the final character controller.

Headset acceptance checks:

1. Pull a living NPC by the arm for several metres; torso and entity follow.
2. Release it; it must not snap to the position before the pull. AI may resume
   walking toward its existing navigation goal.
3. Push the shoulder/chest; the actor must translate with the physical body.
4. Drag toward a wall; the living collider must remain blocked by the wall.
5. Compare standing and walking NPCs; check oscillation and unintended drift.

Diagnostics: `[VRNPCRoot]` reports pelvis/root error and follower velocity.

## Subsequent checkpoints

2. Bone-specific bullet impulses with bounded configurable strength.
3. Balance measurements and transition into free ragdoll.
4. Ground-aware procedural stepping.
5. Procedural damage pose and motor-strength changes.
6. Physical hand targets and arm reactions.
7. Locomotion/ragdoll/recovery transitions.

Each checkpoint is built and installed separately, then checked in the headset
before proceeding to the next stage.
