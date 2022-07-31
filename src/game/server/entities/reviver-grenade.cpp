/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/math.h>
#include <base/vmath.h>
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include <game/server/entities/growingexplosion.h>
#include "growingexplosion.h"
#include "reviver-grenade.h"

CReviverGrenade::CReviverGrenade(CGameWorld *pGameWorld, int Owner, vec2 Pos, vec2 Dir)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_REVIVER_GRENADE)
{
	m_Pos = Pos;
	m_ActualPos = Pos;
	m_ActualDir = Dir;
	m_Direction = Dir;
	m_Owner = Owner;
	m_Angle = 0;
	m_LifeSpan =  g_Config.m_InfReviverGrenadeLifeSpan * Server()->TickSpeed();
	m_StartTick = Server()->Tick();

	GameWorld()->InsertEntity(this);
	for(int i=0; i< NUM_AMMO; i++)
	{
		m_IDs[i] = Server()->SnapNewID();
	}
}

CReviverGrenade::~CReviverGrenade()
{
	for(int i=0; i< NUM_AMMO; i++)
	{
		Server()->SnapFreeID(m_IDs[i]);
	}
}

void CReviverGrenade::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

vec2 CReviverGrenade::GetPos(float Time)
{
	float Curvature = GameServer()->Tuning()->m_GrenadeCurvature;
	float Speed = GameServer()->Tuning()->m_GrenadeSpeed;

	return CalcPos(m_Pos, m_Direction, Curvature, Speed, Time);
}

void CReviverGrenade::TickPaused()
{
	m_StartTick++;
}

void CReviverGrenade::Tick()
{
	if(!GameServer()->GetPlayerChar(m_Owner) || GameServer()->GetPlayerChar(m_Owner)->IsZombie())
	{
		GameServer()->m_World.DestroyEntity(this);
	}
	float Pt = (Server()->Tick()-m_StartTick-1)/(float)Server()->TickSpeed();
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	vec2 PrevPos = GetPos(Pt);
	vec2 CurPos = GetPos(Ct);
	
	m_ActualPos = CurPos;
	m_ActualDir = normalize(CurPos - PrevPos);
	
	m_LifeSpan--;
	m_Angle += 4;

	for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;
		float Len = distance(pChr->m_Pos, m_ActualPos);
		if(Len < pChr->m_ProximityRadius)
		{
			Explode(m_ActualPos);
		}
	}

	
	if(GameLayerClipped(CurPos))
	{
		GameServer()->m_World.DestroyEntity(this);
		return;
	}
	
	vec2 LastPos;
	int Collide = GameServer()->Collision()->IntersectLine(PrevPos, CurPos, NULL, &LastPos);
	if(Collide || m_LifeSpan <= 0)
	{
		Explode(LastPos);
	}
	
}

void CReviverGrenade::FillInfo(CNetObj_Projectile *pProj)
{
	pProj->m_X = (int)m_Pos.x;
	pProj->m_Y = (int)m_Pos.y;
	pProj->m_VelX = (int)(m_Direction.x*100.0f);
	pProj->m_VelY = (int)(m_Direction.y*100.0f);
	pProj->m_StartTick = m_StartTick;
	pProj->m_Type = WEAPON_GRENADE;
}

void CReviverGrenade::Snap(int SnappingClient)
{
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	
	if(IsDontSnapEntity(SnappingClient, GetPos(Ct)))
		return;
	
	CNetObj_Projectile *pProj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_ID, sizeof(CNetObj_Projectile)));
	if(pProj)
		FillInfo(pProj);

	float Radius = 16.0f;
	int Degres = m_Angle;

	for(int i=0;i < NUM_AMMO;i++)
	{
		vec2 Pos = GetPos(Ct) + (GetDir(Degres*pi/180) * Radius);
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_IDs[i], sizeof(CNetObj_Laser)));
		if(pObj)
		{
			pObj->m_FromX = (int)Pos.x;
			pObj->m_X = (int)Pos.x;
			pObj->m_FromY = (int)Pos.y;
			pObj->m_Y = (int)Pos.y;
			pObj->m_StartTick = Server()->Tick();
		}
		Degres += 360 / NUM_AMMO;
	}
}
	
void CReviverGrenade::Explode(vec2 Pos)
{
	float Radius = 5 * 16.0f;
	new CGrowingExplosion(GameWorld(), Pos, vec2(0.0, -1.0), m_Owner, Radius / 32 * 3, GROWINGEXPLOSIONEFFECT_LOVE_INFECTED);
	GameServer()->CreateExplosionDisk(Pos, Radius, Radius, 0, -1.0f, m_Owner, WEAPON_GRENADE, TAKEDAMAGEMODE_NOINFECTION);
	GameServer()->CreateSound(Pos, SOUND_GRENADE_EXPLODE);

	for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;
		float Len = distance(pChr->m_Pos, Pos);
		if(Len < pChr->m_ProximityRadius + Radius)
		{
			pChr->SlowMotionEffect(g_Config.m_InfReviverGrenadeSlowTime * 10);
		}
	}

	GameServer()->m_World.DestroyEntity(this);
}
