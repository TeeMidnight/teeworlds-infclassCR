/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/vmath.h>

#include "elastic-entity.h"
#include "elastic-hole.h"

CElasticEntity::CElasticEntity(CGameWorld *pGameWorld, vec2 CenterPos, vec2 Dir,int OwnerClientID)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_ELASTIC_ENTITY)
{
	m_Pos = CenterPos;
	GameWorld()->InsertEntity(this);
	m_DetectionRadius = 60.0f;
	m_StartTick = Server()->Tick();
	m_Owner = OwnerClientID;
    m_Direction = Dir;
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
    GameServer()->CreateExplosion(m_Pos, m_Owner, WEAPON_HAMMER, false, TAKEDAMAGEMODE_NOINFECTION);

	new CElasticHole(GameWorld(), m_Pos, m_Owner, false, 46);

	GameServer()->CreateSound(m_Pos, SOUND_GRENADE_EXPLODE);

	GameServer()->m_World.DestroyEntity(this);
}

vec2 CElasticEntity::GetPos(float Time)
{
	float Curvature = 0;
	float Speed = 0;

	Curvature = 1.25f;
	Speed = 64.0f;

	return CalcPos(m_Pos, m_Direction, Curvature, Speed, Time);
}

void CElasticEntity::TickPaused()
{
	m_StartTick++;
}



void CElasticEntity::Tick()
{
    float Pt = (Server()->Tick()-m_StartTick-1)/(float)Server()->TickSpeed();
	float Ct = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	vec2 PrevPos = GetPos(Pt);
	vec2 CurPos = GetPos(Ct);

	m_Pos = CurPos;

	if(GameLayerClipped(CurPos))
	{
		GameServer()->m_World.DestroyEntity(this);
		return;
	}

	if(m_LifeSpan <= 0)
	{
		Explode();
	}else
	{
		m_LifeSpan--;
	}

   
	int Collide = GameServer()->Collision()->IntersectLine(PrevPos, CurPos, &CurPos, 0);

    if(Collide)
        Explode();
}

void CElasticEntity::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CElasticEntity::Snap(int SnappingClient)
{

	if(NetworkClipped(SnappingClient))
		return;

	int Degres = 0;

	for(int i=0;i < CElasticEntity::NUM_IDS;i++)
	{
		CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, m_IDs[i], sizeof(CNetObj_Pickup)));
		if(!pP)
			return;

		pP->m_X = (int)m_Pos.x + (GetDir(Degres*pi/180) * m_Radius).x;;
		pP->m_Y = (int)m_Pos.y + (GetDir(Degres*pi/180) * m_Radius).y;

		pP->m_Type = POWERUP_ARMOR;
		pP->m_Subtype = 0;

		Degres += 360 / NUM_IDS;
	}
	for(int i=0;i < CElasticEntity::NUM_PARTICLES;i++)
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
}