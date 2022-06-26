/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_OGISGRE_H
#define GAME_SERVER_ENTITIES_OGISGRE_H

#include <game/server/entity.h>

class CSciogistGrenade : public CEntity
{
public:
	CSciogistGrenade(CGameWorld *pGameWorld, int Owner, vec2 Pos, vec2 Dir);

	virtual void Reset();
	virtual void Tick();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);
	void Explode();	
	
protected:
	bool HitCharacter(vec2 From, vec2 To);
	void CreateElasticHole(vec2 CenterPos);
	vec2 GetPos(float Time);
private:
	vec2 m_From;
	vec2 m_Direction;
	vec2 m_ActualDir;
	int m_StartTick;
	int m_Owner;
	vec2 m_ActualPos;

	CCharacter *m_OwnerChar;
};

#endif
