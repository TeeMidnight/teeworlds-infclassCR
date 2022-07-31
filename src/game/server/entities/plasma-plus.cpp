// Strongly modified version of ddnet Plasma. Source: Shereef Marzouk
#include <engine/server.h>
#include <engine/config.h>
#include <engine/shared/config.h>
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include "plasma-plus.h"

CPlasmaPlus::CPlasmaPlus(CGameWorld *pGameWorld, vec2 Pos, int Owner, vec2 Direction, bool Freeze, bool Explosive)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_PLASMA_PLUS)
{
	m_Owner = Owner;
	m_Pos = Pos;
	m_Speed = g_Config.m_InfPlasmaPlusSpeed;
	m_Freeze = Freeze;
	m_TrackedPlayer = -1;
	m_Dir = Direction;
	m_Explosive = Explosive;
	m_StartTick = Server()->Tick();
	m_LifeSpan = Server()->TickSpeed()*g_Config.m_InfPlasmaPlusLifeSpan;
	m_InitialAmount = 1.0f;
	GameWorld()->InsertEntity(this);
}

int CPlasmaPlus::GetOwner() const
{
	return m_Owner;
}

void CPlasmaPlus::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

void CPlasmaPlus::Tick()
{
	
	//reduce lifespan
	if (m_LifeSpan < 0)
	{
		Explode();
		return;
	}
	m_LifeSpan--;
	
	// tracking, position and collision calculation
	CCharacter *pTarget = GameServer()->GetPlayerChar(m_TrackedPlayer);
	if(!pTarget)
	{
		m_Pos += m_Dir * m_Speed;

		float MinDistance = 2400.0f, MinDistancePlayer = -1;
		for(CCharacter *p = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); p; p = (CCharacter *)p->TypeNext())
		{
			if(p->IsHuman())continue;

			float Len = distance(p->m_Pos, m_Pos);
			if(Len < p->m_ProximityRadius + g_Config.m_InfPlasmaPlusRange)
			{
				if(MinDistance > Len)
				{
					MinDistance = Len;
					MinDistancePlayer = p->GetPlayer()->GetCID();
				}
			}
		}
		if(MinDistancePlayer > -1)
		{
			m_TrackedPlayer = MinDistancePlayer;
		}
	} 
	else
	{
		float Dist = distance(m_Pos, pTarget->m_Pos);
		if(Dist < 24.0f)
		{
			//freeze or explode
			if (m_Freeze) 
			{
				pTarget->Freeze(3.0f, m_Owner, FREEZEREASON_FLASH);
			}
			
			Explode();
		}
		else
		{
			
			m_Dir = normalize(pTarget->m_Pos - m_Pos);
			m_Pos += m_Dir*m_Speed;
		}
	}

	//collision detection
	if(GameServer()->Collision()->CheckPoint(m_Pos.x, m_Pos.y)) // this only works as long as the projectile is not moving too fast
	{
		Explode();
	}
	
}

void CPlasmaPlus::Explode() 
{
	//GameServer()->CreateSound(CurPos, m_SoundImpact);
	CCharacter *pTarget = GameServer()->GetPlayerChar(m_TrackedPlayer);
	if(m_Explosive && pTarget) 
	{
		GameServer()->CreateExplosionDisk(pTarget->m_Pos, 32.0f, 32.0f, g_Config.m_InfPlasmaPlusDamage, 16.0f, m_Owner, WEAPON_GUN, TAKEDAMAGEMODE_NOINFECTION);
	}else if(m_Explosive)
	{
		GameServer()->CreateExplosionDisk(m_Pos, 32.0f, 32.0f, g_Config.m_InfPlasmaPlusDamage, 16.0f, m_Owner, WEAPON_GUN, TAKEDAMAGEMODE_NOINFECTION);
	}
	Reset();
}

void CPlasmaPlus::Snap(int SnappingClient)
{
	if (IsDontSnapEntity(SnappingClient))
		return;
	
	
	CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(
		NETOBJTYPE_LASER, m_ID, sizeof(CNetObj_Laser)));
	
	if(!pObj)
		return;
	
	pObj->m_X = (int)m_Pos.x;
	pObj->m_Y = (int)m_Pos.y;
	pObj->m_FromX = (int)m_Pos.x;
	pObj->m_FromY = (int)m_Pos.y;
	pObj->m_StartTick = m_StartTick;
}
