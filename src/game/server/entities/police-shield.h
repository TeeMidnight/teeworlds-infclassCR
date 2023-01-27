#ifndef GAME_SERVER_ENTITIES_POLICE_WALL_H
#define GAME_SERVER_ENTITIES_POLICE_WALL_H

#include <game/server/entity.h>

class CPoliceShield : public CEntity
{
public:
	enum
	{
		NUM_IDS = 2,
	};
public:
	CPoliceShield(CGameWorld *pGameWorld, vec2 Pos, vec2 Pos2, int Owner);
	virtual ~CPoliceShield();
	virtual void Reset();
	virtual void Tick();
	virtual void Snap(int SnappingClient);
	int m_Owner;
	vec2 m_Pos;
	vec2 m_Pos2;
	int m_GrenadeID[NUM_IDS];
	int m_EndPointID;
	int m_Health;
};

#endif
