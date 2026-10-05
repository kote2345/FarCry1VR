"""Regression checks for capture-lag contamination of anatomical grip frames."""
import math
import random
from pathlib import Path
from check_vr_tracking import mul, inv, rotate, close

rng = random.Random(61005)
def quaternion():
    raw = [rng.uniform(-1,1) for _ in range(4)]
    norm = math.sqrt(sum(v*v for v in raw))
    return tuple(v/norm for v in raw)

for _ in range(1500):
    controller_q = quaternion()
    reference_q = quaternion()
    controller_pos = [rng.uniform(-2,2) for _ in range(3)]
    length = rng.uniform(.04,.15)
    # Ideal hand geometry starts at the wrist (the controller's translation).
    half = rotate(reference_q, (0,0,length*.5))
    reference_pos = [p+d for p,d in zip(controller_pos,half)]
    offset_q = mul(inv(controller_q),reference_q)
    offset_pos = rotate(inv(controller_q), [a-b for a,b in zip(reference_pos,controller_pos)])
    next_controller_q = quaternion()
    next_pos = [rng.uniform(-2,2) for _ in range(3)]
    target_q = mul(next_controller_q,offset_q)
    target_pos = [p+d for p,d in zip(next_pos,rotate(next_controller_q,offset_pos))]
    wrist = [p+d for p,d in zip(target_pos,rotate(target_q,(0,0,-length*.5)))]
    close(wrist,next_pos)
    # Rendering recovers the same wrist/controller frame from physical hand COM.
    visual_q = mul(target_q,inv(offset_q))
    close([p-d for p,d in zip(target_pos,rotate(visual_q,offset_pos))],next_pos)
    # A disturbed capture pose must never become an anatomical attachment frame.
    disturbed_q = quaternion()
    disturbed_pos = [p+rng.uniform(-.3,.3) for p in reference_pos]
    object_q = quaternion()
    relative_object = mul(inv(disturbed_q),object_q)
    close(mul(disturbed_q,relative_object),object_q)
    # Converting a known anatomical endpoint to world and back preserves it,
    # independently of the physical body's capture offset/orientation.
    local = (0,0,-length*.5)
    endpoint = [p+d for p,d in zip(disturbed_pos,rotate(disturbed_q,local))]
    close(rotate(inv(disturbed_q),[p-d for p,d in zip(endpoint,disturbed_pos)]),local)

root = Path(__file__).resolve().parents[1]
body = (root/'SourceCode/CryGame/VRBodyPhysics.cpp').read_text()
grip = (root/'SourceCode/CryGame/VRPhysicalWeapons.cpp').read_text()
assert 'joint.pt[0] = joint.pt[1] = joints[buddies[link]]' not in body
assert 'state.pose*state.gripHandOffset' in grip
assert 'state.gripHandRotation = !Quat(Matrix33(state.pose))*Quat(palm.q)' not in grip
assert 'Quat(handPose.q)*state.gripObjectRotation' in grip
print('PASS: 1500 arbitrary wrist rotations, anatomical endpoints, visual frames, and capture-pose disturbances')
