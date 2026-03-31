#include "damage_type.h"

#include <base/tl/ic_enum.h>
#include <game/server/player.h>

static const char *gs_aDamageTypeNames[] = {
	"invalid",
	"unused1",

	"hammer",
	"gun",
	"shotgun",
	"grenade",
	"laser",
	"ninja",

	"sniper_rifle",
	"scientist_laser",
	"medic_shotgun",
	"biologist_shotgun",
	"looper_laser",
	"mercenary_gun",
	"mercenary_grenade",
	"scientist_teleport",
	"stunning_grenade",
	"artillery_shotgun",
	"artillery_grenade",
	"artillery_laser",
	"artillery_bomb",

	"laser_wall",
	"soldier_bomb",
	"scientist_mine",
	"biologist_mine",
	"mercenary_bomb",
	"white_hole",
	"turret_destruction",
	"turret_laser",
	"turret_plasma",

	"infection_hammer",
	"bite",
	"boomer_explosion",
	"slug_slime",
	"drying_hook",

	"death_tile",
	"infection_tile",

	"game",
	"kill_command",
	"game_final_explosion",
	"game_infection",

	"no_damage",
	"medic_revival",
	"damage_tile",
};

const char *toString(EDamageType DamageType)
{
	return toStringImpl(DamageType, gs_aDamageTypeNames);
}

int DamageTypeToWeapon(EDamageType DamageType, TAKEDAMAGEMODE *pMode)
{
	int Weapon = WEAPON_GAME;
	TAKEDAMAGEMODE HelperMode;
	TAKEDAMAGEMODE &Mode = pMode ? *pMode : HelperMode;

	Mode = TAKEDAMAGEMODE::NOINFECTION;

	switch(DamageType)
	{
	case EDamageType::INVALID:
	case EDamageType::UNUSED1:
		break;
	case EDamageType::NO_DAMAGE:
		break;

	case EDamageType::HAMMER:
	case EDamageType::BITE:
	case EDamageType::LASER_WALL:
	case EDamageType::BIOLOGIST_MINE:
	case EDamageType::TURRET_DESTRUCTION:
	case EDamageType::TURRET_LASER:
	case EDamageType::TURRET_PLASMA:
	case EDamageType::WHITE_HOLE:
	case EDamageType::SLUG_SLIME:
		Weapon = WEAPON_HAMMER;
		break;
	case EDamageType::SOLDIER_BOMB:
	case EDamageType::MERCENARY_BOMB:
	case EDamageType::SCIENTIST_MINE:
	case EDamageType::SCIENTIST_TELEPORT:
		Mode = TAKEDAMAGEMODE::ALLOW_SELFHARM;
		Weapon = WEAPON_HAMMER;
		break;
	case EDamageType::INFECTED_HAMMER:
	case EDamageType::BOOMER_EXPLOSION:
		Mode = TAKEDAMAGEMODE::INFECTION;
		Weapon = WEAPON_HAMMER;
		break;
	case EDamageType::GUN:
	case EDamageType::MERCENARY_GUN:
		Weapon = WEAPON_GUN;
		break;
	case EDamageType::SHOTGUN:
	case EDamageType::MEDIC_SHOTGUN:
	case EDamageType::BIOLOGIST_SHOTGUN:
	case EDamageType::ARTILLERY_SHOTGUN:
		Weapon = WEAPON_SHOTGUN;
		break;
	case EDamageType::GRENADE:
	case EDamageType::STUNNING_GRENADE:
	case EDamageType::MERCENARY_GRENADE:
	case EDamageType::ARTILLERY_GRENADE:
	case EDamageType::ARTILLERY_BOMB:
		Weapon = WEAPON_GRENADE;
		break;
	case EDamageType::LASER:
	case EDamageType::SNIPER_RIFLE:
	case EDamageType::SCIENTIST_LASER:
	case EDamageType::LOOPER_LASER:
	case EDamageType::ARTILLERY_LASER:
		Weapon = WEAPON_LASER;
		break;
	case EDamageType::NINJA:
	case EDamageType::DRYING_HOOK:
		Weapon = WEAPON_NINJA;
		break;

	case EDamageType::DEATH_TILE:
	case EDamageType::INFECTION_TILE:
	case EDamageType::DAMAGE_TILE:
		Weapon = WEAPON_WORLD;
		break;
	case EDamageType::GAME:
		Weapon = WEAPON_GAME;
		break;
	case EDamageType::KILL_COMMAND:
		Weapon = WEAPON_SELF;
		break;
	case EDamageType::GAME_FINAL_EXPLOSION:
	case EDamageType::GAME_INFECTION:
		// This is how the infection world work
		Weapon = WEAPON_WORLD;
		break;
	case EDamageType::MEDIC_REVIVAL:
		Weapon = WEAPON_LASER;
		Mode = TAKEDAMAGEMODE::ALLOW_SELFHARM;
		break;
	}

	return Weapon;
}
