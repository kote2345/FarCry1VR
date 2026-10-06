#include "StdAfx.h"
#include "VRBodyPhysics.h"
#include "VRArmIK.h"
#include "VRPhysicalWeapons.h"
#include <primitives.h>
#include <VRPhysicsDiagnostics.h>

namespace
{
bool HandSpaceFree(IPhysicalWorld* physics, IPhysicalEntity* player, const Vec3& center, float radius)
{
	primitives::sphere sphere; sphere.center.zero(); sphere.r = radius;
	IGeometry* probe = physics->GetGeomManager()->CreatePrimitive(primitives::sphere::type,&sphere);
	if (!probe) return false;
	IPhysicalEntity** found = NULL;
	const Vec3 extent(radius,radius,radius);
	const int count = physics->GetEntitiesInBox(center-extent,center+extent,found,
		ent_terrain|ent_static|ent_rigid|ent_sleeping_rigid|ent_living|ent_independent);
	std::vector<IPhysicalEntity*> bodies;
	for (int i=0; i<count; ++i) bodies.push_back(found[i]);
	bool clear = true;
	geom_world_data probeWorld; probeWorld.offset = center;
	for (size_t i=0; clear && i<bodies.size(); ++i)
	{
		IPhysicalEntity* body = bodies[i];
		if (body==player || body->GetiForeignData()==100) continue;
		pe_status_nparts parts;
		const int n = body->GetStatus(&parts);
		for (int p=0; clear && p<n; ++p)
		{
			pe_status_pos pose; pose.ipart = p;
			if (!body->GetStatus(&pose) || !(pose.flagsOR & geom_colltype_player)) continue;
			IGeometry* geometry = pose.pGeomProxy ? pose.pGeomProxy : pose.pGeom;
			if (!geometry) continue;
			geom_world_data world; world.offset = pose.pos; world.R = Matrix33(pose.q); world.scale = pose.scale;
			intersection_params params;
			// Occupancy needs only a contact, not a polygon contact manifold.
			params.bNoAreaContacts = true;
			params.bStopAtFirstTri = true;
			geom_contact* contacts = NULL;
			clear = !geometry->Intersect(probe,&world,&probeWorld,&params,contacts);
			// Mesh PointInsideStatus requires an initialized ray hash and does
			// not bound its cell index. A sphere query does not prepare that
			// hash. Only analytic primitives support this containment query.
			const int type = geometry->GetType();
			if (clear && pose.scale>0 && (type==GEOM_BOX || type==GEOM_SPHERE || type==GEOM_CYLINDER))
				clear = !geometry->PointInsideStatus((!pose.q*(center-Vec3(pose.pos)))/pose.scale);
		}
	}
	probe->Release();
	return clear;
}

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
		previousValid(false), collisionEnabled(true),  previousCenter(0,0,0), previousPlayerPosition(0,0,0) {}
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
	m_separatedHand[0] = m_separatedHand[1] = false;
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
	if (hand<0 || hand>1 || !m_handBlocked[hand]) return false;
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
	const int solidTypes=ent_terrain|ent_static|ent_rigid|ent_sleeping_rigid|ent_living|ent_independent;
	const int rayFlags=rwi_stop_at_pierceable|(geom_colltype_player<<rwi_colltype_bit);
	for (int hand=0; hand<2; ++hand)
	{
		if (interactions && !interactions->CanRecoverHand(hand))
		{ m_separatedHand[hand]=false; continue; }
		// Rendering uses the last accepted wrist target. Geometry queries and
		// recovery state changes belong only to the physical update.
		if (!drive) continue;
		const Vec3 shoulder=targetA[3+hand];
		bool separated=false;
		// Require BOTH occlusion and a contact against that very collider
		// with the opposite surface normal. Near-side pressure is not escape.
		for (int link=0; drive && link<4 && !separated; ++link)
		{
			Part& part=*m_parts[1+hand+link*2];
			if (!part.body) continue;
			pe_status_pos pose; pe_status_contact_normals contacts;
			if (!part.body->GetStatus(&pose) || part.body->GetStatus(&contacts)<=0) continue;
			const Vec3 travel=Vec3(pose.pos)-shoulder;
			const float distance=travel.GetLength();
			if (distance<=part.radius+.02f) continue;
			ray_hit hit;
			if (!physics->RayWorldIntersection(shoulder,travel,solidTypes,rayFlags,&hit,1,player->GetPhysics(),part.body)) continue;
			if (hit.dist>=distance-part.radius-.01f) continue;
			for (int n=0; n<contacts.count; ++n)
				separated |= contacts.colliders[n]==hit.pCollider && contacts.normals[n]*hit.n < -.5f;
		}
		if (!separated && !m_separatedHand[hand]) continue;
		const auto clearArm = [&](const std::vector<Vec3>& a, const std::vector<Vec3>& b) -> bool
		{
			ray_hit obstruction;
			const Vec3 wristTravel=a[7+hand]-shoulder;
			if (wristTravel.GetLengthSquared()>1.e-8f && physics->RayWorldIntersection(
				shoulder,wristTravel,solidTypes,rayFlags,&obstruction,1,player->GetPhysics(),m_parts[7+hand]->body)) return false;
			for (int link=0; link<4; ++link)
			{
				const int index=1+hand+link*2;
				const float radius=m_parts[index]->radius*1.1f;
				const int samples=max(1,(int)((b[index]-a[index]).GetLength()/max(.01f,radius))+1);
				for (int sample=0; sample<=samples; ++sample)
					if (!HandSpaceFree(physics,player->GetPhysics(),a[index]+(b[index]-a[index])*((float)sample/samples),radius)) return false;
			}
			return true;
		};
		std::vector<Vec3> recoveredA=targetA, recoveredB=targetB;
		bool clear=clearArm(recoveredA,recoveredB);
		if (m_separatedHand[hand] && !separated && clear)
		{
			m_separatedHand[hand]=false;
			if (drive) for (int link=0; link<4; ++link) m_parts[1+hand+link*2]->previousValid=false;
			continue;
		}
		const Vec3 rawWrist=targetA[7+hand];
		const Vec3 fingers=targetB[7+hand]-rawWrist;
		const float upperLength=(targetB[3+hand]-shoulder).GetLength();
		const float forearmLength=(rawWrist-targetA[5+hand]).GetLength();
		Vec3 outward=shoulder-targetA[3+1-hand]; outward.z=0;
		if (outward.GetLengthSquared()>1.e-8f) outward.Normalize(); else outward.Set(hand==0 ? -1.0f : 1.0f,0,0);
		const Vec3 pole=targetA[5+hand]+outward*(.35f*(upperLength+forearmLength))-Vec3(0,0,.2f*(upperLength+forearmLength));
		Vec3 preferred=rawWrist;
		ray_hit targetHit;
		const Vec3 desiredTravel=rawWrist-shoulder;
		if (desiredTravel.GetLengthSquared()>1.e-8f && physics->RayWorldIntersection(shoulder,desiredTravel,solidTypes,rayFlags,&targetHit,1,player->GetPhysics(),m_parts[7+hand]->body))
			preferred=shoulder+GetNormalized(desiredTravel)*max(.02f,targetHit.dist-.12f);
		for (int candidate=0; !clear && candidate<7; ++candidate)
		{
			Vec3 wristTarget=preferred;
			if (candidate>0)
			{
				const Vec3 forward=Matrix33(world)*Vec3(0,1,0);
				wristTarget=shoulder+forward*(candidate<4 ? .18f : -.12f)+outward*(.08f*(candidate%3))-Vec3(0,0,.20f);
			}
			Vec3 elbow,wrist;
			if (!SolveVRArm(shoulder,wristTarget,pole,upperLength,forearmLength,elbow,wrist)) continue;
			recoveredB[1+hand]=shoulder;
			recoveredA[3+hand]=shoulder; recoveredB[3+hand]=elbow;
			recoveredA[5+hand]=elbow; recoveredB[5+hand]=wrist;
			recoveredA[7+hand]=wrist; recoveredB[7+hand]=wrist+fingers;
			clear=clearArm(recoveredA,recoveredB);
		}
		if (!clear) continue; // No safe destination: preserve physical blocking.
		targetA=recoveredA; targetB=recoveredB;
		m_recoveredWrist[hand]=targetA[7+hand];
		m_separatedHand[hand]=true;
		if (!drive || !separated) continue;
		UpdateArmConstraints(hand,false,targetA);
		for (int link=0; link<4; ++link)
		{
			const int index=1+hand+link*2;
			Part& part=*m_parts[index];
			if (!part.body) continue;
			const Vec3 axis=targetB[index]-targetA[index];
			Quat rotation(1,0,0,0);
			if (axis.GetLengthSquared()>1.e-8f) rotation=Quat(GetRotationV0V1<float>(Vec3(0,0,1),GetNormalized(axis)));
			pe_action_reset reset; part.body->Action(&reset);
			pe_params_pos position; position.pos=(targetA[index]+targetB[index])*.5f; position.q=rotation;
			part.body->SetParams(&position);
			part.previousValid=false;
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
		pe_status_pos pose; pe_status_dynamics dynamics;
		if (!part.body->GetStatus(&pose) || !part.body->GetStatus(&dynamics)) continue;
		pe_status_contact_normals contacts;
		bool touching = part.collisionEnabled && part.body->GetStatus(&contacts) > 0;
		// Tracking lag behind a wall is not a teleport. Never recenter a
		// blocked body through that wall just because the controller kept moving.
		ray_hit recoveryHit;
		const Vec3 recoveryTravel = center-Vec3(pose.pos);
		const bool recoveryNeeded = drive && hand<0 && !connectedBody && recoveryTravel.GetLengthSquared()>2.25f;
		const bool recoveryClear = recoveryNeeded && !touching && !physics->RayWorldIntersection(
			pose.pos,recoveryTravel,ent_terrain|ent_static|ent_rigid|ent_sleeping_rigid|ent_living|ent_independent,
			rwi_stop_at_pierceable|(geom_colltype_player<<rwi_colltype_bit),&recoveryHit,1,player->GetPhysics(),part.body);
		if (recoveryClear)
		{
			// Recenter/teleport recovery; normal tracking never writes body position.
			pe_params_pos position; position.pos = center; position.q = rotation;
			part.body->SetParams(&position);
			pe_action_set_velocity stop; stop.v.zero(); stop.w.zero(); part.body->Action(&stop);
			part.previousValid = false; part.body->GetStatus(&pose); part.body->GetStatus(&dynamics);
		}
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
			const Vec3 physical = Vec3(pose.pos)-Matrix33(pose.q)*Vec3(0,0,part.length*.5f);
			// Only block motion into contact planes. Tangential tracking remains
			// immediate; releasing a contact no longer switches whole hand poses.
			m_handPosition[hand] = a;
			for (int pass=0; pass<3; ++pass) for (int n=0; n<contacts.count; ++n)
			{
				const Vec3 normal(contacts.normals[n]);
				m_handPosition[hand] += normal*max(0.0f, (physical-m_handPosition[hand])*normal);
			}
			// A contact history entry alone is not a tracking obstruction.
			// Once motion no longer enters its plane, render raw controller IK.
			m_handBlocked[hand] = (m_handPosition[hand]-a).GetLengthSquared()>1.e-6f;
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
	for (int hand=0; hand<2; ++hand) if (m_separatedHand[hand])
	{
		m_handBlocked[hand]=true;
		m_handPosition[hand]=m_recoveredWrist[hand];
	}
	for (int hand=0;drive && hand<2;++hand)
		UpdateArmConstraints(hand,interactions && interactions->HasPhysicalGrip(hand),targetA);
}
