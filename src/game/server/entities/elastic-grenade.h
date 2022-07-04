/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_ELASTIC_GRENADE_H
#define GAME_SERVER_ENTITIES_ELASTIC_GRENADE_H

class CElasticGrenade : public CEntity
{
public:
	int m_Owner;
	
public:
	CElasticGrenade(CGameWorld *pGameWorld, int Owner, int Weapon, vec2 Pos, vec2 Dir);

	vec2 GetPos(float Time);
	void FillInfo(CNetObj_Projectile *pProj);

	virtual void Reset();
	virtual void Tick();
	virtual void TickPaused();
	virtual void Explode();
	virtual void Snap(int SnappingClient);
	
	int GetTick() { return m_LifeSpan; }


private:
	vec2 m_ActualPos;
	vec2 m_ActualDir;
	vec2 m_Direction;
	int m_Weapon;
	int m_StartTick;
	int m_LifeSpan;
	int m_CollisionNum;
};

#endif
