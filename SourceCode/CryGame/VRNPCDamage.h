#ifndef CRYGAME_VR_NPC_DAMAGE_H
#define CRYGAME_VR_NPC_DAMAGE_H

struct SWeaponHit;
class CVRNPCDamageController
{
public:
	struct Hit {
		Hit() : entity(0), part(-1), impulse(0,0,0), position(0,0,0) {}
		EntityId entity;
		int part;
		Vec3 impulse,position;
	};
	static bool Prepare(ISystem* system, const SWeaponHit& hit, Hit& reaction);
	static void Apply(ISystem* system, const Hit& reaction);
};
#endif
