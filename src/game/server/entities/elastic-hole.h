/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_ELASTIC_HOLE_H
#define GAME_SERVER_ENTITIES_ELASTIC_HOLE_H

#include <game/server/entity.h>
#include <base/math.h>
#include <engine/shared/config.h>
#include <base/tl/array.h>

class CElasticHole : public CEntity
{
public:
	enum
	{
		NUM_PARTICLES = 14,
		NUM_AMMO = 5,
		NUM_IDS = 16,
	};
	enum
	{
		GROW_GROWING=0,
		GROW_STOPING,
		GROW_ZOOMING,
	};
public:
	CElasticHole(CGameWorld *pGameWorld, vec2 CenterPos, int OwnerClientID, bool IsExplode,float MaxRadius = g_Config.m_InfElasticHoleRadius);
	virtual ~CElasticHole();
	virtual void Tick();
	virtual void Reset();
	virtual void Snap(int SnappingClient);
	int GetTick() { return m_LifeSpan; }

	int GetOwner(){ return m_Owner; }

private:
	int m_ParticleIDs[NUM_PARTICLES];
	int m_AmmoIDs[NUM_AMMO];
	array<int> m_IDs;
	void Explode();

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
	bool m_IsExplode;
};

#endif


