/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/vmath.h>

#include "heal-boom.h"
#include "growingexplosion.h"
#include <engine/server/roundstatistics.h>

CHealBoom::CHealBoom(CGameWorld *pGameWorld, vec2 CenterPos, int OwnerClientID)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_HEAL_BOOM)
{
	m_Pos = CenterPos;
	GameWorld()->InsertEntity(this);
	m_DetectionRadius = 60.0f;
	m_StartTick = Server()->Tick();
	m_Owner = OwnerClientID;
	m_Damage = 0;
	m_MaxRadius = g_Config.m_InfHealBoomRadius;
	m_Radius = 0;
	m_LifeSpan = g_Config.m_InfHealBoomLifeSpan * Server()->TickSpeed();
	m_Growing = GROW_GROWING;
	m_StartGrowingTick = Server()->Tick();
	m_HealNum = 0;
	for(int i = 0;i < NUM_LASERS; i ++)
	{
		m_IDs[i] = Server()->SnapNewID();
	}
}

CHealBoom::~CHealBoom()
{
	for(int i = 0;i < NUM_LASERS; i ++)
	{
		Server()->SnapFreeID(m_IDs[i]);
	}
}

void CHealBoom::Tick()
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

	CCharacter *pOwnerChar = GameServer()->GetPlayerChar(m_Owner);

	for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsHuman()) continue;

		float Len = distance(pChr->m_Pos, m_Pos);

		int old_class = pChr->GetPlayer()->GetOldClass();

		auto& reviver = pOwnerChar;
		auto& zombie = pChr;

		if(Len < zombie->m_ProximityRadius + m_Radius && GameServer()->GetZombieCount() > 5)
		{
			zombie->GetPlayer()->SetClass(old_class);
			if (zombie->GetPlayer()->GetCharacter()) {
				zombie->GetPlayer()->GetCharacter()->SetHealthArmor(1, 0);
				zombie->Unfreeze();
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Reviver {str:ReviverName}'s heal boom revived {str:PlayerName}"),
						"ReviverName", Server()->ClientName(reviver->GetPlayer()->GetCID()),
						"PlayerName", Server()->ClientName(zombie->GetPlayer()->GetCID()));
				int ClientID = reviver->GetPlayer()->GetCID();
				Server()->RoundStatistics()->OnScoreEvent(ClientID, SCOREEVENT_MEDIC_REVIVE, reviver->GetClass(), Server()->ClientName(ClientID), GameServer()->Console());
				m_HealNum++;
			}
		}
		if(m_HealNum == g_Config.m_InfHealBoomMaxHeal || GameServer()->GetZombieCount() < 5 )
		{
			break;
		}
	}
	if(m_Radius < 0 || m_HealNum == g_Config.m_InfHealBoomMaxHeal || GameServer()->GetZombieCount() < 5)
	{
		GameServer()->CreateExplosion(m_Pos, m_Owner, WEAPON_HAMMER, true, TAKEDAMAGEMODE_NOINFECTION);
		GameServer()->CreateSound(m_Pos, SOUND_GRENADE_EXPLODE);
		GameServer()->m_World.DestroyEntity(this);
	}
}

void CHealBoom::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CHealBoom::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	for(int i=0;i < NUM_LASERS;i++)
	{
		float RandomRadius = random_float()*(m_Radius-4.0f);
		float RandomAngle = 2.0f * pi * random_float();
		vec2 LaserPos = m_Pos + vec2(RandomRadius * cos(RandomAngle), RandomRadius * sin(RandomAngle));
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_IDs[i], sizeof(CNetObj_Laser)));
		if(!pObj)
			return;
		pObj->m_FromX = (int)LaserPos.x;
		pObj->m_FromY = (int)LaserPos.y;
		pObj->m_X = (int)LaserPos.x;
		pObj->m_Y = (int)LaserPos.y;
		pObj->m_StartTick = Server()->Tick();
	}
}