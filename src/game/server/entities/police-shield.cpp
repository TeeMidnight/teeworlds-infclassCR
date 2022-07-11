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
	m_ExplodeTick = 0;
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
	if(!GameServer()->GetPlayerChar(m_Owner) || GameServer()->GetPlayerChar(m_Owner)->IsZombie())
	{
		GameServer()->m_World.DestroyEntity(this);
	}else
	{
		m_OwnerChrCore = GameServer()->GetPlayerChar(m_Owner)->GetCore();
	}

	if(m_ExplodeTick)
		m_ExplodeTick--;

    m_Pos = m_OwnerChrCore.m_Pos;
    int Angle = int(m_OwnerChrCore.m_Angle / 4.5);

    for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;

        for(int i=0;i < CPoliceShield::NUM_IDS;i++)
	    {
		    float Len = distance(pChr->m_Pos, m_SnapIDsPos[i]);

		    if(Len < pChr->m_ProximityRadius + 2)
		    {
				if(GameServer()->GetPlayerChar(m_Owner))
					switch (GameServer()->GetPlayerChar(m_Owner)->m_ShieldExplode)
					{
						case 0:
							if(abs(m_OwnerChrCore.m_Vel.x) > 4)
							{
								pChr->SetVel(vec2(m_OwnerChrCore.m_Vel.x*1.5f, pChr->GetCore().m_Vel.y));
							}

							if(abs(m_OwnerChrCore.m_Vel.y) > 4)
							{
								pChr->SetVel(vec2(pChr->GetCore().m_Vel.x, m_OwnerChrCore.m_Vel.y*1.5f));
							}
							break;
						case 1:
							if((abs(m_OwnerChrCore.m_Vel.x) > 8 || abs(m_OwnerChrCore.m_Vel.y) > 8) && !m_ExplodeTick)
								GameServer()->CreateExplosionDisk(m_SnapIDsPos[i], 32, 48, g_Config.m_InfPoliceShieldDamage, 32.0f, m_Owner, WEAPON_HAMMER, TAKEDAMAGEMODE_NOINFECTION);
								m_ExplodeTick = g_Config.m_InfPoliceShieldExplodeTime;
							break;
					}
				
			}
        }
	}

    for(CSlimeEntity *pSlime = (CSlimeEntity*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_SLIME_ENTITY); pSlime; pSlime = (CSlimeEntity *)pSlime->TypeNext())
	{
        for(int i=0;i < CPoliceShield::NUM_IDS;i++)
	    {
		    float Len = distance(pSlime->m_ActualPos, m_SnapIDsPos[i]);

		    if(Len < pSlime->m_ProximityRadius + 16)
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

	int Degres = int((m_OwnerChrCore.m_Angle + 200) / 4.5);

	for(int i=0;i < CPoliceShield::NUM_IDS;i++)
	{
		vec2 StartPos = m_Pos + (GetDir(Degres*pi/180) * m_Radius);
		Degres -= 90 / NUM_IDS;
		vec2 EndPos = m_Pos + (GetDir(Degres*pi/180) * m_Radius);
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_IDs[i], sizeof(CNetObj_Laser)));
		if(!pObj)
			return;

		pObj->m_FromX = (int)StartPos.x;
		pObj->m_FromY = (int)StartPos.y;
		pObj->m_X = (int)EndPos.x;
		pObj->m_Y = (int)EndPos.y;
		pObj->m_StartTick = Server()->Tick();

        m_SnapIDsPos[i] = vec2(pObj->m_FromX, pObj->m_FromY);
	}
}
