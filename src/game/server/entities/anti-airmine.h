/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_ANTI_AIRMINE_H
#define GAME_SERVER_ENTITIES_ANTI_AIRMINE_H

#include <game/server/entity.h>

class CAntiAirMine : public CEntity
{

public:
    enum
    {
        NUM_LASERS = 6,
        NUM_AMMOS = NUM_LASERS,
        NUM_IDS = NUM_LASERS + NUM_AMMOS
    };

public:

	CAntiAirMine(CGameWorld *pGameWorld, vec2 Pos, int Owner);
	virtual ~CAntiAirMine();

	virtual void Snap(int SnappingClient);
	virtual void Reset();
	virtual void TickPaused();
	virtual void Tick();

	int GetOwner() const;
	void Explode();
    void MovePlayer();

    int m_AttackNow;

private:
    vec2 m_LaserPos[NUM_LASERS];
    int m_StartTick;
    int m_LifeSpan;
    int m_Owner;
    int m_Angle;
    int m_LaserIDs[NUM_LASERS];
    int m_AmmoIDs[NUM_AMMOS];

};

#endif