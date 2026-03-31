#pragma once

#include <game/server/infclass/entities/artillery-projectile.h>

enum class TAKEDAMAGEMODE;
enum class EDamageType;

class CArtilleryGrenade : public CArtilleryProjectile
{
public:
	CArtilleryGrenade(CGameContext *pGameContext, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
		int Damage, float Force, EDamageType DamageType);

	static int EntityId;
};
