#include "StdAfx.h"
#include "VRNPCDamage.h"
#include "WeaponClass.h"
#include "XPlayer.h"
#include <VRPhysicsDiagnostics.h>

namespace
{
float Setting(IConsole* console, const char* name, const char* initial, float low, float high)
{
	ICVar* var = console->GetCVar(name);
	if (!var) var = console->CreateVariable(name,initial,0);
	const float value = var->GetFVal();
	return value>=low && value<=high ? value : (float)atof(initial);
}

// Hit ids are bone ids, not physics-array indices. LODs and small bones can
// lack collision bodies: walk the actual bone hierarchy to a physical parent.
int PhysicalPart(ICryCharInstance* character, IPhysicalEntity* body, int bone)
{
	ICryCharModel* model = character ? character->GetModel() : NULL;
	if (!body || !model || bone<0 || bone>=model->NumBones()) return -1;
	ICryBone* current = character->GetBoneByName(model->GetBoneName(bone));
	for (int depth=0;current && depth<model->NumBones();++depth) {
		for (int i=0;i<model->NumBones();++i) if (character->GetBoneByName(model->GetBoneName(i))==current) {
			pe_status_pos part; part.partid = i;
			if (body->GetStatus(&part) && (part.pGeom || part.pGeomProxy)) return i;
			break;
		}
		ICryBone* parent = current->GetParent();
		if (parent==current) break;
		current = parent;
	}
	return -1;
}

int ClosestPhysicalPart(IPhysicalEntity* body, const Vec3& point)
{
	pe_status_nparts count;
	const int parts = body->GetStatus(&count);
	float nearest = .45f*.45f;
	int selected = -1;
	for (int i=0;i<parts;++i) {
		pe_status_pos pose; pose.ipart = i;
		if (!body->GetStatus(&pose)) continue;
		IGeometry* geometry = pose.pGeom ? pose.pGeom : pose.pGeomProxy;
		if (!geometry) continue;
		geom_world_data world; world.offset = pose.pos; world.R = matrix3x3f(pose.q); world.scale = pose.scale;
		int primitive=0, feature=0; vectorf closest[2];
		if (geometry->FindClosestPoint(&world,primitive,feature,point,point,closest)<0) continue;
		const float distance = (Vec3(closest[0])-point).GetLengthSquared();
		if (distance<nearest) {
			pe_params_part part; part.ipart = i;
			if (body->GetParams(&part)) { nearest = distance; selected = part.partid; }
		}
	}
	return selected;
}
}

bool CVRNPCDamageController::Prepare(ISystem* system, const SWeaponHit& hit, Hit& reaction)
{
	CPlayer* player = NULL;
	if (!hit.target || !hit.target->GetContainer() ||
		!hit.target->GetContainer()->QueryContainerInterface(CIT_IPLAYER,(void**)&player) ||
		!player || !player->IsAI() || !player->IsAlive() || !(hit.damage>=0 && hit.damage<1E6f) ||
		!(hit.dir.GetLengthSquared()>1E-8f && hit.dir.GetLengthSquared()<1E6f) ||
		!(hit.pos.GetLengthSquared()<1E12f)) return false;
	IConsole* console = system->GetIConsole();
	if (Setting(console,"vr_npc_bullet_impulse","1",0,1)<.5f) return false;
	// Hits outside the contact radius also need a brief active window.
	if (hit.damage>0 && !player->EnsurePhysicalNPCDamageRig()) return false;
	ICryCharInstance* character = hit.target->GetCharInterface()->GetCharacter(0);
	IPhysicalEntity* rig = character ? character->GetCharacterPhysics() : NULL;
	pe_params_articulated_body params;
	if (!rig || !rig->GetParams(&params) || !params.bExertImpulse || !params.pHost ||
		params.pHost!=hit.target->GetPhysics() || params.pHost->GetType()!=PE_LIVING) return false;
	int part = PhysicalPart(character,rig,hit.ipart);
	// A living-collider hit may carry no bone id. Resolve it against the actual
	// physical surfaces near the hit, never a guessed array index.
	if (part<0) part = ClosestPhysicalPart(rig,hit.pos);
	pe_status_dynamics dynamics; dynamics.partid = part;
	if (part<0 || !rig->GetStatus(&dynamics) || !(dynamics.mass>0 && dynamics.mass<10000)) return false;
	const float scale = Setting(console,"vr_npc_bullet_scale","0.15",0,2);
	const float maximum = Setting(console,"vr_npc_bullet_max_impulse","18",0,100);
	const float maxVelocity = Setting(console,"vr_npc_bullet_max_part_velocity","2.5",0,10);
	// Authored weapon impact strength distinguishes weapons, but never uses
	// the old death multipliers as unbounded momentum. This is a tunable
	// damage-to-impulse approximation, not a ballistic momentum simulation.
	const float weaponScale = max(.5f,min(1.5f,sqrtf(max(1.0f,(float)hit.iImpactForceMul)/100.0f)));
	const float magnitude = min(min(hit.damage*scale*weaponScale,maximum),dynamics.mass*maxVelocity);
	reaction.entity = hit.target->GetId(); reaction.part = part;
	reaction.position = hit.pos; reaction.impulse = GetNormalized(hit.dir)*magnitude;
	return true;
}

void CVRNPCDamageController::Apply(ISystem* system, const Hit& reaction)
{
	// The ray loop marks repeat contacts on the same actor with zero damage.
	// Their legacy impulse is suppressed, but no second physical hit is added.
	if (!(reaction.impulse.GetLengthSquared()>1E-12f)) return;
	// Damage callbacks can replace character physics or delete the target.
	// Resolve fresh objects after them; never retain a rigid-body pointer.
	IEntity* entity = system->GetIEntitySystem()->GetEntity(reaction.entity);
	if (!entity) return;
	ICryCharInstance* character = entity->GetCharInterface()->GetCharacter(0);
	IPhysicalEntity* body = character ? character->GetCharacterPhysics() : NULL;
	if ((!body || body->GetType()!=PE_ARTICULATED) && entity->GetPhysics() &&
		entity->GetPhysics()->GetType()==PE_ARTICULATED) body = entity->GetPhysics();
	const int part = PhysicalPart(character,body,reaction.part);
	pe_status_dynamics dynamics; dynamics.partid = part;
	if (part<0 || !body->GetStatus(&dynamics) || !(dynamics.mass>0)) return;
	Vec3 impulse = reaction.impulse;
	const float limit = dynamics.mass*Setting(system->GetIConsole(),"vr_npc_bullet_max_part_velocity","2.5",0,10);
	if (impulse.GetLengthSquared()>limit*limit) impulse *= limit/impulse.GetLength();
	pe_action_impulse action;
	action.partid = part; action.point = reaction.position; action.impulse = impulse;
	action.iSource = PHYS_IMPULSE_NPC_BULLET; action.iApplyTime = 0;
	const int applied = body->Action(&action);
	VRPhysicsTrace("[VRNPCBullet] entity=%d part=%d mass=%.3f impulse=%.3f applied=%d pos=(%.3f %.3f %.3f)",
		entity->GetId(),part,dynamics.mass,impulse.GetLength(),applied,reaction.position.x,reaction.position.y,reaction.position.z);
}
