/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include "police-shield.h"
#include "slime-entity.h"
#include <engine/server/roundstatistics.h>
#include <engine/shared/config.h>

const float g_BarrierMaxLength = 200.0;
const float g_BarrierRadius = 4.0;

CPoliceShield::CPoliceShield(CGameWorld *pGameWorld, vec2 Pos, vec2 Pos2, int Owner)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_POLICE_SHIELD)
{
	m_Pos = Pos;
	if(distance(Pos, Pos2) > g_BarrierMaxLength)
	{
		m_Pos2 = Pos + normalize(Pos2 - Pos)*g_BarrierMaxLength;
	}
	else m_Pos2 = Pos2;
	m_Owner = Owner;
	m_Health = 50;
	GameWorld()->InsertEntity(this);

	m_EndPointID = Server()->SnapNewID();
    for(int i=0; i<NUM_IDS; i++)
	{
		m_GrenadeID[i] = Server()->SnapNewID();
	}
}

CPoliceShield::~CPoliceShield()
{
	Server()->SnapFreeID(m_EndPointID);
	for(int i=0; i<NUM_IDS; i++)
	{
		Server()->SnapFreeID(m_GrenadeID[i]);
	}
}

void CPoliceShield::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CPoliceShield::Tick()
{
	if(!GameServer()->GetPlayerChar(m_Owner) || GameServer()->GetPlayerChar(m_Owner)->IsZombie())
	{
		GameServer()->m_World.DestroyEntity(this);
		return;
	}

	if(m_Health <= 0)
	{
		GameServer()->SendBroadcast_Localization(m_Owner, BROADCAST_PRIORITY_WEAPONSTATE, BROADCAST_DURATION_GAMEANNOUNCE,
			_("Shield was destroy!"),
			NULL
		);
		GameServer()->m_World.DestroyEntity(this);
		return;
	}

	for(CCharacter *p = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); p; p = (CCharacter *)p->TypeNext())
	{
		if(p->IsHuman()) continue;

		vec2 IntersectPos = closest_point_on_line(m_Pos, m_Pos2, p->m_Pos);
		float Len = distance(p->m_Pos, IntersectPos);
		if(Len < p->m_ProximityRadius+g_BarrierRadius)
		{
			vec2 Vel = normalize(p->m_Pos - IntersectPos);
			p->SetVel(Vel);
		}
	}

    for(CSlimeEntity *pSlime = (CSlimeEntity*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_SLIME_ENTITY); pSlime; pSlime = (CSlimeEntity *)pSlime->TypeNext())
	{
		vec2 IntersectPos = closest_point_on_line(m_Pos, m_Pos2, pSlime->m_Pos);
		float Len = distance(pSlime->m_Pos, IntersectPos);

		if(Len < pSlime->m_ProximityRadius + g_BarrierRadius + 16.0f)
		{
			GameServer()->m_World.DestroyEntity(pSlime);
			GameServer()->CreatePlayerSpawn(pSlime->m_Pos);
		}
	}
}

void CPoliceShield::Snap(int SnappingClient)
{
	if(IsDontSnapEntity(SnappingClient, m_Pos) && IsDontSnapEntity(SnappingClient, m_Pos2))
		return;

	{
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_ID, sizeof(CNetObj_Laser)));
		if(!pObj)
			return;

		pObj->m_X = (int)m_Pos.x;
		pObj->m_Y = (int)m_Pos.y;
		pObj->m_FromX = (int)m_Pos2.x;
		pObj->m_FromY = (int)m_Pos2.y;
		pObj->m_StartTick = Server()->Tick();
	}
	{
		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_GrenadeID[0], sizeof(CNetObj_Projectile)));
		if(!pObj)
			return;

		pObj->m_X = (int)m_Pos.x;
		pObj->m_Y = (int)m_Pos.y;
		pObj->m_Type = WEAPON_GRENADE;
		pObj->m_VelX = 0;
		pObj->m_VelY = 0;
		pObj->m_StartTick = Server()->Tick();
	}
	{
		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_GrenadeID[1], sizeof(CNetObj_Projectile)));
		if(!pObj)
			return;

		pObj->m_X = (int)m_Pos2.x;
		pObj->m_Y = (int)m_Pos2.y;
		pObj->m_Type = WEAPON_GRENADE;
		pObj->m_VelX = 0;
		pObj->m_VelY = 0;
		pObj->m_StartTick = Server()->Tick();
	}
	if(!Server()->GetClientAntiPing(SnappingClient))
	{
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_EndPointID, sizeof(CNetObj_Laser)));
		if(!pObj)
			return;
		
		vec2 Pos = m_Pos2;

		pObj->m_X = (int)Pos.x;
		pObj->m_Y = (int)Pos.y;
		pObj->m_FromX = (int)Pos.x;
		pObj->m_FromY = (int)Pos.y;
		pObj->m_StartTick = Server()->Tick();
	}
}
