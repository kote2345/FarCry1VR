#ifndef CRYGAME_VR_PHYSICAL_NPC_H
#define CRYGAME_VR_PHYSICAL_NPC_H

// Stage one keeps the proven host-bound articulation. External loads yield
// the living root; later balance/stepping controllers can share this state.
class CVRPhysicalNPCController
{
public:
	explicit CVRPhysicalNPCController(ISystem* system);
	void Update(IEntity* entity, ICryCharInstance* character, float dt, bool enabled);
	void Reset(IEntity* entity);
private:
	enum State { Normal, Interaction, Recovery };
	ISystem* m_system;
	State m_state;
	float m_recoveryRemaining, m_nextTrace;
	Vec3 m_startPosition;
};
#endif
