/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/
#include "gamecontext.h"

#include <base/logger.h>
#include <base/math.h>
#include <engine/console.h>
#include <engine/map.h>
#include <engine/server/lua.h>
#include <engine/server/lua_callback.h>
#include <engine/server/roundstatistics.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/shared/linereader.h>
#include <engine/shared/protocolglue.h>
#include <engine/storage.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/version.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <algorithm>
#include <ranges>

extern IGameController *CreateInfclassModController(CGameContext *pGameServer);

#ifdef CONF_GEOLOCATION
#include <infclassr/geolocation.h>
#endif

// Not thread-safe!
class CClientChatLogger : public ILogger
{
	CGameContext *m_pGameServer;
	int m_ClientId;
	ILogger *m_pOuterLogger;

public:
	CClientChatLogger(CGameContext *pGameServer, int ClientId, ILogger *pOuterLogger) :
		m_pGameServer(pGameServer),
		m_ClientId(ClientId),
		m_pOuterLogger(pOuterLogger)
	{
	}
	void Log(const CLogMessage *pMessage) override;
};

void CClientChatLogger::Log(const CLogMessage *pMessage)
{
	if(str_comp(pMessage->m_aSystem, "chatresp") == 0)
	{
		if(m_Filter.Filters(pMessage))
		{
			return;
		}
		m_pGameServer->SendChatTarget(m_ClientId, pMessage->Message());
	}
	else
	{
		m_pOuterLogger->Log(pMessage);
	}
}

enum
{
	RESET,
	NO_RESET
};

/* INFECTION MODIFICATION START ***************************************/
bool CGameContext::m_ClientMuted[MAX_CLIENTS][MAX_CLIENTS];
icArray<std::string, 256> CGameContext::m_aChangeLogEntries;
icArray<uint32_t, 16> CGameContext::m_aChangeLogPageIndices;

/* INFECTION MODIFICATION END *****************************************/

bool CheckClientId(int ClientId)
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS;
}

void CGameContext::Construct(int Resetting)
{
	m_Resetting = false;
	m_pServer = nullptr;
	m_pConfig = nullptr;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_apPlayers[i] = nullptr;
		m_aHitSoundState[i] = 0;
	}

	mem_zero(&m_aLastPlayerInput, sizeof(m_aLastPlayerInput));
	mem_zero(&m_aPlayerHasInput, sizeof(m_aPlayerHasInput));

	if(Resetting == NO_RESET) // first init
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
			for(int j = 0; j < MAX_CLIENTS; j++)
				CGameContext::m_ClientMuted[i][j] = false;
	}

	m_pController = nullptr;
	m_aVoteCommand[0] = 0;
	m_VoteCloseTime = 0;
	m_pVoteOptionFirst = nullptr;
	m_pVoteOptionLast = nullptr;
	m_NumVoteOptions = 0;
	m_LastMapVote = 0;
	m_VoteBanClientId = -1;
	ResetDefaultMaps();

	if(Resetting == NO_RESET)
	{
		m_NonEmptySince = 0;
		m_pVoteOptionHeap = new CHeap();
	}
}

void CGameContext::Destruct(int Resetting)
{
	for(int i = 0; i < m_LaserDots.size(); i++)
		Server()->SnapFreeId(m_LaserDots[i].m_SnapId);
	for(int i = 0; i < m_HammerDots.size(); i++)
		Server()->SnapFreeId(m_HammerDots[i].m_SnapId);

	for(auto &pPlayer : m_apPlayers)
		delete pPlayer;

	if(Resetting == NO_RESET)
		delete m_pVoteOptionHeap;

#ifdef CONF_GEOLOCATION
	if(Resetting == NO_RESET)
	{
		Geolocation::Shutdown();
	}
#endif
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	Destruct(m_Resetting ? RESET : NO_RESET);
}

void CGameContext::Clear()
{
	CHeap *pVoteOptionHeap = m_pVoteOptionHeap;
	CVoteOptionServer *pVoteOptionFirst = m_pVoteOptionFirst;
	CVoteOptionServer *pVoteOptionLast = m_pVoteOptionLast;
	int NumVoteOptions = m_NumVoteOptions;
	CTuningParams Tuning = m_Tuning;

	m_Resetting = true;
	this->~CGameContext();
	new(this) CGameContext(RESET);

	m_pVoteOptionHeap = pVoteOptionHeap;
	m_pVoteOptionFirst = pVoteOptionFirst;
	m_pVoteOptionLast = pVoteOptionLast;
	m_NumVoteOptions = NumVoteOptions;
	m_Tuning = Tuning;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_BroadcastStates[i].m_NoChangeTick = 0;
		m_BroadcastStates[i].m_LifeSpanTick = 0;
		m_BroadcastStates[i].m_Priority = EBroadcastPriority::LOWEST;
		m_BroadcastStates[i].m_TimedPriority = EBroadcastPriority::LOWEST;
		m_BroadcastStates[i].m_PrevMessage[0] = 0;
		m_BroadcastStates[i].m_NextMessage[0] = 0;
	}
}

CNetObj_PlayerInput CGameContext::GetLastPlayerInput(int ClientId) const
{
	dbg_assert(0 <= ClientId && ClientId < MAX_CLIENTS, "invalid ClientId");
	return m_aLastPlayerInput[ClientId];
}

CPlayer *CGameContext::GetPlayer(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return nullptr;

	return m_apPlayers[ClientId];
}

class CCharacter *CGameContext::GetPlayerChar(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || !m_apPlayers[ClientId])
		return nullptr;
	return m_apPlayers[ClientId]->GetCharacter();
}

void CGameContext::FillAntibot(CAntibotRoundData *pData)
{
}

void CGameContext::CreateDamageInd(vec2 Pos, float Angle, int Amount, CClientMask Mask)
{
	float a = 3 * 3.14159f / 2 + Angle;
	// float a = get_angle(dir);
	float s = a - pi / 3;
	float e = a + pi / 3;
	for(int i = 0; i < Amount; i++)
	{
		float f = mix(s, e, static_cast<float>(i + 1) / static_cast<float>(Amount + 2));
		CNetEvent_DamageInd *pEvent = static_cast<CNetEvent_DamageInd *>(m_Events.Create(NETEVENTTYPE_DAMAGEIND, sizeof(CNetEvent_DamageInd), Mask));
		if(pEvent)
		{
			pEvent->m_X = static_cast<int>(Pos.x);
			pEvent->m_Y = static_cast<int>(Pos.y);
			pEvent->m_Angle = static_cast<int>(f * 256.0f);
		}
	}
}

void CGameContext::CreateHammerHit(vec2 Pos, CClientMask Mask)
{
	// create the event
	CNetEvent_HammerHit *pEvent = static_cast<CNetEvent_HammerHit *>(m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit), Mask));
	if(pEvent)
	{
		pEvent->m_X = static_cast<int>(Pos.x);
		pEvent->m_Y = static_cast<int>(Pos.y);
	}
}

void CGameContext::CreateLaserDotEvent(vec2 Pos0, vec2 Pos1, int LifeSpan)
{
	CGameContext::LaserDotState State;
	State.m_Pos0 = Pos0;
	State.m_Pos1 = Pos1;
	State.m_LifeSpan = LifeSpan;
	State.m_SnapId = Server()->SnapNewId();

	m_LaserDots.add(State);
}

void CGameContext::CreateHammerDotEvent(vec2 Pos, int LifeSpan)
{
	CGameContext::HammerDotState State;
	State.m_Pos = Pos;
	State.m_LifeSpan = LifeSpan;
	State.m_SnapId = Server()->SnapNewId();

	m_HammerDots.add(State);
}

void CGameContext::CreateLoveEvent(vec2 Pos)
{
	CGameContext::LoveDotState State;
	State.m_Pos = Pos;
	State.m_LifeSpan = Server()->TickSpeed();
	State.m_SnapId = Server()->SnapNewId();

	m_LoveDots.add(State);
}

void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, CClientMask Mask)
{
	// create the event
	CNetEvent_Explosion *pEvent = static_cast<CNetEvent_Explosion *>(m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion), Mask));
	if(pEvent)
	{
		pEvent->m_X = static_cast<int>(Pos.x);
		pEvent->m_Y = static_cast<int>(Pos.y);
	}
}

/*
void create_smoke(vec2 Pos)
{
	// create the event
	EV_EXPLOSION *pEvent = (EV_EXPLOSION *)events.create(EVENT_SMOKE, sizeof(EV_EXPLOSION));
	if(pEvent)
	{
		pEvent->x = (int)Pos.x;
		pEvent->y = (int)Pos.y;
	}
}*/

void CGameContext::CreatePlayerSpawn(vec2 Pos, CClientMask Mask)
{
	// create the event
	CNetEvent_Spawn *ev = static_cast<CNetEvent_Spawn *>(m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn), Mask));
	if(ev)
	{
		ev->m_X = static_cast<int>(Pos.x);
		ev->m_Y = static_cast<int>(Pos.y);
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientId, CClientMask Mask)
{
	// create the event
	CNetEvent_Death *pEvent = static_cast<CNetEvent_Death *>(m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death), Mask));
	if(pEvent)
	{
		pEvent->m_X = static_cast<int>(Pos.x);
		pEvent->m_Y = static_cast<int>(Pos.y);
		pEvent->m_ClientId = ClientId;
	}
}

void CGameContext::CreateFinishEffect(vec2 Pos, CClientMask Mask)
{
	CNetEvent_Finish *pEvent = m_Events.Create<CNetEvent_Finish>(Mask);
	if(pEvent)
	{
		pEvent->m_X = static_cast<int>(Pos.x);
		pEvent->m_Y = static_cast<int>(Pos.y);
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound, CClientMask Mask)
{
	if(Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = static_cast<CNetEvent_SoundWorld *>(m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask));
	if(pEvent)
	{
		pEvent->m_X = static_cast<int>(Pos.x);
		pEvent->m_Y = static_cast<int>(Pos.y);
		pEvent->m_SoundId = Sound;
	}
}

void CGameContext::CreateSoundGlobal(int Sound, int Target)
{
	if(Sound < 0)
		return;

	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundId = Sound;
	if(Target == -2)
		Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
	else
	{
		if(Target == -1)
		{
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, SERVER_DEMO_CLIENT);
		}

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, Target);
	}
}

bool CGameContext::SnapLaserObject(const CSnapContext &Context, int SnapId, const vec2 &To, const vec2 &From, int StartTick, int Owner, int LaserType, int Subtype, int SwitchNumber)
{
	if(Context.GetClientVersion() >= VERSION_DDNET_MULTI_LASER)
	{
		CNetObj_DDNetLaser *pObj = Server()->SnapNewItem<CNetObj_DDNetLaser>(SnapId);
		if(!pObj)
			return false;

		pObj->m_ToX = static_cast<int>(To.x);
		pObj->m_ToY = static_cast<int>(To.y);
		pObj->m_FromX = static_cast<int>(From.x);
		pObj->m_FromY = static_cast<int>(From.y);
		pObj->m_StartTick = StartTick;
		pObj->m_Owner = Owner;
		pObj->m_Type = LaserType;
		pObj->m_Subtype = Subtype;
		pObj->m_SwitchNumber = SwitchNumber;
	}
	else
	{
		CNetObj_Laser *pObj = Server()->SnapNewItem<CNetObj_Laser>(SnapId);
		if(!pObj)
			return false;

		pObj->m_X = static_cast<int>(To.x);
		pObj->m_Y = static_cast<int>(To.y);
		pObj->m_FromX = static_cast<int>(From.x);
		pObj->m_FromY = static_cast<int>(From.y);
		pObj->m_StartTick = StartTick;
	}

	return true;
}

bool CGameContext::SnapPickup(const CSnapContext &Context, int SnapId, const vec2 &Pos, int Type, int SubType, int SwitchNumber)
{
	if(Context.IsSixup())
	{
		protocol7::CNetObj_Pickup *pPickup = Server()->SnapNewItem<protocol7::CNetObj_Pickup>(SnapId);
		if(!pPickup)
			return false;

		pPickup->m_X = static_cast<int>(Pos.x);
		pPickup->m_Y = static_cast<int>(Pos.y);

		if(Type == POWERUP_WEAPON)
			pPickup->m_Type = SubType == WEAPON_SHOTGUN ? protocol7::PICKUP_SHOTGUN : SubType == WEAPON_GRENADE ? protocol7::PICKUP_GRENADE :
																												  protocol7::PICKUP_LASER;
		else if(Type == POWERUP_NINJA)
			pPickup->m_Type = protocol7::PICKUP_NINJA;
	}
	else if(Context.GetClientVersion() >= VERSION_DDNET_ENTITY_NETOBJS)
	{
		CNetObj_DDNetPickup *pPickup = Server()->SnapNewItem<CNetObj_DDNetPickup>(SnapId);
		if(!pPickup)
			return false;

		pPickup->m_X = static_cast<int>(Pos.x);
		pPickup->m_Y = static_cast<int>(Pos.y);
		pPickup->m_Type = Type;
		pPickup->m_Subtype = SubType;
		pPickup->m_SwitchNumber = SwitchNumber;
	}
	else
	{
		CNetObj_Pickup *pPickup = Server()->SnapNewItem<CNetObj_Pickup>(SnapId);
		if(!pPickup)
			return false;

		pPickup->m_X = static_cast<int>(Pos.x);
		pPickup->m_Y = static_cast<int>(Pos.y);

		pPickup->m_Type = Type;
		if(Context.GetClientVersion() < VERSION_DDNET_WEAPON_SHIELDS)
		{
			if(Type >= POWERUP_ARMOR_SHOTGUN && Type <= POWERUP_ARMOR_LASER)
			{
				pPickup->m_Type = POWERUP_ARMOR;
			}
		}
		pPickup->m_Subtype = SubType;
	}

	return true;
}

void CGameContext::CallVote(int ClientId, const char *pDesc, const char *pCmd, const char *pReason)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	int64_t Now = Server()->Tick();
	CPlayer *pPlayer = m_apPlayers[ClientId];

	if(!pPlayer)
		return;

	if(Lua()->HasGlobalCallable("can_start_vote"))
	{
		std::optional<bool> allowed = RunCallbackWithResult<bool>(Lua()->GetLuaState(), "can_start_vote", ClientId, pCmd, pDesc, pReason);
		if(allowed.has_value() && allowed.value() != true)
		{
			return;
		}
	}

	m_VoteCreator = ClientId;
	StartVote(pDesc, pCmd, pReason);
	pPlayer->m_Vote = 1;
	pPlayer->m_VotePos = m_VotePos = 1;
	pPlayer->m_LastVoteCall = Now;
}

void CGameContext::SendChatTarget(int To, const char *pText)
{
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientId = -1;
	Msg.m_pMessage = pText;
	// only for demo record
	if(To < 0)
	{
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, SERVER_DEMO_CLIENT);

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "*** %s", pText);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chat", aBuf);
	}

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, To);
}

/* INFECTION MODIFICATION START ***************************************/
void CGameContext::SendChatTarget_Localization(int To, int Category, const char *pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_CLIENTS : To + 1);

	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientId = -1;

	std::string Buffer;

	va_list VarArgs;
	va_start(VarArgs, pText);

	bool Sent = false;
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i] && !m_apPlayers[i]->IsBot())
		{
			Buffer.clear();
			Buffer.append(GetChatCategoryPrefix(Category));
			Buffer.append(Server()->Localization()->Format_VL(m_apPlayers[i]->GetLanguage(), pText, VarArgs));

			Msg.m_pMessage = Buffer.c_str();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
			Sent = true;
		}
	}

	if(To < 0 && Sent)
	{
		Buffer.clear();
		Buffer.append(GetChatCategoryPrefix(Category));
		// one message for record
		std::string tmpBuf;
		tmpBuf.append(Buffer);
		tmpBuf.append(Server()->Localization()->Format_VL(Config()->m_InfDefaultLanguageCode, pText, VarArgs));
		Msg.m_pMessage = tmpBuf.c_str();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "*** %s", Msg.m_pMessage);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chat", aBuf);
	}

	va_end(VarArgs);
}

void CGameContext::SendChatTarget_Localization_P(int To, int Category, int Number, const char *pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_CLIENTS : To + 1);

	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientId = -1;

	std::string Buffer;

	va_list VarArgs;
	va_start(VarArgs, pText);

	bool Sent = false;
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i] && !m_apPlayers[i]->IsBot())
		{
			Buffer.clear();
			Buffer.append(GetChatCategoryPrefix(Category));
			Buffer.append(Server()->Localization()->Format_VLP(m_apPlayers[i]->GetLanguage(), Number, pText, VarArgs));

			Msg.m_pMessage = Buffer.c_str();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
			Sent = true;
		}
	}

	if(To < 0 && Sent)
	{
		Buffer.clear();
		Buffer.append(GetChatCategoryPrefix(Category));
		// one message for record
		std::string tmpBuf;
		tmpBuf.append(Buffer);
		tmpBuf.append(Server()->Localization()->Format_VLP(Config()->m_InfDefaultLanguageCode, Number, pText, VarArgs));
		Msg.m_pMessage = tmpBuf.c_str();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);
	}

	va_end(VarArgs);
}

void CGameContext::SendMOTD(int To, const char *pText)
{
	if(m_apPlayers[To])
	{
		CNetMsg_Sv_Motd Msg;

		Msg.m_pMessage = pText;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
	}
}

void CGameContext::SendMOTD_Localization(int To, const char *pText, ...)
{
	if(m_apPlayers[To])
	{
		std::string Buffer;

		CNetMsg_Sv_Motd Msg;

		va_list VarArgs;
		va_start(VarArgs, pText);

		Buffer.append(Server()->Localization()->Format_VL(m_apPlayers[To]->GetLanguage(), pText, VarArgs));

		va_end(VarArgs);

		Msg.m_pMessage = Buffer.c_str();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
	}
}

void CGameContext::AddBroadcast(int ClientId, const char *pText, EBroadcastPriority Priority, int LifeSpan)
{
	if(LifeSpan > 0)
	{
		if(m_BroadcastStates[ClientId].m_TimedPriority > Priority)
			return;

		str_copy(m_BroadcastStates[ClientId].m_TimedMessage, pText);
		m_BroadcastStates[ClientId].m_LifeSpanTick = LifeSpan;
		m_BroadcastStates[ClientId].m_TimedPriority = Priority;
	}
	else
	{
		if(m_BroadcastStates[ClientId].m_Priority > Priority)
			return;

		str_copy(m_BroadcastStates[ClientId].m_NextMessage, pText);
		m_BroadcastStates[ClientId].m_Priority = Priority;
	}
}

void CGameContext::SetClientLanguage(int ClientId, const char *pLanguage)
{
	Server()->SetClientLanguage(ClientId, pLanguage);
	if(m_apPlayers[ClientId])
	{
		m_apPlayers[ClientId]->SetLanguage(pLanguage);
	}
}

void CGameContext::InitChangelog()
{
	if(m_aChangeLogEntries.IsEmpty())
	{
		ReloadChangelog();
	}
}

void CGameContext::ReloadChangelog()
{
	for(std::string &Entry : m_aChangeLogEntries)
	{
		Entry.clear();
	}
	m_aChangeLogEntries.Clear();
	m_aChangeLogPageIndices.Clear();

	const char *pChangelogFilename = Config()->m_SvChangeLogFile;
	if(!pChangelogFilename || pChangelogFilename[0] == 0)
	{
		dbg_msg("ChangeLog", "ChangeLog file is not set");
		return;
	}

	CLineReader LineReader;
	if(!LineReader.OpenFile(m_pStorage->OpenFile(pChangelogFilename, IOFLAG_READ, IStorage::TYPE_ALL)))
	{
		dbg_msg("ChangeLog", "unable to open '%s'", pChangelogFilename);
		return;
	}
	const uint32_t MaxLinesPerPage = Config()->m_SvChangeLogMaxLinesPerPage;
	uint32_t AddedLines = 0;

	icArray<char, 8> SamePageItemStartChars = {
		' ',
		'-',
	};

	while(const char *pLine = LineReader.Get())
	{
		if(pLine[0] == 0)
			continue;

		bool ThisLineIsPartOfPrevious = pLine[0] == ' ';
		bool ImplicitNewPage = str_comp(pLine, "<page>") == 0;
		bool ExplicitNewPage = m_aChangeLogPageIndices.IsEmpty() || (AddedLines >= MaxLinesPerPage);
		ExplicitNewPage = ExplicitNewPage || !SamePageItemStartChars.Contains(pLine[0]);
		if(ImplicitNewPage || ExplicitNewPage)
		{
			if(m_aChangeLogPageIndices.Size() == m_aChangeLogPageIndices.Capacity())
			{
				dbg_msg("ChangeLog", "ChangeLog truncated: only %zu pages allowed", m_aChangeLogPageIndices.Capacity());
				break;
			}
			if(ThisLineIsPartOfPrevious && !m_aChangeLogEntries.IsEmpty())
			{
				m_aChangeLogPageIndices.Add(m_aChangeLogEntries.Size() - 1);
			}
			else
			{
				m_aChangeLogPageIndices.Add(m_aChangeLogEntries.Size());
			}
			AddedLines = 0;
		}
		if(ImplicitNewPage)
		{
			continue;
		}

		if(m_aChangeLogEntries.Size() == m_aChangeLogEntries.Capacity())
		{
			dbg_msg("ChangeLog", "ChangeLog truncated: only %zu lines allowed", m_aChangeLogEntries.Capacity());
			break;
		}
		m_aChangeLogEntries.Add(pLine);
		++AddedLines;
	}
}

bool CGameContext::IsPaused() const
{
	return m_World.m_Paused;
}

void CGameContext::SetPaused(bool Paused)
{
	if(m_pController->IsGameOver())
		return;

	m_World.m_Paused = Paused;
}

bool CGameContext::MapExists(const char *pMapName) const
{
	char aMapFilename[128];
	str_format(aMapFilename, sizeof(aMapFilename), "%s.map", pMapName);

	char aBuf[512];
	return Storage()->FindFile(aMapFilename, "maps", IStorage::TYPE_ALL, aBuf, sizeof(aBuf));
}

void CGameContext::SendBroadcast(int To, const char *pText, EBroadcastPriority Priority, int LifeSpan)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_CLIENTS : To + 1);

	// only for server demo record
	if(To < 0)
	{
		CNetMsg_Sv_Broadcast Msg;
		Msg.m_pMessage = pText;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);
	}

	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
			AddBroadcast(i, pText, Priority, LifeSpan);
	}
}

void CGameContext::ClearBroadcast(int To, EBroadcastPriority Priority)
{
	SendBroadcast(To, "", Priority, BROADCAST_DURATION_REALTIME);
}

const char *CGameContext::GetChatCategoryPrefix(int Category)
{
	switch(Category)
	{
	case CHATCATEGORY_INFECTION:
		return "☣ | ";
	case CHATCATEGORY_SCORE:
		return "★ | ";
	case CHATCATEGORY_PLAYER:
		return "♟ | ";
	case CHATCATEGORY_INFECTED:
		return "⛃ | ";
	case CHATCATEGORY_HUMANS:
		return "⛁ | ";
	case CHATCATEGORY_ACCUSATION:
		return "☹ | ";
	default:
		break;
	}

	return "";
}

void CGameContext::SendBroadcast_Localization(int To, EBroadcastPriority Priority, int LifeSpan, const char *pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_CLIENTS : To + 1);

	std::string Buffer;

	va_list VarArgs;
	va_start(VarArgs, pText);

	// only for server demo record
	if(To < 0)
	{
		CNetMsg_Sv_Broadcast Msg;
		Buffer.append(Server()->Localization()->Format_VL(Config()->m_InfDefaultLanguageCode, pText, VarArgs));
		Msg.m_pMessage = Buffer.c_str();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);
	}

	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i] && !m_apPlayers[i]->IsBot())
		{
			Buffer.clear();
			Buffer.append(Server()->Localization()->Format_VL(m_apPlayers[i]->GetLanguage(), pText, VarArgs));
			AddBroadcast(i, Buffer.c_str(), Priority, LifeSpan);
		}
	}

	va_end(VarArgs);
}

void CGameContext::SendBroadcast_Localization_P(int To, EBroadcastPriority Priority, int LifeSpan, int Number, const char *pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_CLIENTS : To + 1);

	std::string Buffer;

	va_list VarArgs;
	va_start(VarArgs, pText);

	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i] && !m_apPlayers[i]->IsBot())
		{
			Buffer.append(Server()->Localization()->Format_VLP(m_apPlayers[i]->GetLanguage(), Number, pText, VarArgs));
			AddBroadcast(i, Buffer.c_str(), Priority, LifeSpan);
		}
	}

	va_end(VarArgs);
}

/* INFECTION MODIFICATION END *****************************************/

void CGameContext::SendChat(int ChatterClientId, int Team, const char *pText, int SpamProtectionClientId)
{
	if(SpamProtectionClientId >= 0 && SpamProtectionClientId < MAX_CLIENTS)
		if(ProcessSpamProtection(SpamProtectionClientId))
			return;

	char aBuf[256], aText[256];
	str_copy(aText, pText);
	if(ChatterClientId >= 0 && ChatterClientId < MAX_CLIENTS)
		str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", ChatterClientId, Team, Server()->ClientName(ChatterClientId), pText);
	else if(ChatterClientId == -2)
	{
		str_format(aBuf, sizeof(aBuf), "### %s", aText);
		str_copy(aText, aBuf);
		ChatterClientId = -1;
	}
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", aText);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, Team != CHAT_ALL ? "teamchat" : "chat", aBuf);

	if(aText[0] == '!' && Config()->m_SvFilterChatCommands)
	{
		return;
	}

	if(ChatterClientId >= 0)
	{
		const char *pCallbackId = Team == CGameContext::CHAT_ALL ? "on_chat_message" : "on_teamchat_message";
		RunCallback(Lua()->GetLuaState(), pCallbackId, ChatterClientId, std::string(aText));
	}

	if(SpamProtectionClientId < 0)
		SpamProtectionClientId = ChatterClientId;

	if(Team == CGameContext::CHAT_ALL)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientId = ChatterClientId;
		Msg.m_pMessage = aText;

		// pack one for the recording only
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, SERVER_DEMO_CLIENT);

		// send to the clients that did not mute chatter
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if((SpamProtectionClientId < 0 || SpamProtectionClientId >= MAX_CLIENTS) || (m_apPlayers[i] && !CGameContext::m_ClientMuted[i][SpamProtectionClientId]))
			{
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
			}
		}
	}
	else
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 1;
		Msg.m_ClientId = ChatterClientId;
		Msg.m_pMessage = aText;

		// pack one for the recording only
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, SERVER_DEMO_CLIENT);

		// send to the clients
		for(int i = 0; i < Server()->MaxClients(); i++)
		{
			if(m_apPlayers[i] != nullptr)
			{
				if(Team == CHAT_SPEC)
				{
					if(m_apPlayers[i]->GetTeam() == CHAT_SPEC)
					{
						Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
					}
				}
				else
				{
					if(m_pController->GetPlayerTeam(i) == Team && m_apPlayers[i]->GetTeam() != CHAT_SPEC)
					{
						Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
					}
				}
			}
		}
	}
}

void CGameContext::SendEmoticon(int ClientId, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientId = ClientId;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientId, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CGameContext::SendMotd(int ClientId)
{
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = Config()->m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CGameContext::SendKillMessage(int Killer, int Victim, int Weapon, int ModeSpecial)
{
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = Victim;
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

//
void CGameContext::StartVote(const char *pDesc, const char *pCommand, const char *pReason)
{
	const char *pSixupDesc = pDesc;
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	// reset votes
	m_VoteEnforce = VOTE_ENFORCE_UNKNOWN;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->m_Vote = 0;
			m_apPlayers[i]->m_VotePos = 0;
		}
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "Starting vote \"%s\" for command \"%s\"", pDesc, pCommand);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "vote", aBuf);

	// start vote
	m_VoteCloseTime = time_get() + time_freq() * g_Config.m_SvVoteTime;
	str_copy(m_aVoteDescription, pDesc);
	str_copy(m_aSixupVoteDescription, pSixupDesc, sizeof(m_aSixupVoteDescription));
	str_copy(m_aVoteCommand, pCommand);
	str_copy(m_aVoteReason, pReason);
	SendVoteSet(-1);
	m_VoteUpdate = true;
}

void CGameContext::EndVote()
{
	if(m_VoteCloseTime == 0)
		return;

	{
		char aBuf[256];
		const auto GetVoteDisplayChar = [](int Vote) -> char {
			if(Vote > 0)
				return 'y';
			if(Vote < 0)
				return 'n';

			return 'i';
		};

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!m_apPlayers[i] || m_apPlayers[i]->IsBot())
				continue;

			if(m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS) // don't count in votes by spectators
				continue;

			if(m_VoteEnforce != VOTE_ENFORCE_UNKNOWN)
			{
				// If the vote led to a decision then skip those who abstain
				if(m_apPlayers[i]->m_Vote == 0)
					continue;
			}
			str_format(aBuf, sizeof(aBuf), "cid=%d vote=%c name=\"%s\"", i, GetVoteDisplayChar(m_apPlayers[i]->m_Vote), Server()->ClientName(i));
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "vote", aBuf);
		}
	}

	m_VoteCloseTime = 0;
	SendVoteSet(-1);
}

void CGameContext::SendVoteSet(int ClientId)
{
	::CNetMsg_Sv_VoteSet Msg6;
	protocol7::CNetMsg_Sv_VoteSet Msg7;

	Msg7.m_ClientId = m_VoteCreator;
	if(m_VoteCloseTime)
	{
		Msg6.m_Timeout = Msg7.m_Timeout = (m_VoteCloseTime - time_get()) / time_freq();
		Msg6.m_pDescription = m_aVoteDescription;
		Msg7.m_pDescription = m_aSixupVoteDescription;
		Msg6.m_pReason = Msg7.m_pReason = m_aVoteReason;

		int &Type = (Msg7.m_Type = protocol7::VOTE_UNKNOWN);
		if(IsKickVote())
			Type = protocol7::VOTE_START_KICK;
		else if(IsSpecVote())
			Type = protocol7::VOTE_START_SPEC;
		else if(IsOptionVote())
			Type = protocol7::VOTE_START_OP;
	}
	else
	{
		Msg6.m_Timeout = Msg7.m_Timeout = 0;
		Msg6.m_pDescription = Msg7.m_pDescription = "";
		Msg6.m_pReason = Msg7.m_pReason = "";

		int &Type = (Msg7.m_Type = protocol7::VOTE_UNKNOWN);
		if(m_VoteEnforce == VOTE_ENFORCE_NO || m_VoteEnforce == VOTE_ENFORCE_NO_ADMIN)
			Type = protocol7::VOTE_END_FAIL;
		else if(m_VoteEnforce == VOTE_ENFORCE_YES || m_VoteEnforce == VOTE_ENFORCE_YES_ADMIN)
			Type = protocol7::VOTE_END_PASS;
		else if(m_VoteEnforce == VOTE_ENFORCE_ABORT || m_VoteEnforce == VOTE_ENFORCE_CANCEL)
			Type = protocol7::VOTE_END_ABORT;

		if(m_VoteEnforce == VOTE_ENFORCE_NO_ADMIN || m_VoteEnforce == VOTE_ENFORCE_YES_ADMIN)
			Msg7.m_ClientId = -1;
	}

	if(ClientId == -1)
	{
		for(int i = 0; i < Server()->MaxClients(); i++)
		{
			if(!m_apPlayers[i])
				continue;
			if(!Server()->IsSixup(i))
				Server()->SendPackMsg(&Msg6, MSGFLAG_VITAL, i);
			else
				Server()->SendPackMsg(&Msg7, MSGFLAG_VITAL, i);
		}
	}
	else
	{
		if(!Server()->IsSixup(ClientId))
			Server()->SendPackMsg(&Msg6, MSGFLAG_VITAL, ClientId);
		else
			Server()->SendPackMsg(&Msg7, MSGFLAG_VITAL, ClientId);
	}
}

void CGameContext::SendVoteStatus(int ClientId, int Total, int Yes, int No)
{
	if(ClientId == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(Server()->ClientIngame(i))
				SendVoteStatus(i, Total, Yes, No);
		return;
	}

	if(Total > VANILLA_MAX_CLIENTS && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetClientVersion() <= VERSION_DDRACE)
	{
		Yes = (Yes * VANILLA_MAX_CLIENTS) / static_cast<float>(Total);
		No = (No * VANILLA_MAX_CLIENTS) / static_cast<float>(Total);
		Total = VANILLA_MAX_CLIENTS;
	}

	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes + No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CGameContext::AbortVoteKickOnDisconnect(int ClientId)
{
	if(m_VoteCloseTime && ((!str_comp_num(m_aVoteCommand, "kick ", 5) && str_toint(&m_aVoteCommand[5]) == ClientId) ||
							  (!str_comp_num(m_aVoteCommand, "set_team ", 9) && str_toint(&m_aVoteCommand[9]) == ClientId)))
		m_VoteCloseTime = -1;

	if(m_VoteCloseTime && m_VoteBanClientId == ClientId)
	{
		m_VoteCloseTime = -1;
		m_VoteBanClientId = -1;
	}
}

void CGameContext::RequestVotesUpdate()
{
	m_VoteUpdate = true;
}

bool CGameContext::HasActiveVote() const
{
	return m_VoteCloseTime;
}

void CGameContext::CheckPureTuning()
{
	// might not be created yet during start up
	if(!m_pController)
		return;

	if(str_comp(m_pController->GameType(), "DM") == 0 ||
		str_comp(m_pController->GameType(), "TDM") == 0 ||
		str_comp(m_pController->GameType(), "CTF") == 0)
	{
		CTuningParams p;
		if(mem_comp(&p, &m_Tuning, sizeof(p)) != 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "resetting tuning due to pure server");
			m_Tuning = p;
		}
	}
}

void CGameContext::SendTuningParams(int ClientId)
{
	if(ClientId == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(m_apPlayers[i])
			{
				SendTuningParams(i);
			}
		}
		return;
	}

	CheckPureTuning();

	SendTuningParams(ClientId, m_Tuning);
}

void CGameContext::SendTuningParams(int ClientId, const CTuningParams &params)
{
	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	const int *pParams = reinterpret_cast<const int *>(&params);

	for(unsigned i = 0; i < sizeof(m_Tuning) / sizeof(int); i++)
	{
		if(m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
		{
			if((i == 30) // laser_damage is removed from 0.7
				&& (Server()->IsSixup(ClientId)))
			{
				continue;
			}
			else if((i == 31) // collision
					&& (m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_SOLO || m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOCOLL))
			{
				Msg.AddInt(0);
			}
			else if((i == 32) // hooking
					&& (m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_SOLO || m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOHOOK))
			{
				Msg.AddInt(0);
			}
			else if((i == 3) // ground jump impulse
					&& m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOJUMP)
			{
				Msg.AddInt(0);
			}
			else if((i == 33) // jetpack
					&& !(m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_JETPACK))
			{
				Msg.AddInt(0);
			}
			else if((i == 36) // hammer hit
					&& m_apPlayers[ClientId]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOHAMMER)
			{
				Msg.AddInt(0);
			}
			else
			{
				Msg.AddInt(pParams[i]);
			}
		}
		else
			Msg.AddInt(pParams[i]); // if everything is normal just send true tunings
	}
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CGameContext::SendHitSound(int ClientId)
{
	if(m_aHitSoundState[ClientId] < 1)
	{
		m_aHitSoundState[ClientId] = 1;
	}
}

void CGameContext::SendScoreSound(int ClientId)
{
	m_aHitSoundState[ClientId] = 2;
}

void CGameContext::OnPreTickTeehistorian()
{
}

bool CGameContext::OnClientDDNetVersionKnown(int ClientId)
{
	IServer::CClientInfo Info;
	dbg_assert(Server()->GetClientInfo(ClientId, &Info), "failed to get client info");
	int ClientVersion = Info.m_DDNetVersion;
	int InfClassVersion = ClientVersion ? Server()->GetClientInfclassVersion(ClientId) : 0;
	dbg_msg("ddnet", "cid=%d version=%d inf=%d", ClientId, ClientVersion, InfClassVersion);

	const int MaxVersion = Config()->m_SvMaxDDNetVersion;
	if(MaxVersion && (ClientVersion > MaxVersion))
	{
		constexpr int BanDuration = 60 * 60 * 24;
		Server()->Ban(ClientId, BanDuration, "unsupported client");
		return true;
	}

	// Autoban known bot versions.
	if(g_Config.m_SvBannedVersions[0] != '\0' && IsVersionBanned(ClientVersion))
	{
		Server()->Kick(ClientId, "unsupported client");
		return true;
	}

	return false;
}

void CGameContext::OnTick()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			// Show top10
			if(!Server()->GetClientMemory(i, CLIENTMEMORY_TOP10))
			{
				if(!g_Config.m_SvMotd[0] || Server()->GetClientMemory(i, CLIENTMEMORY_ROUNDSTART_OR_MAPCHANGE))
					Server()->SetClientMemory(i, CLIENTMEMORY_TOP10, true);
			}
		}
	}

	// Check for banvote
	if(!m_VoteCloseTime)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(Server()->ClientShouldBeBanned(i))
			{
				char aDesc[VOTE_DESC_LENGTH] = {0};
				char aCmd[VOTE_CMD_LENGTH] = {0};
				str_format(aCmd, sizeof(aCmd), "ban %d %d Banned by vote", i, g_Config.m_SvVoteKickBantime * 3);
				str_format(aDesc, sizeof(aDesc), "Ban \"%s\"", Server()->ClientName(i));
				m_VoteBanClientId = i;
				m_VoteType = VOTE_TYPE_KICK;
				StartVote(aDesc, aCmd, "");
				continue;
			}
		}
	}

	// Check for mapVote
	if(!m_VoteCloseTime && m_pController->CanVote()) // there is currently no vote && its the start of a round
	{
		IServer::CMapVote *mapVote = Server()->GetMapVote();
		if(mapVote)
		{
			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
				_("Starting vote '{str:Desc}'"), "Desc", mapVote->m_pDesc, nullptr);
			StartVote(mapVote->m_pDesc, mapVote->m_pCommand, mapVote->m_pReason);
		}
	}

	// check tuning
	CheckPureTuning();

	m_Collision.SetTime(m_pController->GetTime());
	m_World.SetGameTick(Server()->Tick());

	m_pController->TickBeforeWorld();

	// copy tuning
	m_World.m_Core.m_Tuning = m_Tuning;
	m_World.Tick();

	UpdatePlayerMaps();

	// if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	int NumActivePlayers = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			// send vote options
			ProgressVoteOptions(i);

			if(m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				NumActivePlayers++;

			Server()->RoundStatistics()->UpdatePlayer(i, m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS);

			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();

			if(m_VoteLanguageTick[i] > 0)
			{
				if(m_VoteLanguageTick[i] == 1)
				{
					m_VoteLanguageTick[i] = 0;

					CNetMsg_Sv_VoteSet Msg;
					Msg.m_Timeout = 0;
					Msg.m_pDescription = "";
					Msg.m_pReason = "";
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);

					str_copy(m_VoteLanguage[i], Config()->m_InfDefaultLanguageCode);
				}
				else
				{
					m_VoteLanguageTick[i]--;
				}
			}
		}
	}

	// Check for new broadcast
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			if(m_BroadcastStates[i].m_LifeSpanTick > 0 && m_BroadcastStates[i].m_TimedPriority > m_BroadcastStates[i].m_Priority)
			{
				str_copy(m_BroadcastStates[i].m_NextMessage, m_BroadcastStates[i].m_TimedMessage);
			}

			// Send broadcast only if the message is different, or to fight auto-fading
			if(
				str_comp(m_BroadcastStates[i].m_PrevMessage, m_BroadcastStates[i].m_NextMessage) != 0 ||
				m_BroadcastStates[i].m_NoChangeTick > Server()->TickSpeed())
			{
				CNetMsg_Sv_Broadcast Msg;
				Msg.m_pMessage = m_BroadcastStates[i].m_NextMessage;
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);

				str_copy(m_BroadcastStates[i].m_PrevMessage, m_BroadcastStates[i].m_NextMessage);

				m_BroadcastStates[i].m_NoChangeTick = 0;
			}
			else
				m_BroadcastStates[i].m_NoChangeTick++;

			// Update broadcast state
			if(m_BroadcastStates[i].m_LifeSpanTick > 0)
				m_BroadcastStates[i].m_LifeSpanTick--;

			if(m_BroadcastStates[i].m_LifeSpanTick <= 0)
			{
				m_BroadcastStates[i].m_TimedMessage[0] = 0;
				m_BroadcastStates[i].m_TimedPriority = EBroadcastPriority::LOWEST;
			}
			m_BroadcastStates[i].m_NextMessage[0] = 0;
			m_BroadcastStates[i].m_Priority = EBroadcastPriority::LOWEST;
		}
		else
		{
			m_BroadcastStates[i].m_NoChangeTick = 0;
			m_BroadcastStates[i].m_LifeSpanTick = 0;
			m_BroadcastStates[i].m_Priority = EBroadcastPriority::LOWEST;
			m_BroadcastStates[i].m_TimedPriority = EBroadcastPriority::LOWEST;
			m_BroadcastStates[i].m_PrevMessage[0] = 0;
			m_BroadcastStates[i].m_NextMessage[0] = 0;
			m_BroadcastStates[i].m_TimedMessage[0] = 0;
		}
	}

	// Send score and hit sound
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			int Sound = -1;
			if(m_aHitSoundState[i] == 1)
				Sound = SOUND_HIT;
			else if(m_aHitSoundState[i] == 2)
				Sound = SOUND_CTF_GRAB_PL;

			if(Sound >= 0)
			{
				CClientMask Mask;
				Mask.set(i);
				for(int j = 0; j < MAX_CLIENTS; j++)
				{
					if(m_apPlayers[j] && m_apPlayers[j]->GetTeam() == TEAM_SPECTATORS && m_apPlayers[j]->m_SpectatorId == i)
						Mask.set(j);
				}
				CreateSound(m_apPlayers[i]->m_ViewPos, Sound, Mask);
			}
		}

		m_aHitSoundState[i] = 0;
	}

	Server()->RoundStatistics()->UpdateNumberOfPlayers(NumActivePlayers);

	/* INFECTION MODIFICATION START ***************************************/
	// Clean old dots
	int DotIter;

	DotIter = 0;
	while(DotIter < m_LaserDots.size())
	{
		m_LaserDots[DotIter].m_LifeSpan--;
		if(m_LaserDots[DotIter].m_LifeSpan <= 0)
		{
			Server()->SnapFreeId(m_LaserDots[DotIter].m_SnapId);
			m_LaserDots.remove_index(DotIter);
		}
		else
			DotIter++;
	}

	DotIter = 0;
	while(DotIter < m_HammerDots.size())
	{
		m_HammerDots[DotIter].m_LifeSpan--;
		if(m_HammerDots[DotIter].m_LifeSpan <= 0)
		{
			Server()->SnapFreeId(m_HammerDots[DotIter].m_SnapId);
			m_HammerDots.remove_index(DotIter);
		}
		else
			DotIter++;
	}

	DotIter = 0;
	while(DotIter < m_LoveDots.size())
	{
		m_LoveDots[DotIter].m_LifeSpan--;
		m_LoveDots[DotIter].m_Pos.y -= 5.0f;
		if(m_LoveDots[DotIter].m_LifeSpan <= 0)
		{
			Server()->SnapFreeId(m_LoveDots[DotIter].m_SnapId);
			m_LoveDots.remove_index(DotIter);
		}
		else
			DotIter++;
	}
	/* INFECTION MODIFICATION END *****************************************/

	// update voting
	if(m_VoteCloseTime)
	{
		// abort the kick-vote on player-leave
		if(m_VoteCloseTime == -1)
		{
			EndVote();
			SendChat(-1, CGameContext::CHAT_ALL, "Vote aborted");
		}
		else
		{
			int Total = 0, Yes = 0, No = 0;
			if(m_VoteUpdate)
			{
				// count votes
				char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}};
				for(int i = 0; i < MAX_CLIENTS; i++)
					if(m_apPlayers[i])
						Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
				bool aVoteChecked[MAX_CLIENTS] = {0};
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!m_apPlayers[i] || aVoteChecked[i])
						continue;

					if(m_apPlayers[i]->IsBot())
						continue;

					if(m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS) // don't count in votes by spectators
						continue;

					int ActVote = m_apPlayers[i]->m_Vote;
					int ActVotePos = m_apPlayers[i]->m_VotePos;

					// check for more players with the same ip (only use the vote of the one who voted first)
					for(int j = i + 1; j < MAX_CLIENTS; j++)
					{
						if(!m_apPlayers[j] || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]) != 0)
							continue;

						aVoteChecked[j] = true;
						if(m_apPlayers[j]->m_Vote && (!ActVote || ActVotePos > m_apPlayers[j]->m_VotePos))
						{
							ActVote = m_apPlayers[j]->m_Vote;
							ActVotePos = m_apPlayers[j]->m_VotePos;
						}
					}

					Total++;
					if(ActVote > 0)
						Yes++;
					else if(ActVote < 0)
						No++;
				}

				if(Yes >= Total / 2 + 1)
					m_VoteEnforce = VOTE_ENFORCE_YES;
				else if(No >= (Total + 1) / 2)
					m_VoteEnforce = VOTE_ENFORCE_NO;
			}

			if(m_VoteEnforce == VOTE_ENFORCE_YES)
			{
				if(m_VoteBanClientId >= 0)
				{
					Server()->RemoveAccusations(m_VoteBanClientId);
					m_VoteBanClientId = -1;
				}

				Server()->SetRconCid(IServer::RCON_CID_VOTE);
				Console()->ExecuteLine(m_aVoteCommand);
				Server()->SetRconCid(IServer::RCON_CID_SERV);
				EndVote();
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Vote passed"), nullptr);
				if(GetOptionVoteType(m_aVoteCommand) & MAP_VOTE_BITS)
					Server()->ResetMapVotes();

				if(m_apPlayers[m_VoteCreator])
					m_apPlayers[m_VoteCreator]->m_LastVoteCall = 0;
			}
			else if(m_VoteEnforce == VOTE_ENFORCE_NO || time_get() > m_VoteCloseTime)
			{
				EndVote();
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Vote failed"), nullptr);
				if(GetOptionVoteType(m_aVoteCommand) & MAP_VOTE_BITS)
					Server()->ResetMapVotes();

				// Remove accusation if needed
				if(m_VoteBanClientId >= 0)
				{
					Server()->RemoveAccusations(m_VoteBanClientId);
					m_VoteBanClientId = -1;
				}
			}
			else if(m_VoteUpdate)
			{
				m_VoteUpdate = false;
				SendVoteStatus(-1, Total, Yes, No);
			}
		}
	}

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies; i++)
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i & 1) ? -1 : 1;
			m_apPlayers[MAX_CLIENTS - i - 1]->OnPredictedInput(&Input);
		}
	}
#endif
}

// Server hooks
void CGameContext::OnClientPrepareInput(int ClientId, void *pInput)
{
	auto *pPlayerInput = static_cast<CNetObj_PlayerInput *>(pInput);
	if(Server()->IsSixup(ClientId))
		pPlayerInput->m_PlayerFlags = PlayerFlags_SevenToSix(pPlayerInput->m_PlayerFlags);
}

void CGameContext::OnClientDirectInput(int ClientId, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientId]->OnDirectInput(static_cast<CNetObj_PlayerInput *>(pInput));

	int Flags = static_cast<CNetObj_PlayerInput *>(pInput)->m_PlayerFlags;
	if((Flags & 256) || (Flags & 512))
	{
		Server()->Kick(ClientId, "please update your client or use DDNet client");
	}
}

void CGameContext::OnClientPredictedInput(int ClientId, void *pInput)
{
	// early return if no input at all has been sent by a player
	if(pInput == nullptr && !m_aPlayerHasInput[ClientId])
		return;

	// set to last sent input when no new input has been sent
	const CNetObj_PlayerInput *pApplyInput = static_cast<CNetObj_PlayerInput *>(pInput);
	if(pApplyInput == nullptr)
	{
		pApplyInput = &m_aLastPlayerInput[ClientId];
	}

	if(!m_World.m_Paused)
		m_apPlayers[ClientId]->OnPredictedInput(pApplyInput);
}

void CGameContext::OnClientPredictedEarlyInput(int ClientId, void *pInput)
{
	// early return if no input at all has been sent by a player
	if(pInput == nullptr && !m_aPlayerHasInput[ClientId])
		return;

	// set to last sent input when no new input has been sent
	CNetObj_PlayerInput *pApplyInput = static_cast<CNetObj_PlayerInput *>(pInput);
	if(pApplyInput == nullptr)
	{
		pApplyInput = &m_aLastPlayerInput[ClientId];
	}
	else
	{
		// Store input in this function and not in `OnClientPredictedInput`,
		// because this function is called on all inputs, while
		// `OnClientPredictedInput` is only called on the first input of each
		// tick.
		mem_copy(&m_aLastPlayerInput[ClientId], pApplyInput, sizeof(m_aLastPlayerInput[ClientId]));
		m_aPlayerHasInput[ClientId] = true;
	}

	if(!m_World.m_Paused)
		m_apPlayers[ClientId]->OnPredictedEarlyInput(pApplyInput);
}

struct CVoteOptionServer *CGameContext::GetVoteOption(int Index)
{
	CVoteOptionServer *pCurrent;
	for(pCurrent = m_pVoteOptionFirst;
		Index > 0 && pCurrent;
		Index--, pCurrent = pCurrent->m_pNext)
		;

	if(Index > 0)
		return nullptr;
	return pCurrent;
}

void CGameContext::ProgressVoteOptions(int ClientId)
{
	CPlayer *pPl = m_apPlayers[ClientId];

	if(pPl->m_SendVoteIndex == -1)
		return; // we didn't start sending options yet

	if(pPl->m_SendVoteIndex > m_NumVoteOptions)
		return; // shouldn't happen / fail silently

	int VotesLeft = m_NumVoteOptions - pPl->m_SendVoteIndex;
	int NumVotesToSend = minimum(g_Config.m_SvSendVotesPerTick, VotesLeft);

	if(!VotesLeft)
	{
		// player has up to date vote option list
		return;
	}

	// build vote option list msg
	int CurIndex = 0;

	CNetMsg_Sv_VoteOptionListAdd OptionMsg;
	OptionMsg.m_pDescription0 = "";
	OptionMsg.m_pDescription1 = "";
	OptionMsg.m_pDescription2 = "";
	OptionMsg.m_pDescription3 = "";
	OptionMsg.m_pDescription4 = "";
	OptionMsg.m_pDescription5 = "";
	OptionMsg.m_pDescription6 = "";
	OptionMsg.m_pDescription7 = "";
	OptionMsg.m_pDescription8 = "";
	OptionMsg.m_pDescription9 = "";
	OptionMsg.m_pDescription10 = "";
	OptionMsg.m_pDescription11 = "";
	OptionMsg.m_pDescription12 = "";
	OptionMsg.m_pDescription13 = "";
	OptionMsg.m_pDescription14 = "";

	// get current vote option by index
	CVoteOptionServer *pCurrent = GetVoteOption(pPl->m_SendVoteIndex);

	while(CurIndex < NumVotesToSend && pCurrent != nullptr)
	{
		switch(CurIndex)
		{
		case 0: OptionMsg.m_pDescription0 = pCurrent->m_aDescription; break;
		case 1: OptionMsg.m_pDescription1 = pCurrent->m_aDescription; break;
		case 2: OptionMsg.m_pDescription2 = pCurrent->m_aDescription; break;
		case 3: OptionMsg.m_pDescription3 = pCurrent->m_aDescription; break;
		case 4: OptionMsg.m_pDescription4 = pCurrent->m_aDescription; break;
		case 5: OptionMsg.m_pDescription5 = pCurrent->m_aDescription; break;
		case 6: OptionMsg.m_pDescription6 = pCurrent->m_aDescription; break;
		case 7: OptionMsg.m_pDescription7 = pCurrent->m_aDescription; break;
		case 8: OptionMsg.m_pDescription8 = pCurrent->m_aDescription; break;
		case 9: OptionMsg.m_pDescription9 = pCurrent->m_aDescription; break;
		case 10: OptionMsg.m_pDescription10 = pCurrent->m_aDescription; break;
		case 11: OptionMsg.m_pDescription11 = pCurrent->m_aDescription; break;
		case 12: OptionMsg.m_pDescription12 = pCurrent->m_aDescription; break;
		case 13: OptionMsg.m_pDescription13 = pCurrent->m_aDescription; break;
		case 14: OptionMsg.m_pDescription14 = pCurrent->m_aDescription; break;
		}

		CurIndex++;
		pCurrent = pCurrent->m_pNext;
	}

	// send msg
	if(pPl->m_SendVoteIndex == 0)
	{
		CNetMsg_Sv_VoteOptionGroupStart StartMsg;
		Server()->SendPackMsg(&StartMsg, MSGFLAG_VITAL, ClientId);
	}

	OptionMsg.m_NumOptions = NumVotesToSend;
	Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, ClientId);

	pPl->m_SendVoteIndex += NumVotesToSend;

	if(pPl->m_SendVoteIndex == m_NumVoteOptions)
	{
		CNetMsg_Sv_VoteOptionGroupEnd EndMsg;
		Server()->SendPackMsg(&EndMsg, MSGFLAG_VITAL, ClientId);
	}
}

void CGameContext::OnClientEnter(int ClientId)
{
	IServer::CClientInfo Info;
	if(Server()->GetClientInfo(ClientId, &Info) && Info.m_GotDDNetVersion)
	{
		if(OnClientDDNetVersionKnown(ClientId))
			return; // kicked
	}

	m_pController->OnPlayerConnect(m_apPlayers[ClientId]);

	m_apPlayers[ClientId]->m_IsInGame = true;

	{
		CNetMsg_Sv_CommandInfoGroupStart Msg;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
	}
	for(const IConsole::CCommandInfo *pCmd = Console()->FirstCommandInfo(EAccessLevel::USER, CFGFLAG_CHAT);
		pCmd; pCmd = pCmd->NextCommandInfo(EAccessLevel::USER, CFGFLAG_CHAT))
	{
		const char *pName = pCmd->m_pName;

		if(Server()->IsSixup(ClientId))
		{
			if(!str_comp_nocase(pName, "w") || !str_comp_nocase(pName, "whisper"))
				continue;

			protocol7::CNetMsg_Sv_CommandInfo Msg;
			Msg.m_pName = pName;
			Msg.m_pArgsFormat = pCmd->m_pParams;
			Msg.m_pHelpText = pCmd->m_pHelp;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}
		else
		{
			CNetMsg_Sv_CommandInfo Msg;
			Msg.m_pName = pName;
			Msg.m_pArgsFormat = pCmd->m_pParams;
			Msg.m_pHelpText = pCmd->m_pHelp;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}
	}
	{
		CNetMsg_Sv_CommandInfoGroupEnd Msg;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
	}

	m_VoteUpdate = true;

	// send active vote
	if(m_VoteCloseTime)
		SendVoteSet(ClientId);

	Server()->ExpireServerInfo();

	mem_zero(&m_aLastPlayerInput[ClientId], sizeof(m_aLastPlayerInput[ClientId]));
	m_aPlayerHasInput[ClientId] = false;

	// initial chat delay
	if(Config()->m_SvChatInitialDelay != 0 && !Server()->ClientPrevIngame(ClientId))
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("This server has an initial chat delay, you will need to wait {int:Sec} seconds before talking."),
			"Sec", &Config()->m_SvChatInitialDelay, nullptr);
		Server()->Mute(ClientId, Config()->m_SvChatInitialDelay, "Initial chat delay", SUPPRESS_CHAT_MESSAGE);
	}
}

bool CGameContext::OnClientDataPersist(int ClientId, void *pData)
{
	CPersistentClientData *pPersistent = static_cast<CPersistentClientData *>(pData);
	const CPlayer *pPlayer = m_apPlayers[ClientId];
	if(!pPlayer)
	{
		return false;
	}

	pPersistent->m_IsSpectator = pPlayer->GetTeam() == TEAM_SPECTATORS;
	pPersistent->m_ClientNameLocked = pPlayer->m_ClientNameLocked;

	return m_pController->GetClientPersistentData(ClientId, pData);
}

void CGameContext::OnClientConnected(int ClientId, void *pData)
{
	CPersistentClientData *pPersistentData = static_cast<CPersistentClientData *>(pData);
	bool Spec = false;
	bool NameLocked = false;
	if(pPersistentData)
	{
		Spec = pPersistentData->m_IsSpectator;
		NameLocked = pPersistentData->m_ClientNameLocked;
	}

	{
		bool Empty = true;
		for(auto &pPlayer : m_apPlayers)
		{
			// connecting clients with spoofed ips can clog slots without being ingame
			if(pPlayer && Server()->ClientIngame(pPlayer->GetCid()))
			{
				Empty = false;
				break;
			}
		}
		if(Empty)
		{
			m_NonEmptySince = Server()->Tick();
		}
	}

	dbg_assert(!m_apPlayers[ClientId], "non-free player slot");
	m_apPlayers[ClientId] = m_pController->CreatePlayer(ClientId, Spec, pData);
	m_apPlayers[ClientId]->m_ClientNameLocked = NameLocked;

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		if(ClientId >= MAX_CLIENTS - g_Config.m_DbgDummies)
			return;
	}
#endif

	// send motd
	if(!Server()->GetClientMemory(ClientId, CLIENTMEMORY_MOTD))
	{
		SendMotd(ClientId);
		Server()->SetClientMemory(ClientId, CLIENTMEMORY_MOTD, true);
	}

	m_BroadcastStates[ClientId].m_NoChangeTick = 0;
	m_BroadcastStates[ClientId].m_LifeSpanTick = 0;
	m_BroadcastStates[ClientId].m_Priority = EBroadcastPriority::LOWEST;
	m_BroadcastStates[ClientId].m_PrevMessage[0] = 0;
	m_BroadcastStates[ClientId].m_NextMessage[0] = 0;

	Server()->ExpireServerInfo();
}

void CGameContext::OnClientDrop(int ClientId, EClientDropType Type, const char *pReason)
{
	AbortVoteKickOnDisconnect(ClientId);
	if(!m_apPlayers[ClientId])
		return;

	m_pController->OnPlayerDisconnect(m_apPlayers[ClientId], Type, pReason);
	delete m_apPlayers[ClientId];
	m_apPlayers[ClientId] = nullptr;

	m_VoteUpdate = true;

	// update spectator modes
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer && pPlayer->m_SpectatorId == ClientId)
			pPlayer->m_SpectatorId = SPEC_FREEVIEW;
	}

	// update conversation targets
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer && pPlayer->m_LastWhisperTo == ClientId)
			pPlayer->m_LastWhisperTo = -1;
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		// remove everyone this player had muted
		CGameContext::m_ClientMuted[ClientId][i] = false;

		// reset mutes for everyone that muted this player
		CGameContext::m_ClientMuted[i][ClientId] = false;
	}

	Server()->ExpireServerInfo();
}

void CGameContext::OnClientEngineJoin(int ClientId, bool Sixup)
{
}

void CGameContext::OnClientEngineDrop(int ClientId, const char *pReason)
{
}

CGameContext::OPTION_VOTE_TYPE CGameContext::GetOptionVoteType(const char *pVoteCommand)
{
	char command[512] = {0};
	int i = 0;
	for(; i < 510; i++)
	{
		if(pVoteCommand[i] == 0)
			break;
		if(pVoteCommand[i] == ' ')
			break;
		command[i] = pVoteCommand[i];
	}
	command[++i] = 0;
	if(str_comp_nocase(command, "sv_map") == 0)
		return SV_MAP;
	if(str_comp_nocase(command, "change_map") == 0)
		return CHANGE_MAP;
	if(str_comp_nocase(command, "skip_map") == 0)
		return SKIP_MAP;
	if(str_comp_nocase(command, "adjust sv_rounds_per_map +") == 0)
		return PLAY_MORE_VOTE_TYPE;
	if(str_startswith(command, "queue_"))
		return QUEUED_VOTE;
	return OTHER_OPTION_VOTE_TYPE;
}

// copies the map name inside pCommand into pMapName
// make sure pMapName is big enough to hold the name and pCommand is null terminated
// example: input pCommand = "change_map infc_newdust", output pMapName = "infc_newdust"
void CGameContext::GetMapNameFromCommand(char *pMapName, const char *pCommand)
{
	bool readingMapName = false;
	int k = 0;
	for(int i = 0; i < 510; i++)
	{
		if(pCommand[i] == 0)
			break;
		if(pCommand[i] == ' ')
		{
			readingMapName = true;
			continue;
		}
		if(!readingMapName)
			continue;
		pMapName[k] = pCommand[i];
		k++;
	}
	pMapName[k] = 0;
}

char *CGameContext::ParseStringArgumentInplace(char *&pInput) const
{
	pInput = str_skip_whitespaces(pInput);
	char *pArgument{};

	// add token
	if(*pInput == '"')
	{
		pInput++;
		pArgument = pInput;

		char *pDst = pInput; // we might have to process escape data
		while(true)
		{
			if(pInput[0] == '"')
				break;
			else if(pInput[0] == '\\')
			{
				if(pInput[1] == '\\')
					pInput++; // skip due to escape
				else if(pInput[1] == '"')
					pInput++; // skip due to escape
			}
			else if(pInput[0] == 0)
				return nullptr; // return error

			*pDst = *pInput;
			pDst++;
			pInput++;
		}

		// write null termination
		*pDst = 0;

		pInput++;
	}
	else
	{
		pArgument = pInput;
		pInput = str_skip_to_whitespace(pInput);

		if(pInput[0] != '\0') // check for end of string
		{
			pInput[0] = '\0';
			pInput++;
		}
	}

	return pArgument;
}

std::optional<int> CGameContext::GetClientId(const char *pName) const
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(str_comp(pName, Server()->ClientName(i)) == 0)
			return i;
	}

	return std::nullopt;
}

bool CheckClientId2(int ClientId)
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS;
}

void *CGameContext::PreProcessMsg(int *pMsgId, CUnpacker *pUnpacker, int ClientId)
{
	if(Server()->IsSixup(ClientId) && *pMsgId < OFFSET_UUID)
	{
		void *pRawMsg = m_NetObjHandler7.SecureUnpackMsg(*pMsgId, pUnpacker);
		if(!pRawMsg)
			return nullptr;

		CPlayer *pPlayer = m_apPlayers[ClientId];
		static char s_aRawMsg[1024];

		if(*pMsgId == protocol7::NETMSGTYPE_CL_SAY)
		{
			protocol7::CNetMsg_Cl_Say *pMsg7 = static_cast<protocol7::CNetMsg_Cl_Say *>(pRawMsg);
			// Should probably use a placement new to start the lifetime of the object to avoid future weirdness
			::CNetMsg_Cl_Say *pMsg = reinterpret_cast<::CNetMsg_Cl_Say *>(s_aRawMsg);

			if(pMsg7->m_Mode == protocol7::CHAT_WHISPER)
			{
				if(!CheckClientId2(pMsg7->m_Target) || !Server()->ClientIngame(pMsg7->m_Target))
					return nullptr;
				if(ProcessSpamProtection(ClientId))
					return nullptr;

				WhisperId(ClientId, pMsg7->m_Target, pMsg7->m_pMessage);
				return nullptr;
			}
			else
			{
				pMsg->m_Team = pMsg7->m_Mode == protocol7::CHAT_TEAM;
				pMsg->m_pMessage = pMsg7->m_pMessage;
			}
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_STARTINFO)
		{
			protocol7::CNetMsg_Cl_StartInfo *pMsg7 = static_cast<protocol7::CNetMsg_Cl_StartInfo *>(pRawMsg);
			::CNetMsg_Cl_StartInfo *pMsg = reinterpret_cast<::CNetMsg_Cl_StartInfo *>(s_aRawMsg);

			pMsg->m_pName = pMsg7->m_pName;
			pMsg->m_pClan = pMsg7->m_pClan;
			pMsg->m_Country = pMsg7->m_Country;

			CTeeInfo Info(pMsg7->m_apSkinPartNames, pMsg7->m_aUseCustomColors, pMsg7->m_aSkinPartColors);
			Info.FromSixup();
			pPlayer->m_TeeInfos = Info;

			str_copy(s_aRawMsg + sizeof(*pMsg), Info.m_aSkinName, sizeof(s_aRawMsg) - sizeof(*pMsg));

			pMsg->m_pSkin = s_aRawMsg + sizeof(*pMsg);
			pMsg->m_UseCustomColor = pPlayer->m_TeeInfos.m_UseCustomColor;
			pMsg->m_ColorBody = pPlayer->m_TeeInfos.m_ColorBody;
			pMsg->m_ColorFeet = pPlayer->m_TeeInfos.m_ColorFeet;
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_SKINCHANGE)
		{
			protocol7::CNetMsg_Cl_SkinChange *pMsg = static_cast<protocol7::CNetMsg_Cl_SkinChange *>(pRawMsg);
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo &&
				pPlayer->m_LastChangeInfo + Server()->TickSpeed() * g_Config.m_SvInfoChangeDelay > Server()->Tick())
				return nullptr;

			pPlayer->m_LastChangeInfo = Server()->Tick();

			CTeeInfo Info(pMsg->m_apSkinPartNames, pMsg->m_aUseCustomColors, pMsg->m_aSkinPartColors);
			Info.FromSixup();
			pPlayer->m_TeeInfos = Info;

			protocol7::CNetMsg_Sv_SkinChange Msg;
			Msg.m_ClientId = ClientId;
			for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
			{
				Msg.m_apSkinPartNames[p] = pMsg->m_apSkinPartNames[p];
				Msg.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
				Msg.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
			}

			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);

			return nullptr;
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_SETSPECTATORMODE)
		{
			protocol7::CNetMsg_Cl_SetSpectatorMode *pMsg7 = static_cast<protocol7::CNetMsg_Cl_SetSpectatorMode *>(pRawMsg);
			::CNetMsg_Cl_SetSpectatorMode *pMsg = reinterpret_cast<::CNetMsg_Cl_SetSpectatorMode *>(s_aRawMsg);

			if(pMsg7->m_SpecMode == protocol7::SPEC_FREEVIEW)
				pMsg->m_SpectatorId = SPEC_FREEVIEW;
			else if(pMsg7->m_SpecMode == protocol7::SPEC_PLAYER)
				pMsg->m_SpectatorId = pMsg7->m_SpectatorId;
			else
				pMsg->m_SpectatorId = SPEC_FREEVIEW; // Probably not needed
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_SETTEAM)
		{
			protocol7::CNetMsg_Cl_SetTeam *pMsg7 = static_cast<protocol7::CNetMsg_Cl_SetTeam *>(pRawMsg);
			::CNetMsg_Cl_SetTeam *pMsg = reinterpret_cast<::CNetMsg_Cl_SetTeam *>(s_aRawMsg);

			pMsg->m_Team = pMsg7->m_Team;
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_COMMAND)
		{
			protocol7::CNetMsg_Cl_Command *pMsg7 = static_cast<protocol7::CNetMsg_Cl_Command *>(pRawMsg);
			::CNetMsg_Cl_Say *pMsg = reinterpret_cast<::CNetMsg_Cl_Say *>(s_aRawMsg);

			str_format(s_aRawMsg + sizeof(*pMsg), sizeof(s_aRawMsg) - sizeof(*pMsg), "/%s %s", pMsg7->m_pName, pMsg7->m_pArguments);
			pMsg->m_pMessage = s_aRawMsg + sizeof(*pMsg);
			pMsg->m_Team = 0;

			*pMsgId = NETMSGTYPE_CL_SAY;
			return s_aRawMsg;
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_CALLVOTE)
		{
			protocol7::CNetMsg_Cl_CallVote *pMsg7 = static_cast<protocol7::CNetMsg_Cl_CallVote *>(pRawMsg);
			::CNetMsg_Cl_CallVote *pMsg = reinterpret_cast<::CNetMsg_Cl_CallVote *>(s_aRawMsg);

			int Authed = Server()->GetAuthedState(ClientId);
			if(pMsg7->m_Force)
			{
				str_format(s_aRawMsg, sizeof(s_aRawMsg), "force_vote \"%s\" \"%s\" \"%s\"", pMsg7->m_pType, pMsg7->m_pValue, pMsg7->m_pReason);
				Console()->SetAccessLevel(Authed == AUTHED_ADMIN ? EAccessLevel::ADMIN : Authed == AUTHED_MOD ? EAccessLevel::MOD :
																												EAccessLevel::HELPER);
				Console()->ExecuteLine(s_aRawMsg, ClientId, false);
				Console()->SetAccessLevel(EAccessLevel::ADMIN);
				return nullptr;
			}

			pMsg->m_pValue = pMsg7->m_pValue;
			pMsg->m_pReason = pMsg7->m_pReason;
			pMsg->m_pType = pMsg7->m_pType;
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_EMOTICON)
		{
			protocol7::CNetMsg_Cl_Emoticon *pMsg7 = static_cast<protocol7::CNetMsg_Cl_Emoticon *>(pRawMsg);
			::CNetMsg_Cl_Emoticon *pMsg = reinterpret_cast<::CNetMsg_Cl_Emoticon *>(s_aRawMsg);

			pMsg->m_Emoticon = pMsg7->m_Emoticon;
		}
		else if(*pMsgId == protocol7::NETMSGTYPE_CL_VOTE)
		{
			protocol7::CNetMsg_Cl_Vote *pMsg7 = static_cast<protocol7::CNetMsg_Cl_Vote *>(pRawMsg);
			::CNetMsg_Cl_Vote *pMsg = reinterpret_cast<::CNetMsg_Cl_Vote *>(s_aRawMsg);

			pMsg->m_Vote = pMsg7->m_Vote;
		}

		*pMsgId = Msg_SevenToSix(*pMsgId);

		return s_aRawMsg;
	}
	else
		return m_NetObjHandler.SecureUnpackMsg(*pMsgId, pUnpacker);
}

void CGameContext::CensorMessage(char *pCensoredMessage, const char *pMessage, int Size)
{
	str_copy(pCensoredMessage, pMessage, Size);
}

bool CGameContext::MessageTriggersBanOrKick(int FromCid, const char *pMessage)
{
	const char aKrx[] = "bro, check out this client: kr​xclient";
	if(str_comp_num(pMessage, aKrx, sizeof(aKrx)) == 0)
	{
		char aAddrStr[NETADDR_MAXSTRSIZE]{};
		Server()->GetClientAddr(FromCid, aAddrStr, sizeof(aAddrStr));

		const CPlayer *pPlayer = GetPlayer(FromCid);
		const char *pTimeout = pPlayer->m_aTimeoutCode[0] ? pPlayer->m_aTimeoutCode : "<none>";

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "Auto ban IP '%s' to get rid of client '%s' (%d) with timeout code '%s' for message '%s'", aAddrStr, Server()->ClientName(FromCid), FromCid, pTimeout, pMessage);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

		Server()->Ban(FromCid, 1000 * 60, "krx");
		return true;
	}

	return false;
}

void CGameContext::OnMessage(int MsgId, CUnpacker *pUnpacker, int ClientId)
{
	void *pRawMsg = PreProcessMsg(&MsgId, pUnpacker, ClientId);

	if(!pRawMsg)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientId];
	// HACK: DDNet Client did something wrong that we can detect
	// Round and Score conditions are here only to prevent false-positif
	if(!pPlayer && Server()->GetClientNbRound(ClientId) == 0)
	{
		Server()->Kick(ClientId, "Kicked (is probably a dummy)");
		return;
	}

	if(Server()->ClientIngame(ClientId))
	{
		switch(MsgId)
		{
		case NETMSGTYPE_CL_SAY:
			OnSayNetMessage(static_cast<CNetMsg_Cl_Say *>(pRawMsg), ClientId, pUnpacker);
			break;
		case NETMSGTYPE_CL_CALLVOTE:
			OnCallVoteNetMessage(static_cast<CNetMsg_Cl_CallVote *>(pRawMsg), ClientId);
			break;
		case NETMSGTYPE_CL_VOTE:
			OnVoteNetMessage(static_cast<CNetMsg_Cl_Vote *>(pRawMsg), ClientId);
			break;
		case NETMSGTYPE_CL_SETTEAM:
			OnSetTeamNetMessage(static_cast<CNetMsg_Cl_SetTeam *>(pRawMsg), ClientId);
			break;
		case NETMSGTYPE_CL_ISDDNETLEGACY:
			OnIsDDNetLegacyNetMessage(static_cast<CNetMsg_Cl_IsDDNetLegacy *>(pRawMsg), ClientId, pUnpacker);
			break;
		case NETMSGTYPE_CL_SETSPECTATORMODE:
			OnSetSpectatorModeNetMessage(static_cast<CNetMsg_Cl_SetSpectatorMode *>(pRawMsg), ClientId);
			break;
		case NETMSGTYPE_CL_CHANGEINFO:
			OnChangeInfoNetMessage(static_cast<CNetMsg_Cl_ChangeInfo *>(pRawMsg), ClientId);
			break;
		case NETMSGTYPE_CL_EMOTICON:
			OnEmoticonNetMessage(static_cast<CNetMsg_Cl_Emoticon *>(pRawMsg), ClientId);
			break;
		case NETMSGTYPE_CL_KILL:
			OnKillNetMessage(static_cast<CNetMsg_Cl_Kill *>(pRawMsg), ClientId);
			break;
		default:
			break;
		}
	}
	if(MsgId == NETMSGTYPE_CL_STARTINFO)
	{
		OnStartInfoNetMessage(static_cast<CNetMsg_Cl_StartInfo *>(pRawMsg), ClientId);
	}
}

void CGameContext::OnSayNetMessage(const CNetMsg_Cl_Say *pMsg, int ClientId, const CUnpacker *pUnpacker)
{
	if(!str_utf8_check(pMsg->m_pMessage))
	{
		return;
	}
	CPlayer *pPlayer = m_apPlayers[ClientId];
	int Team = pMsg->m_Team;

	// trim right and set maximum length to 256 utf8-characters
	int Length = 0;
	const char *p = pMsg->m_pMessage;
	const char *pEnd = nullptr;
	while(*p)
	{
		const char *pStrOld = p;
		int Code = str_utf8_decode(&p);

		// check if unicode is not empty
		if(!str_utf8_isspace(Code))
		{
			pEnd = nullptr;
		}
		else if(pEnd == nullptr)
			pEnd = pStrOld;

		if(++Length >= 256)
		{
			*(const_cast<char *>(p)) = 0;
			break;
		}
	}
	if(pEnd != nullptr)
		*(const_cast<char *>(pEnd)) = 0;

	// drop empty and autocreated spam messages (more than 32 characters per second)
	if(Length == 0 || (pMsg->m_pMessage[0] != '/' && (g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat + Server()->TickSpeed() * ((31 + Length) / 32) > Server()->Tick())))
		return;

	if(Team)
	{
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			Team = CHAT_SPEC;
		}
		else
		{
			Team = m_pController->GetPlayerTeam(pPlayer->GetCid());
		}
	}
	else
	{
		Team = CHAT_ALL;
	}

	if(pMsg->m_pMessage[0] == '/')
	{
		if(str_startswith_nocase(pMsg->m_pMessage + 1, "w "))
		{
			char aWhisperMsg[256];
			str_copy(aWhisperMsg, pMsg->m_pMessage + 3, 256);
			Whisper(pPlayer->GetCid(), aWhisperMsg);
		}
		else if(str_startswith_nocase(pMsg->m_pMessage + 1, "whisper "))
		{
			char aWhisperMsg[256];
			str_copy(aWhisperMsg, pMsg->m_pMessage + 9, 256);
			Whisper(pPlayer->GetCid(), aWhisperMsg);
		}
		else if(str_startswith_nocase(pMsg->m_pMessage + 1, "c "))
		{
			char aWhisperMsg[256];
			str_copy(aWhisperMsg, pMsg->m_pMessage + 3, 256);
			Converse(pPlayer->GetCid(), aWhisperMsg);
		}
		else if(str_startswith_nocase(pMsg->m_pMessage + 1, "converse "))
		{
			char aWhisperMsg[256];
			str_copy(aWhisperMsg, pMsg->m_pMessage + 10, 256);
			Converse(pPlayer->GetCid(), aWhisperMsg);
		}
		/* INFECTION MODIFICATION START ***************************************/
		else if(str_comp_num(pMsg->m_pMessage + 1, "msg ", 4) == 0)
		{
			PrivateMessage(pMsg->m_pMessage + 5, ClientId, (Team != CGameContext::CHAT_ALL));
		}
		else if(str_comp_num(pMsg->m_pMessage + 1, "mute ", 5) == 0)
		{
			char aMsg[256];
			str_copy(aMsg, pMsg->m_pMessage + 6);
			MutePlayer(ClientId, aMsg);
		}
		else
		{
			if(g_Config.m_SvSpamprotection && !str_startswith(pMsg->m_pMessage + 1, "timeout ") && pPlayer->m_aLastCommands[0] && pPlayer->m_aLastCommands[0] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_aLastCommands[1] && pPlayer->m_aLastCommands[1] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_aLastCommands[2] && pPlayer->m_aLastCommands[2] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_aLastCommands[3] && pPlayer->m_aLastCommands[3] + Server()->TickSpeed() > Server()->Tick())
				return;

			int64_t Now = Server()->Tick();
			pPlayer->m_aLastCommands[pPlayer->m_LastCommandPos] = Now;
			pPlayer->m_LastCommandPos = (pPlayer->m_LastCommandPos + 1) % 4;

			switch(Server()->GetAuthedState(ClientId))
			{
			case AUTHED_ADMIN:
				Console()->SetAccessLevel(EAccessLevel::ADMIN);
				break;
			case AUTHED_MOD:
				Console()->SetAccessLevel(EAccessLevel::MOD);
				break;
			default:
				Console()->SetAccessLevel(EAccessLevel::USER);
			}

			{
				CClientChatLogger Logger(this, ClientId, log_get_scope_logger());
				CLogScope Scope(&Logger);
				Console()->ExecuteLineFlag(pMsg->m_pMessage + 1, CFGFLAG_CHAT, ClientId, (Team != CGameContext::CHAT_ALL));
			}

			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "%d used %s", ClientId, pMsg->m_pMessage);
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "chat-command", aBuf);

			Console()->SetAccessLevel(EAccessLevel::ADMIN);
		}
	}
	else
	{
		// Inverse order and add ligature for arabic
		std::string Buffer;
		Buffer.append(pMsg->m_pMessage);
		Server()->Localization()->ArabicShaping(Buffer);
		SendChat(ClientId, Team, Buffer.c_str(), ClientId);
	}
	/* INFECTION MODIFICATION END *****************************************/
}

void CGameContext::OnCallVoteNetMessage(const CNetMsg_Cl_CallVote *pMsg, int ClientId)
{
	CPlayer *pPlayer = m_apPlayers[ClientId];
	if(!pPlayer)
		return;

	if(RateLimitPlayerVote(ClientId))
		return;

	m_VoteType = VOTE_TYPE_UNKNOWN;
	char aDesc[VOTE_DESC_LENGTH] = {0};
	char aCmd[VOTE_CMD_LENGTH] = {0};
	char aReason[VOTE_REASON_LENGTH] = "No reason given";
	if(!str_utf8_check(pMsg->m_pType) || !str_utf8_check(pMsg->m_pReason) || !str_utf8_check(pMsg->m_pValue))
	{
		return;
	}
	if(pMsg->m_pReason[0])
	{
		str_copy(aReason, pMsg->m_pReason);
	}

	if(str_comp_nocase(pMsg->m_pType, "kick") == 0)
	{
		int KickId = str_toint(pMsg->m_pValue);
		if(KickId < 0 || KickId >= MAX_CLIENTS || !m_apPlayers[KickId])
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Invalid client id to kick"), nullptr);
			return;
		}
		if(KickId == ClientId)
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("You can't kick yourself"), nullptr);
			return;
		}
		if(m_apPlayers[KickId]->IsBot())
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Unable to kick a server-side bot"), nullptr);
			return;
		}
		if(Server()->GetAuthedState(KickId) != AUTHED_NO)
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("You can't kick admins"), nullptr);
			SendChatTarget_Localization(KickId, CHATCATEGORY_DEFAULT,
				_("'{str:CallerName}' called for vote to kick you"),
				"CallerName", Server()->ClientName(ClientId), nullptr);
			return;
		}

		Server()->AddAccusation(ClientId, KickId, aReason);
	}
	else
	{
		const int Authed = Server()->GetAuthedState(ClientId);
		if(pPlayer->GetTeam() == TEAM_SPECTATORS && (Authed != AUTHED_ADMIN))
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Spectators aren't allowed to start a vote."), nullptr);
			return;
		}

		if(str_comp_nocase(pMsg->m_pType, "option") == 0)
		{
			// this vote is not a kick/ban or spectate vote
			CVoteOptionServer *pOption = m_pVoteOptionFirst;
			while(pOption) // loop through all option votes to find out which vote it is
			{
				if(str_comp_nocase(pMsg->m_pValue, pOption->m_aDescription) == 0) // found out which vote it is
				{
					if(!Console()->LineIsValid(pOption->m_aCommand))
					{
						SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
							_("Invalid option"), nullptr);
						return;
					}
					OPTION_VOTE_TYPE OptionVoteType = GetOptionVoteType(pOption->m_aCommand);
					if(OptionVoteType & MAP_VOTE_BITS) // this is a map vote
					{
						if(OptionVoteType == SV_MAP || OptionVoteType == CHANGE_MAP)
						{
							// check if we are already playing on the map the user wants to vote
							char MapName[VOTE_CMD_LENGTH] = {0};
							GetMapNameFromCommand(MapName, pOption->m_aCommand);
							if(str_comp_nocase(MapName, g_Config.m_SvMap) == 0)
							{
								SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
									_("Server is already on map {str:MapName}"), "MapName", MapName, nullptr);
								return;
							}
						}

						int RoundCount = m_pController->GetRoundCount();
						if(m_pController->IsRoundEndTime())
							RoundCount++;
						if(g_Config.m_InfMinRoundsForMapVote > RoundCount && Server()->GetActivePlayerCount() > 1)
						{
							SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
								_("Each map must be played at least {int:Rounds} rounds before calling a map vote"),
								"Rounds", &g_Config.m_InfMinRoundsForMapVote, nullptr);
							return;
						}
					}
					if((OptionVoteType == PLAY_MORE_VOTE_TYPE) || (OptionVoteType == QUEUED_VOTE))
					{
						// copy information to start a vote
						SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
							_("'{str:CallerName}' called vote to change server option '{str:VoteName}' ({str:Reason})"),
							"CallerName", Server()->ClientName(ClientId), "VoteName", &pOption->m_aDescription,
							"Reason", &aReason, nullptr);
						str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);
						str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
						break;
					}
					if(g_Config.m_InfMinPlayerNumberForMapVote <= 1 || OptionVoteType == OTHER_OPTION_VOTE_TYPE)
					{
						// (this is not a map vote) or ("InfMinPlayerNumberForMapVote <= 1" and we keep default behaviour)
						if(!m_pController->CanVote() && (Authed != AUTHED_ADMIN))
						{
							SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
								_("Votes are only allowed when the round start."), nullptr);
							return;
						}

						// copy information to start a vote
						SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
							_("'{str:CallerName}' called vote to change server option '{str:VoteName}' ({str:Reason})"),
							"CallerName", Server()->ClientName(ClientId), "VoteName", &pOption->m_aDescription,
							"Reason", &aReason, nullptr);
						str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);
						str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
						break;
					}

					if(OptionVoteType & MAP_VOTE_BITS)
					{
						// this vote is a map vote
						Server()->AddMapVote(ClientId, pOption->m_aCommand, aReason, pOption->m_aDescription);
						return;
					}

					break;
				}

				pOption = pOption->m_pNext;
			}

			if(!pOption)
			{
				if(Authed != AUTHED_ADMIN) // allow admins to call any vote they want
				{
					SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
						_("'{str:VoteName}' isn't an option on this server"), "VoteName", pMsg->m_pValue, nullptr);
					return;
				}
				else
				{
					SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
						_("'{str:CallerName}' called vote to change server option '{str:VoteName}'"),
						"CallerName", Server()->ClientName(ClientId), "VoteName", pMsg->m_pValue, nullptr);
					str_format(aDesc, sizeof(aDesc), "%s", pMsg->m_pValue);
					str_format(aCmd, sizeof(aCmd), "%s", pMsg->m_pValue);
				}
			}

			m_VoteType = VOTE_TYPE_OPTION;
		}
		else if(str_comp_nocase(pMsg->m_pType, "spectate") == 0)
		{
			if(!g_Config.m_SvVoteSpectate)
			{
				SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
					_("Server does not allow voting to move players to spectators"), nullptr);
				return;
			}

			int SpectateId = str_toint(pMsg->m_pValue);
			if(SpectateId < 0 || SpectateId >= MAX_CLIENTS || !m_apPlayers[SpectateId] || m_apPlayers[SpectateId]->GetTeam() == TEAM_SPECTATORS)
			{
				SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
					_("Invalid client id to move"), nullptr);
				return;
			}
			if(SpectateId == ClientId)
			{
				SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
					_("You can't move yourself"), nullptr);
				return;
			}
			if(m_apPlayers[SpectateId]->IsBot())
			{
				SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
					_("Unable to move a server-side bot to spectators"), nullptr);
				return;
			}
			if(!Server()->ReverseTranslate(SpectateId, ClientId))
			{
				return;
			}

			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
				_("'{str:CallerName}' called for vote to move '{str:SpectateName}' to spectators ({str:Reason})"),
				"CallerName", Server()->ClientName(ClientId), "SpectateName", Server()->ClientName(SpectateId),
				"Reason", &aReason, nullptr);
			str_format(aDesc, sizeof(aDesc), "move '%s' to spectators", Server()->ClientName(SpectateId));
			str_format(aCmd, sizeof(aCmd), "set_team %d -1 %d", SpectateId, g_Config.m_SvVoteSpectateRejoindelay);
			m_VoteType = VOTE_TYPE_SPECTATE;
		}

		// Start a vote
		if(aCmd[0])
		{
			CallVote(ClientId, aDesc, aCmd, aReason);
		}
	}
}

void CGameContext::OnVoteNetMessage(const CNetMsg_Cl_Vote *pMsg, int ClientId)
{
	if(!pMsg->m_Vote)
		return;

	if(m_VoteLanguageTick[ClientId] > 0)
	{
		if(pMsg->m_Vote)
		{
			if(pMsg->m_Vote > 0)
			{
				SetClientLanguage(ClientId, m_VoteLanguage[ClientId]);
			}

			m_VoteLanguageTick[ClientId] = 0;

			CNetMsg_Sv_VoteSet Msg;
			Msg.m_Timeout = 0;
			Msg.m_pDescription = "";
			Msg.m_pReason = "";
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
		}
		return;
	}

	CPlayer *pPlayer = m_apPlayers[ClientId];

	if(!m_VoteCloseTime || pPlayer->m_Vote)
	{
		m_pController->OnPlayerVoteCommand(ClientId, pMsg->m_Vote);
	}

	int64_t Now = Server()->Tick();

	pPlayer->m_LastVoteTry = Now;
	pPlayer->m_Vote = pMsg->m_Vote;
	pPlayer->m_VotePos = ++m_VotePos;
	m_VoteUpdate = true;

	CNetMsg_Sv_YourVote Msg = {pMsg->m_Vote};
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CGameContext::OnSetTeamNetMessage(const CNetMsg_Cl_SetTeam *pMsg, int ClientId)
{
	if(m_World.m_Paused)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientId];

	if(pPlayer->GetTeam() == pMsg->m_Team || (g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * 3 > Server()->Tick()))
		return;

	if(pPlayer->m_TeamChangeTick > Server()->Tick())
	{
		pPlayer->m_LastSetTeam = Server()->Tick();
		int TimeLeft = (pPlayer->m_TeamChangeTick - Server()->Tick()) / Server()->TickSpeed();
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Time to wait before changing team: %02d:%02d", TimeLeft / 60, TimeLeft % 60);
		SendBroadcast(ClientId, aBuf, EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE);
		return;
	}

	/* INFECTION MODIFICATION START ***************************************/
	if(pMsg->m_Team == TEAM_SPECTATORS)
	{
		if(!m_pController->CanJoinTeam(TEAM_SPECTATORS, ClientId))
		{
			SendBroadcast_Localization(ClientId, EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE,
				_("You can't join the spectators right now"), nullptr);
			return;
		}
	}
	else
	{
		const bool AccountsAreMandatory = str_comp(Config()->m_SvAccounts, "mandatory") == 0;
		if(AccountsAreMandatory)
		{
			if(!Server()->IsClientLogged(ClientId))
			{
				const char *pText = _("You have to log in to join the game");
				SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
					pText, nullptr);
				SendBroadcast_Localization(ClientId,
					EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE,
					pText, nullptr);

				return;
			}
		}
	}

	/* INFECTION MODIFICATION END *****************************************/

	m_pController->OnTeamChangeRequested(ClientId, pMsg->m_Team);
}

void CGameContext::OnIsDDNetLegacyNetMessage(const CNetMsg_Cl_IsDDNetLegacy *pMsg, int ClientId, CUnpacker *pUnpacker)
{
	IServer::CClientInfo Info;
	if(Server()->GetClientInfo(ClientId, &Info) && Info.m_GotDDNetVersion)
	{
		return;
	}
	int DDNetVersion = pUnpacker->GetInt();
	if(pUnpacker->Error() || DDNetVersion < 0)
	{
		DDNetVersion = VERSION_DDRACE;
	}
	Server()->SetClientDDNetVersion(ClientId, DDNetVersion);
	OnClientDDNetVersionKnown(ClientId);
}

void CGameContext::OnSetSpectatorModeNetMessage(const CNetMsg_Cl_SetSpectatorMode *pMsg, int ClientId)
{
	if(m_World.m_Paused)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientId];
	if(pPlayer->GetTeam() != TEAM_SPECTATORS || pPlayer->m_SpectatorId == pMsg->m_SpectatorId || ClientId == pMsg->m_SpectatorId ||
		(g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode + Server()->TickSpeed() * 3 > Server()->Tick()))
		return;

	pPlayer->m_LastSetSpectatorMode = Server()->Tick();
	if(pMsg->m_SpectatorId != SPEC_FREEVIEW && (!m_apPlayers[pMsg->m_SpectatorId] || m_apPlayers[pMsg->m_SpectatorId]->GetTeam() == TEAM_SPECTATORS))
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Invalid spectator id used"), nullptr);
	else
		pPlayer->m_SpectatorId = pMsg->m_SpectatorId;
}

void CGameContext::OnChangeInfoNetMessage(const CNetMsg_Cl_ChangeInfo *pMsg, int ClientId)
{
	CPlayer *pPlayer = m_apPlayers[ClientId];
	if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo + Server()->TickSpeed() * g_Config.m_SvInfoChangeDelay > Server()->Tick())
		return;

	if(!str_utf8_check(pMsg->m_pName) || !str_utf8_check(pMsg->m_pClan) || !str_utf8_check(pMsg->m_pSkin))
	{
		return;
	}
	pPlayer->m_LastChangeInfo = Server()->Tick();

	// set infos
	if(!pPlayer->m_ClientNameLocked && Server()->WouldClientNameChange(ClientId, pMsg->m_pName))
	{
		char aOldName[MAX_NAME_LENGTH];
		str_copy(aOldName, Server()->ClientName(ClientId));

		Server()->SetClientName(ClientId, pMsg->m_pName);

		SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER, _("{str:PlayerName} changed their name to {str:NewName}"), "PlayerName", aOldName, "NewName", Server()->ClientName(ClientId), nullptr);
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "change_name previous='%s' now='%s'", aOldName, Server()->ClientName(ClientId));
		Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}
	Server()->SetClientClan(ClientId, pMsg->m_pClan);
#ifndef CONF_FORCE_COUNTRY_BY_IP
	Server()->SetClientCountry(ClientId, pMsg->m_Country);
#endif
	Server()->ExpireServerInfo();
}

void CGameContext::OnEmoticonNetMessage(const CNetMsg_Cl_Emoticon *pMsg, int ClientId)
{
	if(m_World.m_Paused)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientId];

	// Still apply a reasonable limit: 1-2 emotes per tick
	if(g_Config.m_SvSpamprotection && pPlayer->m_LastEmote &&
		pPlayer->m_LastEmote + maximum(Server()->TickSpeed() * g_Config.m_SvEmoticonDelay,
								   Server()->TickSpeed() / g_Config.m_SvSnapsPerSecond) >
			Server()->Tick())
		return;

	CCharacter *pChr = pPlayer->GetCharacter();

	// player needs a character to send emotes
	if(!pChr)
		return;

	pPlayer->m_LastEmote = Server()->Tick();

	SendEmoticon(ClientId, pMsg->m_Emoticon);
}

void CGameContext::OnKillNetMessage(const CNetMsg_Cl_Kill *pMsg, int ClientId)
{
	if(m_World.m_Paused)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientId];
	if(pPlayer->m_LastKill && pPlayer->m_LastKill + Server()->TickSpeed() * 3 > Server()->Tick())
		return;

	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr)
		return;

	pPlayer->m_LastKill = Server()->Tick();
	pPlayer->KillCharacter(WEAPON_SELF);
}

void CGameContext::OnStartInfoNetMessage(const CNetMsg_Cl_StartInfo *pMsg, int ClientId)
{
	CPlayer *pPlayer = m_apPlayers[ClientId];

	if(pPlayer->m_IsReady)
		return;

	if(!str_utf8_check(pMsg->m_pName))
	{
		Server()->Kick(ClientId, "name is not valid utf8");
		return;
	}
	if(!str_utf8_check(pMsg->m_pClan))
	{
		Server()->Kick(ClientId, "clan is not valid utf8");
		return;
	}
	if(!str_utf8_check(pMsg->m_pSkin))
	{
		Server()->Kick(ClientId, "skin is not valid utf8");
		return;
	}

	pPlayer->m_LastChangeInfo = Server()->Tick();

	// set start infos
	Server()->SetClientName(ClientId, pMsg->m_pName);
	// trying to set client name can delete the player object, check if it still exists
	if(!m_apPlayers[ClientId])
	{
		return;
	}
	Server()->SetClientClan(ClientId, pMsg->m_pClan);
	Server()->SetClientCountry(ClientId, pMsg->m_Country);

	/* INFECTION MODIFICATION START ***************************************/
	if(!Server()->GetClientMemory(ClientId, CLIENTMEMORY_LANGUAGESELECTION))
	{
#ifdef CONF_GEOLOCATION
		char aAddrStr[NETADDR_MAXSTRSIZE]{};
		Server()->GetClientAddr(ClientId, aAddrStr, sizeof(aAddrStr));
		std::string ip(aAddrStr);

		int LocatedCountry = Geolocation::get_country_iso_numeric_code(ip);
#ifdef CONF_FORCE_COUNTRY_BY_IP
		Server()->SetClientCountry(ClientId, LocatedCountry);
#endif // CONF_FORCE_COUNTRY_BY_IP
#else
		int LocatedCountry = -1;
#endif // CONF_GEOLOCATION

		const auto LangFromClient = CLocalization::LanguageCodeByCountryCode(pMsg->m_Country);
		const auto LangForIp = CLocalization::LanguageCodeByCountryCode(LocatedCountry);

		const auto pDefaultLang = Config()->m_InfDefaultLanguageCode;
		std::string LangForVote;

		if(!LangFromClient.empty() && LangFromClient != pDefaultLang)
			LangForVote = LangFromClient;
		else if(!LangForIp.empty() && LangForIp != pDefaultLang)
			LangForVote = LangForIp;

		dbg_msg("lang", "init_language ClientId=%d, lang from flag: \"%s\", lang for IP: \"%s\"", ClientId, LangFromClient.data(), LangForIp.data());

		SetClientLanguage(ClientId, pDefaultLang);

		if(!LangForVote.empty())
		{
			CNetMsg_Sv_VoteSet Msg;
			Msg.m_Timeout = 10;
			Msg.m_pReason = "";
			str_copy(m_VoteLanguage[ClientId], LangForVote.c_str());
			const auto LangName = Server()->Localization()->GetLangaugeNameByCode(LangForVote);
			const auto Buffer = Server()->Localization()->Format_L(m_VoteLanguage[ClientId], "Switch language to {str:LangName}?", _("LangName"), LangName.c_str());
			Msg.m_pDescription = Buffer.c_str();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
			m_VoteLanguageTick[ClientId] = 10 * Server()->TickSpeed();
		}
		else
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("You can change the language of this mod using the command /language."), nullptr);
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("If your language is not available, you can help with translation (/help translate)."), nullptr);
		}

		Server()->SetClientMemory(ClientId, CLIENTMEMORY_LANGUAGESELECTION, true);
	}
	/* INFECTION MODIFICATION END *****************************************/

	// send clear vote options
	CNetMsg_Sv_VoteClearOptions ClearMsg;
	Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientId);

	// begin sending vote options
	pPlayer->m_SendVoteIndex = 0;

	// send tuning parameters to client
	SendTuningParams(ClientId);

	// client is ready to enter
	pPlayer->m_IsReady = true;
	CNetMsg_Sv_ReadyToEnter m;
	Server()->SendPackMsg(&m, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);

	Server()->ExpireServerInfo();
}

void CGameContext::ConConverse(IConsole::IResult *pResult, void *pUserData)
{
	// This will never be called
}

void CGameContext::ConMute(IConsole::IResult *pResult, void *pUserData)
{
	// This will never be called
}

void CGameContext::ConShowOthers(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	if(pSelf->Server()->GetAuthedState(pResult->m_ClientId))
	{
		if(pResult->NumArguments())
			pPlayer->m_ShowOthers = pResult->GetInteger(0);
		else
			pPlayer->m_ShowOthers = !pPlayer->m_ShowOthers;
	}
	else
		pSelf->Console()->Print(
			IConsole::OUTPUT_LEVEL_STANDARD,
			"chatresp",
			"Custom 'show others' is disabled");
}

void CGameContext::ConShowAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	if(pSelf->Server()->GetAuthedState(pResult->m_ClientId) == 0)
	{
		pSelf->Console()->Print(
			IConsole::OUTPUT_LEVEL_STANDARD,
			"chatresp",
			"Custom 'show all' is disabled");
	}

	if(pResult->NumArguments())
	{
		if(pPlayer->m_ShowAll == static_cast<bool>(pResult->GetInteger(0)))
			return;

		pPlayer->m_ShowAll = pResult->GetInteger(0);
	}
	else
	{
		pPlayer->m_ShowAll = !pPlayer->m_ShowAll;
	}

	if(pPlayer->m_ShowAll)
		pSelf->SendChatTarget_Localization(pResult->m_ClientId, CHATCATEGORY_DEFAULT,
			_("You will now see all tees on this server, no matter the distance"), nullptr);
	else
		pSelf->SendChatTarget_Localization(pResult->m_ClientId, CHATCATEGORY_DEFAULT,
			_("You will no longer see all tees on this server"), nullptr);
}

void CGameContext::ConTimeout(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(!CheckClientId(pResult->m_ClientId))
		return;

	int ClientId = pResult->m_ClientId;
	CPlayer *pPlayer = pSelf->m_apPlayers[ClientId];
	if(!pPlayer)
		return;

	const char *pTimeout = pResult->NumArguments() > 0 ? pResult->GetString(0) : pPlayer->m_aTimeoutCode;
	const char *pClientName = pSelf->Server()->ClientName(ClientId);
	char aAddress[NETADDR_MAXSTRSIZE];
	pSelf->Server()->GetClientAddr(ClientId, &aAddress[0], sizeof(aAddress));

	dbg_msg("timeout", "Used with code %s by (#%02i) '%s' (id=%s)", pTimeout, ClientId, pClientName, aAddress);

	if(!pSelf->Server()->IsSixup(ClientId))
	{
		for(int i = 0; i < pSelf->Server()->MaxClients(); i++)
		{
			if(i == pResult->m_ClientId)
				continue;
			if(!pSelf->m_apPlayers[i])
				continue;
			if(str_comp(pSelf->m_apPlayers[i]->m_aTimeoutCode, pTimeout))
				continue;
			if(pSelf->Server()->SetTimedOut(i, pResult->m_ClientId))
			{
				return;
			}
		}
	}
	else
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp",
			"Your timeout code has been set. 0.7 clients can not reclaim their tees on timeout; however, a 0.6 client can claim your tee ");
	}

	pSelf->Server()->SetTimeoutProtected(pResult->m_ClientId);
	str_copy(pPlayer->m_aTimeoutCode, pResult->GetString(0));
}

void CGameContext::ConMe(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(!CheckClientId(pResult->m_ClientId))
		return;

	char aBuf[256 + 24];

	str_format(aBuf, 256 + 24, "'%s' %s",
		pSelf->Server()->ClientName(pResult->m_ClientId),
		pResult->GetString(0));
	if(g_Config.m_SvSlashMe)
		pSelf->SendChat(-2, CGameContext::CHAT_ALL, aBuf, pResult->m_ClientId);
	else
		pSelf->Console()->Print(
			IConsole::OUTPUT_LEVEL_STANDARD,
			"chatresp",
			"/me is disabled on this server");
}

void CGameContext::ConWhisper(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pThis = static_cast<CGameContext *>(pUserData);

	const char *pStrClientId = pResult->GetString(0);
	const char *pText = pResult->GetString(1);

	if(!str_isallnum(pStrClientId))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", "Invalid client id");
		return;
	}

	int ToClientId = str_toint(pStrClientId);
	const CPlayer *pPlayer = pThis->GetPlayer(ToClientId);
	if(!pPlayer)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", "Invalid client id");
		return;
	}

	pThis->SendChatTarget(ToClientId, pText);

	// Confirm message sent
	char aBuf[1024];
	str_format(aBuf, sizeof(aBuf), "Whisper '%s' sent to %s",
		pText,
		pThis->Server()->ClientName(ToClientId));
	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", aBuf);
}

void CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);

	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		//~ pSelf->SendTuningParams(-1);
	}
	else
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CGameContext::ConToggleTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pParamName = pResult->GetString(0);
	float OldValue;

	char aBuf[256];
	if(!pSelf->Tuning()->Get(pParamName, &OldValue))
	{
		str_format(aBuf, sizeof(aBuf), "No such tuning parameter: %s", pParamName);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		return;
	}

	float NewValue = fabs(OldValue - pResult->GetFloat(1)) < 0.0001f ? pResult->GetFloat(2) : pResult->GetFloat(1);

	pSelf->Tuning()->Set(pParamName, NewValue);
	pSelf->Tuning()->Get(pParamName, &NewValue);

	str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	pSelf->SendTuningParams(-1);
}

void CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	CTuningParams TuningParams;
	*pSelf->Tuning() = TuningParams;
	//~ pSelf->SendTuningParams(-1);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
}

void CGameContext::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	char aBuf[256];
	for(int i = 0; i < CTuningParams::Num(); i++)
	{
		float Value;
		pSelf->Tuning()->Get(i, &Value);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", CTuningParams::Name(i), Value);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

void CGameContext::ConPause(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->SetPaused(!pSelf->IsPaused());
}

void CGameContext::ConChangeMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->m_pController->ChangeMap(pResult->NumArguments() ? pResult->GetString(0) : "");
}

void CGameContext::ConSkipMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->m_pController->SkipMap();
}

void CGameContext::ConQueueMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	const char *pMapName = pResult->GetString(0);

	char aBuf[256];
	if(pSelf->MapExists(pMapName))
	{
		str_format(aBuf, sizeof(aBuf), "Map '%s' will be the next map", pMapName);
		pSelf->m_pController->QueueMap(pResult->GetString(0));
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "Unable to find map '%s'", pMapName);
	}

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CGameContext::ConAddMap(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CGameContext *>(pUserData);

	if(pResult->NumArguments() != 1)
		return;

	const char *pMapName = pResult->GetString(0);
	pSelf->AddMap(pMapName);
}

void CGameContext::ConRemoveMap(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CGameContext *>(pUserData);

	if(pResult->NumArguments() != 1)
		return;

	const char *pMapName = pResult->GetString(0);
	pSelf->RemoveMap(pMapName);
}

void CGameContext::ConClearMaps(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CGameContext *>(pUserData);

	if(pSelf->m_pController)
	{
		for(auto &MapName : pSelf->m_MapRotationList)
			pSelf->m_pController->OnMapRemoved(MapName.c_str());
	}

	pSelf->m_MapRotationList.clear();

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "All maps in the rotation list have been removed");
}

void CGameContext::AddMap(const std::string_view MapName)
{
	constexpr size_t MaxNameLength = 127;
	std::string StringMapName(MapName);

	if(!str_utf8_check(StringMapName.c_str()))
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid (non UTF-8) filename");
		return;
	}

	string_strip(StringMapName);

	if(StringMapName.empty())
	{
		const auto Msg = std::format("Invalid (empty) filename: {}", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
		return;
	}

	if(StringMapName.size() > MaxNameLength)
	{
		const auto Msg = std::format("The map name {} is too long (max 127 bytes)", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
		return;
	}

	if(std::ranges::find(m_MapRotationList, StringMapName) != m_MapRotationList.end())
	{
		const auto Msg = std::format("The map {} is already in the rotation list", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
		return;
	}

	if(!MapExists(StringMapName.data()))
	{
		const auto Msg = std::format("Unable to find map {}", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
		return;
	}

	m_MapRotationList.push_back(StringMapName);

	{
		const auto Msg = std::format("Map {} added to the rotation list", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
	}

	if(m_pController)
		m_pController->OnMapAdded(StringMapName.data());
}

void CGameContext::RemoveMap(const std::string_view MapName)
{
	std::string StringMapName(MapName);

	if(!str_utf8_check(StringMapName.c_str()))
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid (non UTF-8) filename");
		return;
	}

	string_strip(StringMapName);

	if(StringMapName.empty())
	{
		const auto Msg = std::format("Invalid (empty) filename: {}", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
		return;
	}

	if(std::ranges::find(m_MapRotationList, StringMapName) == m_MapRotationList.end())
	{
		const auto Msg = std::format("The map {} is not in the rotation list", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
		return;
	}

	std::erase(m_MapRotationList, StringMapName);

	{
		const auto Msg = std::format("Map {} has been removed from the rotation list", StringMapName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", Msg.c_str());
	}

	if(m_pController)
		m_pController->OnMapRemoved(StringMapName.data());
}

const std::string *CGameContext::GetRandomMap() const
{
	if(m_MapRotationList.size() == 0)
		return nullptr;
	return &m_MapRotationList[random_int(0, m_MapRotationList.size() - 1)];
}

void CGameContext::ConRestart(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(pResult->NumArguments())
		pSelf->m_pController->DoWarmup(pResult->GetInteger(0));
	else
		pSelf->m_pController->StartRound();
}

void CGameContext::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	char aBuf[1024];
	str_copy(aBuf, pResult->GetString(0));

	int i, j;
	for(i = 0, j = 0; aBuf[i]; i++, j++)
	{
		if(aBuf[i] == '\\' && aBuf[i + 1] == 'n')
		{
			aBuf[j] = '\n';
			i++;
		}
		else if(i != j)
		{
			aBuf[j] = aBuf[i];
		}
	}
	aBuf[j] = '\0';

	pSelf->SendBroadcast(-1, aBuf, EBroadcastPriority::SERVERANNOUNCE, pSelf->Server()->TickSpeed() * 3);
}

void CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->SendChat(-1, CGameContext::CHAT_ALL, pResult->GetString(0));
}

void CGameContext::ConSetTeam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int ClientId = clamp(pResult->GetInteger(0), 0, static_cast<int>(MAX_CLIENTS) - 1);
	int Team = clamp(pResult->GetInteger(1), -1, 1);
	int Delay = pResult->NumArguments() > 2 ? pResult->GetInteger(2) : 0;
	if(!pSelf->m_apPlayers[ClientId])
		return;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "moved client %d to team %d", ClientId, Team);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	pSelf->m_apPlayers[ClientId]->m_TeamChangeTick = pSelf->Server()->Tick() + pSelf->Server()->TickSpeed() * Delay * 60;
	pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[ClientId], Team);
}

void CGameContext::ConSetTeamAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int Team = clamp(pResult->GetInteger(0), -1, 1);

	pSelf->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
		_("All players were moved to the {str:TeamName}"),
		"TeamName", pSelf->m_pController->GetTeamName(Team), nullptr);

	for(auto &pPlayer : pSelf->m_apPlayers)
		if(pPlayer)
			pSelf->m_pController->DoTeamChange(pPlayer, Team, false);
}

void CGameContext::ConInsertVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int Index = pResult->GetInteger(0);
	const char *pDescription = pResult->GetString(1);
	const char *pCommand = pResult->GetString(2);

	pSelf->InsertVote(Index, pDescription, pCommand);
}

void CGameContext::ConAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pDescription = pResult->GetString(0);
	const char *pCommand = pResult->GetString(1);

	pSelf->AddVote(pDescription, pCommand);
}

bool CGameContext::InsertVote(int Position, const char *pDescription, const char *pCommand)
{
	if((Position < 0) || (Position > m_NumVoteOptions))
		Position = m_NumVoteOptions;

	if(m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return false;
	}

	// check for valid option
	if(!Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return false;
	}
	while(*pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return false;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return false;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	int Len = str_length(pCommand);

	pOption = static_cast<CVoteOptionServer *>(m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len, alignof(CVoteOptionServer)));
	if(Position == m_NumVoteOptions)
	{
		// Append
		pOption->m_pNext = nullptr;
		pOption->m_pPrev = m_pVoteOptionLast;
		if(pOption->m_pPrev)
			pOption->m_pPrev->m_pNext = pOption;
		m_pVoteOptionLast = pOption;
	}
	else
	{
		// Insert
		pOption->m_pPrev = nullptr;
		pOption->m_pNext = m_pVoteOptionFirst;
		if(Position == 0)
		{
			m_pVoteOptionFirst = pOption;
		}
		else
		{
			int CurrentPos = 1;
			CVoteOptionServer *pPrevOption = m_pVoteOptionFirst;
			while(CurrentPos < Position)
			{
				pPrevOption = pPrevOption->m_pNext;
				++CurrentPos;
			}

			pOption->m_pPrev = pPrevOption;
			pOption->m_pNext = pPrevOption->m_pNext;

			pOption->m_pPrev->m_pNext = pOption;
			pOption->m_pNext->m_pPrev = pOption;
		}
	}

	if(!m_pVoteOptionFirst)
		m_pVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription);
	mem_copy(pOption->m_aCommand, pCommand, Len + 1);
	++m_NumVoteOptions;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "added option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	if(pOption->m_pNext)
	{
		// Inserted

		CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
		Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);

		// reset sending of vote options
		for(auto &pPlayer : m_apPlayers)
		{
			if(pPlayer)
				pPlayer->m_SendVoteIndex = 0;
		}
	}

	return true;
}

void CGameContext::AddVote(const char *pDescription, const char *pCommand)
{
	InsertVote(m_NumVoteOptions, pDescription, pCommand);
}

void CGameContext::ConRemoveVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pDescription = pResult->GetString(0);
	pSelf->RemoveVote(pDescription);
}

void CGameContext::RemoveVote(const char *pVoteOption)
{
	// check for valid option
	CVoteOptionServer *pOption = m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pVoteOption, pOption->m_aDescription) == 0)
			break;
		if(str_comp_nocase(pVoteOption, pOption->m_aCommand) == 0)
			break;
		pOption = pOption->m_pNext;
	}
	if(!pOption)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "option '%s' does not exist", pVoteOption);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// start reloading vote option list
	// clear vote options
	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);

	// reset sending of vote options
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
			pPlayer->m_SendVoteIndex = 0;
	}

	// TODO: improve this
	// remove the option
	--m_NumVoteOptions;

	CHeap *pVoteOptionHeap = new CHeap();
	CVoteOptionServer *pVoteOptionFirst = nullptr;
	CVoteOptionServer *pVoteOptionLast = nullptr;
	int NumVoteOptions = m_NumVoteOptions;
	for(CVoteOptionServer *pSrc = m_pVoteOptionFirst; pSrc; pSrc = pSrc->m_pNext)
	{
		if(pSrc == pOption)
			continue;

		// copy option
		int Len = str_length(pSrc->m_aCommand);
		CVoteOptionServer *pDst = static_cast<CVoteOptionServer *>(pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len));
		pDst->m_pNext = nullptr;
		pDst->m_pPrev = pVoteOptionLast;
		if(pDst->m_pPrev)
			pDst->m_pPrev->m_pNext = pDst;
		pVoteOptionLast = pDst;
		if(!pVoteOptionFirst)
			pVoteOptionFirst = pDst;

		str_copy(pDst->m_aDescription, pSrc->m_aDescription);
		mem_copy(pDst->m_aCommand, pSrc->m_aCommand, Len + 1);
	}

	// clean up
	delete m_pVoteOptionHeap;
	m_pVoteOptionHeap = pVoteOptionHeap;
	m_pVoteOptionFirst = pVoteOptionFirst;
	m_pVoteOptionLast = pVoteOptionLast;
	m_NumVoteOptions = NumVoteOptions;
}

void CGameContext::ClearVotes()
{
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "cleared votes");
	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);
	m_pVoteOptionHeap->Reset();
	m_pVoteOptionFirst = nullptr;
	m_pVoteOptionLast = nullptr;
	m_NumVoteOptions = 0;

	// reset sending of vote options
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
			pPlayer->m_SendVoteIndex = 0;
	}
}

void CGameContext::ConForceVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pType = pResult->GetString(0);
	const char *pValue = pResult->GetString(1);
	const char *pReason = pResult->NumArguments() > 2 && pResult->GetString(2)[0] ? pResult->GetString(2) : "No reason given";
	char aBuf[128] = {0};

	if(str_comp_nocase(pType, "option") == 0)
	{
		CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
		while(pOption)
		{
			if(str_comp_nocase(pValue, pOption->m_aDescription) == 0)
			{
				pSelf->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
					_("authorized player forced server option '{str:VoteName}' ({str:Reason})"),
					"VoteName", pValue, "Reason", pReason, nullptr);
				pSelf->Console()->ExecuteLine(pOption->m_aCommand);
				break;
			}

			pOption = pOption->m_pNext;
		}

		if(!pOption)
		{
			str_format(aBuf, sizeof(aBuf), "'%s' isn't an option on this server", pValue);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
	}
	else if(str_comp_nocase(pType, "kick") == 0)
	{
		int KickId = str_toint(pValue);
		if(KickId < 0 || KickId >= MAX_CLIENTS || !pSelf->m_apPlayers[KickId])
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid client id to kick");
			return;
		}

		if(!g_Config.m_SvVoteKickBantime)
		{
			str_format(aBuf, sizeof(aBuf), "kick %d %s", KickId, pReason);
			pSelf->Console()->ExecuteLine(aBuf);
		}
		else
		{
			char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
			pSelf->Server()->GetClientAddr(KickId, aAddrStr, sizeof(aAddrStr));
			str_format(aBuf, sizeof(aBuf), "ban %s %d %s", aAddrStr, g_Config.m_SvVoteKickBantime, pReason);
			pSelf->Console()->ExecuteLine(aBuf);
		}
	}
	else if(str_comp_nocase(pType, "spectate") == 0)
	{
		int SpectateId = str_toint(pValue);
		if(SpectateId < 0 || SpectateId >= MAX_CLIENTS || !pSelf->m_apPlayers[SpectateId] || pSelf->m_apPlayers[SpectateId]->GetTeam() == TEAM_SPECTATORS)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid client id to move");
			return;
		}
		pSelf->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
			_("'{str:PlayerName}' was moved to spectator ({str:Reason})"),
			"PlayerName", pSelf->Server()->ClientName(SpectateId),
			"Reason", pReason, nullptr);
		str_format(aBuf, sizeof(aBuf), "set_team %d -1 %d", SpectateId, g_Config.m_SvVoteSpectateRejoindelay);
		pSelf->Console()->ExecuteLine(aBuf);
	}
}

void CGameContext::ConClearVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->ClearVotes();
}

struct CMapNameItem
{
	char m_aName[IO_MAX_PATH_LENGTH - 4];

	bool operator<(const CMapNameItem &Other) const { return str_comp_nocase(m_aName, Other.m_aName) < 0; }
};

void CGameContext::ConAddMapVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	std::vector<CMapNameItem> vMapList;
	pSelf->Storage()->ListDirectory(IStorage::TYPE_ALL, "maps", MapScan, &vMapList);
	std::sort(vMapList.begin(), vMapList.end());

	for(auto &Item : vMapList)
	{
		char aDescription[64];
		str_format(aDescription, sizeof(aDescription), "Map: %s", Item.m_aName);

		char aCommand[IO_MAX_PATH_LENGTH * 2 + 10];
		char aMapEscaped[IO_MAX_PATH_LENGTH * 2];
		char *pDst = aMapEscaped;
		str_escape(&pDst, Item.m_aName, aMapEscaped + sizeof(aMapEscaped));
		str_format(aCommand, sizeof(aCommand), "change_map \"%s\"", aMapEscaped);

		pSelf->AddVote(aDescription, aCommand);
	}

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "added maps to votes");
}

int CGameContext::MapScan(const char *pName, int IsDir, int DirType, void *pUserData)
{
	if(IsDir || !str_endswith(pName, ".map"))
		return 0;

	CMapNameItem Item;
	str_truncate(Item.m_aName, sizeof(Item.m_aName), pName, str_length(pName) - str_length(".map"));
	static_cast<std::vector<CMapNameItem> *>(pUserData)->push_back(Item);

	return 0;
}

void CGameContext::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	// check if there is a vote running
	if(!pSelf->m_VoteCloseTime)
		return;

	if(str_comp_nocase(pResult->GetString(0), "yes") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_YES;
	else if(str_comp_nocase(pResult->GetString(0), "no") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_NO;
	char aBuf[256];
	pSelf->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
		_("admin forced vote {str:VoteName}"),
		"VoteName", pResult->GetString(0), nullptr);
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pResult->GetString(0));
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CNetMsg_Sv_Motd Msg;
		Msg.m_pMessage = g_Config.m_SvMotd;
		CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(pSelf->m_apPlayers[i])
				pSelf->Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
	}
}

void CGameContext::ConchainSyncMapRotation(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);

	if(pResult->NumArguments())
	{
		CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
		if(pSelf->m_pController)
		{
			pSelf->m_pController->SyncSmartMapRotationData();
		}
	}
}

/* INFECTION MODIFICATION START ***************************************/

void CGameContext::ConVersion(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp",
		"InfectionClass Mod. Version: " GAME_VERSION);

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp",
		"Compiled: " LAST_COMPILE_DATE);

	if(GIT_SHORTREV_HASH)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Git revision hash: %s", GIT_SHORTREV_HASH);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", aBuf);
	}
}

void CGameContext::ConCredits(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	int ClientId = pResult->GetClientId();
	const char *pLanguage = pSelf->m_apPlayers[ClientId]->GetLanguage();

	std::string Buffer;

	const char aThanks[] = "guenstig werben, Defeater, Orangus, BlinderHeld, Warpaint, Serena, FakeDeath, tee_to_F_U_UP!, Denis, NanoSlime_, tria, pinkieval…";
	const char aContributors[] = "necropotame, Stitch626, yavl, Socialdarwinist"
								 ", bretonium, duralakun, FluffyTee, ResamVi"
								 ", Kaffeine";

	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "InfectionClass, by necropotame (version {str:VersionCode})", _("VersionCode"), "InfectionDust", nullptr));
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "Based on the concept of Infection mod by Gravity", _(nullptr)));
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "Main contributors: {str:ListOfContributors}", _("ListOfContributors"), aContributors, nullptr));
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "Thanks to {str:ListOfContributors}", _("ListOfContributors"), aThanks, nullptr));
	Buffer.append("\n\n");
	pSelf->SendMOTD(ClientId, Buffer.c_str());
}

void CGameContext::ConInfo(IConsole::IResult *pResult, void *pUserData)
{
	ConAbout(pResult, pUserData);
}

void CGameContext::ConAbout(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->ConAbout(pResult);
}

void CGameContext::ConAbout(IConsole::IResult *pResult)
{
	int ClientId = pResult->GetClientId();
	const char *pLanguage = m_apPlayers[ClientId]->GetLanguage();

	std::string Buffer;
	Buffer = Server()->Localization()->Format_L(pLanguage, "InfectionClass, by necropotame (version {str:VersionCode})", _("VersionCode"), GAME_VERSION, nullptr);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());

	Buffer = Server()->Localization()->Format_L(pLanguage, "Server version from {str:ServerCompileDate} ", _("ServerCompileDate"), LAST_COMPILE_DATE, nullptr);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());

	if(GIT_SHORTREV_HASH)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Git revision hash: %s", GIT_SHORTREV_HASH);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", aBuf);
	}

	const char *pSourceUrl = Config()->m_AboutSourceUrl;
	if(pSourceUrl[0])
	{
		Buffer = Server()->Localization()->Format_L(pLanguage, "Sources: {str:SourceUrl} ", _("SourceUrl"), pSourceUrl,
			nullptr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());
	}

	if(Config()->m_AboutContactsDiscord[0])
	{
		Buffer = Server()->Localization()->Format_L(pLanguage, "Discord: {str:Url}", _("Url"), Config()->m_AboutContactsDiscord,
			nullptr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());
	}
	if(Config()->m_AboutContactsTelegram[0])
	{
		Buffer = Server()->Localization()->Format_L(pLanguage, "Telegram: {str:Url}", _("Url"), Config()->m_AboutContactsTelegram,
			nullptr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());
	}
	if(Config()->m_AboutContactsMatrix[0])
	{
		Buffer = Server()->Localization()->Format_L(pLanguage, "Matrix room: {str:Url}", _("Url"), Config()->m_AboutContactsMatrix,
			nullptr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());
	}
	if(Config()->m_AboutTranslationUrl[0])
	{
		Buffer = Server()->Localization()->Format_L(pLanguage, "Translation project: {str:Url}", _("Url"), Config()->m_AboutTranslationUrl,
			nullptr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());
	}

	Buffer = Server()->Localization()->Format_L(pLanguage, "See also: /credits", _(nullptr));
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Buffer.c_str());
	Buffer.clear();
}

void CGameContext::PrivateMessage(const char *pStr, int ClientId, bool TeamChat)
{
	if(ProcessSpamProtection(ClientId))
		return;

	bool ArgumentFound = false;
	const char *pArgumentIter = pStr;
	while(*pArgumentIter)
	{
		if(*pArgumentIter != ' ')
		{
			ArgumentFound = true;
			break;
		}

		pArgumentIter++;
	}

	if(!ArgumentFound)
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Usage: /msg <username or group> <message>"), nullptr);
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Send a private message to a player or a group of players"), nullptr);
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Available groups: !near, !engineer, !soldier, ..."), nullptr);
		return;
	}

	dynamic_string FinalMessage;
	int TextIter = 0;

	bool CheckDistance = false;
	vec2 CheckDistancePos = vec2(0.0f, 0.0f);

	std::optional<int> CheckTeam;
	EPlayerClass CheckClass = EPlayerClass::Invalid;

	if(TeamChat && m_apPlayers[ClientId])
	{
		if(m_apPlayers[ClientId]->GetTeam() == TEAM_SPECTATORS)
			CheckTeam = TEAM_SPECTATORS;
		if(m_apPlayers[ClientId]->IsInfected())
			CheckTeam = TEAM_RED;
		else
			CheckTeam = TEAM_BLUE;
	}

	char aNameFound[32];
	aNameFound[0] = 0;

	char aChatTitle[32];
	aChatTitle[0] = 0;
	unsigned int c = 0;
	for(; c < sizeof(aNameFound) - 1; c++)
	{
		if(pStr[c] == ' ' || pStr[c] == 0)
		{
			if(str_comp(aNameFound, "!near") == 0)
			{
				if(m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
				{
					CheckDistance = true;
					CheckDistancePos = m_apPlayers[ClientId]->GetCharacter()->m_Pos;
					str_copy(aChatTitle, "near");
				}
			}
			else if(str_comp(aNameFound, "!engineer") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Engineer;
				str_copy(aChatTitle, "engineer");
			}
			else if(str_comp(aNameFound, "!soldier ") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Soldier;
				str_copy(aChatTitle, "soldier");
			}
			else if(str_comp(aNameFound, "!scientist") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Scientist;
				str_copy(aChatTitle, "scientist");
			}
			else if(str_comp(aNameFound, "!biologist") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Biologist;
				str_copy(aChatTitle, "biologist");
			}
			else if(str_comp(aNameFound, "!looper") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Looper;
				str_copy(aChatTitle, "looper");
			}
			else if(str_comp(aNameFound, "!medic") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Medic;
				str_copy(aChatTitle, "medic");
			}
			else if(str_comp(aNameFound, "!hero") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Hero;
				str_copy(aChatTitle, "hero");
			}
			else if(str_comp(aNameFound, "!ninja") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Ninja;
				str_copy(aChatTitle, "ninja");
			}
			else if(str_comp(aNameFound, "!mercenary") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Mercenary;
				str_copy(aChatTitle, "mercenary");
			}
			else if(str_comp(aNameFound, "!sniper") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Sniper;
				str_copy(aChatTitle, "sniper");
			}
			else if(str_comp(aNameFound, "!artillery") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Sniper;
				str_copy(aChatTitle, "artillery");
			}
			else if(str_comp(aNameFound, "!smoker") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Smoker;
				str_copy(aChatTitle, "smoker");
			}
			else if(str_comp(aNameFound, "!hunter") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Hunter;
				str_copy(aChatTitle, "hunter");
			}
			else if(str_comp(aNameFound, "!bat") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Bat;
				str_copy(aChatTitle, "bat");
			}
			else if(str_comp(aNameFound, "!boomer") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Boomer;
				str_copy(aChatTitle, "boomer");
			}
			else if(str_comp(aNameFound, "!spider") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Spider;
				str_copy(aChatTitle, "spider");
			}
			else if(str_comp(aNameFound, "!ghost") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Ghost;
				str_copy(aChatTitle, "ghost");
			}
			else if(str_comp(aNameFound, "!ghoul") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Ghoul;
				str_copy(aChatTitle, "ghoul");
			}
			else if(str_comp(aNameFound, "!slug") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Slug;
				str_copy(aChatTitle, "slug");
			}
			else if(str_comp(aNameFound, "!undead") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Undead;
				str_copy(aChatTitle, "undead");
			}
			else if(str_comp(aNameFound, "!witch") == 0 && m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetCharacter())
			{
				CheckClass = EPlayerClass::Witch;
				str_copy(aChatTitle, "witch");
			}
			else
			{
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(m_apPlayers[i] && str_comp(Server()->ClientName(i), aNameFound) == 0)
					{
						const char *pMessage = pStr[c] == 0 ? &pStr[c] : &pStr[c + 1];
						WhisperId(ClientId, i, pMessage);
						return;
					}
				}
			}
		}

		if(aChatTitle[0] || pStr[c] == 0)
		{
			aNameFound[c] = 0;
			break;
		}
		else
		{
			aNameFound[c] = pStr[c];
			aNameFound[c + 1] = 0;
		}
	}

	if(!aChatTitle[0])
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("No player was found with this name"));
		return;
	}

	pStr += c;
	while(*pStr == ' ')
		pStr++;

	std::string Buffer;
	Buffer.append(pStr);
	Server()->Localization()->ArabicShaping(Buffer);

	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = (TeamChat ? 1 : 0);
	Msg.m_ClientId = ClientId;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && !CGameContext::m_ClientMuted[i][ClientId])
		{
			if(i != ClientId)
			{
				if(CheckTeam.has_value())
				{
					if(CheckTeam == TEAM_SPECTATORS && m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
						continue;
					else if(CheckTeam == TEAM_RED && m_apPlayers[i]->IsHuman())
						continue;
					else if(CheckTeam == TEAM_BLUE && m_apPlayers[i]->IsInfected())
						continue;
				}
				if((CheckClass != EPlayerClass::Invalid) && !(m_apPlayers[i]->GetClass() == CheckClass))
					continue;

				if(CheckDistance && !(m_apPlayers[i]->GetCharacter() && distance(m_apPlayers[i]->GetCharacter()->m_Pos, CheckDistancePos) < 1000.0f))
					continue;
			}

			FinalMessage.clear();
			TextIter = 0;
			if(i == ClientId)
			{
				if(str_comp(aChatTitle, "private") == 0)
				{
					TextIter = FinalMessage.append_at(TextIter, aNameFound);
					TextIter = FinalMessage.append_at(TextIter, " (");
					TextIter = FinalMessage.append_at(TextIter, aChatTitle);
					TextIter = FinalMessage.append_at(TextIter, "): ");
				}
				else
				{
					TextIter = FinalMessage.append_at(TextIter, "(");
					TextIter = FinalMessage.append_at(TextIter, aChatTitle);
					TextIter = FinalMessage.append_at(TextIter, "): ");
				}
				TextIter = FinalMessage.append_at(TextIter, Buffer.c_str());
			}
			else
			{
				TextIter = FinalMessage.append_at(TextIter, Server()->ClientName(i));
				TextIter = FinalMessage.append_at(TextIter, " (");
				TextIter = FinalMessage.append_at(TextIter, aChatTitle);
				TextIter = FinalMessage.append_at(TextIter, "): ");
				TextIter = FinalMessage.append_at(TextIter, Buffer.c_str());
			}
			Msg.m_pMessage = FinalMessage.buffer();

			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
		}
	}
}

void CGameContext::MutePlayer(int ClientId, char *pStr)
{
	const char *pName = ParseStringArgumentInplace(pStr);
	if(pName == nullptr || pName[0] == '\0')
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Invalid player name"), nullptr);
		return;
	}

	std::optional<int> Target = GetClientId(pName);

	if(!Target.has_value())
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("No player with name \"{str:PlayerName}\" found"),
			"PlayerName", pName, nullptr);
		return;
	}
	const int TargetId = Target.value();

	CGameContext::m_ClientMuted[ClientId][TargetId] = !CGameContext::m_ClientMuted[ClientId][TargetId];
	if(CGameContext::m_ClientMuted[ClientId][TargetId])
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Player muted. Mute will persist until you or the muted player disconnects."), nullptr);
	}
	else
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Player unmuted. You can see their messages again."), nullptr);
	}
}

void CGameContext::InitGeolocation()
{
#ifdef CONF_GEOLOCATION
	const char aGeoDBFileName[] = "geo/GeoLite2-Country.mmdb";
	char aBuf[512];
	Storage()->GetDataPath(aGeoDBFileName, aBuf, sizeof(aBuf));
	if(aBuf[0])
	{
		Geolocation::Initialize(aBuf);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "Unable to find geolocation data file %s", aGeoDBFileName);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
#endif
}

void CGameContext::ConRegister(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int ClientId = pResult->GetClientId();
	const char *pLogin = pResult->GetString(0);
	const char *pPassword = pResult->GetString(1);

	pSelf->Server()->Register(ClientId, pLogin, pPassword);
}

void CGameContext::ConLogin(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int ClientId = pResult->GetClientId();
	const char *pLogin = pResult->GetString(0);
	const char *pPassword = pResult->GetString(1);

	pSelf->Server()->Login(ClientId, pLogin, pPassword);
}

void CGameContext::ConLogout(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int ClientId = pResult->GetClientId();

	pSelf->Server()->Logout(ClientId);
}

void CGameContext::ConHelp(IConsole::IResult *pResult, void *pUserData)
{
	int ClientId = pResult->GetClientId();
	const char *pHelpPage = (pResult->NumArguments() > 0) ? pResult->GetString(0) : nullptr;

	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->ChatHelp(ClientId, pHelpPage);
}

void CGameContext::ChatHelp(int ClientId, const char *pHelpPage)
{
	const char *pLanguage = m_apPlayers[ClientId]->GetLanguage();

	dynamic_string Buffer;

	if(pHelpPage && ((str_comp_nocase(pHelpPage, "accounts") == 0) || (str_comp_nocase(pHelpPage, "registration") == 0)))
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, _("Accounts"), nullptr).c_str());
		Buffer.append(" ~~\n\n");
		bool AccountsDisabled{};
		if(str_comp(Config()->m_SvAccounts, "mandatory") == 0)
		{
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "You have to use an account to play on this server."), nullptr).c_str());
		}
		else if(str_comp(Config()->m_SvAccounts, "enabled") == 0)
		{
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "You can use an account to unlock some features on this server."), nullptr).c_str());
		}
		else
		{
			AccountsDisabled = true;
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "Account system is currently disabled."), nullptr).c_str());
		}
		if(!AccountsDisabled)
		{
			Buffer.append("\n\n");
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "You can use RCON interface or chat commands to register and log in."), nullptr).c_str());
			Buffer.append("\n\n");
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "Use `/register` without arguments to start registration via RCON."), nullptr).c_str());
			Buffer.append("\n\n");
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "Use `/register <username> <password>` to register via chat commands."), nullptr).c_str());
			Buffer.append("\n\n");
			Buffer.append(Server()->Localization()->Format_L(pLanguage, _C("Accounts", "Use RCON or `/login <username> <password>` to login into an existing account."), nullptr).c_str());
		}
	}
	else
	{
		m_pController->GetHelpText(&Buffer, ClientId, pHelpPage);
	}

	if(Buffer.empty())
	{
		const std::string Hint(Server()->Localization()->Localize(pLanguage, _("Choose a help page with /help <page>")));
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Hint.c_str());

		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", Server()->Localization()->Format_L(pLanguage, "Available help pages: {str:PageList}", _("PageList"), "game, translate, msg, mute, taxi", nullptr).c_str());

		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", "engineer, soldier, scientist, biologist, looper, medic, hero, ninja, mercenary, sniper, artillery");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp", "whitehole, smoker, hunter, bat, boomer, ghost, spider, ghoul, slug, voodoo, undead, witch.");
	}
	else
	{
		SendMOTD(ClientId, Buffer.buffer());
	}
}

void CGameContext::ConRules(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	bool Printed = false;
	if(g_Config.m_SvDDRaceRules)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp",
			"Be nice and respect others privacy.");
		Printed = true;
	}
	char *apRuleLines[] = {
		g_Config.m_SvRulesLine1,
		g_Config.m_SvRulesLine2,
		g_Config.m_SvRulesLine3,
		g_Config.m_SvRulesLine4,
		g_Config.m_SvRulesLine5,
		g_Config.m_SvRulesLine6,
		g_Config.m_SvRulesLine7,
		g_Config.m_SvRulesLine8,
		g_Config.m_SvRulesLine9,
		g_Config.m_SvRulesLine10,
	};
	for(auto &pRuleLine : apRuleLines)
	{
		if(pRuleLine[0])
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD,
				"chatresp", pRuleLine);
			Printed = true;
		}
	}
	if(!Printed)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chatresp",
			"No Rules Defined, Kill em all!!");
	}
}

void CGameContext::ConLanguage(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

	int ClientId = pResult->GetClientId();

	const char *pLanguageCode = (pResult->NumArguments() > 0) ? pResult->GetString(0) : nullptr;
	char aFinalLanguageCode[8];
	aFinalLanguageCode[0] = 0;

	if(pLanguageCode)
	{
		if(str_comp_nocase(pLanguageCode, "ua") == 0)
			str_copy(aFinalLanguageCode, "uk");
		else
		{
			for(const auto &Key : pSelf->Server()->Localization()->m_pLanguages | std::views::keys)
			{
				if(str_comp_nocase(pLanguageCode, Key.c_str()) == 0)
					str_copy(aFinalLanguageCode, pLanguageCode);
			}
		}
	}

	if(aFinalLanguageCode[0])
	{
		pSelf->SetClientLanguage(ClientId, aFinalLanguageCode);
	}
	else
	{
		const char *pLanguage = pSelf->m_apPlayers[ClientId]->GetLanguage();
		const std::string TxtUnknownLanguage(pSelf->Server()->Localization()->Localize(pLanguage, _("Unknown language")));
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "language", TxtUnknownLanguage.c_str());

		dynamic_string BufferList;
		int BufferIter = 0;
		int i = 0;
		for(const auto &Key : pSelf->Server()->Localization()->m_pLanguages | std::views::keys)
		{
			if(i > 0)
				BufferIter = BufferList.append_at(BufferIter, ", ");
			BufferIter = BufferList.append_at(BufferIter, Key.c_str());
			i++;
		}

		const auto Buffer = pSelf->Server()->Localization()->Format_L(pLanguage, "Available languages: {str:ListOfLanguage}", _("ListOfLanguage"), BufferList.buffer(), nullptr);

		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "language", Buffer.c_str());
	}
}

void CGameContext::ConCmdList(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	int ClientId = pResult->GetClientId();
	const char *pLanguage = pSelf->m_apPlayers[ClientId]->GetLanguage();

	std::string Buffer;

	Buffer.append("~~ ");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "List of commands").c_str());
	Buffer.append(" ~~\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "/antiping, /alwaysrandom, /customskin, /help, /about, /language", nullptr).c_str());
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "/msg, /mute", nullptr).c_str());
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "/changelog", nullptr).c_str());
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "/register, /login, /logout", nullptr).c_str());
	Buffer.append("\n\n");
	Buffer.append(pSelf->Server()->Localization()->Format_L(pLanguage, "Press <F3> or <F4> to enable or disable hook protection", _(nullptr)).c_str());

	pSelf->SendMOTD(ClientId, Buffer.c_str());
}

void CGameContext::ConChangeLog(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->ConChangeLog(pResult);
}

void CGameContext::ConChangeLog(IConsole::IResult *pResult)
{
	int ClientId = pResult->GetClientId();

	if(m_aChangeLogEntries.Size() == 0)
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("ChangeLog is not provided"), nullptr);
		return;
	}

	int PageNumber = pResult->GetInteger(0);
	if(PageNumber <= 0)
	{
		PageNumber = 1;
	}
	int PagesInTotal = m_aChangeLogPageIndices.Size();
	if(PageNumber > PagesInTotal)
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("ChangeLog page {int:PageNumber} is not available"),
			"PageNumber", &PageNumber,
			nullptr);
		return;
	}

	uint32_t PageIndex = PageNumber - 1;
	uint32_t From = m_aChangeLogPageIndices.At(PageIndex);
	uint32_t To = (PageIndex + 1) < m_aChangeLogPageIndices.Size() ? m_aChangeLogPageIndices.At(PageIndex + 1) : m_aChangeLogEntries.Size();

	for(uint32_t i = From; i < To; ++i)
	{
		const std::string &Text = m_aChangeLogEntries.At(i);
		SendChatTarget(ClientId, Text.c_str());
	}

	if(PageNumber != PagesInTotal)
	{
		int NextPage = PageNumber + 1;

		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("(page {int:PageNumber}/{int:PagesInTotal}, see /changelog {int:NextPage})"),
			"PageNumber", &PageNumber,
			"PagesInTotal", &PagesInTotal,
			"NextPage", &NextPage,
			nullptr);
	}
}

void CGameContext::ConReloadChangeLog(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->ReloadChangelog();
}

/* INFECTION MODIFICATION END *****************************************/

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pLua = Kernel()->RequestInterface<ILua>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	Console()->Register("tune", "s[tuning] i[value]", CFGFLAG_SERVER | CFGFLAG_GAME, ConTuneParam, this, "Tune variable to value");
	Console()->Register("toggle_tune", "s[tuning] i[value 1] i[value 2]", CFGFLAG_SERVER | CFGFLAG_GAME, ConToggleTuneParam, this, "Toggle tune variable");
	Console()->Register("tune_reset", "", CFGFLAG_SERVER, ConTuneReset, this, "Reset tuning");
	Console()->Register("tune_dump", "", CFGFLAG_SERVER, ConTuneDump, this, "Dump tuning");
	Console()->Register("pause_game", "", CFGFLAG_SERVER, ConPause, this, "Pause/unpause game");
	Console()->Register("change_map", "?r[map]", CFGFLAG_SERVER | CFGFLAG_STORE, ConChangeMap, this, "Change map");
	Console()->Register("restart", "?i[seconds]", CFGFLAG_SERVER | CFGFLAG_STORE, ConRestart, this, "Restart in x seconds (0 = abort)");
	Console()->Register("broadcast", "r[message]", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("say", "r[message]", CFGFLAG_SERVER, ConSay, this, "Say in chat");
	Console()->Register("set_team", "i[id] i[team-id] ?i[delay in minutes]", CFGFLAG_SERVER, ConSetTeam, this, "Set team of player to team");
	Console()->Register("set_team_all", "i[team-id]", CFGFLAG_SERVER, ConSetTeamAll, this, "Set team of all players to team");

	Console()->Register("insert_vote", "i[position] s[name] r[command]", CFGFLAG_SERVER, ConInsertVote, this, "Insert a voting option");
	Console()->Register("add_vote", "s[name] r[command]", CFGFLAG_SERVER, ConAddVote, this, "Add a voting option");
	Console()->Register("remove_vote", "r[name]", CFGFLAG_SERVER, ConRemoveVote, this, "remove a voting option");
	Console()->Register("force_vote", "s[name] s[command] ?r[reason]", CFGFLAG_SERVER, ConForceVote, this, "Force a voting option");
	Console()->Register("clear_votes", "", CFGFLAG_SERVER, ConClearVotes, this, "Clears the voting options");
	Console()->Register("add_map_votes", "", CFGFLAG_SERVER, ConAddMapVotes, this, "Automatically adds voting options for all maps");
	Console()->Register("vote", "r['yes'|'no']", CFGFLAG_SERVER, ConVote, this, "Force a vote to yes/no");

	/* INFECTION MODIFICATION START ***************************************/
	Console()->Register("skip_map", "", CFGFLAG_SERVER, ConSkipMap, this, "Change map to the next in the rotation");
	Console()->Register("queue_map", "?r[map]", CFGFLAG_SERVER, ConQueueMap, this, "Set the next map");
	Console()->Register("add_map", "?r[map]", CFGFLAG_SERVER, ConAddMap, this, "Add a map to the maps rotation list");
	Console()->Register("remove_map", "?r[map]", CFGFLAG_SERVER, ConRemoveMap, this, "Remove a map from the maps rotation list");
	Console()->Register("clear_maps", "", CFGFLAG_SERVER, ConClearMaps, this, "Remove all maps from the maps rotation list");

	Console()->Register("kill_pl", "v[id]", CFGFLAG_SERVER, ConKillPlayer, this, "Kills player v and announces the kill");
	// Chat Command
	Console()->Register("version", "", CFGFLAG_SERVER, ConVersion, this, "Display information about the server version and build");

	Console()->Register("about", "", CFGFLAG_CHAT, ConAbout, this, "Display information about the mod");

	Console()->Register("register", "?s[name] ?s[password]", CFGFLAG_CHAT, ConRegister, this, "Start creating an account");
	Console()->Register("login", "s[username] s[password]", CFGFLAG_CHAT, ConLogin, this, "Login to an account");
	Console()->Register("logout", "", CFGFLAG_CHAT, ConLogout, this, "Logout");

	Console()->Register("reload_changelog", "?i[page]", CFGFLAG_SERVER, ConReloadChangeLog, this, "Reload the changelog file");

	/* INFECTION MODIFICATION END *****************************************/

	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);

	Console()->Chain("sv_maprotation", ConchainSyncMapRotation, this);

	RegisterChatCommands();

	InitGeolocation();
}

void CGameContext::RegisterChatCommands()
{
	Console()->Register("credits", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConCredits, this, "Shows the credits of the DDNet mod");
	Console()->Register("rules", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConRules, this, "Shows the server rules");
	Console()->Register("help", "?s[page]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConHelp, this, "Display help");
	Console()->Register("info", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConInfo, this, "Shows info about this server");
	Console()->Register("changelog", "?i[page]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConChangeLog, this, "Display a changelog page");
	Console()->Register("cmdlist", "", CFGFLAG_CHAT, ConCmdList, this, "List of commands");

	Console()->Register("me", "r[message]", CFGFLAG_CHAT | CFGFLAG_NONTEEHISTORIC, ConMe, this, "Like the famous irc command '/me says hi' will display '<yourname> says hi'");
	Console()->Register("w", "s[player name] r[message]", CFGFLAG_CHAT | CFGFLAG_NONTEEHISTORIC, ConWhisper, this, "Whisper something to someone (private message)");
	Console()->Register("whisper", "s[player name] r[message]", CFGFLAG_CHAT | CFGFLAG_NONTEEHISTORIC, ConWhisper, this, "Whisper something to someone (private message)");
	Console()->Register("c", "r[message]", CFGFLAG_CHAT | CFGFLAG_NONTEEHISTORIC, ConConverse, this, "Converse with the last person you whispered to (private message)");
	Console()->Register("converse", "r[message]", CFGFLAG_CHAT | CFGFLAG_NONTEEHISTORIC, ConConverse, this, "Converse with the last person you whispered to (private message)");
	Console()->Register("msg", "s[player or group name] r[message]", CFGFLAG_CHAT | CFGFLAG_NONTEEHISTORIC, ConConverse, this, "Check '/help msg' for details");

	Console()->Register("mute", "r[player name]", CFGFLAG_CHAT, ConMute, this, "Mute player with specified id for x minutes for any reason");

	Console()->Register("timeout", "?s[code]", CFGFLAG_CHAT, ConTimeout, this, "Set timeout protection code s");

	static char aLangs[256] = {};
	if(!aLangs[0] && Server()->Localization())
	{
		dynamic_string BufferList;
		int BufferIter = 0;
		int i = 0;
		for(const auto &Key : Server()->Localization()->m_pLanguages | std::views::keys)
		{
			if(i > 0)
				BufferIter = BufferList.append_at(BufferIter, "|");
			BufferIter = BufferList.append_at(BufferIter, Key.c_str());
			i++;
		}
		if(!BufferList.empty())
		{
			str_format(aLangs, sizeof(aLangs), "s[%s]", BufferList.buffer());
		}
	}

	if(aLangs[0])
	{
		Console()->Register("language", aLangs, CFGFLAG_CHAT, ConLanguage, this, "Set the language");
		Console()->Register("lang", aLangs, CFGFLAG_CHAT, ConLanguage, this, "Set the language");
	}
}

void CGameContext::OnInit(const void *pPersistentData)
{
	[[maybe_unused]] const CPersistentData *pPersistent = static_cast<const CPersistentData *>(pPersistentData);

	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pLua = Kernel()->RequestInterface<ILua>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pEngine = nullptr;
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	CLua::SetStaticVars(m_pServer, this);

	m_GameUuid = RandomUuid();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		Server()->ResetClientMemoryAboutGame(i);
	}

	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_VoteLanguageTick[i] = 0;
		str_copy(m_VoteLanguage[i], Config()->m_InfDefaultLanguageCode);
	}

	m_Layers.Init(Kernel());
	m_Collision.Init(&m_Layers);

	Lua()->StartupLua();
	// select gametype
	if(!str_comp(Config()->m_SvGametype, "mod"))
		m_pController = CreateInfclassModController(this);
	else
		m_pController = new IGameController(this);
	m_pController->RegisterChatCommands(Console());

	Console()->ExecuteFile(g_Config.m_SvResetFile);

	InitChangelog();
	m_pController->InitSmartMapRotation();

	{
		// Open file
		char *pMapShortName = &g_Config.m_SvMap[0];
		char MapCfgFilename[512];
		str_format(MapCfgFilename, sizeof(MapCfgFilename), "maps/%s.cfg", pMapShortName);
		Console()->ExecuteFile(MapCfgFilename);
	}

	// create all entities from the game layer
	CreateAllEntities(true);

	if(Config()->m_SvLua >= 2)
	{
		char aLuaFilename[512];
		if(Config()->m_SvLuaRuntime[0])
		{
			str_format(aLuaFilename, sizeof(aLuaFilename), "lua/runtime/%s", Config()->m_SvLuaRuntime);
			Lua()->ExecScriptFile(aLuaFilename);
		}

		if(Config()->m_SvLua >= 2)
		{
			// Open file
			const char *pMapShortName = &g_Config.m_SvMap[0];
			str_format(aLuaFilename, sizeof(aLuaFilename), "lua/maps/%s.lua", pMapShortName);
			Lua()->ExecScriptFile(aLuaFilename);
		}

		RunCallback(Lua()->GetLuaState(), "on_loaded");
	}

	if(GIT_SHORTREV_HASH)
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "git-revision", GIT_SHORTREV_HASH);

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies; i++)
		{
			OnClientConnected(MAX_CLIENTS - i - 1, nullptr);
		}
	}
#endif
}

void CGameContext::CreateAllEntities(bool Initial)
{
	const CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	const CTile *pTiles = static_cast<CTile *>(Kernel()->RequestInterface<IMap>()->GetData(pTileMap->m_Data));

	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			const int Index = y * pTileMap->m_Width + x;

			// Game layer
			{
				const int GameIndex = pTiles[Index].m_Index;
				if(GameIndex >= ENTITY_OFFSET)
				{
					m_pController->OnEntity(GameIndex - ENTITY_OFFSET, x, y, LAYER_GAME, pTiles[Index].m_Flags, Initial);
				}
			}
		}
	}

	// create all entities from entity layers
	if(m_Layers.EntityGroup())
	{
		char aLayerName[12];

		const CMapItemGroup *pGroup = m_Layers.EntityGroup();
		for(int l = 0; l < pGroup->m_NumLayers; l++)
		{
			const CMapItemLayer *pLayer = m_Layers.GetLayer(pGroup->m_StartLayer + l);
			if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				const CMapItemLayerQuads *pQLayer = reinterpret_cast<const CMapItemLayerQuads *>(pLayer);
				IntsToStr(pQLayer->m_aName, std::size(pQLayer->m_aName), aLayerName, std::size(aLayerName));

				const CQuad *pQuads = static_cast<const CQuad *>(Kernel()->RequestInterface<IMap>()->GetDataSwapped(pQLayer->m_Data));

				for(int q = 0; q < pQLayer->m_NumQuads; q++)
				{
					vec2 P0(fx2f(pQuads[q].m_aPoints[0].x), fx2f(pQuads[q].m_aPoints[0].y));
					vec2 P1(fx2f(pQuads[q].m_aPoints[1].x), fx2f(pQuads[q].m_aPoints[1].y));
					vec2 P2(fx2f(pQuads[q].m_aPoints[2].x), fx2f(pQuads[q].m_aPoints[2].y));
					vec2 P3(fx2f(pQuads[q].m_aPoints[3].x), fx2f(pQuads[q].m_aPoints[3].y));
					vec2 Pivot(fx2f(pQuads[q].m_aPoints[4].x), fx2f(pQuads[q].m_aPoints[4].y));
					m_pController->OnEntity(aLayerName, Pivot, P0, P1, P2, P3, pQuads[q].m_PosEnv);
				}
			}
		}
	}
}

void CGameContext::OnMapChange(char *pNewMapName, int MapNameSize)
{
}

void CGameContext::OnShutdown(void *pPersistentData)
{
	m_pController->OnShutdown();

	auto *pPersistent = static_cast<CPersistentData *>(pPersistentData);

	if(pPersistent)
	{
		pPersistent->m_PrevGameUuid = m_GameUuid;
	}

	// reset votes.
	EndVote();

	m_pController = nullptr;
	Clear();
	delete m_pController;
}

void CGameContext::OnSnap(int ClientId)
{
	// add tuning to demo
	CTuningParams StandardTuning;
	if(Server()->IsRecording(ClientId > -1 ? ClientId : MAX_CLIENTS) && mem_comp(&StandardTuning, &m_Tuning, sizeof(CTuningParams)) != 0)
	{
		CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
		int *pParams = reinterpret_cast<int *>(&m_Tuning);
		for(unsigned i = 0; i < sizeof(m_Tuning) / sizeof(int); i++)
			Msg.AddInt(pParams[i]);
		Server()->SendMsg(&Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND, ClientId);
	}

	m_World.Snap(ClientId);
	m_pController->Snap(ClientId);
	m_Events.Snap(ClientId);

	/* INFECTION MODIFICATION START ***************************************/
	int SnappingClientVersion = GetClientVersion(ClientId);
	CSnapContext Context(SnappingClientVersion);
	// Snap laser dots
	for(int i = 0; i < m_LaserDots.size(); i++)
	{
		if(ClientId >= 0)
		{
			vec2 CheckPos = (m_LaserDots[i].m_Pos0 + m_LaserDots[i].m_Pos1) * 0.5f;
			float dx = m_apPlayers[ClientId]->m_ViewPos.x - CheckPos.x;
			float dy = m_apPlayers[ClientId]->m_ViewPos.y - CheckPos.y;
			if(absolute(dx) > 1000.0f || absolute(dy) > 800.0f)
				continue;
			if(distance(m_apPlayers[ClientId]->m_ViewPos, CheckPos) > 1100.0f)
				continue;
		}

		SnapLaserObject(Context, m_LaserDots[i].m_SnapId, m_LaserDots[i].m_Pos1, m_LaserDots[i].m_Pos0, Server()->Tick());
	}
	for(int i = 0; i < m_HammerDots.size(); i++)
	{
		if(ClientId >= 0)
		{
			vec2 CheckPos = m_HammerDots[i].m_Pos;
			float dx = m_apPlayers[ClientId]->m_ViewPos.x - CheckPos.x;
			float dy = m_apPlayers[ClientId]->m_ViewPos.y - CheckPos.y;
			if(absolute(dx) > 1000.0f || absolute(dy) > 800.0f)
				continue;
			if(distance(m_apPlayers[ClientId]->m_ViewPos, CheckPos) > 1100.0f)
				continue;
		}

		CNetObj_Projectile *pObj = Server()->SnapNewItem<CNetObj_Projectile>(m_HammerDots[i].m_SnapId);
		if(pObj)
		{
			pObj->m_X = static_cast<int>(m_HammerDots[i].m_Pos.x);
			pObj->m_Y = static_cast<int>(m_HammerDots[i].m_Pos.y);
			pObj->m_VelX = 0;
			pObj->m_VelY = 0;
			pObj->m_StartTick = Server()->Tick();
			pObj->m_Type = WEAPON_HAMMER;
		}
	}
	for(int i = 0; i < m_LoveDots.size(); i++)
	{
		if(ClientId >= 0)
		{
			vec2 CheckPos = m_LoveDots[i].m_Pos;
			float dx = m_apPlayers[ClientId]->m_ViewPos.x - CheckPos.x;
			float dy = m_apPlayers[ClientId]->m_ViewPos.y - CheckPos.y;
			if(absolute(dx) > 1000.0f || absolute(dy) > 800.0f)
				continue;
			if(distance(m_apPlayers[ClientId]->m_ViewPos, CheckPos) > 1100.0f)
				continue;
		}

		CNetObj_Pickup *pObj = Server()->SnapNewItem<CNetObj_Pickup>(m_LoveDots[i].m_SnapId);
		if(pObj)
		{
			pObj->m_X = static_cast<int>(m_LoveDots[i].m_Pos.x);
			pObj->m_Y = static_cast<int>(m_LoveDots[i].m_Pos.y);
			pObj->m_Type = POWERUP_HEALTH;
			pObj->m_Subtype = 0;
		}
	}
	/* INFECTION MODIFICATION END *****************************************/

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
			m_apPlayers[i]->Snap(ClientId);
	}
}

void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_Events.Clear();
}

std::optional<CViewParams> CGameContext::GetClientViewParams(int SnappingClient) const
{
	if(SnappingClient == SERVER_DEMO_CLIENT || m_apPlayers[SnappingClient]->m_ShowAll)
		return {};

	const CPlayer *pPlayer = m_apPlayers[SnappingClient];
	return CViewParams{pPlayer->m_ViewPos, pPlayer->m_ShowDistance};
}

bool CGameContext::NetworkClipped(int SnappingClient, vec2 CheckPos) const
{
	if(SnappingClient == SERVER_DEMO_CLIENT || m_apPlayers[SnappingClient]->m_ShowAll)
		return false;

	float dx = m_apPlayers[SnappingClient]->m_ViewPos.x - CheckPos.x;
	if(absolute(dx) > m_apPlayers[SnappingClient]->m_ShowDistance.x)
		return true;

	float dy = m_apPlayers[SnappingClient]->m_ViewPos.y - CheckPos.y;
	return absolute(dy) > m_apPlayers[SnappingClient]->m_ShowDistance.y;
}

bool CGameContext::NetworkClippedLine(int SnappingClient, vec2 StartPos, vec2 EndPos) const
{
	if(SnappingClient == SERVER_DEMO_CLIENT || m_apPlayers[SnappingClient]->m_ShowAll)
		return false;

	vec2 &ViewPos = m_apPlayers[SnappingClient]->m_ViewPos;
	vec2 &ShowDistance = m_apPlayers[SnappingClient]->m_ShowDistance;

	vec2 DistanceToLine, ClosestPoint;
	if(closest_point_on_line(StartPos, EndPos, ViewPos, ClosestPoint))
	{
		DistanceToLine = ViewPos - ClosestPoint;
	}
	else
	{
		// No line section was passed but two equal points
		DistanceToLine = ViewPos - StartPos;
	}
	float ClippDistance = maximum(ShowDistance.x, ShowDistance.y);
	return (absolute(DistanceToLine.x) > ClippDistance || absolute(DistanceToLine.y) > ClippDistance);
}

void CGameContext::UpdatePlayerMaps()
{
	const auto DistCompare = [](std::pair<float, int> a, std::pair<float, int> b) -> bool {
		return (a.first < b.first);
	};

	if(Server()->Tick() % g_Config.m_SvMapUpdateRate != 0)
		return;

	std::pair<float, int> Dist[MAX_CLIENTS];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!Server()->ClientIngame(i))
			continue;
		if(Server()->GetClientVersion(i) >= VERSION_DDNET_OLD)
			continue;
		int *pMap = Server()->GetIdMap(i);

		// compute distances
		for(int j = 0; j < MAX_CLIENTS; j++)
		{
			Dist[j].second = j;
			if(j == i)
				continue;
			if(!Server()->ClientIngame(j) || !m_apPlayers[j])
			{
				Dist[j].first = 1e10;
				continue;
			}
			CCharacter *pChr = m_apPlayers[j]->GetCharacter();
			if(!pChr)
			{
				Dist[j].first = 1e9;
				continue;
			}
			if(!pChr->CanSnapCharacter(i))
				Dist[j].first = 1e8;
			else
				Dist[j].first = length_squared(m_apPlayers[i]->m_ViewPos - pChr->GetPos());
		}

		// always send the player themselves, even if all in same position
		Dist[i].first = -1;

		std::nth_element(&Dist[0], &Dist[VANILLA_MAX_CLIENTS - 1], &Dist[MAX_CLIENTS], DistCompare);

		int Index = 1; // exclude self client id
		for(int j = 0; j < VANILLA_MAX_CLIENTS - 1; j++)
		{
			pMap[j + 1] = -1; // also fill player with empty name to say chat msgs
			if(Dist[j].second == i || Dist[j].first > 5e9f)
				continue;
			pMap[Index++] = Dist[j].second;
		}

		// sort by real client ids, guarantee order on distance changes, O(Nlog(N)) worst case
		// sort just clients in game always except first (self client id) and last (fake client id) indexes
		std::sort(&pMap[1], &pMap[minimum(Index, VANILLA_MAX_CLIENTS - 1)]);
	}
}

bool CGameContext::IsClientReady(int ClientId) const
{
	return m_apPlayers[ClientId] && m_apPlayers[ClientId]->m_IsReady;
}

bool CGameContext::IsClientPlayer(int ClientId) const
{
	return m_apPlayers[ClientId] && m_apPlayers[ClientId]->GetTeam() != TEAM_SPECTATORS;
}

int CGameContext::PersistentClientDataSize() const
{
	dbg_assert(m_pController != nullptr, "There must be a controller to query the client persistent data size");
	return m_pController ? m_pController->PersistentClientDataSize() : 0;
}

CUuid CGameContext::GameUuid() const { return m_GameUuid; }
const char *CGameContext::GameType() const { return m_pController ? m_pController->GameType() : ""; }
const char *CGameContext::Version() const { return GAME_VERSION; }
const char *CGameContext::NetVersion() const { return GAME_NETVERSION; }

IGameServer *CreateGameServer() { return new CGameContext; }

void CGameContext::OnSetAuthed(int ClientId, int Level)
{
}

void CGameContext::SendRecord(int ClientId)
{
	CNetMsg_Sv_Record Msg;
	CNetMsg_Sv_RecordLegacy MsgLegacy;
	MsgLegacy.m_PlayerTimeBest = Msg.m_PlayerTimeBest = m_pController->PlayerBestRaceTime(ClientId) * 100.0f;
	MsgLegacy.m_ServerTimeBest = Msg.m_ServerTimeBest = m_pController->ServerBestRaceTime() * 100.0f; // TODO: finish this
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
	if(!Server()->IsSixup(ClientId) && GetClientVersion(ClientId) < VERSION_DDNET_MSG_LEGACY)
	{
		Server()->SendPackMsg(&MsgLegacy, MSGFLAG_VITAL, ClientId);
	}
}

void CGameContext::SendFinish(int ClientId, float Time, float PreviousBestTime)
{
	int ClientVersion = m_apPlayers[ClientId]->GetClientVersion();

	if(!Server()->IsSixup(ClientId))
	{
		CNetMsg_Sv_DDRaceTime Msg;
		CNetMsg_Sv_DDRaceTimeLegacy MsgLegacy;
		MsgLegacy.m_Time = Msg.m_Time = static_cast<int>(Time * 100.0f);
		MsgLegacy.m_Check = Msg.m_Check = 0;
		MsgLegacy.m_Finish = Msg.m_Finish = 1;

		if(PreviousBestTime)
		{
			float Diff100 = (Time - PreviousBestTime) * 100;
			MsgLegacy.m_Check = Msg.m_Check = static_cast<int>(Diff100);
		}
		if(VERSION_DDRACE <= ClientVersion)
		{
			if(ClientVersion < VERSION_DDNET_MSG_LEGACY)
			{
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
			}
			else
			{
				Server()->SendPackMsg(&MsgLegacy, MSGFLAG_VITAL, ClientId);
			}
		}
	}

	CNetMsg_Sv_RaceFinish RaceFinishMsg;
	RaceFinishMsg.m_ClientId = ClientId;
	RaceFinishMsg.m_Time = Time * 1000;
	RaceFinishMsg.m_Diff = 0;
	if(PreviousBestTime)
	{
		float Diff = absolute(Time - PreviousBestTime);
		RaceFinishMsg.m_Diff = Diff * 1000 * (Time < PreviousBestTime ? -1 : 1);
	}
	RaceFinishMsg.m_RecordPersonal = (Time < PreviousBestTime || !PreviousBestTime);
	RaceFinishMsg.m_RecordServer = Time < m_pController->ServerBestRaceTime();
	Server()->SendPackMsg(&RaceFinishMsg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);
}

bool CGameContext::ProcessSpamProtection(int ClientId, bool RespectChatInitialDelay)
{
	if(!m_apPlayers[ClientId])
		return false;
	if(g_Config.m_SvSpamprotection && m_apPlayers[ClientId]->m_LastChat && m_apPlayers[ClientId]->m_LastChat + Server()->TickSpeed() * g_Config.m_SvChatDelay > Server()->Tick())
		return true;

	m_apPlayers[ClientId]->m_LastChat = Server()->Tick();

	int Muted = 0;
	if(Server()->GetClientSession(ClientId) && Server()->GetClientSession(ClientId)->m_MuteTick > 0)
	{
		Muted = Server()->GetClientSession(ClientId)->m_MuteTick / Server()->TickSpeed();
	}

	if(Muted > 0)
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_ACCUSATION, _("You are muted for {sec:Duration}"), "Duration", &Muted, nullptr);
		return true;
	}

	return false;
}

void CGameContext::Whisper(int ClientId, char *pStr)
{
	if(ProcessSpamProtection(ClientId))
		return;

	char *pName = ParseStringArgumentInplace(pStr);
	if(pName == nullptr || pName[0] == '\0' || pStr[0] == '\0')
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Invalid whisper"), nullptr);
		return;
	}

	std::optional<int> Target = GetClientId(pName);

	if(!Target.has_value())
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("No player with name \"{str:PlayerName}\" found"),
			"PlayerName", pName, nullptr);
		return;
	}

	WhisperId(ClientId, Target.value(), pStr);
}

void CGameContext::WhisperId(int ClientId, int VictimId, const char *pMessage)
{
	if(!CheckClientId2(ClientId))
		return;

	if(!CheckClientId2(VictimId))
		return;

	if(m_apPlayers[ClientId])
	{
		m_apPlayers[ClientId]->m_LastWhisperTo = VictimId;
		m_apPlayers[ClientId]->m_LastChat = Server()->Tick();
	}

	const bool AccountsAreMandatory = str_comp(Config()->m_SvAccounts, "mandatory") == 0;
	if(AccountsAreMandatory)
	{
		if(!Server()->IsClientLogged(ClientId))
		{
			SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("You have to log in to chat or join the game"));
			return;
		}
	}

	char aCensoredMessage[256];
	CensorMessage(aCensoredMessage, pMessage, sizeof(aCensoredMessage));

	char aBuf[256];

	if(Server()->IsSixup(ClientId))
	{
		protocol7::CNetMsg_Sv_Chat Msg;
		Msg.m_ClientId = ClientId;
		Msg.m_Mode = protocol7::CHAT_WHISPER;
		Msg.m_pMessage = aCensoredMessage;
		Msg.m_TargetId = VictimId;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
	}
	else if(GetClientVersion(ClientId) >= VERSION_DDNET_WHISPER)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = CHAT_WHISPER_SEND;
		Msg.m_ClientId = VictimId;
		Msg.m_pMessage = aCensoredMessage;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "[→ %s] %s", Server()->ClientName(VictimId), aCensoredMessage);
		SendChatTarget(ClientId, aBuf);
	}

	if(ClientId == VictimId)
	{
		return;
	}

	if(CGameContext::m_ClientMuted[VictimId][ClientId])
	{
		return;
	}

	if(MessageTriggersBanOrKick(ClientId, pMessage))
		return;

	if(Server()->IsSixup(VictimId))
	{
		protocol7::CNetMsg_Sv_Chat Msg;
		Msg.m_ClientId = ClientId;
		Msg.m_Mode = protocol7::CHAT_WHISPER;
		Msg.m_pMessage = aCensoredMessage;
		Msg.m_TargetId = VictimId;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, VictimId);
	}
	else if(GetClientVersion(VictimId) >= VERSION_DDNET_WHISPER)
	{
		CNetMsg_Sv_Chat Msg2;
		Msg2.m_Team = CHAT_WHISPER_RECV;
		Msg2.m_ClientId = ClientId;
		Msg2.m_pMessage = aCensoredMessage;

		Server()->SendPackMsg(&Msg2, MSGFLAG_VITAL | MSGFLAG_NORECORD, VictimId);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "[← %s] %s", Server()->ClientName(ClientId), aCensoredMessage);
		SendChatTarget(VictimId, aBuf);
	}
}

void CGameContext::Converse(int ClientId, const char *pStr)
{
	CPlayer *pPlayer = m_apPlayers[ClientId];
	if(!pPlayer)
		return;

	if(pPlayer->m_LastWhisperTo < 0)
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("You do not have an ongoing conversation. Whisper to someone to start one"), nullptr);
	}
	else
	{
		WhisperId(ClientId, pPlayer->m_LastWhisperTo, pStr);
	}
}

bool CGameContext::IsVersionBanned(int Version)
{
	char aVersion[16];
	str_format(aVersion, sizeof(aVersion), "%d", Version);

	return str_in_list(g_Config.m_SvBannedVersions, ",", aVersion);
}

int CGameContext::GetClientVersion(int ClientId) const
{
	return Server()->GetClientVersion(ClientId);
}

bool CGameContext::RateLimitPlayerVote(int ClientId)
{
	int64_t Now = Server()->Tick();
	int64_t TickSpeed = Server()->TickSpeed();
	CPlayer *pPlayer = m_apPlayers[ClientId];

	if(g_Config.m_SvRconVote && !Server()->GetAuthedState(ClientId))
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("You can only vote after logging in."), nullptr);
		return true;
	}

	if(g_Config.m_SvSpamprotection && pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry + TickSpeed * 3 > Now)
		return true;

	pPlayer->m_LastVoteTry = Now;
	if(m_VoteCloseTime)
	{
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Wait for current vote to end before calling a new one."), nullptr);
		return true;
	}

	if(Server()->GetAuthedState(ClientId))
		return false;

	int TimeLeft = pPlayer->m_LastVoteCall + TickSpeed * g_Config.m_SvVoteDelay - Now;
	if(pPlayer->m_LastVoteCall && TimeLeft > 0)
	{
		int TimeLeftSeconds = static_cast<int>(TimeLeft / TickSpeed) + 1;
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("You must wait {int:TimeLeftSeconds} seconds before making another vote."),
			"TimeLeftSeconds", &TimeLeftSeconds, nullptr);
		return true;
	}

	return false;
}

bool CGameContext::RateLimitPlayerMapVote(int ClientId)
{
	if(!Server()->GetAuthedState(ClientId) && time_get() < m_LastMapVote + (time_freq() * g_Config.m_SvVoteMapTimeDelay))
	{
		int WaitTime = static_cast<int>((m_LastMapVote + g_Config.m_SvVoteMapTimeDelay * time_freq() - time_get()) / time_freq());
		SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("There's a {int:Delay} second delay between map-votes, please wait {int:WaitTime} seconds."),
			"Delay", &g_Config.m_SvVoteMapTimeDelay,
			"WaitTime", &WaitTime,
			nullptr);
		return true;
	}
	return false;
}

void CGameContext::OnUpdatePlayerServerInfo(char *aBuf, int BufSize, int Id)
{
	if(BufSize <= 0)
		return;

	aBuf[0] = '\0';

	if(!m_apPlayers[Id])
		return;

	char aCSkinName[64];

	CTeeInfo &TeeInfo = m_apPlayers[Id]->m_TeeInfos;

	char aJsonSkin[400];
	aJsonSkin[0] = '\0';

	if(!Server()->IsSixup(Id))
	{
		// 0.6
		if(TeeInfo.m_UseCustomColor)
		{
			str_format(aJsonSkin, sizeof(aJsonSkin),
				"\"name\":\"%s\","
				"\"color_body\":%d,"
				"\"color_feet\":%d",
				EscapeJson(aCSkinName, sizeof(aCSkinName), TeeInfo.m_aSkinName),
				TeeInfo.m_ColorBody,
				TeeInfo.m_ColorFeet);
		}
		else
		{
			str_format(aJsonSkin, sizeof(aJsonSkin),
				"\"name\":\"%s\"",
				EscapeJson(aCSkinName, sizeof(aCSkinName), TeeInfo.m_aSkinName));
		}
	}
	else
	{
		const char *apPartNames[protocol7::NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"};
		char aPartBuf[64];

		for(int i = 0; i < protocol7::NUM_SKINPARTS; ++i)
		{
			str_format(aPartBuf, sizeof(aPartBuf),
				"%s\"%s\":{"
				"\"name\":\"%s\"",
				i == 0 ? "" : ",",
				apPartNames[i],
				EscapeJson(aCSkinName, sizeof(aCSkinName), TeeInfo.m_apSkinPartNames[i]));

			str_append(aJsonSkin, aPartBuf, sizeof(aJsonSkin));

			if(TeeInfo.m_aUseCustomColors[i])
			{
				str_format(aPartBuf, sizeof(aPartBuf),
					",\"color\":%d",
					TeeInfo.m_aSkinPartColors[i]);
				str_append(aJsonSkin, aPartBuf, sizeof(aJsonSkin));
			}
			str_append(aJsonSkin, "}", sizeof(aJsonSkin));
		}
	}

	str_format(aBuf, BufSize,
		",\"skin\":{"
		"%s"
		"},"
		"\"afk\":%s,"
		"\"team\":%d",
		aJsonSkin,
		JsonBool(m_apPlayers[Id]->IsAfk()),
		m_apPlayers[Id]->GetTeam());
}

void CGameContext::ResetDefaultMaps()
{
	m_MapRotationList.clear();
	m_MapRotationList.emplace_back("infc_lunaroutpost");
	m_MapRotationList.emplace_back("infc_skull");
	m_MapRotationList.emplace_back("infc_warehouse");
	m_MapRotationList.emplace_back("infc_damascus");
	m_MapRotationList.emplace_back("infc_eidalfitr");
	m_MapRotationList.emplace_back("infc_newdust");
	m_MapRotationList.emplace_back("infc_hardcorepit");
	m_MapRotationList.emplace_back("infc_normandie");
	m_MapRotationList.emplace_back("infc_deathdealer");
	m_MapRotationList.emplace_back("infc_bamboo3");
	m_MapRotationList.emplace_back("infc_halfdust");
	m_MapRotationList.emplace_back("infc_warehouse2");
	m_MapRotationList.emplace_back("infc_malinalli_k9f");
	m_MapRotationList.emplace_back("infc_canyon");
}
