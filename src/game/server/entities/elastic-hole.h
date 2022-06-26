/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_ELASTIC_HOLE_H
#define GAME_SERVER_ENTITIES_ELASTIC_HOLE_H

#include <game/server/entity.h>
#include <base/math.h>
#include <base/tl/array.h>

class CElasticHole : public CEntity
{
public:
	enum
	{
		NUM_PARTICLES = 24,
		NUM_IDS =24,
	};
	enum
	{
		GROW_GROWING=0,
		GROW_STOPING,
		GROW_ZOOMING,
	};
public:
	CElasticHole(CGameWorld *pGameWorld, vec2 CenterPos, int OwnerClientID);
	virtual ~CElasticHole();
	virtual void Tick();
	virtual void Reset();
	virtual void Snap(int SnappingClient);
	int GetTick() { return m_LifeSpan; }

private:
	int m_ParticleIDs[NUM_PARTICLES];
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
	int m_Growing;
};

#endif


