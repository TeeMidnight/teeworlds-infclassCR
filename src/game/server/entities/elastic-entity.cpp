/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/vmath.h>

#include "elastic-entity.h"
#include "elastic-hole.h"
#include "laser.h"
#include "growingexplosion.h"

CElasticEntity::CElasticEntity(CGameWorld *pGameWorld, vec2 CenterPos, vec2 Dir,int OwnerClientID)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_ELASTIC_ENTITY)
{
	m_Pos = CenterPos;
	m_ActualPos = CenterPos;
	GameWorld()->InsertEntity(this);
	m_DetectionRadius = 60.0f;
	m_StartTick = Server()->Tick();
	m_Owner = OwnerClientID;
    m_Direction = Dir;
	m_ActualDir = Dir;
	m_Damage = 3;
    m_Radius = g_Config.m_InfElasticEntityRadius;
	m_LifeSpan = g_Config.m_InfElasticEntityLifeSpan * Server()->TickSpeed();
	m_StartGrowingTick = Server()->Tick();
	
	
	m_IDs.set_size(NUM_IDS);
	for(int i=0; i<NUM_IDS; i++)
	{
		m_IDs[i] = Server()->SnapNewID();
	}
	for(int i=0; i<NUM_PARTICLES; i++)
	{
		m_ParticleIDs[i] = Server()->SnapNewID();
	}
}

CElasticEntity::~CElasticEntity()
{
	for(int i=0; i<NUM_PARTICLES; i++)
	{
		Server()->SnapFreeID(m_ParticleIDs[i]);
	}
	for(int i=0; i<NUM_IDS; i++)
	{
		Server()->SnapFreeID(m_IDs[i]);
	}
}

void CElasticEntity::Explode()
{
    GameServer()->CreateExplosion(m_Pos, m_Owner, WEAPON_HAMMER, true, TAKEDAMAGEMODE_NOINFECTION);

	int Degres = 0;
	for(int i=0;i < CElasticEntity::NUM_IDS;i++)
	{
		vec2 DirPos = m_ActualPos + (GetDir(Degres*pi/180) * 2);
		vec2 Dir = normalize(DirPos - m_ActualPos);
		vec2 StartPos = DirPos + Dir*-3.0f;
		new CLaser(GameWorld(), StartPos, Dir, 200.0f, m_Owner, 8);
		Degres += 360 / NUM_IDS;
	}
	
	GameServer()->CreateSound(m_LastPos, SOUND_GRENADE_EXPLODE);

	GameServer()->m_World.DestroyEntity(this);
}

vec2 CElasticEntity::GetPos(float Time)
{
	float Curvature = 0;
	float Speed = 0;

	Curvature = 3.25f;
	Speed = 1500.0f;

	return CalcPos(m_Pos, m_Direction, Curvature, Speed, Time);
}

void CElasticEntity::TickPaused()
{
	m_StartTick++;
}

void CElasticEntity::Collision()
{
	GameServer()->CreateExplosion(m_LastPos, m_Owner, WEAPON_HAMMER, false, TAKEDAMAGEMODE_NOINFECTION);

	float Pt = (Server()->Tick()-m_StartTick-1)/(float)Server()->TickSpeed();
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	vec2 PrevPos = GetPos(Pt);
	vec2 CurPos = GetPos(Ct);

	m_CollisionNum++;
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

void CElasticEntity::Tick()
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

	if(GameLayerClipped(CurPos))
	{
		GameServer()->m_World.DestroyEntity(this);
		return;
	}

	for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;
		float Len = distance(pChr->m_Pos, m_ActualPos);
		if(Len < pChr->m_ProximityRadius+m_Radius)
		{
			vec2 Vel = pChr->GetVel();
			pChr->SetVel(vec2((int)(m_ActualDir.x*25.0f), (int)(m_ActualDir.y*25.0f)));
			pChr->TakeDamage(vec2(0.0f,0.0f), g_Config.m_InfElasticEntityDamage, m_Owner, WEAPON_RIFLE, TAKEDAMAGEMODE_NOINFECTION);
			Collision();
		}
	}

	int Collide = GameServer()->Collision()->IntersectLine(PrevPos, CurPos, NULL, &m_LastPos);
	if(Collide)
	{
		Collision();
	}

	if(m_LifeSpan <= 0 || m_CollisionNum >= g_Config.m_InfElasticEntityCollisionNum)
	{
		Explode();
	}else
	{
		m_LifeSpan--;
	}
}

void CElasticEntity::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CElasticEntity::Snap(int SnappingClient)
{

	if(NetworkClipped(SnappingClient, m_ActualPos))
		return;

	int Degres = 0;

	for(int i=0;i < CElasticEntity::NUM_IDS;i++)
	{
		vec2 StartPos = m_ActualPos + (GetDir(Degres*pi/180) * m_Radius);
		Degres += 360 / NUM_IDS;
		vec2 EndPos = m_ActualPos + (GetDir(Degres*pi/180) * m_Radius);
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_IDs[i], sizeof(CNetObj_Laser)));
		if(!pObj)
			return;

		pObj->m_FromX = (int)StartPos.x;
		pObj->m_FromY = (int)StartPos.y;
		pObj->m_X = (int)EndPos.x;
		pObj->m_Y = (int)EndPos.y;
		pObj->m_StartTick = Server()->Tick();
	}
	
	if(Server()->GetClientAntiPing(SnappingClient))
		return;

	for(int i=0;i < CElasticEntity::NUM_PARTICLES;i++)
	{
		float RandomRadius = random_float()*(m_Radius-4.0f);
		float RandomAngle = 2.0f * pi * random_float();
		vec2 ParticlePos = m_ActualPos + vec2(RandomRadius * cos(RandomAngle), RandomRadius * sin(RandomAngle));
			
		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_ParticleIDs[i], sizeof(CNetObj_Projectile)));
		if(pObj)
		{
			pObj->m_X = (int)ParticlePos.x;
			pObj->m_Y = (int)ParticlePos.y;
			pObj->m_VelX = (int)(m_Direction.x*100.0f);
			pObj->m_VelY = (int)(m_Direction.y*100.0f);
			pObj->m_StartTick = Server()->Tick();
			pObj->m_Type = WEAPON_HAMMER;
		}
	}
}