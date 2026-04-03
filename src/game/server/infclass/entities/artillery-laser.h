#pragma once

#include <game/server/infclass/entities/artillery-projectile.h>
#include <engine/shared/config.h>

enum class TAKEDAMAGEMODE;
enum class EDamageType;

class CArtilleryLaser : public CArtilleryProjectile
{
public:
	static int EntityId;

	CArtilleryLaser(CGameContext *pGameContext, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
		int Damage, float Force, EDamageType DamageType, bool IsSuperWeapon);

	virtual vec2 GetPos() override;
	virtual bool OnProjectileCollide(CCharacter *pChr) override;
	virtual void Tick() override;
	void FillAirStrike(bool IsSuperWeapon)
	{
		m_AirStrikeTotal = IsSuperWeapon ? g_Config.m_InfAirStrikeNumSuper : g_Config.m_InfAirStrikeNum;
		m_AirStrikeRange = IsSuperWeapon ? g_Config.m_InfAirStrikeRadiusSuper : g_Config.m_InfAirStrikeRadius;
		m_AirStrikeDeley = IsSuperWeapon ? g_Config.m_InfAirStrikeIntervalSuper : g_Config.m_InfAirStrikeInterval;
		m_AirStrikeSpacing = (IsSuperWeapon ? g_Config.m_InfAirStrikeSpacingSuper : g_Config.m_InfAirStrikeSpacing) * 0.5f;
		m_AirStrikePerBomb = IsSuperWeapon ? g_Config.m_InfAirStrikePerBombSuper : g_Config.m_InfAirStrikePerBomb;
	};
protected:
	int m_AirStrikeTotal;
	int m_AirStrikeRange;
	int m_AirStrikeDeley;
	float m_AirStrikeSpacing;
	int m_AirStrikePerBomb;

	int m_AirStrikeTick;
	int m_AirStrikeNum;
	bool m_AirStrikeInvert;
	int GetAirStrikeSlot(bool IsInvert = false)
	{
		return (((((m_AirStrikeNum + (m_AirStrikePerBomb & 1)) / 2) * 2) + !(m_AirStrikePerBomb & 1)) % m_AirStrikeRange)
				* (m_AirStrikeNum & 1 ? -1 : 1) * (IsInvert ? -1 : 1);
	}
};
