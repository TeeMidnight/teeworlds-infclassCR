// Copyright (c) ST-Chara 2024 - 2024
#include <game/server/gamecontext.h>
#include <engine/shared/config.h>
#include <base/math.h>
#include <base/vmath.h>
#include <game/server/player.h>
#include "laser.h"
#include "doctor-funnel.h"

CDoctorFunnel::CDoctorFunnel(CGameWorld *pGameWorld, vec2 Pos, int Owner)
    : CEntity(pGameWorld, CGameWorld::ENTTYPE_DOCTOR_FUNNEL), m_Owner(Owner), m_LockTarget(false), m_TargetCID(-1)
{
    for (int i = 0; i < NUM_LASER; i++)
        m_LaserIDs[i] = Server()->SnapNewID();
    m_ConnectID = Server()->SnapNewID();

    GameWorld()->InsertEntity(this);
}

CDoctorFunnel::~CDoctorFunnel()
{
    for (int i = 0; i < NUM_LASER; i++)
        Server()->SnapFreeID(m_LaserIDs[i]);
    Server()->SnapFreeID(m_ConnectID);
}

void CDoctorFunnel::ConsumePower(int Power)
{
    if (!GameServer()->GetPlayerChar(m_Owner))
        return;

    if (GameServer()->GetPlayerChar(m_Owner)->m_PowerBattery <= 0)
    {
        GameServer()->GetPlayerChar(m_Owner)->m_PowerBattery = 0;
        m_LowPower = true;
    }
    else
        GameServer()->GetPlayerChar(m_Owner)->m_PowerBattery -= Power;
}

void CDoctorFunnel::FunnelMove()
{
    m_Pos += (m_TargetPos - m_Pos) / 24.f;
}

void CDoctorFunnel::ResetLock()
{
    m_LockTarget = false;
    m_ChangeTargetNeed = 0;
}

vec2 CDoctorFunnel::GetOwnerPos()
{
    return GameServer()->GetPlayerChar(m_Owner)->m_Pos;
}

vec2 CDoctorFunnel::GetTargetPos()
{
    if (!m_LockTarget)
    {
        std::vector<int> Targets;
        CPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
        while (Iter.Next())
        {
            if (!Iter.Player()->GetCharacter())
                continue;

            if (!Iter.Player()->IsZombie() ||
                (Iter.Player()->GetClass() == PLAYERCLASS_UNDEAD && Iter.Player()->GetCharacter()->IsFrozen()) ||
                (Iter.Player()->GetClass() == PLAYERCLASS_VOODOO && Iter.Player()->GetCharacter()->m_VoodooAboutToDie) ||
                (Iter.Player()->GetClass() == PLAYERCLASS_VOODOO && Iter.Player()->GetCharacter()->m_VoodooAboutToDie) && g_Config.m_InfVoodooBoomer == 1)
                continue;

            if (distance(Iter.Player()->GetCharacter()->m_Pos, GameServer()->GetPlayerChar(m_Owner)->m_Pos) < 2000)
                Targets.push_back(Iter.ClientID());
        }

        float Distance = 999999999;
        int ID = -1;
        for (size_t i = 0; i < Targets.size(); i++)
        {
            if (!GameServer()->GetPlayerChar(Targets[i]))
                continue;

            if (distance(GameServer()->GetPlayerChar(Targets[i])->m_Pos, GameServer()->GetPlayerChar(m_Owner)->m_Pos) < Distance)
            {
                Distance = distance(GameServer()->GetPlayerChar(Targets[i])->m_Pos, GameServer()->GetPlayerChar(m_Owner)->m_Pos);
                ID = Targets[i];
            }
        }

        if (Targets.size() > 0)
        {
            m_LockTarget = true;
            m_TargetCID = ID;
        }
    }

    if (m_TargetCID >= 0 && GameServer()->GetPlayerChar(m_TargetCID))
        return GameServer()->GetPlayerChar(m_TargetCID)->m_Pos;

    return vec2(GetOwnerPos().x, GetOwnerPos().y - 128.f);
}

void CDoctorFunnel::Tick()
{
    if (!GameServer()->GetPlayerChar(m_Owner) || GameServer()->GetPlayerChar(m_Owner)->IsZombie())
        return Reset();

    if (Server()->Tick() % 50 == 0)
        m_ChangeTargetNeed--;

    int Power = GameServer()->GetPlayerChar(m_Owner)->m_PowerBattery / 50.f;
    int Max = g_Config.m_InfDoctorMaxPowerBattery / 50.f;
    dynamic_string State;
    State.clear();
    switch (GameServer()->GetPlayerChar(m_Owner)->m_FunnelState)
    {
    case STATE_FOLLOW:
    {
        State.copy(Server()->Localization()->Localize(GameServer()->m_apPlayers[m_Owner]->GetLanguage(), _("Following")));
        ResetLock();
        if (!m_LowPower)
        {
            m_TargetPos = vec2(GetOwnerPos().x, GetOwnerPos().y - 128.f);
            for (CCharacter *pChr = (CCharacter *)GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
            {
                if (!pChr->IsZombie() ||
                    (pChr->GetClass() == PLAYERCLASS_UNDEAD && pChr->IsFrozen()) ||
                    (pChr->GetClass() == PLAYERCLASS_VOODOO && pChr->m_VoodooAboutToDie) ||
                    (pChr->GetClass() == PLAYERCLASS_BOOMER && pChr->m_VoodooAboutToDie && g_Config.m_InfVoodooBoomer == 1))
                    continue;

                float Len = distance(pChr->m_Pos, m_Pos);

                if (Len < 500 && Server()->Tick() % 10 == 0)
                {
                    vec2 Direction = normalize(pChr->m_Pos - m_Pos);

                    new CLaser(GameWorld(), m_Pos, Direction, GameServer()->Tuning()->m_LaserReach, m_Owner, g_Config.m_InfDoctorFunnelDamage);

                    GameServer()->CreateSound(m_Pos, SOUND_RIFLE_FIRE);
                }
            }
        }
        FunnelMove();
        ConsumePower(1);
        break;
    }
    case STATE_FIND:
    {
        State.copy(Server()->Localization()->Localize(GameServer()->m_apPlayers[m_Owner]->GetLanguage(), _("Tracking")));

        if (!m_LowPower)
            m_TargetPos = vec2(GetTargetPos().x, GetTargetPos().y);
        FunnelMove();
        if (m_LowPower)
            break;

        if (!GameServer()->GetPlayerChar(m_TargetCID) || m_ChangeTargetNeed > 10 || (GameServer()->GetPlayerChar(m_TargetCID) && distance(GameServer()->GetPlayerChar(m_TargetCID)->m_Pos, GameServer()->GetPlayerChar(m_Owner)->m_Pos) > 2000))
            ResetLock();

        if (GameServer()->GetPlayerChar(m_TargetCID) && distance(m_TargetPos, m_Pos) < 150.f && Server()->Tick() % 10 == 0)
            GameServer()->GetPlayerChar(m_TargetCID)->TakeDamage(vec2(0, 0), g_Config.m_InfDoctorFunnelDamage, m_Owner, WEAPON_HAMMER, TAKEDAMAGEMODE_NOINFECTION);

        ConsumePower(2);
        break;
    }
    case STATE_STAY:
    {
        State.copy(Server()->Localization()->Localize(GameServer()->m_apPlayers[m_Owner]->GetLanguage(), _("Charging")));
        ResetLock();
        m_Pos = vec2(GetOwnerPos().x, GetOwnerPos().y);

        if (GameServer()->GetPlayerChar(m_Owner)->m_PowerBattery < g_Config.m_InfDoctorMaxPowerBattery && Server()->Tick() % 50 == 0)
        {
            GameServer()->CreateSound(GameServer()->GetPlayerChar(m_Owner)->m_Pos, SOUND_HOOK_NOATTACH);
            GameServer()->GetPlayerChar(m_Owner)->m_PowerBattery += 25;
        }
        m_LowPower = false;
        break;
    }
    default:
        break;
    }

    GameServer()->SendBroadcast_Localization(m_Owner, BROADCAST_PRIORITY_WEAPONSTATE, BROADCAST_DURATION_REALTIME,
                                             _("Remaining power of Funnel: {int:power}/{int:max}\nState: {str:state}"),
                                             "power", &Power,
                                             "max", &Max, "state", State.buffer());
}

void CDoctorFunnel::Snap(int SnappingClient)
{
    if (IsDontSnapEntity(SnappingClient) || !GameServer()->GetPlayerChar(m_Owner))
        return;

    {
        vec2 SnapPos[NUM_LASER] = {vec2(-48, 0), vec2(-24, -14), vec2(0, -28),
                                   vec2(24, -14), vec2(48, 0),
                                   vec2(24, 14), vec2(0, 28), vec2(-24, 14),
                                   vec2(-48, 0)};

        for (int i = 0; i < NUM_LASER; i++)
        {
            CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_LaserIDs[i], sizeof(CNetObj_Laser)));
            if (!pObj)
                return;

            pObj->m_FromX = SnapPos[i].x + m_Pos.x;
            pObj->m_FromY = SnapPos[i].y + m_Pos.y;
            pObj->m_X = SnapPos[(i >= NUM_LASER - 1) ? 0 : i + 1].x + m_Pos.x;
            pObj->m_Y = SnapPos[(i >= NUM_LASER - 1) ? 0 : i + 1].y + m_Pos.y;
            pObj->m_StartTick = Server()->Tick();
        }

        {
            CNetObj_Projectile *pProj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, m_ID, sizeof(CNetObj_Projectile)));
            if (!pProj)
                return;

            pProj->m_X = m_Pos.x;
            pProj->m_Y = m_Pos.y;
            pProj->m_VelX = 0;
            pProj->m_VelY = 0;
            pProj->m_Type = WEAPON_HAMMER;
            pProj->m_StartTick = Server()->Tick();
        }

        if (GameServer()->GetPlayerChar(m_Owner)->m_FunnelState == STATE_FIND)
        {
            if (distance(m_TargetPos, m_Pos) < 150.f)
            {
                CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, m_ConnectID, sizeof(CNetObj_Laser)));
                if (!pObj)
                    return;

                pObj->m_FromX = m_Pos.x;
                pObj->m_FromY = m_Pos.y;
                pObj->m_X = m_TargetPos.x;
                pObj->m_Y = m_TargetPos.y;
                pObj->m_StartTick = Server()->Tick();
            }
        }
    }
}

void CDoctorFunnel::Reset()
{
    GameServer()->m_World.DestroyEntity(this);
}