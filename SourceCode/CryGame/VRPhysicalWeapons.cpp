#include "StdAfx.h"
#include "XPlayer.h"
#include "VRPhysicalWeapons.h"
#include "VRBodyPhysics.h"
#include <VRPhysicsDiagnostics.h>
#include <VRGripMath.h>
#include "WeaponClass.h"
#include "WeaponSystemEx.h"
#include <IStatObj.h>
#include <CryCharAnimationParams.h>
#include <MeshIdx.h>
#include <primitives.h>

namespace
{
const float GrabRadius = 0.22f;
const float RecallDelay = 2.5f;
const char* ContextKeys[] = { "fireparams", "weapon_info", "sounddata", "soundids", "weapon", "_MuzzleFlashParams" };

// Lua weapon effects use cached fields on the shooter. Keep a scoped context
// for each instance so left-hand effects cannot use the right-hand weapon.
void CopyContext(IScriptSystem* scripts, IScriptObject* source, IScriptObject* destination)
{
	for (unsigned i = 0; i < sizeof(ContextKeys)/sizeof(ContextKeys[0]); ++i)
	{
		_SmartScriptObject value(scripts, true);
		if (source->GetValue(ContextKeys[i], value)) destination->SetValue(ContextKeys[i], value);
		else destination->SetToNull(ContextKeys[i]);
	}
}

Matrix34 StaticModelPose(IStatObj* object, const Matrix34& grip)
{
	if (const Matrix44* helper = object->GetHelperMatrixByName("hold"))
		return grip * Matrix34(GetTransposed44(*helper)).GetInverted();
	if (const Matrix44* helper = object->GetHelperMatrixByName("grip"))
		return grip * Matrix34(GetTransposed44(*helper)).GetInverted();
	// Third-person pickup meshes have no first-person arm geometry. If no
	// authored grip helper exists, put the mesh centre at the physical grip.
	Matrix34 offset;
	offset.SetIdentity();
	offset.SetTranslation(-(object->GetBoxMin() + object->GetBoxMax()) * 0.5f);
	return grip * offset;
}

bool IsMelee(CWeaponClass* weapon)
{
	WeaponParams mode;
	return weapon && weapon->GetModeParams(0, mode) && mode.iFireModeType == FireMode_Melee;
}
}

struct CVRPhysicalWeapons::Item
{
	enum State { Holstered, Held, Dropped, Consumed };
	Item() : weaponID(-1), personal(false), state(Holstered), holder(-1), slot(0),
		entity(0), body(NULL), proxy(NULL), clip(0), capacity(0), recallAt(0), context(NULL), character(NULL),
		slideHand(-1), slideDistance(0), slideStart(0,0,0), slideAxis(0,1,0), bindingClass(NULL),
		meleeGeometry(NULL), ownsMeleeGeometry(false), meleePoseValid(false)
	{ model.SetIdentity(); }
	~Item()
	{
		if (context) context->Release();
		if (meleeGeometry && ownsMeleeGeometry) meleeGeometry->Release();
		if (character)
		{
			if (bindingClass) bindingClass->ClearMuzzleBindings(character);
			character->Release();
		}
	}
	int weaponID;
	bool personal;
	State state;
	int holder, slot;
	EntityId entity;
	IPhysicalEntity* body;
	phys_geometry* proxy;
	int clip, capacity;
	string ammoType;
	float recallAt;
	Matrix34 model;
	WeaponInfo fire;
	IScriptObject* context;
	ICryCharInstance* character;
	string slideBone;
	int slideHand;
	float slideDistance;
	Vec3 slideStart, slideAxis;
	std::vector<Matrix44> animationPose;
	CWeaponClass* bindingClass;
	IGeometry* meleeGeometry;
	bool ownsMeleeGeometry, meleePoseValid;
	Matrix34 previousMeleePose;
	Vec3 previousBodyPosition;
	std::map<IPhysicalEntity*, float> meleeHits;
};

struct CVRPhysicalWeapons::Magazine
{
	Magazine() : weaponID(-1), rounds(0), character(NULL), center(0,0,0), scale(1) {}
	~Magazine() { if (character) character->Release(); }
	int weaponID, rounds;
	string ammoType, bone;
	ICryCharInstance* character;
	Vec3 center;
	float scale;
};

namespace
{
const char* MagazineBone(CWeaponClass* weapon, ICryCharInstance* character)
{
	if (!weapon || !character) return NULL;
	const char* configured = NULL;
	weapon->GetScriptObject()->GetValue("VRMagazineBone", configured);
	if (configured && character->GetBoneByName(configured)) return configured;
	const char* candidates[] = {"magazine", "magazine01", "reload"};
	for (int i = 0; i < 3; ++i)
		if (character->GetBoneByName(candidates[i])) return candidates[i];
	return NULL;
}

float HeldWeaponScale(CWeaponClass* weapon)
{
	return weapon && (!stricmp(weapon->GetName().c_str(), "Falcon") ||
		!stricmp(weapon->GetName().c_str(), "DE")) ? 1.2f : 1.0f;
}
}

void CVRPhysicalWeapons::PrepareCharacter(Item& item)
{
	if (item.character) return;
	CWeaponClass* weapon = Weapon(item);
	if (IsMelee(weapon)) return; // Melee renders its hand-free mesh directly.
	ICryCharInstance* prototype = weapon ? weapon->GetCharacter() : NULL;
	if (!prototype || !prototype->GetModel()) return;
	item.character = m_player.m_pGame->GetSystem()->GetIAnimationSystem()->MakeCharacter(prototype->GetModel()->GetFileName());
	if (item.character)
	{
		item.bindingClass = weapon;
		item.character->SetFlags(prototype->GetFlags() & ~CS_FLAG_DRAW_NEAR);
		item.character->ResetAnimations();
		CryCharAnimationParams idle;
		idle.fBlendInTime = 0;
		idle.fBlendOutTime = 0;
		idle.nLayerID = 0;
		item.character->SetAnimationSpeed(1.0f);
		item.character->SetDefaultIdleAnimation(0, "Idle11");
		item.character->StartAnimation("Idle11", idle);
		item.character->Update();
		item.character->ForceUpdate();
		const char* slide = NULL;
		weapon->m_soWeaponClass->GetValue("VRSlideBone", slide);
		if (slide && slide[0]) item.slideBone = slide;
		else if (strstr(prototype->GetModel()->GetFileName(), "/DE/") ||
			strstr(prototype->GetModel()->GetFileName(), "/de/") ||
			!stricmp(weapon->GetName().c_str(), "DE") ||
			!stricmp(weapon->GetName().c_str(), "Falcon")) item.slideBone = "reload01";
	}
}

void CVRPhysicalWeapons::SetHandPalm(int hand, const Vec3& worldPalm, const Matrix34& controller)
{
	// The calibrated palm offset is anatomical. Never teach it the distance
	// between a constrained hand and its freely moving tracked controller.
	if (HasPhysicalGrip(hand)) return;
	m_hands[hand].palmOffset = Matrix34(controller).GetInverted() * worldPalm;
	m_hands[hand].palmValid = true;
}

bool CVRPhysicalWeapons::GetHandRecoil(int hand, const Matrix34& controller, Matrix34& delta)
{
	Item* item = m_hands[hand].item;
	CWeaponClass* weapon = item ? Weapon(*item) : NULL;
	if (!weapon || IsMelee(weapon) || !item->character || item->animationPose.empty()) return false;
	// Apply authored shot recoil only. Idle, reload and activation arm motion
	// must not move the avatar away from tracking.
	const char* animation = item->character->GetCurAnimation();
	if (!animation || (!strstr(animation, "fire") && !strstr(animation, "Fire"))) return false;
	Matrix34 model;
	if (!HeldModel(hand, controller, model)) return false;
	const char* wrist = weapon->GetVRHandBoneName(false);
	for (size_t i = 0; i < item->animationPose.size(); ++i)
	{
		if (strcmp(item->character->GetModel()->GetBoneName((int)i), wrist)) continue;
		const Matrix34 local = Matrix34(GetTransposed44(item->animationPose[i])) * weapon->m_vrGripHandInverse;
		delta = model * local * model.GetInverted();
		return true;
	}
	return false;
}

bool CVRPhysicalWeapons::HeldModel(int hand, const Matrix34& grip, Matrix34& model)
{
	Item* item = m_hands[hand].item;
	CWeaponClass* weapon = item ? Weapon(*item) : NULL;
	if (!weapon || !weapon->GetVRHandModelTransform(grip, model)) return false;
	const float scale = HeldWeaponScale(weapon);
	if (scale != 1.0f)
	{
		const Matrix33 original(model);
		const Vec3 anchor = model * weapon->m_vrGripBonePosition;
		model = Matrix34(original * scale);
		model.SetTranslation(anchor-Matrix33(model)*weapon->m_vrGripBonePosition);
	}
	// The same unmirrored gun model is held in either hand. Its controller-
	// local translation must be identical; use the working right-hand socket
	// instead of reflecting a differently oriented left wrist frame.
	const Hand& anchor = m_hands[1].palmValid ? m_hands[1] : m_hands[hand];
	if (anchor.palmValid)
		model.SetTranslation(model.GetTranslation() + Matrix33(grip) * anchor.palmOffset);
	// Controller-local placement: 3 cm forward and 1 cm up, identical
	// for either hand. Shared by rendering, muzzle and slide interaction.
	model.SetTranslation(model.GetTranslation() + Matrix33(grip) * Vec3(0, .03f, .01f));
	return true;
}

bool CVRPhysicalWeapons::GrabSlide(int hand)
{
	Item* item = m_hands[1 - hand].item;
	if (!item || !item->character || item->slideBone.empty() || item->slideHand >= 0) return false;
	ICryBone* bone = item->character->GetBoneByName(item->slideBone.c_str());
	Matrix34 model;
	if (!bone || !HeldModel(item->holder, m_hands[item->holder].pose, model)) return false;
	const Vec3 localHand = model.GetInverted() * m_hands[hand].pose.GetTranslation();
	if ((localHand - bone->GetBonePosition()).GetLengthSquared() > 0.14f * 0.14f) return false;
	// DE_Fire11 translates reload01 by +4 cm on its parent's local Y axis.
	ICryBone* parent = bone->GetParent();
	if (!parent) return false;
	item->slideAxis = Matrix33(GetTransposed44(parent->GetAbsoluteMatrix())) * Vec3(0,1,0);
	if (item->slideAxis.GetLengthSquared() < 1.0e-6f) return false;
	item->slideAxis.Normalize();
	item->slideStart = localHand;
	item->slideHand = hand;
	item->slideDistance = 0;
	return true;
}

void CVRPhysicalWeapons::UpdateSlidePose(Item& item)
{
	if (item.slideBone.empty()) return;
	const int hand = item.slideHand;
	if (hand >= 0)
	{
		Matrix34 model;
		const bool gripping = m_hands[hand].tracked && m_hands[hand].gripDown &&
			!m_hands[hand].item && !m_hands[hand].magazine && m_hands[hand].propID < 0 && item.state == Item::Held &&
			HeldModel(item.holder, m_hands[item.holder].pose, model);
		if (!gripping) { item.slideHand = -1; item.slideDistance = 0; }
		else
		{
			const Vec3 localHand = model.GetInverted() * m_hands[hand].pose.GetTranslation();
			item.slideDistance = max(0.0f, min(0.04f, (localHand - item.slideStart) | item.slideAxis));
			if ((localHand - item.slideStart).GetLengthSquared() > 0.30f * 0.30f)
			{ item.slideHand = -1; item.slideDistance = 0; }
		}
	}
	ICryBone* root = item.character->GetBoneByName(item.slideBone.c_str());
	if (!root) return;
	for (int i = 0; i < item.character->GetModel()->NumBones(); ++i)
	{
		ICryBone* bone = item.character->GetBoneByName(item.character->GetModel()->GetBoneName(i));
		ICryBone* ancestor = bone;
		while (ancestor && ancestor != root) ancestor = ancestor->GetParent();
		if (!ancestor) continue;
		// Set an absolute displacement from the pristine pose, including zero
		// after release, rather than adding onto a previously displaced matrix.
		if ((size_t)i >= item.animationPose.size()) continue;
		Matrix44 pose = item.animationPose[i];
		pose.SetTranslationOLD(pose.GetTranslationOLD() + item.slideAxis * item.slideDistance);
		const_cast<Matrix44&>(bone->GetAbsoluteMatrix()) = pose;
	}
}

CVRPhysicalWeapons::CVRPhysicalWeapons(CPlayer& player)
	: m_player(player), m_active(false), m_nextInventorySync(0)
{
	m_body.SetIdentity();
	IScriptSystem* scripts = player.m_pGame->GetSystem()->GetIScriptSystem();
	_SmartScriptObject classes(scripts);
	CWeaponSystemEx* weapons = player.m_pGame->GetWeaponSystemEx();
	for (unsigned i = 0; i < weapons->GetNumWeaponClasses(); ++i)
	{
		CWeaponClass* weapon = weapons->GetWeaponClass(i);
		classes->SetValue((string("Pickup") + weapon->GetName()).c_str(), 1);
	}
	scripts->SetGlobalValue("VRPhysicalWeaponPickupClasses", classes);
}

CVRPhysicalWeapons::~CVRPhysicalWeapons()
{
	RemovePhysicalGrip(0); RemovePhysicalGrip(1);
	ReleaseMagazine(0); ReleaseMagazine(1);
	IEntitySystem* entities = m_player.m_pGame->GetSystem()->GetIEntitySystem();
	for (unsigned i = 0; i < m_items.size(); ++i)
	{
		Item& item = *m_items[i];
		if (item.entity)
			if (IEntity* entity = entities->GetEntity(item.entity))
			{
				entity->SetPos(item.model.GetTranslation());
				entity->SetAngles(RAD2DEG(Ang3::GetAnglesXYZ(Matrix33(item.model))));
				entity->Hide(false); entity->EnablePhysics(true);
			}
		DestroyBody(item);
		delete m_items[i];
	}
	if (m_player.GetEntity() && m_player.GetEntity()->GetScriptObject())
		m_player.GetEntity()->GetScriptObject()->SetToNull("VRPhysicalWeapons");
}

CWeaponClass* CVRPhysicalWeapons::Weapon(const Item& item) const
{
	return m_player.m_pGame->GetWeaponSystemEx()->GetWeaponClassByID(item.weaponID);
}

void CVRPhysicalWeapons::InitializeMagazine(Item& item, bool external)
{
	IScriptSystem* scripts = m_player.m_pGame->GetSystem()->GetIScriptSystem();
	CWeaponClass* weapon = Weapon(item);
	if (!weapon || (!weapon->IsLoaded() && !weapon->Load())) return;
	_SmartScriptObject modes(scripts, true), mode(scripts, true);
	const char* ammoType = "";
	if (weapon->GetScriptObject()->GetValue("FireParams", modes) && modes->GetAt(1, mode))
	{
		mode->GetValue("bullets_per_clip", item.capacity);
		mode->GetValue("AmmoType", ammoType);
	}
	item.capacity = max(0, item.capacity);
	item.ammoType = ammoType;
	item.clip = 0;
	if (external)
	{
		item.clip = item.capacity;
		if (IEntity* entity = m_player.m_pGame->GetSystem()->GetIEntitySystem()->GetEntity(item.entity))
		{
			_SmartScriptObject properties(scripts, true);
			if (entity->GetScriptObject() && entity->GetScriptObject()->GetValue("Properties", properties))
				properties->GetValue("Amount", item.clip);
		}
	}
	else if (item.weaponID == m_player.m_nSelectedWeaponID)
		item.clip = m_player.m_stats.ammo_in_clip;
	else
	{
		_SmartScriptObject states(scripts, true), state(scripts, true), clips(scripts, true);
		if (m_player.GetEntity()->GetScriptObject()->GetValue("WeaponState", states) &&
			states->GetAt(item.weaponID, state) && state->GetValue("AmmoInClip", clips))
			clips->GetAt(1, item.clip);
	}
	item.clip = max(0, min(item.capacity, item.clip));
	item.fire.iFireMode = 0;
	item.fire.fireTime = m_player.m_pTimer->GetCurrTime();
	item.fire.fireLastShot = 0;
}

void CVRPhysicalWeapons::SaveMagazine(Item& item)
{
	if (!item.personal) return;
	IScriptSystem* scripts = m_player.m_pGame->GetSystem()->GetIScriptSystem();
	_SmartScriptObject states(scripts, true), state(scripts, true), clips(scripts, true);
	IScriptObject* shooter = m_player.GetEntity()->GetScriptObject();
	if (!shooter->GetValue("WeaponState", states))
	{
		_SmartScriptObject created(scripts);
		shooter->SetValue("WeaponState", created);
		states->Attach(created);
	}
	if (!states->GetAt(item.weaponID, state))
	{
		_SmartScriptObject created(scripts);
		states->SetAt(item.weaponID, created);
		state->Attach(created);
	}
	if (!state->GetValue("AmmoInClip", clips))
	{
		_SmartScriptObject created(scripts);
		state->SetValue("AmmoInClip", created);
		clips->Attach(created);
	}
	clips->SetAt(1, item.clip);
	if (item.weaponID == m_player.m_nSelectedWeaponID) m_player.m_stats.ammo_in_clip = item.clip;
}

void CVRPhysicalWeapons::SyncInventory()
{
	int slot = 0;
	for (CPlayer::PlayerWeaponsItor it = m_player.m_mapPlayerWeapons.begin(); it != m_player.m_mapPlayerWeapons.end(); ++it)
	{
		if (!it->second.owns) continue;
		Item* existing = NULL;
		for (unsigned i = 0; i < m_items.size(); ++i)
			if (m_items[i]->personal && m_items[i]->weaponID == it->first && m_items[i]->state != Item::Consumed)
			{ existing = m_items[i]; break; }
		if (!existing)
		{
			existing = new Item;
			existing->weaponID = it->first;
			existing->personal = true;
			InitializeMagazine(*existing, false);
			m_items.push_back(existing);
		}
		existing->slot = slot++;
	}
	for (unsigned i = 0; i < m_items.size(); ++i)
	{
		Item& item = *m_items[i];
		if (item.personal && !m_player.GetWeaponInfo(item.weaponID).owns)
		{
			if (item.holder >= 0) Release(item.holder, true);
			DestroyBody(item);
			item.state = Item::Consumed;
		}
	}
}

void CVRPhysicalWeapons::UpdateBody()
{
	CCamera camera = m_player.m_pGame->GetSystem()->GetViewCamera();
	camera.SetAngle(Vec3(0, 0, m_player.m_vEyeAngles.z +
		RAD2DEG(m_player.m_pGame->GetSystem()->GetVRHeadYawDeltaRadians())));
	camera.Update();
	m_body = Matrix34(camera.GetVMatrix()).GetInverted();
	m_body.SetTranslation(camera.GetPos() - Vec3(0, 0, 0.65f));
}

Matrix34 CVRPhysicalWeapons::HolsterPose(int slot) const
{
	int count = 0;
	for (unsigned i = 0; i < m_items.size(); ++i)
		if (m_items[i]->personal && m_items[i]->state != Item::Consumed) ++count;
	const float angle = count <= 1 ? 0.0f : DEG2RAD(-70.0f + 140.0f * slot / max(1, count - 1));
	Matrix34 local = Matrix34::CreateRotationXYZ(Vec3(gf_PI_DIV_2, 0, -angle),
		Vec3(cry_sinf(angle) * 0.44f, cry_cosf(angle) * 0.44f, 0));
	return m_body * local;
}

void CVRPhysicalWeapons::DestroyBody(Item& item)
{
	IPhysicalWorld* world = m_player.m_pGame->GetSystem()->GetIPhysicalWorld();
	if (item.body) world->DestroyPhysicalEntity(item.body);
	item.body = NULL;
	if (item.proxy) world->GetGeomManager()->UnregisterGeometry(item.proxy);
	item.proxy = NULL;
}

bool CVRPhysicalWeapons::DropBody(Item& item, const Vec3& velocity, const Vec3& angularVelocity)
{
	CWeaponClass* weapon = Weapon(item);
	IStatObj* object = weapon ? weapon->GetObject() : NULL;
	if (!object) return false;
	IPhysicalWorld* world = m_player.m_pGame->GetSystem()->GetIPhysicalWorld();
	pe_params_pos position;
	position.pos = item.model.GetTranslation();
	position.q = Quat(Matrix33(item.model));
	item.body = world->CreatePhysicalEntity(PE_RIGID, &position);
	if (!item.body) return false;
	phys_geometry* geometry = object->GetPhysGeom();
	if (!geometry)
	{
		primitives::box box;
		box.Basis.SetIdentity(); box.bOriented = 0;
		box.center = (object->GetBoxMin() + object->GetBoxMax()) * 0.5f;
		Vec3 half = (object->GetBoxMax() - object->GetBoxMin()) * 0.5f;
		box.size = Vec3(max(0.015f, half.x), max(0.015f, half.y), max(0.015f, half.z));
		IGeometry* boxGeometry = world->GetGeomManager()->CreatePrimitive(primitives::box::type, &box);
		if (boxGeometry)
		{
			item.proxy = world->GetGeomManager()->RegisterGeometry(boxGeometry);
			if (!item.proxy) boxGeometry->Release();
		}
		geometry = item.proxy;
	}
	ICVar* propScale = m_player.m_pGame->GetSystem()->GetIConsole()->GetCVar("vr_prop_mass_scale");
	pe_geomparams params; params.mass = 2.0f*(propScale ? max(.05f,propScale->GetFVal()) : 1.0f);
	if (!geometry || item.body->AddGeometry(geometry, &params) < 0)
	{ DestroyBody(item); return false; }
	pe_simulation_params simulation;
	simulation.gravity = Vec3(0,0,-9.81f); simulation.damping = 0.15f;
	item.body->SetParams(&simulation);
	pe_action_set_velocity action; action.v = velocity; action.w = angularVelocity;
	item.body->Action(&action);
	return true;
}

void CVRPhysicalWeapons::ReturnToHolster(Item& item)
{
	item.slideHand = -1;
	item.slideDistance = 0;
	DestroyBody(item);
	item.state = Item::Holstered;
	item.holder = -1;
	item.recallAt = 0;
	SaveMagazine(item);
}

void CVRPhysicalWeapons::Grab(int hand, bool scanWorld)
{
	Hand& target = m_hands[hand];
	const Vec3 position = target.pose.GetTranslation();
	Item* best = NULL;
	float bestDistance = GrabRadius * GrabRadius;
	for (unsigned i = 0; i < m_items.size(); ++i)
	{
		Item& item = *m_items[i];
		if (item.state != Item::Holstered && item.state != Item::Dropped) continue;
		Vec3 point = item.state == Item::Holstered ? HolsterPose(item.slot).GetTranslation() : item.model.GetTranslation();
			if (CWeaponClass* weapon = Weapon(item))
				if (IStatObj* object = weapon->GetObject())
				{
					// Reach any part of the visible dropped mesh, not just its
					// centre (which can be outside grab range on longer weapons).
					Matrix34 visibleModel = item.state == Item::Holstered ?
						StaticModelPose(object, HolsterPose(item.slot)) : item.model;
					const Vec3 local = visibleModel.GetInverted() * position;
					const Vec3 lo = object->GetBoxMin(), hi = object->GetBoxMax();
					point = visibleModel * Vec3(max(lo.x, min(hi.x, local.x)),
						max(lo.y, min(hi.y, local.y)), max(lo.z, min(hi.z, local.z)));
				}
		const float distance = (point - position).GetLengthSquared();
		if (distance < bestDistance) { best = &item; bestDistance = distance; }
	}
	// Search world pickups only on a grip edge, never every render frame.
	IEntitySystem* entities = m_player.m_pGame->GetSystem()->GetIEntitySystem();
	IEntity* pickup = NULL;
	CWeaponClass* pickupWeapon = NULL;
	if (scanWorld)
	{
	IEntityIt* iterator = entities->GetEntityIterator();
	iterator->MoveFirst();
	while (IEntity* entity = iterator->Next())
	{
		if (entity->IsHidden()) continue;
		const char* name = entity->GetEntityClassName();
		if (!name || strncmp(name, "Pickup", 6)) continue;
		CWeaponClass* weapon = m_player.m_pGame->GetWeaponSystemEx()->GetWeaponClassByName(name + 6);
		if (!weapon) continue;
		bool alreadyTracked = false;
		for (unsigned i = 0; i < m_items.size(); ++i)
			if (m_items[i]->entity == entity->GetId()) { alreadyTracked = true; break; }
		if (alreadyTracked) continue;
		Vec3 boxMin, boxMax;
		entity->GetBBox(boxMin, boxMax);
		const Vec3 nearest(max(boxMin.x, min(boxMax.x, position.x)),
			max(boxMin.y, min(boxMax.y, position.y)), max(boxMin.z, min(boxMax.z, position.z)));
		const float distance = (nearest - position).GetLengthSquared();
		if (distance < bestDistance)
		{ pickup = entity; pickupWeapon = weapon; best = NULL; bestDistance = distance; }
	}
	iterator->Release();
	}
	if (pickup)
	{
		best = new Item;
		best->weaponID = pickupWeapon->GetID(); best->entity = pickup->GetId();
		best->state = Item::Dropped;
		InitializeMagazine(*best, true);
		m_items.push_back(best);
		if (!m_player.GetWeaponInfo(best->weaponID).owns)
		{
			m_player.MakeWeaponAvailable(best->weaponID, true);
			best->personal = true;
			SaveMagazine(*best);
			entities->RemoveEntity(best->entity);
			best->entity = 0;
			SyncInventory();
		}
	}
	if (!best) return;
	DestroyBody(*best);
	if (best->entity)
		if (IEntity* entity = entities->GetEntity(best->entity))
	{ entity->KillTimer(); entity->EnablePhysics(false); entity->Hide(true); }
	best->state = Item::Held; best->holder = hand; best->recallAt = 0;
	best->meleePoseValid = false;
	best->meleeHits.clear();
	PrepareCharacter(*best);
	target.item = best;
	best->fire.fireTime = m_player.m_pTimer->GetCurrTime();
}

void CVRPhysicalWeapons::AddAmmo(Item& item)
{
	IScriptSystem* scripts = m_player.m_pGame->GetSystem()->GetIScriptSystem();
	_SmartScriptObject ammo(scripts, true);
	if (item.ammoType.empty() || item.ammoType == "Unlimited") return;
	if (!m_player.GetEntity()->GetScriptObject()->GetValue("Ammo", ammo)) return;
	int amount = 0; ammo->GetValue(item.ammoType.c_str(), amount);
	int limit = m_player.GetWeaponInfo(item.weaponID).maxAmmo;
	int result = amount + item.clip;
	if (limit > 0) result = min(result, limit);
	ammo->SetValue(item.ammoType.c_str(), result);
	if (CWeaponClass* selected = m_player.GetSelectedWeapon())
	{
		_SmartScriptObject params(scripts, true);
		const char* selectedAmmo = "";
		if (m_player.GetEntity()->GetScriptObject()->GetValue("fireparams", params) &&
			params->GetValue("AmmoType", selectedAmmo) && item.ammoType == selectedAmmo)
			m_player.m_stats.ammo = result;
	}
}

void CVRPhysicalWeapons::Release(int hand, bool trackingLost)
{
	Hand& source = m_hands[hand];
	ReleaseMagazine(hand);
	RemovePhysicalGrip(hand);
	source.propID = -1;
	Item* item = source.item;
	if (!item) return;
	item->slideHand = -1;
	item->slideDistance = 0;
	item->meleePoseValid = false;
	Fire(hand, false);
	source.item = NULL;
	item->holder = -1;
	const Vec3 local = m_body.GetInverted() * source.pose.GetTranslation();
	const bool atBody = !trackingLost && fabsf(local.x) < 0.5f && local.y > -0.25f && local.y < 0.55f &&
		local.z > -0.35f && local.z < 0.4f;
	const bool atHolster = !trackingLost &&
		(source.pose.GetTranslation() - HolsterPose(item->slot).GetTranslation()).GetLengthSquared() < 0.24f * 0.24f;
	if (item->personal && atHolster) { ReturnToHolster(*item); return; }
	if (!item->personal && atBody)
	{
		AddAmmo(*item);
		if (item->entity) m_player.m_pGame->GetSystem()->GetIEntitySystem()->RemoveEntity(item->entity);
		item->entity = 0; item->state = Item::Consumed;
		return;
	}
	item->state = Item::Dropped;
	CWeaponClass* weapon = Weapon(*item);
	if (weapon && weapon->GetObject()) item->model = StaticModelPose(weapon->GetObject(), source.pose);
	const Vec3 velocity = trackingLost ? Vec3(0,0,0) : source.velocity;
	const Vec3 angularVelocity = trackingLost ? Vec3(0,0,0) : source.angularVelocity;
	// Both kinds use actual rigid-body simulation. Keep the source pickup
	// hidden while its physical VR instance is on the ground; it is restored
	// if the interaction system is destroyed without consuming that instance.
	const bool rigid = DropBody(*item, velocity, angularVelocity);
	if (item->entity)
	{
		if (IEntity* entity = m_player.m_pGame->GetSystem()->GetIEntitySystem()->GetEntity(item->entity))
		{
			entity->SetPos(item->model.GetTranslation());
			entity->SetAngles(RAD2DEG(Ang3::GetAnglesXYZ(Matrix33(item->model))));
			entity->Hide(rigid); entity->EnablePhysics(!rigid);
			if (!rigid)
			if (IPhysicalEntity* physics = entity->GetPhysics())
			{
				pe_action_set_velocity action; action.v = velocity; action.w = angularVelocity;
				physics->Action(&action);
			}
			_SmartScriptObject properties(m_player.m_pScriptSystem, true);
			if (entity->GetScriptObject()->GetValue("Properties", properties)) properties->SetValue("Amount", item->clip);
		}
	}
	else if (!rigid) { ReturnToHolster(*item); return; }
	if (item->personal) item->recallAt = m_player.m_pTimer->GetCurrTime() + RecallDelay;
	SaveMagazine(*item);
}

void CVRPhysicalWeapons::UpdateProximityHaptics()
{
	ISystem* system = m_player.m_pGame->GetSystem();
	const float now = m_player.m_pTimer->GetCurrTime();
	bool scan[2] = {false,false};
	float distances[2] = {10000,10000};
	for (int hand=0; hand<2; ++hand)
	{
		Hand& state = m_hands[hand];
		if (!m_active || !m_player.IsAlive() || !state.tracked)
		{ state.nearWeapon = false; state.nextProximityScan = 0; continue; }
		if (now < state.nextProximityScan) continue;
		state.nextProximityScan = now+.08f;
		scan[hand] = true;
		const Vec3 position = state.pose.GetTranslation();
		for (size_t i=0; i<m_items.size(); ++i)
		{
			Item& item = *m_items[i];
			if (item.state==Item::Consumed) continue;
			if (item.personal)
				distances[hand] = min(distances[hand],(position-HolsterPose(item.slot).GetTranslation()).GetLengthSquared());
			if (item.state==Item::Held && item.holder==hand) continue;
			CWeaponClass* weapon = Weapon(item);
			IStatObj* object = weapon ? weapon->GetObject() : NULL;
			if (!object) continue;
			Matrix34 model = item.model;
			if (item.state==Item::Holstered) model = StaticModelPose(object,HolsterPose(item.slot));
			else if (item.state==Item::Held && !HeldModel(item.holder,m_hands[item.holder].pose,model)) continue;
			const Vec3 local = model.GetInverted()*position;
			const Vec3 lo = object->GetBoxMin(), hi = object->GetBoxMax();
			const Vec3 closest = model*Vec3(max(lo.x,min(hi.x,local.x)),max(lo.y,min(hi.y,local.y)),max(lo.z,min(hi.z,local.z)));
			distances[hand] = min(distances[hand],(position-closest).GetLengthSquared());
		}
	}
	if (!scan[0] && !scan[1]) return;
	// Query nearby physical entities instead of walking the whole level.
	// Copy the world scratch list before inspecting individual entities.
	for (int hand=0; hand<2; ++hand) if (scan[hand])
	{
		const Vec3 p = m_hands[hand].pose.GetTranslation();
		const Vec3 extent(.3f,.3f,.3f);
		IPhysicalEntity** found = NULL;
		const int count = system->GetIPhysicalWorld()->GetEntitiesInBox(p-extent,p+extent,found,
			ent_static|ent_rigid|ent_sleeping_rigid|ent_independent);
		std::vector<IPhysicalEntity*> nearby;
		for (int i=0; i<count; ++i) nearby.push_back(found[i]);
		for (size_t n=0; n<nearby.size(); ++n)
		{
			IPhysicalEntity* body = nearby[n];
			if (body->GetiForeignData()!=OT_ENTITY) continue;
			IEntity* entity = static_cast<IEntity*>(body->GetForeignData(OT_ENTITY));
			if (!entity || entity->IsHidden()) continue;
			const char* name = entity->GetEntityClassName();
			if (!name || strncmp(name,"Pickup",6) || !m_player.m_pGame->GetWeaponSystemEx()->GetWeaponClassByName(name+6)) continue;
			bool tracked = false;
			for (size_t i=0; i<m_items.size(); ++i)
				if (m_items[i]->entity==entity->GetId()) { tracked=true; break; }
			if (tracked) continue;
			Vec3 lo,hi; entity->GetBBox(lo,hi);
			const Vec3 closest(max(lo.x,min(hi.x,p.x)),max(lo.y,min(hi.y,p.y)),max(lo.z,min(hi.z,p.z)));
			distances[hand] = min(distances[hand],(p-closest).GetLengthSquared());
		}
	}
	for (int hand=0; hand<2; ++hand) if (scan[hand])
	{
		Hand& state = m_hands[hand];
		const float radius = state.nearWeapon ? .26f : .20f;
		const bool near = distances[hand] < radius*radius;
		if (near && !state.nearWeapon && now>=state.shotHapticUntil)
			system->PulseVRController(hand==0,.25f,.02f);
		state.nearWeapon = near;
	}
}

void CVRPhysicalWeapons::Fire(int hand, bool pressed)
{
	Item* item = m_hands[hand].item;
	if (!item) return;
	if (!pressed && !item->context) return;
	CWeaponClass* weapon = Weapon(*item);
	if (!weapon) return;
	WeaponParams mode;
	if (!weapon->GetModeParams(0, mode)) return;
	if (pressed && item->clip <= 0 && !mode.no_ammo && item->ammoType != "Unlimited") return;
	const eFireType event = pressed ? (m_hands[hand].triggerDown ? eHolding : ePressing) : eReleasing;
	if (pressed)
	{
		if (event == eHolding && !(mode.fire_activation & eHolding)) return;
		const float interval = event == ePressing ? mode.fTapFireRate : mode.fFireRate;
		if (m_player.m_pTimer->GetCurrTime() - item->fire.fireLastShot < interval) return;
	}
	IScriptSystem* scripts = m_player.m_pScriptSystem;
	IScriptObject* shooter = m_player.GetEntity()->GetScriptObject();
	_SmartScriptObject originalContext(scripts);
	CopyContext(scripts, shooter, originalContext);
	_SmartScriptObject originalWeapon(scripts, true);
	const bool hadWeapon = m_player.m_pScriptObject->GetValue("weapon", originalWeapon);
	int originalWeaponID = -1, originalMode = 1;
	const bool hadWeaponID = m_player.m_pScriptObject->GetValue("weaponid", originalWeaponID);
	shooter->GetValue("firemodenum", originalMode);
	const CPlayer::PlayerStats saved = m_player.m_stats;
	const int selected = m_player.m_nSelectedWeaponID;
	ICryCharInstance* originalCharacter = weapon->m_pCharacter;
	const Vec3 originalPosition = weapon->m_vPos, originalAngles = weapon->m_vAngles;
	Matrix34 heldModel;
	const bool modelValid = HeldModel(hand, m_hands[hand].pose, heldModel);
	if (item->character) weapon->m_pCharacter = item->character;
	if (modelValid)
	{
		weapon->m_vPos = heldModel.GetTranslation();
		weapon->m_vAngles = RAD2DEG(Ang3::GetAnglesXYZ(Matrix33(heldModel)));
	}
	m_player.m_nSelectedWeaponID = item->weaponID;
	m_player.m_pScriptObject->SetValue("weapon", weapon->GetScriptObject());
	m_player.m_pScriptObject->SetValue("weaponid", item->weaponID);
	m_player.m_stats.weapon = item->weaponID;
	m_player.m_stats.firemode = 0;
	shooter->SetValue("firemodenum", 1);
	if (!item->context)
	{
		_SmartScriptObject originalAmmo(scripts, true), savedAmmo(scripts);
		const bool hadAmmo = shooter->GetValue("Ammo", originalAmmo);
		if (hadAmmo) savedAmmo->Clone(originalAmmo);
		weapon->ScriptOnActivate(m_player.GetEntity());
		item->context = scripts->CreateObject();
		CopyContext(scripts, shooter, item->context);
		item->context->SetToNull("_MuzzleFlashParams");
		// A private weapon_info prevents an external instance consuming the
		// magazine of an owned weapon with the same class.
		_SmartScriptObject info(scripts), clips(scripts), activeInfo(scripts, true);
		if (shooter->GetValue("weapon_info", activeInfo)) info->Clone(activeInfo);
		clips->SetAt(1, item->clip);
		info->SetValue("AmmoInClip", clips);
		item->context->SetValue("weapon_info", info);
		// Activation builds effect metadata but must not transfer reserve
		// ammunition between the two independently held instances.
		if (hadAmmo) { originalAmmo->Clear(); originalAmmo->Clone(savedAmmo); shooter->SetValue("Ammo", originalAmmo); }
	}
	CopyContext(scripts, item->context, shooter);
	m_player.m_stats.ammo_in_clip = item->clip;
	m_player.m_stats.ammo = 0;
	m_player.m_stats.reloading = false;
	m_player.m_stats.weapon_busy = 0;
	m_player.m_stats.aiming = false;
	m_player.m_stats.firing = pressed;
	m_player.m_stats.FiringType = event;
	m_player.m_vrPhysicalWeaponFire = true;
	if (pressed)
	{
		Matrix34 model;
		if (HeldModel(hand, m_hands[hand].pose, model))
		{
			Matrix34 muzzle = m_hands[hand].pose;
			const char* name = "spitfire"; weapon->GetScriptObject()->GetValue("SpitFireBone", name);
			if (ICryBone* bone = weapon->GetCharacter()->GetBoneByName(name))
				muzzle = model * Matrix34(GetTransposed44(bone->GetAbsoluteMatrix())) * Matrix33::CreateRotationY(gf_PI_DIV_2);
			m_player.m_vrPhysicalFireOrigin = muzzle.GetTranslation();
			m_player.m_vrPhysicalFireAngles = RAD2DEG(Ang3::GetAnglesXYZ(Matrix33(muzzle)));
			const int shots = weapon->Fire(muzzle.GetTranslation(),
				m_player.m_vrPhysicalFireAngles, &m_player, item->fire, NULL);
			if (shots > 0 && mode.iFireModeType != FireMode_Melee)
			{
				m_player.m_pGame->GetSystem()->PulseVRController(hand == 0, .8f, .035f);
				m_hands[hand].shotHapticUntil = m_player.m_pTimer->GetCurrTime()+.035f;
			}
			// Scripts normally debit the clip. Keep native magazines correct for
			// modes whose scripts do not update the cached value.
			if (!mode.no_ammo && item->ammoType != "Unlimited")
				item->clip = max(0, min(item->clip - shots, m_player.m_stats.ammo_in_clip));
		}
	}
	else weapon->ScriptOnStopFiring(m_player.GetEntity());
	CopyContext(scripts, shooter, item->context);
	m_player.m_vrPhysicalWeaponFire = false;
	weapon->m_pCharacter = originalCharacter;
	weapon->m_vPos = originalPosition; weapon->m_vAngles = originalAngles;
	m_player.m_nSelectedWeaponID = selected;
	m_player.m_stats.weapon = saved.weapon;
	m_player.m_stats.firemode = saved.firemode;
	m_player.m_stats.ammo = saved.ammo;
	m_player.m_stats.ammo_in_clip = saved.ammo_in_clip;
	m_player.m_stats.reloading = saved.reloading;
	m_player.m_stats.weapon_busy = saved.weapon_busy;
	m_player.m_stats.aiming = saved.aiming;
	m_player.m_stats.firing = saved.firing;
	m_player.m_stats.FiringType = saved.FiringType;
	CopyContext(scripts, originalContext, shooter);
	shooter->SetValue("firemodenum", originalMode);
	if (hadWeapon) m_player.m_pScriptObject->SetValue("weapon", originalWeapon);
	else m_player.m_pScriptObject->SetToNull("weapon");
	if (hadWeaponID) m_player.m_pScriptObject->SetValue("weaponid", originalWeaponID);
	else m_player.m_pScriptObject->SetToNull("weaponid");
	SaveMagazine(*item);
}

void CVRPhysicalWeapons::UpdateMelee(Item& item)
{
	CWeaponClass* weapon = Weapon(item);
	IStatObj* object = weapon ? weapon->GetObject() : NULL;
	WeaponParams mode;
	if (!object || !weapon->GetModeParams(0, mode)) return;
	IPhysicalWorld* world = m_player.m_pGame->GetSystem()->GetIPhysicalWorld();
	if (!item.meleeGeometry)
	{
		// Prefer the render triangles so the collision footprint is the same
		// as the hand-free mesh drawn for melee weapons.
		CIndexedMesh* mesh = object->GetTriData();
		if (mesh && mesh->m_pVerts && mesh->m_pFaces && mesh->m_nFaceCount > 0 && mesh->m_nVertCount <= 65535)
		{
			std::vector<index_t> indices(mesh->m_nFaceCount * 3);
			for (int face = 0; face < mesh->m_nFaceCount; ++face)
				for (int v = 0; v < 3; ++v) indices[face * 3 + v] = mesh->m_pFaces[face].v[v];
			item.meleeGeometry = world->GetGeomManager()->CreateMesh(mesh->m_pVerts,
				&indices[0], NULL, mesh->m_nFaceCount, mesh_AABB, true, true);
			item.ownsMeleeGeometry = item.meleeGeometry != NULL;
		}
		if (!item.meleeGeometry)
		{
			CLeafBuffer* leaf = object->GetLeafBuffer();
			CLeafBuffer* vertices = leaf ? leaf->GetVertexContainer() : NULL;
			if (leaf && vertices && vertices->m_pSecVertBuffer &&
				vertices->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData &&
				leaf->m_nPrimetiveType == R_PRIMV_TRIANGLES)
			{
				int stride = 0, count = 0;
				byte* positions = leaf->GetPosPtr(stride);
				ushort* indices = leaf->GetIndices(&count);
				if (positions && indices && count >= 3)
				{
					strided_pointer<const vectorf> points;
					points.data = (const vectorf*)positions; points.iStride = stride;
					std::vector<index_t> meshIndices(indices, indices + count);
					item.meleeGeometry = world->GetGeomManager()->CreateMesh(points,
						&meshIndices[0], NULL, count / 3, mesh_AABB, true, true);
					item.ownsMeleeGeometry = item.meleeGeometry != NULL;
				}
			}
		}
	}
	const Matrix34 current = StaticModelPose(object, m_hands[item.holder].pose);
	const Vec3 bodyPosition = m_body.GetTranslation();
	const float delta = m_player.m_pTimer->GetFrameTime();
	if (!item.meleePoseValid || delta <= 0 || delta > 0.1f)
	{
		item.previousMeleePose = current; item.previousBodyPosition = bodyPosition;
		item.meleePoseValid = true; return;
	}
	const Matrix34 previous = item.previousMeleePose;
	const Vec3 bodyMotion = bodyPosition - item.previousBodyPosition;
	item.previousMeleePose = current; item.previousBodyPosition = bodyPosition;
	float movement = 0;
	Vec3 hitDirection(0,0,0);
	const Vec3 lo = object->GetBoxMin(), hi = object->GetBoxMax();
	for (int corner = 0; corner < 8; ++corner)
	{
		const Vec3 p(corner & 1 ? hi.x : lo.x, corner & 2 ? hi.y : lo.y, corner & 4 ? hi.z : lo.z);
		const Vec3 motion = current * p - previous * p - bodyMotion;
		if (motion.GetLength() > movement) { movement = motion.GetLength(); hitDirection = motion; }
	}
	const float now = m_player.m_pTimer->GetCurrTime();
	for (std::map<IPhysicalEntity*, float>::iterator i = item.meleeHits.begin(); i != item.meleeHits.end();)
		if (now - i->second > 1.0f) item.meleeHits.erase(i++); else ++i;
	// Ignore resting contact, locomotion alone, recentering and tracking jumps.
	if (!item.meleeGeometry || movement / delta < 0.8f || movement > 0.75f) return;
	hitDirection.Normalize();
	const float radius = max(lo.GetLength(), hi.GetLength());
	const Vec3 a = previous.GetTranslation(), b = current.GetTranslation();
	const Vec3 bbMin(min(a.x,b.x)-radius, min(a.y,b.y)-radius, min(a.z,b.z)-radius);
	const Vec3 bbMax(max(a.x,b.x)+radius, max(a.y,b.y)+radius, max(a.z,b.z)+radius);
	IPhysicalEntity** nearby = NULL;
	const int count = world->GetEntitiesInBox(bbMin, bbMax, nearby,
		ent_static | ent_rigid | ent_sleeping_rigid | ent_living | ent_independent);
	if (count <= 0) return;
	std::vector<IPhysicalEntity*> candidates(nearby, nearby + count);
	const Quat from((Matrix33(previous))), to((Matrix33(current)));
	const int steps = min(32, max(1, (int)ceilf(movement / 0.01f)));
	for (int targetIndex = 0; targetIndex < count; ++targetIndex)
	{
		IPhysicalEntity* target = candidates[targetIndex];
		if (!target || target == m_player.GetEntity()->GetPhysics()) continue;
		const int foreignType = target->GetiForeignData();
		if (foreignType != OT_STAT_OBJ && foreignType != OT_ENTITY) continue;
		if (item.meleeHits.find(target) != item.meleeHits.end() && now - item.meleeHits[target] < 0.35f) continue;
		pe_status_nparts parts;
		const int partCount = target->GetStatus(&parts);
		bool hitTarget = false;
		for (int partIndex = 0; partIndex < partCount && !hitTarget; ++partIndex)
		{
			pe_status_pos part; part.ipart = partIndex;
			if (!target->GetStatus(&part) || !part.pGeom) continue;
			geom_world_data targetData;
			targetData.R = matrix3x3f(part.q); targetData.offset = part.pos; targetData.scale = part.scale;
			for (int sample = 1; sample <= steps; ++sample)
			{
				const float fraction = (float)sample / steps;
				geom_world_data bladeData;
				bladeData.R = matrix3x3f(Slerp(from, to, fraction));
				bladeData.offset = a + (b - a) * fraction;
				intersection_params params; params.bStopAtFirstTri = true;
				params.bNoAreaContacts = true; params.bBothConvex = false;
				geom_contact* contacts = NULL;
				if (item.meleeGeometry->Intersect(part.pGeom, &bladeData, &targetData, &params, contacts) <= 0 || !contacts) continue;
				SWeaponHit hit = {};
				hit.pos = contacts[0].pt; hit.normal = contacts[0].n; hit.dir = hitDirection;
				hit.damage = (float)mode.nDamage; hit.ipart = part.partid;
				hit.surface_id = contacts[0].id[1];
				hit.shooter = m_player.GetEntity(); hit.weapon = weapon->GetScriptObject();
				hit.objecttype = foreignType;
				if (foreignType == OT_STAT_OBJ) hit.targetStat = (IEntityRender*)target->GetForeignData(OT_STAT_OBJ);
				else hit.target = (IEntity*)target->GetForeignData();
				if (hit.target == m_player.GetEntity()) break;
				hit.weapon_death_anim_id = mode.iDeathAnim;
				hit.iImpactForceMul = mode.iImpactForceMul;
				hit.iImpactForceMulFinal = mode.iImpactForceMulFinal;
				hit.iImpactForceMulFinalTorso = mode.iImpactForceMulFinalTorso;
				const WeaponParams saved = weapon->m_fireParams;
				weapon->m_fireParams = mode;
				weapon->ProcessHit(hit);
				weapon->m_fireParams = saved;
				item.meleeHits[target] = now;
				hitTarget = true; break;
			}
		}
	}
}

void CVRPhysicalWeapons::Reload()
{
	if (m_active) return; // VR reload requires physically inserting a magazine.
	IScriptSystem* scripts = m_player.m_pScriptSystem;
	_SmartScriptObject ammo(scripts, true);
	if (!m_player.GetEntity()->GetScriptObject()->GetValue("Ammo", ammo)) return;
	for (int hand = 0; hand < 2; ++hand)
	{
		Item* item = m_hands[hand].item;
		if (!item || !item->personal || item->ammoType.empty()) continue;
		int reserve = 0; ammo->GetValue(item->ammoType.c_str(), reserve);
		const int count = min(max(0, reserve), max(0, item->capacity - item->clip));
		item->clip += count;
		ammo->SetValue(item->ammoType.c_str(), reserve - count);
		_SmartScriptObject selectedParams(scripts, true);
		const char* selectedAmmo = "";
		if (m_player.GetEntity()->GetScriptObject()->GetValue("fireparams", selectedParams) &&
			selectedParams->GetValue("AmmoType", selectedAmmo) && item->ammoType == selectedAmmo)
			m_player.m_stats.ammo = reserve - count;
		SaveMagazine(*item);
	}
}

bool CVRPhysicalWeapons::GrabMagazine(int hand)
{
	Hand& state = m_hands[hand];
	Item* gun = m_hands[1-hand].item;
	if (state.item || state.magazine || state.propID >= 0 || !gun || gun->clip > 0 || gun->capacity <= 0) return false;
	CWeaponClass* weapon = Weapon(*gun);
	if (!weapon || IsMelee(weapon) || !gun->character) return false;
	const Vec3 head = m_player.m_pGame->GetSystem()->GetViewCamera().GetPos();
	const Vec3 reach = Matrix33(m_body).GetInverted() * (state.pose.GetTranslation()-head);
	if (fabsf(reach.x) > .85f || reach.z < -.30f || reach.z > .50f ||
		reach.y > .20f || reach.y < -.85f || (reach.y > .05f && fabsf(reach.x) < .22f)) return false;
	const char* boneName = MagazineBone(weapon, gun->character);
	if (!boneName) return false;
	_SmartScriptObject ammo(m_player.m_pScriptSystem, true);
	if (!m_player.GetEntity()->GetScriptObject()->GetValue("Ammo", ammo)) return false;
	int reserve = 0; ammo->GetValue(gun->ammoType.c_str(), reserve);
	if (reserve <= 0) return false;
	const int rounds = min(reserve, gun->capacity);
	Magazine* magazine = new Magazine;
	magazine->character = m_player.m_pGame->GetSystem()->GetIAnimationSystem()->MakeCharacter(gun->character->GetModel()->GetFileName());
	if (!magazine->character) { delete magazine; return false; }
	magazine->weaponID = gun->weaponID; magazine->rounds = rounds;
	magazine->ammoType = gun->ammoType; magazine->bone = boneName;
	magazine->scale = HeldWeaponScale(weapon);
	ICryCharInstance* character = magazine->character;
	character->SetFlags(gun->character->GetFlags() & ~CS_FLAG_DRAW_NEAR);
	character->ResetAnimations();
	CryCharAnimationParams idle; idle.fBlendInTime = idle.fBlendOutTime = 0; idle.nLayerID = 0;
	character->StartAnimation("Idle11", idle); character->Update(); character->ForceUpdate();
	ICryBone* root = character->GetBoneByName(boneName);
	if (!root) { delete magazine; return false; }
	magazine->center = root->GetBonePosition();
	std::vector<Matrix44> poses(character->GetModel()->NumBones());
	for (size_t i = 0; i < poses.size(); ++i)
	{
		ICryBone* bone = character->GetBoneByName(character->GetModel()->GetBoneName((int)i));
		ICryBone* ancestor = bone;
		while (ancestor && ancestor != root) ancestor = ancestor->GetParent();
		poses[i] = bone->GetAbsoluteMatrix();
		if (!ancestor)
		{
			poses[i] = Matrix44(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1);
			poses[i].SetTranslationOLD(magazine->center);
		}
		const_cast<Matrix44&>(bone->GetAbsoluteMatrix()) = poses[i];
	}
	character->SetRenderBonePose(&poses[0], (unsigned)poses.size());
	ammo->SetValue(gun->ammoType.c_str(), reserve-rounds);
	if (gun->weaponID == m_player.m_nSelectedWeaponID) m_player.m_stats.ammo = reserve-rounds;
	state.magazine = magazine;
	gun->slideHand = -1; gun->slideDistance = 0;
	return true;
}

void CVRPhysicalWeapons::ReleaseMagazine(int hand)
{
	Magazine* magazine = m_hands[hand].magazine;
	if (!magazine) return;
	if (magazine->rounds > 0)
	{
		_SmartScriptObject ammo(m_player.m_pScriptSystem, true);
		if (m_player.GetEntity()->GetScriptObject()->GetValue("Ammo", ammo))
		{
			int reserve = 0; ammo->GetValue(magazine->ammoType.c_str(), reserve);
			ammo->SetValue(magazine->ammoType.c_str(), reserve+magazine->rounds);
			if (magazine->weaponID == m_player.m_nSelectedWeaponID) m_player.m_stats.ammo = reserve+magazine->rounds;
		}
	}
	delete magazine; m_hands[hand].magazine = NULL;
}

bool CVRPhysicalWeapons::MagazineModel(int hand, const Matrix34& grip, Matrix34& model)
{
	Magazine* magazine = m_hands[hand].magazine;
	if (!magazine) return false;
	Matrix34 gun;
	Item* opposite = m_hands[1-hand].item;
	if (!opposite || opposite->weaponID != magazine->weaponID || !HeldModel(1-hand, m_hands[1-hand].pose, gun)) return false;
	const Matrix33 relative = Matrix33(m_hands[1-hand].pose).GetInverted()*Matrix33(gun);
	model = Matrix34(Matrix33(grip)*relative);
	const Vec3 palm = grip * (m_hands[hand].palmValid ? m_hands[hand].palmOffset : Vec3(0,0,0));
	model.SetTranslation(palm-Matrix33(model)*magazine->center);
	return true;
}

void CVRPhysicalWeapons::UpdateMagazine(int hand)
{
	Magazine* magazine = m_hands[hand].magazine;
	if (!magazine) return;
	Item* gun = m_hands[1-hand].item;
	if (!gun || gun->weaponID != magazine->weaponID || gun->clip > 0 || !gun->character)
	{ ReleaseMagazine(hand); return; }
	ICryBone* socket = gun->character->GetBoneByName(magazine->bone.c_str());
	Matrix34 weaponModel, magazineModel;
	if (!socket || !HeldModel(1-hand, m_hands[1-hand].pose, weaponModel) || !MagazineModel(hand,m_hands[hand].pose,magazineModel)) return;
	const Vec3 target = weaponModel*socket->GetBonePosition();
	if ((magazineModel*magazine->center-target).GetLengthSquared() > .07f*.07f) return;
	gun->clip = magazine->rounds; magazine->rounds = 0;
	SaveMagazine(*gun);
	ReleaseMagazine(hand);
}

bool CVRPhysicalWeapons::IsOwnBody(IPhysicalEntity* body) const
{
	IEntity* player = m_player.GetEntity();
	if (!body || body == player->GetPhysics() || body->GetForeignData() == player) return true;
	// VR proxies use a dedicated foreign-data type. GetForeignData() asks
	// for type zero and therefore cannot identify these bodies by owner.
	if (body->GetiForeignData() == PHYS_FOREIGN_ID_VR_BODY) return true;
	for (int slot = 0; slot < 2; ++slot)
	{
		ICryCharInstance* character = player->GetCharInterface()->GetCharacter(slot);
		if (!character) continue;
		if (body == character->GetCharacterPhysics() || body->GetForeignData() == character) return true;
		for (int aux = 0; aux < 16; ++aux)
		{
			IPhysicalEntity* physics = character->GetCharacterPhysics(aux);
			if (!physics) break;
			if (body == physics) return true;
		}
	}
	return false;
}

// Force-based grab: the object remains simulated, with contacts and gravity.
void CVRPhysicalWeapons::GrabProp(int hand)
{
	Hand& state = m_hands[hand];
	if (state.item || state.propID >= 0) return;
	IPhysicalWorld* physics = m_player.m_pGame->GetSystem()->GetIPhysicalWorld();
	const Vec3 palmOffset = state.palmValid ? state.palmOffset : Vec3(0,0,0);
	const Vec3 center = state.pose * palmOffset, radius(.16f,.16f,.16f);
	IPhysicalEntity** list = NULL;
	const int count = physics->GetEntitiesInBox(center-radius, center+radius,
		list, ent_rigid | ent_sleeping_rigid | ent_independent);
	std::vector<IPhysicalEntity*> candidates;
	if (count > 0) candidates.assign(list, list+count);
	float nearest = .16f*.16f;
	for (size_t i = 0; i < candidates.size(); ++i)
	{
		IPhysicalEntity* body = candidates[i];
		if (IsOwnBody(body) ||
			(body->GetType() != PE_RIGID && body->GetType() != PE_ARTICULATED)) continue;
		bool weaponBody = false;
		for (size_t j = 0; j < m_items.size(); ++j)
			if (m_items[j]->body == body || (m_items[j]->entity &&
				m_player.m_pGame->GetSystem()->GetIEntitySystem()->GetEntity(m_items[j]->entity) &&
				m_player.m_pGame->GetSystem()->GetIEntitySystem()->GetEntity(m_items[j]->entity)->GetPhysics() == body)) weaponBody = true;
		if (weaponBody) continue;
		pe_status_nparts np; const int parts = body->GetStatus(&np);
		for (int part = 0; part < parts; ++part)
		{
			pe_status_pos pose; pose.ipart = part;
			if (!body->GetStatus(&pose) || !(pose.flagsOR & geom_collides)) continue;
			IGeometry* geometry = pose.pGeom ? pose.pGeom : pose.pGeomProxy;
			if (!geometry) continue;
			geom_world_data world; world.offset = pose.pos;
			world.R = matrix3x3f(pose.q); world.scale = pose.scale;
			int primitive = 0, feature = 0; vectorf closest[2];
			if (geometry->FindClosestPoint(&world, primitive, feature, center, center, closest) < 0) continue;
			IEntity* entity = body->GetiForeignData() == 0 ? (IEntity*)body->GetForeignData(0) : NULL;
			IStatObj* object = entity && body->GetType() == PE_RIGID ? entity->GetIStatObj(part) : NULL;
			CIndexedMesh* mesh = object ? object->GetTriData() : NULL;
			if (mesh && mesh->m_pVerts && mesh->m_pFaces && mesh->m_nFaceCount > 0 && mesh->m_nVertCount <= 65535)
			{
				std::vector<index_t> indices(mesh->m_nFaceCount*3);
				for (int f = 0; f < mesh->m_nFaceCount; ++f)
					for (int v = 0; v < 3; ++v) indices[f*3+v] = mesh->m_pFaces[f].v[v];
				IGeometry* visible = physics->GetGeomManager()->CreateMesh(mesh->m_pVerts, &indices[0], NULL, mesh->m_nFaceCount, mesh_AABB, true, true);
				if (visible)
				{
					primitive = feature = 0;
					vectorf surface[2];
					if (visible->FindClosestPoint(&world, primitive, feature, center, center, surface) >= 0)
						closest[0] = surface[0];
					visible->Release();
				}
			}
			const float distance = (Vec3(closest[0])-center).GetLengthSquared();
			if (distance >= nearest) continue;
			pe_status_dynamics dynamics; dynamics.ipart = part;
			if (!body->GetStatus(&dynamics) || dynamics.mass <= 0) continue;
			const int id = physics->GetPhysicalEntityId(body);
			if (id < 0) continue;
			nearest = distance; state.propID = id;
			state.propPart = part;
			Matrix34 transform(Matrix33(pose.q)); transform.SetTranslation(pose.pos);
			state.propAnchor = transform.GetInverted() * Vec3(closest[0]);
			// Put the avatar palm on the selected surface, not the controller
			// origin or an arbitrary offset in empty space.
			state.propHandAnchor = palmOffset;
			state.propGripRotation = !Quat(Matrix33(state.pose)) * Quat(pose.q);
			static int grabSamples = 0;
			if (grabSamples++<32) VRPhysicsTrace("[VRGrabSelected] hand=%d owner=%d slot=%d distance=%.4f",hand,id,part,sqrtf(distance));
		}
	}
}

IPhysicalEntity* CVRPhysicalWeapons::GetHeldPropPhysics(int hand) const
{
	if (m_hands[hand].propID < 0) return NULL;
	return m_player.m_pGame->GetSystem()->GetIPhysicalWorld()->GetPhysicalEntityById(m_hands[hand].propID);
}

bool CVRPhysicalWeapons::GetPropHandTarget(int hand, Matrix34& target)
{
	Hand& state = m_hands[hand];
	if (state.propID < 0) return false;
	IPhysicalEntity* body = m_player.m_pGame->GetSystem()->GetIPhysicalWorld()->GetPhysicalEntityById(state.propID);
	pe_status_pos pose; pose.ipart = state.propPart;
	if (IsOwnBody(body) || !body->GetStatus(&pose)) return false;
	target = Matrix34(Matrix33(Quat(pose.q) * !state.propGripRotation));
	if (HasPhysicalGrip(hand)) {
		IPhysicalEntity* physicalHand = m_player.m_pGame->GetSystem()->GetIPhysicalWorld()->GetPhysicalEntityById(state.gripHandID);
		pe_status_pos handPose;
		if (physicalHand && physicalHand->GetStatus(&handPose)) {
			target = Matrix34(Matrix33(Quat(handPose.q)*!state.gripHandRotation));
			const float now = m_player.m_pTimer->GetCurrTime();
			if (state.gripTraceCount<64 && now>=state.nextGripTrace) {
				state.nextGripTrace = now+.25f; ++state.gripTraceCount;
				const Vec3 surface = Vec3(pose.pos)+Matrix33(pose.q)*state.propAnchor;
				const Vec3 palm = Vec3(handPose.pos)+Matrix33(handPose.q)*state.gripHandAnchor;
				const Quat desired = Quat(handPose.q)*state.gripObjectRotation;
				const Quat error = Quat(pose.q)*!desired;
				const float angle = 2*cry_acosf(min(1.0f,fabsf(error.w)));
				const Quat trackingError = Quat(Matrix33(state.pose))*state.gripHandRotation*!Quat(handPose.q);
				const float trackingAngle = 2*cry_acosf(min(1.0f,fabsf(trackingError.w)));
				VRPhysicsTrace("[VRPhysicalGripError] owner=%d hand=%d separation=%.5f angle=%.4f controllerLag=%.3f trackingAngle=%.4f",state.propID,
					state.gripHandID,(surface-palm).GetLength(),angle,(state.pose*state.propHandAnchor-surface).GetLength(),trackingAngle);
				pe_status_vr_drive drive;
				if (physicalHand->GetStatus(&drive)) {
					const Quat nativeError = Quat(drive.targetRotation)*!Quat(handPose.q);
					VRPhysicsTrace("[VRGripDrive] hand=%d dt=%.5f age=%.4f force=%.2f torque=%.2f nativeAngle=%.4f angularBody=%d",state.gripHandID,
						drive.dt,drive.age,Vec3(drive.force).GetLength(),Vec3(drive.torque).GetLength(),2*cry_acosf(min(1.0f,fabsf(nativeError.w))),drive.angularBodyId);
					VRPhysicsTrace("[VRGripAngularRate] requested=(%.3f %.3f %.3f) before=(%.3f %.3f %.3f) after=(%.3f %.3f %.3f)",
						drive.requestedW.x,drive.requestedW.y,drive.requestedW.z,drive.beforeW.x,drive.beforeW.y,drive.beforeW.z,drive.afterW.x,drive.afterW.y,drive.afterW.z);
				}
			}
			// Draw the actual physical wrist using the same reference as its
			// drive. An object-anchor correction would hide a broken joint and
			// introduce a second, inconsistent wrist location into the arm IK.
			target.SetTranslation(Vec3(handPose.pos)-Matrix33(target)*state.gripHandOffset);
			return true;
		}
	}
	const Vec3 anchor = Vec3(pose.pos)+Matrix33(pose.q)*state.propAnchor;
	// Keep the palm contact on the moving surface while the physical body lags.
	target.SetTranslation(anchor-Matrix33(target)*state.propHandAnchor);
	return true;
}

bool CVRPhysicalWeapons::HasPhysicalGrip(int hand) const
{
	return hand>=0 && hand<2 && m_hands[hand].gripConstraint>0 && m_hands[hand].gripAngularConstraint>0;
}

bool CVRPhysicalWeapons::CanRecoverHand(int hand) const
{
	// Weapons, magazines and slides do not create a physical prop grip.
	return hand>=0 && hand<2 && m_hands[hand].propID<0 &&
		m_hands[hand].gripConstraint==0 && m_hands[hand].gripAngularConstraint==0;
}

bool CVRPhysicalWeapons::GetGripTrackingRotation(int hand, Quat& rotation, Vec3* angularVelocity) const
{
	if (!HasPhysicalGrip(hand) || !m_hands[hand].tracked) return false;
	rotation = Quat(Matrix33(m_hands[hand].pose))*m_hands[hand].gripHandRotation;
	if (angularVelocity) *angularVelocity = m_hands[hand].angularVelocity;
	return true;
}

bool CVRPhysicalWeapons::GetGripTrackingPose(int hand, Matrix34& pose) const
{
	Quat rotation;
	if (!GetGripTrackingRotation(hand,rotation)) return false;
	const Hand& state = m_hands[hand];
	pose = Matrix34(Matrix33(rotation));
	pose.SetTranslation(state.pose*state.gripHandOffset);
	return true;
}

void CVRPhysicalWeapons::RemovePhysicalGrip(int hand)
{
	Hand& state = m_hands[hand];
	if (state.gripConstraint>0) {
		IPhysicalEntity* owner = GetHeldPropPhysics(hand);
		if (owner && (owner->GetType()==PE_ARTICULATED || owner->GetType()==PE_RIGID)) {
			pe_action_remove_constraint remove; remove.idConstraint = state.gripConstraint;
			owner->Action(&remove);
			if (state.gripAngularConstraint>0) {
				remove.idConstraint = state.gripAngularConstraint; owner->Action(&remove);
				if (owner->GetType()==PE_RIGID) {
					pe_action_vr_grip_owner carrier;
					carrier.carrierId = m_player.m_pGame->GetSystem()->GetIPhysicalWorld()->GetPhysicalEntityId(m_player.GetEntity()->GetPhysics());
					carrier.hand = hand; carrier.enabled = 0; owner->Action(&carrier);
				}
			}
		}
	}
	state.gripConstraint = 0; state.gripAngularConstraint = 0; state.gripHandID = -1;
	if (IPhysicalEntity* playerBody = m_player.GetEntity()->GetPhysics()) {
		pe_action_vr_locomotion_limit limit; limit.hand = hand;
		playerBody->Action(&limit);
	}
	if (m_player.m_pVRBodyPhysics) m_player.m_pVRBodyPhysics->ReleaseHandConstraints(hand);
}

bool CVRPhysicalWeapons::CreatePhysicalGrip(int hand)
{
	Hand& state = m_hands[hand];
	if (HasPhysicalGrip(hand)) return true;
	IPhysicalEntity* owner = GetHeldPropPhysics(hand);
	IPhysicalEntity* physicalHand = m_player.m_pVRBodyPhysics ? m_player.m_pVRBodyPhysics->GetHandPhysics(hand) : NULL;
	pe_status_pos part; part.ipart = state.propPart;
	pe_status_pos palm;
	if (!owner || !physicalHand || !owner->GetStatus(&part) || !physicalHand->GetStatus(&palm)) {
		if (state.gripTraceCount<8) {
			++state.gripTraceCount;
			VRPhysicsTrace("[VRPhysicalGripFailed] hand=%d owner=%d physicalHand=%d part=%d",hand,owner!=NULL,physicalHand!=NULL,state.propPart);
		}
		return false;
	}
	// pe_status_pos does not return the part id when queried by slot. Resolve
	// it through pe_params_part; passing status.partid (-1) rejects the grip.
	pe_params_part selected; selected.ipart = state.propPart;
	if (!owner->GetParams(&selected)) return false;
	const Vec3 point = Vec3(part.pos)+Matrix33(part.q)*state.propAnchor;
	Matrix34 trackingHand;
	if (!m_player.m_pVRBodyPhysics->GetHandTrackingPose(hand,trackingHand)) return false;
	// Acquisition must not create a joint whose hand is still metres away.
	// Keep the pending grab and retry as the free physical hand approaches.
	if ((point-Vec3(palm.pos)).GetLengthSquared() > .35f*.35f) return false;
	pe_action_add_constraint grip;
	grip.pBuddy = physicalHand;
	grip.partid[0] = selected.partid;
	grip.pt[0] = point;
	// Start with coincident attachment points. An acquisition radius must
	// not become initial constraint error and inject a corrective impulse.
	grip.pt[1] = point;
	state.propHandAnchor = state.pose.GetInverted()*point;
	// Lock translation and all three relative rotation axes in the solver.
	// Tracking motors remain finite: load displaces the physical hand/arm.
	state.gripConstraint = owner->Action(&grip);
	if (state.gripConstraint<=0) {
		if (state.gripTraceCount++<8) VRPhysicsTrace("[VRPhysicalGripRejected] hand=%d owner=%d slot=%d part=%d",hand,state.propID,state.propPart,selected.partid);
		state.gripConstraint = 0; return false;
	}
	grip.flags = world_frames | fixed_angular_only;
	grip.qframe[0] = grip.qframe[1] = palm.q;
	state.gripAngularConstraint = owner->Action(&grip);
	if (state.gripAngularConstraint<=0) { RemovePhysicalGrip(hand); return false; }
	if (owner->GetType()==PE_RIGID) {
		pe_action_vr_grip_owner carrier;
		carrier.carrierId = m_player.m_pGame->GetSystem()->GetIPhysicalWorld()->GetPhysicalEntityId(m_player.GetEntity()->GetPhysics());
		carrier.hand = hand;
		if (!owner->Action(&carrier)) { RemovePhysicalGrip(hand); return false; }
	}
	state.gripHandID = m_player.m_pGame->GetSystem()->GetIPhysicalWorld()->GetPhysicalEntityId(physicalHand);
	// Calibration belongs to the anatomical tracking target. Capturing the
	// displaced physics pose here preserves load-induced error as a NEW target.
	state.gripHandRotation = !Quat(Matrix33(state.pose))*Quat(Matrix33(trackingHand));
	state.gripHandOffset = state.pose.GetInverted()*trackingHand.GetTranslation();
	state.gripObjectRotation = !Quat(palm.q)*Quat(part.q);
	state.gripHandAnchor = !Quat(palm.q)*(Vec3(grip.pt[1])-Vec3(palm.pos));
	VRPhysicsTrace("[VRPhysicalGrip6DOF] owner=%d hand=%d part=%d position=%d orientation=%d",state.propID,state.gripHandID,selected.partid,state.gripConstraint,state.gripAngularConstraint);
	pe_params_flags solverFlags;
	if (owner->GetParams(&solverFlags)) VRPhysicsTrace("[VRGripSolver] owner=%d simple=%d motor=bounded-solver frames=anatomical-v2 reaction=bounded-pair carrier=pair-filter-v1 angular=sampled-pose-v3 inertia=volume-correct root=velocity-planes carrier=substep-frame",state.propID,(solverFlags.flags & ref_use_simple_solver)!=0);
	pe_status_dynamics load; load.ipart = state.propPart;
	if (owner->GetStatus(&load)) VRPhysicsTrace("[VRGripLoad] owner=%d mass=%.4f inertiaInverse=(%.3f %.3f %.3f)",state.propID,load.mass,load.inertiaInverse(0,0),load.inertiaInverse(1,1),load.inertiaInverse(2,2));
	return true;
}

void CVRPhysicalWeapons::LimitNPCGripMovement(Vec3& velocity)
{
    IPhysicalEntity* playerBody = m_player.GetEntity()->GetPhysics();
    if (!playerBody) return;
    const float dt = max(.001f,min(.05f,m_player.m_pTimer->GetFrameTime()));
    vectorf normals[2] = {vectorf(zero),vectorf(zero)};
    float limits[2] = {0,0}; bool enabled[2] = {false,false};
    for (int hand=0;hand<2;++hand) {
        pe_action_vr_locomotion_limit limit; limit.hand = hand;
        Vec3 shoulder,wrist,wristVelocity; float reach;
        if (m_active && HasPhysicalGrip(hand) && m_player.m_pVRBodyPhysics &&
            m_player.m_pVRBodyPhysics->GetArmReach(hand,m_player.GetEntity(),shoulder,reach) &&
            m_player.m_pVRBodyPhysics->GetPhysicalWrist(hand,wrist,wristVelocity)) {
            Vec3 radial = shoulder-wrist;
            const float height = radial.z; radial.z = 0;
            const float distance = radial.GetLength();
            if (distance>.001f) {
                const Vec3 outward = radial/distance;
                Vec3 relative = velocity-wristVelocity; relative.z = 0;
                const Vec3 tangent = relative-outward*relative.Dot(outward);
                const float radius = max(.1f,reach-.02f);
                const float available = sqrtf(max(0.0f,radius*radius-height*height-tangent.GetLengthSquared()*dt*dt));
                limit.normal = outward;
                limit.maxSpeed = max(0.0f,wristVelocity.Dot(outward)+max(0.0f,available-distance)/dt);
                limit.enabled = 1;
                normals[hand] = limit.normal; limits[hand] = limit.maxSpeed; enabled[hand] = true;
            }
        }
        // One locomotion authority: constrain requested motion and the living
        // body's coast inside its own step. The hand drive supplies all pull
        // forces; do not separately kick the capsule or grabbed object.
        playerBody->Action(&limit);
    }
    vectorf requested = velocity;
    ProjectVRGripVelocity(requested,normals,limits,enabled);
    velocity = requested;
}

void CVRPhysicalWeapons::UpdateProp(int hand)
{
	Hand& state = m_hands[hand];
	if (state.propID < 0) return;
	IPhysicalEntity* body = GetHeldPropPhysics(hand);
	pe_status_pos part; part.ipart = state.propPart;
	if (IsOwnBody(body) || !body->GetStatus(&part)) {
		RemovePhysicalGrip(hand); state.propID = -1; return;
	}
	// This path is shared by rigid props, living rigs and corpses. The only
	// grip motor is the dynamic hand's bounded tracking drive. All load passes
	// through solver constraints; never set the grabbed body's pose/velocity.
	CreatePhysicalGrip(hand);
	pe_action_awake awake; body->Action(&awake);
}

void CVRPhysicalWeapons::Update()
{
	static int updateSamples = 0;
	if (updateSamples++<3) VRPhysicsTrace("[VRGripBuild-20261005-update-owner] Update reached player=%d",m_player.GetEntity()->GetId());
	ISystem* system = m_player.m_pGame->GetSystem();
	float amounts[2][2] = {};
	bool anyTracked = false;
	for (int hand = 0; hand < 2; ++hand)
	{
		m_hands[hand].tracked = system->GetVRControllerTransform(hand == 0, m_hands[hand].pose) &&
			system->GetVRFingerInput(hand == 0, amounts[hand][0], amounts[hand][1]);
		anyTracked |= m_hands[hand].tracked;
	}
	const bool wasActive = m_active;
	m_active = m_player.IsAlive() && m_player.m_bFirstPerson && !m_player.m_pVehicle && !m_player.m_pMountedWeapon;
	if ((!m_active || !anyTracked) && m_player.m_pVRBodyPhysics)
	{
		RemovePhysicalGrip(0); RemovePhysicalGrip(1);
		delete m_player.m_pVRBodyPhysics; m_player.m_pVRBodyPhysics = NULL;
	}
	if (m_active && !wasActive)
	{
		IPhysicalEntity* physics = m_player.GetEntity()->GetPhysics();
		pe_player_dimensions dimensions;
		if (physics && physics->GetParams(&dimensions)) m_player.SetPhysicalDimensions(physics, dimensions);
	}
	m_player.GetEntity()->GetScriptObject()->SetValue("VRPhysicalWeapons", m_active ? 1 : 0);
	UpdateBody();
	const float now = m_player.m_pTimer->GetCurrTime();
	if (now >= m_nextInventorySync)
	{ SyncInventory(); m_nextInventorySync = now + 0.5f; }
	IEntitySystem* entities = system->GetIEntitySystem();
	for (unsigned i = 0; i < m_items.size(); ++i)
	{
		Item& item = *m_items[i];
		if (item.state != Item::Dropped) continue;
		if (item.entity && !entities->GetEntity(item.entity))
		{
			item.entity = 0;
			if (!item.body) { item.state = Item::Consumed; continue; }
		}
		if (item.personal && now >= item.recallAt) { ReturnToHolster(item); continue; }
		if (item.body)
		{
			pe_status_pos pose;
			if (item.body->GetStatus(&pose))
			{ item.model = Matrix34(Matrix33(pose.q)); item.model.SetTranslation(pose.pos); }
		}
		else if (item.entity)
		{
			if (IEntity* entity = entities->GetEntity(item.entity))
				item.model = Matrix34::CreateRotationXYZ(Deg2Rad(entity->GetAngles()), entity->GetPos());
			else { item.entity = 0; item.state = Item::Consumed; }
		}
	}
	const Vec3 cameraOrigin = system->GetViewCamera().GetPos();
	Vec3 locomotionVelocity(0,0,0);
	pe_status_dynamics playerDynamics;
	if (m_player.GetEntity()->GetPhysics() && m_player.GetEntity()->GetPhysics()->GetStatus(&playerDynamics))
		locomotionVelocity = playerDynamics.v;
	for (int hand = 0; hand < 2; ++hand)
	{
		Hand& state = m_hands[hand];
		if (!m_active || !m_player.IsAlive() || !state.tracked)
		{
			Release(hand, true);
			state.gripDown = state.triggerDown = state.previousValid = false;
			state.palmValid = false;
			continue;
		}
		const float delta = m_player.m_pTimer->GetFrameTime();
		Vec3 velocity(0,0,0);
		Vec3 angularVelocity(0,0,0);
		const Quat rotation(Matrix33(state.pose));
		if (state.previousValid && delta > 0.001f && delta < 0.15f)
		{
			velocity = (state.pose.GetTranslation()-state.previousPosition)/delta;
			Quat change = rotation * !state.previousRotation;
			if (change.w < 0) { change.w = -change.w; change.v = -change.v; }
			const float sine = change.v.GetLength();
			if (sine > 1.0e-4f)
				angularVelocity = change.v * (2.0f * cry_acosf(max(-1.0f, min(1.0f, change.w))) / (sine * delta));
		}
		if (velocity.GetLength() > 8.0f) velocity *= 8.0f / velocity.GetLength();
		if (angularVelocity.GetLength() > 20.0f) angularVelocity *= 20.0f / angularVelocity.GetLength();
		state.velocity = velocity;
		// The implicit motor supplies damping. Delaying its feed-forward speed
		// by a second low-pass filter creates lag when starting/stopping a turn.
		state.angularVelocity = angularVelocity;
		state.previousRotation = rotation;
		state.previousPosition = state.pose.GetTranslation();
		state.previousCameraOrigin = cameraOrigin; state.previousValid = true;
		const bool grip = amounts[hand][0] > (state.gripDown ? 0.35f : 0.65f);
		const bool trigger = amounts[hand][1] > (state.triggerDown ? 0.35f : 0.55f);
		if (grip!=state.gripDown) VRPhysicsTrace("[VRGripInput] hand=%d down=%d amount=%.3f tracked=%d item=%d magazine=%d prop=%d",hand,grip,amounts[hand][0],state.tracked,state.item!=NULL,state.magazine!=NULL,state.propID);
		if (grip && !state.gripDown && !state.item && !state.magazine && state.propID < 0 && !GrabMagazine(hand) && !GrabSlide(hand))
		{
			if (m_hands[1-hand].propID >= 0) GrabProp(hand);
			if (state.propID < 0) Grab(hand);
			if (!state.item && state.propID < 0) GrabProp(hand);
		}
		if (!grip && state.gripDown) Release(hand);
		if (state.item)
		{
			Item& item = *state.item;
			if (item.character)
			{
				item.character->SetRenderBonePose(NULL, 0);
				// Restore pristine animation matrices before processing another
				// frame. Rendering/slide offsets must never become the next base.
				for (size_t i = 0; i < item.animationPose.size(); ++i)
					const_cast<Matrix44&>(item.character->GetBoneByName(item.character->GetModel()->GetBoneName((int)i))->GetAbsoluteMatrix()) = item.animationPose[i];
				item.character->Update(state.pose.GetTranslation());
				item.character->ForceUpdate();
				item.animationPose.resize(item.character->GetModel()->NumBones());
				for (size_t i = 0; i < item.animationPose.size(); ++i)
					item.animationPose[i] = item.character->GetBoneByName(item.character->GetModel()->GetBoneName((int)i))->GetAbsoluteMatrix();
				CWeaponClass* weapon = Weapon(item);
				ICryBone* gunRoot = item.character->GetBoneByName("weapon");
				if (gunRoot)
				{
					// Technical BoneXX names include fingers. Preserve the entire
					// gun subtree and collapse every remaining arm/root weight.
					const Vec3 pivot = gunRoot->GetBonePosition();
					for (int i = 0; i < item.character->GetModel()->NumBones(); ++i)
					{
						ICryBone* bone = item.character->GetBoneByName(item.character->GetModel()->GetBoneName(i));
						ICryBone* ancestor = bone;
						while (ancestor && ancestor != gunRoot) ancestor = ancestor->GetParent();
						if (ancestor) continue;
						Matrix44 collapsed(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1);
						collapsed.SetTranslationOLD(pivot);
						const_cast<Matrix44&>(bone->GetAbsoluteMatrix()) = collapsed;
					}
				}
				// Hide authored first-person arms: the tracked avatar supplies them.
				for (int side = 0; !gunRoot && side < 2; ++side)
				{
					ICryBone* bone = item.character->GetBoneByName(weapon->GetVRHandBoneName(side == 0));
					if (!bone) continue;
					ICryBone* handRoot = bone;
					const Vec3 armPivot = bone->GetBonePosition();
					// Absolute matrices are independent at skinning time. Collapse
					// only the arm joints, retaining the gun bones below the wrist.
					for (int parent = 0; parent < 4 && bone; ++parent, bone = bone->GetParent())
					{
						Matrix44 collapsed(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1);
						collapsed.SetTranslationOLD(armPivot);
						const_cast<Matrix44&>(bone->GetAbsoluteMatrix()) = collapsed;
					}
					// Finger weights also belong to the authored arms. Keep gun
					// joints intact even when they are parented beneath the wrist.
					for (int i = 0; i < item.character->GetModel()->NumBones(); ++i)
					{
						const char* name = item.character->GetModel()->GetBoneName(i);
						if (!strstr(name, "Finger") && !strstr(name, "finger")) continue;
						ICryBone* finger = item.character->GetBoneByName(name);
						ICryBone* ancestor = finger;
						while (ancestor && ancestor != handRoot) ancestor = ancestor->GetParent();
						if (!ancestor) continue;
						Matrix44 collapsed(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1);
						collapsed.SetTranslationOLD(armPivot);
						const_cast<Matrix44&>(finger->GetAbsoluteMatrix()) = collapsed;
					}
				}
			}
			if (IsMelee(Weapon(item))) UpdateMelee(item);
			else if (trigger && item.slideHand < 0) Fire(hand, true);
			else if (state.triggerDown) Fire(hand, false);
		}
		state.gripDown = grip; state.triggerDown = trigger;
	}
	// Evaluate slide acquisition after both hands and weapon poses are updated.
	UpdateProximityHaptics();
	// A held grip can engage when the free hand reaches the slide; it need not
	// be pressed in the acquisition radius on precisely one input frame.
	for (int hand = 0; hand < 2; ++hand)
		if (m_hands[hand].tracked && m_hands[hand].gripDown && !m_hands[hand].item && !m_hands[hand].magazine && m_hands[hand].propID < 0)
		{
			Item* opposite = m_hands[1 - hand].item;
			// Retry the shoulder gesture while grip remains held; missing the
			// zone on the button edge must not block magazine acquisition.
			if (GrabMagazine(hand)) continue;
			if (opposite && opposite->slideHand == hand) continue;
			if (now >= m_hands[hand].nextPropScan)
			{
				m_hands[hand].nextPropScan = now+.10f;
				GrabProp(hand);
				if (m_hands[hand].propID >= 0) continue;
			}
			if (!GrabSlide(hand)) Grab(hand, false);
		}
	// Run physics independently of avatar visibility and render recursion.
	if (m_active && anyTracked) m_player.UpdateVRBodyPhysics();
	for (int hand = 0; hand < 2; ++hand)
		if (m_hands[hand].tracked && m_hands[hand].gripDown) UpdateProp(hand);
	for (int hand = 0; hand < 2; ++hand) UpdateMagazine(hand);
	for (int hand = 0; hand < 2; ++hand)
	{
		Item* item = m_hands[hand].item;
		if (!item || !item->character) continue;
		UpdateSlidePose(*item);
		if (item->clip <= 0)
		{
			const char* name = MagazineBone(Weapon(*item), item->character);
			ICryBone* root = name ? item->character->GetBoneByName(name) : NULL;
			if (root)
			{
				const Vec3 pivot = root->GetBonePosition();
				for (int i = 0; i < item->character->GetModel()->NumBones(); ++i)
				{
					ICryBone* bone = item->character->GetBoneByName(item->character->GetModel()->GetBoneName(i));
					ICryBone* ancestor = bone;
					while (ancestor && ancestor != root) ancestor = ancestor->GetParent();
					if (!ancestor) continue;
					Matrix44 collapsed(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1);
					collapsed.SetTranslationOLD(pivot);
					const_cast<Matrix44&>(bone->GetAbsoluteMatrix()) = collapsed;
				}
			}
		}
		std::vector<Matrix44> pose(item->character->GetModel()->NumBones());
		for (size_t i = 0; i < pose.size(); ++i)
			pose[i] = item->character->GetBoneByName(item->character->GetModel()->GetBoneName((int)i))->GetAbsoluteMatrix();
		if (!pose.empty()) item->character->SetRenderBonePose(&pose[0], (unsigned)pose.size());
	}
	for (size_t i = 0; i < m_items.size();)
	{
		if (m_items[i]->state == Item::Consumed)
		{ DestroyBody(*m_items[i]); delete m_items[i]; m_items.erase(m_items.begin() + i); }
		else ++i;
	}
}

void CVRPhysicalWeapons::Render(const SRendParams& params)
{
	if (!m_active || params.pShadowVolumeLightSource || !m_player.IsAlive()) return;
	UpdateBody();
	for (unsigned i = 0; i < m_items.size(); ++i)
	{
		Item& item = *m_items[i];
		if (item.state == Item::Consumed || (item.state == Item::Dropped && item.entity && !item.body)) continue;
		CWeaponClass* weapon = Weapon(item);
		if (!weapon || !weapon->GetObject()) continue;
		Matrix34 model;
		ICryCharInstance* character = NULL;
		if (item.state == Item::Held)
		{
			// Use the same current camera/controller mapping as avatar arm IK,
			// rather than the pose cached before player movement this frame.
			Matrix34 renderGrip = m_hands[item.holder].pose;
			m_player.m_pGame->GetSystem()->GetVRControllerTransform(item.holder == 0, renderGrip);
			if (IsMelee(weapon)) model = StaticModelPose(weapon->GetObject(), renderGrip);
			else if (HeldModel(item.holder, renderGrip, model)) character = item.character;
			else model = StaticModelPose(weapon->GetObject(), renderGrip);
		}
		else if (item.state == Item::Holstered) model = StaticModelPose(weapon->GetObject(), HolsterPose(item.slot));
		else model = item.model;
		if (item.state == Item::Dropped &&
			(model.GetTranslation() - m_player.m_pGame->GetSystem()->GetViewCamera().GetPos()).GetLengthSquared() > 100.0f * 100.0f) continue;
		Matrix44 matrix = GetTransposed44(Matrix44(model));
		SRendParams render = params;
		render.pMatrix = &matrix; render.pMaterial = NULL;
		render.vPos = model.GetTranslation(); render.fScale = 1;
		render.dwFObjFlags &= ~FOB_NEAREST;
		if (character) character->Draw(render, render.vPos);
		else weapon->GetObject()->Render(render, render.vPos, 0);
	}
	for (int hand = 0; hand < 2; ++hand)
	{
		Magazine* magazine = m_hands[hand].magazine;
		if (!magazine) continue;
		Matrix34 grip = m_hands[hand].pose, model;
		m_player.m_pGame->GetSystem()->GetVRControllerTransform(hand == 0, grip);
		if (!MagazineModel(hand, grip, model)) continue;
		Matrix44 matrix = GetTransposed44(Matrix44(model));
		SRendParams render = params; render.pMatrix = &matrix; render.pMaterial = NULL;
		render.vPos = model.GetTranslation(); render.fScale = 1; render.dwFObjFlags &= ~FOB_NEAREST;
		magazine->character->Draw(render, render.vPos);
	}

}

void CPlayer::UpdateVRPhysicalWeapons()
{
	if (!IsMyPlayer()) return;
	if (!m_pVRPhysicalWeapons)
	{
		Matrix34 pose;
		if (!m_pGame->GetSystem()->GetVRControllerTransform(true, pose) &&
			!m_pGame->GetSystem()->GetVRControllerTransform(false, pose)) return;
		m_pVRPhysicalWeapons = new CVRPhysicalWeapons(*this);
	}
	m_pVRPhysicalWeapons->Update();
}

bool CPlayer::IsVRPhysicalWeaponsActive() const
{
	return m_pVRPhysicalWeapons && m_pVRPhysicalWeapons->IsActive();
}
