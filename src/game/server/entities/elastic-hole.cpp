/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/vmath.h>

#include "elastic-hole.h"
#include "growingexplosion.h"

CElasticHole::CElasticHole(CGameWorld *pGameWorld, vec2 CenterPos, int OwnerClientID, bool IsExplode, float MaxRadius)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_ELASTIC_HOLE)
{
	m_Pos = CenterPos;
	GameWorld()->InsertEntity(this);
	m_DetectionRadius = 60.0f;
	m_StartTick = Server()->Tick();
	m_Owner = OwnerClientID;
	m_Damage = 0;
	m_MaxRadius = MaxRadius;
	m_IsExplode = IsExplode;
	m_Radius = 0;
	m_LifeSpan = g_Config.m_InfElasticHoleLifeSpan * Server()->TickSpeed();
	m_Growing = GROW_GROWING;
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
	for(int i=0; i<NUM_AMMO; i++)
	{
		m_AmmoIDs[i] = Server()->SnapNewID();
	}
}

CElasticHole::~CElasticHole()
{
	for(int i=0; i<NUM_PARTICLES; i++)
	{
		Server()->SnapFreeID(m_ParticleIDs[i]);
	}
	for(int i=0; i<NUM_IDS; i++)
	{
		Server()->SnapFreeID(m_IDs[i]);
	}
	for(int i=0; i<NUM_AMMO; i++)
	{
		Server()->SnapFreeID(m_AmmoIDs[i]);
	}
}

void CElasticHole::Explode()
{
	new CGrowingExplosion(GameWorld(), m_Pos, vec2(0.0, -1.0), m_Owner, m_MaxRadius/32*7, GROWINGEXPLOSIONEFFECT_BOOM_ALL);
	GameServer()->CreateSound(m_Pos, SOUND_GRENADE_EXPLODE);

	GameServer()->m_World.DestroyEntity(this);
}

void CElasticHole::Tick()
{
	if(!GameServer()->GetPlayerChar(m_Owner) || GameServer()->GetPlayerChar(m_Owner)->IsZombie())
	{
		GameServer()->m_World.DestroyEntity(this);
	}

	if(m_Radius > m_MaxRadius)
	{
		m_Growing = GROW_STOPING;
		m_Radius = m_MaxRadius;
	}
	else if(m_Growing == GROW_GROWING)
		m_Radius += 2;
	else if(m_Growing == GROW_ZOOMING)
		m_Radius -= 2;
	
	if(m_LifeSpan <= 0)
	{
		m_Growing = GROW_ZOOMING;
	}else
	{
		m_LifeSpan--;
	}
	for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;
		float Len = distance(pChr->m_Pos, m_Pos);
		if(Len < pChr->m_ProximityRadius+m_Radius)
		{
			vec2 Vel = pChr->GetVel();
			pChr->SetVel(vec2(Vel.x*-1.25, Vel.y*-1.25));
		}
	}
	bool boom = false;
	if(m_Radius < 0)
	{
		if(!boom && m_IsExplode)
		{
			Explode();
			boom = true;
		}
		else
		{
			GameServer()->CreateExplosion(m_Pos, m_Owner, WEAPON_HAMMER, true, TAKEDAMAGEMODE_NOINFECTION);
			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_EXPLODE);
			GameServer()->m_World.DestroyEntity(this);
		}
	}
}

void CElasticHole::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CElasticHole::Snap(int SnappingClient)
{

	if(NetworkClipped(SnappingClient, m_Pos))
		return;

	int Degres = 0;

	for(int i=0;i < CElasticHole::NUM_IDS;i++)
	{
		vec2 StartPos = m_Pos + (GetDir(Degres*pi/180) * m_Radius);
		Degres += 360 / NUM_IDS;
		vec2 EndPos = m_Pos + (GetDir(Degres*pi/180) * m_Radius);
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_IDs[i], sizeof(CNetObj_Laser)));
		if(!pObj)
			return;

		pObj->m_FromX = (int)StartPos.x;
		pObj->m_FromY = (int)StartPos.y;
		pObj->m_X = (int)EndPos.x;
		pObj->m_Y = (int)EndPos.y;
		pObj->m_StartTick = Server()->Tick();

			
	}
	for(int i=0;i < CElasticHole::NUM_PARTICLES;i++)
	{
		float RandomRadius = random_float()*(m_Radius-4.0f);
		float RandomAngle = 2.0f * pi * random_float();
		vec2 ParticlePos = m_Pos + vec2(RandomRadius * cos(RandomAngle), RandomRadius * sin(RandomAngle));

		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_ParticleIDs[i], sizeof(CNetObj_Projectile)));
		if(pObj)
		{
			pObj->m_X = (int)ParticlePos.x;
			pObj->m_Y = (int)ParticlePos.y;
			pObj->m_VelX = 0;
			pObj->m_VelY = 0;
			pObj->m_StartTick = Server()->Tick();
			pObj->m_Type = WEAPON_HAMMER;
		}
	}

	for(int i = 0 ; i < 2; i ++)
	{
		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_AmmoIDs[i], sizeof(CNetObj_Projectile)));
		if(pObj)
		{
			pObj->m_X = (int)m_Pos.x + m_Radius * (i%2 ? 1 : -1);
			pObj->m_Y = (int)m_Pos.y;
			pObj->m_VelX = 0;
			pObj->m_VelY = 0;
			pObj->m_StartTick = Server()->Tick();
			pObj->m_Type = WEAPON_GRENADE;
		}
	}

	for(int i = 0 ; i < 2; i ++)
	{
		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_AmmoIDs[i+2], sizeof(CNetObj_Projectile)));
		if(pObj)
		{
			pObj->m_X = (int)m_Pos.x;
			pObj->m_Y = (int)m_Pos.y + m_Radius * (i%2 ? 1 : -1);
			pObj->m_VelX = 0;
			pObj->m_VelY = 0;
			pObj->m_StartTick = Server()->Tick();
			pObj->m_Type = WEAPON_GRENADE;
		}
	}

	CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_AmmoIDs[4], sizeof(CNetObj_Projectile)));
	if(pObj)
	{
		pObj->m_X = (int)m_Pos.x;
		pObj->m_Y = (int)m_Pos.y;
		pObj->m_VelX = 0;
		pObj->m_VelY = 0;
		pObj->m_StartTick = Server()->Tick();
		pObj->m_Type = WEAPON_GRENADE;
	}
}