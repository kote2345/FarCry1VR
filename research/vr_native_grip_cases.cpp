#undef DECLARE_MTXNxM_POOL
#define DECLARE_MTXNxM_POOL(ftype,sz) template<> ftype matrix_tpl<ftype>::mtx_pool[sz] = {}; template<> int matrix_tpl<ftype>::mtx_pool_pos=0; template<> int matrix_tpl<ftype>::mtx_pool_size=sz;
#undef DECLARE_VECTORN_POOL
#define DECLARE_VECTORN_POOL(ftype,sz) template<> ftype vectorn_tpl<ftype>::vecn_pool[sz] = {}; template<> int vectorn_tpl<ftype>::vecn_pool_pos=0; template<> int vectorn_tpl<ftype>::vecn_pool_size=sz;
#include "../SourceCode/CryPhysics/matrixnm.cpp"

#include <VRGripMath.h>
void motorContact(entity_contact& c,RigidBody& b,RigidBody& ground,const vectorf& goal,const quaternionf& qgoal,const vectorf& goalW,float dt,bool angular,const RigidBody* paired=nullptr,const vectorf& carrierVelocity=vectorf(zero)) {
    c=entity_contact(); c.pbody[0]=&b;c.pbody[1]=&ground;
    c.pt[0]=c.pt[1]=b.pos;c.n.Set(1,0,0);c.C.SetIdentity();
    c.flags=contact_constraint_3dof|contact_vr_motor|(angular?contact_angular:0);
    c.K.SetIdentity();c.K*=b.Minv;if(angular)c.K=b.Iinv;
    if(angular) {
        quaternionf e=qgoal*!b.q;if(e.w<0){e.w=-e.w;e.v=-e.v;}
        float d=e.v.len();c.vreq=d>1e-6f?e.v*(2*acos_tpl(min(1.f,e.w))/(d*dt)):vectorf(zero);
        if(paired && d>1e-6f) {
            float distance=2*acos_tpl(min(1.f,e.w)); vectorf direction=e.v/d;
            matrix3x3f inertia=b.Iinv; inertia.Invert(); matrix3x3f wrist=paired->Iinv; wrist.Invert();
            float acceleration=40/max(1e-8f,(inertia*direction).len()+(wrist*direction).len());
            float brake=acceleration*dt;
            float speed=2*acceleration*distance/(sqrt_tpl(brake*brake+2*acceleration*distance)+brake);
            c.vreq=direction*min(distance/dt,speed);
        }
        // The sampled predicted pose is the goal; no second prediction.
        if(c.vreq.len()>20)c.vreq*=20/c.vreq.len();
        c.motorImpulseLimit=40*dt;
    }else{
        float k=6000,d=140;c.motorSoftness=1/(dt*(d+k*dt));
        c.vreq=(goal-b.pos)*(k/(d+k*dt))+carrierVelocity;c.motorImpulseLimit=600*dt;
        for(int i=0;i<3;++i)c.K(i,i)+=c.motorSoftness;
    }
    RegisterContact(&c);
}
void jointContact(entity_contact& c,RigidBody& a,RigidBody& b,const vectorf& pa,const vectorf& pb,bool angular) {
    c=entity_contact();c.pbody[0]=&a;c.pbody[1]=&b;c.C.SetIdentity();c.n.Set(1,0,0);
    c.flags=contact_constraint_3dof|(angular?contact_angular:0);
    if(angular){c.K=a.Iinv+b.Iinv;c.pt[0]=a.pos;c.pt[1]=b.pos;c.vreq.zero();}
    else {c.pt[0]=a.pos+a.q*pa;c.pt[1]=b.pos+b.q*pb;c.K.SetZero();
        a.GetContactMatrix(c.pt[0]-a.pos,c.K);b.GetContactMatrix(c.pt[1]-b.pos,c.K);c.vreq=(c.pt[1]-c.pt[0])*10;}
    RegisterContact(&c);
}
vectorf geometryProbe() {
 RigidBody hand; quaternionf q;q.SetIdentity(); float r=.04f,l=.08f;
 phys_geometry geometry; SphereMassProperties(r,&geometry);
 float sv=geometry.V,si=geometry.Ibody.x;
 if(fabsf(si/sv-.4f*r*r)>1e-7f) { printf("FAIL sphere inertia units\n"); exit(1); }
 hand.Create(vectorf(0,0,-l*.5f),vectorf(si,si,si),q,sv,.2f,q,vectorf(zero));
 hand.Add(vectorf(0,0,l*.5f),vectorf(si,si,si),q,sv,.2f);
 float cv=gf_PI*r*r*l;
 hand.Add(vectorf(zero),vectorf(cv*(3*r*r+l*l)/12,cv*(3*r*r+l*l)/12,cv*r*r*.5f),q,cv,.2f);
 printf("hand: mass %.3f inertia=(%.6f %.6f %.6f) inv=(%.3f %.3f %.3f) pos=(%.5f %.5f %.5f)\n",hand.M,hand.Ibody.x,hand.Ibody.y,hand.Ibody.z,hand.Iinv(0,0),hand.Iinv(1,1),hand.Iinv(2,2),hand.pos.x,hand.pos.y,hand.pos.z);
 return vectorf(hand.Ibody.x,hand.Ibody.y,hand.Ibody.z);
}
int main(int argc,char** argv) {
 const float fixtureDt=argc>1?float(atof(argv[1])):.01f;
 const float loadMass=argc>2?float(atof(argv[2])):.5f;
 const bool staticTurn=argc>3 && strcmp(argv[3],"static")==0;
 const bool walking=argc>3 && strcmp(argv[3],"walk")==0;
 vectorf handInertia=geometryProbe();
 for(int k=0;k<20000;++k) {
 float a=k*.731f,b=k*.193f; vectorf normals[2]={vectorf(cosf(a),sinf(a),0),vectorf(cosf(b),sinf(b),0)};
 float limits[2]={float(k%7)*.1f,float(k%11)*.1f}; bool enabled[2]={true,true};
 vectorf v(sinf(k*.13f)*5,cosf(k*.17f)*5,.7f),before=v; ProjectVRGripVelocity(v,normals,limits,enabled);
 if(v*normals[0]>limits[0]+.002f || v*normals[1]>limits[1]+.002f || v.len2()>before.len2()+.002f || v.z!=before.z) return 2;
 }
 printf("PASS 20000 native two-hand velocity projections\n");
    RigidBody bodies[6],ground;quaternionf q;q.SetIdentity();
    vectorf endpoints[6]={vectorf(0,0,1.3f),vectorf(.2f,0,1.55f),vectorf(.4f,.05f,1.3f),vectorf(.4f,-.25f,1.25f),vectorf(.4f,-.33f,1.25f),vectorf(.4f,-.47f,1.25f)};
    vectorf goals[6];quaternionf rotations[6];
    for(int i=0;i<5;++i){
        goals[i]=(endpoints[i]+endpoints[i+1])*.5f;rotations[i]=GetRotationV0V1(vectorf(0,0,1),(endpoints[i+1]-endpoints[i]).normalized());
        float m=i==0?15:i==4?.6f:i==3?1.2f:2;
        float length=(endpoints[i+1]-endpoints[i]).len();
        bodies[i].Create(goals[i],(i==4?handInertia:vectorf(m*(.001f+length*length/12),m*(.001f+length*length/12),m*.001f)),rotations[i],1,1,rotations[i],goals[i]);
        bodies[i].M=m;bodies[i].Minv=1/m;
    }
    goals[5]=endpoints[5];rotations[5]=rotations[4];
    bodies[5].Create(goals[5],vectorf(.012f,.012f,.008f),rotations[5],1,1,rotations[5],goals[5]);bodies[5].M=loadMass;bodies[5].Minv=1/loadMass;
    vectorf localA[5],localB[5];
    for(int i=0;i<5;++i){vectorf pt=endpoints[i+1];localA[i]=!bodies[i].q*(pt-bodies[i].pos);localB[i]=!bodies[i+1].q*(pt-bodies[i+1].pos);}
    SolverSettings settings={};settings.nMaxMCiters=6000;settings.accuracyMC=.005f;settings.minSeparationSpeed=.02f;
    float rms=0,peak=0,carrierX=0,steadyLag=0;
    for(int step=0;step<int(3/fixtureDt);++step){float dt=fixtureDt,t=step*dt;
        vectorf carry(walking && t>.2f && t<2?2.f:0,0,0); carrierX+=carry.x*dt;
        float angle=staticTurn?.8f:(t<2?.8f*sinf(4*gf_PI*t):0), speed=t<2?.8f*4*gf_PI*cosf(4*gf_PI*t):0;
        if(walking) angle=0;
        quaternionf desired=GetRotationAA(angle,vectorf(0,0,1))*rotations[4];
        for(auto& b:bodies)b.Step(dt);
        InitContactSolver(dt);entity_contact joints[6],motors[12];
        for(int i=0;i<5;++i)jointContact(joints[i],bodies[i],bodies[i+1],localA[i],localB[i],false);
        jointContact(joints[5],bodies[4],bodies[5],vectorf(zero),vectorf(zero),true);
        for(int i=0;i<6;++i){if(i==5)continue;
            vectorf target=goals[i];if(i==4)target=endpoints[4]+desired*vectorf(0,0,(endpoints[5]-endpoints[4]).len()*.5f);
            target.x+=carrierX;
            motorContact(motors[i*2],bodies[i],ground,target,rotations[i],vectorf(zero),dt,false,nullptr,carry);
            if(i<4)motorContact(motors[i*2+1],bodies[i],ground,goals[i],rotations[i],vectorf(zero),dt,true);
            else motorContact(motors[i*2+1],bodies[5],ground,goals[5],desired,vectorf(0,0,speed),dt,true,&bodies[4]);
        }
        InvokeContactSolver(dt,&settings);
        quaternionf e=desired*!bodies[5].q;float error=2*acos_tpl(min(1.f,fabsf(e.w)));
        if(staticTurn) { quaternionf rotation=bodies[5].q*!rotations[4]; float actual=2*atan2f(rotation.v.z,rotation.w); if(actual>.805f || actual<-.005f) { printf("FAIL overshoot %.6f\n",actual);return 3;} }
        if(walking && t>.8f && t<1.9f) steadyLag=max(steadyLag,fabsf(bodies[5].pos.x-goals[5].x-carrierX));
        peak=max(peak,error);if(t>1&&t<2)rms+=error*error;
        if(step==0 || step==int(2.5f/fixtureDt))printf("step %d error %.5f loadW %.3f handW %.3f torque %.3f\n",step,error,bodies[5].w.z,bodies[4].w.z,motors[9].solvedExternalImpulse.len()/dt);
    }
    printf("full chain peak %.6f rms %.6f\n",peak,sqrtf(rms/99));
    if(walking) {printf("walking steady load lag %.6f m\n",steadyLag); if(steadyLag>.01f) return 4;}
    return peak<(staticTurn?.81f:.20f) && bodies[5].w.len()<.03f?0:1;
}
