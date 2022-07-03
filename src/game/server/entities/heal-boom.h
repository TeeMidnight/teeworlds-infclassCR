/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_HEAL_BOOM_H
#define GAME_SERVER_ENTITIES_HEAL_BOOM_H

#include <game/server/entity.h>
#include <base/math.h>
#include <engine/shared/config.h>
#include <base/tl/array.h>

class CHealBoom : public CEntity
{
public:
	enum
	{
		NUM_LASERS = 28,
	};
	enum
	{
		GROW_GROWING=0,
		GROW_STOPING,
		GROW_ZOOMING,
	};
public:
	CHealBoom(CGameWorld *pGameWorld, vec2 CenterPos, int OwnerClientID);
	~CHealBoom();
	virtual void Tick();
	virtual void Reset();
	virtual void Snap(int SnappingClient);
	int GetTick() { return m_LifeSpan; }

	int GetOwner(){ return m_Owner; }

public:

	int m_StartTick;
	float m_DetectionRadius;

	int m_Owner;
	int m_Damage;
	int m_LifeSpan;
	int m_StartGrowingTick;
	float m_Radius;
	float m_MaxRadius;
	int m_Growing;
	int m_HealNum;
	int m_IDs[NUM_LASERS];
};

#endif


