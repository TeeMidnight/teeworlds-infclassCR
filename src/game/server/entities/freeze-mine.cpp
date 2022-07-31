/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "freeze-mine.h"
#include "growingexplosion.h"
#include <game/server/gamecontext.h>

CFreezeMine::CFreezeMine(CGameWorld *pGameWorld, vec2 Pos, int Owner, float Radius)
: CEntity(pGameWorld, CGameWorld::ENTTYPE_FREEZE_MINE)
{
    m_Pos = Pos;
	m_ActualPos = Pos;
    m_Owner = Owner;
    m_Radius = Radius;
    m_StartTick = Server()->Tick();
    m_IDs.set_size(9);
	for(int i = 0; i < m_IDs.size(); i++)
	{
		m_IDs[i] = Server()->SnapNewID();
	}
    GameWorld()->InsertEntity(this);
}

CFreezeMine::~CFreezeMine()
{
	for(int i = 0; i < m_IDs.size(); i++)
	{
		Server()->SnapFreeID(m_IDs[i]);
	}
}

void CFreezeMine::Reset()
{
    GameServer()->m_World.DestroyEntity(this);
}

int CFreezeMine::GetOwner() const
{
	return m_Owner;
}

void CFreezeMine::Tick()
{
	for(CCharacter *pChr = (CCharacter*) GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->IsZombie()) continue;
		float Len = distance(pChr->m_Pos, m_Pos);
		if(Len < pChr->m_ProximityRadius+m_Radius)
		{
			vec2 Dir = normalize(pChr->m_Pos - m_ActualPos);
			m_ActualPos += Dir * 8.0f;
			float ActualLen = distance(pChr->m_Pos, m_ActualPos);
			if(ActualLen < pChr->m_ProximityRadius + 4)
			{
				Dir = normalize(pChr->m_Pos - m_ActualPos);
				Explode();
			}
		}
	}
}

void CFreezeMine::Explode()
{
	new CGrowingExplosion(GameWorld(), m_ActualPos, vec2(0.0, 0.0), m_Owner, m_Radius / 32 * 2, GROWINGEXPLOSIONEFFECT_FREEZE_HUMAN);

	if(GameServer()->GetPlayerChar(m_Owner))
	{
		GameServer()->GetPlayerChar(m_Owner)->m_HasFreezeMine = false;
		GameServer()->SendScoreSound(m_Owner);
	}
	Reset();
}

void CFreezeMine::Snap(int SnappingClient)
{
	
	if(IsDontSnapEntity(SnappingClient))
		return;
    
    if(GameServer()->m_apPlayers[SnappingClient]->IsHuman())
        return;


	float time = (Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed();
	float angle = fmodf(time*pi/2, 2.0f*pi);
	
	for(int i=0; i<m_IDs.size()-1; i++)
	{	
		float shiftedAngle = angle + 2.0*pi*static_cast<float>(i)/static_cast<float>(m_IDs.size()-1);
		
		CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_IDs[i], sizeof(CNetObj_Projectile)));
		
		if(!pObj)
			continue;
		
		pObj->m_X = (int)(m_Pos.x + m_Radius*cos(shiftedAngle));
		pObj->m_Y = (int)(m_Pos.y + m_Radius*sin(shiftedAngle));
		pObj->m_VelX = 0;
		pObj->m_VelY = 0;
		pObj->m_StartTick = Server()->Tick();
	}
	
	CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_IDs[m_IDs.size()-1], sizeof(CNetObj_Projectile)));
	
	if(!pObj)
		return;
	
	pObj->m_X = (int)m_ActualPos.x;
	pObj->m_Y = (int)m_ActualPos.y;
    pObj->m_Type = WEAPON_GRENADE;
    pObj->m_VelX = 0;
    pObj->m_VelY = 0;
	pObj->m_StartTick = Server()->Tick();
}
