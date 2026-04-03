#pragma once

#include <algorithm>
#include <array>

enum class EPlayerClass
{
	Invalid = -1,
	Random = 0,
	None = 0,

	Mercenary,
	Medic,
	Hero,
	Engineer,
	Soldier,
	Ninja,
	Sniper,
	Scientist,
	Biologist,
	Looper,
	Artillery,

	Smoker,
	Boomer,
	Hunter,
	Bat,
	Ghost,
	Spider,
	Ghoul,
	Slug,
	Voodoo,
	Witch,
	Undead,
	Tank,
	Spitter,

	Count
};

constexpr std::size_t NB_PLAYERCLASS = static_cast<std::size_t>(EPlayerClass::Count);

static constexpr EPlayerClass AllHumanClasses[]{
	EPlayerClass::None,

	EPlayerClass::Mercenary,
	EPlayerClass::Medic,
	EPlayerClass::Hero,
	EPlayerClass::Engineer,
	EPlayerClass::Soldier,
	EPlayerClass::Ninja,
	EPlayerClass::Sniper,
	EPlayerClass::Scientist,
	EPlayerClass::Biologist,
	EPlayerClass::Looper,
	EPlayerClass::Artillery,
};

static constexpr EPlayerClass AllInfectedClasses[]{
	EPlayerClass::Smoker,
	EPlayerClass::Boomer,
	EPlayerClass::Hunter,
	EPlayerClass::Bat,
	EPlayerClass::Ghost,
	EPlayerClass::Spider,
	EPlayerClass::Ghoul,
	EPlayerClass::Slug,
	EPlayerClass::Voodoo,
	EPlayerClass::Witch,
	EPlayerClass::Undead,
	EPlayerClass::Tank,
	EPlayerClass::Spitter,
};

static constexpr EPlayerClass AllPlayerClasses[]{
	EPlayerClass::None,

	EPlayerClass::Mercenary,
	EPlayerClass::Medic,
	EPlayerClass::Hero,
	EPlayerClass::Engineer,
	EPlayerClass::Soldier,
	EPlayerClass::Ninja,
	EPlayerClass::Sniper,
	EPlayerClass::Scientist,
	EPlayerClass::Biologist,
	EPlayerClass::Looper,
	EPlayerClass::Artillery,

	EPlayerClass::Smoker,
	EPlayerClass::Boomer,
	EPlayerClass::Hunter,
	EPlayerClass::Bat,
	EPlayerClass::Ghost,
	EPlayerClass::Spider,
	EPlayerClass::Ghoul,
	EPlayerClass::Slug,
	EPlayerClass::Voodoo,
	EPlayerClass::Witch,
	EPlayerClass::Undead,
	EPlayerClass::Tank,
	EPlayerClass::Spitter,
};

constexpr int NB_HUMANCLASS = std::size(AllHumanClasses);
constexpr int NB_INFECTEDCLASS = std::size(AllInfectedClasses);
static_assert(NB_HUMANCLASS + NB_INFECTEDCLASS == NB_PLAYERCLASS);
static_assert(std::size(AllPlayerClasses) == NB_PLAYERCLASS);

inline constexpr bool IsHumanClass(EPlayerClass C)
{
	return std::find(std::begin(AllHumanClasses), std::end(AllHumanClasses), C) != std::end(AllHumanClasses);
}

inline constexpr bool IsInfectedClass(EPlayerClass C)
{
	return std::find(std::begin(AllInfectedClasses), std::end(AllInfectedClasses), C) != std::end(AllInfectedClasses);
}

int toNetValue(EPlayerClass C);

const char *toString(EPlayerClass PlayerClass);
