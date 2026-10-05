#include "StdAfx.h"
#include "VRBodyPhysics.h"
#include "VRPhysicalWeapons.h"
#include <primitives.h>
#include <VRPhysicsDiagnostics.h>

namespace
{
bool Descendant(ICryBone* bone, ICryBone* root)
{
	while (bone && bone != root) bone = bone->GetParent();
	return bone == root;
}

void RotateChain(ICryCharInstance* character, ICryBone* root,
	const Vec3& pivot, const Vec3& from, const Vec3& to)
{
	if (from.GetLengthSquared() < 1.0e-8f || to.GetLengthSquared() < 1.0e-8f) return;
	const Matrix33 rotation(GetRotationV0V1<float>(GetNormalized(from), GetNormalized(to)));
	Matrix34 delta(rotation); delta.SetTranslation(pivot - rotation * pivot);
	for (int i = 0; i < character->GetModel()->NumBones(); ++i)
	{
		ICryBone* bone = character->GetBoneByName(character->GetModel()->GetBoneName(i));
		if (!Descendant(bone, root)) continue;
		// Head remains anchored to HMD; torso contact must not shift the eyes.
		ICryBone* head = character->GetBoneByName("Bip01 Head");
		if (head && Descendant(bone, head)) continue;
		const Matrix44 pose = GetTransposed44(Matrix44(delta) * GetTransposed44(bone->GetAbsoluteMatrix()));
		const_cast<Matrix44&>(bone->GetAbsoluteMatrix()) = pose;
	}
}
}

struct CVRBodyPhysics::Part
{
	Part(const char* first, const char* second, float r, bool root, float weight)
		: a(first), b(second), radius(r), mass(weight), length(0), body(NULL), physics(NULL),
		previousValid(false), collisionEnabled(true), previousCenter(0,0,0), previousPlayerPosition(0,0,0) {}
	~Part()
	{
		if (physics && body) physics->DestroyPhysicalEntity(body);
		if (physics) for (size_t i = 0; i < geometry.size(); ++i)
			physics->GetGeomManager()->UnregisterGeometry(geometry[i]);
	}
	const char *a, *b;
	float radius, mass, length;
	IPhysicalEntity* body;
	IPhysicalWorld* physics;
	std::vector<phys_geometry*> geometry;
	bool previousValid, collisionEnabled;
	Vec3 previousCenter, previousPlayerPosition;
	Matrix34 trackingPose;
};

CVRBodyPhysics::CVRBodyPhysics()
{
	m_handBlocked[0] = m_handBlocked[1] = false;
	m_reachValid[0] = m_reachValid[1] = false;
	memset(m_armConstraints,0,sizeof(m_armConstraints));
	m_parts.push_back(new Part("Bip01 Spine", "Bip01 Spine2", .14f, false, 15));
	const char* names[][2] = {
		{"Bip01 L Clavicle", "Bip01 L UpperArm"}, {"Bip01 R Clavicle", "Bip01 R UpperArm"},
		{"Bip01 L UpperArm", "Bip01 L Forearm"}, {"Bip01 R UpperArm", "Bip01 R Forearm"},
		{"Bip01 L Forearm", "Bip01 L Hand"}, {"Bip01 R Forearm", "Bip01 R Hand"},
		{"Bip01 L Hand", "Bip01 L Finger2"}, {"Bip01 R Hand", "Bip01 R Finger2"}
	};
	const float radii[] = {.055f,.055f,.055f,.055f,.045f,.045f,.04f,.04f};
	for (int i = 0; i < 8; ++i) m_parts.push_back(new Part(names[i][0], names[i][1], radii[i], false, i >= 6 ? .6f : i>=4 ? 1.2f : 2.0f));
}

CVRBodyPhysics::~CVRBodyPhysics()
{
	const std::vector<Vec3> empty;
	UpdateArmConstraints(0,false,empty); UpdateArmConstraints(1,false,empty);
	for (size_t i = 0; i < m_parts.size(); ++i) delete m_parts[i];
}

bool CVRBodyPhysics::GetHandCollisionTarget(int hand, Matrix34& target) const
{
	if (!m_handBlocked[hand]) return false;
	target.SetTranslation(m_handPosition[hand]);
	return true;
}

IPhysicalEntity* CVRBodyPhysics::GetHandPhysics(int hand) const
{
	return hand>=0 && hand<2 ? m_parts[7+hand]->body : NULL;
}

bool CVRBodyPhysics::GetHandTrackingPose(int hand, Matrix34& target) const
{
	if (hand<0 || hand>1 || !m_parts[7+hand]->previousValid) return false;
	target = m_parts[7+hand]->trackingPose;
	return true;
}

bool CVRBodyPhysics::GetArmReach(int hand, IEntity* player, Vec3& shoulder, float& length) const
{
	if (hand<0 || hand>1 || !player || !m_reachValid[hand]) return false;
	shoulder = player->GetPos()+m_shoulderOffset[hand];
	length = m_parts[3+hand]->length+m_parts[5+hand]->length;
	return length>.1f;
}

void CVRBodyPhysics::ReleaseHandConstraints(int hand)
{
	const std::vector<Vec3> empty;
	UpdateArmConstraints(hand,false,empty);
}

bool CVRBodyPhysics::GetPhysicalWrist(int hand, Vec3& position, Vec3& velocity) const
{
	if (hand<0 || hand>1) return false;
	const Part& part = *m_parts[7+hand];
	pe_status_pos pose; pe_status_dynamics dynamics;
	if (!part.body || !part.body->GetStatus(&pose) || !part.body->GetStatus(&dynamics)) return false;
	position = Vec3(pose.pos)+Matrix33(pose.q)*Vec3(0,0,-part.length*.5f);
	velocity = Vec3(dynamics.v)+(Vec3(dynamics.w) ^ (position-Vec3(dynamics.centerOfMass)));
	return true;
}

void CVRBodyPhysics::UpdateArmConstraints(int hand, bool enabled, const std::vector<Vec3>& joints)
{
	// Hand -> forearm -> upper arm -> clavicle -> torso. Ball joints keep
	// anatomical attachment points together while finite motors control pose.
	const int owners[4] = {5+hand,3+hand,1+hand,0};
	const int buddies[4] = {7+hand,5+hand,3+hand,1+hand};
	for (int link=0;link<4;++link) {
		IPhysicalEntity* owner = m_parts[owners[link]]->body;
		IPhysicalEntity* buddy = m_parts[buddies[link]]->body;
		if (!enabled) {
			if (owner && m_armConstraints[hand][link]) {
				pe_action_remove_constraint remove; remove.idConstraint = m_armConstraints[hand][link];
				owner->Action(&remove);
			}
			m_armConstraints[hand][link] = 0;
		} else if (!m_armConstraints[hand][link] && owner && buddy && joints.size()==m_parts.size()) {
			pe_action_add_constraint joint; joint.pBuddy = buddy;
			pe_status_pos ownerPose, buddyPose;
			if (!owner->GetStatus(&ownerPose) || !buddy->GetStatus(&buddyPose)) continue;
			// Anatomical attachment frames come from the tracked reference pose,
			// not from the current lag of either physical segment. Capturing one
			// shared tracked WORLD point used to bake that lag into both joints.
			const Vec3 point = joints[buddies[link]];
			const Vec3 ownerLocal = link<3 ? Vec3(0,0,m_parts[owners[link]]->length*.5f) :
				m_parts[owners[link]]->trackingPose.GetInverted()*point;
			const Vec3 buddyLocal(0,0,-m_parts[buddies[link]]->length*.5f);
			joint.pt[0] = Vec3(ownerPose.pos)+Matrix33(ownerPose.q)*ownerLocal;
			joint.pt[1] = Vec3(buddyPose.pos)+Matrix33(buddyPose.q)*buddyLocal;
			m_armConstraints[hand][link] = max(0,owner->Action(&joint));
		}
	}
}

void CVRBodyPhysics::Resolve(ICryCharInstance* character, Matrix34& world,
	ISystem* system, IEntity* player, float deltaTime, CVRPhysicalWeapons* interactions)
{
	if (!character || !character->GetModel() || !system->GetIPhysicalWorld()) return;
	IPhysicalWorld* physics = system->GetIPhysicalWorld();
	// Game update submits a target even on slow frames. Integration belongs
	// to the physics substeps; rendering calls this with zero delta time.
	const bool drive = deltaTime > 0;
	const bool connectedBody = interactions && (interactions->HasPhysicalGrip(0) || interactions->HasPhysicalGrip(1));
	m_handBlocked[0] = m_handBlocked[1] = false;
	Vec3 playerPosition = player->GetPos();
	pe_status_pos playerPose;
	if (player->GetPhysics() && player->GetPhysics()->GetStatus(&playerPose)) playerPosition = playerPose.pos;
	Vec3 bodyVelocity(0,0,0);
	pe_status_dynamics playerDynamics;
	if (player->GetPhysics() && player->GetPhysics()->GetStatus(&playerDynamics)) bodyVelocity = playerDynamics.v;
	// Snapshot desired skeleton before any segment corrects the visible pose.
	std::vector<Vec3> targetA(m_parts.size()), targetB(m_parts.size());
	for (size_t i = 0; i < m_parts.size(); ++i)
	{
		ICryBone* first = character->GetBoneByName(m_parts[i]->a);
		ICryBone* second = character->GetBoneByName(m_parts[i]->b);
		if (first) targetA[i] = world*first->GetBonePosition();
		if (second) targetB[i] = world*second->GetBonePosition();
	}
	for (int hand=0;drive && hand<2;++hand) {
		if (character->GetBoneByName(hand==0 ? "Bip01 L UpperArm" : "Bip01 R UpperArm")) {
			m_shoulderOffset[hand] = targetA[3+hand]-player->GetPos();
			m_reachValid[hand] = true;
		}
	}
	for (size_t index = 0; index < m_parts.size(); ++index)
	{
		Part& part = *m_parts[index];
		ICryBone* first = character->GetBoneByName(part.a);
		ICryBone* second = character->GetBoneByName(part.b);
		if (!first || !second) {
			static int missingSamples = 0;
			if (missingSamples++<18) VRPhysicsTrace("[VRBodyMissingBone] first=%s second=%s found=%d/%d",part.a,part.b,first!=NULL,second!=NULL);
			continue;
		}
		Vec3 a = targetA[index], b = targetB[index];
		const int hand = !strcmp(part.a,"Bip01 L Hand") ? 0 : !strcmp(part.a,"Bip01 R Hand") ? 1 : -1;
		const bool constrainedHand = hand>=0 && interactions && interactions->HasPhysicalGrip(hand);
		// The update supplies raw tracked IK for the entire arm. Rendering
		// supplies the held contact pose with zero drive time. Do not transform
		// the hand again or let held-pose arm motors oppose its tracking drive.
		Vec3 center = (a+b)*.5f;
		const Vec3 axis = b-a;
		Quat rotation(1,0,0,0);
		if (axis.GetLengthSquared() > 1.e-8f) rotation = Quat(GetRotationV0V1<float>(Vec3(0,0,1),GetNormalized(axis)));
		if (drive) {
			part.trackingPose = Matrix34(Matrix33(rotation));
			part.trackingPose.SetTranslation(center);
		}
		if (constrainedHand && drive) {
			Matrix34 trackedHand;
			if (interactions->GetGripTrackingPose(hand,trackedHand)) {
				center = trackedHand.GetTranslation();
				rotation = Quat(Matrix33(trackedHand));
			}
		}
		const int side = strstr(part.a,"Bip01 L ") ? 0 : strstr(part.a,"Bip01 R ") ? 1 : -1;
		const bool held = interactions && side >= 0 && interactions->GetHeldPropPhysics(side);
		if (!part.body)
		{
			if (!drive) continue;
			part.physics = physics; part.length = max(.01f,axis.GetLength());
			pe_params_pos position; position.pos = center; position.q = rotation;
			part.body = physics->CreatePhysicalEntity(PE_RIGID,&position,player,PHYS_FOREIGN_ID_VR_BODY);
			if (!part.body) continue;
			for (int shape = 0; shape < 3; ++shape)
			{
				IGeometry* geometry = NULL;
				if (shape < 2)
				{
					primitives::sphere sphere; sphere.center.zero(); sphere.r = part.radius;
					geometry = physics->GetGeomManager()->CreatePrimitive(primitives::sphere::type,&sphere);
				}
				else
				{
					primitives::cylinder cylinder; cylinder.center.zero(); cylinder.axis.Set(0,0,1);
					cylinder.r = part.radius; cylinder.hh = part.length*.5f;
					geometry = physics->GetGeomManager()->CreatePrimitive(primitives::cylinder::type,&cylinder);
				}
				if (!geometry) continue;
				phys_geometry* registered = physics->GetGeomManager()->RegisterGeometry(geometry);
				// RegisterGeometry takes ownership without AddRef; only a failed
				// registration leaves the primitive owned by this caller.
				if (!registered) { geometry->Release(); continue; }
				part.geometry.push_back(registered);
				pe_geomparams params; params.mass = part.mass/3.0f;
				params.pos = Vec3(0,0,shape == 0 ? -part.length*.5f : shape == 1 ? part.length*.5f : 0);
				// These bodies initiate contacts with player-solid surfaces, but are
				// invisible to bullets, pickups, the living capsule and one another.
				params.flags = geom_colltype14; params.flagsCollider = geom_colltype_player;
				part.body->AddGeometry(registered,&params,shape);
			}
			pe_simulation_params simulation; simulation.gravity.zero(); simulation.gravityFreefall.zero();
			simulation.maxTimeStep = .01f; simulation.minEnergy = .001f;
			simulation.damping = simulation.dampingFreefall = .1f;
			part.body->SetParams(&simulation);
			pe_params_flags flags; flags.flagsOR = pef_never_affect_triggers;
			part.body->SetParams(&flags);
		}
		if (drive && !part.collisionEnabled)
		{
			for (int shape = 0; shape < 3; ++shape)
			{
				pe_params_part params; params.ipart = shape; params.flagsColliderAND = 0;
				params.flagsColliderOR = geom_colltype_player;
				part.body->SetParams(&params);
			}
			part.collisionEnabled = true;
		}
		pe_status_pos pose; pe_status_dynamics dynamics;
		if (!part.body->GetStatus(&pose) || !part.body->GetStatus(&dynamics)) continue;
		if (drive && !connectedBody && (center-Vec3(pose.pos)).GetLengthSquared() > 2.25f)
		{
			// Recenter/teleport recovery; normal tracking never writes body position.
			pe_params_pos position; position.pos = center; position.q = rotation;
			part.body->SetParams(&position);
			pe_action_set_velocity stop; stop.v.zero(); stop.w.zero(); part.body->Action(&stop);
			part.previousValid = false; part.body->GetStatus(&pose); part.body->GetStatus(&dynamics);
		}
		pe_status_contact_normals contacts;
		const bool touching = part.collisionEnabled && part.body->GetStatus(&contacts) > 0;
		if (drive)
		{
			// Differentiate tracking relative to the carrier. Its translation and
			// current velocity are supplied by physics on each substep, once.
			Vec3 velocity(0,0,0);
			if (part.previousValid)
				velocity = ((center-part.previousCenter)-(playerPosition-part.previousPlayerPosition))/deltaTime;
			if (velocity.GetLengthSquared() > 100) velocity *= 10.0f/velocity.GetLength();
			pe_action_vr_tracking target;
			target.pos = center; target.q = rotation; target.v = velocity;
			if (player->GetPhysics()) {
				target.referenceId = physics->GetPhysicalEntityId(player->GetPhysics());
				target.referencePos = playerPosition;
			} else target.v += bodyVelocity;
			target.maxForce = constrainedHand ? 350.0f : side<0 ? 900.0f : 600.0f;
			target.maxTorque = 40.0f; target.frequency = touching && !constrainedHand ? 25.0f : 60.0f;
			target.angularVelocityDrive = constrainedHand ? 1 : 0;
			if (held) target.ignoreId = physics->GetPhysicalEntityId(interactions->GetHeldPropPhysics(side));
			Vec3 angularVelocity(0,0,0);
			if (constrainedHand) interactions->GetGripTrackingRotation(hand,rotation,&angularVelocity);
			target.w = angularVelocity;
			part.body->Action(&target);
			part.previousCenter = center; part.previousPlayerPosition = playerPosition; part.previousValid = true;
		}
		// Render the current tracked pose in free space. A previous simulation
		// pose is only authoritative while a contact actually blocks this part.
		// This keeps physics active without introducing its step latency into IK.
		if (hand >= 0 && !held && touching)
		{
			m_handBlocked[hand] = true;
			const Vec3 physical = Vec3(pose.pos)-Matrix33(pose.q)*Vec3(0,0,part.length*.5f);
			// Only block motion into contact planes. Tangential tracking remains
			// immediate; releasing a contact no longer switches whole hand poses.
			m_handPosition[hand] = a;
			for (int pass=0; pass<3; ++pass) for (int n=0; n<contacts.count; ++n)
			{
				const Vec3 normal(contacts.normals[n]);
				m_handPosition[hand] += normal*max(0.0f, (physical-m_handPosition[hand])*normal);
			}
		}
		else if (hand < 0 && touching)
		{
			// Preserve the root/head anchor, adapting the visible upper chain to
				// its physical segment without moving locomotion or the camera.
			const Vec3 physical = Vec3(pose.pos)+Matrix33(pose.q)*Vec3(0,0,part.length*.5f);
			Vec3 end = touching ? b : physical;
			for (int pass=0; touching && pass<3; ++pass) for (int n=0; n<contacts.count; ++n)
			{
				const Vec3 normal(contacts.normals[n]);
				end += normal*max(0.0f, (physical-end)*normal);
			}
			const Vec3 pivot = first->GetBonePosition();
			RotateChain(character,first,pivot,second->GetBonePosition()-pivot,world.GetInverted()*end-pivot);
		}
	}
	for (int hand=0;drive && hand<2;++hand)
		UpdateArmConstraints(hand,interactions && interactions->HasPhysicalGrip(hand),targetA);
}
