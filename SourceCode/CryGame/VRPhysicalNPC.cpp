#include "StdAfx.h"
#include "VRPhysicalNPC.h"
#include <VRPhysicsDiagnostics.h>

namespace
{
float Setting(IConsole* console, const char* name, float fallback, float lo, float hi)
{
	ICVar* value = console->GetCVar(name);
	const float result = value ? value->GetFVal() : fallback;
	return result >= lo && result <= hi ? result : fallback;
}
}

CVRPhysicalNPCController::CVRPhysicalNPCController(ISystem* system)
	: m_system(system), m_state(Normal), m_recoveryRemaining(0), m_nextTrace(0), m_startPosition(0,0,0)
{
	IConsole* console = system->GetIConsole();
	if (!console->GetCVar("vr_npc_root"))
		console->CreateVariable("vr_npc_root", "1", 0, "Enable compliant living NPC roots during VR interaction");
	if (!console->GetCVar("vr_npc_root_force"))
		console->CreateVariable("vr_npc_root_force", "450", 0, "Maximum horizontal force transferred to the NPC root (N)");
	if (!console->GetCVar("vr_npc_root_recovery"))
		console->CreateVariable("vr_npc_root_recovery", "0.45", 0, "Seconds of locomotion yielding after a physical interaction");
}

void CVRPhysicalNPCController::Reset(IEntity* entity)
{
	if (m_state != Normal && entity && entity->GetPhysics() && entity->GetPhysics()->GetType()==PE_LIVING) {
		pe_action_npc_root release;
		entity->GetPhysics()->Action(&release);
	}
	m_state = Normal; m_recoveryRemaining = 0;
}

void CVRPhysicalNPCController::Update(IEntity* entity, ICryCharInstance* character, float dt, bool enabled)
{
	IPhysicalEntity* host = entity ? entity->GetPhysics() : NULL;
	IPhysicalEntity* rig = character ? character->GetCharacterPhysics() : NULL;
	pe_status_npc_interaction input;
	// Consume even while disabled, so toggling the controller cannot replay loads.
	const bool valid = rig && rig->GetStatus(&input);
	if (!enabled || !host || host->GetType()!=PE_LIVING || !valid || dt<=0 || dt>.1f ||
		Setting(m_system->GetIConsole(),"vr_npc_root",1,0,1)<.5f) {
		Reset(entity); return;
	}
	Vec3 impulse = Vec3(input.gripImpulse)+Vec3(input.contactImpulse);
	impulse.z = 0;
	if (!(impulse.GetLengthSquared()<1E8f)) { Reset(entity); return; }
	// Ignore tiny collision noise. Grasping also yields AI with a stationary
	// hand; its zero-load case must not let navigation walk through the hold.
	const bool grasped = input.gripAge < .10f;
	const bool damaged = input.damageImpulse.len2() > 1E-8f;
	const bool loaded = impulse.GetLengthSquared() > sqr(12.0f*dt) || damaged;
	if (grasped || loaded) {
		if (m_state==Normal) m_startPosition = entity->GetPos();
		m_state = Interaction;
		m_recoveryRemaining = Setting(m_system->GetIConsole(),"vr_npc_root_recovery",.45f,.1f,1.0f);
	} else if (m_state != Normal) {
		m_state = Recovery;
		m_recoveryRemaining = max(0.0f,m_recoveryRemaining-dt);
		if (m_recoveryRemaining<=0) { Reset(entity); return; }
	} else return;
	const float limit = Setting(m_system->GetIConsole(),"vr_npc_root_force",450,0,1200)*dt;
	if (impulse.GetLengthSquared()>limit*limit) impulse *= limit/impulse.GetLength();
	if (!grasped && !loaded) impulse.zero();
	pe_action_npc_root root;
	root.impulse = impulse;
	// Bullet momentum is an event, not a continuous hand force: its root
	// contribution must not depend on rendering frame duration.
	if (input.damageImpulse.len2()<1E8f) root.impulse += input.damageImpulse*.25f;
	root.yieldTime = min(.2f,m_recoveryRemaining);
	host->Action(&root);
	const float now = m_system->GetITimer()->GetCurrTime();
	if (now >= m_nextTrace) {
		m_nextTrace = now+.25f;
		pe_status_dynamics motion; host->GetStatus(&motion);
		const Vec3 displacement = entity->GetPos()-m_startPosition;
		VRPhysicsTrace("[VRNPCRoot2] entity=%d state=%d grip=%d impulse=(%.3f %.3f) velocity=(%.3f %.3f) displacement=(%.3f %.3f)",
			entity->GetId(),(int)m_state,grasped,impulse.x,impulse.y,motion.v.x,motion.v.y,displacement.x,displacement.y);
	}
}
