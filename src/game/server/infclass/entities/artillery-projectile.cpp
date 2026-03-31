#include <base/math.h>
#include <base/vmath.h>
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>

#include <game/infclass/damage_type.h>
#include <game/server/infclass/entities/growingexplosion.h>
#include <game/server/infclass/entities/ic_character.h>
#include <game/server/infclass/ic_gamecontroller.h>

#include "artillery-projectile.h"

CArtilleryProjectile::CArtilleryProjectile(CGameContext *pGameContext, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
	int Damage, float Force, EDamageType DamageType, int EntityId) :
	CIcEntity(pGameContext, EntityId, Pos, Owner)
{
	m_Type = Type;
	m_Direction = Dir;
	m_LifeSpan = Span;
	m_Force = Force;
	m_Damage = Damage;
	m_DamageType = DamageType;
	m_StartTick = Server()->Tick();
	m_ActualPos = Pos;
	m_Weapon = DamageTypeToWeapon(DamageType, &m_TakeDamageMode);
	m_TileCollideSpan = 0;
	m_EntityCollideSpan = 0;
}

vec2 CArtilleryProjectile::GetPos()
{
	return m_Pos + (m_Direction * ((Server()->Tick()-m_StartTick)/(float)Server()->TickSpeed()) * GameServer()->Tuning()->m_GrenadeSpeed);
}

void CArtilleryProjectile::Tick()
{
	vec2 PrevPos = m_ActualPos;
	m_ActualPos = GetPos();

	m_LifeSpan--;

	int Collide = 0;
	if(m_TileCollideSpan > 0)
	{
		m_TileCollideSpan--;
	}
	else
		Collide = GameServer()->Collision()->IntersectLine(PrevPos, m_ActualPos, &m_ActualPos, nullptr);
	const float ProjectileRadius = 6.0f;
	const CIcCharacter *pOwnerChar = GetOwnerCharacter();
	const bool IsInfected = pOwnerChar && pOwnerChar->IsInfected();
	CharacterFilter OnlyOtherTeamFilter = IsInfected ? CIcCharacter::GetHumansFilter() : CIcCharacter::GetInfectedFilter();
	CIcCharacter *TargetChr = nullptr;
	if(m_EntityCollideSpan > 0)
		m_EntityCollideSpan--;
	else
		TargetChr = CIcCharacter::GetInstance(GameWorld()->IntersectCharacter(PrevPos, m_ActualPos, ProjectileRadius, m_ActualPos, OnlyOtherTeamFilter));

	if(TargetChr || Collide || m_LifeSpan < 0 || GameLayerClipped(m_ActualPos))
	{
		if(!OnProjectileCollide(TargetChr))
			return;
		
		if(m_LifeSpan >= 0 || (m_Weapon == WEAPON_GRENADE))
		{
			if(m_SoundImpact.has_value())
				GameServer()->CreateSound(m_ActualPos, m_SoundImpact.value());
		}

		if(m_FlashRadius)
		{
			vec2 Dir = normalize(PrevPos - m_ActualPos);
			if(length(Dir) > 1.1) Dir = normalize(m_Pos - m_ActualPos);
			
			new CGrowingExplosion(GameServer(), m_ActualPos, Dir, GetOwner(), m_FlashRadius, m_DamageType);
		}
		else if(m_Explosive)
		{
			GameController()->CreateExplosion(m_ActualPos, GetOwner(), m_DamageType);
		}
		else if(TargetChr)
		{
			if(pOwnerChar)
			{
				if(pOwnerChar->IsHuman() && TargetChr->IsHuman())
				{
					TargetChr->TakeDamage(m_Direction * 0.001f, m_Damage, GetOwner(), m_DamageType);
				}
				else
				{
					TargetChr->TakeDamage(m_Direction * maximum(0.001f, m_Force), m_Damage, GetOwner(), m_DamageType);
				}
			}
		}

		GameWorld()->DestroyEntity(this);
	}
}

void CArtilleryProjectile::TickPaused()
{
	++m_StartTick;
}

void CArtilleryProjectile::FillInfo(CNetObj_Projectile *pProj)
{
	pProj->m_X = (int)m_ActualPos.x;
	pProj->m_Y = (int)m_ActualPos.y;
	pProj->m_VelX = (int)(m_Direction.x*100.0f);
	pProj->m_VelY = (int)(m_Direction.y*100.0f);
	pProj->m_StartTick = Server()->Tick();
	pProj->m_Type = m_Type;
}

void CArtilleryProjectile::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient, m_ActualPos))
		return;

	CNetObj_Projectile *pProj = Server()->SnapNewItem<CNetObj_Projectile>(GetId());
	if(pProj)
		FillInfo(pProj);
}

void CArtilleryProjectile::SetFlashRadius(int Radius)
{
	m_FlashRadius = Radius;
}

void CArtilleryProjectile::SetExplosive(bool Value)
{
	m_Explosive = Value;
}

void CArtilleryProjectile::SetSoundImpact(std::optional<ESound> Sound)
{
	m_SoundImpact = Sound;
}

void CArtilleryProjectile::SetCollideSpan(int TileSpan, int EntitySpan)
{
	m_TileCollideSpan = TileSpan;
	m_EntityCollideSpan = EntitySpan;
}

bool CArtilleryProjectile::OnProjectileCollide(CCharacter *pChr)
{
	const CIcCharacter *pOwnerChar = GetOwnerCharacter();
	const bool IsInfected = pOwnerChar && pOwnerChar->IsInfected();
	if(IsInfected)
	{
		GameWorld()->RemoveEntity(this);
		return false;
	}
	return true;
}
