#include "weapons.h"

#include <base/tl/ic_enum.h>

static const char *gs_aInfclassWeaponNames[] = {
	"none",
	"hammer",
	"gun",
	"shotgun",
	"grenade",
	"laser",
	"ninja",
	"engineer_laser",
	"sniper_rifle",
	"soldier_grenade",
	"teleport_gun",
	"explosive_laser",
	"healing_grenade",
	"reviving_laser",
	"medic_shotgun",
	"laser_turret",
	"plasma_turret",
	"hero_grenade",
	"hero_laser",
	"hero_shotgun",
	"ricochet_shotgun",
	"biologist_mine_las",
	"looper_laser",
	"looper_grenade",
	"ninja_katana",
	"ninja_grenade",
	"mercenary_gun",
	"poison_grenade",
	"mercenary_upgrade_",
	"blinding_laser",
	"tranquilizer_rifle",
	
	"artillery_shotgun",
	"artillery_grenade",
	"artillery_rifle",

	"jaws",
	"slime",
	"infected_hammer",
	"boomer_explosion",

	"invalid",
};

const char *toString(EInfclassWeapon Weapon)
{
	return toStringImpl(Weapon, gs_aInfclassWeaponNames);
}

EWeaponClass GetWeaponClassById(EInfclassWeapon Weapon)
{
	switch(Weapon)
	{
	case EInfclassWeapon::HAMMER:
	case EInfclassWeapon::JAWS:
	case EInfclassWeapon::SLIME:
	case EInfclassWeapon::INFECTED_HAMMER:
		return EWeaponClass::HAMMER;
	case EInfclassWeapon::BOOMER_EXPLOSION:
		return EWeaponClass::SELF_EXPLOSION;
	case EInfclassWeapon::SOLDIER_GRENADE:
	case EInfclassWeapon::HERO_GRENADE:
	case EInfclassWeapon::LOOPER_GRENADE:
		return EWeaponClass::GRENADE;
	case EInfclassWeapon::LASER:
	case EInfclassWeapon::SNIPER_RIFLE:
	case EInfclassWeapon::EXPLOSIVE_LASER:
	case EInfclassWeapon::HERO_LASER:
	case EInfclassWeapon::LOOPER_LASER:
		return EWeaponClass::LASER;
	default:
		break;
	}

	return EWeaponClass::UNKNOWN;
}
