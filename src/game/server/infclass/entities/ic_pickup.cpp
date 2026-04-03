/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "ic_pickup.h"

#include "engine/shared/config.h"

#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <game/server/infclass/classes/ic_playerclass.h>
#include <game/server/infclass/entities/ic_character.h>
#include <game/server/infclass/ic_gamecontroller.h>
#include <game/server/infclass/player_upgrades.h>
#include <game/server/infclass/snap_filter.h>

struct SUpgradeNetInfo
{
	int Type;
	int SubType;
};

struct SClassUpgrade
{
	SClassUpgrade() = default;

	void AddPickup(int Type, int SubType = 0)
	{
		m_aPickups.Add({Type, SubType});
	}

	bool IsValid() const { return !m_aPickups.IsEmpty(); }

	static int GetFlagPickupId()
	{
		return NUM_POWERUPS + 1;
	}

	icArray<SUpgradeNetInfo, 6> m_aPickups;
};

SClassUpgrade GetUpgradeInfo(const PlayerUpgradesArray &Upgrades)
{
	SClassUpgrade UpgradeInfo;
	for(EUpgradeType Upgrade : Upgrades)
	{
		switch(Upgrade)
		{
		case EUpgradeType::MercBombTools:
			UpgradeInfo.AddPickup(POWERUP_ARMOR);
			break;
		case EUpgradeType::MercGunAirRegen:
		case EUpgradeType::MercGunRegen:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GUN);
			break;
		case EUpgradeType::MercGrenades:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;
		case EUpgradeType::MercBombSupercharge:
			UpgradeInfo.AddPickup(POWERUP_HEALTH);
			break;

		case EUpgradeType::MedicPistolRegen:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GUN);
			break;
		case EUpgradeType::MedicShotgunSpread:
		case EUpgradeType::MedicShotgunRegen:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_SHOTGUN);
			break;
		case EUpgradeType::MedicHealingHose:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;

		case EUpgradeType::HeroFlagGift:
			UpgradeInfo.AddPickup(SClassUpgrade::GetFlagPickupId());
			break;
		case EUpgradeType::HeroWeapons:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GUN);
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_SHOTGUN);
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_LASER);
			break;
		case EUpgradeType::HeroArmor:
			UpgradeInfo.AddPickup(POWERUP_ARMOR);
			break;

		case EUpgradeType::NinjaSlashBreaksHooks:
		case EUpgradeType::NinjaSlashCombo:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_NINJA);
			break;
		case EUpgradeType::NinjaFlashGrenadeArea:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;

		case EUpgradeType::SniperLaserRegenReload:
		case EUpgradeType::SniperLaserRange:
		case EUpgradeType::SniperLaserPiercing:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_LASER);
			break;

		case EUpgradeType::ScientistLaserRegenReload:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_LASER);
			break;
		case EUpgradeType::ScientistTeleportGun:
		case EUpgradeType::ScientistPortalGun:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;
		case EUpgradeType::ScientistExtraMine:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_HAMMER);
			break;

		case EUpgradeType::BiologistShotgunSpread:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_SHOTGUN);
			break;
		case EUpgradeType::BiologistMineCharges:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_LASER);
			break;
		case EUpgradeType::BiologistGrenade:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;
		case EUpgradeType::BiologistInvisibilityHammer:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_HAMMER);
			break;

		case EUpgradeType::LooperLaserRegen:
		case EUpgradeType::LooperLaserWeapon:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_LASER);
			break;
		case EUpgradeType::LooperGrenadesRegen:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;
		case EUpgradeType::EngineerWallDamage:
		case EUpgradeType::EngineerWallTime:
		case EUpgradeType::EngineerWallTimeReductionDecrease:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_HAMMER);
			break;
		
		case EUpgradeType::ArtilleryGrenadeRegen:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_GRENADE);
			break;
		case EUpgradeType::ArtilleryLaserBomb:
		case EUpgradeType::ArtilleryLaserSuperAirStrike:
			UpgradeInfo.AddPickup(POWERUP_WEAPON, WEAPON_LASER);
			break;
		}
	}

	return UpgradeInfo;
}

int CIcPickup::EntityId = CGameWorld::ENTTYPE_PICKUP;

CIcPickup::CIcPickup(CGameContext *pGameContext, EICPickupType Type, vec2 Pos, int Owner) :
	CIcEntity(pGameContext, EntityId, Pos, Owner, PickupPhysSize)
{
	m_Type = Type;
	m_SpawnTick = -1;

	switch(Type)
	{
	case EICPickupType::Health:
		m_NetworkType = POWERUP_HEALTH;
		break;
	case EICPickupType::Armor:
		m_NetworkType = POWERUP_ARMOR;
		break;
	case EICPickupType::ClassUpgrade:
	case EICPickupType::Invalid:
		break;
	}

	GameWorld()->InsertEntity(this);
}

void CIcPickup::Reset()
{
	if(HasOwner())
		CIcEntity::Reset();

	m_SpawnTick = -1;
}

void CIcPickup::Tick()
{
	// wait for respawn
	if(m_SpawnTick > 0)
	{
		if(Server()->Tick() > m_SpawnTick)
		{
			// respawn
			m_SpawnTick = -1;
			m_AutoPickUpTick = Server()->Tick() + Server()->TickSpeed() * Config()->m_InfUpgradeAutoPickupTime;
			GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SPAWN);
		}
		else
			return;
	}

	CIcEntity::Tick();

	if(m_Type == EICPickupType::ClassUpgrade)
	{
		if(!GameController()->IsPositionAvailableForHumans(GetPos()))
		{
			MarkForDestroy();
			return;
		}
	}

	// Check if a player intersected us
	CIcCharacter *pChr = nullptr;
	if(GetOwner() >= 0)
	{
		CIcCharacter *pOwner = GetOwnerCharacter();
		if(pOwner && (distance(GetPos(), pOwner->GetPos()) < PickupPhysSize + pOwner->GetProximityRadius() ||
						 (m_AutoPickUpTick > 0 && Server()->Tick() >= m_AutoPickUpTick)))
		{
			pChr = pOwner;
		}
	}
	else
	{
		pChr = (CIcCharacter *)GameWorld()->ClosestEntity(m_Pos, PickupPhysSize, CGameWorld::ENTTYPE_CHARACTER, nullptr);
	}

	if(pChr && pChr->IsAlive())
	{
		bool Picked = false;
		switch(m_Type)
		{
		case EICPickupType::Health:
			if(pChr->GiveHealth(1))
			{
				Picked = true;
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_HEALTH);
			}
			break;

		case EICPickupType::Armor:
			if(pChr->GiveArmor(1))
			{
				Picked = true;
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_ARMOR);
			}
			break;
		case EICPickupType::ClassUpgrade:
			pChr->GetClass()->GiveUpgrades(m_Upgrades);

			if(m_NetworkType == POWERUP_ARMOR)
			{
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_ARMOR);
			}
			else if(m_NetworkSubtype == WEAPON_GRENADE)
			{
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_GRENADE);
			}
			else
			{
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_SHOTGUN);
			}
			Picked = true;
			break;
		default:
			break;
		};

		if(Picked)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "pickup player='%d:%s' item=%d",
				pChr->GetCid(), Server()->ClientName(pChr->GetCid()), static_cast<int>(m_Type));
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
			if(m_SpawnInterval >= 0)
			{
				Spawn(m_SpawnInterval);
			}
			else
			{
				MarkForDestroy();
			}
		}
	}

	UpdateNetworkTypes();
}

void CIcPickup::TickPaused()
{
	if(m_SpawnTick != -1)
		++m_SpawnTick;
}

void CIcPickup::Snap(int SnappingClient)
{
	if(m_SpawnTick != -1 || NetworkClipped(SnappingClient))
		return;

	if(m_Type == EICPickupType::Invalid)
		return;

	if(!SnapFiltersPassed(this, SnappingClient, EFilterFlag::Follower | EFilterFlag::FreeSpec | EFilterFlag::Demo | EFilterFlag::Restricted))
		return;

	int NetworkType = -1;
	int Subtype = 0;
	switch(m_Type)
	{
	case EICPickupType::Health:
	case EICPickupType::Armor:
	case EICPickupType::ClassUpgrade:
		if(m_NetworkType == SClassUpgrade::GetFlagPickupId())
		{
			SnapAsFlag();
		}
		else
		{
			NetworkType = m_NetworkType;
			Subtype = m_NetworkSubtype;
		}
		break;
	case EICPickupType::Invalid:
		break;
	}

	if(NetworkType < 0)
		return;

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	GameServer()->SnapPickup(CSnapContext(SnappingClientVersion), GetId(), m_Pos, NetworkType, Subtype);
}

void CIcPickup::SnapAsFlag()
{
	CNetObj_Flag *pFlag = Server()->SnapNewItem<CNetObj_Flag>(GetId());
	if(!pFlag)
		return;

	pFlag->m_X = round_to_int(m_Pos.x);
	pFlag->m_Y = round_to_int(m_Pos.y) + 16;
	pFlag->m_Team = TEAM_RED;
}

void CIcPickup::Spawn(float Delay)
{
	m_SpawnTick = Server()->Tick() + Server()->TickSpeed() * Delay;

	UpdateNetworkTypes();
}

void CIcPickup::SetRespawnInterval(float Seconds)
{
	m_SpawnInterval = Seconds;
}

void CIcPickup::SetUpgrades(const PlayerUpgradesArray &Upgrades)
{
	m_Upgrades = Upgrades;
}

void CIcPickup::UpdateNetworkTypes()
{
	if(m_Upgrades.IsEmpty())
		return;

	SClassUpgrade Info = GetUpgradeInfo(m_Upgrades);

	SUpgradeNetInfo UpgradeInfo = Info.m_aPickups.First();
	if(m_Upgrades.Size() > 1)
	{
		int Index = (Server()->Tick() - m_SpawnTick) / (Server()->TickSpeed() * 1.5f);
		UpgradeInfo = Info.m_aPickups.At(Index % Info.m_aPickups.Size());
	}

	m_NetworkType = UpgradeInfo.Type;
	m_NetworkSubtype = UpgradeInfo.SubType;
}
