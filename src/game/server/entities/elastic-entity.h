/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_ELASTIC_ENTITY_H
#define GAME_SERVER_ENTITIES_ELASTIC_ENTITY_H

#include <game/server/entity.h>
#include <base/math.h>
#include <base/tl/array.h>

class CElasticEntity : public CEntity
{
public:
	enum
	{
		NUM_PARTICLES = 4,
		NUM_IDS = 8,
	};
public:
	CElasticEntity(CGameWorld *pGameWorld, vec2 CenterPos, vec2 Dir, int OwnerClientID);
	virtual ~CElasticEntity();
	virtual void Tick();
	virtual void Reset();
	virtual void Snap(int SnappingClient);
	int GetTick() { return m_LifeSpan; }

	int GetOwner() { return m_Owner; }

private:

	int m_ParticleIDs[NUM_PARTICLES];
	array<int> m_IDs;
	void Explode();
	void Collision();
	void TickPaused();
	vec2 GetPos(float Time);

public:
	
	vec2 m_ActualPos;
	vec2 m_LastPos;
	vec2 m_ActualDir;
	int m_StartTick;
	float m_DetectionRadius;
	vec2 m_Direction;
	int m_CollisionNum;

	int m_Owner;
	int m_Damage;
	int m_LifeSpan;
	int m_StartGrowingTick;
	float m_Radius;
};

#endif