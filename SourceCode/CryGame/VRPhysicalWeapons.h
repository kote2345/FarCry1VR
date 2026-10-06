#ifndef CRYGAME_VR_PHYSICAL_WEAPONS_H
#define CRYGAME_VR_PHYSICAL_WEAPONS_H

class CPlayer;
class CWeaponClass;
struct WeaponInfo;

// Local VR interaction owns instances, not weapon classes. Weapon classes
// remain shared definitions; magazines and fire clocks belong to instances.
class CVRPhysicalWeapons
{
public:
	explicit CVRPhysicalWeapons(CPlayer& player);
	~CVRPhysicalWeapons();
	void Update();
	void Render(const SRendParams& params);
	void Reload();
	void SetHandPalm(int hand, const Vec3& worldPalm, const Matrix34& controller);
	bool GetHandRecoil(int hand, const Matrix34& controller, Matrix34& delta);
	bool GetPropHandTarget(int hand, Matrix34& target);
	IPhysicalEntity* GetHeldPropPhysics(int hand) const;
	void LimitNPCGripMovement(Vec3& velocity);
	bool HasPhysicalGrip(int hand) const;
	bool CanRecoverHand(int hand) const;
	bool GetGripTrackingRotation(int hand, Quat& rotation, Vec3* angularVelocity = NULL) const;
	bool GetGripTrackingPose(int hand, Matrix34& pose) const;
	bool IsActive() const { return m_active; }
private:
	struct Item;
	struct Magazine;
	struct Hand
	{
		Hand() : item(NULL), magazine(NULL), gripDown(false), triggerDown(false), tracked(false), previousValid(false), velocity(0,0,0), angularVelocity(0,0,0), palmValid(false), palmOffset(0,0,0), propID(-1), propPart(-1), propAnchor(0,0,0), propHandAnchor(0,0,0), propGripRotation(1,0,0,0), nextPropScan(0), gripConstraint(0), gripHandID(-1), gripHandRotation(1,0,0,0), gripHandAnchor(0,0,0), gripHandOffset(0,0,0), gripObjectRotation(1,0,0,0), nextGripTrace(0), gripTraceCount(0) {}
		Item* item;
		Magazine* magazine;
		bool gripDown, triggerDown, tracked, previousValid;
		Matrix34 pose;
		Vec3 previousPosition, previousCameraOrigin, velocity, angularVelocity;
		Quat previousRotation;
		bool palmValid;
		Vec3 palmOffset;
		int propID, propPart;
		Vec3 propAnchor, propHandAnchor;
		Quat propGripRotation;
		float nextPropScan;
		int gripConstraint, gripHandID;
		int gripAngularConstraint = 0;
		Quat gripHandRotation;
		Vec3 gripHandAnchor;
		Vec3 gripHandOffset;
		Quat gripObjectRotation;
		float nextGripTrace;
		int gripTraceCount;
		bool nearWeapon = false;
		float nextProximityScan = 0;
		float shotHapticUntil = 0;
	};
	void SyncInventory();
	void UpdateBody();
	Matrix34 HolsterPose(int slot) const;
	void Grab(int hand, bool scanWorld = true);
	void Release(int hand, bool trackingLost = false);
	void Fire(int hand, bool pressed);
	void UpdateProximityHaptics();
	void ReturnToHolster(Item& item);
	void DestroyBody(Item& item);
	bool DropBody(Item& item, const Vec3& velocity, const Vec3& angularVelocity);
	void SaveMagazine(Item& item);
	void InitializeMagazine(Item& item, bool external);
	void PrepareCharacter(Item& item);
	bool GrabSlide(int hand);
	void UpdateSlidePose(Item& item);
	void UpdateMelee(Item& item);
	bool HeldModel(int hand, const Matrix34& grip, Matrix34& model);
	void GrabProp(int hand);
	void UpdateProp(int hand);
	bool CreatePhysicalGrip(int hand);
	void RemovePhysicalGrip(int hand);
	bool IsOwnBody(IPhysicalEntity* body) const;
	bool GrabMagazine(int hand);
	void ReleaseMagazine(int hand);
	void UpdateMagazine(int hand);
	bool MagazineModel(int hand, const Matrix34& grip, Matrix34& model);
	void AddAmmo(Item& item);
	CWeaponClass* Weapon(const Item& item) const;
	CPlayer& m_player;
	std::vector<Item*> m_items;
	Hand m_hands[2];
	Matrix34 m_body;
	bool m_active;
	float m_nextInventorySync;
};
#endif
