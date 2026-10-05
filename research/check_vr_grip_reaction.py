"""Check that arm reach is enforced in locomotion, without root/load impulses."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
game=(root/'SourceCode/CryGame/VRPhysicalWeapons.cpp').read_text()
living=(root/'SourceCode/CryPhysics/livingentity.cpp').read_text()
body=(root/'SourceCode/CryGame/VRBodyPhysics.cpp').read_text()
limit=game.split('void CVRPhysicalWeapons::LimitNPCGripMovement')[1].split('void CVRPhysicalWeapons::UpdateProp')[0]
assert 'pe_action_impulse' not in limit
assert 'GetPhysicalWrist' in limit and 'ProjectVRGripVelocity' in limit
assert 'playerBody->Action(&limit)' in limit
assert 'LimitVRLocomotion(m_vel);' in living
assert living.index('LimitVRLocomotion(m_vel);')<living.index('move += m_vel*time_interval;')
assert 'm_vrLocomotionAge>.1f' in living or 'm_vrLocomotionAge > .1f' in living
assert 'hand<0 && touching' in body or 'hand < 0 && touching' in body
print('PASS: living velocity authority, no manual launch impulses, real wrist reach, torso contact only')
