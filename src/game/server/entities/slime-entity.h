/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_SLIME_ENTITY_H
#define GAME_SERVER_ENTITIES_SLIME_ENTITY_H

#include <game/server/entity.h>

class CSlimeEntity : public CEntity
{
public:
	int m_Owner;
	
public:
	CSlimeEntity(CGameWorld *pGameWorld, int Owner, vec2 Pos, vec2 Dir);

	vec2 GetPos(float Time);
	void FillInfo(CNetObj_Laser *pProj);

	virtual void Reset();
	virtual void Tick();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);
	void Explode();
	void Collision();
	vec2 m_ActualPos;
	int m_StartTick;

private:
	vec2 m_LastPos;
	vec2 m_ActualDir;
	vec2 m_Direction;
	bool m_IsFlashGrenade;
};

#endif