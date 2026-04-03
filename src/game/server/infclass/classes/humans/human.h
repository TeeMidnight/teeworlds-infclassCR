#ifndef GAME_SERVER_INFCLASS_CLASSES_HUMAN_H
#define GAME_SERVER_INFCLASS_CLASSES_HUMAN_H

#include "../ic_playerclass.h"

#include <base/tl/ic_array.h>
#include <game/server/alloc.h>

class CHeroFlag;
class CMercenaryBomb;
class CWhiteHole;

enum class EGiftType
{
	BonusZone,
	HeroFlag,
};

enum class EUpgradeType;

class CInfClassHuman : public CIcPlayerClass
{
	MACRO_ALLOC_POOL_ID()

public:
	explicit CInfClassHuman(CIcPlayer *pPlayer);

	static CInfClassHuman *GetInstance(CIcPlayer *pPlayer);
	static CInfClassHuman *GetInstance(CIcCharacter *pCharacter);
	static CInfClassHuman *GetInstance(CIcPlayerClass *pClass);

	bool IsHuman() const final { return true; }

	SkinGetter GetSkinGetter() const override;
	void SetupSkinContext(CSkinContext *pOutput, bool ForSameTeam) const override;
	static bool SetupSkin(const CSkinContext &Context, CWeakSkinInfo *pOutput, int DDNetVersion, int InfClassVersion);

	CAmmoParams GetAmmoParams(int Weapon) const override;
	int GetJumps() const override;

	void GiveGift(EGiftType GiftType, int Level = 0);

	bool CanBeHit() const override;
	PlayerUpgradesArray GetUpgrade(int Level) const override;
	void OnPlayerClassChanged() override;

	void OnPlayerSnap(int SnappingClient, int InfClassVersion) override;

	void OnCharacterPreCoreTick() override;
	void OnCharacterTick() override;
	void OnCharacterTickPaused() override;
	void OnCharacterPostCoreTick() override;
	void OnCharacterSnap(int SnappingClient) override;
	void OnCharacterSpawned(const SpawnContext &Context) override;
	void OnCharacterDamage(SDamageContext *pContext) override;
	void OnCharacterBackFromDead() override;

	void OnKilledCharacter(CIcCharacter *pVictim, const DeathContext &Context) override;
	void OnHumanHammerHitHuman(CIcCharacter *pTarget);

	void OnHookAttachedPlayer() override;

	void HandleNinja() override;

	void OnWeaponFired(WeaponFireContext *pFireContext) override;
	void OnHammerFired(WeaponFireContext *pFireContext) override;
	void OnGunFired(WeaponFireContext *pFireContext) override;
	void OnShotgunFired(WeaponFireContext *pFireContext) override;
	void OnGrenadeFired(WeaponFireContext *pFireContext) override;
	void OnLaserFired(WeaponFireContext *pFireContext) override;

	void OnSlimeEffect(int Owner, int Damage, float DamageInterval) override;
	bool HasWhiteHole() const;
	void GiveWhiteHole();
	void RemoveWhiteHole();
	bool HasAirStrike() const;
	void GiveAirStrike();
	void RemoveAirStrike();

	void GiveInvisibility(float Duration, int FromCID);
	void ResetInvisibility();
	float GetInvisibilityRemainingDuration() const;

	void UpgradeMercBomb(CMercenaryBomb *pBomb, float UpgradePoints);
	void OnHeroFlagTaken(CIcCharacter *pHero);
	void OnWhiteHoleSpawned(CWhiteHole *pWhiteHole);

	void GiveUpgrades(const PlayerUpgradesArray &NewUpgrades) override;
	void RefreshHeroFlagPosition();

	[[nodiscard]] int GetSurvivalHeroReviveCharges() const { return m_SurvivalHeroReviveCharges; }
	void SetSurvivalHeroReviveCharges(const int Charges) { m_SurvivalHeroReviveCharges = Charges; }
	[[nodiscard]] int GetSurvivalNoHookEndTick() const { return m_SurvivalNoHookEndTick; }

protected:
	void GiveClassAttributes() override;
	void SpawnChildEntities();
	void DestroyChildEntities() override;
	void BroadcastWeaponState() const override;

	void ResetUpgrades();

	void CheckSuperWeaponAccess();
	void OnNinjaTargetKiller(bool Assisted);
	void GiveNinjaBuf();

	void SnapHero(int SnappingClient);
	void SnapScientist(int SnappingClient);

	void ActivateNinja(WeaponFireContext *pFireContext);
	void PlaceEngineerWall(WeaponFireContext *pFireContext);
	void PlaceLooperWall(WeaponFireContext *pFireContext);
	void FireMercenaryBomb(WeaponFireContext *pFireContext);
	void PlaceScientistMine(WeaponFireContext *pFireContext);
	void PlaceTurret(WeaponFireContext *pFireContext);

	void OnPoisonGrenadeFired(WeaponFireContext *pFireContext);
	void OnMedicGrenadeFired(WeaponFireContext *pFireContext);

	void OnMercLaserFired(WeaponFireContext *pFireContext);

	bool PositionLockAvailable() const;
	int GetTurretGive() const;
	int GetMaxTurrets() const;

	int GetMaxSciMines() const;

private:
	icArray<int, 2> m_BarrierHintIds;

	int m_BonusTick = 0;
	int m_ResetKillsTick = 0;
	float m_KillsProgression = 0;
	float m_WeaponRegenIntervalModifier[NUM_WEAPONS];
	float m_WeaponReloadIntervalModifier[NUM_WEAPONS];
	float m_LaserReachModifier = 1.0f;

	float m_MercBombs = 0;
	float m_MercInAirAmmoRegenMaxTime{};
	int m_TurretCount = 0;
	int m_BroadcastWhiteHoleReady; // used to broadcast "WhiteHole ready" for a short period of time
	int m_PositionLockTicksRemaining = 0;
	int m_NinjaTargetTick = 0;
	int m_NinjaTargetCid = -1;
	int m_NinjaVelocityBuff = 0;
	int m_NinjaExtraDamage = 0;
	int m_NinjaAmmoBuff = 0;
	int m_NinjaComboFirstTick = 0;
	icArray<CEntity *, 24> m_apHitObjects;
	bool m_HasWhiteHole = false;
	int m_BroadcastAirStrikeReady;
	bool m_HasAirStrike = false;
	int m_InvisibilityStartTick{};
	int m_InvisibilityEndTick{};

	CHeroFlag *m_pHeroFlag = nullptr;
	int m_HeroFlagRefreshTick = 0;
	int m_SurvivalHeroReviveCharges = 0;
	int m_SurvivalNoHookEndTick = 0;
};

#endif // GAME_SERVER_INFCLASS_CLASSES_HUMAN_H
