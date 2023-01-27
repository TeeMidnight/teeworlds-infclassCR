/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "slime-entity.h"
#include "slug-slime.h"
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/vmath.h>

#include "growingexplosion.h"

CSlimeEntity::CSlimeEntity(CGameWorld *pGameWorld, int Owner, vec2 Pos, vec2 Dir)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_SLIME_ENTITY)
{
	m_Pos = Pos;
	m_ActualPos = Pos;
	m_ActualDir = Dir;
	m_Direction = Dir;
	m_Owner = Owner;
	m_StartTick = Server()->Tick();

	GameWorld()->InsertEntity(this);
}

void CSlimeEntity::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

vec2 CSlimeEntity::GetPos(float Time)
{
	float Curvature = 3.25f;
	float Speed = 2000.0f;

	return CalcPos(m_Pos, m_Direction, Curvature, Speed, Time);
}

void CSlimeEntity::TickPaused()
{
	m_StartTick++;
}

void CSlimeEntity::Tick()
{
	float Pt = (Server()->Tick()-m_StartTick-1)/(float)Server()->TickSpeed();
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	vec2 PrevPos = GetPos(Pt);
	vec2 CurPos = GetPos(Ct);
	
	m_ActualPos = CurPos;
	m_ActualDir = normalize(CurPos - PrevPos);

	
	if(GameLayerClipped(CurPos))
	{
		GameServer()->m_World.DestroyEntity(this);
		return;
	}
	
	
	int Collide = GameServer()->Collision()->IntersectLine(PrevPos, CurPos, NULL, &m_LastPos);
	if(Collide)
	{
		Explode();
	}
	
}

void CSlimeEntity::Collision()
{
	float Pt = (Server()->Tick()-m_StartTick-1)/(float)Server()->TickSpeed();
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	vec2 PrevPos = GetPos(Pt);
	vec2 CurPos = GetPos(Ct);

	//Thanks to TeeBall 0.6
	vec2 CollisionPos;
	CollisionPos.x = m_LastPos.x;
	CollisionPos.y = CurPos.y;
	int CollideY = GameServer()->Collision()->IntersectLine(PrevPos, CollisionPos, NULL, NULL);
	CollisionPos.x = CurPos.x;
	CollisionPos.y = m_LastPos.y;
	int CollideX = GameServer()->Collision()->IntersectLine(PrevPos, CollisionPos, NULL, NULL);
	
	m_Pos = m_LastPos;
	m_ActualPos = m_Pos;
	vec2 vel;
	vel.x = m_Direction.x;
	vel.y = m_Direction.y + 2*3.25f/10000*Ct*1500.0f;
	
	if (CollideX && !CollideY)
	{
		m_Direction.x = -vel.x;
		m_Direction.y = vel.y;
	}
	else if (!CollideX && CollideY)
	{
		m_Direction.x = vel.x;
		m_Direction.y = -vel.y;
	}
	else
	{
		m_Direction.x = -vel.x;
		m_Direction.y = -vel.y;
	}
	
	m_Direction.x *= (100 - 50) / 100.0;
	m_Direction.y *= (100 - 50) / 100.0;
	m_StartTick = Server()->Tick();
	
	m_ActualDir = normalize(m_Direction);
}

void CSlimeEntity::Snap(int SnappingClient)
{
	
	if(IsDontSnapEntity(SnappingClient, m_ActualPos))
		return;
	
	CNetObj_Pickup *pObj = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, m_ID, sizeof(CNetObj_Pickup)));
    if(pObj)
	{
		float t = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
		vec2 Pos(GetPos(t));
		pObj->m_Type = POWERUP_HEALTH;
		pObj->m_Subtype = 0;
		pObj->m_X = Pos.x;
		pObj->m_Y = Pos.y;
	}
}
	
void CSlimeEntity::Explode()
{
	float t = (Server()->Tick()-m_StartTick-1.5)/(float)Server()->TickSpeed();
	new CSlugSlime(GameWorld(), GetPos(t), m_Owner);

	GameServer()->SendHitSound(m_Owner);
	
	GameServer()->m_World.DestroyEntity(this);
	
}
