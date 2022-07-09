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
	float Curvature = GameServer()->Tuning()->m_GrenadeCurvature;
	float Speed = 1500.0f;

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

void CSlimeEntity::FillInfo(CNetObj_Laser *pProj)
{
	pProj->m_X = (int)m_ActualPos.x;
	pProj->m_Y = (int)m_ActualPos.y;
	pProj->m_FromX = (int)m_ActualPos.x;
	pProj->m_FromY = (int)m_ActualPos.y;
	pProj->m_StartTick = Server()->Tick();
}

void CSlimeEntity::Snap(int SnappingClient)
{
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	
	if(NetworkClipped(SnappingClient, GetPos(Ct)))
		return;
	
	CNetObj_Laser *pL = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_ID, sizeof(CNetObj_Laser)));
    if(pL)
		FillInfo(pL);
}
	
void CSlimeEntity::Explode()
{
	float t = (Server()->Tick()-m_StartTick-1.5)/(float)Server()->TickSpeed();
	new CSlugSlime(GameWorld(), GetPos(t), m_Owner);

	GameServer()->SendHitSound(m_Owner);
	
	GameServer()->m_World.DestroyEntity(this);
	
}
