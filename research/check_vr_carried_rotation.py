"""Reduced welded wrist/load checks; not a full CryPhysics or headset test."""
import math
import random
from pathlib import Path


def mul(a,b):
    w,x,y,z=a; v,p,q,r=b
    return (w*v-x*p-y*q-z*r,w*p+x*v+y*r-z*q,
            w*q-x*r+y*v+z*p,w*r+x*q-y*p+z*v)


def inv(q): return (q[0],-q[1],-q[2],-q[3])


rng=random.Random(20261006)
def quat():
    q=[rng.uniform(-1,1) for _ in range(4)]
    n=math.sqrt(sum(x*x for x in q))
    return tuple(x/n for x in q)


for _ in range(5000):
    hand,load,world_frame,hand_qfb,new_entity_target=[quat() for _ in range(5)]
    rel0=mul(inv(load),world_frame)
    rel1=mul(inv(hand),world_frame)
    desired=mul(mul(mul(new_entity_target,inv(hand_qfb)),rel1),inv(rel0))
    expected_frame=mul(mul(new_entity_target,inv(hand_qfb)),rel1)
    actual_frame=mul(desired,rel0)
    assert max(abs(a-b) for a,b in zip(expected_frame,actual_frame))<1e-12
print('PASS: 5000 arbitrary wrist/load/principal-inertia frame mappings')


def turn(dt,hand_inertia,load_inertia,iterations,load_side,rate_servo=False):
    x=[0.,0.]; w=[0.,0.]
    peak_torque=error=peak_stop=0.
    count=0
    for step in range(math.ceil(3/dt)):
        t=step*dt
        # Reversals followed by a stationary target: measure residual motion.
        goal=.8*math.sin(4*math.pi*t) if t<2 else 0.
        goal_w=.8*4*math.pi*math.cos(4*math.pi*t) if t<2 else 0.
        x=[a+b*dt for a,b in zip(x,w)]
        axis=1 if load_side else 0
        inertia=load_inertia if load_side else hand_inertia
        softness=1/(dt*(8+300*dt))
        requested=(300*(goal-x[axis])+8*goal_w)/(8+300*dt)
        if rate_servo:
            softness=0.
            distance=abs(goal-x[axis])
            acceleration=40/(hand_inertia+load_inertia)
            brake_step=acceleration*dt
            stopping_speed=2*acceleration*distance/(math.sqrt(brake_step**2+2*acceleration*distance)+brake_step)
            correction=math.copysign(min(distance/dt,stopping_speed),goal-x[axis])
            requested=max(-20.,min(20.,correction))
        drift=(x[1]-x[0])*10
        acc=0.
        for _ in range(iterations):
            delta=-(w[axis]-requested+acc*softness)/(1/inertia+softness)
            updated=max(-40*dt,min(40*dt,acc+delta))
            w[axis]+=(updated-acc)/inertia
            acc=updated
            impulse=-(w[0]-w[1]-drift)/(1/hand_inertia+1/load_inertia)
            w[0]+=impulse/hand_inertia; w[1]-=impulse/load_inertia
        peak_torque=max(peak_torque,abs(acc)/dt)
        assert all(math.isfinite(v) for v in x+w)
        if 1<t<2: error+=(x[1]-goal)**2; count+=1
        if t>2.5: peak_stop=max(peak_stop,abs(x[1]))
    return math.sqrt(error/max(1,count)),peak_stop,peak_torque


for dt in (.005,.01,1/72,1/90):
    for load in (.001,.015,.1):
        for iterations in (8,32,160):
            result=turn(dt,.0007,load,iterations,True)
            assert result[0]<.15 and result[1]<.005 and result[2]<=40+1e-8,(dt,load,iterations,result)
    print(f'PASS dt={dt:.6f}: reversal/stop, 3 load inertias, 3 solve budgets, 40 Nm cap')

for dt in (.001,.005,.01,1/72,1/90):
    for load in (.001,.015,.1):
        for iterations in (8,32,160):
            new=turn(dt,.0007,load,iterations,True,rate_servo=True)
            old=turn(dt,.0007,load,iterations,True)
            assert new[0]<.20,(dt,load,iterations,new,old)
            assert new[1]<1e-6 and new[2]<=40+1e-8,(dt,load,iterations,new)
    print(f'PASS dt={dt:.6f}: sampled-pose servo has bounded tracking lag, settles without residual spin, bounded torque')

# Static target from rest: finite torque must brake heavy loads before overshoot,
# rather than repeatedly changing velocity sign as an angular spring does.
for dt in (.001,.005,.01,.02,1/72,1/90):
    for inertia in (.001,.015,.1,1.,20.,80.):
        angle=velocity=0.
        acceleration=40/inertia
        for _ in range(math.ceil(30/dt)):
            angle+=velocity*dt
            distance=abs(1-angle)
            brake_step=acceleration*dt
            stopping=2*acceleration*distance/(math.sqrt(brake_step**2+2*acceleration*distance)+brake_step)
            request=math.copysign(min(20.,distance/dt,stopping),1-angle)
            impulse=max(-40*dt,min(40*dt,(request-velocity)*inertia))
            velocity+=impulse/inertia
            assert -1e-10<=angle<=1+1e-10,(dt,inertia,angle)
            assert abs(velocity)<=20+1e-10
            assert abs(impulse)<=40*dt+1e-10
        assert abs(angle-1)<1e-7 and abs(velocity)<1e-7,(dt,inertia,angle,velocity)
print('PASS: 36 static light/heavy rotation cases, no overshoot, zero residual spin')

# Two hands must retain the owner exclusion until the last actual release.
for first in (0,1):
    mask=0
    for hand in (0,1,0,1): mask|=1<<hand
    assert mask==3
    mask&=~(1<<first); assert mask==1<<(1-first)
    mask&=~(1<<first); assert mask==1<<(1-first)
    mask&=~(1<<(1-first)); assert mask==0

def excluded(mask,carrier,candidate,foreign_type,carrier_data,candidate_data):
    return bool(mask and carrier is not None and
                (candidate==carrier or (foreign_type==100 and carrier_data is not None and
                                       candidate_data==carrier_data)))

for mask in (1,2,3):
    assert excluded(mask,10,10,0,'player','player')
    assert excluded(mask,10,11,100,'player','player')
    assert not excluded(mask,10,12,100,'player','another-player')
    assert not excluded(mask,10,13,0,'player','prop')
    assert not excluded(mask,10,14,0,'player','wall')
assert not excluded(0,10,10,0,'player','player')
assert not excluded(1,None,11,100,'player','player')

root=Path(__file__).resolve().parents[1]
native=(root/'SourceCode/CryPhysics/rigidentity.cpp').read_text()
living=(root/'SourceCode/CryPhysics/livingentity.cpp').read_text()
game=(root/'SourceCode/CryGame/VRPhysicalWeapons.cpp').read_text()
limit=game.split('void CVRPhysicalWeapons::LimitNPCGripMovement')[1].split('void CVRPhysicalWeapons::UpdateProp')[0]
assert 'controller*state.propHandAnchor' not in limit
assert 'reach-.02f' in limit and 'GetPhysicalWrist' in limit
assert 'IgnoreVRGripCollision(pentlist[i])' in native
assert '!IgnoreVRGripCollision(pentlist[ient])' in native
assert living.count('IsVRGripCarrier(this)')>=3
assert 'm_vrCarrierHands &= ~(1u<<action->hand)' in native
assert 'peer->m_pColliderContacts[j] = 0' in native
assert 'const constraint_info& frame' in native
assert 'if (axis) motor.K = driven->m_body.Iinv;' in native
assert 'carrier.enabled = 0' in game
assert 'motor.vreq = correction;' in native
assert 'motor.motorSoftness = 0;' in native
assert 'if (axis && m_vrTracking.angularVelocityDrive)' in native
assert 'target.angularVelocityDrive = constrainedHand ? 1 : 0;' in (root/'SourceCode/CryGame/VRBodyPhysics.cpp').read_text()
print('PASS: two-hand lifecycle, contact/sweep/ground filtering, anatomical reach retained')
