/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */


#include "sciogist-grenade.h"

#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>


#include "elastic-hole.h"
#include "growingexplosion.h"

CSciogistGrenade::CSciogistGrenade(CGameWorld *pGameWorld, int Owner, vec2 Pos, vec2 Dir)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_SCIOGIST_GRENADE)
{
	m_Pos = Pos;
	m_Owner = Owner;
	m_Direction = Dir;
	m_ActualDir = Dir;
	m_StartTick = Server()->Tick();
	m_OwnerChar = GameServer()->GetPlayerChar(m_Owner);
	GameWorld()->InsertEntity(this);
}

void CSciogistGrenade::Explode()
{
	GameServer()->CreateExplosion(m_ActualPos, m_Owner, WEAPON_GRENADE, false, TAKEDAMAGEMODE_NOINFECTION);
	GameServer()->CreateSound(m_ActualPos, SOUND_GRENADE_EXPLODE);
	if(m_OwnerChar && m_OwnerChar->m_HasElasticHole)
	{
		new CGrowingExplosion(GameWorld(), m_ActualPos, vec2(0.0, -1.0), m_Owner, 5, GROWINGEXPLOSIONEFFECT_BOOM_INFECTED);
		new CElasticHole(GameWorld(), m_ActualPos, m_Owner, true);
		
		//Make it unavailable
		m_OwnerChar->m_HasElasticHole = false;
		m_OwnerChar->m_HasIndicator = false;
		m_OwnerChar->GetPlayer()->ResetNumberKills();
	}
	GameServer()->m_World.DestroyEntity(this);
	
}

void CSciogistGrenade::Tick()
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

	CCharacter *OwnerChar = GameServer()->GetPlayerChar(m_Owner);
	CCharacter *TargetChr = GameServer()->m_World.IntersectCharacter(PrevPos, CurPos, 6.0f, CurPos, OwnerChar);
	vec2 LastPos;
	int Collide = GameServer()->Collision()->IntersectLine(PrevPos, CurPos, NULL, &LastPos);
	if(TargetChr || Collide)
	{
		Explode();
	}
	
	
}

void CSciogistGrenade::TickPaused()
{
	m_StartTick++;
}

vec2 CSciogistGrenade::GetPos(float Time)
{
	float Curvature = 7.0f;
	float Speed = 1000.0f;

	return CalcPos(m_Pos, m_Direction, Curvature, Speed, Time);
}

void CSciogistGrenade::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CSciogistGrenade::Snap(int SnappingClient)
{
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	
	if(NetworkClipped(SnappingClient, GetPos(Ct)))
		return;
	
	CNetObj_Projectile *pProj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_ID, sizeof(CNetObj_Projectile)));
	if(pProj)
	{
		pProj->m_X = (int)m_Pos.x;
		pProj->m_Y = (int)m_Pos.y;
		pProj->m_VelX = (int)(m_Direction.x*100.0f);
		pProj->m_VelY = (int)(m_Direction.y*100.0f);
		pProj->m_StartTick = m_StartTick;
		pProj->m_Type = WEAPON_GRENADE;
	}
}
