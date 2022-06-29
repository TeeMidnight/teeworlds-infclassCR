/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include "police-shield.h"
#include "slime-entity.h"
#include <engine/server/roundstatistics.h>
#include <engine/shared/config.h>

CPoliceShield::CPoliceShield(CGameWorld *pGameWorld, int Owner)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_POLICE_SHIELD)
{
	m_Owner = Owner;
    m_Radius = g_Config.m_InfPoliceShieldRadius;
	GameWorld()->InsertEntity(this);

    m_IDs.set_size(NUM_IDS);

    for(int i=0; i<NUM_IDS; i++)
	{
		m_IDs[i] = Server()->SnapNewID();
        m_SnapIDsPos[i] = vec2(0,0);
	}
}

CPoliceShield::~CPoliceShield()
{
	for(int i=0; i<NUM_IDS; i++)
	{
		Server()->SnapFreeID(m_IDs[i]);
	}
}

void CPoliceShield::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CPoliceShield::Tick()
{
	if(!GameServer()->GetPlayerChar(m_Owner))
	{
		GameServer()->m_World.DestroyEntity(this);
	}else
	{
		m_OwnerChrCore = GameServer()->GetPlayerChar(m_Owner)->GetCore();
	}
    m_Pos = m_OwnerChrCore.m_Pos;
    int Angle = int(m_OwnerChrCore.m_Angle / 4.5);

    for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;

        for(int i=0;i < CPoliceShield::NUM_IDS;i++)
	    {
		    float Len = distance(pChr->m_Pos, m_SnapIDsPos[i]);

		    if(Len < pChr->m_ProximityRadius + 32)
		    {
			    pChr->SetVel(vec2(m_OwnerChrCore.m_Vel.x, m_OwnerChrCore.m_Vel.y));
		    }
        }
	}

    for(CSlimeEntity *pSlime = (CSlimeEntity*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_SLIME_ENTITY); pSlime; pSlime = (CSlimeEntity *)pSlime->TypeNext())
	{
        for(int i=0;i < CPoliceShield::NUM_IDS;i++)
	    {
		    float Len = distance(pSlime->m_ActualPos, m_SnapIDsPos[i]);

		    if(Len < pSlime->m_ProximityRadius + 32)
		    {
			    GameServer()->m_World.DestroyEntity(pSlime);
                GameServer()->CreatePlayerSpawn(m_SnapIDsPos[i]);
                GameServer()->CreateSound(m_SnapIDsPos[i], SOUND_PLAYER_SPAWN);
		    }
        }
	}
}

void CPoliceShield::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	int Degres = int((m_OwnerChrCore.m_Angle + 400) / 4.5);

	for(int i=0;i < CPoliceShield::NUM_IDS;i++)
	{
		CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, m_IDs[i], sizeof(CNetObj_Pickup)));
		if(!pP)
			return;

		pP->m_X = (int)m_Pos.x + (GetDir(Degres*pi/180) * m_Radius).x;
		pP->m_Y = (int)m_Pos.y + (GetDir(Degres*pi/180) * m_Radius).y;

        m_SnapIDsPos[i] = vec2(pP->m_X, pP->m_Y);

		pP->m_Type = POWERUP_ARMOR;
		pP->m_Subtype = 0;

		Degres -= 180 / NUM_IDS;
	}
}
