/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/math.h>
#include <base/vmath.h>

#include "anti-airmine.h"

#include "growingexplosion.h"

CAntiAirMine::CAntiAirMine(CGameWorld *pGameWorld, vec2 Pos, int Owner)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_ANTI_AIRMINE)
{
	m_Pos = Pos;
	GameWorld()->InsertEntity(this);
	m_StartTick = Server()->Tick();
	m_Owner = Owner;
    m_AttackNow = false;
    m_LifeSpan = g_Config.m_InfAntiAirMineLifeSpan;
    m_Angle = 0;

	for(int i=0; i<NUM_LASERS; i++)
	{
		m_LaserIDs[i] = Server()->SnapNewID();
	}

    for(int i=0; i<NUM_AMMOS; i++)
	{
		m_AmmoIDs[i] = Server()->SnapNewID();
	}
}

CAntiAirMine::~CAntiAirMine()
{
	for(int i=0; i<NUM_LASERS; i++)
	{
		Server()->SnapFreeID(m_LaserIDs[i]);
	}

    for(int i=0; i<NUM_AMMOS; i++)
	{
		Server()->SnapFreeID(m_AmmoIDs[i]);
	}
}

void CAntiAirMine::Reset()
{
	GameServer()->m_World.DestroyEntity(this);
}

int CAntiAirMine::GetOwner() const
{
	return m_Owner;
}

void CAntiAirMine::Explode()
{
	float Radius = g_Config.m_InfAntiAirMineRadius;
	GameServer()->CreateExplosionDisk(m_Pos, Radius, Radius, g_Config.m_InfAntiAirMineDamage,
		 32.0f, m_Owner, WEAPON_HAMMER, TAKEDAMAGEMODE_SELFHARM);
		
	GameServer()->m_World.DestroyEntity(this);
}

void CAntiAirMine::Snap(int SnappingClient)
{
	
	if(NetworkClipped(SnappingClient))
		return;
		
	float Radius = g_Config.m_InfAntiAirMineRadius;
	float Angle = m_Angle;

    for(int i = 0; i < NUM_LASERS; i++)
	{
		vec2 EndPos = m_Pos + (GetDir(Angle*pi/180) * Radius);
		CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_LaserIDs[i], sizeof(CNetObj_Laser)));
		if(!pObj)
			return;

		pObj->m_FromX = (int)m_Pos.x;
		pObj->m_FromY = (int)m_Pos.y;
		pObj->m_X = (int)EndPos.x;
		pObj->m_Y = (int)EndPos.y;
		pObj->m_StartTick = Server()->Tick();
		
		m_LaserPos[i] = EndPos;

		if(!Server()->GetClientAntiPing(SnappingClient))
		{
			vec2 AmmoPos = m_Pos + (GetDir((Angle + 180 / NUM_LASERS )*pi/180) * Radius);
			CNetObj_Projectile *pPro = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_AmmoIDs[i], sizeof(CNetObj_Projectile)));

			pPro->m_X = AmmoPos.x;
			pPro->m_Y = AmmoPos.y;
			pPro->m_Type = WEAPON_SHOTGUN;
			pPro->m_StartTick = Server()->Tick()-1;
		}
		Angle += 360 / NUM_LASERS;
	}

}

void CAntiAirMine::Tick()
{
	if(!m_AttackNow)
    {
        for(CCharacter *p = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); p; p = (CCharacter *)p->TypeNext())
        {
            if(p->IsHuman()) continue;
            if(p->GetClass() == PLAYERCLASS_UNDEAD && p->IsFrozen()) continue;
            if(p->GetClass() == PLAYERCLASS_VOODOO && p->m_VoodooAboutToDie) continue;

            float Len = distance(p->m_Pos, m_Pos);
            if(Len < p->m_ProximityRadius+g_Config.m_InfAntiAirMineRadius)
            {
                m_AttackNow = true;
            }
        }
    }else
    {
		for(CCharacter *p = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); p; p = (CCharacter *)p->TypeNext())
        {
            if(p->IsHuman() && p->GetPlayer()->GetCID() != m_Owner) continue;
            if(p->GetClass() == PLAYERCLASS_UNDEAD && p->IsFrozen()) continue;
            if(p->GetClass() == PLAYERCLASS_VOODOO && p->m_VoodooAboutToDie) continue;
			
			for(int i = 0;i < NUM_LASERS; i++)
			{
				vec2 IntersectPos = closest_point_on_line(m_Pos, m_LaserPos[i], p->m_Pos);
				float Len = distance(p->m_Pos, IntersectPos);

				if(Len < p->m_ProximityRadius)
				{
					float Radius = g_Config.m_InfAntiAirMineRadius;
					int Damage = g_Config.m_InfAntiAirMineDamage;
					
					GameServer()->CreateExplosionDisk(m_Pos, Radius, Radius, m_Owner == p->GetPlayer()->GetCID() ? Damage/2 : Damage,
						0.0f, m_Owner, WEAPON_HAMMER, TAKEDAMAGEMODE_SELFHARM);
				}
			}
        }

		MovePlayer();
        m_Angle += g_Config.m_InfAntiAirMineSpeed;
        m_LifeSpan--;
		
    }
	
	if(m_LifeSpan <= 0)
		Explode();
}

void CAntiAirMine::MovePlayer()
{
	for(CCharacter *p = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); p; p = (CCharacter *)p->TypeNext())
	{
		if(p->IsHuman()) continue;
		int Len = distance(p->m_Pos, m_Pos);
		if(Len < p->m_ProximityRadius + g_Config.m_InfAntiAirMineRadius)
		{
			vec2 Dir = normalize(m_Pos - p->m_Pos);
			p->SetVel(Dir * 8.0f);
		}
		
	}
}

void CAntiAirMine::TickPaused()
{
	++m_StartTick;
}
