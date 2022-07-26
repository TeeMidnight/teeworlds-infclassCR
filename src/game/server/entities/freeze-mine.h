/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_FREEZE_MINE_H
#define GAME_SERVER_ENTITIES_FREEZE_MINE_H

#include <game/server/entity.h>

class CFreezeMine : public CEntity
{
public:
	CFreezeMine(CGameWorld *pGameWorld, vec2 Pos, int Owner, float Radius = 96.0f);
	virtual ~CFreezeMine();
	
	virtual void Reset();
	virtual void Tick();
	virtual void Snap(int SnappingClient);
	void Explode();

	int GetOwner() const;

private:
	int m_StartTick;
	int m_Radius;
	vec2 m_ActualPos;
	int m_Owner;
	
	array<int> m_IDs;

	int m_LifeSpan;

};

#endif
