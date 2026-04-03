#include <base/math.h>
#include <base/vmath.h>
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>

#include <game/infclass/damage_type.h>
#include <game/server/infclass/classes/humans/human.h>
#include <game/server/infclass/entities/ic_character.h>
#include <game/server/infclass/entities/artillery-grenade.h>
#include <game/server/infclass/ic_player.h>
#include <game/server/infclass/ic_gamecontroller.h>
#include <game/server/infclass/player_upgrades.h>

#include "artillery-laser.h"

int CArtilleryLaser::EntityId{};

CArtilleryLaser::CArtilleryLaser(CGameContext *pGameContext, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
	int Damage, float Force, EDamageType DamageType, bool IsSuperWeapon)
		: CArtilleryProjectile(pGameContext, Type, Owner, Pos, Dir, Span, Damage, Force, DamageType, EntityId)
{
	m_AirStrikeNum = 0;
	m_AirStrikeTick = 0;
	FillAirStrike(IsSuperWeapon);

	GameWorld()->InsertEntity(this);

	CIcCharacter *pOwnerChr = GetOwnerCharacter();
	if(!pOwnerChr)
		return;
	CInfClassHuman *pHuman = CInfClassHuman::GetInstance(pOwnerChr->GetPlayer());
	if(!pHuman)
		return;
	if(GameController()->GetRoundType() == ERoundType::Survival)
	{
		m_AirStrikeTotal = !IsSuperWeapon ? (pHuman->HasUpgrade(EUpgradeType::ArtilleryLaserBomb) ? 5 : 3) :
											(pHuman->HasUpgrade(EUpgradeType::ArtilleryLaserSuperAirStrike) ? 30 : 15);
	}
	if(!pHuman->HasAirStrike())
		return;
	pHuman->RemoveAirStrike();
};

vec2 CArtilleryLaser::GetPos()
{
	return CalcPos(m_Pos, m_Direction, (float)GameServer()->Tuning()->m_GrenadeCurvature, (float)GameServer()->Tuning()->m_GrenadeSpeed, (float)(Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed() * 2);
}

bool CArtilleryLaser::OnProjectileCollide(CCharacter *pChr)
{
	m_LifeSpan = -1;
	CCharacter *pOwnerChr = GetOwnerCharacter();
	if(pOwnerChr)
		m_AirStrikeInvert = m_ActualPos.x < pOwnerChr->GetPos().x;
	return m_AirStrikeTotal <= 0;
}

void CArtilleryLaser::Tick()
{
	if(m_LifeSpan > 0)
		CArtilleryProjectile::Tick();
	if(m_LifeSpan > 0 || m_AirStrikeTick > Server()->Tick())
		return;
	CIcCharacter *pOwnerChr = GetOwnerCharacter();
	for(int i = m_AirStrikePerBomb; i > 0 && m_AirStrikeNum < m_AirStrikeTotal; i--)
	{
		vec2 StartPos = m_ActualPos;
		vec2 StartDir;

		float Angle = (random_angle() * 0.0625f) - (pi * 0.5625f);
		if(pOwnerChr)
			Angle += clamp((pOwnerChr->GetPos().x - StartPos.x) / 640.f, -0.25f, 0.25f);

		StartPos.x += GetAirStrikeSlot(m_AirStrikeInvert) * m_AirStrikeSpacing;
		StartDir = direction(Angle);

		StartPos += StartDir * 768.f;
		StartDir = -StartDir * 2;

		CArtilleryGrenade *pEnt = new CArtilleryGrenade(GameServer(), (int)WEAPON_GRENADE, m_Owner.value(), StartPos, StartDir,
				(int)(GameServer()->Server()->TickSpeed() * GameServer()->Tuning()->m_GrenadeLifetime), 0, 0.f, EDamageType::ARTILLERY_BOMB);
		pEnt->SetExplosive(true);
		pEnt->SetSoundImpact(SOUND_GRENADE_EXPLODE);
		pEnt->SetCollideSpan(Server()->TickSpeed() * 0.35f, Server()->TickSpeed() * 0.2f);

		m_AirStrikeNum++;
	}

	if(m_AirStrikeNum >= m_AirStrikeTotal)
	{
		GameWorld()->DestroyEntity(this);
		return;
	}

	m_AirStrikeTick = Server()->Tick() + m_AirStrikeDeley / (1000.f / (float)Server()->TickSpeed());
}
