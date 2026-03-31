#include "ic_gamecontroller.h"

#include <game/server/infclass/entities/biologist-mine.h>
#include <game/server/infclass/entities/bouncing-bullet.h>
#include <game/server/infclass/entities/engineer-wall.h>
#include <game/server/infclass/entities/flyingpoint.h>
#include <game/server/infclass/entities/growingexplosion.h>
#include <game/server/infclass/entities/hero-flag.h>
#include <game/server/infclass/entities/ic_door.h>
#include <game/server/infclass/entities/ic_projectile.h>
#include <game/server/infclass/entities/laser-teleport.h>
#include <game/server/infclass/entities/looper-wall.h>
#include <game/server/infclass/entities/merc-bomb.h>
#include <game/server/infclass/entities/plasma.h>
#include <game/server/infclass/entities/scatter-grenade.h>
#include <game/server/infclass/entities/scientist-mine.h>
#include <game/server/infclass/entities/slug-slime.h>
#include <game/server/infclass/entities/soldier-bomb.h>
#include <game/server/infclass/entities/superweapon-indicator.h>
#include <game/server/infclass/entities/turret.h>
#include <game/server/infclass/entities/white-hole.h>
#include <game/server/infclass/entities/artillery-grenade.h>
#include <game/server/infclass/entities/artillery-laser.h>

void CIcGameController::RegisterEntityTypes()
{
	GameWorld()->RegisterEntityType<CBiologistMine>();
	GameWorld()->RegisterEntityType<CBouncingBullet>();
	GameWorld()->RegisterEntityType<CDoor>();
	GameWorld()->RegisterEntityType<CEngineerWall>();
	GameWorld()->RegisterEntityType<CFlyingPoint>();
	GameWorld()->RegisterEntityType<CGrowingExplosion>();
	GameWorld()->RegisterEntityType<CHeroFlag>();
	GameWorld()->RegisterEntityType<CIcProjectile>();
	GameWorld()->RegisterEntityType<CLaserTeleport>();
	GameWorld()->RegisterEntityType<CLooperWall>();
	GameWorld()->RegisterEntityType<CMercenaryBomb>();
	GameWorld()->RegisterEntityType<CPlasma>();
	GameWorld()->RegisterEntityType<CScatterGrenade>();
	GameWorld()->RegisterEntityType<CScientistMine>();
	GameWorld()->RegisterEntityType<CSlugSlime>();
	GameWorld()->RegisterEntityType<CSoldierBomb>();
	GameWorld()->RegisterEntityType<CSuperWeaponIndicator>();
	GameWorld()->RegisterEntityType<CTurret>();
	GameWorld()->RegisterEntityType<CWhiteHole>();
	GameWorld()->RegisterEntityType<CArtilleryGrenade>();
	GameWorld()->RegisterEntityType<CArtilleryLaser>();
}

void CIcGameController::DestroyChildEntities(int OwnerId)
{
	const int InfCEntities[] = {
		CGameWorld::ENTTYPE_PICKUP,
		CGameWorld::ENTTYPE_LASER,

		CBiologistMine::EntityId,
		CBouncingBullet::EntityId,
		CEngineerWall::EntityId,
		CFlyingPoint::EntityId,
		CGrowingExplosion::EntityId,
		CHeroFlag::EntityId,
		CIcProjectile::EntityId,
		CLaserTeleport::EntityId,
		CLooperWall::EntityId,
		CMercenaryBomb::EntityId,
		CPlasma::EntityId,
		CScatterGrenade::EntityId,
		CScientistMine::EntityId,
		CSlugSlime::EntityId,
		CSoldierBomb::EntityId,
		CSuperWeaponIndicator::EntityId,
		CTurret::EntityId,
		CWhiteHole::EntityId,
		CArtilleryLaser::EntityId,
		CArtilleryGrenade::EntityId,
	};

	for(const auto EntityType : InfCEntities)
	{
		for(CIcEntity *p = (CIcEntity *)GameWorld()->FindFirst(EntityType); p; p = (CIcEntity *)p->TypeNext())
		{
			if(p->GetOwner() != OwnerId)
				continue;

			GameServer()->m_World.DestroyEntity(p);
		}
	}
}
