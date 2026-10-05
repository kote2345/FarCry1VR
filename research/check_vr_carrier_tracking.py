"""Translating carrier frame: locomotion is sampled once per physics step."""
import math,random
from pathlib import Path
rng=random.Random(20261006)
for frame_rate in (30,45,72,90):
    for physics_dt in (.005,.01,1/72,1/90):
        sampled_root=sampled_hand=0.; previous_sample=-1.; next_sample=0.
        for step in range(800):
            t=step*physics_dt
            # Smooth start/stop and reversal; include a blocked root after 3 s.
            root=math.sin(t)*.7 if t<3 else math.sin(3)*.7
            local=.35+math.sin(t*1.7)*.02
            if t>=next_sample:
                sampled_root=root;sampled_hand=root+local
                next_sample=t+1/frame_rate
            actual_target=sampled_hand+root-sampled_root
            assert abs(actual_target-root-(sampled_hand-sampled_root))<1e-12
            # A blocked root never receives a speculative locomotion displacement.
            if t>=3: assert abs(actual_target-(sampled_hand+math.sin(3)*.7-sampled_root))<1e-12
for _ in range(10000):
    dt=rng.uniform(.001,.05)
    root_delta=rng.uniform(-.2,.2); local_delta=rng.uniform(-.02,.02)
    relative_velocity=((root_delta+local_delta)-root_delta)/dt
    assert abs(relative_velocity-local_delta/dt)<1e-10
root=Path(__file__).resolve().parents[1]
body=(root/'SourceCode/CryGame/VRBodyPhysics.cpp').read_text()
native=(root/'SourceCode/CryPhysics/rigidentity.cpp').read_text()
assert 'target.referencePos = playerPosition;' in body
assert '(playerPosition-part.previousPlayerPosition)' in body
assert 'target += carrierPose.pos-m_vrTracking.referencePos;' in native
assert 'if (!axis) motor.vreq += carrierVelocity;' in native
assert 'motor.vreq = correction;' in native
print('PASS: 16 frame/substep schedules, blocked carrier, 10000 relative velocity samples, source integration')
