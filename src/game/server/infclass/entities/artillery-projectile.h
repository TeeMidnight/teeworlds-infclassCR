#pragma once

#include <game/server/infclass/entities/ic_entity.h>

enum class TAKEDAMAGEMODE;
enum class EDamageType;

class CArtilleryProjectile : public CIcEntity
{
public:
	CArtilleryProjectile(CGameContext *pGameContext, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
		int Damage, float Force, EDamageType DamageType, int EntityId);

	virtual vec2 GetPos();
	void FillInfo(CNetObj_Projectile *pProj);

	void Tick() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;

	void SetFlashRadius(int Radius);
	void SetExplosive(bool Value);
	void SetSoundImpact(std::optional<ESound> Sound);
	void SetCollideSpan(int TileSpan, int EntitySpan);

	virtual bool OnProjectileCollide(CCharacter *pChr);

protected:
	vec2 m_Direction;
	int m_LifeSpan;
	int m_Type;
	int m_Damage;
	std::optional<ESound> m_SoundImpact;
	int m_Weapon;
	EDamageType m_DamageType;
	float m_Force;
	int m_StartTick;
	bool m_Explosive{};

	int m_FlashRadius{};
	vec2 m_ActualPos;
	TAKEDAMAGEMODE m_TakeDamageMode;

	int m_TileCollideSpan;
	int m_EntityCollideSpan;
};
