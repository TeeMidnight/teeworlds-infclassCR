#include "artillery-grenade.h"

CArtilleryGrenade::CArtilleryGrenade(CGameContext *pGameContext, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
		int Damage, float Force, EDamageType DamageType) : CArtilleryProjectile(pGameContext, Type, Owner, Pos, Dir, Span,
			Damage, Force, DamageType, EntityId)
{
	GameWorld()->InsertEntity(this);
};

int CArtilleryGrenade::EntityId{};
