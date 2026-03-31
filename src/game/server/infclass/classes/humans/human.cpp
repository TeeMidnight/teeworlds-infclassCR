#include "human.h"
#include "game/mapitems.h"

#include <engine/server/roundstatistics.h>
#include <engine/shared/config.h>
#include <game/infclass/ic_classes.h>
#include <game/server/gamecontext.h>

#include <game/generated/server_data.h>

#include <game/server/infclass/damage_context.h>
#include <game/infclass/damage_type.h>
#include <game/server/infclass/death_context.h>
#include <game/server/infclass/entities/artillery-grenade.h>
#include <game/server/infclass/entities/artillery-laser.h>
#include <game/server/infclass/entities/biologist-mine.h>
#include <game/server/infclass/entities/blinding-laser.h>
#include <game/server/infclass/entities/bouncing-bullet.h>
#include <game/server/infclass/entities/engineer-wall.h>
#include <game/server/infclass/entities/growingexplosion.h>
#include <game/server/infclass/entities/hero-flag.h>
#include <game/server/infclass/entities/ic_character.h>
#include <game/server/infclass/entities/ic_projectile.h>
#include <game/server/infclass/entities/laser-teleport.h>
#include <game/server/infclass/entities/looper-wall.h>
#include <game/server/infclass/entities/medic-laser.h>
#include <game/server/infclass/entities/merc-bomb.h>
#include <game/server/infclass/entities/merc-laser.h>
#include <game/server/infclass/entities/scatter-grenade.h>
#include <game/server/infclass/entities/scientist-laser.h>
#include <game/server/infclass/entities/scientist-mine.h>
#include <game/server/infclass/entities/soldier-bomb.h>
#include <game/server/infclass/entities/turret.h>
#include <game/server/infclass/entities/white-hole.h>
#include <game/server/infclass/ic_gamecontroller.h>
#include <game/server/infclass/ic_player.h>
#include <game/server/teeinfo.h>

static const int s_SniperPositionLockTimeLimit = 15;

MACRO_ALLOC_POOL_ID_IMPL(CInfClassHuman, MAX_CLIENTS)

CInfClassHuman::CInfClassHuman(CIcPlayer *pPlayer)
	: CIcPlayerClass(pPlayer)
{
	m_BroadcastWhiteHoleReady = -100;
	m_BroadcastAirStrikeReady = -100;

	ResetUpgrades();
}

CInfClassHuman *CInfClassHuman::GetInstance(CIcPlayer *pPlayer)
{
	CIcPlayerClass *pClass = pPlayer ? pPlayer->GetCharacterClass() : nullptr;
	return GetInstance(pClass);
}

CInfClassHuman *CInfClassHuman::GetInstance(CIcCharacter *pCharacter)
{
	CIcPlayerClass *pClass = pCharacter ? pCharacter->GetClass() : nullptr;
	return GetInstance(pClass);
}

CInfClassHuman *CInfClassHuman::GetInstance(CIcPlayerClass *pClass)
{
	if(pClass && pClass->IsHuman())
	{
		return static_cast<CInfClassHuman *>(pClass);
	}

	return nullptr;
}

SkinGetter CInfClassHuman::GetSkinGetter() const
{
	return CInfClassHuman::SetupSkin;
}

void CInfClassHuman::SetupSkinContext(CSkinContext *pOutput, bool ForSameTeam) const
{
	pOutput->PlayerClass = GetPlayerClass();
	pOutput->ExtraData1 = 0;
}

bool CInfClassHuman::SetupSkin(const CSkinContext &Context, CWeakSkinInfo *pOutput, int DDNetVersion, int InfClassVersion)
{
	switch(Context.PlayerClass)
	{
	case EPlayerClass::Engineer:
		pOutput->UseCustomColor = 0;
		pOutput->pSkinName = "limekitty";
		break;
	case EPlayerClass::Soldier:
		pOutput->pSkinName = "brownbear";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Sniper:
		pOutput->pSkinName = "warpaint";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Mercenary:
		pOutput->pSkinName = "bluestripe";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Scientist:
		pOutput->pSkinName = "toptri";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Biologist:
		pOutput->pSkinName = "twintri";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Looper:
		pOutput->pSkinName = "bluekitty";
		pOutput->UseCustomColor = 1;
		pOutput->ColorBody = 255;
		pOutput->ColorFeet = 0;
		break;
	case EPlayerClass::Medic:
		pOutput->pSkinName = "twinbop";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Hero:
		pOutput->pSkinName = "redstripe";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::Ninja:
		pOutput->pSkinName = "default";
		pOutput->UseCustomColor = 1;
		pOutput->ColorBody = 255;
		pOutput->ColorFeet = 0;
		break;
	case EPlayerClass::Artillery:
		pOutput->pSkinName = "kintaro_2";
		pOutput->UseCustomColor = 0;
		break;
	case EPlayerClass::None:
		pOutput->pSkinName = "default";
		pOutput->UseCustomColor = 0;
		break;
	default:
		return false;
	}

	return true;
}


CAmmoParams CInfClassHuman::GetAmmoParams(int Weapon) const
{
	CAmmoParams Params = CIcPlayerClass::GetAmmoParams(Weapon);
	EInfclassWeapon InfWID = m_pCharacter->GetInfWeaponId(Weapon);

	const float RegenIntervalModifier = m_WeaponRegenIntervalModifier[Weapon];
	Params.RegenInterval *= RegenIntervalModifier;

	switch(InfWID)
	{
	case EInfclassWeapon::NINJA_GRENADE:
		Params.MaxAmmo = minimum(Params.MaxAmmo + m_NinjaAmmoBuff, 10);
		break;
	case EInfclassWeapon::MERCENARY_GUN:
		if(m_pCharacter->GetInAirTick() > Server()->TickSpeed() * 4)
		{
			Params.RegenInterval = 0;
		}
		break;
	default:
		break;
	}

	if((Config()->m_InfTaxi == 1) && m_pCharacter->IsPassenger())
	{
		Params.RegenInterval = 0;
	}

	return Params;
}

int CInfClassHuman::GetJumps() const
{
	switch(GetPlayerClass())
	{
	case EPlayerClass::Sniper:
	case EPlayerClass::Looper:
		return 3;
	default:
		return 2;
	}
}

void CInfClassHuman::GiveGift(EGiftType GiftType)
{
	if(!m_pCharacter)
		return;

	m_pCharacter->IncreaseHealth(1);
	m_pCharacter->GiveArmor(4);

	const EWeapon AllWeaponsWithAmmo[] =
		{
			WEAPON_GUN,
			WEAPON_SHOTGUN,
			WEAPON_GRENADE,
			WEAPON_LASER,
		};

	for(EWeapon WeaponSlot : AllWeaponsWithAmmo)
	{
		if(m_pCharacter->HasWeapon(WeaponSlot))
		{
			m_pCharacter->GiveWeapon(WeaponSlot, -1);
		}
	}
}

bool CInfClassHuman::CanBeHit() const
{
	if(GetPlayerClass() == EPlayerClass::Ninja)
	{
		// Do not hit slashing ninjas
		if(m_pCharacter->m_DartLifeSpan >= 0)
		{
			return false;
		}
	}

	return true;
}

SClassUpgrade CInfClassHuman::GetNextUpgrade() const
{
	return SClassUpgrade::Invalid();
}

void CInfClassHuman::OnPlayerClassChanged()
{
	CIcPlayerClass::OnPlayerClassChanged();

	ResetUpgrades();
	ResetUpgradeLevel();
}

void CInfClassHuman::CheckSuperWeaponAccess()
{
	if(m_KillsProgression < 0)
		return;

	// check kills of player
	int Kills = m_KillsProgression;

	// Only scientists can receive white holes
	if(GetPlayerClass() == EPlayerClass::Scientist)
	{
		if(!GameController()->WhiteHoleEnabled() || m_pCharacter->HasSuperWeaponIndicator())
		{
			// Can't receive a super weapon while having one available
			return;
		}

		// enable white hole probabilities
		if(Kills < Config()->m_InfWhiteHoleMinimalKills)
		{
			return;
		}

		if(random_prob(Config()->m_InfWhiteHoleProbability / 100.0f))
		{
			// Scientist-laser.cpp will make it unavailable after usage and reset player kills
			GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("White hole found, adjusting scientific parameters..."), nullptr);
			m_pCharacter->SetSuperWeaponIndicatorEnabled(true);
		}
	}
}

void CInfClassHuman::OnPlayerSnap(int SnappingClient, int InfClassVersion)
{
	if(InfClassVersion < VERSION_INFC_140)
	{
		// CNetObj_InfClassClassInfo introduced in v0.1.4
		return;
	}

	CNetObj_InfClassClassInfo *pClassInfo = Server()->SnapNewItem<CNetObj_InfClassClassInfo>(GetCid());
	if(!pClassInfo)
		return;
	pClassInfo->m_Class = toNetValue(GetPlayerClass());
	pClassInfo->m_Flags = 0;
	pClassInfo->m_Data1 = -1;

	if(GameController()->CanSeeDetails(SnappingClient, GetCid()))
	{
		switch(GetPlayerClass())
		{
		case EPlayerClass::Hero:
			if(m_pHeroFlag && m_pHeroFlag->IsAvailable())
			{
				pClassInfo->m_Data1 = m_pHeroFlag->GetSpawnTick();
			}
			else
			{
				pClassInfo->m_Data1 = -1;
			}
			break;
		case EPlayerClass::Engineer:
			for(TEntityPtr<CEngineerWall> pWall = GameWorld()->FindFirst<CEngineerWall>(); pWall; ++pWall)
			{
				if(pWall->GetOwner() != GetCid())
				{
					continue;
				}

				pClassInfo->m_Data1 = pWall->GetEndTick().value_or(-1);
				break;
			}
			break;
		case EPlayerClass::Scientist:
			if(m_pCharacter && m_pCharacter->IsAlive())
			{
				pClassInfo->m_Data1 = f2fx(m_KillsProgression);
			}
			break;
		case EPlayerClass::Looper:
			for(TEntityPtr<CLooperWall> pWall = GameWorld()->FindFirst<CLooperWall>(); pWall; ++pWall)
			{
				if(pWall->GetOwner() != GetCid())
				{
					continue;
				}

				pClassInfo->m_Data1 = pWall->GetEndTick().value_or(-1);
				break;
			}
			break;
		default:
			break;
		}

		if(m_pCharacter)
		{
			if(m_pCharacter->IsInvisible())
				pClassInfo->m_Flags |= INFCLASS_CLASSINFO_FLAG_IS_INVISIBLE;
		}
	}
}

void CInfClassHuman::OnCharacterPreCoreTick()
{
	CIcPlayerClass::OnCharacterPreCoreTick();

	switch (GetPlayerClass())
	{
		case EPlayerClass::Sniper:
		{
			if(m_pCharacter->PositionIsLocked())
			{
				--m_PositionLockTicksRemaining;
				if((m_PositionLockTicksRemaining <= 0) || m_pCharacter->IsPassenger())
				{
					m_pCharacter->UnlockPosition();
				}
			}

			if(!m_pCharacter->PositionIsLocked())
			{
				if(m_pCharacter->IsGrounded())
				{
					m_PositionLockTicksRemaining = Server()->TickSpeed() * s_SniperPositionLockTimeLimit;
				}
			}

			if(m_pCharacter->PositionIsLocked())
			{
				if(m_pCharacter->m_Input.m_Jump && !m_pCharacter->m_PrevInput.m_Jump)
				{
					m_pCharacter->UnlockPosition();
				}
				else
				{
					m_pCharacter->ResetMovementsInput();
					m_PositionLockPosition = m_pCharacter->GetPos();
				}
			}
		}
			break;
		case EPlayerClass::Ninja:
		{
			if(m_pCharacter->IsGrounded() && m_pCharacter->m_DartLifeSpan <= 0)
			{
				m_pCharacter->m_DartLeft = Config()->m_InfNinjaJump;
			}
		}
			break;
		default:
			break;
	}
}

void CInfClassHuman::OnCharacterTick()
{
	CIcPlayerClass::OnCharacterTick();

	switch(GetPlayerClass())
	{
	case EPlayerClass::Ninja:
	{
		if(Server()->Tick() > m_NinjaTargetTick)
		{
			const ClientsArray &ValidNinjaTargets = GameController()->GetValidNinjaTargets();
			if(!ValidNinjaTargets.Contains(m_NinjaTargetCid))
			{
				if(ValidNinjaTargets.IsEmpty())
				{
					m_NinjaTargetCid = -1;
				}
				else
				{
					int Index = random_int(0, ValidNinjaTargets.Size() - 1);
					m_NinjaTargetCid = ValidNinjaTargets[Index];
				}
			}
		}
		else
		{
			m_NinjaTargetCid = -1;
		}
		break;
	}
	default:
		break;
	}

	if(m_pCharacter->IsAlive() && GameController()->IsInfectionStarted())
	{
		int BonusZoneIndex = GameController()->GetBonusZoneValueAt(GetPos());
		if(BonusZoneIndex == ZONE_BONUS_BONUS)
		{
			m_BonusTick++;
		}

		if(m_BonusTick > Server()->TickSpeed()*60)
		{
			m_BonusTick = 0;

			GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE,
				_("You have held a bonus area for one minute, +5 points"), nullptr);
			GameServer()->SendEmoticon(GetCid(), EMOTICON_MUSIC);
			m_pCharacter->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
			GiveGift(EGiftType::BonusZone);

			Server()->RoundStatistics()->OnScoreEvent(GetCid(), EScoreEvent::BONUS, GetPlayerClass(),
				Server()->ClientName(GetCid()), GameServer()->Console());
			GameServer()->SendScoreSound(GetCid());
		}
	}
	else
	{
		m_BonusTick = 0;
	}

	if(m_ResetKillsTick >= 0)
	{
		if(Server()->Tick() >= m_ResetKillsTick)
		{
			m_KillsProgression = 0;
			m_ResetKillsTick = -1;
		}
	}
}

void CInfClassHuman::OnCharacterTickPaused()
{
	if(m_ResetKillsTick >= 0)
	{
		++m_ResetKillsTick;
	}
}

void CInfClassHuman::OnCharacterPostCoreTick()
{
	CIcPlayerClass::OnCharacterPostCoreTick();

	switch(GetPlayerClass())
	{
	case EPlayerClass::Sniper:
		if(m_pCharacter->PositionIsLocked())
		{
			m_pCharacter->ResetVelocity();
			m_pCharacter->SetPosition(m_PositionLockPosition);
		}
		break;
	default:
		break;
	}
}

void CInfClassHuman::OnCharacterSnap(int SnappingClient)
{
	switch(GetPlayerClass())
	{
	case EPlayerClass::Hero:
		SnapHero(SnappingClient);
		break;
	case EPlayerClass::Scientist:
		SnapScientist(SnappingClient);
		break;
	default:
		break;
	}

	if(SnappingClient != m_pPlayer->GetCid())
	{
		const CIcPlayer *pDestClient = GameController()->GetPlayer(SnappingClient);
		if(pDestClient && pDestClient->GetCharacter())
		{
			switch(pDestClient->GetClass())
			{
			case EPlayerClass::Medic:
				if(m_pCharacter->GetArmor() < 10)
				{
					if(GetPlayerClass() == EPlayerClass::Hero)
					{
						if(pDestClient->GetCharacter()->GetActiveWeapon() != WEAPON_GRENADE)
						{
							return;
						}
					}

					CNetObj_Pickup *pP = Server()->SnapNewItem<CNetObj_Pickup>(m_pCharacter->GetHeartId());
					if(!pP)
						return;

					const vec2 Pos = m_pCharacter->GetPos();
					pP->m_X = Pos.x;
					pP->m_Y = Pos.y - 60.0;

					if(m_pCharacter->GetHealth() < 10 && m_pCharacter->GetArmor() == 0)
						pP->m_Type = POWERUP_HEALTH;
					else
						pP->m_Type = POWERUP_ARMOR;
					pP->m_Subtype = 0;
				}
				break;
			case EPlayerClass::Biologist:
				if(m_pCharacter->IsPoisoned())
				{
					CNetObj_Pickup *pP = Server()->SnapNewItem<CNetObj_Pickup>(m_pCharacter->GetHeartId());
					if(!pP)
						return;

					const vec2 Pos = m_pCharacter->GetPos();
					pP->m_X = Pos.x;
					pP->m_Y = Pos.y - 60.0;
					pP->m_Type = POWERUP_HEALTH;
					pP->m_Subtype = 0;
				}
				break;
			default:
				break;
			}
		}
	}
}

void CInfClassHuman::OnCharacterSpawned(const SpawnContext &Context)
{
	CIcPlayerClass::OnCharacterSpawned(Context);

	ResetUpgrades();
	ResetUpgradeLevel();
}

void CInfClassHuman::OnCharacterDamage(SDamageContext *pContext)
{
	switch(GetPlayerClass())
	{
	case EPlayerClass::Hero:
		if(pContext->Mode == TAKEDAMAGEMODE::INFECTION)
		{
			pContext->Mode = TAKEDAMAGEMODE::NOINFECTION;
			pContext->Damage = 12;
		}
		break;
	default:
		break;
	}

	if(pContext->DamageType == EDamageType::NINJA)
	{
		// Humans are immune to Ninja's force
		pContext->Force = vec2(0, 0);
	}
}

void CInfClassHuman::OnKilledCharacter(CIcCharacter *pVictim, const DeathContext &Context)
{
	if(!m_pCharacter)
		return;

	const bool Assisted = Context.Killer != GetCid();
	if(m_KillsProgression >= 0)
	{
		m_KillsProgression += Assisted ? 0.5f : 1.0f;
	}
	else
	{
		// Progression is disabled
	}

	switch(GetPlayerClass())
	{
	case EPlayerClass::Mercenary:
		if(!Assisted)
		{
			const int Bonus = GameController()->GetRoundType() == ERoundType::Survival ? 1 : 3;
			m_pCharacter->AddAmmo(WEAPON_LASER, Bonus);
		}
		break;
	case EPlayerClass::Ninja:
		if(pVictim->GetCid() == m_NinjaTargetCid)
		{
			OnNinjaTargetKiller(Assisted);
		}
		if(Context.DamageType == EDamageType::NINJA)
		{
			m_pCharacter->Heal(1);
		}
		break;
	case EPlayerClass::Medic:
		if(!Assisted)
		{
			m_pCharacter->AddAmmo(WEAPON_GRENADE, 1);
		}
		break;
	case EPlayerClass::Scientist:
		CheckSuperWeaponAccess();
		break;
	default:
		break;
	}
}

void CInfClassHuman::OnHumanHammerHitHuman(CIcCharacter *pTarget)
{
	if(GetPlayerClass() == EPlayerClass::Medic)
	{
		if(pTarget->GetPlayerClass() != EPlayerClass::Hero)
		{
			const int HadArmor = pTarget->GetArmor();
			if(pTarget->GiveArmor(4, GetCid()))
			{
				const int GivenArmor = pTarget->GetArmor() - HadArmor;
				Server()->RoundStatistics()->OnScoreEvent(GetCid(), EScoreEvent::HUMAN_HEALING,
					GetPlayerClass(), Server()->ClientName(GetCid()), GameServer()->Console(),
					GivenArmor);

				if(pTarget->GetArmor() == pTarget->GetMaxArmor())
				{
					GameServer()->SendScoreSound(GetCid());
					m_pCharacter->AddAmmo(WEAPON_GRENADE, 1);
				}
			}
		}
	}
	if(GetPlayerClass() == EPlayerClass::Biologist)
	{
		if(pTarget->IsPoisoned())
		{
			pTarget->ResetPoisonEffect();
		}
	}
}

void CInfClassHuman::OnHookAttachedPlayer()
{
	if(!m_pCharacter)
		return;

	if(m_pCharacter->IsPassenger())
		return;

	if(!GameController()->GetTaxiMode())
		return;

	CIcCharacter *pHookedCharacter = GameController()->GetCharacter(m_pCharacter->GetHookedPlayer());
	if(!pHookedCharacter || !pHookedCharacter->IsHuman())
		return;

	m_pCharacter->TryBecomePassenger(pHookedCharacter);
}

void CInfClassHuman::HandleNinja()
{
	if(GetPlayerClass() != EPlayerClass::Ninja)
		return;

	EInfclassWeapon Weapon = m_pCharacter->GetInfWeaponId(m_pCharacter->GetActiveWeapon());
	if(Weapon != EInfclassWeapon::NINJA_KATANA)
		return;

	m_pCharacter->m_DartLifeSpan--;

	auto &m_DartLifeSpan = m_pCharacter->m_DartLifeSpan;
	auto &m_DartDir = m_pCharacter->m_DartDir;
	auto &m_DartOldVelAmount = m_pCharacter->m_DartOldVelAmount;
	const float Force = GameController()->GetWeaponForce(Weapon);

	if(m_DartLifeSpan == 0)
	{
		// reset velocity
		m_pCharacter->SetVelocity(m_DartDir * m_DartOldVelAmount);
	}

	if(m_DartLifeSpan > 0)
	{
		vec2 OldPos = GetPos();
		// Set velocity
		float VelocityBuff = 1.0f + static_cast<float>(m_NinjaVelocityBuff) / 2.0f;
		m_pCharacter->HandleNinjaMove(g_pData->m_Weapons.m_Ninja.m_Velocity * VelocityBuff);

		// check if we Hit anything along the way
		vec2 NewPos = m_pCharacter->Core()->m_Pos;
		if(NewPos != OldPos)
		{
			// Find other players
			for(TEntityPtr<CIcCharacter> pTarget = GameWorld()->FindFirst<CIcCharacter>(); pTarget; ++pTarget)
			{
				if(m_apHitObjects.Capacity() == m_apHitObjects.Size())
				{
					break;
				}

				if(pTarget->IsHuman())
					continue;

				if(m_apHitObjects.Contains(pTarget))
					continue;

				vec2 IntersectPos;
				if(!closest_point_on_line(OldPos, NewPos, pTarget->GetPos(), IntersectPos))
					continue;

				float Len = distance(pTarget->GetPos(), IntersectPos);
				if(Len >= pTarget->GetProximityRadius() / 2 + GetProximityRadius() / 2)
				{
					continue;
				}

				// Hit a player, give him damage and stuffs...
				GameServer()->CreateSound(pTarget->GetPos(), SOUND_NINJA_HIT);
				// set his velocity to fast upward (for now)
				m_apHitObjects.Add(pTarget);

				pTarget->TakeDamage(vec2(0, -Force), minimum(g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage + m_NinjaExtraDamage, 20), GetCid(), EDamageType::NINJA);
			}
		}
	}
}

void CInfClassHuman::OnWeaponFired(WeaponFireContext *pFireContext)
{
	if(m_pPlayer->IsInfectionStarted())
	{
		pFireContext->FireAccepted = false;
		return;
	}

	const float ReloadIntervalModifier = m_WeaponReloadIntervalModifier[pFireContext->Weapon];
	pFireContext->ReloadInterval *= ReloadIntervalModifier;

	CIcPlayerClass::OnWeaponFired(pFireContext);
}

void CInfClassHuman::OnHammerFired(WeaponFireContext *pFireContext)
{
	switch(GetPlayerClass())
	{
	case EPlayerClass::Mercenary:
		if(GameController()->MercBombsEnabled())
		{
			FireMercenaryBomb(pFireContext);
			return;
		}
		else
		{
			break;
		}
	case EPlayerClass::Sniper:
		if(m_pCharacter->PositionIsLocked())
		{
			m_pCharacter->UnlockPosition();
		}
		else if(PositionLockAvailable())
		{
			m_pCharacter->LockPosition();
		}
		return;
	case EPlayerClass::Hero:
		PlaceTurret(pFireContext);
		return;
	case EPlayerClass::Engineer:
		PlaceEngineerWall(pFireContext);
		return;
	case EPlayerClass::Soldier:
		CSoldierBomb::OnFired(m_pCharacter, pFireContext);
		return;
	case EPlayerClass::Ninja:
		ActivateNinja(pFireContext);
		return;
	case EPlayerClass::Scientist:
		PlaceScientistMine(pFireContext);
		return;
	case EPlayerClass::Looper:
		PlaceLooperWall(pFireContext);
		return;
	default:
		break;
	}

	const vec2 ProjStartPos = GetProjectileStartPos(GetHammerProjOffset());

	// Lookup for humans
	ClientsArray Targets;
	GameController()->GetSortedTargetsInRange(ProjStartPos, GetHammerRange(), ClientsArray({GetCid()}), &Targets);

	int Hits = 0;
	for(const int TargetCid : Targets)
	{
		CIcCharacter *pTarget = GameController()->GetCharacter(TargetCid);
		if(pTarget->IsSolo())
			continue;

		if(GameServer()->Collision()->IntersectLine(ProjStartPos, pTarget->GetPos()))
			continue;

		if(pTarget->IsInfected())
		{
			vec2 Dir;
			if(length(pTarget->GetPos() - GetPos()) > 0.0f)
				Dir = normalize(pTarget->GetPos() - GetPos());
			else
				Dir = vec2(0.f, -1.f);

			float BaseForce = GameController()->GetWeaponForce(pFireContext->InfClassWeapon);
			vec2 Force = vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * BaseForce;

			int Damage = 20;
			if(GameController()->GetRoundType() == ERoundType::Survival)
			{
				Damage = 5;
			}

			pTarget->TakeDamage(Force, Damage, GetCid(), EDamageType::HAMMER);
		}
		else
		{
			OnHumanHammerHitHuman(pTarget);
		}

		Hits++;

		CreateHammerHit(ProjStartPos, pTarget);
	}

	// if we Hit anything, we have to wait for the reload
	if(Hits)
	{
		pFireContext->ReloadInterval = 0.33f;
	}

	if(pFireContext->FireAccepted)
	{
		GameServer()->CreateSound(GetPos(), SOUND_HAMMER_FIRE);
	}
}

void CInfClassHuman::OnGunFired(WeaponFireContext *pFireContext)
{
	if(pFireContext->NoAmmo)
		return;

	vec2 Direction = GetDirection();
	vec2 ProjStartPos = GetProjectileStartPos(GetProximityRadius() * 0.75f);

	EDamageType DamageType = EDamageType::GUN;
	ESound FireSound = SOUND_GUN_FIRE;

	if(pFireContext->InfClassWeapon == EInfclassWeapon::MERCENARY_GUN)
	{
		DamageType = EDamageType::MERCENARY_GUN;
		FireSound = SOUND_HOOK_LOOP;
	}

	int Damage = 1;
	float Force = GameController()->GetWeaponForce(pFireContext->InfClassWeapon);
	{
		[[maybe_unused]] CIcProjectile *pProj = new CIcProjectile(GameContext(), WEAPON_GUN,
			GetCid(),
			ProjStartPos,
			Direction,
			(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GunLifetime),
			Damage, Force, DamageType);
	}

	if(pFireContext->InfClassWeapon == EInfclassWeapon::MERCENARY_GUN)
	{
		float MaxSpeed = GameServer()->Tuning()->m_GroundControlSpeed * 1.7f;
		vec2 Recoil = Direction * (-MaxSpeed / 5.0f);
		m_pCharacter->SaturateVelocity(Recoil, MaxSpeed);
	}

	GameServer()->CreateSound(GetPos(), FireSound);
}

void CInfClassHuman::OnShotgunFired(WeaponFireContext *pFireContext)
{
	if(pFireContext->NoAmmo)
		return;

	vec2 Direction = GetDirection();
	vec2 ProjStartPos = GetProjectileStartPos(GetProximityRadius() * 0.75f);

	float Force = GameController()->GetWeaponForce(pFireContext->InfClassWeapon);
	float SpreadingValue = 0.07f;
	int ShotSpread = 3;
	EDamageType DamageType = EDamageType::SHOTGUN;

	switch(pFireContext->InfClassWeapon)
	{
	case EInfclassWeapon::RICOCHET_SHOTGUN:
		ShotSpread = 1;
		break;
	case EInfclassWeapon::MEDIC_SHOTGUN:
		DamageType = EDamageType::MEDIC_SHOTGUN;
		break;
	case EInfclassWeapon::ARTILLERY_SHOTGUN:
		ShotSpread = 4;
		break;
	default:
		break;
	}

	for(int i = -ShotSpread; i <= ShotSpread; ++i)
	{
		const float Spreading = i * SpreadingValue;
		float a = angle(Direction);
		a += Spreading * 2.0f * (0.25f + 0.75f * static_cast<float>(10 - pFireContext->AmmoAvailable) / 10.0f);
		float v = 1 - (absolute(i) / static_cast<float>(ShotSpread));
		float Speed = mix<float>(GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
		vec2 Direction = vec2(cosf(a), sinf(a)) * Speed;

		float LifeTime = GameServer()->Tuning()->m_ShotgunLifetime + 0.1f * static_cast<float>(pFireContext->AmmoAvailable)/10.0f;

		if(pFireContext->InfClassWeapon == EInfclassWeapon::RICOCHET_SHOTGUN)
		{
			[[maybe_unused]] CBouncingBullet *pProj = new CBouncingBullet(GameServer(),
				GetCid(),
				ProjStartPos,
				Direction);
		}
		else
		{
			int Damage = 1;
			[[maybe_unused]] CIcProjectile *pProj = new CIcProjectile(GameContext(), WEAPON_SHOTGUN,
				GetCid(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed() * LifeTime),
				Damage, Force, DamageType);
		}
	}

	GameServer()->CreateSound(GetPos(), SOUND_SHOTGUN_FIRE);
}

void CInfClassHuman::OnGrenadeFired(WeaponFireContext *pFireContext)
{
	switch(pFireContext->InfClassWeapon)
	{
	case EInfclassWeapon::POISON_GRENADE:
		// Does not need the ammo in some cases
		OnPoisonGrenadeFired(pFireContext);
		return;
	case EInfclassWeapon::TELEPORT_GUN:
		CLaserTeleport::OnFired(m_pCharacter, pFireContext, Config()->m_InfScientistTpSelfharm);
		return;
	case EInfclassWeapon::HEALING_GRENADE:
		OnMedicGrenadeFired(pFireContext);
		return;
	default:
		break;
	}

	if(pFireContext->NoAmmo)
		return;

	vec2 Direction = GetDirection();
	vec2 ProjStartPos = GetPos() + Direction * GetProximityRadius() * 0.75f;

	if(pFireContext->InfClassWeapon == EInfclassWeapon::NINJA_GRENADE)
	{
		CIcProjectile *pProj = CIcProjectile::MakeGrenade(GameContext(), ProjStartPos, Direction, GetCid(), EDamageType::STUNNING_GRENADE);
		pProj->SetFlashRadius(8);
	}
	else if(pFireContext->InfClassWeapon == EInfclassWeapon::ARTILLERY_GRENADE)
	{
		CArtilleryGrenade *pProj = new CArtilleryGrenade(GameContext(), (int)WEAPON_GRENADE, GetCid(), ProjStartPos, Direction * 2, (int)(Server()->TickSpeed() * GameServer()->Tuning()->m_GrenadeLifetime), 6, 0.f, EDamageType::ARTILLERY_GRENADE);
		pProj->SetExplosive(true);
		pProj->SetSoundImpact(SOUND_GRENADE_EXPLODE);
	}
	else
	{
		CIcProjectile::MakeGrenade(GameContext(), ProjStartPos, Direction, GetCid(), EDamageType::GRENADE);
	}

	GameServer()->CreateSound(GetPos(), SOUND_GRENADE_FIRE);
}

void CInfClassHuman::OnLaserFired(WeaponFireContext *pFireContext)
{
	if(pFireContext->NoAmmo)
	{
		return;
	}

	vec2 Direction = GetDirection();
	float StartEnergy = GameServer()->Tuning()->m_LaserReach * m_LaserReachModifier;
	int Damage = GameServer()->Tuning()->m_LaserDamage;

	switch(pFireContext->InfClassWeapon)
	{
	case EInfclassWeapon::BLINDING_LASER:
		CBlindingLaser::OnFired(m_pCharacter, pFireContext);
		return;
	case EInfclassWeapon::BIOLOGIST_MINE_LASER:
		CBiologistMine::OnFired(m_pCharacter, pFireContext, Config()->m_InfBioMineLasers);
		return;
	case EInfclassWeapon::EXPLOSIVE_LASER:
		StartEnergy *= 0.6f;
		new CScientistLaser(GameServer(), GetPos(), Direction, StartEnergy, GetCid(), Damage);
		break;
	case EInfclassWeapon::MERCENARY_UPGRADE_LASER:
		OnMercLaserFired(pFireContext);
		break;
	case EInfclassWeapon::MEDIC_LASER:
	case EInfclassWeapon::TRANQUILIZER_RIFLE:
		CMedicLaser::OnFired(m_pCharacter, pFireContext, StartEnergy);
		return;
	case EInfclassWeapon::LOOPER_LASER:
		StartEnergy *= 0.7f;
		Damage = 5;
		CIcLaser::MakeLaser(GameServer(), GetPos(), Direction, StartEnergy, GetCid(), Damage, pFireContext->InfClassWeapon);
		break;
	case EInfclassWeapon::SNIPER_RIFLE:
		Damage = m_pCharacter->PositionIsLocked() ? 30 : random_int(10, 13);
		CIcLaser::MakeLaser(GameServer(), GetPos(), Direction, StartEnergy, GetCid(), Damage, pFireContext->InfClassWeapon);
		break;
	case EInfclassWeapon::ENGINEER_LASER:
	case EInfclassWeapon::HERO_LASER:
		CIcLaser::MakeLaser(GameServer(), GetPos(), Direction, StartEnergy, GetCid(), Damage, pFireContext->InfClassWeapon);
		break;
	case EInfclassWeapon::ARTILLERY_LASER:
		new CArtilleryLaser(GameServer(), (int)WEAPON_LASER, GetCid(), GetPos() + Direction * GetProximityRadius() * 0.75f, Direction, (int)(Server()->TickSpeed() * GameServer()->Tuning()->m_GrenadeLifetime), 0, 0.f, EDamageType::ARTILLERY_LASER, m_HasAirStrike);
		break;
	default:
		break;
	}

	if(pFireContext->FireAccepted)
	{
		GameServer()->CreateSound(GetPos(), SOUND_LASER_FIRE);
	}
}

void CInfClassHuman::GiveClassAttributes()
{
	m_ResetKillsTick = -1;
	m_TurretCount = 0;
	m_NinjaTargetTick = 0;
	m_NinjaTargetCid = -1;
	m_NinjaVelocityBuff = 0;
	m_NinjaExtraDamage = 0;
	m_NinjaAmmoBuff = 0;

	RemoveWhiteHole();
	RemoveAirStrike();

	CIcPlayerClass::GiveClassAttributes();

	if(!m_pCharacter)
	{
		return;
	}

	switch(GetPlayerClass())
	{
	case EPlayerClass::Engineer:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_LASER);
		break;
	case EPlayerClass::Soldier:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_GRENADE);
		break;
	case EPlayerClass::Mercenary:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		if(GameController()->MercBombsEnabled())
			m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_GUN);
		break;
	case EPlayerClass::Sniper:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_LASER);
		break;
	case EPlayerClass::Scientist:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_LASER);
		break;
	case EPlayerClass::Biologist:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->GiveWeapon(WEAPON_SHOTGUN, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_SHOTGUN);
		break;
	case EPlayerClass::Looper:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_LASER);
		break;
	case EPlayerClass::Medic:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_SHOTGUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_SHOTGUN);
		break;
	case EPlayerClass::Hero:
		if(GameController()->AreTurretsEnabled())
			m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_SHOTGUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_GRENADE);
		break;
	case EPlayerClass::Ninja:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		if(GameController()->GetRoundType() == ERoundType::Survival)
		{
			// Increase the velocity
			m_NinjaVelocityBuff = 1;

			// Set the total damage to 10
			m_NinjaExtraDamage = 1;

			// Give two extra grenades
			m_NinjaAmmoBuff = 2;
		}
		break;
	case EPlayerClass::Artillery:
		m_pCharacter->GiveWeapon(WEAPON_SHOTGUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_SHOTGUN);
		break;
	case EPlayerClass::None:
		m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		m_pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	default:
		break;
	}

	if(GetPlayerClass() == EPlayerClass::Sniper)
	{
		m_PositionLockTicksRemaining = Server()->TickSpeed() * s_SniperPositionLockTimeLimit;
	}
	else
	{
		m_PositionLockTicksRemaining = 0;
	}

	SpawnChildEntities();

	m_KillsProgression = 0;
	m_pCharacter->UnlockPosition();
}

void CInfClassHuman::SpawnChildEntities()
{
	if(GetPlayerClass() == EPlayerClass::Hero)
	{
		if(!m_pHeroFlag)
			m_pHeroFlag = new CHeroFlag(GameServer(), m_pPlayer->GetCid());
	}
}

void CInfClassHuman::DestroyChildEntities()
{
	m_PositionLockTicksRemaining = 0;
	m_NinjaTargetTick = 0;
	m_NinjaTargetCid = -1;

	if(m_pHeroFlag)
	{
		// The flag removed in CIcCharacter::DestroyChildEntities()
		// delete m_pHeroFlag;
		m_pHeroFlag = nullptr;
	}

	CIcPlayerClass::DestroyChildEntities();

	if(!m_pCharacter)
	{
		return;
	}

	m_pCharacter->UnlockPosition();
}

void CInfClassHuman::BroadcastWeaponState() const
{
	const int CurrentTick = Server()->Tick();
	int ClientVersion = Server()->GetClientInfclassVersion(GetCid());
	const EInfclassWeapon Weapon = m_pCharacter->GetInfWeaponId();

	switch(Weapon)
	{
	case EInfclassWeapon::MEDIC_LASER:
	{
		int MinimumHP = Config()->m_InfRevivalDamage + 1;
		int MinimumInfected = GameController()->MinimumInfectedForRevival();

		if(m_pCharacter->GetHealthArmorSum() < MinimumHP)
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("You need at least {int:MinHp} HP to revive a zombie"),
				"MinHp", &MinimumHP,
				NULL);
		}
		else if(GameController()->GetInfectedCount() < MinimumInfected)
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("Too few zombies to revive anyone (less than {int:MinZombies})"),
				"MinZombies", &MinimumInfected,
				NULL);
		}
		return;
	}
	case EInfclassWeapon::MERCENARY_UPGRADE_LASER:
	{
		CMercenaryBomb *pCurrentBomb = nullptr;
		for(TEntityPtr<CMercenaryBomb> pBomb = GameWorld()->FindFirst<CMercenaryBomb>(); pBomb; ++pBomb)
		{
			if(pBomb->GetOwner() == m_pPlayer->GetCid())
			{
				pCurrentBomb = pBomb;
				break;
			}
		}

		if(pCurrentBomb)
		{
			float BombLevel = pCurrentBomb->GetLoad() / static_cast<float>(Config()->m_InfMercBombs);

			if(BombLevel < 1.0)
			{
				dynamic_string Line1;
				Server()->Localization()->Format_L(Line1, GetPlayer()->GetLanguage(),
					_C("Mercenary", "Use the laser to upgrade the bomb"), NULL);

				dynamic_string Line2;
				Server()->Localization()->Format_L(Line2, GetPlayer()->GetLanguage(),
					_C("Mercenary", "Explosive yield: {percent:BombLevel}"), "BombLevel", &BombLevel, NULL);

				Line1.append("\n");
				Line1.append(Line2);

				GameServer()->AddBroadcast(GetPlayer()->GetCid(), Line1.buffer(),
					EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME);
			}
			else
			{
				GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
					EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
					_C("Mercenary", "The bomb is fully upgraded.\n"
									"There is nothing to do with the laser."),
					NULL);
			}
		}
		else
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_C("Mercenary", "Use the hammer to place a bomb and\n"
								"then use the laser to upgrade it"),
				NULL);
		}

		return;
	}
	case EInfclassWeapon::LASER_TURRET:
	case EInfclassWeapon::PLASMA_TURRET:
	{
		int Turrets = m_TurretCount;
		if(!GameController()->AreTurretsEnabled())
		{
			GameServer()->SendBroadcast_Localization(GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("The turrets are not allowed by the game rules (at least right now)."),
				nullptr);
		}
		else if(Turrets > 0)
		{
			int MaxTurrets = Config()->m_InfTurretMaxPerPlayer;
			if(MaxTurrets == 1)
			{
				GameServer()->SendBroadcast_Localization(GetCid(),
					EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
					_("You have a turret. Use the hammer to place it."),
					nullptr
					);
			}
			else
			{
				GameServer()->SendBroadcast_Localization_P(GetCid(),
					EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME, Turrets,
					_("You have {int:NumTurrets} of {int:MaxTurrets} turrets. Use the hammer to place one."),
					"NumTurrets", &Turrets,
					"MaxTurrets", &MaxTurrets,
					nullptr
					);
			}
		}
		else
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("You don't have a turret to place"),
				nullptr
				);
		}
		return;
	}
	default:
		break;
	}

	if(GetPlayerClass() == EPlayerClass::Engineer)
	{
		if(ClientVersion >= VERSION_INFC_160)
			return;

		CEngineerWall *pOwnWall = nullptr;
		for(TEntityPtr<CEngineerWall> pWall = GameWorld()->FindFirst<CEngineerWall>(); pWall; ++pWall)
		{
			if(pWall->GetOwner() == GetCid())
			{
				pOwnWall = pWall;
				break;
			}
		}

		if(pOwnWall && pOwnWall->HasSecondPosition() && pOwnWall->GetEndTick().has_value())
		{
			int Seconds = pOwnWall->GetLifespan() + 1;
			GameServer()->SendBroadcast_Localization(GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("Laser wall: {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				nullptr
			);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Looper)
	{
		if(ClientVersion >= VERSION_INFC_160)
			return;

		CLooperWall* pOwnWall = nullptr;
		for(TEntityPtr<CLooperWall> pWall = GameWorld()->FindFirst<CLooperWall>(); pWall; ++pWall)
		{
			if(pWall->GetOwner() == m_pPlayer->GetCid())
			{
				pOwnWall = pWall;
				break;
			}
		}

		if(pOwnWall && pOwnWall->HasSecondPosition())
		{
			int Seconds = pOwnWall->GetLifespan() + 1;
			GameServer()->SendBroadcast_Localization(GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("Looper laser wall: {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				nullptr
			);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Soldier)
	{
		int NumBombs = 0;
		for(TEntityPtr<CSoldierBomb> pBomb = GameWorld()->FindFirst<CSoldierBomb>(); pBomb; ++pBomb)
		{
			if(pBomb->GetOwner() == GetCid())
			{
				NumBombs += pBomb->GetNbBombs();
			}
		}

		if(NumBombs)
		{
			GameServer()->SendBroadcast_Localization_P(GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				NumBombs,
				_CP("Soldier", "{int:NumBombs} bomb left", "{int:NumBombs} bombs left", NumBombs),
				"NumBombs", &NumBombs,
				NULL
			);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Scientist)
	{
		int NumMines = 0;
		for(TEntityPtr<CScientistMine> pMine = GameWorld()->FindFirst<CScientistMine>(); pMine; ++pMine)
		{
			if(pMine->GetOwner() == m_pPlayer->GetCid())
				NumMines++;
		}

		CWhiteHole *pOwnWhiteHole = nullptr;
		for(TEntityPtr<CWhiteHole> pWhiteHole = GameWorld()->FindFirst<CWhiteHole>(); pWhiteHole; ++pWhiteHole)
		{
			if(pWhiteHole->GetOwner() == m_pPlayer->GetCid())
			{
				pOwnWhiteHole = pWhiteHole;
				break;
			}
		}

		if(m_BroadcastWhiteHoleReady + (2 * Server()->TickSpeed()) > Server()->Tick())
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("The white hole is available!"),
				NULL
			);
		}
		else if(m_BroadcastAirStrikeReady + (2 * Server()->TickSpeed()) > Server()->Tick())
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("The airstrike is available!"),
				NULL
			);
		}
		else if(NumMines > 0 && !pOwnWhiteHole)
		{
			GameServer()->SendBroadcast_Localization_P(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME, NumMines,
				_P("{int:NumMines} mine is active", "{int:NumMines} mines are active", NumMines),
				"NumMines", &NumMines,
				NULL
			);
		}
		else if(NumMines <= 0 && pOwnWhiteHole)
		{
			int Seconds = 1 + pOwnWhiteHole->GetLifespan();
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("White hole: {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				NULL
			);
		}
		else if(NumMines > 0 && pOwnWhiteHole)
		{
			int Seconds = 1 + pOwnWhiteHole->GetLifespan();
			dynamic_string Buffer;
			Server()->Localization()->Format_LP(Buffer, GetPlayer()->GetLanguage(), NumMines,
				_P("{int:NumMines} mine is active", "{int:NumMines} mines are active", NumMines),
				"NumMines", &NumMines,
				nullptr);
			Buffer.append("\n");
			Server()->Localization()->Format_L(Buffer, GetPlayer()->GetLanguage(),
				_("White hole: {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				nullptr);
			GameServer()->SendBroadcast(GetCid(), Buffer.buffer(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Biologist)
	{
		int NumMines = 0;
		for(TEntityPtr<CBiologistMine> pMine = GameWorld()->FindFirst<CBiologistMine>(); pMine; ++pMine)
		{
			if(pMine->GetOwner() == m_pPlayer->GetCid())
				NumMines++;
		}

		if(NumMines > 0)
		{
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_C("Biologist", "Mine activated"),
				NULL
			);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Ninja)
	{
		int TargetId = m_NinjaTargetCid;
		int CoolDown = m_NinjaTargetTick - Server()->Tick();

		if((CoolDown > 0))
		{
			int Seconds = 1 + CoolDown / Server()->TickSpeed();
			GameServer()->SendBroadcast_Localization(GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_C("Ninja", "Next target in {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				NULL
			);
		}
		else if(TargetId >= 0)
		{
			GameServer()->SendBroadcast_Localization(GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_C("Ninja", "Target to eliminate: {str:PlayerName}"),
				"PlayerName", Server()->ClientName(TargetId),
				NULL
			);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Sniper)
	{
		if(m_pCharacter->PositionIsLocked())
		{
			int Seconds = 1+m_PositionLockTicksRemaining/Server()->TickSpeed();
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_C("Sniper", "Position lock: {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				NULL
			);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Mercenary)
	{
		CMercenaryBomb *pOwnBomb = nullptr;
		for(TEntityPtr<CMercenaryBomb> pBomb = GameWorld()->FindFirst<CMercenaryBomb>(); pBomb; ++pBomb)
		{
			if(pBomb->GetOwner() == m_pPlayer->GetCid())
			{
				pOwnBomb = pBomb;
				break;
			}
		}

		if(pOwnBomb)
		{
			float BombLevel = pOwnBomb->GetLoad() / static_cast<float>(Config()->m_InfMercBombs);
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_C("Mercenary", "Explosive yield: {percent:BombLevel}"),
				"BombLevel", &BombLevel,
				NULL);
		}
	}
	else if(GetPlayerClass() == EPlayerClass::Hero)
	{
		//Search for flag
		int CoolDown = m_pHeroFlag ? m_pHeroFlag->GetSpawnTick() - CurrentTick : 0;

		if(CoolDown > 0 && (ClientVersion < VERSION_INFC_140)) // 140 introduces native timers for Hero
		{
			int Seconds = 1 + CoolDown / Server()->TickSpeed();
			GameServer()->SendBroadcast_Localization(GetPlayer()->GetCid(),
				EBroadcastPriority::WEAPONSTATE, BROADCAST_DURATION_REALTIME,
				_("Next flag in {sec:RemainingTime}"),
				"RemainingTime", &Seconds,
				nullptr
			);
		}
	}
}

void CInfClassHuman::ResetUpgrades()
{
	for(float &Modifier : m_WeaponRegenIntervalModifier)
	{
		Modifier = 1.f;
	}

	for(float &Modifier : m_WeaponReloadIntervalModifier)
	{
		Modifier = 1.f;
	}

	m_LaserReachModifier = 1.0f;
}

void CInfClassHuman::OnNinjaTargetKiller(bool Assisted)
{
	GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("You have eliminated your target, +2 points"), NULL);
	Server()->RoundStatistics()->OnScoreEvent(GetCid(), EScoreEvent::KILL_TARGET, GetPlayerClass(), Server()->ClientName(GetCid()), GameServer()->Console());

	if(m_pCharacter)
	{
		GiveNinjaBuf();
	}

	int PlayerCounter = Server()->GetActivePlayerCount();
	int CooldownTicks = Server()->TickSpeed() * (10 + 3 * maximum(0, 16 - PlayerCounter));

	m_NinjaTargetCid = -1;
	m_NinjaTargetTick = Server()->Tick() + CooldownTicks;
}

void CInfClassHuman::GiveNinjaBuf()
{
	switch(random_int(0, 2))
	{
	case 0: //Velocity Buff
		m_NinjaVelocityBuff++;
		GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("Sword velocity increased"), NULL);
		break;
	case 1: //Strength Buff
		m_NinjaExtraDamage++;
		GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("Sword strength increased"), NULL);
		break;
	case 2: //Ammo Buff
		m_NinjaAmmoBuff++;
		GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("Grenade limit increased"), NULL);
		break;
	}

	m_pCharacter->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
	GameServer()->SendEmoticon(GetCid(), EMOTICON_MUSIC);
}

void CInfClassHuman::SnapHero(int SnappingClient)
{
	if(SnappingClient != m_pPlayer->GetCid())
		return;

	const int CurrentTick = Server()->Tick();

	if(m_pHeroFlag && m_pHeroFlag->IsAvailable() && Config()->m_InfHeroFlagIndicator)
	{
		const float FlagIndicatorTime = GameController()->GetRoundType() == ERoundType::Survival ? 1 : Config()->m_InfHeroFlagIndicatorTime;
		int TickLimit = m_pPlayer->m_LastActionMoveTick + FlagIndicatorTime * Server()->TickSpeed();
		TickLimit = maximum(TickLimit, m_pHeroFlag->GetSpawnTick());

		if(CurrentTick > TickLimit)
		{
			CNetObj_Laser *pObj = Server()->SnapNewItem<CNetObj_Laser>(m_pCharacter->GetCursorId());
			if(!pObj)
				return;

			float Angle = atan2f(m_pHeroFlag->GetPos().y - GetPos().y, m_pHeroFlag->GetPos().x - GetPos().x);
			vec2 vecDir = vec2(cos(Angle), sin(Angle));
			vec2 Indicator = GetPos() + vecDir * 84.0f;
			vec2 IndicatorM = GetPos() - vecDir * 84.0f;

			// display laser beam for 0.5 seconds
			int TickShowBeamTime = Server()->TickSpeed() * 0.5;
			long TicksInactive = TickShowBeamTime - (Server()->Tick() - TickLimit);
			if(g_Config.m_InfHeroFlagIndicatorTime > 0 && TicksInactive > 0)
			{
				Indicator = IndicatorM + vecDir * 168.0f * (1.0f - (TicksInactive / (float)TickShowBeamTime));

				pObj->m_X = (int)Indicator.x;
				pObj->m_Y = (int)Indicator.y;
				pObj->m_FromX = (int)IndicatorM.x;
				pObj->m_FromY = (int)IndicatorM.y;
				if(TicksInactive < 4)
				{
					pObj->m_StartTick = Server()->Tick() - (6 - TicksInactive);
				}
				else
				{
					pObj->m_StartTick = Server()->Tick() - 3;
				}
			}
			else
			{
				pObj->m_X = (int)Indicator.x;
				pObj->m_Y = (int)Indicator.y;
				pObj->m_FromX = pObj->m_X;
				pObj->m_FromY = pObj->m_Y;
				pObj->m_StartTick = Server()->Tick();
			}
		}
	}
}

void CInfClassHuman::SnapScientist(int SnappingClient)
{
	if(SnappingClient != m_pPlayer->GetCid())
		return;

	if(m_pCharacter->GetActiveWeapon() == WEAPON_GRENADE)
	{
		const std::optional<vec2> PortalPos = CLaserTeleport::FindPortalPosition(m_pCharacter);

		if(PortalPos.has_value())
		{
			const int CursorId = GameController()->GetPlayerOwnCursorId(GetCid());
			GameController()->SendHammerDot(PortalPos.value(), CursorId);
		}
	}
}

void CInfClassHuman::ActivateNinja(WeaponFireContext *pFireContext)
{
	if(m_pCharacter->m_DartLeft || m_pCharacter->m_InWater)
	{
		if(!m_pCharacter->m_InWater)
			m_pCharacter->m_DartLeft--;

		m_apHitObjects.Clear();

		m_pCharacter->m_DartDir = GetDirection();
		m_pCharacter->m_DartLifeSpan = g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000;
		m_pCharacter->m_DartOldVelAmount = length(m_pCharacter->Velocity());

		GameServer()->CreateSound(GetPos(), SOUND_NINJA_HIT);
	}
}

void CInfClassHuman::PlaceEngineerWall(WeaponFireContext *pFireContext)
{
	TEntityPtr<CEngineerWall> pExistingWall;
	for(TEntityPtr<CEngineerWall> pWall = GameWorld()->FindFirst<CEngineerWall>(); pWall; ++pWall)
	{
		if(pWall->GetOwner() == GetCid())
		{
			if(pWall->HasSecondPosition())
			{
				GameWorld()->DestroyEntity(pWall);
			}
			else
			{
				pExistingWall = pWall;
			}
			break;
		}
	}

	if(!pExistingWall)
	{
		pExistingWall = new CEngineerWall(GameServer(), GetPos(), GetCid());
	}
	else if(distance(pExistingWall->GetPos(), GetPos()) > 10.0)
	{
		vec2 FirstPos = pExistingWall->GetPos();
		for(int i = 0; i < 15; i++)
		{
			vec2 TestPos = FirstPos + (GetPos() - FirstPos) * (static_cast<float>(i) / 14.0f);
			if(!GameController()->HumanWallAllowedInPos(TestPos))
			{
				pFireContext->FireAccepted = false;
				break;
			}
		}

		if(pFireContext->FireAccepted)
		{
			pExistingWall->SetSecondPosition(GetPos());
			pExistingWall->SetLifespan(Config()->m_InfBarrierLifeSpan);
			GameServer()->CreateSound(GetPos(), SOUND_LASER_FIRE);
		}
		else
		{
			GameWorld()->DestroyEntity(pExistingWall);
		}
	}
}

void CInfClassHuman::PlaceLooperWall(WeaponFireContext *pFireContext)
{
	TEntityPtr<CLooperWall> pExistingWall;
	for(TEntityPtr<CLooperWall> pWall = GameWorld()->FindFirst<CLooperWall>(); pWall; ++pWall)
	{
		if(pWall->GetOwner() == GetCid())
		{
			if(pWall->HasSecondPosition())
			{
				GameWorld()->DestroyEntity(pWall);
			}
			else
			{
				pExistingWall = pWall;
			}
			break;
		}
	}

	if(!pExistingWall)
	{
		pExistingWall = new CLooperWall(GameServer(), GetPos(), GetCid());
	}
	else if(distance(pExistingWall->GetPos(), GetPos()) > 10.0)
	{
		vec2 FirstPos = pExistingWall->GetPos();
		for(int i = 0; i < 15; i++)
		{
			vec2 TestPos = FirstPos + (GetPos() - FirstPos) * (static_cast<float>(i) / 14.0f);
			if(!GameController()->HumanWallAllowedInPos(TestPos))
			{
				pFireContext->FireAccepted = false;
				break;
			}
		}

		if(pFireContext->FireAccepted)
		{
			pExistingWall->SetSecondPosition(GetPos());
			float LifeSpanFactor = 1.0f;
			if(GameController()->GetRoundType() == ERoundType::Survival)
			{
				LifeSpanFactor *= 0.5f;
			}
			pExistingWall->SetLifespan(Config()->m_InfLooperBarrierLifeSpan * LifeSpanFactor);
			GameServer()->CreateSound(GetPos(), SOUND_LASER_FIRE);
		}
		else
		{
			GameWorld()->DestroyEntity(pExistingWall);
		}
	}
}

void CInfClassHuman::FireMercenaryBomb(WeaponFireContext *pFireContext)
{
	CMercenaryBomb *pOwnBomb = nullptr;
	for(TEntityPtr<CMercenaryBomb> pBomb = GameWorld()->FindFirst<CMercenaryBomb>(); pBomb; ++pBomb)
	{
		if(pBomb->GetOwner() == GetCid())
		{
			pOwnBomb = pBomb;
			break;
		}
	}

	if(pOwnBomb)
	{
		float Distance = distance(pOwnBomb->GetPos(), GetPos());
		const float SafeDistance = 16;
		if(pOwnBomb->IsReadyToExplode() || Distance > pOwnBomb->GetProximityRadius() + SafeDistance)
		{
			pOwnBomb->Explode(GetCid());
		}
		else
		{
			const float UpgradePoints = Distance <= pOwnBomb->GetProximityRadius() ? 2 : 0.5;
			UpgradeMercBomb(pOwnBomb, UpgradePoints);
		}
	}
	else
	{
		new CMercenaryBomb(GameServer(), GetPos(), GetCid());
	}

	pFireContext->ReloadInterval = 0.25f;
}

void CInfClassHuman::PlaceScientistMine(WeaponFireContext *pFireContext)
{
	vec2 Direction = GetDirection();
	vec2 ProjStartPos = GetPos() + Direction * GetProximityRadius() * 0.75f;

	bool FreeSpace = true;
	int NbMine = 0;

	int OlderMineTick = Server()->Tick() + 1;
	CScientistMine *pOlderMine = nullptr;
	CScientistMine *pIntersectMine = nullptr;

	for(TEntityPtr<CScientistMine> p = GameWorld()->FindFirst<CScientistMine>(); p; ++p)
	while(p)
	{
		float d = distance(p->GetPos(), ProjStartPos);

		if(p->GetOwner() == GetCid())
		{
			if(OlderMineTick > p->m_StartTick)
			{
				OlderMineTick = p->m_StartTick;
				pOlderMine = p;
			}
			NbMine++;

			if(d < 2.0f * g_Config.m_InfMineRadius)
			{
				if(pIntersectMine)
					FreeSpace = false;
				else
					pIntersectMine = p;
			}
		}
		else if(d < 2.0f * g_Config.m_InfMineRadius)
			FreeSpace = false;

		p = (CScientistMine *)p->TypeNext();
	}

	if(!FreeSpace)
		return;

	if(pIntersectMine) // Move the mine
		GameWorld()->DestroyEntity(pIntersectMine);
	else if(NbMine >= g_Config.m_InfMineLimit && pOlderMine)
		GameWorld()->DestroyEntity(pOlderMine);

	new CScientistMine(GameServer(), ProjStartPos, GetCid());
	pFireContext->ReloadInterval = 0.5f;
}

void CInfClassHuman::PlaceTurret(WeaponFireContext *pFireContext)
{
	if(GameController()->AreTurretsEnabled() && m_TurretCount)
	{
		CTurret *pTurret{};
		if(pFireContext->InfClassWeapon == EInfclassWeapon::LASER_TURRET)
		{
			pTurret = new CTurret(GameServer(), GetPos(), GetCid(), CTurret::LASER);
		}
		else if(pFireContext->InfClassWeapon == EInfclassWeapon::PLASMA_TURRET)
		{
			pTurret = new CTurret(GameServer(), GetPos(), GetCid(), CTurret::PLASMA);
		}
		else
		{
			dbg_assert(false, "Unknown Turret weapon type");
			return;
		}
		pTurret->SetLifespan(GameController()->InfTurretDuration());

		GameServer()->CreateSound(GetPos(), SOUND_GRENADE_FIRE);
		m_TurretCount--;
		int TurretsNumber = m_TurretCount;
		GameServer()->SendChatTarget_Localization_P(GetCid(), CHATCATEGORY_SCORE, TurretsNumber,
			_P("Placed a turret, {int:TurretsNumber} turret left", "Placed a turret, {int:TurretsNumber} turrets left", TurretsNumber),
			"TurretsNumber", &TurretsNumber,
			nullptr);
	}
	else
	{
		pFireContext->NoAmmo = true;
	}
}

void CInfClassHuman::OnPoisonGrenadeFired(WeaponFireContext *pFireContext)
{
	float BaseAngle = angle(GetDirection());

	// Find grenades
	bool GrenadeFound = false;

	for(TEntityPtr<CScatterGrenade> pGrenade = GameWorld()->FindFirst<CScatterGrenade>(); pGrenade; ++pGrenade)
	{
		if(pGrenade->GetOwner() != GetCid())
			continue;
		pGrenade->Explode();
		GrenadeFound = true;
	}

	if(GrenadeFound)
	{
		pFireContext->AmmoConsumed = 0;
		pFireContext->NoAmmo = false;
		return;
	}

	if(pFireContext->NoAmmo)
		return;

	int ShotSpread = 2;

	for(int i = -ShotSpread; i <= ShotSpread; ++i)
	{
		float a = BaseAngle + random_float() / 3.0f;

		[[maybe_unused]] CScatterGrenade *pProj = new CScatterGrenade(GameServer(), GetCid(), GetPos(), vec2(cosf(a), sinf(a)));
	}

	GameServer()->CreateSound(GetPos(), SOUND_GRENADE_FIRE);

	pFireContext->ReloadInterval = 0.25f;
}

void CInfClassHuman::OnMedicGrenadeFired(WeaponFireContext *pFireContext)
{
	if(pFireContext->NoAmmo)
		return;

	int HealingExplosionRadius = 4;
	new CGrowingExplosion(GameServer(), GetPos(), GetDirection(), GetCid(), HealingExplosionRadius, EGrowingExplosionEffect::HEAL_HUMANS);

	GameServer()->CreateSound(GetPos(), SOUND_GRENADE_FIRE);
}

void CInfClassHuman::OnMercLaserFired(WeaponFireContext *pFireContext)
{
	CMercenaryBomb *pCurrentBomb = nullptr;
	for(TEntityPtr<CMercenaryBomb> pBomb = GameWorld()->FindFirst<CMercenaryBomb>(); pBomb; ++pBomb)
	{
		if(pBomb->GetOwner() == GetCid())
		{
			pCurrentBomb = pBomb;
			break;
		}
	}

	if(!pCurrentBomb)
	{
		pFireContext->FireAccepted = false;
	}
	else
	{
		float UpgradePoints = 1.5f;
		new CMercenaryLaser(GameServer(), GetPos(), GetDirection(), GameServer()->Tuning()->m_LaserReach, GetCid(), UpgradePoints);
	}
}

bool CInfClassHuman::PositionLockAvailable() const
{
	const int TickSpeed = GameContext()->Server()->TickSpeed();
	if(m_PositionLockTicksRemaining < TickSpeed * 1.0f)
	{
		return false;
	}

	if(GetPos().y <= -600)
	{
		return false;
	}

	if(m_pCharacter->IsPassenger())
	{
		return false;
	}

	return true;
}

void CInfClassHuman::OnSlimeEffect(int Owner, int Damage, float DamageInterval)
{
	if(GetPlayerClass() == EPlayerClass::Biologist)
	{
		// Note: actually probably the character 'll stay in the slime for
		// more than 1 tick and it 'll result in 2 damage dealt
		Damage = 1;
	}
	m_pCharacter->Poison(Damage, Owner, EDamageType::SLUG_SLIME, DamageInterval);
}

bool CInfClassHuman::HasWhiteHole() const
{
	return m_HasWhiteHole;
}

void CInfClassHuman::GiveWhiteHole()
{
	m_HasWhiteHole = true;
	m_BroadcastWhiteHoleReady = Server()->Tick();
	GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("The white hole is ready, use the laser rifle to disrupt space-time"), nullptr);
}

void CInfClassHuman::RemoveWhiteHole()
{
	m_HasWhiteHole = false;

	if(m_pCharacter)
	{
		m_pCharacter->SetSuperWeaponIndicatorEnabled(false);
	}
}

bool CInfClassHuman::HasAirStrike() const
{
	return m_HasAirStrike;
}

void CInfClassHuman::GiveAirStrike()
{
	m_HasAirStrike = true;
	m_BroadcastAirStrikeReady = Server()->Tick();
	GameServer()->SendChatTarget_Localization(GetCid(), CHATCATEGORY_SCORE, _("The airstrike is ready, use the laser rifle to spawn it"), nullptr);
}

void CInfClassHuman::RemoveAirStrike()
{
	m_HasAirStrike = false;

	if(m_pCharacter)
	{
		m_pCharacter->SetSuperWeaponIndicatorEnabled(false);
	}
}

void CInfClassHuman::UpgradeMercBomb(CMercenaryBomb *pBomb, float UpgradePoints)
{
	float Load = pBomb->GetLoad();
	float NewLoad = minimum<float>(Config()->m_InfMercBombs, Load + UpgradePoints);
	pBomb->SetLoad(NewLoad);
}

void CInfClassHuman::OnHeroFlagTaken(CIcCharacter *pHero)
{
	if(!m_pCharacter)
		return;

	m_pCharacter->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
	GameServer()->SendEmoticon(GetCid(), EMOTICON_MUSIC);

	if(pHero != m_pCharacter)
	{
		GiveGift(EGiftType::HeroFlag);
		return;
	}

	{
		// Gift to self
		m_pCharacter->SetHealthArmor(10, 10);
		m_pCharacter->GiveWeapon(WEAPON_GUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_SHOTGUN, -1);
		m_pCharacter->GiveWeapon(WEAPON_GRENADE, -1);
		m_pCharacter->GiveWeapon(WEAPON_LASER, -1);

		if(GameController()->AreTurretsEnabled())
		{
			int NewNumberOfTurrets = clamp<int>(m_TurretCount + Config()->m_InfTurretGive, 0, Config()->m_InfTurretMaxPerPlayer);
			if(m_TurretCount != NewNumberOfTurrets)
			{
				if(m_TurretCount == 0)
					m_pCharacter->GiveWeapon(WEAPON_HAMMER, -1);

				m_TurretCount = NewNumberOfTurrets;

				GameServer()->SendChatTarget_Localization_P(GetCid(), CHATCATEGORY_SCORE, m_TurretCount,
					_P("You have {int:NumTurrets} turret available, use the Hammer to place it",
						"You have {int:NumTurrets} turrets available, use the Hammer to place it", m_TurretCount),
					"NumTurrets", &m_TurretCount,
					nullptr);
			}
		}
	}

	// Only increase your *own* character health when on cooldown
	if(!GameController()->HeroGiftAvailable())
		return;

	GameController()->OnHeroFlagCollected(GetCid());

	// Find other players
	for(TEntityPtr<CIcCharacter> p = GameWorld()->FindFirst<CIcCharacter>(); p; ++p)
	{
		if(p->IsInfected() || p == m_pCharacter)
			continue;

		CInfClassHuman *pHumanClass = CInfClassHuman::GetInstance(p);
		pHumanClass->OnHeroFlagTaken(m_pCharacter);
	}
}

void CInfClassHuman::OnWhiteHoleSpawned(CWhiteHole *pWhiteHole)
{
	pWhiteHole->SetLifespan(GameController()->GetWhiteHoleLifeSpan());

	m_KillsProgression = -1;
	m_ResetKillsTick = pWhiteHole->GetEndTick().value() + Server()->TickSpeed() * 3;
}

void CInfClassHuman::GiveUpgrade()
{
	m_UpgradeLevel++;
}
