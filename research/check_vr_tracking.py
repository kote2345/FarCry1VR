"""Numerical checks of tracking formulas; not a replacement for CryPhysics/Quest tests."""
import math
import random
from pathlib import Path


def simulate(dt, mass, force_limit):
    x, velocity = 1.0, 0.0
    stiffness, damping = 3600.0, 120.0
    gain = dt / (1 + damping * dt + stiffness * dt * dt)
    max_impulse = force_limit * dt
    peak = abs(x)
    # Equal physical duration at every dt, including slow force-limited loads.
    for _ in range(math.ceil(60 / dt)):
        impulse = (-x * stiffness - velocity * damping) * mass * gain
        impulse = max(-max_impulse, min(max_impulse, impulse))
        assert abs(impulse) <= max_impulse + 1e-12
        velocity += impulse / mass
        x += velocity * dt
        peak = max(peak, abs(x))
        assert math.isfinite(x) and math.isfinite(velocity)
    assert peak <= 1.000001, (dt, mass, peak)
    assert abs(x) < 1e-7 and abs(velocity) < 1e-7, (dt, mass, x, velocity)


def mul(a, b):
    w, x, y, z = a
    v, p, q, r = b
    return (w*v-x*p-y*q-z*r, w*p+x*v+y*r-z*q,
            w*q-x*r+y*v+z*p, w*r+x*q-y*p+z*v)


def inv(q):
    return (q[0], -q[1], -q[2], -q[3])


def rotate(q, v):
    return mul(mul(q, (0, *v)), inv(q))[1:]


def close(a, b):
    assert max(abs(x-y) for x, y in zip(a, b)) < 1e-10, (a, b)


rng = random.Random(20261005)
for dt in (.001, .003, .005, .01, .02, 1/72, 1/90):
    for mass in (.35, .6, 1.348, 12.5, 80):
        simulate(dt, mass, 350)
    # Angular inertia is in kg*m^2, not the object's mass in kg.
    for inertia in (.0001, .0007, .005, .02, .1, 1):
        simulate(dt, inertia, 40)
print('PASS: 77 bounded linear/angular servo cases, finite state and convergence')

for _ in range(1000):
    raw = [rng.uniform(-1, 1) for _ in range(4)]
    norm = math.sqrt(sum(x*x for x in raw))
    controller = tuple(x/norm for x in raw)
    raw = [rng.uniform(-1, 1) for _ in range(4)]
    norm = math.sqrt(sum(x*x for x in raw))
    hand = tuple(x/norm for x in raw)
    controller_pos = [rng.uniform(-5, 5) for _ in range(3)]
    hand_pos = [rng.uniform(-5, 5) for _ in range(3)]
    point = [rng.uniform(-5, 5) for _ in range(3)]
    local_controller = rotate(inv(controller), [a-b for a,b in zip(point,controller_pos)])
    local_hand = rotate(inv(hand), [a-b for a,b in zip(point,hand_pos)])
    captured = mul(inv(controller), hand)
    target_q = mul(controller, captured)
    target_pos = [p+a-b for p,a,b in zip(controller_pos,
                  rotate(controller,local_controller),rotate(target_q,local_hand))]
    close(target_q, hand)
    close(target_pos, hand_pos)
    close([a+b for a,b in zip(target_pos,rotate(target_q,local_hand))], point)
    # Joint frames captured in world space must initially coincide.
    close(mul(controller, mul(inv(controller), hand)), hand)
    close(mul(hand, mul(inv(hand), hand)), hand)
print('PASS: 1000 capture frames, coincident anchors, no acquisition pose jump')

root = Path(__file__).resolve().parents[1]
native = (root/'SourceCode/CryPhysics/rigidentity.cpp').read_text()
body = (root/'SourceCode/CryGame/VRBodyPhysics.cpp').read_text()
assert 'ApplyVRTracking(time_interval);' not in native
assert 'RegisterVRTrackingContacts(time_interval);' in native
assert 'm_flags &= ~ref_use_simple_solver;' in native
assert 'if (i<0) return 0;' in native
assert 'for (int hand=0;drive && hand<2;++hand)' in body
assert 'm_lastImpulseFrame' not in body
print('PASS: source guards, substep placement, full solver selection, render drive guard')
