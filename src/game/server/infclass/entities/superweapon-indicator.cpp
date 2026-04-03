/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "superweapon-indicator.h"
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>

#include <game/server/infclass/classes/humans/human.h>
#include <game/server/infclass/entities/ic_character.h>
#include <game/server/infclass/ic_gamecontroller.h>
#include <game/server/infclass/ic_player.h>

int CSuperWeaponIndicator::EntityId{};

CSuperWeaponIndicator::CSuperWeaponIndicator(CGameContext *pGameContext, vec2 Pos, int Owner) : CIcEntity(pGameContext, EntityId, Pos, Owner)
{
	GameWorld()->InsertEntity(this);
	m_Radius = 40.0f;
	m_StartTick = Server()->Tick();

	m_warmUpCounter = Server()->TickSpeed() * 3;
	m_IsWarmingUp = true;

	m_Ids.set_size(3);
	for(int i = 0; i < m_Ids.size(); i++)
	{
		m_Ids[i] = Server()->SnapNewId();
	}
}

CSuperWeaponIndicator::~CSuperWeaponIndicator()
{
	for(int i = 0; i < m_Ids.size(); i++)
		Server()->SnapFreeId(m_Ids[i]);
}

void CSuperWeaponIndicator::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	const CIcPlayer *pPlayer = GameController()->GetPlayer(SnappingClient);
	const bool AntiPing = pPlayer && pPlayer->GetAntiPingEnabled();

	if(AntiPing)
		return;

	float time = (Server()->Tick() - m_StartTick) / (float)Server()->TickSpeed();
	float angle = fmodf(time * pi / 2, 2.0f * pi);

	for(int i = 0; i < m_Ids.size(); i++)
	{
		float shiftedAngle = angle + 2.0 * pi * static_cast<float>(i) / static_cast<float>(m_Ids.size());
		vec2 ParticlePos = m_Pos + vec2(cos(shiftedAngle), sin(shiftedAngle)) * m_Radius;

		GameController()->SendHammerDot(ParticlePos, m_Ids[i]);
	}
}

void CSuperWeaponIndicator::Tick()
{
	if(IsMarkedForDestroy())
		return;

	CIcCharacter *pOwnerChar = GetOwnerCharacter();
	CInfClassHuman *pHuman = CInfClassHuman::GetInstance(pOwnerChar);

	if(!pHuman)
		return;

	if(!pOwnerChar->HasSuperWeaponIndicator())
	{
		GameWorld()->DestroyEntity(this);
		return;
	}

	// refresh indicator position
	SetPos(pOwnerChar->Core()->m_Pos);

	if(m_IsWarmingUp)
	{
		if(m_warmUpCounter > 0)
		{
			m_warmUpCounter--;
		}
		else
		{
			m_IsWarmingUp = false;
			if(pHuman->GetPlayerClass() == EPlayerClass::Scientist)
				pHuman->GiveWhiteHole();
			else if(pHuman->GetPlayerClass() == EPlayerClass::Artillery)
				pHuman->GiveAirStrike();
		}
	}
}
