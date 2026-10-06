#ifndef CRYGAME_VR_BODY_PHYSICS_H
#define CRYGAME_VR_BODY_PHYSICS_H

// Dynamic rigid segments follow the tracked upper-body pose through bounded
// forces. World contacts and impacts use the engine physics solver; the living
// entity remains responsible for locomotion.
class CVRBodyPhysics
{
public:
	CVRBodyPhysics();
	~CVRBodyPhysics();
	void Resolve(ICryCharInstance* character, Matrix34& world, ISystem* system,
		IEntity* player, float deltaTime, class CVRPhysicalWeapons* interactions = NULL);
	bool GetHandCollisionTarget(int hand, Matrix34& target) const;
	IPhysicalEntity* GetHandPhysics(int hand) const;
	bool GetHandTrackingPose(int hand, Matrix34& target) const;
	void ReleaseHandConstraints(int hand);
	bool GetArmReach(int hand, IEntity* player, Vec3& shoulder, float& length) const;
	bool GetPhysicalWrist(int hand, Vec3& position, Vec3& velocity) const;
private:
	struct Part;
	std::vector<Part*> m_parts;
	bool m_handBlocked[2];
	Vec3 m_handPosition[2];
	bool m_separatedHand[2];
	Vec3 m_recoveredWrist[2];
	Vec3 m_shoulderOffset[2];
	bool m_reachValid[2];
	int m_armConstraints[2][4];
	void UpdateArmConstraints(int hand, bool enabled, const std::vector<Vec3>& joints);
};
#endif
