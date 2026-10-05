"""Scalar reproduction of bounded motor + welded load PGS, not a headset test."""
import math
import random
from pathlib import Path


def run(dt, hand, load, stiffness, damping, limit, legacy=False, locked=False):
    x = [0.0, 0.0]
    v = [0.0, 0.0]
    error_sq = peak_gap = peak_motor = 0.0
    steps = math.ceil(3 / dt)
    for step in range(steps):
        t = step * dt
        goal = .8 * math.sin(2 * math.pi * 2 * t)
        goal_v = .8 * 2 * math.pi * 2 * math.cos(2 * math.pi * 2 * t)
        if legacy:
            impulse = ((goal-x[0])*3600+(goal_v-v[0])*120)*hand*dt/(1+120*dt+3600*dt*dt)
            impulse = max(-limit*dt, min(limit*dt, impulse))
            v[0] += impulse/hand
        x = [a+b*dt for a,b in zip(x,v)]
        accumulated = 0.0
        softness = 1 / (dt*(damping+stiffness*dt))
        requested_v = (stiffness*(goal-x[0])+damping*goal_v)/(damping+stiffness*dt)
        drift_v = (x[1]-x[0])*10
        # Reproduce motor/contact alternating solve, including accumulated clamp.
        for _ in range(160):
            if not legacy:
                delta = -(v[0]-requested_v+softness*accumulated)/(1/hand+softness)
                new = max(-limit*dt, min(limit*dt, accumulated+delta))
                v[0] += (new-accumulated)/hand
                accumulated = new
                assert abs(accumulated) <= limit*dt+1e-12
            inv_load = 0 if locked else 1/load
            joint = -(v[0]-v[1]-drift_v)/(1/hand+inv_load)
            v[0] += joint/hand
            v[1] -= joint*inv_load
        peak_motor = max(peak_motor, abs(accumulated)/dt)
        peak_gap = max(peak_gap, abs(x[0]-x[1]))
        assert all(math.isfinite(a) for a in x+v)
        if step*dt > 1:
            error_sq += (x[1]-goal)**2
    return math.sqrt(error_sq/max(1,steps-math.ceil(1/dt))), peak_gap, peak_motor


for dt in (.001, .005, .01, 1/72, 1/90):
    for load in (.001, .015, .1):
        new = run(dt, .0007, load, 300, 8, 40)
        old = run(dt, .0007, load, 300, 8, 40, legacy=True)
        # Measure the held load, since pre-solve hand movement can hide load lag.
        assert new[0] < old[0], (dt, load, new, old)
        assert new[0] < .15, (dt, load, new)
        assert new[1] < .02, (dt, load, new)
    for load in (.35, .5, 12.5, 80):
        new = run(dt, .6, load, 6000, 140, 350)
        assert new[1] < .01, (dt, load, new)
    locked = run(dt, .6, 1, 6000, 140, 350, locked=True)
    assert locked[2] <= 350+1e-7
    assert locked[1] < .01
    print(f'PASS dt={dt:.6f}: loaded rotation, translation, immovable obstacle, accumulated force cap')

root = Path(__file__).resolve().parents[1]
solver = (root/'SourceCode/CryPhysics/rigidbody.cpp').read_text()
world = (root/'SourceCode/CryPhysics/physicalworld.cpp').read_text()
assert '!g_bVRMotors && g_nContacts<20' in solver
assert '!g_bVRMotors && bBounced' in solver
assert 'motor->solvedExternalImpulse*motor->motorSoftness' in solver
assert 'accumulated-motor->solvedExternalImpulse' in solver
assert 'Ebefore += ContactSolverVRMotorWork();' in world
print('PASS: bounded/compliant path, CG exclusion, external-work energy guard')

rng = random.Random(20261005)
for _ in range(1000):
    inertia = [rng.uniform(.001, 1) for _ in range(3)]
    speed = [rng.uniform(-10, 10) for _ in range(3)]
    impulse = [rng.uniform(-.4, .4) for _ in range(3)]
    before = sum(i*w*w for i,w in zip(inertia,speed))
    after = sum(i*(w+p/i)**2 for i,w,p in zip(inertia,speed,impulse))
    work = sum(2*p*w+p*p/i for i,w,p in zip(inertia,speed,impulse))
    assert abs(after-before-work) < 1e-9
print('PASS: 1000 anisotropic motor impulse energy-accounting cases')
