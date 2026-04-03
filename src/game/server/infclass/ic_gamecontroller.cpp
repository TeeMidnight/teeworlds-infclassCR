/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/
#include "ic_gamecontroller.h"

#include "classes/humans/human.h"
#include "engine/console.h"

#include <game/infclass/damage_type.h>
#include <game/infclass/ic_classes.h>
#include <game/server/infclass/classes/ic_playerclass.h>
#include <game/server/infclass/classes/infected/infected.h>
#include <game/server/infclass/death_context.h>
#include <game/server/infclass/entities/control-point.h>
#include <game/server/infclass/entities/flyingpoint.h>
#include <game/server/infclass/entities/ic_character.h>
#include <game/server/infclass/entities/ic_door.h>
#include <game/server/infclass/entities/ic_pickup.h>
#include <game/server/infclass/events-director.h>
#include <game/server/infclass/ic_player.h>
#include <game/server/infclass/survival.h>

#include <game/generated/protocol.h>
#include <game/mapitems.h>
#include <game/server/gamecontext.h>
#include <game/version.h>

#include <engine/lua.h>
#include <engine/message.h>
#include <engine/server/lua_callback.h>
#include <engine/server/mapconverter.h>
#include <engine/server/roundstatistics.h>
#include <engine/shared/config.h>
#include <engine/shared/network.h>
#include <engine/shared/protocolglue.h>

#include <base/tl/ic_array.h>
#include <base/tl/ic_enum.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <time.h>

#define DEBUG_PLAYER

#ifdef DEBUG_PLAYER
#include "debug-bot-player.h"
using UPlayerClass = CDebugPlayer;
#else
using UPlayerClass = CIcPlayer;
#include "bot-player.h"
#endif

#include "bot_config_parser.h"
#include "bot_utils.h"

constexpr int InfClassModeSpecialSkip = 0x100;

static const char *gs_aRoundEndReasonNames[] = {
	"finished",
	"canceled",
	"invalid",
};

const char *toString(ERoundEndReason Reason)
{
	return toStringImpl(Reason, gs_aRoundEndReasonNames);
}

static const char *gs_aRoundNames[] = {
	"classic",
	"fun",
	"fast",
	"survival",
	"hide-and-seek",
	"invalid",
};

const char *toString(ERoundType RoundType)
{
	return toStringImpl(RoundType, gs_aRoundNames);
}

template ERoundType fromString<ERoundType>(const char *pString);

static const char *gs_aCharacterSpawnTypes[] = {
	"map",
	"witch",
	"control-point",
	"scripted",
	"invalid",
};

const char *toString(SpawnContext::SPAWN_TYPE SpawnType)
{
	return toStringImpl(SpawnType, gs_aCharacterSpawnTypes);
}

class CHintMessage
{
public:
	CHintMessage(const char *pText) :
		m_pText(pText)
	{
	}

	CHintMessage(const char *pText, const char *pArg1Name, void *pArg1Value) :
		m_pText(pText),
		m_pArg1Name(pArg1Name),
		m_pArg1Value(pArg1Value)
	{
	}

	const char *m_pText{};
	const char *m_pArg1Name{};
	void *m_pArg1Value{};
};

static constexpr char gs_aInvalidWeaponIdMsg[] = "Invalid weapon id."
												 " Use 'inf_list_weapons' to get the list of the available weapons.";
static const CHintMessage gs_aHintMessages[] = {
	_("Taxi prevents ammo regeneration for all passengers."),
	_("Choosing a random class grants full armor."),
	_("You can toggle hook protection by pressing f3 (\"Vote yes\" keybind)."),
	_("Mercenary can reduce ammo usage during flight by tapping instead of holding down the \"fire\" button."),
	_("Mercenary's grenades prevent zombies from healing."),
	_("Medic can heal Heroes using the grenade launcher."),
	_("Medic and Biologist can use hammer to instantly kill an infected."),
	_("Hero can stand still for a short time to be pointed towards the flag."),
	_("Hero's turrets are great for detecting Ghosts."),
	_("Hero's flags fully restore ammo to all humans"),
	_("Soldier's bombs can hit through walls."),
	_("Ninja's slash deals 9 damage by default. One more pistol shot will kill an infected with no armor."),
	_("Ninja with a single strength upgrade can kill an armorless infected with a single slash."),
	_("Ninja doesn't need to directly kill their target. An assist with a laser blind or grenade stun will still grant the rewards."),
	_("Ninja heals slightly on a target kill."),
	_("Sniper deals double as much damage in locked position."),
	_("Scientist can use Taxi to teleport his teammates into safety."),
	{
		_P("Scientist can get a white hole after {int:Kills} kill.",
			"Scientist can get a white hole after {int:Kills} kills.", g_Config.m_InfWhiteHoleMinimalKills),
		"Kills",
		&g_Config.m_InfWhiteHoleMinimalKills,
	},
	_("Scientist can rocket jump with the laser rifle."),
	_("Biologist's hammer can stop poison effect on teammates. Look for heart icons."),
	_("Biologist's bouncy shotgun can be used to hit the infected around corners."),
	_("Smoker heals by hooking humans."),
	_("Boomer can infect through narrow walls."),
	_("Hunter receives no knockback from Medic's shotgun."),
	_("Bat can heal by hitting humans."),
	_("Spider doesn't need to be in Web mode to automatically grab any humans touching its hook."),
	_("Spider can be hooked and transported by teammates to extend its hook trap."),
	{
		_("Slug can heal itself and allies over time up to {int:MaxHP} HP with its slime."),
		"MaxHP",
		&g_Config.m_InfSlimeMaxHeal,
	},
	_("Slug can hold down the \"fire\" button to automatically spread slime. The hammer swings won't hurt humans this way though."),
	_("Voodoo can unfreeze an Undead while in Spirit mode."),
	_("Witch can spawn the infected through narrow walls."),
	_("Undead can be removed from a game by throwing it into kill tiles or reviving it as a Medic.")};

enum class ROUND_CANCELATION_REASON
{
	INVALID,
	ALL_INFECTED_LEFT_THE_GAME,
	EVERYONE_INFECTED_BY_THE_GAME,
};

class CCollisionWrapper : public ICollision
{
public:
	void SetGameContext(CGameContext *pGameContext)
	{
		m_pGameContext = pGameContext;
	}

	int GetTile(int x, int y) const override
	{
		return m_pGameContext->Collision()->GetCollisionAt(x, y);
	}

	int GameLayerWidth() const override
	{
		return m_pGameContext->Collision()->GetWidth();
	}

	int GameLayerHeight() const override
	{
		return m_pGameContext->Collision()->GetHeight();
	}

protected:
	CGameContext *m_pGameContext = nullptr;
};

class CGameDebugSink : public IDebugSink
{
public:
	void SetGameContext(CGameContext *pGameContext)
	{
		m_pGameContext = pGameContext;
	}

	bool IsVerbosityEnabled(int VerbosityLevel) const override { return VerbosityLevel < m_pGameContext->Config()->m_InfBotDebugLevel; }
	void SendFormattedMessage(int VerbosityLevel, const char *pMessage) const override
	{
		if(!IsVerbosityEnabled(VerbosityLevel))
			return;

		m_pGameContext->SendChat(-1, CGameContext::CHAT_ALL, pMessage);
	}

	void HighlightPosition(const vec2 &Position) override
	{
		m_pGameContext->CreateHammerDotEvent(Position, m_pGameContext->Server()->TickSpeed() * 3.0f);
	}

	void HighlightLineSegment(const vec2 &From, const vec2 &To) override
	{
		m_pGameContext->CreateLaserDotEvent(From, To, m_pGameContext->Server()->TickSpeed() * 2.0f);
	}

	void HighlightCircle(const vec2 &Center, float Radius, int Segments) override
	{
		if(Segments < 6)
			Segments = 6;

		float AngleStep = 2.0f * pi / Segments;
		for(int i = 0; i < Segments; ++i)
		{
			vec2 From = Center + direction(AngleStep * i) * Radius;
			vec2 To = Center + direction(AngleStep * (i + 1)) * Radius;
			HighlightLineSegment(From, To);
		}
	}

protected:
	CGameContext *m_pGameContext = nullptr;
};

struct InfclassPlayerPersistantData : public CGameContext::CPersistentClientData
{
	EPlayerScoreMode m_ScoreMode = EPlayerScoreMode::Class;
	bool m_AntiPing = false;
	EPlayerClass m_PreferredClass = EPlayerClass::Invalid;
	EPlayerClass m_PreviouslyPickedClass = EPlayerClass::Invalid;
	int m_LastInfectionTime = 0;
};

enum class ESurvivalConfigOption
{
	MaxPlayers,
	Hardmode,
	Count,
	Invalid = Count
};

static const char *gs_aSurvivalOptionNames[] = {
	"max-players",
	"hardmode",
	"invalid",
};

const char *toString(ESurvivalConfigOption RoundType)
{
	return toStringImpl(RoundType, gs_aSurvivalOptionNames);
}

template ESurvivalConfigOption fromString<ESurvivalConfigOption>(const char *pString);

int SurvivalWaveConfiguration::GetTotalInfectedLives() const
{
	int TotalBotLives = 0;
	for(const SurvivalBotConfiguration &BotConf : BotConfigurations)
	{
		int BotLives = BotConf.Lives ? BotConf.Lives : g_Config.m_InfBotLives;
		if(BotLives < 0)
			return 0;

		TotalBotLives += BotLives;
	}

	return TotalBotLives;
}

SurvivalGameConfiguration m_SurvivalConfiguration;

class CSpawnedBotsTracker
{
public:
	void ResetSpawnedBotsTracking()
	{
		m_SpawnedWaveBots = 0;
		for(bool &Spawned : m_SpawnedWaveMap)
		{
			Spawned = false;
		}
	}

	std::size_t GetSpawnedCount() const
	{
		return m_SpawnedWaveBots;
	}

	std::size_t GetFirstBotIndex() const
	{
		return 0;
	}

	bool IsBotSpawned(std::size_t BotIndex) const
	{
		return m_SpawnedWaveMap[BotIndex];
	}

	void MarkSpawned(std::size_t BotIndex)
	{
		m_SpawnedWaveMap[BotIndex] = true;
		m_SpawnedWaveBots++;
	}

private:
	std::size_t m_SpawnedWaveBots = 0;
	bool m_SpawnedWaveMap[MaxBotsPerWave] = {};
};

CSpawnedBotsTracker SpawnedBotsTracker;

int64_t CIcGameController::m_LastTipTime = 0;

IGameController *CreateInfclassModController(CGameContext *pGameServer) { return new CIcGameController(pGameServer); }

CIcGameController::CIcGameController(class CGameContext *pGameServer) : IGameController(pGameServer)
{
	m_Teams.m_Core.m_IsInfclass = true;

	for(std::vector<vec2> &vTeamSpawnPoints : m_avSpawnPoints)
	{
		vTeamSpawnPoints.reserve(32);
	}

	m_GrowingMap = nullptr;

	// Get zones
	m_ZoneHandle_icDamage = GameServer()->Collision()->GetZoneHandle("icDamage");
	m_ZoneHandle_icTeleport = GameServer()->Collision()->GetZoneHandle("icTele");
	m_ZoneHandle_icBonus = GameServer()->Collision()->GetZoneHandle("icBonus");

	m_MapWidth = GameServer()->Collision()->GetWidth();
	m_MapHeight = GameServer()->Collision()->GetHeight();
	m_GrowingMap = new int[m_MapWidth * m_MapHeight];

	m_RoundType = GetDefaultRoundType();
	m_QueuedRoundType = ERoundType::Invalid;
	m_InfectedStarted = false;
	m_VanillaMapLoaded = !str_startswith(Config()->m_SvMap, "infc_");

	for(int j = 0; j < m_MapHeight; j++)
	{
		for(int i = 0; i < m_MapWidth; i++)
		{
			const vec2 TilePos = vec2(16.0f, 16.0f) + vec2(i * 32.0f, j * 32.0f);
			if(GameServer()->Collision()->CheckPoint(TilePos))
			{
				m_GrowingMap[j * m_MapWidth + i] = 4;
			}
			else
			{
				m_GrowingMap[j * m_MapWidth + i] = 1;
			}
		}
	}

	SetHealthArmorHudEnabled(true);
	SetAmmoHudEnabled(true);

	RegisterEntityTypes();
	InitWeapons();
	ReservePlayerOwnSnapItems();
	RegisterLuaBindings();
	RegisterBotsContext();

	ResetPlayerClassesEnablement();
}

CIcGameController::~CIcGameController()
{
	RemoveBots();

	FreePlayerOwnSnapItems();

	if(m_GrowingMap)
		delete[] m_GrowingMap;
}

const char *CIcGameController::GameType() const
{
	return m_GameType.has_value() ? m_GameType.value().c_str() : "InfClassR";
}

void CIcGameController::SetGameType(const char *pGameType)
{
	m_GameType = pGameType;
	Server()->ExpireServerInfo();
}

void CIcGameController::IncreaseCurrentRoundCounter()
{
	IGameController::IncreaseCurrentRoundCounter();

	m_MoreRoundsSuggested = false;

	MaybeSuggestMoreRounds();
}

void CIcGameController::DoTeamBalance()
{
	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	const int NumPlayers = NumHumans + NumInfected;
	const int NumFirstPickedPlayers = GetMinimumInfectedForPlayers(NumPlayers);
	int PlayersToBalance = 0;
	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		PlayersToBalance = maximum<int>(0, NumFirstPickedPlayers - NumHumans);
	}
	else
	{
		PlayersToBalance = maximum<int>(0, NumFirstPickedPlayers - NumInfected);
	}

	if(PlayersToBalance == 0)
	{
		m_InfUnbalancedTick = -1;
	}
	else if(m_InfUnbalancedTick < 0)
	{
		m_InfUnbalancedTick = Server()->Tick();
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTION,
			_("The game is not balanced. Infection is coming."), nullptr);
	}
	else
	{
		const int BalancingTick = m_InfUnbalancedTick + Server()->TickSpeed() * Config()->m_InfTeamBalanceSeconds;
		if(Server()->Tick() > BalancingTick)
		{
			ForcePlayersBalance(PlayersToBalance);
		}
		else
		{
			BroadcastInfectionComing(BalancingTick);
		}
	}
}

void CIcGameController::OnPlayerConnect(CPlayer *pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);
	const int ClientId = pPlayer->GetCid();

	pPlayer->SetOriginalName(Server()->ClientName(ClientId));

	Server()->RoundStatistics()->ResetPlayer(ClientId);

	if(!Server()->ClientPrevIngame(ClientId))
	{
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER, _("{str:PlayerName} entered and joined the game"), "PlayerName", Server()->ClientName(ClientId), nullptr);

		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("InfectionClass Mod. Version: {str:VerStr}"), "VerStr", GAME_VERSION, nullptr);
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("See also: /help, /changelog, /about"), nullptr);

		if(Config()->m_AboutContactsDiscord[0])
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Join our Discord server: {str:Url}"), "Url",
				Config()->m_AboutContactsDiscord, nullptr);
		}
		if(Config()->m_AboutContactsTelegram[0])
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Join our Telegram: {str:Url}"), "Url",
				Config()->m_AboutContactsTelegram, nullptr);
		}
		if(Config()->m_AboutContactsMatrix[0])
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Join our Matrix room: {str:Url}"), "Url",
				Config()->m_AboutContactsMatrix, nullptr);
		}
	}
}

void CIcGameController::OnPlayerDisconnect(CPlayer *pBasePlayer, EClientDropType Type, const char *pReason)
{
	Server()->RoundStatistics()->ResetPlayer(pBasePlayer->GetCid());

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(pPlayer && (pPlayer != pBasePlayer))
		{
			if(CIcCharacter *pCharacter = CIcCharacter::GetInstance(pPlayer->GetCharacter()))
			{
				pCharacter->RemoveReferencesToCid(pBasePlayer->GetCid());
			}
		}
	}

	CIcPlayer *pPlayer = CIcPlayer::GetInstance(pBasePlayer);
	if(PlayerScore *pScore = GetSurvivalPlayerScore(pPlayer->GetCid()))
	{
		str_copy(pScore->aPlayerName, Server()->ClientName(pPlayer->GetCid()));
		pScore->ClientId = -1;
		pScore->Kills = pPlayer->GetKills();
		pScore->Assists = pPlayer->GetAssists();
	}
	m_SurvivalState.SurvivedPlayers.RemoveOne(pPlayer->GetCid());
	m_SurvivalState.KilledPlayers.RemoveOne(pPlayer->GetCid());

	static const auto aIgnoreReasons = []() {
		EClientDropType aIgnoreReasons[]{
			EClientDropType::Ban,
			EClientDropType::Kick,
			EClientDropType::Redirected,
			EClientDropType::Shutdown,
			EClientDropType::TimeoutProtectionUsed,
		};

		return icArray(aIgnoreReasons);
	}();

	if(!aIgnoreReasons.Contains(Type))
	{
		if(pPlayer && pPlayer->IsInGame() && pPlayer->IsInfected() && m_InfectedStarted && !pPlayer->IsBot())
		{
			int NumHumans;
			int NumInfected;
			GetPlayerCounter(pPlayer->GetCid(), NumHumans, NumInfected);
			const int NumPlayers = NumHumans + NumInfected;
			const int NumFirstInfected = GetMinimumInfectedForPlayers(NumPlayers);

			if(NumInfected < NumFirstInfected)
			{
				Server()->Ban(pPlayer->GetCid(), 60 * Config()->m_InfLeaverBanTime, "Leaver");
			}
		}
	}

	if(pPlayer->IsBot())
	{
		auto pAsBot = static_cast<CBotPlayer *>(pPlayer);
		std::optional<std::size_t> BotIndex = m_Bots.IndexOf(pAsBot);
		if(BotIndex.has_value())
		{
			m_Bots.RemoveAt(BotIndex.value());
		}
		else
		{
			dbg_msg("bot", "Disconnected bot (id %d) is not in the bots list", pAsBot->GetCid());
		}
	}

	IGameController::OnPlayerDisconnect(pBasePlayer, Type, pReason);
}

void CIcGameController::OnReset()
{
	// IGameController::OnReset();

	for(const auto &pPlayer : GameServer()->m_apPlayers)
	{
		if(pPlayer)
		{
			pPlayer->Respawn();
			pPlayer->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() / 2;
		}
	}

	for(bool &Sent : m_ControlPointHintSent)
		Sent = false;

	RunCallback(Lua()->GetLuaState(), "on_world_reset");
	RegisterBotsContext();
}

void CIcGameController::OnShutdown()
{
	RunCallback(Lua()->GetLuaState(), "on_shutdown");
}

void CIcGameController::DoPlayerInfection(CIcPlayer *pPlayer, CIcPlayer *pInfectiousPlayer, EPlayerClass PreviousClass)
{
	if(GetRoundType() == ERoundType::Survival)
	{
		DoTeamChange(pPlayer, TEAM_SPECTATORS, false);
		return;
	}

	const EPlayerClass c = ChooseInfectedClass(pPlayer);
	pPlayer->SetClass(c);

	FallInLoveIfInfectedEarly(pPlayer->GetCharacter());

	if(!pInfectiousPlayer)
	{
		if(pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive())
		{
			// Still send a kill message to notify other players about the infection
			GameServer()->SendKillMessage(pPlayer->GetCid(), pPlayer->GetCid(), WEAPON_WORLD, 0);
			GameServer()->CreateSound(pPlayer->GetCharacter()->m_Pos, SOUND_PLAYER_DIE);
		}

		return;
	}

	const int InfectedByCid = pInfectiousPlayer->GetCid();
	if(!IsInfectedClass(PreviousClass) && (pPlayer != pInfectiousPlayer))
	{
		if(pInfectiousPlayer->IsHuman())
		{
			GameServer()->SendChatTarget_Localization(InfectedByCid, CHATCATEGORY_SCORE,
				_("You have infected {str:VictimName}, shame on you!"),
				"VictimName", Server()->ClientName(pPlayer->GetCid()),
				nullptr);
			GameServer()->SendEmoticon(pInfectiousPlayer->GetCid(), EMOTICON_SORRY);
			if(CIcCharacter *pGuiltyCharacter = pInfectiousPlayer->GetCharacter())
			{
				constexpr float GuiltyPlayerFreeze = 3;
				pGuiltyCharacter->Freeze(GuiltyPlayerFreeze, -1, FREEZEREASON_INFECTION);
				pGuiltyCharacter->SetEmote(EMOTE_PAIN, Server()->Tick() + Server()->TickSpeed() * GuiltyPlayerFreeze);
			}
		}
		else
		{
			GameServer()->SendChatTarget_Localization(pPlayer->GetCid(), CHATCATEGORY_INFECTED,
				_("You have been infected by {str:KillerName}"),
				"KillerName", Server()->ClientName(pInfectiousPlayer->GetCid()),
				nullptr);
			GameServer()->SendChatTarget_Localization(InfectedByCid, CHATCATEGORY_SCORE,
				_("You have infected {str:VictimName}, +3 points"),
				"VictimName", Server()->ClientName(pPlayer->GetCid()),
				nullptr);
			Server()->RoundStatistics()->OnScoreEvent(InfectedByCid, EScoreEvent::INFECTION,
				pInfectiousPlayer->GetClass(), Server()->ClientName(InfectedByCid), Console());
			GameServer()->SendScoreSound(InfectedByCid);
		}
	}

	// Search for hook
	for(TEntityPtr<CIcCharacter> pHook = GameWorld()->FindFirst<CIcCharacter>(); pHook; ++pHook)
	{
		if(
			pHook->GetPlayer() &&
			pHook->GetHookedPlayer() == pPlayer->GetCid() &&
			pHook->GetCid() != InfectedByCid)
		{
			Server()->RoundStatistics()->OnScoreEvent(pHook->GetCid(), EScoreEvent::HELP_HOOK_INFECTION, pHook->GetPlayerClass(), Server()->ClientName(pHook->GetCid()), Console());
			GameServer()->SendScoreSound(pHook->GetCid());
		}
	}
}

void CIcGameController::OnHeroFlagCollected(int ClientId)
{
	const char *pText = _("The Hero found the flag!");
	if(GetRoundType() == ERoundType::Survival)
	{
		pText = _("The Hero got the flag!");
	}
	GameServer()->SendBroadcast_Localization(-1, EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, pText, nullptr);
	GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE);

	const int Tick = Server()->Tick();
	if(Tick < m_HeroGiftTick)
		return;

	m_HeroGiftTick = Tick + GetHeroFlagCooldown() * Server()->TickSpeed();
}

float CIcGameController::GetHeroFlagCooldown() const
{
	if(GetRoundType() == ERoundType::Survival)
	{
		return 30;
	}

	// Set cooldown for next flag depending on how many players are online
	const int PlayerCount = Server()->GetActivePlayerCount();
	if(PlayerCount <= 1)
	{
		// only 1 player on, let him find as many flags as he wants
		return 2.0 / Server()->TickSpeed();
	}

	float t = (8 - PlayerCount) / 8.0f;
	if(t < 0.0f)
		t = 0.0f;

	return 15 + (120 * t);
}

void CIcGameController::ApplyControlPointEffect(CControlPoint *pControlPoint)
{
	pControlPoint->SetNextEffectTime(Config()->m_InfControlPointGlobalInterval);

	RunCallback(Lua()->GetLuaState(), "on_control_point_effect", pControlPoint);
}

void CIcGameController::OnControlPointCaptured(CControlPoint *pControlPoint)
{
	pControlPoint->SetNextEffectTime(Config()->m_InfControlPointGlobalInterval);

	const char *pText = pControlPoint->IsInfected() ? _("Control Point is captured by the infected") : _("Control Point is captured by humans");
	GameServer()->SendBroadcast_Localization(-1, EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, pText, nullptr);
	GameServer()->CreateSoundGlobal(SOUND_CTF_GRAB_PL);

	RunCallback(Lua()->GetLuaState(), "on_control_point_captured", pControlPoint);

	int Index = pControlPoint->IsInfected() ? 0 : 1;
	if(m_ControlPointHintSent[Index])
		return;

	m_ControlPointHintSent[Index] = true;

	if(pControlPoint->IsInfected())
	{
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTED, _("The infected can now spawn at a Control Point"));
	}
	else
	{
		int Seconds = Config()->m_InfControlPointGlobalInterval;
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_HUMANS, _("The control point gives a bonus to all humans every {sec:GlobalEffectInterval}"),
			"GlobalEffectInterval", &Seconds,
			nullptr);
	}
}

bool CIcGameController::OnEntity(const char *pName, vec2 Pivot, vec2 P0, vec2 P1, vec2 P2, vec2 P3, int PosEnv)
{
	const bool Result = IGameController::OnEntity(pName, Pivot, P0, P1, P2, P3, PosEnv);
	const vec2 Pos = (P0 + P1 + P2 + P3) / 4.0f;

	if(str_comp(pName, "icInfected") == 0)
		m_avSpawnPoints[0].push_back(Pos);
	else if(str_comp(pName, "icHuman") == 0)
		m_avSpawnPoints[1].push_back(Pos);

	CIcEntity *pNewEntity = nullptr;
	if(str_comp(pName, "icInfected") == 0)
	{
		const int SpawnX = static_cast<int>(Pos.x) / 32.0f;
		const int SpawnY = static_cast<int>(Pos.y) / 32.0f;

		if(SpawnX >= 0 && SpawnX < m_MapWidth && SpawnY >= 0 && SpawnY < m_MapHeight)
		{
			m_GrowingMap[SpawnY * m_MapWidth + SpawnX] = 6;
		}
	}
	else if(str_comp(pName, "icHeroFlag") == 0)
	{
		// Add hero flag spawns
		m_HeroFlagPositions.add(Pos);
	}
	else if(str_comp(pName, "health") == 0)
	{
		CIcPickup *p = new CIcPickup(GameServer(), EICPickupType::Health, Pos);
		p->SetRespawnInterval(15);
		p->Spawn();
		pNewEntity = p;
	}
	else if(str_comp(pName, "armor") == 0)
	{
		CIcPickup *p = new CIcPickup(GameServer(), EICPickupType::Armor, Pos);
		p->SetRespawnInterval(15);
		p->Spawn();
		pNewEntity = p;
	}

	if(pNewEntity && (PosEnv >= 0))
	{
		pNewEntity->SetAnimatedPos(Pivot, Pos - Pivot, PosEnv);
	}

	return Result;
}

void CIcGameController::HandleCharacterTiles(CIcCharacter *pCharacter) const
{
	ZoneData Data0;
	ZoneData Data1;
	ZoneData Data2;
	ZoneData Data3;

	GetDamageZoneValueAt(vec2(pCharacter->GetPos().x + pCharacter->GetProximityRadius() / 3.f, pCharacter->GetPos().y - pCharacter->GetProximityRadius() / 3.f), &Data0);
	GetDamageZoneValueAt(vec2(pCharacter->GetPos().x + pCharacter->GetProximityRadius() / 3.f, pCharacter->GetPos().y + pCharacter->GetProximityRadius() / 3.f), &Data1);
	GetDamageZoneValueAt(vec2(pCharacter->GetPos().x - pCharacter->GetProximityRadius() / 3.f, pCharacter->GetPos().y - pCharacter->GetProximityRadius() / 3.f), &Data2);
	GetDamageZoneValueAt(vec2(pCharacter->GetPos().x - pCharacter->GetProximityRadius() / 3.f, pCharacter->GetPos().y + pCharacter->GetProximityRadius() / 3.f), &Data3);

	icArray<int, 4> Indices;
	Indices.Add(Data0.Index);
	Indices.Add(Data1.Index);
	Indices.Add(Data2.Index);
	Indices.Add(Data3.Index);

	if(Indices.Contains(ZONE_DAMAGE_DEATH))
	{
		pCharacter->Die(pCharacter->GetCid(), EDamageType::DEATH_TILE);
	}
	else if(pCharacter->GetPlayerClass() != EPlayerClass::Undead && Indices.Contains(ZONE_DAMAGE_DEATH_NOUNDEAD))
	{
		pCharacter->Die(pCharacter->GetCid(), EDamageType::DEATH_TILE);
	}
	else if(pCharacter->IsInfected() && Indices.Contains(ZONE_DAMAGE_DEATH_INFECTED))
	{
		pCharacter->Die(pCharacter->GetCid(), EDamageType::DEATH_TILE);
	}
	else if(pCharacter->IsAlive() && Indices.Contains(ZONE_DAMAGE_INFECTION))
	{
		if((GetRoundType() == ERoundType::Survival) && pCharacter->IsHuman())
		{
			constexpr int Damage = 3;
			pCharacter->OnCharacterInDamageZone(Damage, 0.25f);
		}
		else
		{
			pCharacter->OnCharacterInInfectionZone();
		}
	}
	if(pCharacter->IsAlive() && !Indices.Contains(ZONE_DAMAGE_INFECTION))
	{
		pCharacter->OnCharacterOutOfInfectionZone();
	}

	const int TeamDamageIndex = pCharacter->IsHuman() ? ZONE_DAMAGE_DAMAGE_HUMANS : ZONE_DAMAGE_DAMAGE_INFECTED;
	if(Indices.Contains(ZONE_DAMAGE_DAMAGE) || Indices.Contains(TeamDamageIndex))
	{
		int Damage = 0;
		for(const auto &[Index, ExtraData] : {Data0, Data1, Data2, Data3})
		{
			if((Index == ZONE_DAMAGE_DAMAGE) || Index == TeamDamageIndex)
			{
				Damage = maximum(Damage, ExtraData);
			}
		}

		if(Damage <= 0)
		{
			Damage = Config()->m_InfTileDamage;
		}
		if(Damage > 0)
		{
			pCharacter->OnCharacterInDamageZone(Damage);
		}
	}
}

void CIcGameController::HandleLastHookers()
{
	const int CurrentTick = Server()->Tick();
	ClientsArray CharacterHookedBy[MAX_CLIENTS]{};

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		const CIcCharacter *pCharacter = GetCharacter(i);
		if(!pCharacter)
		{
			continue;
		}

		const int HookedPlayer = pCharacter->GetHookedPlayer();
		if(HookedPlayer < 0)
		{
			continue;
		}

		CharacterHookedBy[HookedPlayer].Add(i);
	}

	for(int TargetCid = 0; TargetCid < MAX_CLIENTS; ++TargetCid)
	{
		ClientsArray &HookedBy = CharacterHookedBy[TargetCid];
		if(HookedBy.IsEmpty())
		{
			continue;
		}
		CIcCharacter *pHookedCharacter = GetCharacter(TargetCid);
		if(!pHookedCharacter)
		{
			continue;
		}

		if(HookedBy.Size() > 1)
		{
			SortCharactersByDistance(HookedBy, &HookedBy, pHookedCharacter->GetPos());
		}
		pHookedCharacter->UpdateLastHookers(HookedBy, CurrentTick);
	}
}

float CIcGameController::GetSecondsElapsed() const
{
	return (Server()->Tick() - m_RoundStartTick) / (static_cast<float>(Server()->TickSpeed()));
}

float CIcGameController::GetSecondsRemaining() const
{
	return GetTimeLimitMinutes() * 60 - GetSecondsElapsed();
}

bool CIcGameController::CanSeeDetails(int Who, int Whom) const
{
	if(Who == SERVER_DEMO_CLIENT)
		return true;

	const CIcPlayer *pWhom = GetPlayer(Whom);
	if(!pWhom || pWhom->GetTeam() == TEAM_SPECTATORS)
		return false;

	const CIcPlayer *pWho = GetPlayer(Who);
	if(!pWho)
		return false;

	if(pWho->GetTeam() == TEAM_SPECTATORS)
		return Config()->m_SvStrictSpectateMode == 0;

	return pWho->IsHuman() == pWhom->IsHuman();
}

CClientMask CIcGameController::GetBlindCharactersMask(int ExcludeCid) const
{
	CClientMask Mask;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == ExcludeCid)
			continue;

		const CIcCharacter *pTarget = GetCharacter(i);
		if(!pTarget)
			continue;
		if(!pTarget->IsBlind())
			continue;

		Mask.set(i);
	}

	return Mask;
}

CClientMask CIcGameController::GetMaskForPlayerWorldEvent(int Asker, int ExceptId)
{
	dbg_assert(Asker >= 0, "Incorrect client id");

	const CIcCharacter *pCharacter = GetCharacter(Asker);
	if(!pCharacter || !pCharacter->IsInvisible())
	{
		if(ExceptId == -1)
			return CClientMask().set();
		return CClientMask().set().reset(ExceptId);
	}

	return m_Teams.TeamMask(GetPlayerTeam(Asker), ExceptId, Asker);
}

bool CIcGameController::HumanWallAllowedInPos(const vec2 &Pos) const
{
	constexpr float Radius = 32.0f;

	if(GetDamageZoneValueAt(Pos) == ZONE_DAMAGE_INFECTION)
		return false;

	{ // Check for spawns
		constexpr int Type = 0; // InfectedSpawn

		// get spawn point
		for(std::size_t i = 0; i < m_avSpawnPoints[Type].size(); i++)
		{
			if(distance(Pos, m_avSpawnPoints[Type][i]) <= Radius)
			{
				return false;
			}
		}
	}

	return true;
}

int CIcGameController::GetZoneValueAt(int ZoneHandle, const vec2 &Pos, ZoneData *pData) const
{
	return GameServer()->Collision()->GetZoneValueAt(ZoneHandle, Pos, pData);
}

int CIcGameController::GetDamageZoneValueAt(const vec2 &Pos, ZoneData *pData) const
{
	int DamageIndex = GetZoneValueAt(m_ZoneHandle_icDamage, Pos, pData);
	if(!DamageIndex && m_VanillaMapLoaded)
	{
		const int GameTile = GameServer()->Collision()->GetCollisionAt(Pos.x, Pos.y);
		if(GameTile == TILE_DEATH)
		{
			DamageIndex = ZONE_DAMAGE_DEATH;

			if(pData)
			{
				pData->Index = DamageIndex;
				pData->ExtraData = 0;
			}
		}
	}

	return DamageIndex;
}

EZoneTele CIcGameController::GetTeleportZoneValueAt(const vec2 &Pos, ZoneData *pData) const
{
	return static_cast<EZoneTele>(GetZoneValueAt(m_ZoneHandle_icTeleport, Pos, pData));
}

int CIcGameController::GetBonusZoneValueAt(const vec2 &Pos, ZoneData *pData) const
{
	return GetZoneValueAt(m_ZoneHandle_icBonus, Pos, pData);
}

void CIcGameController::ExecuteFileEx(const char *pBaseName)
{
	char aBuf[256];
	const char *pFileName = pBaseName;
	{
		constexpr char aPlayersNumberVar[] = "${players_number}";
		if(const char *pPlayersNumber = str_find(pFileName, aPlayersNumberVar))
		{
			constexpr int ClientException = -1;
			int NumHumans;
			int NumInfected;
			GetPlayerCounter(ClientException, NumHumans, NumInfected);
			const int Count = NumHumans + NumInfected;

			str_copy(aBuf, pFileName);
			const std::ptrdiff_t Offset = pPlayersNumber - pFileName;
			str_format(aBuf + Offset, std::size(aBuf) - Offset, "%d%s", Count, pPlayersNumber + std::size(aPlayersNumberVar) - 1);
			pFileName = &aBuf[0];
		}
	}

	{
		constexpr char aMapNameVar[] = "${map_name}";
		if(const char *pVarIndex = str_find(pFileName, aMapNameVar))
		{
			if(pFileName != &aBuf[0])
			{
				str_copy(aBuf, pFileName);
			}

			const char *pMapName = Server()->GetMapName();
			const std::ptrdiff_t Offset = pVarIndex - pFileName;
			str_format(aBuf + Offset, std::size(aBuf) - Offset, "%s%s", pMapName, pVarIndex + std::size(aMapNameVar) - 1);
			pFileName = &aBuf[0];
		}
	}

	Console()->ExecuteFile(pFileName, -1, true);
}

void CIcGameController::CreateExplosion(const vec2 &Pos, int Owner, EDamageType DamageType, float DamageFactor)
{
	constexpr int Weapon = WEAPON_WORLD;
	GameServer()->CreateExplosion(Pos, Owner, Weapon);

	if(DamageFactor != 0)
	{
		// deal damage
		bool AffectOwner = true;
		if(DamageType == EDamageType::WHITE_HOLE)
			AffectOwner = false;

		CIcCharacter *apEnts[MAX_CLIENTS];
		constexpr float Radius = 135.0f;
		constexpr float InnerRadius = 48.0f;
		const int Num = GameWorld()->FindEntities(Pos, Radius, reinterpret_cast<CEntity **>(apEnts), MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; i++)
		{
			if(apEnts[i]->GetCid() == Owner)
			{
				if(!AffectOwner)
					continue;
			}
			if(!Config()->m_InfShockwaveAffectHumans)
			{
				if(apEnts[i]->GetCid() == Owner)
				{
					// owner selfharm
				}
				else if(apEnts[i]->IsHuman())
				{
					continue; // humans are not affected by force
				}
			}
			vec2 Diff = apEnts[i]->m_Pos - Pos;
			vec2 ForceDir(0, 1);
			float l = length(Diff);
			if(l)
				ForceDir = normalize(Diff);
			l = 1 - clamp((l - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
			if(const float Dmg = 6 * l * DamageFactor; static_cast<int>(Dmg))
			{
				apEnts[i]->TakeDamage(ForceDir * Dmg * 2, static_cast<int>(Dmg), Owner, DamageType);
			}
		}
	}
}

// Thanks to Stitch for the idea
void CIcGameController::CreateExplosionDisk(vec2 Pos, float InnerRadius, float DamageRadius, int Damage, float Force, int Owner, EDamageType DamageType)
{
	CreateExplosionDiskGfx(Pos, InnerRadius, DamageRadius, Owner);

	if(Damage > 0)
	{
		// deal damage
		CIcCharacter *apEnts[MAX_CLIENTS];
		const int Num = GameWorld()->FindEntities(Pos, DamageRadius, reinterpret_cast<CEntity **>(apEnts), MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; i++)
		{
			vec2 Diff = apEnts[i]->m_Pos - Pos;
			if(Diff.x == 0.0f && Diff.y == 0.0f)
				Diff.y = -0.5f;
			vec2 ForceDir(0, 1);
			float len = length(Diff);
			len = 1 - clamp((len - InnerRadius) / (DamageRadius - InnerRadius), 0.0f, 1.0f);

			if(len)
				ForceDir = normalize(Diff);

			const float DamageToDeal = 1 + ((Damage - 1) * len);
			apEnts[i]->TakeDamage(ForceDir * Force * len, DamageToDeal, Owner, DamageType);
		}
	}
}

void CIcGameController::CreateExplosionDiskGfx(vec2 Pos, float InnerRadius, float DamageRadius, int Owner) const
{
	constexpr int Weapon = WEAPON_WORLD;
	GameServer()->CreateExplosion(Pos, Owner, Weapon);

	const float CircleLength = 2.0 * pi * maximum(DamageRadius - 135.0f, 0.0f);
	const int NumSuroundingExplosions = CircleLength / 32.0f;
	const float AngleStart = random_float() * pi * 2.0f;
	const float AngleStep = pi * 2.0f / static_cast<float>(NumSuroundingExplosions);
	const float Radius = (DamageRadius - 135.0f);
	for(int i = 0; i < NumSuroundingExplosions; i++)
	{
		vec2 Offset = vec2(Radius * cos(AngleStart + i * AngleStep), Radius * sin(AngleStart + i * AngleStep));
		GameServer()->CreateExplosion(Pos + Offset, Owner, Weapon);
	}
}

void CIcGameController::CreateDeathEffectDiskGfx(vec2 Pos, float InnerRadius, float DamageRadius, int Owner)
{
	GameServer()->CreateDeath(Pos, Owner);

	float CircleLength = 2.0 * pi * maximum(DamageRadius - 135.0f, 0.0f);
	int NumSuroundingExplosions = CircleLength / 32.0f;
	float AngleStart = random_float() * pi * 2.0f;
	float AngleStep = pi * 2.0f / static_cast<float>(NumSuroundingExplosions);
	const float Radius = (DamageRadius - 135.0f);
	for(int i = 0; i < NumSuroundingExplosions; i++)
	{
		vec2 Offset = vec2(Radius * cos(AngleStart + i * AngleStep), Radius * sin(AngleStart + i * AngleStep));
		GameServer()->CreateDeath(Pos + Offset, Owner);
	}
}

void CIcGameController::SendHammerDot(const vec2 &Pos, int SnapId)
{
	CNetObj_Projectile *pObj = Server()->SnapNewItem<CNetObj_Projectile>(SnapId);
	if(!pObj)
		return;
	;

	pObj->m_X = Pos.x;
	pObj->m_Y = Pos.y;
	pObj->m_VelX = 0;
	pObj->m_VelY = 0;
	pObj->m_Type = WEAPON_HAMMER;
	pObj->m_StartTick = Server()->Tick();
}

void CIcGameController::SendServerParams(int ClientId) const
{
	CNetMsg_InfClass_ServerParams Msg{};
	Msg.m_Version = 1;
	if(WhiteHoleEnabled())
	{
		Msg.m_WhiteHoleMinKills = Config()->m_InfWhiteHoleMinimalKills;
	}
	Msg.m_SoldierBombs = Config()->m_InfSoldierBombs;

	if(ClientId == -1)
	{
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, SERVER_DEMO_CLIENT);

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GetPlayer(i))
			{
				const int InfclassVersion = Server()->GetClientInfclassVersion(i);
				if(InfclassVersion >= VERSION_INFC_180)
				{
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
				}
			}
		}
	}
	else
	{
		const int InfclassVersion = Server()->GetClientInfclassVersion(ClientId);
		if(InfclassVersion >= VERSION_INFC_180)
		{
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}
	}
}

void CIcGameController::ResetFinalExplosion()
{
	m_FinalExplosionState = EFinalExplosionState::NotStarted;

	for(int j = 0; j < m_MapHeight; j++)
	{
		for(int i = 0; i < m_MapWidth; i++)
		{
			if(!(m_GrowingMap[j * m_MapWidth + i] & 4))
			{
				m_GrowingMap[j * m_MapWidth + i] = 1;
			}
		}
	}
}

void CIcGameController::SaveRoundRules()
{
	SendServerParams(-1);
}

void CIcGameController::EndSurvivalGame()
{
	// Sync the scores
	for(PlayerScore &Score : m_SurvivalState.Scores)
	{
		if(Score.ClientId < 0)
			continue;

		const CIcPlayer *pPlayer = GetPlayer(Score.ClientId);
		Score.Kills = pPlayer->GetKills();
		Score.Assists = pPlayer->GetAssists();
		str_copy(Score.aPlayerName, Server()->ClientName(pPlayer->GetCid()));
	}

	const auto Sorter = [](const PlayerScore &s1, const PlayerScore &s2) -> bool {
		return s1.GetScore() > s2.GetScore();
	};

	std::ranges::stable_sort(m_SurvivalState.Scores, Sorter);

	GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "Score", nullptr);
	for(const PlayerScore &Score : m_SurvivalState.Scores)
	{
		int ScoreValue = Score.GetScore();
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "- {str:PlayerName}: {int:Score}",
			"PlayerName", Score.aPlayerName,
			"Score", &ScoreValue,
			nullptr);
	}
	int Score = m_SurvivalState.Kills;
	GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "Total team kills: {int:Kills}",
		"Kills", &Score,
		nullptr);

	if(m_BestSurvivalScore)
	{
		if(Score > m_BestSurvivalScore)
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "This is the new best score on the server!",
				nullptr);
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "The previous best score is {int:Score}",
				"Score", &m_BestSurvivalScore,
				nullptr);
		}
		else if(Score == m_BestSurvivalScore)
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "This is the same score as the best one!",
				nullptr);
		}
		else
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "The best score is: {int:Score}",
				"Score", &m_BestSurvivalScore,
				nullptr);
		}
	}

	if(Score > m_BestSurvivalScore)
	{
		m_BestSurvivalScore = Score;
	}

	m_WaveStartTick = 0;
	m_SurvivalState.Wave = 0;

	if(Config()->m_InfSurvivalMode)
	{
		if(Config()->m_InfSurvivalAutostart)
		{
			QueueRoundType(ERoundType::Survival);

			if(m_SurvivalConfiguration.SurvivalWaves.Size() == m_SurvivalState.Wave + 1)
			{
				// Humans won the game.
			}
			else
			{
				m_TriggerSurvivalAutostart = true;
			}
		}
		m_RoundCount = 0;
	}

	m_SurvivalState.Kills = 0;
	m_SurvivalState.Scores.Clear();
	m_SurvivalState.SurvivedPlayers.Clear();
	// Process the killed players OnGameRestart() to show proper score board
	// m_SurvivalState.KilledPlayers.Clear();
}

int CIcGameController::GetRoundTick() const
{
	return Server()->Tick() - m_RoundStartTick;
}

int CIcGameController::GetInfectionTick() const
{
	return Server()->Tick() - GetInfectionStartTick();
}

int CIcGameController::GetInfectionStartTick() const
{
	const int StartTick = GetRoundType() == ERoundType::Survival ? m_WaveStartTick : m_RoundStartTick;
	const int InfectionTick = StartTick + Server()->TickSpeed() * GetInfectionDelay();
	return InfectionTick;
}

bool CIcGameController::IsDefenderClass(EPlayerClass PlayerClass)
{
	switch(PlayerClass)
	{
	case EPlayerClass::Engineer:
	case EPlayerClass::Soldier:
	case EPlayerClass::Scientist:
	case EPlayerClass::Biologist:
	case EPlayerClass::Looper:
		return true;
	default:
		return false;
	}
}

bool CIcGameController::IsSupportClass(EPlayerClass PlayerClass)
{
	switch(PlayerClass)
	{
		case EPlayerClass::Ninja:
		case EPlayerClass::Mercenary:
		case EPlayerClass::Sniper:
		case EPlayerClass::Artillery:
			return true;
		default:
			return false;
	}
}

EPlayerClass CIcGameController::GetClassByName(const char *pClassName, bool *pOk)
{
	struct ExtraName
	{
		ExtraName(const char *pN, EPlayerClass Class) :
			pName(pN), PlayerClass(Class)
		{
		}

		const char *pName = nullptr;
		EPlayerClass PlayerClass = EPlayerClass::Invalid;
	};

	static const ExtraName extraNames[] = {
		ExtraName("bio", EPlayerClass::Biologist),
		ExtraName("bios", EPlayerClass::Biologist),
		ExtraName("engi", EPlayerClass::Engineer),
		ExtraName("merc", EPlayerClass::Mercenary),
		ExtraName("mercs", EPlayerClass::Mercenary),
		ExtraName("sci", EPlayerClass::Scientist),
		ExtraName("random", EPlayerClass::Random),
	};
	if(pOk)
	{
		*pOk = true;
	}
	for(const ExtraName &Extra : extraNames)
	{
		if(str_comp(pClassName, Extra.pName) == 0)
		{
			return Extra.PlayerClass;
		}
	}

	for(const EPlayerClass PlayerClass : AllPlayerClasses)
	{
		const char *pSingularName = CIcGameController::GetClassName(PlayerClass);
		const char *pPluralName = CIcGameController::GetClassPluralName(PlayerClass);
		if((str_comp(pClassName, pSingularName) == 0) || (str_comp(pClassName, pPluralName) == 0))
		{
			return static_cast<EPlayerClass>(PlayerClass);
		}
	}

	if(pOk)
	{
		*pOk = false;
	}
	return EPlayerClass::Invalid;
}

const char *CIcGameController::GetClassName(EPlayerClass PlayerClass)
{
	return toString(PlayerClass);
}

const char *CIcGameController::GetClassPluralName(EPlayerClass PlayerClass)
{
	switch(PlayerClass)
	{
	case EPlayerClass::Mercenary:
		return "mercenaries";
	case EPlayerClass::Medic:
		return "medics";
	case EPlayerClass::Hero:
		return "heroes";
	case EPlayerClass::Engineer:
		return "engineers";
	case EPlayerClass::Soldier:
		return "soldiers";
	case EPlayerClass::Ninja:
		return "ninjas";
	case EPlayerClass::Sniper:
		return "snipers";
	case EPlayerClass::Scientist:
		return "scientists";
	case EPlayerClass::Biologist:
		return "biologists";
	case EPlayerClass::Looper:
		return "loopers";
	case EPlayerClass::Artillery:
		return "artillery";

	case EPlayerClass::Smoker:
		return "smokers";
	case EPlayerClass::Boomer:
		return "boomers";
	case EPlayerClass::Hunter:
		return "hunters";
	case EPlayerClass::Bat:
		return "bats";
	case EPlayerClass::Ghost:
		return "ghosts";
	case EPlayerClass::Spider:
		return "spiders";
	case EPlayerClass::Ghoul:
		return "ghouls";
	case EPlayerClass::Slug:
		return "slugs";
	case EPlayerClass::Voodoo:
		return "voodoos";
	case EPlayerClass::Witch:
		return "witches";
	case EPlayerClass::Undead:
		return "undeads";
	case EPlayerClass::Tank:
		return "tanks";
	case EPlayerClass::Spitter:
		return "spitters";

	case EPlayerClass::Invalid:
	case EPlayerClass::None:
	case EPlayerClass::Count:
	default:
		return "unknown";
	}
}

const char *CIcGameController::GetClassDisplayName(EPlayerClass PlayerClass, const char *pDefaultText)
{
	switch(PlayerClass)
	{
	case EPlayerClass::Mercenary:
		return _("Mercenary");
	case EPlayerClass::Medic:
		return _("Medic");
	case EPlayerClass::Hero:
		return _("Hero");
	case EPlayerClass::Engineer:
		return _("Engineer");
	case EPlayerClass::Soldier:
		return _("Soldier");
	case EPlayerClass::Ninja:
		return _("Ninja");
	case EPlayerClass::Sniper:
		return _("Sniper");
	case EPlayerClass::Scientist:
		return _("Scientist");
	case EPlayerClass::Biologist:
		return _("Biologist");
	case EPlayerClass::Looper:
		return _("Looper");
	case EPlayerClass::Artillery:
		return _("Artillery");

	case EPlayerClass::Smoker:
		return _("Smoker");
	case EPlayerClass::Boomer:
		return _("Boomer");
	case EPlayerClass::Hunter:
		return _("Hunter");
	case EPlayerClass::Bat:
		return _("Bat");
	case EPlayerClass::Ghost:
		return _("Ghost");
	case EPlayerClass::Spider:
		return _("Spider");
	case EPlayerClass::Ghoul:
		return _("Ghoul");
	case EPlayerClass::Slug:
		return _("Slug");
	case EPlayerClass::Voodoo:
		return _("Voodoo");
	case EPlayerClass::Witch:
		return _("Witch");
	case EPlayerClass::Undead:
		return _("Undead");
	case EPlayerClass::Tank:
		return _("Tank");
	case EPlayerClass::Spitter:
		return _("Spitter");

	case EPlayerClass::Invalid:
	case EPlayerClass::None:
	case EPlayerClass::Count:
	default:
		return pDefaultText ? pDefaultText : "Unknown";
	}
}

const char *CIcGameController::GetClassDisplayNameForKilledBy(EPlayerClass PlayerClass, ETextArticle Article)
{
	switch(PlayerClass)
	{
		// Only the infected classes are used for now; do not add others to keep the translations smaller
	case EPlayerClass::Smoker:
		return Article == ETextArticle::Indefinite ? _("a Smoker") : _("the Smoker");
	case EPlayerClass::Boomer:
		return Article == ETextArticle::Indefinite ? _("a Boomer") : _("the Boomer");
	case EPlayerClass::Hunter:
		return Article == ETextArticle::Indefinite ? _("a Hunter") : _("the Hunter");
	case EPlayerClass::Bat:
		return Article == ETextArticle::Indefinite ? _("a Bat") : _("the Bat");
	case EPlayerClass::Ghost:
		return Article == ETextArticle::Indefinite ? _("a Ghost") : _("the Ghost");
	case EPlayerClass::Spider:
		return Article == ETextArticle::Indefinite ? _("a Spider") : _("the Spider");
	case EPlayerClass::Ghoul:
		return Article == ETextArticle::Indefinite ? _("a Ghoul") : _("the Ghoul");
	case EPlayerClass::Slug:
		return Article == ETextArticle::Indefinite ? _("a Slug") : _("the Slug");
	case EPlayerClass::Voodoo:
		return Article == ETextArticle::Indefinite ? _("a Voodoo") : _("the Voodoo");
	case EPlayerClass::Witch:
		return Article == ETextArticle::Indefinite ? _("a Witch") : _("the Witch");
	case EPlayerClass::Undead:
		return Article == ETextArticle::Indefinite ? _("an Undead") : _("the Undead");
	case EPlayerClass::Tank:
		return Article == ETextArticle::Indefinite ? _("a Tank") : _("the Tank");
	case EPlayerClass::Spitter:
		return Article == ETextArticle::Indefinite ? _("a Spitter") : _("the Spitter");

	default:
		return "Unknown";
	}
}

const char *CIcGameController::GetClanForClass(EPlayerClass PlayerClass, const char *pDefaultText)
{
	switch(PlayerClass)
	{
	default:
		return GetClassDisplayName(PlayerClass, pDefaultText);
	}
}

const char *CIcGameController::GetClassPluralDisplayName(EPlayerClass PlayerClass)
{
	switch(PlayerClass)
	{
	case EPlayerClass::Mercenary:
		return _("Mercenaries");
	case EPlayerClass::Medic:
		return _("Medics");
	case EPlayerClass::Hero:
		return _("Heroes");
	case EPlayerClass::Engineer:
		return _("Engineers");
	case EPlayerClass::Soldier:
		return _("Soldiers");
	case EPlayerClass::Ninja:
		return _("Ninjas");
	case EPlayerClass::Sniper:
		return _("Snipers");
	case EPlayerClass::Scientist:
		return _("Scientists");
	case EPlayerClass::Biologist:
		return _("Biologists");
	case EPlayerClass::Looper:
		return _("Loopers");
	case EPlayerClass::Artillery:
		return _("Artilleries");

	case EPlayerClass::Smoker:
		return _("Smokers");
	case EPlayerClass::Boomer:
		return _("Boomers");
	case EPlayerClass::Hunter:
		return _("Hunters");
	case EPlayerClass::Bat:
		return _("Bats");
	case EPlayerClass::Ghost:
		return _("Ghosts");
	case EPlayerClass::Spider:
		return _("Spiders");
	case EPlayerClass::Ghoul:
		return _("Ghouls");
	case EPlayerClass::Slug:
		return _("Slugs");
	case EPlayerClass::Voodoo:
		return _("Voodoos");
	case EPlayerClass::Witch:
		return _("Witches");
	case EPlayerClass::Undead:
		return _("Undeads");

	case EPlayerClass::Tank:
		return _("Tanks");
	case EPlayerClass::Spitter:
		return _("Spitters");

	case EPlayerClass::Invalid:
	case EPlayerClass::None:
	case EPlayerClass::Count:
		break;
	}
	return _("Unknown");
}

EPlayerClass CIcGameController::MenuClassToPlayerClass(int MenuClass)
{
	switch(MenuClass)
	{
	case CMapConverter::MENUCLASS_MEDIC:
		return EPlayerClass::Medic;
	case CMapConverter::MENUCLASS_HERO:
		return EPlayerClass::Hero;
	case CMapConverter::MENUCLASS_NINJA:
		return EPlayerClass::Ninja;
	case CMapConverter::MENUCLASS_MERCENARY:
		return EPlayerClass::Mercenary;
	case CMapConverter::MENUCLASS_SNIPER:
		return EPlayerClass::Sniper;
	case CMapConverter::MENUCLASS_RANDOM:
		return EPlayerClass::None;
	case CMapConverter::MENUCLASS_ENGINEER:
		return EPlayerClass::Engineer;
	case CMapConverter::MENUCLASS_SOLDIER:
		return EPlayerClass::Soldier;
	case CMapConverter::MENUCLASS_SCIENTIST:
		return EPlayerClass::Scientist;
	case CMapConverter::MENUCLASS_BIOLOGIST:
		return EPlayerClass::Biologist;
	case CMapConverter::MENUCLASS_LOOPER:
		return EPlayerClass::Looper;
	case CMapConverter::MENUCLASS_ARTILLERY:
		return EPlayerClass::Artillery;
	default:
		return EPlayerClass::Invalid;
	}
}

int CIcGameController::GetPlayerTeam(int ClientId) const
{
	return m_Teams.m_Core.Team(ClientId);
}

void CIcGameController::SetPlayerInfected(int ClientId, bool Infected)
{
	return m_Teams.m_Core.SetInfected(ClientId, Infected);
}

void CIcGameController::RegisterChatCommands(IConsole *pConsole)
{
	Console()->Register("inf_set_weapon_fire_delay", "s[weapon] i[msec]", CFGFLAG_SERVER, ConSetWeaponFireDelay, this,
		"Set InfClass weapon fire delay");
	Console()->Register("inf_set_weapon_ammo_regen", "s[weapon] i[msec]", CFGFLAG_SERVER, ConSetWeaponAmmoRegen, this,
		"Set InfClass weapon ammo regen interval");
	Console()->Register("inf_set_weapon_max_ammo", "s[weapon] i[ammo]", CFGFLAG_SERVER, ConSetWeaponMaxAmmo, this,
		"Set InfClass weapon max ammo");
	Console()->Register("inf_weapon_force", "s[weapon] ?f[force]", CFGFLAG_SERVER, ConWeaponForce, this, "Set InfClass weapon (base) force");
	Console()->Register("inf_list_weapons", "", CFGFLAG_SERVER, ConListWeapons, this, "List InfClass weapon names");

	pConsole->Register("active_players_number", "", CFGFLAG_SERVER, ConGetActivePlayersNumber, this, "Get the number of active players (excluding spectators and bots)");

	pConsole->Register("restore_client_name", "i[ClientId]", CFGFLAG_SERVER, ConRestoreClientName, this, "Set the name of a player");
	pConsole->Register("set_client_name", "i[ClientId] r[name]", CFGFLAG_SERVER, ConSetClientName, this, "Set the name of a player (and also lock it)");
	pConsole->Register("lock_client_name", "i[ClientId] i[lock]", CFGFLAG_SERVER, ConLockClientName, this, "Set the name of a player");

	pConsole->Register("set_health_armor", "i[ClientId] i[health] i[armor]", CFGFLAG_SERVER, ConSetHealthArmor, this, "Set the player health/armor");
	pConsole->Register("set_invincible", "i[ClientId] i[level]", CFGFLAG_SERVER, ConSetInvincible, this, "Set the player invincibility level (1 inv to damage, 2 inv to inf, 3 inv to death tiles");
	pConsole->Register("set_hook_protection", "i[ClientId] i[protection]", CFGFLAG_SERVER, ConSetHookProtection, this, "Enable the player hook protection (0 disabled, 1 enabled)");
	pConsole->Register("give_upgrade", "i[ClientId]", CFGFLAG_SERVER, ConGiveUpgrade, this, "Give an upgrade to the player");
	pConsole->Register("inf_set_drop", "i[ClientId] ?i[level]", CFGFLAG_SERVER, ConSetDrop, this, "Make the character drop an upgrade on killed or died");

#if CONF_LUA
	pConsole->Register("exec_lua", "r[filename]", CFGFLAG_SERVER, ConExecLua, this, "Execute LUA file");
	pConsole->Register("lua", "r[code]", CFGFLAG_SERVER, ConLua, this, "Execute LUA code");
#endif

	pConsole->Register("inf_revive_near", "i[RevivedClientId] i[TargetClientId]", CFGFLAG_SERVER, ConReviveNear, this, "Revive a player near another player");
	pConsole->Register("inf_set_class", "i[ClientId] s[classname]", CFGFLAG_SERVER, ConSetClass, this, "Set the class of a player");
	pConsole->Register("queue_round", "s[type]", CFGFLAG_SERVER, ConQueueSpecialRound, this, "Queue a special round");
	pConsole->Register("start_round", "?s[type]", CFGFLAG_SERVER, ConStartRound, this, "Start a special round");

	pConsole->Register("start_fun_round", "", CFGFLAG_SERVER, ConStartFunRound, this, "Start a random fun round");
	pConsole->Register("start_special_fun_round", "s[classname] s[classname] ?s[more classes]", CFGFLAG_SERVER, ConStartSpecialFunRound, this, "Start a fun round");
	pConsole->Register("clear_fun_rounds", "", CFGFLAG_SERVER, ConClearFunRounds, this, "Clears added fun rounds");
	pConsole->Register("add_fun_round", "s[classname] s[classname] ?s[more classes]", CFGFLAG_SERVER, ConAddFunRound, this, "Add a fun round to be played when starting a fun round.");

	pConsole->Register("start_survival", "?is", CFGFLAG_SERVER, ConStartSurvival, this, "Set the class of a player");
	pConsole->Register("start_fast_round", "", CFGFLAG_SERVER, ConStartFastRound, this, "Start a faster gameplay round");
	pConsole->Register("queue_fast_round", "", CFGFLAG_SERVER, ConQueueFastRound, this, "Queue a faster gameplay round");
	pConsole->Register("queue_fun_round", "", CFGFLAG_SERVER, ConQueueFunRound, this, "Queue a fun gameplay round");
	pConsole->Register("print_players_picking", "", CFGFLAG_SERVER, ConPrintPlayerPickingTimestamp, this, "");
	pConsole->Register("map_rotation_status", "", CFGFLAG_SERVER, ConMapRotationStatus, this, "Print the status of map rotation");

	pConsole->Register("save_maps_data", "s[filename]", CFGFLAG_SERVER, ConSaveMapsData, this, "Save the map rotation data to a file");
	pConsole->Register("print_maps_data", "", CFGFLAG_SERVER, ConPrintMapsData, this, "Print the data of map rotation");
	pConsole->Register("reset_map_data", "s[mapname]", CFGFLAG_SERVER, ConResetMapData, this, "Reset map rotation data");
	pConsole->Register("add_map_data", "s[mapname] i[timestamp]", CFGFLAG_SERVER, ConAddMapData, this, "Add map rotation data");
	pConsole->Register("set_map_min_max_players", "s[mapname] i[min] ?i[max]", CFGFLAG_SERVER, ConSetMapMinMaxPlayers, this, "Set min/max players for a map");

	Console()->Register("rflag", "", CFGFLAG_CHAT, ConRefreshHeroFlag, this, "Refresh the position of hero flag");

	Console()->Register("revive", "s[player name]", CFGFLAG_CHAT, ChatHeroRevive, this, "Revive a dead teammate near you (a hero) in survival mode");
	Console()->Register("respawn", "s[player name]", CFGFLAG_CHAT, ConChatSurvivalRespawn, this, "Respawn near an alive player in survival mode");
	Console()->Register("prefer_class", "s[classname]", CFGFLAG_CHAT, ConPreferClass, this, "Set the preferred human class to <classname>");
	Console()->Register("alwaysrandom", "i['0'|'1']", CFGFLAG_CHAT, ConAlwaysRandom, this, "Set the preferred class to Random");
	Console()->Register("antiping", "i['0'|'1']", CFGFLAG_CHAT, ConAntiPing, this, "Try to improve your ping (reduce the traffic)");
	Console()->Register("add_control_point", "f[x] f[y]", CFGFLAG_SERVER, ConAddControlPoint, this, "Add a control point at given tile");

	pConsole->Register("set_class", "s[classname]", CFGFLAG_CHAT, ConUserSetClass, this, "Set the class of a player");
	pConsole->Register("save_position", "", CFGFLAG_CHAT, ConSavePosition, this, "Save the current character position");
	pConsole->Register("load_position", "", CFGFLAG_CHAT, ConLoadPosition, this, "Load (restore) the current character position");
	pConsole->Register("sp", "", CFGFLAG_CHAT, ConSavePosition, this, "Save the current character position");
	pConsole->Register("lp", "", CFGFLAG_CHAT, ConLoadPosition, this, "Load (restore) the current character position");

	pConsole->Register("witch", "", CFGFLAG_CHAT, ChatWitch, this, "Call Witch");
	pConsole->Register("santa", "", CFGFLAG_CHAT, ChatWitch, this, "Call the Santa");

	pConsole->Register("say_bot", "i[clientid] r[message]", CFGFLAG_SERVER, ConSayBot, this, "Send a message on a bot behalf");
	pConsole->Register("add_bot", "i[number] s[class] ?s[spawn=<>sec] ?s[lives=<>] ?s[hp=<>] ?s[respawn=<>sec] ?s[drop_level=<>] ?s[tweaks=<>]", CFGFLAG_SERVER, ConAddBot, this, "Add a bot");
	pConsole->Register("remove_bot", "i[CID or -1]", CFGFLAG_SERVER, ConRemoveBot, this, "Remove a bot");
	pConsole->Register("dump_bot", "i", CFGFLAG_SERVER, ConDumpBot, this, "Dump bot state");
	pConsole->Register("ai", "s[enable|disable|debug|danger] ?i[clientid]", CFGFLAG_SERVER, ConCheckAI, this, "Debug bot AI from the caller PoV");
	pConsole->Register("ai_objection", "s[command] ?s[argument]", CFGFLAG_SERVER, ConAiObjection, this, "Setup AI objections");
	pConsole->Register("ai_trace_path", "i[ClientId] f[x] f[y]", CFGFLAG_SERVER, ConAiTracePath, this, "Debug bot AI from the caller PoV");

	pConsole->Register("survival_clear_conf", "", CFGFLAG_SERVER, ConSurvivalClearConf, this, "");
	pConsole->Register("survival_conf", "s[option] ?i[value]", CFGFLAG_SERVER, ConSurvivalConf, this, "Adjust survival configuration");
	pConsole->Register("survival_add_wave", "i[wave] ?s[name]", CFGFLAG_SERVER, ConSurvivalAddWave, this, "");
	pConsole->Register("survival_conf_wave", "i[wave] s[action] s[class] ?s[spawn=<>sec] ?s[lives=<>] ?s[hp=<>] ?s[respawn=<>sec] ?s[drop_level=<>] ?s[tweaks=<>]", CFGFLAG_SERVER, ConSurvivalConfWave, this, "");
	pConsole->Register("start_survival_scenario", "r[file]", CFGFLAG_SERVER | CFGFLAG_CLIENT, ConStartSurvivalScenario, this, "Start survival with scenario loaded from the specified file");
}

EInfclassWeapon CIcGameController::GetWeaponIdFromConArgument(IConsole::IResult *pResult, unsigned int Index)
{
	auto WeaponId = fromString<EInfclassWeapon>(pResult->GetString(Index));
	if(WeaponId == EInfclassWeapon::Invalid)
	{
		// Fallback to old index-based setup
		const int WeaponIdInt = pResult->GetInteger(Index);
		if((WeaponIdInt >= 0) && (WeaponIdInt < NB_INFWEAPON))
		{
			WeaponId = static_cast<EInfclassWeapon>(WeaponIdInt);
		}
	}

	return WeaponId;
}

void CIcGameController::ConSetWeaponFireDelay(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	if(pResult->NumArguments() != 2)
		return;

	const EInfclassWeapon WeaponId = GetWeaponIdFromConArgument(pResult, 0);
	if(WeaponId == EInfclassWeapon::Invalid)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", gs_aInvalidWeaponIdMsg);
		return;
	}

	const int Interval = pResult->GetInteger(1);
	if(Interval < 0)
	{
		return;
	}

	pSelf->SetFireDelay(WeaponId, Interval);
}

void CIcGameController::ConSetWeaponAmmoRegen(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	if(pResult->NumArguments() != 2)
		return;

	const EInfclassWeapon WeaponId = GetWeaponIdFromConArgument(pResult, 0);
	if(WeaponId == EInfclassWeapon::Invalid)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", gs_aInvalidWeaponIdMsg);
		return;
	}

	const int Interval = pResult->GetInteger(1);
	if(Interval < 0)
	{
		return;
	}

	pSelf->SetAmmoRegenTime(WeaponId, Interval);
}

void CIcGameController::ConSetWeaponMaxAmmo(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	if(pResult->NumArguments() != 2)
		return;

	const EInfclassWeapon WeaponId = GetWeaponIdFromConArgument(pResult, 0);
	if(WeaponId == EInfclassWeapon::Invalid)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", gs_aInvalidWeaponIdMsg);
		return;
	}
	const int Interval = pResult->GetInteger(1);
	if(Interval < 0)
	{
		return;
	}

	pSelf->SetMaxAmmo(WeaponId, Interval);
}

void CIcGameController::ConWeaponForce(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	if(pResult->NumArguments() < 1)
		return;

	const EInfclassWeapon WeaponId = GetWeaponIdFromConArgument(pResult, 0);
	if(WeaponId == EInfclassWeapon::Invalid)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", gs_aInvalidWeaponIdMsg);
		return;
	}

	if(pResult->NumArguments() < 2)
	{
		const CFixedPointNumber Force = pSelf->GetWeaponForce(WeaponId);
		char aBuf[32];
		str_format(aBuf, sizeof(aBuf), "Value: %s", Force.AsStr());
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "console", aBuf);
	}

	const float Force = pResult->GetFloat(1);
	if(Force < 0)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", "Negative values are not allowed.");
		return;
	}

	if(Force > 1000)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", "The given value is beyond all reasonable limits. Please use values in range from 0.1 to 100.");
		return;
	}

	pSelf->SetWeaponForce(WeaponId, Force);
}

void CIcGameController::ConListWeapons(IConsole::IResult *pResult, void *pUserData)
{
	const auto *pSelf = static_cast<CIcGameController *>(pUserData);

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", "Available weapons:");
	std::size_t WeaponIndex = 0;
	bool LastWeaponProcessed{};
	do
	{
		std::string line = "    ";
		for(;; ++WeaponIndex)
		{
			LastWeaponProcessed = WeaponIndex + 1 == NB_INFWEAPON;
			const EInfclassWeapon WeaponId = static_cast<EInfclassWeapon>(WeaponIndex);
			line += toString(WeaponId);
			if(LastWeaponProcessed)
				break;

			line += ", ";
			if(line.size() > 80)
			{
				break;
			}
		}

		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", line.c_str());
	} while(!LastWeaponProcessed);
}

void CIcGameController::ConGetActivePlayersNumber(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;

	int NumHumans = 0;
	int NumInfected = 0;
	pSelf->GetPlayerCounter(-1, NumHumans, NumInfected);

	const int ActivePlayerCounter = NumHumans + NumInfected;

	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "Active players number: %d", ActivePlayerCounter);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	pResult->m_Value = ActivePlayerCounter;
}

void CIcGameController::ConStartSurvivalScenario(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConStartSurvivalScenario(pResult);
}

void CIcGameController::ConStartSurvivalScenario(IConsole::IResult *pResult)
{
	if(GetRoundType() == ERoundType::Survival && IsInfectionStarted())
	{
		int ClientID = pResult->GetClientId();
		const char *pErrorMessage = _("The survival is already triggered");
		if(ClientID >= 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", pErrorMessage);
		}
		else
		{
			GameServer()->SendChatTarget(-1, pErrorMessage);
		}

		return;
	}

	m_SurvivalConfiguration.Reset();

	ExecuteFileEx(pResult->GetString(0));

	if(m_SurvivalConfiguration.SurvivalWaves.IsEmpty())
	{
		int ClientID = pResult->GetClientId();
		const char *pErrorMessage = "Unable to load the survival configuration";
		if(ClientID >= 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", pErrorMessage);
		}
		else
		{
			GameServer()->SendChatTarget(-1, pErrorMessage);
		}

		return;
	}

	QueueRoundType(ERoundType::Survival);

	if(!m_Warmup)
	{
		StartRound();
	}
}

void CIcGameController::ConRestoreClientName(IConsole::IResult *pResult, void *pUserData)
{
	const auto *pSelf = static_cast<CIcGameController *>(pUserData);

	const int PlayerId = pResult->GetInteger(0);

	CIcPlayer *pPlayer = pSelf->GetPlayer(PlayerId);
	if(!pPlayer)
	{
		return;
	}

	pPlayer->m_ClientNameLocked = true;
	pSelf->Server()->SetClientName(PlayerId, pPlayer->GetOriginalName());
}

void CIcGameController::ConSetClientName(IConsole::IResult *pResult, void *pUserData)
{
	const auto *pSelf = static_cast<CIcGameController *>(pUserData);

	const int PlayerId = pResult->GetInteger(0);
	const char *pNewName = pResult->GetString(1);

	if(pResult->NumArguments() != 2)
	{
		return;
	}

	CIcPlayer *pPlayer = pSelf->GetPlayer(PlayerId);
	if(!pPlayer)
	{
		return;
	}

	pPlayer->m_ClientNameLocked = true;
	pSelf->Server()->SetClientName(PlayerId, pNewName);
}

void CIcGameController::ConLockClientName(IConsole::IResult *pResult, void *pUserData)
{
	const auto *pSelf = static_cast<CIcGameController *>(pUserData);

	const int PlayerId = pResult->GetInteger(0);
	const int Lock = pResult->GetInteger(1);

	if(pResult->NumArguments() != 2)
	{
		return;
	}

	CIcPlayer *pPlayer = pSelf->GetPlayer(PlayerId);
	if(!pPlayer)
	{
		return;
	}

	pPlayer->m_ClientNameLocked = Lock != 0;
}

void CIcGameController::ConPreferClass(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	const int ClientId = pResult->GetClientId();

	const char *pClassName = pResult->GetString(0);
	pSelf->SetPreferredClass(ClientId, pClassName);
}

void CIcGameController::ConAlwaysRandom(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	const int ClientId = pResult->GetClientId();

	const bool Random = pResult->GetInteger(0) > 0;
	pSelf->SetPreferredClass(ClientId, Random ? EPlayerClass::Random : EPlayerClass::Invalid);
}

void CIcGameController::SetPreferredClass(int ClientId, const char *pClassName)
{
	bool Ok = false;
	const EPlayerClass PlayerClass = GetClassByName(pClassName, &Ok);

	if(!Ok)
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
			_("Unable to set preferred class: Invalid class name"), nullptr);
		return;
	}
	SetPreferredClass(ClientId, PlayerClass);
}

void CIcGameController::SetPreferredClass(int ClientId, EPlayerClass Class)
{
	if(!IsHumanClass(Class))
	{
		return;
	}

	CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
	{
		return;
	}

	pPlayer->SetPreferredClass(Class);

	switch(Class)
	{
	case EPlayerClass::Random:
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_PLAYER,
			_("A random class will be automatically attributed to you when round starts"),
			nullptr);
		break;
	case EPlayerClass::Invalid:
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_PLAYER,
			_("The class selector will be displayed when round starts"),
			nullptr);
		break;
	default:
	{
		const char *pClassDisplayName = GetClassDisplayName(Class);
		const auto Translated = Server()->Localization()->Localize(pPlayer->GetLanguage(), pClassDisplayName);
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_PLAYER,
			_("Class {str:ClassName} will be automatically attributed to you when round starts"),
			"ClassName", Translated.data(),
			nullptr);
		break;
	}
	}
}

void CIcGameController::ConAddBot(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConAddBot(pResult);
}

void CIcGameController::ConAddBot(IConsole::IResult *pResult)
{
	int BotsNumber = pResult->GetInteger(0);
	if(!BotsNumber)
	{
		return;
	}

	if(BotsNumber > MaxBots)
	{
		return;
	}

	const char *pClassName = pResult->GetString(1);
	bool Ok = false;
	EPlayerClass PlayerClass = GetClassByName(pClassName, &Ok);
	if(!Ok)
	{
		PlayerClass = EPlayerClass::Random;
	}

	int Lives = 0;
	if(GetRoundType() == ERoundType::Survival)
	{
		Lives = Config()->m_InfBotLives;
	}
	int HP = 0;
	int DropLevel = 0;
	float RespawnInterval = 0;
	TweaksArray Tweaks;

	for(int Arg = 2; Arg < pResult->NumArguments(); ++Arg)
	{
		const char *pStr = pResult->GetString(Arg);
		bool Ok;
		float Value;

		Value = ParseLives(pStr, &Ok);
		if(Ok)
		{
			Lives = Value;
			continue;
		}

		Value = ParseHP(pStr, &Ok);
		if(Ok)
		{
			HP = Value;
			continue;
		}

		Value = ParseDropLevel(pStr, &Ok);
		if(Ok)
		{
			DropLevel = Value;
			continue;
		}

		Value = ParseRespawn(pStr, &Ok);
		if(Ok)
		{
			RespawnInterval = Value;
			continue;
		}

		ParseTweaks(pStr, &Tweaks);
	}

	for(int i = 0; i < BotsNumber; ++i)
	{
		CBaseBotPlayer *pBot = AddBot();
		if(!pBot)
			return;

		if(PlayerClass == EPlayerClass::Random)
		{
			EPlayerClass c = ChooseInfectedClass(pBot);
			pBot->SetClass(c);
		}
		else
		{
			pBot->SetClass(PlayerClass);
		}

		pBot->SetMaxLives(Lives);
		pBot->SetMaxHP(HP);
		pBot->SetDropLevel(DropLevel);
		pBot->SetRespawnInterval(RespawnInterval);
		pBot->SetTweaks(Tweaks);
	}

	char aBuf[256];

	str_format(aBuf, sizeof(aBuf), "Artificial players joined the game");
	GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
}

void CIcGameController::ConRemoveBot(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConRemoveBot(pResult);
}

void CIcGameController::ConRemoveBot(IConsole::IResult *pResult)
{
	int BotID = pResult->GetInteger(0);
	if(BotID < 0)
	{
		RemoveBots();
		return;
	}

	CIcPlayer *pPlayer = GetPlayer(BotID);
	if(!pPlayer || !pPlayer->IsBot())
	{
		return;
	}

	RemoveBot(BotID, "Console command");
}

void CIcGameController::ConDumpBot(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ChatDumpBot(pResult);
}

void CIcGameController::ChatDumpBot(IConsole::IResult *pResult)
{
	int BotID = pResult->GetInteger(0);
	int ClientID = pResult->GetClientId();
	for(int i = 0; i < m_Bots.Size(); ++i)
	{
		if(m_Bots.At(i) && (m_Bots.At(i)->GetCid() == BotID))
		{
			const char *pBotData = m_Bots.At(i)->DumpBot();
			GameServer()->SendChat(ClientID, CGameContext::CHAT_ALL, pBotData);
			return;
		}
	}

	GameServer()->SendChat(ClientID, CGameContext::CHAT_ALL, "No such bot");
}

void CIcGameController::ConCheckAI(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConCheckAI(pResult);
}

void CIcGameController::ConCheckAI(IConsole::IResult *pResult)
{
	int ClientID = pResult->GetClientId();

	int DebugLevel = Config()->m_InfBotDebugLevel;
	Config()->m_InfBotDebugLevel = std::max<int>(2, DebugLevel);

	const char *pCommand = pResult->GetString(0);
	int CheckCID = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : ClientID;
	if(str_comp(pCommand, "debug") == 0)
	{
		if(std::is_same_v<UPlayerClass, CIcPlayer>)
		{
			GameServer()->SendChatTarget(ClientID, "The server is compiled without AI debug support");
			return;
		}

		CBotPlayer *pPlayer = static_cast<CBotPlayer *>(GetPlayer(CheckCID));
		if(!pPlayer)
		{
			GameServer()->SendChatTarget(ClientID, "No player to debug");
			return;
		}
		CIcCharacter *pCharacter = pPlayer->GetCharacter();
		if(!pCharacter)
		{
			GameServer()->SendChatTarget(ClientID, "No character to debug");
			return;
		}

		CBotPlayer::DIRECTION Direction = pCharacter->Core()->m_Input.m_TargetX > 0 ? CBotPlayer::DIRECTION_RIGHT : CBotPlayer::DIRECTION_LEFT;
		vec2 ToTarget(pCharacter->m_Input.m_TargetX, pCharacter->m_Input.m_TargetY);
		pPlayer->SetRoamingDirection(Direction);
		CNetObj_PlayerInput input;
		pPlayer->UpdateControlsRoaming(&input);
		Config()->m_InfBotDebugLevel = DebugLevel;

		vec2 OverWallTargetPosition;
		vec2 OnPlatformTargetPosition;
		int MaxJumps = pPlayer->GetAvailableJumps();
		int AirTilesAbove = pPlayer->GetAirTilesAbove(Direction, MaxJumps);
		bool HasWall = pPlayer->HasWallInTheDirection(Direction);
		bool HasDanger = pPlayer->HasDamageTiles(pCharacter->GetPos(), ToTarget, pCharacter->GetProximityRadius());
		int JumpsToGetOver = HasWall ? pPlayer->GetJumpsNeededToGetOverWall(Direction, MaxJumps, &OverWallTargetPosition) : 0;
		int JumpsToJumpOn = pPlayer->GetJumpsNeededToJumpOnPlatform(Direction, MaxJumps, &OnPlatformTargetPosition);

		const EThreatLevel LevelOfDanger = pPlayer->GetDangerLevelOnLine(pCharacter->GetPos(), pCharacter->GetPos() + ToTarget);
		int DangerLevel = static_cast<int>(LevelOfDanger);

		if(JumpsToGetOver)
		{
			GameServer()->CreateLaserDotEvent(OverWallTargetPosition, OverWallTargetPosition, Server()->TickSpeed() * 3.0);
		}
		if(JumpsToJumpOn)
		{
			GameServer()->CreateHammerDotEvent(OnPlatformTargetPosition, Server()->TickSpeed() * 3.0);
		}

		char aBuf[100];
		str_format(aBuf, sizeof(aBuf), "DangerInDir: %d, on line: %d", HasDanger ? 1 : 0, DangerLevel);
		GameServer()->SendChatTarget(ClientID, aBuf);
		str_format(aBuf, sizeof(aBuf), "MaxJumps: %d", MaxJumps);
		GameServer()->SendChatTarget(ClientID, aBuf);
		str_format(aBuf, sizeof(aBuf), "AirTilesAbove: %d", AirTilesAbove);
		GameServer()->SendChatTarget(ClientID, aBuf);
		str_format(aBuf, sizeof(aBuf), "JumpsToGetOverWall: %d", JumpsToGetOver);
		GameServer()->SendChatTarget(ClientID, aBuf);
		str_format(aBuf, sizeof(aBuf), "JumpsToJumpOnPlatform: %d", JumpsToJumpOn);
		GameServer()->SendChatTarget(ClientID, aBuf);
	}

	else if(str_comp(pCommand, "enable") == 0)
	{
		CBotPlayer::SetAiEnabled(1);
	}
	else if(str_comp(pCommand, "disable") == 0)
	{
		CBotPlayer::SetAiEnabled(0);
	}

	Config()->m_InfBotDebugLevel = DebugLevel;
}

void CIcGameController::ConAiTracePath(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConAiTracePath(pResult);
}

void CIcGameController::ConAiTracePath(IConsole::IResult *pResult)
{
	const int ClientID = pResult->GetClientId();
	const int CheckCID = pResult->GetInteger(0);
	const float X = pResult->GetFloat(1) * 32.0f;
	const float Y = pResult->GetFloat(2) * 32.0f;

	const CIcPlayer *pPlayer = GetPlayer(CheckCID);
	if(!pPlayer)
	{
		GameServer()->SendChatTarget(ClientID, "No player to debug");
		return;
	}
	if(!pPlayer->GetCharacter())
	{
		GameServer()->SendChatTarget(ClientID, "The player has no character to debug");
		return;
	}

	if(!pPlayer->IsBot())
	{
		if(std::is_same_v<UPlayerClass, CIcPlayer>)
		{
			GameServer()->SendChatTarget(ClientID, "Unable to debug a player: the server is compiled without AI debug support");
			return;
		}
	}

	const int DebugLevel = Config()->m_InfBotDebugLevel;
	Config()->m_InfBotDebugLevel = std::max<int>(2, DebugLevel);
	const CBotPlayer *pBotPlayer = static_cast<const CBotPlayer *>(pPlayer);
	pBotPlayer->GetBotUtils().IsReachableByGroundTraced(pBotPlayer->GetCharacter()->GetPos(), vec2(X, Y), pBotPlayer->GetMaxJumps(), 1000);
	Config()->m_InfBotDebugLevel = DebugLevel;
}

void CIcGameController::ConAiObjection(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConAiObjection(pResult);
}

void CIcGameController::ConAiObjection(IConsole::IResult *pResult)
{
	const char *pCommand = pResult->GetString(0);
	const char *pObjection = pResult->GetString(1);

	if(str_comp(pCommand, "reset") == 0)
	{
		CBotPlayer::ResetEnabledObjections();
		return;
	}

	EObjection Objection = fromString<EObjection>(pObjection);
	if(Objection == EObjection::Invalid)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid objection");
		return;
	}

	if(str_comp(pCommand, "enable") == 0)
	{
		CBotPlayer::SetObjectionEnabled(Objection, true);
	}
	else if(str_comp(pCommand, "disable") == 0)
	{
		CBotPlayer::SetObjectionEnabled(Objection, false);
	}
	else if(str_comp(pCommand, "set") == 0)
	{
		for(int i = 0; i < static_cast<int>(EObjection::Count); ++i)
		{
			EObjection Obj = static_cast<EObjection>(i);
			CBotPlayer::SetObjectionEnabled(Obj, Obj == Objection);
		}
	}
	else
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid command");
	}
}

void CIcGameController::ConAntiPing(IConsole::IResult *pResult, void *pUserData)
{
	const auto *pSelf = static_cast<CIcGameController *>(pUserData);
	const int ClientId = pResult->GetClientId();

	const int Arg = pResult->GetInteger(0);
	dbg_msg("server", "set_antiping ClientId=%d antiping=%d", ClientId, Arg);

	CIcPlayer *pPlayer = pSelf->GetPlayer(ClientId);
	pPlayer->SetAntiPingEnabled(Arg > 0);
}

void CIcGameController::ConAddControlPoint(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	float X = pResult->GetFloat(0) * 32.0f;
	float Y = pResult->GetFloat(1) * 32.0f;
	pSelf->AddControlPoint(vec2(X, Y));
}

void CIcGameController::ConUserSetClass(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ConUserSetClass(pResult);
}

void CIcGameController::ConUserSetClass(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetClientId();
	if(!Config()->m_InfTrainingMode)
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("The command is not available (enabled only in training mode)"), nullptr);
		return;
	}

	const char *pClassName = pResult->GetString(0);

	CIcPlayer *pPlayer = GetPlayer(ClientId);

	if(!pPlayer)
		return;

	bool Ok = false;
	const EPlayerClass PlayerClass = GetClassByName(pClassName, &Ok);
	if(Ok)
	{
		pPlayer->SetClass(PlayerClass);
		const char *pClassDisplayName = GetClassDisplayName(PlayerClass);
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER, _("{str:PlayerName} changed the class to {str:ClassName}"),
			"PlayerName", Server()->ClientName(ClientId),
			"ClassName", pClassDisplayName,
			nullptr);

		return;
	}

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "inf_set_class", "Unknown class");
}

void CIcGameController::ConRefreshHeroFlag(IConsole::IResult *pResult, void *pUserData)
{
	const auto *pSelf = static_cast<CIcGameController *>(pUserData);
	const int ClientId = pResult->GetClientId();
	if(pSelf->GetRoundType() != ERoundType::Survival)
	{
		pSelf->GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, "This command is only available in survival mode.");
		return;
	}
	auto *pPlayer = pSelf->GetPlayer(ClientId);
	if(pPlayer->GetClass() != EPlayerClass::Hero)
	{
		pSelf->GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, "Only heroes can use this command.");
		return;
	}
	auto *HumanClass = dynamic_cast<CInfClassHuman *>(pPlayer->GetCharacterClass());
	HumanClass->RefreshHeroFlagPosition();
}

void CIcGameController::ConReviveNear(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ConReviveNear(pResult);
}

void CIcGameController::ConReviveNear(const IConsole::IResult *pResult)
{
	if(!ReviveNear(pResult->GetInteger(0), pResult->GetInteger(1)))
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "inf_revive_near", "Revive operation failed");
}

bool CIcGameController::ReviveNear(const int RevivedPlayerId, const int TargetPlayerId)
{
	auto *pRevivedPlayer = GetPlayer(RevivedPlayerId);
	auto *pTargetPlayer = GetPlayer(TargetPlayerId);

	if(!(pRevivedPlayer && pTargetPlayer) || pRevivedPlayer->IsBot())
		return false;

	const auto Ok = pRevivedPlayer->TryRespawnNear(pTargetPlayer);

	if(Ok && GetRoundType() == ERoundType::Survival)
	{
		if(pRevivedPlayer->IsInfected() || pRevivedPlayer->GetClass() == EPlayerClass::None)
			pRevivedPlayer->SetClass(ChooseHumanClass(pRevivedPlayer));
		if(m_SurvivalState.KilledPlayers.Contains(pRevivedPlayer->GetCid()))
			m_SurvivalState.KilledPlayers.RemoveOne(pRevivedPlayer->GetCid());
	}

	return Ok;
}

void CIcGameController::ConSetClass(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ConSetClass(pResult);
}

void CIcGameController::ConSetClass(IConsole::IResult *pResult)
{
	const int PlayerId = pResult->GetInteger(0);
	const char *pClassName = pResult->GetString(1);

	CIcPlayer *pPlayer = GetPlayer(PlayerId);

	if(!pPlayer)
		return;

	bool Ok = false;
	const EPlayerClass PlayerClass = GetClassByName(pClassName, &Ok);
	if(Ok)
	{
		pPlayer->SetClass(PlayerClass);
		char aBuf[256];
		const char *pClassDisplayName = GetClassDisplayName(PlayerClass);
		str_format(aBuf, sizeof(aBuf), "The admin change the class of %s to %s", GameServer()->Server()->ClientName(PlayerId), pClassDisplayName);
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);

		return;
	}

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "inf_set_class", "Unknown class");
}

FunRoundConfiguration CIcGameController::ParseFunRoundConfigArguments(IConsole::IResult *pResult)
{
	FunRoundConfiguration FunRoundConfig;

	for(int argN = 0; argN < pResult->NumArguments(); ++argN)
	{
		const char *pArgument = pResult->GetString(argN);
		bool Ok = true;
		const EPlayerClass PlayerClass = CIcGameController::GetClassByName(pArgument, &Ok);
		if(!Ok)
		{
			// Ignore other words (there can be "undeads vs heroes", ignore "vs" in such case)
			continue;
		}
		if(IsHumanClass(PlayerClass))
		{
			FunRoundConfig.HumanClass = PlayerClass;
		}
		if(IsInfectedClass(PlayerClass))
		{
			FunRoundConfig.InfectedClass = PlayerClass;
		}
	}

	return FunRoundConfig;
}

void CIcGameController::ConQueueSpecialRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	const char *pRoundTypeName = pResult->GetString(0);
	pSelf->ConQueueRound(pRoundTypeName);
}

void CIcGameController::ConQueueRound(const char *pRoundTypeName)
{
	const ERoundType Type = fromString<ERoundType>(pRoundTypeName);
	if(Type == ERoundType::Invalid)
	{
		return;
	}
	QueueRoundType(Type);
}

void CIcGameController::ConStartRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	if(pResult->NumArguments() > 0 && pResult->GetString(0))
	{
		const ERoundType Type = fromString<ERoundType>(pResult->GetString(0));

		if(Type == ERoundType::Invalid)
		{
			return;
		}
		pSelf->QueueRoundType(Type);
	}

	pSelf->StartRound();
}

void CIcGameController::ConSurvivalClearConf(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->SurvivalClearConf();
}

void CIcGameController::SurvivalClearConf()
{
	SurvivalGetMutableGameConfiguration()->Reset();

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "survival configuration cleared");
}

void CIcGameController::ConSurvivalConf(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConSurvivalConf(pResult);
}

void CIcGameController::ConSurvivalConf(IConsole::IResult *pResult)
{
	char aBuf[256];
	const char *pOptionStr = pResult->GetString(0);
	ESurvivalConfigOption Option = fromString<ESurvivalConfigOption>(pOptionStr);
	int Value = pResult->GetInteger(1);
	switch(Option)
	{
	case ESurvivalConfigOption::MaxPlayers:
		if(pResult->NumArguments() > 1)
		{
			SurvivalGetMutableGameConfiguration()->MaxPlayers = Value;
		}
		else
		{
			Value = SurvivalGetGameConfiguration()->MaxPlayers;
		}
		break;
	case ESurvivalConfigOption::Hardmode:
		if(pResult->NumArguments() > 1)
		{
			SurvivalGetMutableGameConfiguration()->HardMode = Value;
		}
		else
		{
			Value = SurvivalGetGameConfiguration()->HardMode;
		}
		break;
	case ESurvivalConfigOption::Invalid:
		str_format(aBuf, sizeof(aBuf), "Invalid survival_conf argument '%s'", pOptionStr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	if(pResult->NumArguments() == 1)
	{
		str_format(aBuf, sizeof(aBuf), "survival_conf '%s' value is %d", pOptionStr, Value);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

void CIcGameController::ConSurvivalAddWave(IConsole::IResult *pResult, void *pUserData)
{
	int Wave = pResult->GetInteger(0);
	const char *pWaveName = pResult->GetString(1);

	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->SurvivalAddWave(Wave, pWaveName);
}

void CIcGameController::SurvivalAddWave(int Wave, const char *pWaveName)
{
	if(static_cast<std::size_t>(Wave) != m_SurvivalConfiguration.SurvivalWaves.Size() + 1)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Only incremental setup allowed. Add the previous waves first");
		return;
	}

	m_SurvivalConfiguration.AddWave(pWaveName);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "survival wave added");
}

void CIcGameController::ConSurvivalConfWave(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConSurvivalConfWave(pResult);
}

void CIcGameController::ConSurvivalConfWave(IConsole::IResult *pResult)
{
	int Wave = pResult->GetInteger(0);
	int WaveIndex = Wave - 1;

	SurvivalWaveConfiguration *pConf = SurvivalGetWaveConfiguration(WaveIndex);
	if(!pConf)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid wave number");
		return;
	}

	const char *pAction = pResult->GetString(1);
	if(str_comp(pAction, "add") == 0)
	{
		const char *pClassName = pResult->GetString(2);
		SurvivalBotConfiguration *pBotConfig = SurvivalAddBot(Wave, pClassName);
		if(!pBotConfig)
			return;

		ConSurvivalConfWaveAddBots(pResult, pBotConfig);
	}
	else if(str_comp(pAction, "command") == 0)
	{
		ConSurvivalConfWaveCommand(pResult, pConf);
	}
	else
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid action");
	}
}

SurvivalBotConfiguration *CIcGameController::SurvivalAddBot(int Wave, const char *pClassName)
{
	bool Ok = false;
	EPlayerClass PlayerClass = static_cast<EPlayerClass>(GetClassByName(pClassName, &Ok));
	if(!Ok)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "SurvivalAddBot: Invalid class name");
		return nullptr;
	}

	if(!IsInfectedClass(PlayerClass))
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "SurvivalAddBot: Only infected classes allowed");
		return nullptr;
	}

	int WaveIndex = Wave - 1;
	SurvivalWaveConfiguration *pConf = SurvivalGetWaveConfiguration(WaveIndex);
	if(!pConf)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "SurvivalAddBot: Invalid wave number");
		return nullptr;
	}

	if(pConf->BotConfigurations.Size() >= pConf->BotConfigurations.Capacity())
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "SurvivalAddBot: Too many bots");
		return nullptr;
	}

	pConf->BotConfigurations.Add(SurvivalBotConfiguration{.Class = PlayerClass});

	return &pConf->BotConfigurations.Last();
}

void CIcGameController::ConSurvivalConfWaveAddBots(IConsole::IResult *pResult, SurvivalBotConfiguration *pBotConfiguration) const
{
	int SpawnTimeInSeconds = 0;
	int Lives = 0;
	int HP = 0;
	int DropLevel = 0;
	float RespawnInterval = 0;
	TweaksArray Tweaks;

	for(int Arg = 3; Arg < pResult->NumArguments(); ++Arg)
	{
		const char *pStr = pResult->GetString(Arg);
		bool Ok;
		float Value;

		Value = ParseSpawnTime(pStr, &Ok);
		if(Ok)
		{
			SpawnTimeInSeconds = Value;
			continue;
		}

		Value = ParseLives(pStr, &Ok);
		if(Ok)
		{
			Lives = Value;
			continue;
		}

		Value = ParseHP(pStr, &Ok);
		if(Ok)
		{
			HP = Value;
			continue;
		}

		Value = ParseDropLevel(pStr, &Ok);
		if(Ok)
		{
			DropLevel = Value;
			continue;
		}

		Value = ParseRespawn(pStr, &Ok);
		if(Ok)
		{
			RespawnInterval = Value;
			continue;
		}

		Ok = ParseTweaks(pStr, &Tweaks);
		if(Ok)
		{
			continue;
		}

		char aBuffer[256];
		str_format(aBuffer, sizeof(aBuffer), "Invalid wave specification, unable to parse argument '%s'", pStr);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuffer);
	}

	int SpawnMinTick = Server()->TickSpeed() * SpawnTimeInSeconds;
	pBotConfiguration->SpawnMinTick = SpawnMinTick;
	pBotConfiguration->Lives = Lives;
	pBotConfiguration->HP = HP;
	pBotConfiguration->DropLevel = DropLevel;
	pBotConfiguration->RespawnInterval = RespawnInterval;
	pBotConfiguration->Tweaks = Tweaks;

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "survival wave configuration changed");
}

void CIcGameController::ConSurvivalConfWaveCommand(IConsole::IResult *pResult, SurvivalWaveConfiguration *pConfiguration) const
{
	const char *pCommandEvent = pResult->GetString(2);
	const char *pCommandCode = pResult->GetString(3);

	if(str_length(pCommandCode) >= SurvivalWaveConfiguration::MaxCommandLength)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Unable to add survival wave configuration command: the command too long");
		return;
	}

	// on_won, on_lost
	if(str_comp(pCommandEvent, "on_won") == 0)
	{
		str_copy(pConfiguration->aCommandOnWon, pCommandCode);
	}
	else if(str_comp(pCommandEvent, "on_lost") == 0)
	{
		str_copy(pConfiguration->aCommandOnLost, pCommandCode);
	}
	else
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid wave command specification, unable to parse command 'event' part");
	}
}

void CIcGameController::ConStartSurvival(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConStartSurvival(pResult);
}

void CIcGameController::ConStartSurvival(IConsole::IResult *pResult)
{
	if(Config()->m_InfSurvivalMode == SURVIVAL_MODE_OFF)
	{
		int ClientID = pResult->GetClientId();
		const char *pErrorMessage = "Unable to start a survival: Survival mode is turned off";
		if(ClientID >= 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", pErrorMessage);
		}
		else
		{
			GameServer()->SendChatTarget(-1, pErrorMessage);
		}

		return;
	}

	if(GetRoundType() == ERoundType::Survival)
	{
		int ClientID = pResult->GetClientId();
		const char *pErrorMessage = _("The survival is already triggered");
		if(ClientID >= 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", pErrorMessage);
		}
		else
		{
			GameServer()->SendChatTarget(-1, pErrorMessage);
		}

		return;
	}

	int Wave = pResult->NumArguments() > 0 ? pResult->GetInteger(0) - 1 : 0;

	if(Wave < 0)
		return;

	const int MaxWave = m_SurvivalConfiguration.SurvivalWaves.Size();
	if(Wave >= MaxWave)
		return;

	PrepareSurvival(Wave);

	QueueRoundType(ERoundType::Survival);

	DoWarmup(3);
}

void CIcGameController::PrepareSurvival(int Wave)
{
	if(Config()->m_InfSurvivalMode)
	{
		// Survival is a whole new game, reset the counter!
		m_RoundCount = Wave;
		const int MaxWave = m_SurvivalConfiguration.SurvivalWaves.Size();
		Config()->m_SvRoundsPerMap = MaxWave;
	}

	m_SurvivalState.Wave = Wave;

	m_SurvivalState.Scores.Clear();
	m_SurvivalState.Kills = 0;
	m_SurvivalState.KilledPlayers.Clear();
	m_SurvivalState.SurvivedPlayers.Clear();

	ResetRoundData();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CIcPlayer *pPlayer = GetPlayer(i);
		if(pPlayer)
		{
			pPlayer->KillCharacter();
			pPlayer->SetClass(EPlayerClass::None);
		}
	}
}

bool CIcGameController::SurvivalHumansWinConditionsMet() const
{
	const bool TimeBased = Config()->m_InfSurvivalMode == SURVIVAL_MODE_TIME_BASED;
	if(TimeBased)
	{
		bool TimeIsOut = false;
		const int Seconds = GetRoundTick() / static_cast<float>(Server()->TickSpeed());
		if(GetTimeLimitMinutes() > 0 && Seconds >= GetTimeLimitSeconds())
		{
			TimeIsOut = true;
		}
		return TimeIsOut;
	}

	const SurvivalWaveConfiguration *WaveConf = GetCurrentSurvivalWaveConfiguration();
	if(WaveConf)
	{
		if(SpawnedBotsTracker.GetSpawnedCount() < WaveConf->BotConfigurations.Size())
		{
			return false;
		}
	}

	for(const CBaseBotPlayer *pBot : m_Bots)
	{
		if(!pBot->IsHuman() && (pBot->Lives() != 0))
		{
			return false;
		}
	}

	return true;
}

bool CIcGameController::SurvivalInfectedWinConditionsMet() const
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CIcCharacter *pCharacter = GetCharacter(ClientId);
		if(!pCharacter || !pCharacter->IsHuman() || pCharacter->IsDead())
			continue;

		return false;
	}

	return true;
}

void CIcGameController::ConStartFunRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	if(pSelf->m_FunRoundConfigurations.empty())
	{
		const int ClientId = pResult->GetClientId();
		const char *pErrorMessage = _("Unable to start fun round: rounds configuration is empty");
		if(ClientId >= 0)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", pErrorMessage);
		}
		else
		{
			pSelf->GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, pErrorMessage, nullptr);
		}
		return;
	}
	if(pSelf->m_FunRoundsPassed >= g_Config.m_FunRoundLimit)
	{
		pSelf->GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
			_("Unable to start fun round: limit per map reached"), nullptr);
		return;
	}

	pSelf->QueueRoundType(ERoundType::Fun);
	pSelf->StartRound();
}

void CIcGameController::ConQueueFunRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);

	if(pSelf->m_FunRoundConfigurations.empty())
	{
		const int ClientId = pResult->GetClientId();
		const char *pErrorMessage = "Unable to start a fun round: rounds configuration is empty";
		if(ClientId >= 0)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", pErrorMessage);
		}
		else
		{
			pSelf->GameServer()->SendChatTarget(-1, pErrorMessage);
		}
		return;
	}

	pSelf->QueueRoundType(ERoundType::Fun);
}

void CIcGameController::ConStartSpecialFunRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	FunRoundConfiguration FunRoundConfig = ParseFunRoundConfigArguments(pResult);

	std::vector<FunRoundConfiguration> aOldConfigurations;
	std::swap(pSelf->m_FunRoundConfigurations, aOldConfigurations);
	pSelf->m_FunRoundConfigurations = {FunRoundConfig};

	pSelf->QueueRoundType(ERoundType::Fun);

	if(!pSelf->m_Warmup)
	{
		pSelf->StartRound();
	}

	std::swap(pSelf->m_FunRoundConfigurations, aOldConfigurations);
}

void CIcGameController::ConClearFunRounds(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->m_FunRoundConfigurations.clear();
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "fun rounds cleared");
}

void CIcGameController::ConAddFunRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	const FunRoundConfiguration FunRoundConfig = ParseFunRoundConfigArguments(pResult);

	if((FunRoundConfig.HumanClass == EPlayerClass::Invalid) || (FunRoundConfig.InfectedClass == EPlayerClass::Invalid))
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "invalid special fun round configuration");
		return;
	}
	else
	{
		char aBuf[256];
		const char *HumanClassText = CIcGameController::GetClassPluralDisplayName(FunRoundConfig.HumanClass);
		const char *InfectedClassText = CIcGameController::GetClassPluralDisplayName(FunRoundConfig.InfectedClass);
		str_format(aBuf, sizeof(aBuf), "Added fun round: %s vs %s", InfectedClassText, HumanClassText);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}

	pSelf->m_FunRoundConfigurations.push_back(FunRoundConfig);
}

void CIcGameController::ConStartFastRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->QueueRoundType(ERoundType::Fast);
	pSelf->StartRound();
}

void CIcGameController::ConQueueFastRound(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->QueueRoundType(ERoundType::Fast);
}

void CIcGameController::ConPrintPlayerPickingTimestamp(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ConPrintPlayerPickingTimestamp(pResult);
}

void CIcGameController::ConPrintPlayerPickingTimestamp(IConsole::IResult *pResult)
{
	char aBuf[256];
	const int CurrentTimestamp = time_timestamp();
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const CIcPlayer *pPlayer = GetPlayer(i);
		if(pPlayer == nullptr)
			continue;
		if(pPlayer->IsBot())
			continue;

		const int Timestamp = pPlayer->GetInfectionTimestamp();

		const char *pPickedSecondsAgo = "";
		char aSecondsBuf[32];
		if(Timestamp && CurrentTimestamp > Timestamp)
		{
			const int SecondsAgo = CurrentTimestamp - Timestamp;
			str_format(aSecondsBuf, sizeof(aSecondsBuf), " (%d seconds ago)", SecondsAgo);
			pPickedSecondsAgo = aSecondsBuf;
		}

		str_format(aBuf, sizeof(aBuf), "id=%d name='%s' team='%d' ts=%d%s", i, Server()->ClientName(i), pPlayer->GetTeam(), Timestamp, pPickedSecondsAgo);

		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

void CIcGameController::ConMapRotationStatus(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ConSmartMapRotationStatus();
}

void CIcGameController::ConSaveMapsData(IConsole::IResult *pResult, void *pUserData)
{
	const char *pFileName = pResult->GetString(0);

	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->SaveMapRotationData(pFileName);
}

void CIcGameController::ConPrintMapsData(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->PrintMapRotationData();
}

void CIcGameController::ConResetMapData(IConsole::IResult *pResult, void *pUserData)
{
	const char *pMapName = pResult->GetString(0);

	ResetMapInfo(pMapName);
}

void CIcGameController::ConAddMapData(IConsole::IResult *pResult, void *pUserData)
{
	const char *pMapName = pResult->GetString(0);
	const int Timestamp = pResult->GetInteger(1);

	AddMapTimestamp(pMapName, Timestamp);
}

void CIcGameController::ConSetMapMinMaxPlayers(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConSetMapMinMaxPlayers(pResult);
}

void CIcGameController::ConSetMapMinMaxPlayers(IConsole::IResult *pResult)
{
	if((pResult->NumArguments() < 2) || (pResult->NumArguments() > 3))
	{
		return;
	}

	const char *pMapName = pResult->GetString(0);
	const int MinPlayers = pResult->GetInteger(1);
	const int MaxPlayers = pResult->NumArguments() == 3 ? pResult->GetInteger(2) : 0;

	SetMapMinMaxPlayers(pMapName, MinPlayers, MaxPlayers);
}

void CIcGameController::ConSavePosition(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConSavePosition(pResult);
}

void CIcGameController::ConSavePosition(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetClientId();
	if(!Config()->m_InfTrainingMode)
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("The command is not available (enabled only in training mode)"), nullptr);
		return;
	}

	const CIcCharacter *pCharacter = GetCharacter(ClientId);
	if(!pCharacter)
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Unable to save the position: you have no character to save its position"), nullptr);
		return;
	}

	if(!pCharacter->IsAlive())
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Unable to save the position: the character state is not valid"), nullptr);
		return;
	}

	if(!pCharacter->IsGrounded())
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Unable to save the position: the character does not stand on the ground"), nullptr);
		return;
	}

	const vec2 Position = pCharacter->GetPos();
	CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
	{
		// What...
		return;
	}

	pPlayer->AddSavedPosition(Position);
}

void CIcGameController::ConLoadPosition(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConLoadPosition(pResult);
}

void CIcGameController::ConLoadPosition(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetClientId();
	if(!Config()->m_InfTrainingMode)
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("The command is not available (enabled only in training mode)"), nullptr);
		return;
	}

	CIcCharacter *pCharacter = GetCharacter(ClientId);
	if(!pCharacter)
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Unable to load the position: you have no character to load its position"), nullptr);
		return;
	}

	if(!pCharacter->IsAlive())
	{
		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Unable to load the position: the character state is not valid"), nullptr);
		return;
	}

	vec2 Position;
	const CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
	{
		// What...
		return;
	}

	pPlayer->LoadSavedPosition(&Position);

	pCharacter->SetPosition(Position);
	pCharacter->ResetVelocity();
	GameWorld()->ReleaseHooked(ClientId);
	pCharacter->ResetHook();
}

void CIcGameController::ConSetHealthArmor(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConSetHealthArmor(pResult);
}

void CIcGameController::ConSetHealthArmor(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetInteger(0);
	const int Health = pResult->GetInteger(1);
	const int Armor = pResult->GetInteger(2);

	CIcCharacter *pCharacter = GetCharacter(ClientId);
	if(!pCharacter)
	{
		return;
	}

	pCharacter->SetHealthArmor(Health, Armor);
}

void CIcGameController::ConSetInvincible(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConSetInvincible(pResult);
}

void CIcGameController::ConSetInvincible(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetInteger(0);
	const int Invincible = pResult->GetInteger(1);

	CIcCharacter *pCharacter = GetCharacter(ClientId);
	if(!pCharacter)
	{
		return;
	}

	pCharacter->SetInvincible(Invincible);
}

void CIcGameController::ConSetHookProtection(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConSetHookProtection(pResult);
}

void CIcGameController::ConSetHookProtection(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetInteger(0);
	const int Protection = pResult->GetInteger(1);

	CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
	{
		return;
	}

	constexpr bool Automatic = false;
	pPlayer->SetHookProtection(Protection, Automatic);
}

void CIcGameController::ConGiveUpgrade(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConGiveUpgrade(pResult);
}

void CIcGameController::ConGiveUpgrade(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetInteger(0);

	CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer || !pPlayer->GetCharacterClass())
	{
		return;
	}

	const int Level = pPlayer->GetCharacterClass()->GetUpgradeLevel();
	const auto Upgrades = pPlayer->GetCharacterClass()->GetUpgrade(Level + 1);
	pPlayer->GetCharacterClass()->GiveUpgrades(Upgrades);
}

void CIcGameController::ConSetDrop(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	return pSelf->ConSetDrop(pResult);
}

void CIcGameController::ConSetDrop(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetInteger(0);
	CIcCharacter *pCharacter = GetCharacter(ClientId);
	if(!pCharacter || !pCharacter->IsAlive())
	{
		return;
	}

	constexpr int UnreasonableMaxLevel = 99;
	const int DropLevel = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : UnreasonableMaxLevel;
	if(DropLevel < 0)
	{
		return;
	}

	pCharacter->SetDropLevel(DropLevel);
}

void CIcGameController::ConChatSurvivalRespawn(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ConChatSurvivalRespawn(pResult);
}

void CIcGameController::ConChatSurvivalRespawn(IConsole::IResult *pResult)
{
	if(GetRoundType() != ERoundType::Survival)
		return;
	if(!Config()->m_InfSurvivalRespawn)
	{
		GameServer()->SendChatTarget_Localization(pResult->GetClientId(),
			CHATCATEGORY_DEFAULT,
			_("Survival respawning is disabled"), nullptr);
		return;
	}

	auto *pCaller = GetPlayer(pResult->GetClientId());
	if(pCaller->GetTeam() != TEAM_SPECTATORS)
		return;

	if(Server()->Tick() > pCaller->GetSurvivalRespawnTick())
	{
		const auto pTargetPlayer = GetClientIdByName(pResult->GetString(0));
		if(!pTargetPlayer.has_value())
			return;
		ReviveNear(pResult->GetClientId(), pTargetPlayer.value());
	}
}

void CIcGameController::ChatHeroRevive(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ChatHeroRevive(pResult);
}

void CIcGameController::ChatHeroRevive(IConsole::IResult *pResult)
{
	const int CallerClientId = pResult->GetClientId();
	auto *pCaller = GetPlayer(CallerClientId);
	auto *pCallerCharacterClass = pCaller->GetCharacterClass();
	if(GetRoundType() != ERoundType::Survival || pCaller->GetClass() != EPlayerClass::Hero || !pCallerCharacterClass)
		return;
	const auto RevivedClientId = GetClientIdByName(pResult->GetString(0));
	if(!RevivedClientId.has_value())
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), _("No player with name \"%s\" found"), pResult->GetString(0));
		GameServer()->SendChatTarget_Localization(CallerClientId, CHATCATEGORY_DEFAULT, aBuf);
		return;
	}
	const auto *pRevivedPlayer = GetPlayer(RevivedClientId.value());
	if(pRevivedPlayer->GetTeam() != TEAM_SPECTATORS || pRevivedPlayer->IsBot())
		return;
	auto *pCallerHumanClass = dynamic_cast<CInfClassHuman *>(pCallerCharacterClass);
	if(pCallerHumanClass->GetSurvivalHeroReviveCharges() < 1)
	{
		GameServer()->SendChatTarget_Localization(CallerClientId, CHATCATEGORY_DEFAULT, _("No revive charges left"), nullptr);
		return;
	}

	if(ReviveNear(RevivedClientId.value(), CallerClientId))
	{
		pCallerHumanClass->SetSurvivalHeroReviveCharges(pCallerHumanClass->GetSurvivalHeroReviveCharges() - 1);
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
			_("The hero \"{str:Hero}\" revived \"{str:Revived}\""),
			"Hero", Server()->ClientName(CallerClientId),
			"Revived", Server()->ClientName(RevivedClientId.value()),
			nullptr);
	}
	else
	{
		GameServer()->SendChatTarget_Localization(CallerClientId, CHATCATEGORY_DEFAULT, _("Revive failed"), nullptr);
	}
}

void CIcGameController::ChatWitch(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CIcGameController *>(pUserData);
	pSelf->ChatWitch(pResult);
}

void CIcGameController::ChatWitch(IConsole::IResult *pResult)
{
	const int ClientId = pResult->GetClientId();
	constexpr int REQUIRED_CALLERS_COUNT = 5;
	constexpr int MIN_ZOMBIES = 2;

	Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "conwitch", "ChatWitch() called");

	const bool Winter = EventsDirector::IsWinter();

	{
		bool CanCallWitch = true;
		if(GetRoundType() == ERoundType::Survival)
		{
			CanCallWitch = false;
		}
		if(GetRoundType() == ERoundType::HideAndSeek)
		{
			CanCallWitch = false;
		}

		if(!CanCallWitch)
		{
			const char *pMessage = _("The witch is not available in this round");
			if(Winter)
			{
				pMessage = _("The Santa is not available in this round");
			}

			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, pMessage, nullptr);
			return;
		}
	}

	int MaxWitches = GetClassPlayerLimit(EPlayerClass::Witch);
	if(Winter)
	{
		// Santa is a new Witch; allow only one Santa at time.
		MaxWitches = 1;
	}
	if(GetInfectedCount(EPlayerClass::Witch) >= MaxWitches)
	{
		if(Winter)
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("The Santa is already here"), nullptr);
			return;
		}

		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("All witches are already here"), nullptr);
		return;
	}

	int Humans = 0;
	int Infected = 0;
	GetPlayerCounter(-1, Humans, Infected);

	if(Humans + Infected < REQUIRED_CALLERS_COUNT)
	{
		if(Winter)
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Too few players to call the Santa"), nullptr);
			return;
		}

		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Too few players to call a witch"), nullptr);
		return;
	}
	if(Infected < MIN_ZOMBIES)
	{
		if(Winter)
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Too few infected to call the Santa"), nullptr);
			return;
		}

		GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("Too few infected to call a witch"), nullptr);
		return;
	}

	// It is possible that we had the needed callers but all witches already were there.
	// In that case even if the caller is already in the list, we still want to spawn
	// a new one without a message to the caller.
	if(m_WitchCallers.Size() < REQUIRED_CALLERS_COUNT)
	{
		if(m_WitchCallers.Contains(ClientId))
		{
			if(Winter)
			{
				GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("You have called the Santa once again"), nullptr);
				return;
			}

			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT, _("You can't call witch twice"), nullptr);
			return;
		}

		m_WitchCallers.Add(ClientId);

		int PrintableRequiredCallers = REQUIRED_CALLERS_COUNT;
		int PrintableCallers = m_WitchCallers.Size();
		if(m_WitchCallers.Size() == 1)
		{
			if(Winter)
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
					_("{str:PlayerName} is calling for Santa! (1/{int:RequiredCallers}) To call the Santa write: /santa"),
					"PlayerName", Server()->ClientName(ClientId),
					"RequiredCallers", &PrintableRequiredCallers,
					nullptr);
				return;
			}

			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
				_("{str:PlayerName} is calling for Witch! (1/{int:RequiredCallers}) To call witch write: /witch"),
				"PlayerName", Server()->ClientName(ClientId),
				"RequiredCallers", &PrintableRequiredCallers,
				nullptr);
		}
		else if(m_WitchCallers.Size() < REQUIRED_CALLERS_COUNT)
		{
			if(Winter)
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
					_("Santa ({int:Callers}/{int:RequiredCallers})"),
					"Callers", &PrintableCallers,
					"RequiredCallers", &PrintableRequiredCallers,
					nullptr);
			}
			else
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
					_("Witch ({int:Callers}/{int:RequiredCallers})"),
					"Callers", &PrintableCallers,
					"RequiredCallers", &PrintableRequiredCallers,
					nullptr);
			}
		}
	}

	if(m_WitchCallers.Size() >= REQUIRED_CALLERS_COUNT)
	{
		const int WitchId = GetClientIdForNewWitch();
		if(WitchId < 0)
		{
			if(Winter)
			{
				GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
					_("The Santa is already here"),
					nullptr);
				return;
			}
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("All witches are already here"),
				nullptr);
		}
		else
		{
			if(Winter)
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
					_("Santa {str:PlayerName} has arrived!"),
					"PlayerName", Server()->ClientName(WitchId),
					nullptr);
				return;
			}

			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
				_("Witch {str:PlayerName} has arrived!"),
				"PlayerName", Server()->ClientName(WitchId),
				nullptr);
		}

		m_WitchCallers.Clear();
	}
}

void CIcGameController::ConSayBot(IConsole::IResult *pResult, void *pUserData)
{
	CIcGameController *pSelf = (CIcGameController *)pUserData;
	pSelf->ConSayBot(pResult);
}

void CIcGameController::ConSayBot(IConsole::IResult *pResult)
{
	int BotID = pResult->GetInteger(0);
	const CIcPlayer *pPlayer = GetPlayer(BotID);
	if(!pPlayer || !pPlayer->IsBot())
	{
		return;
	}

	GameServer()->SendChat(BotID, CGameContext::CHAT_ALL, pResult->GetString(1));
}

CControlPoint *CIcGameController::AddControlPoint(const vec2 &At)
{
	return new CControlPoint(GameServer(), At);
}

CDoor *CIcGameController::AddDoor(const vec2 &From, const vec2 &To)
{
	return new CDoor(GameServer(), From, To);
}

IConsole *CIcGameController::Console() const
{
	return GameServer()->Console();
}

IDebugSink *CIcGameController::GetDebugSink() const
{
	return m_pBotUtilsData->m_pDebugSink;
}

CIcPlayer *CIcGameController::GetPlayer(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return nullptr;

	return CIcPlayer::GetInstance(GameServer()->m_apPlayers[ClientId]);
}

CIcCharacter *CIcGameController::GetCharacter(int ClientId) const
{
	CIcPlayer *pPlayer = GetPlayer(ClientId);
	return pPlayer ? pPlayer->GetCharacter() : nullptr;
}

int CIcGameController::GetPlayerOwnCursorId(int ClientId) const
{
	return m_PlayerOwnCursorId;
}

void CIcGameController::SortCharactersByDistance(ClientsArray *pCharacterIds, const vec2 &Center, const float MaxDistance)
{
	SortCharactersByDistance(*pCharacterIds, pCharacterIds, Center, MaxDistance);
}

void CIcGameController::SortCharactersByDistance(const ClientsArray &Input, ClientsArray *pOutput, const vec2 &Center, const float MaxDistance)
{
	struct DistanceItem
	{
		DistanceItem() = default;
		DistanceItem(int C, float D) : ClientId(C), Distance(D)
		{
		}

		int ClientId;
		float Distance;

		bool operator<(const DistanceItem &AnotherDistanceItem) const
		{
			return Distance < AnotherDistanceItem.Distance;
		}
	};

	icArray<DistanceItem, MAX_CLIENTS> Distances;

	for(const int ClientId : Input)
	{
		const CCharacter *pChar = GetCharacter(ClientId);
		if(!pChar)
			continue;

		const vec2 &CharPos = pChar->GetPos();
		const float Distance = std::max<float>(0.f, distance(CharPos, Center) - pChar->GetProximityRadius());
		if(MaxDistance && (Distance > MaxDistance))
			continue;

		Distances.Add(DistanceItem(ClientId, Distance));
	}

	std::sort(Distances.begin(), Distances.end());

	pOutput->Clear();
	for(const DistanceItem &Item : Distances)
	{
		pOutput->Add(Item.ClientId);
	}
}

void CIcGameController::GetSortedTargetsInRange(const vec2 &Center, const float Radius, const ClientsArray &SkipList, ClientsArray *pOutput)
{
	ClientsArray PossibleCids;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CCharacter *pChar = GetCharacter(ClientId);
		if(!pChar)
			continue;

		if(SkipList.Contains(ClientId))
			continue;

		PossibleCids.Add(ClientId);
	}

	SortCharactersByDistance(PossibleCids, pOutput, Center, Radius);
}

void CIcGameController::UpdateNinjaTargets()
{
	m_NinjaTargets.Clear();

	if(!m_InfectedStarted && !Config()->m_InfTrainingMode)
		return;

	if(GetRoundType() == ERoundType::Survival)
		return;

	int InfectedCount = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GetCharacter(i) && GetCharacter(i)->IsInfected())
		{
			InfectedCount++;
			if(GetPlayer(i)->GetClass() == EPlayerClass::Undead)
				continue;

			if(GetCharacter(i)->GetInfZoneTick() * Server()->TickSpeed() < 1000 * Config()->m_InfNinjaTargetAfkTime) // Make sure zombie is not camping in InfZone
			{
				m_NinjaTargets.Add(i);
			}
		}
	}

	if(InfectedCount < Config()->m_InfNinjaMinInfected)
	{
		m_NinjaTargets.Clear();
	}
}

void CIcGameController::ReservePlayerOwnSnapItems()
{
	m_PlayerOwnCursorId = Server()->SnapNewId();
}

void CIcGameController::FreePlayerOwnSnapItems()
{
	Server()->SnapFreeId(m_PlayerOwnCursorId);
}

void CIcGameController::SendHintMessage()
{
	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		// We don't have hints for this round type
		return;
	}

	if((g_Config.m_TipsInterval == 0) || (time_get() - m_LastTipTime < time_freq() * g_Config.m_TipsInterval * 60))
		return;

	m_LastTipTime = time_get();

	const int MessageIndex = random_int(0, std::size(gs_aHintMessages) - 1);
	const CHintMessage &Message = gs_aHintMessages[MessageIndex];
	dynamic_string Buffer;
	const char *pPrevLang = nullptr;
	bool Sent = false;

	const auto PrepareBufferForLanguage = [&](const char *pLang) {
		if(!pPrevLang || str_comp(pLang, pPrevLang) != 0)
		{
			pPrevLang = pLang;

			FormatHintMessage(Message, &Buffer, pLang);
		}
	};

	for(int CID = 0; CID < MAX_CLIENTS; ++CID)
	{
		const CIcPlayer *pPlayer = GetPlayer(CID);
		if(!pPlayer || pPlayer->IsBot() || !pPlayer->m_IsReady)
			continue;
		PrepareBufferForLanguage(GetPlayer(CID)->GetLanguage());
		GameServer()->SendChatTarget(CID, Buffer.buffer());
		Sent = true;
	}

	if(Sent && g_Config.m_SvDemoChat)
	{
		// for demo record
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientId = -1;

		PrepareBufferForLanguage(Config()->m_InfDefaultLanguageCode);
		Msg.m_pMessage = Buffer.buffer();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, SERVER_DEMO_CLIENT);

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "*** %s", Msg.m_pMessage);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "chat", aBuf);
	}
}

void CIcGameController::FormatHintMessage(const CHintMessage &Message, dynamic_string *pBuffer, const char *pLanguage) const
{
	pBuffer->clear();
	pBuffer->append("TIP: ");
	if(Message.m_pArg1Value)
	{
		pBuffer->append(Server()->Localization()->Format_L(pLanguage, Message.m_pText, Message.m_pArg1Name, Message.m_pArg1Value).c_str());
	}
	else
	{
		pBuffer->append(Server()->Localization()->Format_L(pLanguage, Message.m_pText).c_str());
	}
}

void CIcGameController::OnInfectionTriggered()
{
	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	const int NumPlayers = NumHumans + NumInfected;
	const int NumFirstPickedPlayers = GetMinimumInfectedForPlayers(NumPlayers);

	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		const int SeekingPlayers = NumFirstPickedPlayers;
		StartHideAndSeekGameplay(SeekingPlayers);
	}
	else
	{
		const int PlayersToInfect = maximum<int>(0, NumFirstPickedPlayers - NumInfected);
		StartInfectionGameplay(PlayersToInfect);
	}

	m_InfUnbalancedTick = -1;
	MaybeSuggestMoreRounds();

	if(GetRoundType() == ERoundType::Survival)
	{
		int MaxPlayers = SurvivalGetGameConfiguration()->MaxPlayers;
		if(Config()->m_InfSurvivalPlayerLimit && MaxPlayers && NumPlayers > MaxPlayers)
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Unable to start the game: too many players!"), nullptr);
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
				_P("The team has {int:ActivePlayers} active players but only {int:MaxPlayers} player allowed.",
					"The team has {int:ActivePlayers} active players but only {int:MaxPlayers} players allowed.", NumPlayers),
				"ActivePlayers", &NumPlayers,
				"MaxPlayers", &MaxPlayers,
				nullptr);
			EndRound();
			return;
		}

		m_SurvivalState.Scores.Clear();
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			CIcPlayer *pPlayer = GetPlayer(i);
			if(!pPlayer || !pPlayer->GetCharacter())
				continue;

			if(pPlayer->GetClass() == EPlayerClass::None)
			{
				pPlayer->SetClass(ChooseHumanClass(pPlayer));
				pPlayer->SetRandomClassChoosen();
			}
			pPlayer->CloseMapMenu();

			EnsureSurvivalPlayerScore(pPlayer->GetCid());
		}

		const SurvivalWaveConfiguration *WaveConf = GetCurrentSurvivalWaveConfiguration();
		int TotalBotLives = WaveConf->GetTotalInfectedLives();
		if(TotalBotLives > 0 && !Config()->m_InfSurvivalMode)
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTION, _("Total infected lives for this round: {int:InfectedNumber}"),
				"InfectedNumber", &TotalBotLives,
				nullptr);
		}
	}
}

void CIcGameController::StartInfectionGameplay(int PlayersToInfect)
{
	InfectHumans(PlayersToInfect);

	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	while(Iter.Next())
	{
		CIcPlayer *pPlayer = Iter.Player();
		if(pPlayer->GetClass() == EPlayerClass::None)
		{
			pPlayer->SetClass(ChooseHumanClass(pPlayer));
			pPlayer->SetRandomClassChoosen();
			if(CIcCharacter *pCharacter = Iter.Player()->GetCharacter())
			{
				pCharacter->GiveRandomClassSelectionBonus();
			}
		}
		if(pPlayer->IsInfected() || pPlayer->IsInfectionStarted())
		{
			pPlayer->KillCharacter(); // Infect the player
			pPlayer->m_DieTick = m_RoundStartTick;
			continue;
		}
		else if(pPlayer->IsHuman())
		{
			// Ignore pPlayer->RandomClassChoosen()
			pPlayer->SetPreviouslyPickedClass(pPlayer->GetClass());
		}
	}
}

void CIcGameController::StartHideAndSeekGameplay(int SeekingPlayers)
{
	icArray<CIcPlayer *, MAX_CLIENTS> AllPlayers;

	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	while(Iter.Next())
		AllPlayers.Add(Iter.Player());

	const auto Sorter = [](const CIcPlayer *p1, const CIcPlayer *p2) -> bool {
		return p1->GetInfectionTimestamp() < p2->GetInfectionTimestamp();
	};
	std::stable_sort(AllPlayers.begin(), AllPlayers.end(), Sorter);

	int Timestamp = time_timestamp();
	for(CIcPlayer *pPlayer : AllPlayers)
	{
		if(SeekingPlayers > 0)
		{
			SetPlayerPickedTimestamp(pPlayer, Timestamp);

			const EPlayerClass NewClass = ChooseHumanClass(pPlayer);
			pPlayer->SetClass(NewClass);
			pPlayer->m_DieTick = m_RoundStartTick;
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTION,
				_("{str:VictimName} has been revived by the game"),
				"VictimName", Server()->ClientName(pPlayer->GetCid()),
				nullptr);

			--SeekingPlayers;
		}
		else
		{
			pPlayer->KillCharacter(); // Infect the player
			pPlayer->StartInfection();
			pPlayer->m_DieTick = m_RoundStartTick;
		}
	}

	m_HsFastRound = static_cast<int>(AllPlayers.Size()) > Config()->m_HsFastRoundMinPlayers;
	if(m_HsFastRound)
	{
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
			_("The humans should kill each Ghost at least once to win the round"),
			nullptr);
	}
	else
	{
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT,
			_("The humans should kill as many Ghosts as they can"),
			nullptr);
	}
}

void CIcGameController::MaybeSuggestMoreRounds()
{
	if(m_MoreRoundsSuggested)
		return;

	if(Config()->m_SvSuggestMoreRounds == 0)
		return;

	if(m_RoundCount != Config()->m_SvRoundsPerMap - 1)
		return;

	if(Config()->m_InfSurvivalMode)
		return;

	m_SuggestMoreRounds = true;
}

CGameWorld *CIcGameController::GameWorld()
{
	return &GameServer()->m_World;
}

void CIcGameController::StartRound()
{
	const bool StartAfterGameOver = IsGameOver();

	ERoundType NewRoundType = m_QueuedRoundType;
	if(NewRoundType == ERoundType::Invalid)
	{
		NewRoundType = GetDefaultRoundType();
	}

	m_RoundType = NewRoundType;
	QueueRoundType(ERoundType::Invalid);

	m_WinCheckEnabled.reset();
	m_VotesEnabled.reset();
	m_RoundMinimumPlayers.reset();
	m_RoundMinimumInfected.reset();
	m_RoundTimeLimitSeconds.reset();
	m_RoundInfectionDelaySeconds.reset();

	RemoveBots();
	for(auto EnabledPoints : m_EnabledSpawnPoints)
		EnabledPoints.clear();

	switch(GetRoundType())
	{
	case ERoundType::Normal:
		break;
	case ERoundType::Fun:
	{
		if(m_FunRoundConfigurations.empty())
		{
			m_RoundType = ERoundType::Normal;
			break;
		}
		StartFunRound();
	}
	break;
	case ERoundType::Fast:
		GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE);
		GameServer()->SendChatTarget(-1, "Starting the 'fast' round. Good luck everyone!");
		break;
	case ERoundType::Survival:
	{
		if(m_SurvivalConfiguration.SurvivalWaves.IsEmpty())
		{
			m_RoundType = ERoundType::Normal;
			GameServer()->SendChatTarget(-1, "Failed to start survival round: the round is not configured");
		}
		break;
	}
	case ERoundType::HideAndSeek:
		StartHideAndSeekRound();
		break;
	case ERoundType::Invalid:
		break;
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GetPlayer(i))
		{
			Server()->SetClientMemory(i, CLIENTMEMORY_ROUNDSTART_OR_MAPCHANGE, true);
		}
	}

	m_RoundStarted = true;
	IGameController::StartRound();

	if(GetRoundType() == ERoundType::Survival)
	{
		m_WaveStartTick = Server()->Tick();
	}

	if(StartAfterGameOver)
	{
		if(!HardMode())
		{
			for(int CID : m_SurvivalState.KilledPlayers)
			{
				CIcPlayer *pPlayer = GetPlayer(CID);
				if(pPlayer->IsSpectator() && !pPlayer->IsBot())
				{
					DoTeamChange(pPlayer, TEAM_RED, false);
				}
			}

			m_SurvivalState.KilledPlayers.Clear();

			if(Config()->m_InfSurvivalMode)
			{
				for(int CID = 0; CID < MAX_CLIENTS; ++CID)
				{
					CIcPlayer *pPlayer = GetPlayer(CID);
					if(pPlayer && pPlayer->IsSpectator() && !pPlayer->IsBot())
					{
						// Reset the class for other (not recently killed) spectator players
						pPlayer->SetClass(EPlayerClass::None);
					}
				}
			}
		}

		IncreaseCurrentRoundCounter();
	}

	if(GetRoundType() == ERoundType::Survival)
	{
		if(m_TriggerSurvivalAutostart)
		{
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "Survival auto-restarted");
			m_TriggerSurvivalAutostart = false;
			PrepareSurvival();
		}
		else if(Config()->m_InfSurvivalMode)
		{
			if(StartAfterGameOver)
			{
				++m_SurvivalState.Wave;
			}
		}
		else
		{
			PrepareSurvival();
		}

		StartSurvivalRound();
	}
	else
	{
		ResetRoundData();
	}

	RunCallback(Lua()->GetLuaState(), "on_round_started", toString(GetRoundType()));

	SaveRoundRules();
	OnStartRound();
}

void CIcGameController::ResetRoundData()
{
	Server()->ResetStatistics();
	CBotPlayer::OnNewRound();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(CIcPlayer *pPlayer = GetPlayer(i))
		{
			pPlayer->ResetRoundData();
		}
	}

	m_HeroGiftTick = 0;
	m_WitchCallers.Clear();
}

void CIcGameController::EndRound()
{
	// The EndRound() override is called only from the IGameController on map skipped or changed
	EndRound(ERoundEndReason::CANCELED);
}

void CIcGameController::EndRound(ERoundEndReason Reason)
{
	int NumHumans = 0;
	int NumInfected = 0;
	{
		GetPlayerCounter(-1, NumHumans, NumInfected);

		const char *pWinnerTeam = Reason == ERoundEndReason::FINISHED ? (NumHumans > 0 ? "humans" : "zombies") : "none";
		const char *pRoundType = toString(GetRoundType());

		// Win check
		const int Seconds = (Server()->Tick() - m_RoundStartTick) / static_cast<float>(Server()->TickSpeed());

		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "round_end winner='%s' survivors='%d' duration='%d' round='%d of %d' type='%s'",
			pWinnerTeam,
			NumHumans, Seconds, m_RoundCount + 1, Config()->m_SvRoundsPerMap, pRoundType);
		Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}
	ResetFinalExplosion();
	IGameController::EndRound();
	RunCallback(Lua()->GetLuaState(), "on_round_end", toString(Reason));

	if(Reason == ERoundEndReason::FINISHED)
		Server()->OnRoundIsOver();
	else if(Reason == ERoundEndReason::CANCELED)
	{
		if(!m_InfectedStarted || (NumHumans + NumInfected) < 2)
		{
			m_GameOverTick = 0;
		}
	}

	m_InfectedStarted = false;

	switch(GetRoundType())
	{
	case ERoundType::Normal:
	case ERoundType::Fast:
		break;
	case ERoundType::Fun:
		EndFunRound();
		break;
	case ERoundType::Survival:
		EndSurvivalRound(Reason);
		break;
	case ERoundType::HideAndSeek:
		EndHideAndSeekRound();
		break;
	case ERoundType::Invalid:
		break;
	}

	m_RoundStarted = false;
}

void CIcGameController::DoTeamChange(CPlayer *pBasePlayer, int Team, bool DoChatMsg)
{
	CIcPlayer *pPlayer = CIcPlayer::GetInstance(pBasePlayer);
	Team = ClampTeam(Team);
	if(Team == pPlayer->GetTeam())
		return;

	IGameController::DoTeamChange(pPlayer, Team, false);

	const int ClientId = pPlayer->GetCid();

	if(DoChatMsg)
	{
		if(Team == TEAM_SPECTATORS)
		{
			if(Config()->m_InfSurvivalRespawn)
				pPlayer->SetSurvivalRespawnTick(Server()->Tick() + Server()->TickSpeed() * Config()->m_InfSurvivalRespawnDelay);
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER, _("{str:PlayerName} joined the spectators"), "PlayerName", Server()->ClientName(ClientId), nullptr);
		}
		else
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER, _("{str:PlayerName} joined the game"), "PlayerName", Server()->ClientName(ClientId), nullptr);
		}
	}

	if(Team != TEAM_SPECTATORS)
	{
		PreparePlayerToJoin(pPlayer);
	}
}

void CIcGameController::GetPlayerCounter(int ClientException, int &NumHumans, int &NumInfected)
{
	NumHumans = 0;
	NumInfected = 0;

	// Count type of players
	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	while(Iter.Next())
	{
		if(Iter.ClientId() == ClientException)
			continue;

		if(Iter.Player()->IsInfected())
			NumInfected++;
		else if(!Iter.Player()->IsBot())
			NumHumans++;
	}
}

int CIcGameController::GetMinimumInfectedForPlayers(int PlayersNumber) const
{
	if(m_RoundMinimumInfected.has_value())
		return m_RoundMinimumInfected.value();

	if(GetRoundType() == ERoundType::Fast)
	{
		//  7 | 3 vs 4 | 3.01
		//  8 | 3 vs 5 | 3.44
		//  9 | 3 vs 6 | 3.87
		// 10 | 4 vs 6 | 4.30
		// 11 | 4 vs 7 | 4.73
		// 12 | 5 vs 7 | 5.16
		return maximum(1, static_cast<int>(PlayersNumber * 0.43));
	}

	int InitialPlayersLimit = Config()->m_InfFirstInfectedLimit;
	if(GetRoundType() == ERoundType::HideAndSeek)
		InitialPlayersLimit = Config()->m_HsMedicsLimit;
	int NumFirstInfected = 0;

	if(PlayersNumber > 20)
		NumFirstInfected = 4;
	else if(PlayersNumber > 8)
		NumFirstInfected = 3;
	else if(PlayersNumber > 3)
		NumFirstInfected = 2;
	else if(PlayersNumber > 1)
		NumFirstInfected = 1;
	else
		NumFirstInfected = 0;

	if(InitialPlayersLimit && NumFirstInfected > InitialPlayersLimit)
	{
		NumFirstInfected = InitialPlayersLimit;
	}

	return NumFirstInfected;
}

int CIcGameController::InfectedBonusArmor() const
{
	const float Factor = clamp<float>(m_InfBalanceBoostFactor, 0, 1);
	return Factor * 10;
}

void CIcGameController::SendKillMessage(int Victim, const DeathContext &Context)
{
	EDamageType DamageType = Context.DamageType;
	int VanillaWeapon = DamageTypeToWeapon(DamageType);
	int Killer = Context.Killer;
	const int Assistant = Context.Assistant;

	if(Killer < 0)
	{
		Killer = Victim;
	}

	if((Killer != Victim) && (VanillaWeapon < 0))
	{
		VanillaWeapon = WEAPON_NINJA;
	}

	// Old clients have no idea about DAMAGE_TILEs,
	// and we don't need a different UI indication
	if(Context.DamageType == EDamageType::DAMAGE_TILE)
	{
		DamageType = EDamageType::DEATH_TILE;
	}

	// Substitute the weapon for clients for better UI icon
	if(DamageType == EDamageType::DEATH_TILE)
		VanillaWeapon = WEAPON_NINJA;

	const int DamageTypeInt = static_cast<int>(DamageType);
	dbg_msg("inf-proto", "Sent kill message victim=%d damage_type=%s killer=%d assistant=%d", Victim, toString(DamageType), Killer, Assistant);

	CNetMsg_Inf_KillMsg InfClassMsg;
	InfClassMsg.m_Killer = Killer;
	InfClassMsg.m_Victim = Victim;
	InfClassMsg.m_Assistant = Assistant;
	InfClassMsg.m_InfDamageType = DamageTypeInt;
	InfClassMsg.m_Weapon = VanillaWeapon;

	CMsgPacker InfCPacker(InfClassMsg.ms_MsgId, false);
	InfClassMsg.Pack(&InfCPacker);

	CNetMsg_Sv_KillMsg VanillaMsg;
	VanillaMsg.m_Killer = Killer;
	VanillaMsg.m_Victim = Victim;
	VanillaMsg.m_Weapon = VanillaWeapon;
	VanillaMsg.m_ModeSpecial = InfClassModeSpecialSkip;

	CMsgPacker VanillaPacker(VanillaMsg.ms_MsgId, false);
	VanillaMsg.Pack(&VanillaPacker);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(Server()->ClientIngame(i))
		{
			IServer::CClientInfo Info;
			Server()->GetClientInfo(i, &Info);

			if(Info.m_InfClassVersion)
			{
				Server()->SendMsg(&InfCPacker, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
			}
			Server()->SendMsg(&VanillaPacker, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
		}
	}

	Server()->SendMsg(&InfCPacker, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);
	Server()->SendMsg(&VanillaPacker, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);

	if(VanillaWeapon != WEAPON_GAME)
	{
		CIcPlayer *pKiller = GetPlayer(Killer);
		CIcPlayer *pVictim = GetPlayer(Victim);
		CIcPlayer *pAssistant = GetPlayer(Assistant);
		if(pKiller && (pKiller != pVictim))
			pKiller->OnKill();
		if(pVictim)
			pVictim->OnDeath();
		if(pAssistant && (pAssistant != pVictim))
			pAssistant->OnAssist();
	}

	OnKillOrInfection(Victim, Context);
}

std::optional<int> CIcGameController::GetClientIdByName(const char *pName) const
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(str_comp(pName, GameServer()->Server()->ClientName(i)) == 0)
		{
			return i;
		}
	}
	return std::nullopt;
}

void CIcGameController::OnKillOrInfection(int Victim, const DeathContext &Context)
{
	if(GetRoundType() != ERoundType::Survival)
		return;

	const int VanillaWeapon = DamageTypeToWeapon(Context.DamageType);

	if(VanillaWeapon == WEAPON_GAME)
		return;

	CIcPlayer *pVictim = GetPlayer(Victim);
	const CIcPlayer *pKiller = Context.Killer < 0 ? pVictim : GetPlayer(Context.Killer);
	const int Tick = Server()->Tick();
	const int TickSpeed = Server()->TickSpeed();

	if(pKiller && pKiller->IsHuman() && (pVictim != pKiller))
	{
		m_SurvivalState.Kills++;
	}

	if(!pVictim || pVictim->IsBot() || !pVictim->IsHuman())
		return;

	if(Config()->m_InfSurvivalRespawn)
		pVictim->SetSurvivalRespawnTick(Tick + TickSpeed * Config()->m_InfSurvivalRespawnDelay);

	if(!m_SurvivalState.KilledPlayers.Contains(Victim))
	{
		m_SurvivalState.KilledPlayers.Add(Victim);
	}

	const CIcCharacter *pVictimCharacter = pVictim->GetCharacter();
	const CIcCharacter *pKillerCharacter = pKiller ? pKiller->GetCharacter() : nullptr;
	if(pKiller && pKiller != pVictim)
	{
		ETextArticle Article = ETextArticle::Indefinite;
		const EPlayerClass KillerClass = pKiller->GetClass();

		switch(KillerClass)
		{
		case EPlayerClass::Witch:
		case EPlayerClass::Undead:
		case EPlayerClass::Tank:
		{
			const SurvivalWaveConfiguration *pCurrentWave = GetCurrentSurvivalWaveConfiguration();
			if(pCurrentWave)
			{
				const auto KillerClassCounter = [KillerClass](const SurvivalBotConfiguration &BotConfig) {
					return BotConfig.Class == KillerClass;
				};
				int Count = std::count_if(pCurrentWave->BotConfigurations.begin(), pCurrentWave->BotConfigurations.end(), KillerClassCounter);
				if(Count == 1)
				{
					Article = ETextArticle::Definite;
				}
			}
			break;
		}
		default:
			break;
		}

		icArray<const char *, 20> PossibleMessages;
		switch(Context.DamageType)
		{
		case EDamageType::MERCENARY_BOMB:
			PossibleMessages.Add(_("{str:PlayerName} got to know their bomb too closely."));
			PossibleMessages.Add(_("{str:PlayerName} rode the blast wave for the last time."));
			break;
		case EDamageType::BOOMER_EXPLOSION:
			PossibleMessages.Add(("{str:PlayerName} was evaporated by {str:Killer}."));
			PossibleMessages.Add(_("{str:PlayerName} was exploded by {str:Killer}."));
			PossibleMessages.Add(_("{str:PlayerName} was eliminated by {str:Killer}."));
			PossibleMessages.Add(_("{str:PlayerName} met a boomer."));
			if(pVictimCharacter && pKillerCharacter)
			{
				if(distance(pVictimCharacter->GetPos(), pKillerCharacter->GetPos()) < TileSizeF * 2.f)
				{
					PossibleMessages.Add(("{str:PlayerName} hugged a boomer."));
				}
			}
			break;
		case EDamageType::SLUG_SLIME:
			PossibleMessages.Add(_("{str:PlayerName} had no aid against {str:Killer}."));
			break;
		case EDamageType::SCIENTIST_MINE:
			PossibleMessages.Add(_("{str:PlayerName} was electrified by {str:Killer}."));
			break;
		case EDamageType::DRYING_HOOK:
			if(pVictimCharacter->Core()->m_AttachedPlayers.size() >= 2)
			{
				constexpr float StretchingDistance = 12 * TileSizeF;
				constexpr float StretchingDistance2 = StretchingDistance * StretchingDistance;
				int Stretchers = 0;
				for(const auto &AttachedPlayerId : pVictimCharacter->Core()->m_AttachedPlayers)
				{
					const CCharacter *pOtherPlayer = GameServer()->GetPlayerChar(AttachedPlayerId);
					if(pOtherPlayer && distance_squared(pOtherPlayer->GetPos(), pVictimCharacter->GetPos()) > StretchingDistance2)
					{
						Stretchers++;
						if(Stretchers >= 2)
							break;
					}
				}

				if(Stretchers >= 2)
				{
					PossibleMessages.Clear();
					PossibleMessages.Add(_("{str:PlayerName} did a stretching exercise."));
					PossibleMessages.Add(_("{str:PlayerName} was torn apart."));
				}
				break;
			}
			break;
		default:
			break;
		}

		switch(KillerClass)
		{
		case EPlayerClass::Smoker:
			if(Context.DamageType == EDamageType::DRYING_HOOK)
			{
				PossibleMessages.Add(_("{str:PlayerName} was drained by {str:Killer}."));
				PossibleMessages.Add(("{str:PlayerName} was smoked out by {str:Killer}."));
			}
			break;
		case EPlayerClass::Ghost:
			PossibleMessages.Add(_("{str:PlayerName} was surprised by {str:Killer}."));
			PossibleMessages.Add(("{str:PlayerName} was been spirited away by {str:Killer}."));
			PossibleMessages.Add(("{str:PlayerName} was dematerialized by ghostly shenanigans!"));
			PossibleMessages.Add(("Boo! {str:PlayerName} was scared to death!"));
			break;
		case EPlayerClass::Bat:
			PossibleMessages.Add(_("{str:PlayerName} was bitten by {str:Killer}."));
			break;
		default:
			break;
		}

		const CIcCharacter *pVictimCharacter = pVictim->GetCharacter();

		static const icArray<EDamageType, 2> aHopelessDamageTypes = {
			EDamageType::SLUG_SLIME,
			EDamageType::BOOMER_EXPLOSION,
		};
		if(!aHopelessDamageTypes.Contains(Context.DamageType))
		{
			if(pVictimCharacter->GetAttackTick() + TickSpeed * 1.25f < Tick)
			{
				PossibleMessages.Add(_("{str:PlayerName} kinda gave up."));
				PossibleMessages.Add(_("{str:PlayerName} was too exhausted for this fight."));
			}
			else if(pVictimCharacter->GetLastNoAmmoSoundTick() + Server()->TickSpeed() * 0.6 < Tick)
			{
				static const icArray<int, 2> aWeaponsWithoutReload = {
					WEAPON_HAMMER,
					WEAPON_NINJA,
				};

				if(!aWeaponsWithoutReload.Contains(pVictimCharacter->GetActiveWeapon()))
				{
					PossibleMessages.Add(_("{str:PlayerName} had no ammo to kill them all."));
					PossibleMessages.Add(_("{str:PlayerName} had no ammo to reload."));
				}
			}
		}

		const char *apPlayerKilledByMessages[] = {
			_("{str:PlayerName} was destroyed by {str:Killer}."),
			_("{str:PlayerName} was wrecked by {str:Killer}."),
			_("{str:PlayerName} was slain by {str:Killer}."),
			_("{str:PlayerName} was decapitated by {str:Killer}."),
			_("{str:PlayerName} was chopped up by {str:Killer}."),
			_("{str:PlayerName} was removed from this world by {str:Killer}."),
		};

		if(PossibleMessages.IsEmpty())
		{
			for(const char *pMessage : apPlayerKilledByMessages)
			{
				PossibleMessages.Add(pMessage);
			}
		}
		else
		{
			PossibleMessages.Add(apPlayerKilledByMessages[random_int(0, std::size(apPlayerKilledByMessages) - 1)]);
		}

		if(PossibleMessages.Size() > 1)
		{
			for(std::size_t i = 0; i < PossibleMessages.Size(); ++i)
			{
				if(PossibleMessages.At(i) == m_LastUsedKillMessage)
				{
					PossibleMessages.RemoveAt(i);
				}
			}
		}

		const char *pMessage = PossibleMessages.At(random_int(0, PossibleMessages.Size() - 1));
		m_LastUsedKillMessage = pMessage;

		const char *pKillerText = GetClassDisplayNameForKilledBy(KillerClass, Article);
		dbg_assert(pKillerText, "Killer class has no display name");
		if(!pKillerText)
		{
			return;
		}
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER,
			pMessage,
			"PlayerName", GameServer()->Server()->ClientName(Victim),
			"Killer", pKillerText,
			nullptr);
	}
	else
	{
		icArray<const char *, 20> PossibleMessages;

		constexpr icArray<EDamageType, 2> BadTiles = {
			EDamageType::DEATH_TILE,
			EDamageType::INFECTION_TILE,
		};
		if(BadTiles.Contains(Context.DamageType))
		{
			PossibleMessages.Add(_("{str:PlayerName} made a wrong step."));
			PossibleMessages.Add(_("{str:PlayerName} went where they shouldn't."));
		}

		const char *apPlayerDeathMessages[] = {
			_("{str:PlayerName} didn't survive in this round."),
		};

		if(PossibleMessages.IsEmpty())
		{
			for(const char *pMessage : apPlayerDeathMessages)
			{
				PossibleMessages.Add(pMessage);
			}
		}
		else
		{
			PossibleMessages.Add(apPlayerDeathMessages[random_int(0, std::size(apPlayerDeathMessages) - 1)]);
		}

		const char *pMessage = PossibleMessages.At(random_int(0, PossibleMessages.Size() - 1));
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_PLAYER,
			pMessage,
			"PlayerName", GameServer()->Server()->ClientName(Victim),
			nullptr);
	}
}

int CIcGameController::GetClientIdForNewWitch() const
{
	ClientsArray SuitableInfected;
	ClientsArray SafeInfected;

	for(int ClientId : m_WitchCallers)
	{
		const CIcPlayer *pPlayer = GetPlayer(ClientId);
		if(!pPlayer || !pPlayer->IsInGame())
			continue;
		if(pPlayer->GetClass() == EPlayerClass::Witch)
			continue;
		if(!pPlayer->IsInfected())
			continue;

		SuitableInfected.Add(ClientId);

		if(!IsSafeWitchCandidate(ClientId))
			continue;

		SafeInfected.Add(ClientId);
	}

	if(SuitableInfected.IsEmpty())
	{
		// fallback
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			const CIcPlayer *pPlayer = GetPlayer(ClientId);
			if(!pPlayer || !pPlayer->IsInGame())
				continue;
			if(pPlayer->GetClass() == EPlayerClass::Witch)
				continue;
			if(!pPlayer->IsInfected())
				continue;

			SuitableInfected.Add(ClientId);

			if(!IsSafeWitchCandidate(ClientId))
				continue;

			SafeInfected.Add(ClientId);
		}
	}

	if(SuitableInfected.IsEmpty())
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "witch", "Unable to find any suitable player");
		return -1;
	}

	const ClientsArray &Candidates = SafeInfected.IsEmpty() ? SuitableInfected : SafeInfected;
	const int id = random_int(0, Candidates.Size() - 1);
	char aBuf[512];
	/* debug */
	str_format(aBuf, sizeof(aBuf), "going through MAX_CLIENTS=%d, zombie_count=%d, random_int=%d, id=%d", MAX_CLIENTS, static_cast<int>(SuitableInfected.Size()), id, SuitableInfected[id]);
	Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "witch", aBuf);
	/* /debug */
	CIcPlayer *pPlayer = GetPlayer(Candidates[id]);
	pPlayer->SetClass(EPlayerClass::Witch);
	return Candidates[id];
}

bool CIcGameController::IsSafeWitchCandidate(int ClientId) const
{
	constexpr double MaxInactiveSeconds = 5;
	constexpr double SafeRadius = 1000;

	const CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
		return false;

	if(Server()->Tick() > pPlayer->m_LastActionTick + MaxInactiveSeconds * Server()->TickSpeed())
		return false;

	const CIcCharacter *pCharacter = GetCharacter(ClientId);
	if(pCharacter && pCharacter->IsAlive())
	{
		icArray<CIcCharacter *, MAX_CLIENTS> aCharsNearby;
		const int Num = GameServer()->m_World.FindEntities(pCharacter->GetPos(), SafeRadius,
			reinterpret_cast<CEntity **>(aCharsNearby.begin()),
			aCharsNearby.Capacity(),
			CGameWorld::ENTTYPE_CHARACTER);
		aCharsNearby.Resize(Num);

		for(const CIcCharacter *pCharNearby : aCharsNearby)
		{
			if(pCharNearby == pCharacter)
				continue;

			if(pCharNearby->IsAlive() && pCharNearby->IsHuman())
			{
				return false;
			}
		}
	}

	return true;
}

void CIcGameController::RemoveBots()
{
	while(!m_Bots.IsEmpty())
	{
		CBaseBotPlayer *pBot = m_Bots.Last();
		RemoveBot(pBot, "Cleanup");
	}

	SpawnedBotsTracker.ResetSpawnedBotsTracking();
}

int CIcGameController::RequestBotID()
{
	for(int i = MAX_CLIENTS - 1; i > 0; --i)
	{
		if(GameServer()->m_apPlayers[i])
			continue;

		if(Server()->NewBot(i) != 0)
			continue;

		return i;
	}

	return -1;
}

CBaseBotPlayer *CIcGameController::AddBot(int Team)
{
	if(m_Bots.Size() == MaxBots - 1)
	{
		// dbg_msg("bots", "AddBot(): Max bots number reached");
		return nullptr;
	}

	if(IsGameOver())
		return nullptr;

	const int PlayerID = RequestBotID();

	if(PlayerID < 0)
	{
		// dbg_msg("bots", "AddBot(): No slots");
		return nullptr;
	}

	// dbg_msg("bots", "AddBot(): New bot with ID %d", PlayerID);

	CBotPlayer *pPlayer = new(PlayerID) CBotPlayer(this, GetNextClientUniqueId(), PlayerID, Team);

	if(!pPlayer)
		return nullptr;

	pPlayer->SetBotUtilsData(*m_pBotUtilsData);
	GameServer()->m_apPlayers[PlayerID] = pPlayer;

	EPlayerClass PlayerClass = EPlayerClass::Bat;
	pPlayer->SetClass(PlayerClass);

	m_Bots.Add(pPlayer);

	pPlayer->UpdateName();

	return pPlayer;
}

CBaseBotPlayer *CIcGameController::AddBot(const SurvivalBotConfiguration &Configuration)
{
	int Team = 0;
	if(Configuration.SpawnMinTick > GetInfectionTick())
	{
		Team = TEAM_SPECTATORS;
	}

	int MaxLives = Configuration.Lives;
	const bool KillBased = Config()->m_InfSurvivalMode != SURVIVAL_MODE_TIME_BASED;
	if(!MaxLives && KillBased && (Config()->m_InfBotLives > 0))
	{
		MaxLives = Config()->m_InfBotLives;
	}

	CBaseBotPlayer *pBot = AddBot(Team);
	if(!pBot)
		return nullptr;

	// Make the bots spawn a bit less predictable
	pBot->m_RespawnTick += Server()->TickSpeed() * random_float();

	pBot->SetClass(Configuration.Class);
	pBot->SetSpawnMinTick(Configuration.SpawnMinTick);
	pBot->SetMaxLives(MaxLives);
	pBot->SetMaxHP(Configuration.HP);
	pBot->SetDropLevel(Configuration.DropLevel);
	pBot->SetRespawnInterval(Configuration.RespawnInterval);
	pBot->SetTweaks(Configuration.Tweaks);
	pBot->SetTag(Configuration.Tag);
	pBot->UpdateName();

	return pBot;
}

CBaseBotPlayer *CIcGameController::AddBot_Lua(const char *pClass)
{
	EPlayerClass Class = GetClassByName(pClass);
	if(Class == EPlayerClass::Invalid)
		return nullptr;

	CBaseBotPlayer *pPlayer = AddBot();
	if(!pPlayer)
	{
		return nullptr;
	}

	pPlayer->SetClass(Class);

	return m_Bots.Last();
}

CBaseBotPlayer *CIcGameController::GetBot(int ClientId)
{
	CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer || !pPlayer->IsBot())
	{
		return nullptr;
	}

	CBaseBotPlayer *pBot = static_cast<CBaseBotPlayer *>(pPlayer);
	return pBot;
}

bool CIcGameController::RemoveBot(CBaseBotPlayer *pBot, const char *pReason)
{
	std::optional<std::size_t> BotIndex = m_Bots.IndexOf(pBot);
	if(!BotIndex.has_value())
	{
		return false;
	}

	int ClientId = pBot->GetCid();
	dbg_msg("bots", "Remove bot (CID: %d, Reason: %s)", ClientId, pReason);
	GameServer()->OnClientDrop(ClientId, EClientDropType::Kick, pReason);
	Server()->DelBot(ClientId);

	if(m_Bots.Contains(pBot))
	{
		dbg_msg("bot", "Disconnected bot (id %d) is not in the bots list", pBot->GetCid());
	}

	return true;
}

bool CIcGameController::RemoveBot(int ClientId, const char *pReason)
{
	return RemoveBot(GetBot(ClientId), pReason);
}

bool CIcGameController::RemoveBot_Lua(int ClientId)
{
	return RemoveBot(ClientId);
}

void CIcGameController::RegisterBotsContext()
{
	static CCollisionWrapper Collision;
	Collision.SetGameContext(GameServer());
	static CGameDebugSink DebugSink;
	DebugSink.SetGameContext(GameServer());
	static CCollisionCache Cache;
	static CBotUtilsSharedData UtilsData;
	UtilsData.m_pCollision = &Collision;
	UtilsData.m_pDebugSink = &DebugSink;
	UtilsData.m_pCache = &Cache;

	int Width = Collision.GameLayerWidth();
	int Height = Collision.GameLayerHeight();
	Cache.m_AirTilesAboveCache.Reset(Width, Height);

	m_pBotUtilsData = &UtilsData;
}

void CIcGameController::AddMoreBotsAccordingToConfiguration()
{
	const SurvivalWaveConfiguration *WaveConf = GetCurrentSurvivalWaveConfiguration();
	const int InfectionTick = GetInfectionTick();
	int SpawnAheadTicks = Server()->TickSpeed() * 3;
	for(std::size_t BotIndex = SpawnedBotsTracker.GetFirstBotIndex(); BotIndex < WaveConf->BotConfigurations.Size(); ++BotIndex)
	{
		if(SpawnedBotsTracker.IsBotSpawned(BotIndex))
			continue;

		const SurvivalBotConfiguration &BotConf = WaveConf->BotConfigurations[BotIndex];
		if(BotConf.SpawnMinTick < InfectionTick + SpawnAheadTicks)
		{
			CBaseBotPlayer *pBot = AddBot(BotConf);
			if(!pBot)
			{
				break;
			}
			pBot->SetBotConfigId(BotIndex);
			SpawnedBotsTracker.MarkSpawned(BotIndex);
		}
	}
}

CIcGameController::PlayerScore *CIcGameController::GetSurvivalPlayerScore(int ClientId)
{
	for(PlayerScore &Score : m_SurvivalState.Scores)
	{
		if(Score.ClientId == ClientId)
			return &Score;
	}

	return nullptr;
}

CIcGameController::PlayerScore *CIcGameController::EnsureSurvivalPlayerScore(int ClientId)
{
	if(PlayerScore *pScore = GetSurvivalPlayerScore(ClientId))
		return pScore;

	m_SurvivalState.Scores.Add({});
	PlayerScore &Score = m_SurvivalState.Scores.Last();
	Score.ClientId = ClientId;
	Score.aPlayerName[0] = '\0';
	Score.Kills = 0;

	return &Score;
}

const SurvivalWaveConfiguration *CIcGameController::GetCurrentSurvivalWaveConfiguration() const
{
	if(m_SurvivalConfiguration.SurvivalWaves.Size() <= m_SurvivalState.Wave)
	{
		static const SurvivalWaveConfiguration EmptyConfig;
		return &EmptyConfig;
	}

	const SurvivalWaveConfiguration &WaveConf = m_SurvivalConfiguration.SurvivalWaves.At(m_SurvivalState.Wave);
	return &WaveConf;
}

const SurvivalGameConfiguration *CIcGameController::SurvivalGetGameConfiguration() const
{
	return &m_SurvivalConfiguration;
}

SurvivalWaveConfiguration *CIcGameController::SurvivalGetWaveConfiguration(int WaveIndex)
{
	if(WaveIndex < 0)
		return nullptr;

	SurvivalGameConfiguration *pGameConfig = SurvivalGetMutableGameConfiguration();
	const auto Index = static_cast<std::size_t>(WaveIndex);
	if(Index >= pGameConfig->SurvivalWaves.Size())
		return nullptr;

	return &pGameConfig->SurvivalWaves[Index];
}

SurvivalGameConfiguration *CIcGameController::SurvivalGetMutableGameConfiguration()
{
	return &m_SurvivalConfiguration;
}

void CIcGameController::TickBeforeWorld()
{
	if(GameWorld()->m_Paused)
		return;

	// update core properties important for hook
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(CIcCharacter *pCharacter = GetCharacter(i))
		{
			pCharacter->TickBeforeWorld();
			m_Teams.m_Core.SetProtected(i, pCharacter->GetPlayer()->HookProtectionEnabled());
		}
	}

	for(CBaseBotPlayer *pBot : m_Bots)
	{
		pBot->UpdateControls();
	}
}
void CIcGameController::Tick()
{
	IGameController::Tick();

	// Check session
	{
		CIcPlayerIterator<PLAYERITER_ALL> Iter(GameServer()->m_apPlayers);
		while(Iter.Next())
		{
			// Update session
			if(IServer::CClientSession *pSession = Server()->GetClientSession(Iter.ClientId()))
			{
				if(!Server()->GetClientMemory(Iter.ClientId(), CLIENTMEMORY_SESSION_PROCESSED))
				{
					// The client already participated to this round,
					// and he exit the game as infected.
					// To avoid cheating, we assign to him the same class again.
					if(pSession->m_RoundId == m_RoundId)
					{
						const EPlayerClass ClassFromSession = static_cast<EPlayerClass>(pSession->m_Class);
						if(IsInfectedClass(ClassFromSession) == IsInfectionStarted())
						{
							Iter.Player()->SetClass(ClassFromSession);
							Iter.Player()->SetInfectionTimestamp(pSession->m_LastInfectionTime);
						}
					}

					Server()->SetClientMemory(Iter.ClientId(), CLIENTMEMORY_SESSION_PROCESSED, true);
				}

				pSession->m_Class = static_cast<int>(Iter.Player()->GetClass());
				pSession->m_RoundId = GameServer()->m_pController->GetRoundId();
				pSession->m_LastInfectionTime = Iter.Player()->GetInfectionTimestamp();
			}
		}
	}

	CheckRoundFailed();

	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	const int NumPlayers = NumHumans + NumInfected;

	bool Allowed = !m_Warmup;
	if(Config()->m_InfSurvivalMode)
	{
		if(GetRoundType() != ERoundType::Survival)
		{
			// The round is not started yet
			Allowed = false;

			GameServer()->SendBroadcast_Localization(-1,
				EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE,
				_("Vote to start the game"), nullptr);
		}

		if(Allowed)
		{
			CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
			while(Iter.Next())
			{
				const CIcPlayer *pPlayer = Iter.Player();
				if((m_SurvivalState.Wave > 0) && !m_SurvivalState.SurvivedPlayers.Contains(pPlayer->GetCid()))
				{
					continue;
				}
#if 0
				if(pPlayer->MapMenu())
				{
					// One of the players didn't choose the class yet
					Allowed = false;

					GameServer()->SendBroadcast_Localization(-1,
						EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE,
						_("Waiting for players to choose a class"), nullptr);
					break;
				}
#endif
			}
		}
		// Move infected players to spec
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			CIcPlayer *pPlayer = GetPlayer(i);

			if(pPlayer && !pPlayer->IsBot() && pPlayer->IsInfected())
			{
				if(!Allowed)
				{
					pPlayer->KillCharacter();
					pPlayer->SetClass(EPlayerClass::None);
					continue;
				}
				const bool DoChatMsg = false;
				DoTeamChange(pPlayer, TEAM_SPECTATORS, DoChatMsg);
			}
		}
	}

	if(Config()->m_InfSurvivalRespawn)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			CIcPlayer *pPlayer = GetPlayer(i);
			if(!pPlayer)
				continue;

			if(GetRoundType() == ERoundType::Survival && pPlayer->GetTeam() == TEAM_SPECTATORS && !pPlayer->IsBot())
			{
				if(Server()->Tick() > pPlayer->GetSurvivalRespawnTick())
				{
					GameServer()->SendBroadcast_Localization(pPlayer->GetCid(),
						EBroadcastPriority::GAMEANNOUNCE,
						BROADCAST_DURATION_GAMEANNOUNCE,
						_("You can respawn via '/respawn <player name>' now"), nullptr);
				}
				else
				{
					int Seconds = (pPlayer->GetSurvivalRespawnTick() - Server()->Tick()) / Server()->TickSpeed();
					GameServer()->SendBroadcast_Localization(pPlayer->GetCid(),
						EBroadcastPriority::GAMEANNOUNCE,
						BROADCAST_DURATION_GAMEANNOUNCE,
						_("You can respawn after {sec:Time}"), "Time", &Seconds);
				}
			}
		}
	}
	m_InfectedStarted = false;

	// If the game can start ...
	if(Allowed && m_GameOverTick == -1 && NumPlayers >= GetMinPlayers())
	{
		// If the infection started
		if(IsInfectionStarted())
		{
			m_InfectedStarted = true;
			RoundTickAfterInitialInfection();
		}
		else
		{
			RoundTickBeforeInitialInfection();
		}

		const int CurrentTick = Server()->Tick();
		const int BotRemoveDelay = Server()->TickSpeed() * Config()->m_InfBotRemoveDelay;
		const auto Bots = m_Bots;
		for(CBaseBotPlayer *pBot : Bots)
		{
			if((pBot->Lives() == 0) && (CurrentTick > pBot->DieTick() + BotRemoveDelay))
			{
				RemoveBot(pBot, "Rage quit");
			}
		}

		DoWincheck();
		if(m_FinalExplosionState == EFinalExplosionState::Started)
		{
			ProgressFinalExplosion();
		}
	}
	else
	{
		m_RoundStartTick = Server()->Tick();
		if(GetRoundType() == ERoundType::Survival)
		{
			m_WaveStartTick = Server()->Tick();
		}
	}

	if(GameWorld()->m_Paused)
	{
		m_HeroGiftTick++;
		m_WaveStartTick++;
	}
	else
	{
		UpdateNinjaTargets();
		HandleLastHookers();
	}

	if(m_SuggestMoreRounds && !GameServer()->HasActiveVote())
	{
		constexpr char pDescription[] = _("Play more on this map");
		char aCommandBuffer[256];
		str_format(aCommandBuffer, sizeof(aCommandBuffer), "adjust sv_rounds_per_map +%d", Config()->m_SvSuggestMoreRounds);
		constexpr char pReason[] = _("The last round");

		GameServer()->StartVote(pDescription, aCommandBuffer, pReason);

		m_SuggestMoreRounds = false;
		m_MoreRoundsSuggested = true;
	}

	if(NumPlayers)
		SendHintMessage();

	RunCallback(Lua()->GetLuaState(), "on_tick");
}

void CIcGameController::RoundTickBeforeInitialInfection()
{
	BroadcastInfectionComing(GetInfectionStartTick());
}

void CIcGameController::RoundTickAfterInitialInfection()
{
	const bool StartInfectionTrigger = GetInfectionStartTick() == Server()->Tick();

	if(StartInfectionTrigger)
		OnInfectionTriggered();

	if(GetRoundType() == ERoundType::Survival)
	{
		AddMoreBotsAccordingToConfiguration();
	}

	// Ensure that the newly joined players have correct state/class
	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	while(Iter.Next())
	{
		CIcPlayer *pPlayer = Iter.Player();
		if(pPlayer->GetClass() == EPlayerClass::None)
		{
			pPlayer->KillCharacter(); // Infect the player
			pPlayer->StartInfection();
			pPlayer->m_DieTick = m_RoundStartTick;
		}
	}

	if(!StartInfectionTrigger)
		DoTeamBalance();

	UpdateBalanceFactors();

	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		OnHideAndSeekTick();
	}
}

void CIcGameController::PreparePlayerToJoin(CIcPlayer *pPlayer)
{
	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		// Make the player Medic initially
		bool MakeHuman = true;

		if(IsInfectionStarted())
		{
			int NumHumans = 0;
			int NumInfected = 0;
			GetPlayerCounter(pPlayer->GetCid(), NumHumans, NumInfected);

			const int NumPlayers = NumHumans + NumInfected + 1;
			const int NumFirstPickedPlayers = GetMinimumInfectedForPlayers(NumPlayers);
			const int NeededMedics = maximum<int>(0, NumFirstPickedPlayers - NumHumans);

			// If the gameplay already started then make the player Medic if medics needed
			// and make it a ghost otherwise.
			MakeHuman = NeededMedics > 0;
		}

		if(MakeHuman)
		{
			const EPlayerClass NewClass = ChooseHumanClass(pPlayer);
			pPlayer->SetClass(NewClass);
		}
		else
		{
			EPlayerClass c = ChooseInfectedClass(pPlayer);
			pPlayer->SetClass(c);
		}

		return;
	}

	if(IsInfectionStarted())
	{
		if(!pPlayer->IsInfected())
		{
			const EPlayerClass c = ChooseInfectedClass(pPlayer);
			pPlayer->SetClass(c);
		}
	}
}

void CIcGameController::SetPlayerPickedTimestamp(CIcPlayer *pPlayer, int Timestamp) const
{
	const int PrevInfectionTimestamp = pPlayer->GetInfectionTimestamp();
	pPlayer->SetInfectionTimestamp(Timestamp);

	if(PrevInfectionTimestamp && Timestamp > PrevInfectionTimestamp)
	{
		const int PrevInfectionSeconds = Timestamp - PrevInfectionTimestamp;
		dbg_msg("server", "SetPlayerPickedTimestamp: Pick cid=%d (previously picked %d seconds ago)", pPlayer->GetCid(), PrevInfectionSeconds);
	}
	else
	{
		dbg_msg("server", "SetPlayerPickedTimestamp: Pick cid=%d (was not picked before)", pPlayer->GetCid());
	}
}

uint32_t CIcGameController::InfectHumans(uint32_t NumHumansToInfect)
{
	if(NumHumansToInfect == 0)
		return 0;

	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);

	icArray<CIcPlayer *, MAX_CLIENTS> Humans;

	while(Iter.Next())
	{
		CIcPlayer *pPlayer = Iter.Player();
		if(pPlayer->IsHuman())
		{
			Humans.Add(pPlayer);
		}
	}

	if(NumHumansToInfect > Humans.Size())
	{
		// Makes no sense, must be a testing game
		return 0;
	}

	const auto Sorter = [](const CIcPlayer *p1, const CIcPlayer *p2) -> bool {
		return p1->GetInfectionTimestamp() < p2->GetInfectionTimestamp();
	};

	std::ranges::stable_sort(Humans, Sorter);

	const int Timestamp = time_timestamp();

	uint32_t NewInfected = 0;
	for(CIcPlayer *pPlayer : Humans)
	{
		pPlayer->KillCharacter(); // Infect the player
		pPlayer->StartInfection();
		pPlayer->m_DieTick = m_RoundStartTick;
		NewInfected++;

		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTION,
			_("{str:VictimName} has been infected"),
			"VictimName", Server()->ClientName(pPlayer->GetCid()),
			nullptr);

		SetPlayerPickedTimestamp(pPlayer, Timestamp);
		if(NewInfected >= NumHumansToInfect)
		{
			break;
		}
	}

	return NewInfected;
}

void CIcGameController::ForcePlayersBalance(uint32_t PlayersToBalance)
{
	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		icArray<CIcPlayer *, MAX_CLIENTS> Infected;
		CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
		while(Iter.Next())
			Infected.Add(Iter.Player());

		const auto Sorter = [](const CIcPlayer *p1, const CIcPlayer *p2) -> bool {
			return p1->GetInfectionTimestamp() < p2->GetInfectionTimestamp();
		};
		std::stable_sort(Infected.begin(), Infected.end(), Sorter);

		int Timestamp = time_timestamp();
		for(CIcPlayer *pPlayer : Infected)
		{
			SetPlayerPickedTimestamp(pPlayer, Timestamp);

			const EPlayerClass NewClass = ChooseHumanClass(pPlayer);
			pPlayer->SetClass(NewClass);
			pPlayer->m_DieTick = m_RoundStartTick;
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTION,
				_("{str:VictimName} has been revived to balance the game"),
				"VictimName", Server()->ClientName(pPlayer->GetCid()),
				nullptr);

			--PlayersToBalance;
			if(PlayersToBalance == 0)
			{
				return;
			}
		}
	}

	// Force balance
	InfectHumans(PlayersToBalance);
	GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTION,
		_("Someone was infected to balance the game."), nullptr);
}

void CIcGameController::UpdateBalanceFactors()
{
	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	if(NumInfected == 1)
	{
		m_InfBalanceBoostFactor = 1;
		return;
	}

	const int NumPlayers = NumHumans + NumInfected;
	const int NumMinimumInfected = GetMinimumInfectedForPlayers(NumPlayers);
	const int PlayersToInfect = maximum<int>(0, NumMinimumInfected - NumInfected);
	if(PlayersToInfect >= 2)
	{
		// ToInfect / MinInfected
		// 2 / 3 = 0.66 // Could be 66% bonus but 'num infected = 1' already handled and gives 100%
		// 2 / 4 = 0.5  // If min infected = 4 and there are two infected, they'll have 50% bonus (5 extra armor)
		// 2 / 5 = 0.4  // ...
		m_InfBalanceBoostFactor = static_cast<float>(PlayersToInfect) / NumMinimumInfected;
		return;
	}

	m_InfBalanceBoostFactor = 0;
}

void CIcGameController::CancelTheRound(ROUND_CANCELATION_REASON Reason)
{
	switch(Reason)
	{
	case ROUND_CANCELATION_REASON::INVALID:
		return;
	case ROUND_CANCELATION_REASON::ALL_INFECTED_LEFT_THE_GAME:
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTED, _("The round canceled: All infected have left the game"), "RoundDuration", nullptr);
		break;
	case ROUND_CANCELATION_REASON::EVERYONE_INFECTED_BY_THE_GAME:
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTED, _("The round canceled: All humans have left the game"), nullptr);
		break;
	}

	EndRound(ERoundEndReason::CANCELED);
}

void CIcGameController::AnnounceTheWinner(int NumHumans)
{
	const bool HumansWon = NumHumans > 0;
	if(Config()->m_InfSurvivalMode)
	{
		EndRound(ERoundEndReason::FINISHED);
		return;
	}
	else if(GetRoundType() == ERoundType::Survival && HumansWon)
	{
		if(m_SurvivalConfiguration.SurvivalWaves.Size() > m_SurvivalState.Wave + 1)
		{
			m_InfectedStarted = false;
			ResetFinalExplosion();
			EndSurvivalWave();

			m_SurvivalState.Wave++;

			StartSurvivalWave();
			return;
		}
	}

	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		AnnounceHideAndSeekWinner();
	}
	else if(NumHumans)
	{
		GameServer()->SendChatTarget_Localization_P(-1, CHATCATEGORY_HUMANS, NumHumans,
			_P("{int:NumHumans} human won the round",
				"{int:NumHumans} humans won the round", NumHumans),
			"NumHumans", &NumHumans,
			nullptr);

		char aBuf[256];
		CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
		while(Iter.Next())
		{
			if(Iter.Player()->IsHuman())
			{
				// TAG_SCORE
				Server()->RoundStatistics()->OnScoreEvent(Iter.ClientId(), EScoreEvent::HUMAN_SURVIVE, Iter.Player()->GetClass(), Server()->ClientName(Iter.ClientId()), Console());
				Server()->RoundStatistics()->SetPlayerAsWinner(Iter.ClientId());
				GameServer()->SendScoreSound(Iter.ClientId());

				GameServer()->SendChatTarget_Localization(Iter.ClientId(), CHATCATEGORY_SCORE, _("You have survived, +5 points"), nullptr);
				str_format(aBuf, sizeof(aBuf), "survived player='%s'", Server()->ClientName(Iter.ClientId()));
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
			}
		}
	}
	else
	{
		const int Seconds = (Server()->Tick() - m_RoundStartTick) / static_cast<float>(Server()->TickSpeed());
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTED, _("Infected won the round in {sec:RoundDuration}"), "RoundDuration", &Seconds, nullptr);
	}

	EndRound(ERoundEndReason::FINISHED);
}

void CIcGameController::AnnounceHideAndSeekWinner()
{
	struct CPlayerScore
	{
		char aPlayerName[MAX_NAME_LENGTH];
		int Kills;
		int Deaths;
		bool Human;
		bool FirstInfected;
	};

	icArray<CPlayerScore, MAX_CLIENTS> Scores;
	int HumansScore = 0;
	int NumSurvivedGhosts = 0;

	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	while(Iter.Next())
	{
		const CIcPlayer *pPlayer = Iter.Player();
		CPlayerScore Score;

		str_copy(Score.aPlayerName, Server()->ClientName(pPlayer->GetCid()));
		Score.Kills = pPlayer->GetKills();
		Score.Deaths = pPlayer->GetDeaths();
		Score.Human = pPlayer->IsHuman();
		Score.FirstInfected = pPlayer->IsInfected() && (pPlayer->m_TeamChangeTick < GetInfectionStartTick() + 5);

		if(Score.Human)
		{
			HumansScore += Score.Kills;
		}
		else if(Score.FirstInfected)
		{
			if(Score.Deaths == 0)
			{
				++NumSurvivedGhosts;
			}
		}
		Scores.Add(Score);
	}

	const auto Sorter = [NumSurvivedGhosts](const CPlayerScore &s1, const CPlayerScore &s2) -> bool {
		if(s1.Human != s2.Human)
		{
			if(NumSurvivedGhosts > 0)
			{
				// Infected won, show them first
				return !s1.Human;
			}
			else
			{
				// Humans won, show them first
				return s1.Human;
			}
		}

		if(s1.Human)
		{
			return s1.Kills > s2.Kills;
		}
		else
		{
			if(s1.FirstInfected != s2.FirstInfected)
			{
				// Show first infected before joined
				return s1.FirstInfected;
			}

			return s1.Deaths < s2.Deaths;
		}
	};

	std::stable_sort(Scores.begin(), Scores.end(), Sorter);

	GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "Score", nullptr);
	for(const CPlayerScore &Score : Scores)
	{
		if(Score.Human)
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "- {str:PlayerName}: {int:Score} kills",
				"PlayerName", Score.aPlayerName,
				"Score", &Score.Kills,
				nullptr);
		}
		else
		{
			if(Score.FirstInfected)
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "- {str:PlayerName}: {int:Score} deaths",
					"PlayerName", Score.aPlayerName,
					"Score", &Score.Deaths,
					nullptr);
			}
			else
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "- {str:PlayerName}: {int:Score} deaths (joined later)",
					"PlayerName", Score.aPlayerName,
					"Score", &Score.Deaths,
					nullptr);
			}
		}
	}

	const char *pWinMessage{};
	if(NumSurvivedGhosts)
	{
		GameServer()->SendChatTarget_Localization_P(-1, CHATCATEGORY_INFECTED, NumSurvivedGhosts,
			_P("{int:NumInfected} ghost survived",
				"{int:NumInfected} ghosts survived", NumSurvivedGhosts),
			"NumInfected", &NumSurvivedGhosts,
			nullptr);
		pWinMessage = _("The Infected won the round");
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTED, pWinMessage, nullptr);
	}
	else
	{
		if(m_HsFastRound)
		{
			const int Seconds = GetRoundTick() / ((float)Server()->TickSpeed());
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_INFECTED, _("Humans won the round in {sec:RoundDuration}"), "RoundDuration", &Seconds, nullptr);
		}
		else
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE, "Total humans team score: {int:Score}",
				"Score", &HumansScore,
				nullptr);
		}
		pWinMessage = _("Humans won the round");
		GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_HUMANS, pWinMessage, nullptr);
	}

	GameServer()->SendBroadcast_Localization(-1, EBroadcastPriority::GAMEANNOUNCE, 3 * Server()->TickSpeed(), pWinMessage, nullptr);
}

void CIcGameController::BroadcastInfectionComing(int InfectionTick)
{
	if(InfectionTick <= Server()->Tick())
		return;

	int Seconds = (InfectionTick - Server()->Tick()) / Server()->TickSpeed() + 1;
	const EBroadcastPriority Priority = Seconds <= 3 ? EBroadcastPriority::GAMEANNOUNCE : EBroadcastPriority::LOWEST;
	GameServer()->SendBroadcast_Localization(-1,
		Priority,
		BROADCAST_DURATION_REALTIME,
		_("Infection is coming in {sec:RemainingTime}"),
		"RemainingTime", &Seconds,
		nullptr);
}

void CIcGameController::MaybeDropPickup(CIcCharacter *pVictim)
{
	const int DropMaxLevel = pVictim->GetDropLevel();
	if(DropMaxLevel <= 0)
		return;

	// Unset the drop for the Victim so it won't drop more pickups after this one
	pVictim->SetDropLevel(0);

	icArray<int, 4> BadIndices = {
		ZONE_DAMAGE_DEATH,
		ZONE_DAMAGE_DEATH_NOUNDEAD,
		// ZONE_DAMAGE_DAMAGE,
		// ZONE_DAMAGE_DAMAGE_HUMANS,
	};

	if(pVictim->IsInfected())
	{
		// An infected would drop a pickup for humans
		// but humans won't be able to get it from infected zone
		BadIndices.Add(ZONE_DAMAGE_INFECTION);
	}

	const vec2 VictimPos = pVictim->GetPos();
	const icArray<vec2, 3> aPossiblePositions = {
		VictimPos,
		VictimPos + vec2(0, TileSizeF),
		VictimPos - vec2(0, TileSizeF),
	};

	std::optional<vec2> Pos{};
	for(const CTileRoundedPosition Rounded : aPossiblePositions)
	{
		const int ZoneIndex = GetDamageZoneValueAt(Rounded.Center());
		if(BadIndices.Contains(ZoneIndex))
			continue;

		Pos = Rounded.Center();
		break;
	}

	if(!Pos.has_value())
	{
		// No drop if noone will be able to pick it up
		return;
	}

	ClientsArray HasSpawnedPickups;
	for(TEntityPtr<CIcPickup> pPickup = GameWorld()->FindFirst<CIcPickup>(); pPickup; ++pPickup)
	{
		int Owner = pPickup->GetOwner();
		if(Owner >= 0 && !HasSpawnedPickups.Contains(Owner))
		{
			HasSpawnedPickups.Add(Owner);
		}
	}

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CIcCharacter *pCharacter = GetCharacter(ClientId);
		if(!pCharacter)
			continue;

		if(pCharacter->IsHuman() == pVictim->IsHuman())
		{
			// No drop from teammates
			continue;
		}

		const CIcPlayerClass *pClass = pCharacter->GetClass();
		if(pClass->GetUpgradeLevel() >= DropMaxLevel)
		{
			// The player already has an upgrade of this level
			continue;
		}

		PlayerUpgradesArray Upgrade = pClass->GetNextUpgrade();
		if(Upgrade.IsEmpty())
			continue;

		if(HasSpawnedPickups.Contains(ClientId))
			continue;

		CIcPickup *p = new CIcPickup(GameServer(), EICPickupType::ClassUpgrade, Pos.value(), ClientId);
		p->SetUpgrades(Upgrade);
		p->Spawn();
	}
}

bool CIcGameController::IsInfectionStarted() const
{
	if(Config()->m_InfTrainingMode)
		return false;

	return GetInfectionStartTick() <= Server()->Tick();
}

bool CIcGameController::MapRotationEnabled() const
{
	if(Config()->m_InfSurvivalMode)
	{
		return false;
	}

	return true;
}

void CIcGameController::OnTeamChangeRequested(int ClientId, int Team)
{
	CPlayer *pPlayer = GetPlayer(ClientId);
	// Switch team on given client and kill/respawn him
	if(CanJoinTeam(Team, ClientId))
	{
		if(CanChangeTeam(pPlayer, Team))
		{
			pPlayer->m_LastSetTeam = Server()->Tick();
			if(pPlayer->GetTeam() == TEAM_SPECTATORS || Team == TEAM_SPECTATORS)
				GameServer()->RequestVotesUpdate();
			DoTeamChange(pPlayer, Team);
			pPlayer->m_TeamChangeTick = Server()->Tick();
		}
		else
			GameServer()->SendBroadcast(ClientId, "Teams must be balanced, please join other team", EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE);
	}
}

bool CIcGameController::CanJoinTeam(int Team, int ClientId)
{
	if(Team != TEAM_SPECTATORS)
	{
		if(GetRoundType() == ERoundType::Survival)
		{
			if(IsInfectionStarted() || (HardMode() && (m_SurvivalState.Wave > 0)))
			{
				GameServer()->SendBroadcast_Localization(ClientId,
					EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE,
					_("You have to wait until the survival is over"));
				return false;
			}

			int MaxPlayers = SurvivalGetGameConfiguration()->MaxPlayers;
			if(Config()->m_InfSurvivalPlayerLimit && MaxPlayers)
			{
				int NumHumans = 0;
				int NumInfected = 0;
				GetPlayerCounter(-1, NumHumans, NumInfected);

				if(NumHumans >= MaxPlayers)
				{
					GameServer()->SendBroadcast_Localization_P(ClientId,
						EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, MaxPlayers,
						_("Only {int:MaxPlayers} active players are allowed"),
						"MaxPlayers", &MaxPlayers,
						nullptr);
					return false;
				}
			}
		}

		return IGameController::CanJoinTeam(Team, ClientId);
	}

	if(IsGameOver())
		return true;

	const CIcPlayer *pPlayer = GetPlayer(ClientId);

	if(!pPlayer) // Invalid call
		return false;

	if(pPlayer->IsHuman())
		return true;

	int NumHumans;
	int NumInfected;
	GetPlayerCounter(ClientId, NumHumans, NumInfected);
	const int NumPlayers = NumHumans + NumInfected;
	const int NumMinInfected = GetMinimumInfectedForPlayers(NumPlayers);
	if(NumInfected >= NumMinInfected)
	{
		// Let the ClientId join the specs if we'll not have to infect
		// someone after the join.
		return true;
	}

	return false;
}

bool CIcGameController::AreTurretsEnabled() const
{
	if(!Config()->m_InfTurretEnable)
		return false;

	if(GetRoundType() == ERoundType::Survival)
		return true;

	return Server()->GetActivePlayerCount() >= static_cast<uint32_t>(Config()->m_InfMinPlayersForTurrets);
}

int CIcGameController::InfTurretDuration() const
{
	if(GetRoundType() == ERoundType::Survival)
		return 20;

	return Config()->m_InfTurretDuration;
}

bool CIcGameController::MercBombsEnabled() const
{
	return GetRoundType() != ERoundType::Fun;
}

bool CIcGameController::WhiteHoleEnabled() const
{
	if(GetRoundType() == ERoundType::Survival)
		return true;

	if(GetRoundType() == ERoundType::Fast)
		return false;

	if(Server()->GetActivePlayerCount() < static_cast<uint32_t>(Config()->m_InfMinPlayersForWhiteHole))
		return false;

	return Config()->m_InfWhiteHoleProbability > 0;
}

bool CIcGameController::AirstrikeEnabled() const
{
	if(GetRoundType() == ERoundType::Survival)
		return true;

	if(GetRoundType() == ERoundType::Fast)
		return false;

	if(Server()->GetActivePlayerCount() < static_cast<uint32_t>(Config()->m_InfMinPlayersForAirStrike))
		return false;

	return Config()->m_InfAirStrikeProbability > 0;
}

float CIcGameController::GetWhiteHoleLifeSpan() const
{
	if(GetRoundType() == ERoundType::Survival)
	{
		if(HardMode())
			return 12;

		return 15;
	}

	return Config()->m_InfWhiteHoleLifeSpan;
}

int CIcGameController::MinimumInfectedForRevival() const
{
	return Config()->m_InfRevivalMinInfected;
}

bool CIcGameController::IsClassChooserEnabled() const
{
	if(GetRoundType() == ERoundType::HideAndSeek)
		return false;

	return Config()->m_InfClassChooser && Server()->GetTimeShiftUnit();
}

int CIcGameController::HardMode() const
{
	if(GetRoundType() != ERoundType::Survival)
		return 0;

	return std::max<int>(SurvivalGetGameConfiguration()->HardMode, Config()->m_InfSurvivalHardMode);
}

bool CIcGameController::HumanCanPickSameClass() const
{
	return Config()->m_InfAllowPickingSameClass || (Server()->GetActivePlayerCount() == 1);
}

int CIcGameController::GetTaxiMode() const
{
	if(GetRoundType() == ERoundType::Survival)
		return 0;

	return Config()->m_InfTaxi;
}

int CIcGameController::GetGhoulStomackSize() const
{
	if(GetRoundType() == ERoundType::Fun)
		return Config()->m_FunRoundGhoulStomachSize;

	return Config()->m_InfGhoulStomachSize;
}

EPlayerScoreMode CIcGameController::GetPlayerScoreMode(int SnappingClient) const
{
	if(IsGameOver())
	{
		// game over.. wait for restart
		if(Server()->Tick() <= m_GameOverTick + Server()->TickSpeed() * Config()->m_InfShowScoreTime)
		{
			if((Server()->Tick() - m_GameOverTick) > Server()->TickSpeed() * (Config()->m_InfShowScoreTime / 2.0f))
			{
				return EPlayerScoreMode::Time;
			}

			return EPlayerScoreMode::Class;
		}
	}

	if(const CIcPlayer *pSnapPlayer = GetPlayer(SnappingClient))
	{
		return pSnapPlayer->GetScoreMode();
	}

	return EPlayerScoreMode::Class;
}

float CIcGameController::GetTimeLimitMinutes() const
{
	if(Config()->m_InfTrainingMode)
		return 0;

	if(m_RoundTimeLimitSeconds.has_value())
		return m_RoundTimeLimitSeconds.value() / 60.0;

	const float BaseTimeLimit = Config()->m_SvTimelimitInSeconds ? Config()->m_SvTimelimitInSeconds / 60.0 : Config()->m_SvTimelimit;

	switch(GetRoundType())
	{
	case ERoundType::Fun:
		return minimum<float>(BaseTimeLimit, Config()->m_FunRoundDuration);
	case ERoundType::Fast:
		return clamp<float>(BaseTimeLimit * 0.5, 1, 3);
	case ERoundType::Survival:
		if(Config()->m_InfSurvivalMode == SURVIVAL_MODE_TIME_BASED)
			return BaseTimeLimit;
		return 0;
	default:
		return BaseTimeLimit;
	}
}

int CIcGameController::GetTimeLimitSeconds() const
{
	return GetTimeLimitMinutes() * 60;
}

void CIcGameController::SetTimeLimitSeconds(float Seconds)
{
	m_RoundTimeLimitSeconds = Seconds;
}

int CIcGameController::GetInfectionDelay() const
{
	return m_RoundInfectionDelaySeconds.value_or(Config()->m_InfInitialInfectionDelay);
}

void CIcGameController::SetInfectionDelay(int Seconds)
{
	m_RoundInfectionDelaySeconds = Seconds;
}

bool CIcGameController::HeroGiftAvailable() const
{
	return Server()->Tick() >= m_HeroGiftTick;
}

std::optional<vec2> CIcGameController::GetHeroFlagPosition() const
{
	if(Lua()->HasGlobalCallable("Get_hero_flag_position"))
	{
		return RunCallbackWithResult<vec2>(Lua()->GetLuaState(), "Get_hero_flag_position");
	}

	const int NbPos = m_HeroFlagPositions.size();
	if(NbPos == 0)
		return std::nullopt;

	for(int Attempts = 3; Attempts > 0; Attempts--)
	{
		const int Index = random_int(0, NbPos - 1);
		const vec2 Pos = m_HeroFlagPositions[Index];
		if(IsPositionAvailableForHumans(Pos))
		{
			return Pos;
		}
	}

	return std::nullopt;
}

bool CIcGameController::IsPositionAvailableForHumans(const vec2 &Position) const
{
	switch(GetDamageZoneValueAt(Position))
	{
	case ZONE_DAMAGE_INFECTION:
	case ZONE_DAMAGE_DEATH_NOUNDEAD:
	case ZONE_DAMAGE_DAMAGE:
	case ZONE_DAMAGE_DAMAGE_HUMANS:
		return false;
	default:
		return true;
	}
}

void CIcGameController::StartHideAndSeekRound()
{
	GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE);
	GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Hide and Seek round!"));
	GameServer()->SendBroadcast_Localization(-1,
		EBroadcastPriority::GAMEANNOUNCE, Server()->TickSpeed() * (GetInfectionDelay() + 2),
		_("Hide and Seek round!"), nullptr);

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CIcPlayer *pPlayer = GetPlayer(i);
		if(!pPlayer)
			continue;

		const EPlayerClass HumanClass = ChooseHumanClass(pPlayer);
		pPlayer->SetClass(HumanClass);
	}
}

void CIcGameController::EndHideAndSeekRound()
{
}

void CIcGameController::ApplyHideAndSeekAttributes(CIcPlayer *pPlayer)
{
	CIcCharacter *pCharacter = pPlayer->GetCharacter();
	if(pCharacter)
	{
		pCharacter->TakeAllWeapons();
		pCharacter->GiveWeapon(WEAPON_HAMMER, -1);
		pCharacter->SetActiveWeapon(WEAPON_HAMMER);

		pCharacter->CancelLoveEffect();
		if(pPlayer->IsInfected())
		{
			pCharacter->LoveEffect(GetTimeLimitSeconds() + 5);
			float SpawnProtectionDuration = Config()->m_HsGhostsProtection;
			float SpawnDeathPenalty = m_HsFastRound ? pPlayer->GetDeaths() * Config()->m_HsDeathPenaltyTime : 0;
			pCharacter->SetSoloForDuration(SpawnProtectionDuration + SpawnDeathPenalty);
		}
		else
		{
			pCharacter->GiveWeapon(WEAPON_GUN, -1);
			const int InfectionTick = GetInfectionStartTick();
			const float DisabledAttackBeforeInfection = (InfectionTick - Server()->Tick()) * 1.0f / Server()->TickSpeed();
			const float DisabledAttackOnSpawn = 2.0f;
			pCharacter->LoveEffect(IsInfectionStarted() ? DisabledAttackOnSpawn : DisabledAttackBeforeInfection);
			pCharacter->SetInvincible(2);
		}
	}

	CIcPlayerClass *pClass = pPlayer->GetCharacterClass();
	if(pClass)
	{
		if(pPlayer->IsHuman())
		{
			pClass->SetNormalEmote(EMOTE_ANGRY);
		}
	}
}

void CIcGameController::OnHideAndSeekTick()
{
	if(!m_HsFastRound)
		return;

	std::size_t NumSurvivedGhosts{};
	std::optional<int> SurvivorCid{};

	{
		CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
		while(Iter.Next())
		{
			const CIcPlayer *pPlayer = Iter.Player();
			if(pPlayer->IsInfected() || pPlayer->IsInfectionStarted())
			{
				bool FirstInfected = pPlayer->m_TeamChangeTick < GetInfectionStartTick() + 5;
				if(FirstInfected && (pPlayer->GetDeaths() == 0))
				{
					NumSurvivedGhosts++;
					SurvivorCid = pPlayer->GetCid();
				}
			}
		}
		if(NumSurvivedGhosts > 1)
		{
			SurvivorCid.reset();
		}
	}

	if(SurvivorCid.has_value())
	{
		const char *pSurvivorName = Server()->ClientName(SurvivorCid.value());
		GameServer()->SendBroadcast_Localization(-1,
			EBroadcastPriority::GAMEANNOUNCE, 5 * Server()->TickSpeed(),
			_("The last survivor is '{str:PlayerName}'"),
			"PlayerName", pSurvivorName,
			nullptr);
	}
	else if(NumSurvivedGhosts == 0)
	{
		GameServer()->SendBroadcast_Localization(-1,
			EBroadcastPriority::GAMEANNOUNCE, 5 * Server()->TickSpeed(),
			_("Humans won"),
			nullptr);

		EnsureFinalExplosionIsStarted();
	}
}

void CIcGameController::StartFunRound()
{
	if(m_FunRoundConfigurations.empty())
		return;

	const auto &Configs = m_FunRoundConfigurations;
	const int type = random_int(0, Configs.size() - 1);
	const FunRoundConfiguration &Configuration = Configs[type];

	const char *pTitle = Config()->m_FunRoundTitle;
	char aBuf[256];

	const std::vector<const char *> phrases = {
		", glhf!",
		", not ez!",
		" c:",
		" xd",
		", that's gg",
		", good luck!"};
	const char *random_phrase = phrases[random_int(0, phrases.size() - 1)];
	m_FunRoundConfiguration = Configuration;

	const char *HumanClassText = CIcGameController::GetClassPluralDisplayName(Configuration.HumanClass);
	const char *InfectedClassText = CIcGameController::GetClassPluralDisplayName(Configuration.InfectedClass);

	str_format(aBuf, sizeof(aBuf), "%s! %s vs %s%s", pTitle, InfectedClassText, HumanClassText, random_phrase);

	GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE);
	GameServer()->SendChatTarget(-1, aBuf);

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CIcPlayer *pPlayer = GetPlayer(i);
		if(!pPlayer)
			continue;

		pPlayer->SetClass(Configuration.HumanClass);
	}
}

void CIcGameController::EndFunRound()
{
	m_FunRoundsPassed++;
}

void CIcGameController::StartSurvivalRound()
{
	if(!StartSurvivalWave())
	{
		return;
	}

	SetRoundMinimumPlayers(1);
	SetRoundMinimumInfected(0);

	if(Config()->m_InfSurvivalMode && (m_SurvivalState.Wave > 0))
	{
		GameServer()->m_World.m_ResetRequested = false;
	}
}

void CIcGameController::EndSurvivalRound(ERoundEndReason Reason)
{
	if(Reason != ERoundEndReason::FINISHED)
	{
		RemoveBots();
		m_SurvivalState.SurvivedPlayers.Clear();
		return;
	}

	bool IsOver = false;

	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	if((NumHumans == 0) && (NumInfected > 0))
	{
		if(m_SurvivalConfiguration.SurvivalWaves.Size() == 1)
		{
			GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE,
				_("The survival is over. You have failed to survive."));
		}
		else
		{
			if(m_SurvivalState.Wave == 0)
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE,
					_("The survival is over. You have failed to survive a single wave."));
			}
			else
			{
				int NumWaves = m_SurvivalState.Wave + 1;
				GameServer()->SendChatTarget_Localization_P(-1, CHATCATEGORY_SCORE, NumWaves,
					_P(
						"The survival is over after {int:NumWaves} wave.",
						"The survival is over after {int:NumWaves} waves.", NumWaves),
					"NumWaves", &NumWaves,
					nullptr);
			}
		}

		IsOver = true;
	}

	const SurvivalWaveConfiguration *pCurrentWave = GetCurrentSurvivalWaveConfiguration();
	const char *pExecCommand = nullptr;
	if(NumHumans == 0)
	{
		pExecCommand = pCurrentWave->aCommandOnLost;
	}
	else
	{
		pExecCommand = pCurrentWave->aCommandOnWon;
	}
	if(pExecCommand && pExecCommand[0])
	{
		if(Console()->LineIsValid(pExecCommand))
		{
			Console()->ExecuteLine(pExecCommand);
		}
		else
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid survival end_round command");
		}
	}

	if(m_SurvivalConfiguration.SurvivalWaves.Size() == m_SurvivalState.Wave + 1)
	{
		if(NumHumans)
		{
			if(m_SurvivalConfiguration.SurvivalWaves.Size() > 2)
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE,
					_("The survival is over. You have survived!"));
			}
			else
			{
				GameServer()->SendChatTarget_Localization(-1, CHATCATEGORY_SCORE,
					_("The survival is over. You have survived the final wave!"));
			}
		}

		IsOver = true;
	}

	if(IsOver)
	{
		EndSurvivalGame();
		return;
	}

	EndSurvivalWave();

	if(Config()->m_InfSurvivalMode)
	{
		QueueRoundType(ERoundType::Survival);

		if(!IsOver)
		{
			// Cancel gameover, force new round
			StartRound();
		}
	}
}

bool CIcGameController::StartSurvivalWave()
{
	char aBuf[256];

	int WaveDisplayNumber = m_SurvivalState.Wave + 1;
	if(m_SurvivalConfiguration.SurvivalWaves.Size() <= m_SurvivalState.Wave)
	{
		str_format(aBuf, sizeof(aBuf), "Unable to start a survival round: wave %d is not configured", WaveDisplayNumber);
		GameServer()->SendChatTarget(-1, aBuf);
		return false;
	}

	RemoveBots();

	m_WaveStartTick = Server()->Tick();

	if(m_SurvivalConfiguration.SurvivalWaves.Size() == 1)
	{
		str_format(aBuf, sizeof(aBuf), "The survival begins. Enjoy!");
	}
	else
	{
		const SurvivalWaveConfiguration &WaveConf = m_SurvivalConfiguration.SurvivalWaves.At(m_SurvivalState.Wave);

		if(WaveConf.aName[0])
		{
			str_format(aBuf, sizeof(aBuf), "Wave %d: %s", WaveDisplayNumber, WaveConf.aName);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "Wave %d. Enjoy!", WaveDisplayNumber);
		}
	}

	GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);

	return true;
}

void CIcGameController::EndSurvivalWave()
{
	m_WaveStartTick = 0;

	if(Config()->m_InfSurvivalHardMode)
	{
		return;
	}

	m_SurvivalState.SurvivedPlayers.Clear();

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		const CIcCharacter *pCharacter = GetCharacter(i);
		if(pCharacter && pCharacter->IsHuman())
		{
			m_SurvivalState.SurvivedPlayers.Add(i);
		}
	}
}

void CIcGameController::EnsureFinalExplosionIsStarted()
{
	if(m_FinalExplosionState == EFinalExplosionState::NotStarted)
	{
		StartFinalExplosion();
	}
}

void CIcGameController::StartFinalExplosion()
{
	dbg_assert(m_FinalExplosionState == EFinalExplosionState::NotStarted, "Invalid final explosion start");
	for(TEntityPtr<CIcCharacter> p = GameWorld()->FindFirst<CIcCharacter>(); p; ++p)
	{
		if(p->IsInfected())
		{
			GameServer()->SendEmoticon(p->GetCid(), EMOTICON_GHOST);
		}
		else
		{
			GameServer()->SendEmoticon(p->GetCid(), EMOTICON_EYES);
		}
	}

	m_FinalExplosionState = EFinalExplosionState::Started;
}

void CIcGameController::ProgressFinalExplosion()
{
	if(m_FinalExplosionState != EFinalExplosionState::Started)
	{
		return;
	}

	bool NewExplosion = false;

	for(int j = 0; j < m_MapHeight; j++)
	{
		for(int i = 0; i < m_MapWidth; i++)
		{
			if((m_GrowingMap[j * m_MapWidth + i] & 1) && ((i > 0 && m_GrowingMap[j * m_MapWidth + i - 1] & 2) ||
															 (i < m_MapWidth - 1 && m_GrowingMap[j * m_MapWidth + i + 1] & 2) ||
															 (j > 0 && m_GrowingMap[(j - 1) * m_MapWidth + i] & 2) ||
															 (j < m_MapHeight - 1 && m_GrowingMap[(j + 1) * m_MapWidth + i] & 2)))
			{
				NewExplosion = true;
				m_GrowingMap[j * m_MapWidth + i] |= 8;
				m_GrowingMap[j * m_MapWidth + i] &= ~1;
				if(random_prob(0.1f))
				{
					vec2 TilePos = vec2(16.0f, 16.0f) + vec2(i * 32.0f, j * 32.0f);
					static constexpr int Damage = 0;
					CreateExplosion(TilePos, -1, EDamageType::NO_DAMAGE, Damage);
					GameServer()->CreateSound(TilePos, SOUND_GRENADE_EXPLODE);
				}
			}
		}
	}

	for(int j = 0; j < m_MapHeight; j++)
	{
		for(int i = 0; i < m_MapWidth; i++)
		{
			if(m_GrowingMap[j * m_MapWidth + i] & 8)
			{
				m_GrowingMap[j * m_MapWidth + i] &= ~8;
				m_GrowingMap[j * m_MapWidth + i] |= 2;
			}
		}
	}

	for(TEntityPtr<CIcCharacter> p = GameWorld()->FindFirst<CIcCharacter>(); p; ++p)
	{
		if(p->IsHuman())
			continue;

		int tileX = static_cast<int>(round(p->m_Pos.x)) / 32;
		int tileY = static_cast<int>(round(p->m_Pos.y)) / 32;

		if(tileX < 0)
			tileX = 0;
		if(tileX >= m_MapWidth)
			tileX = m_MapWidth - 1;
		if(tileY < 0)
			tileY = 0;
		if(tileY >= m_MapHeight)
			tileY = m_MapHeight - 1;

		if(m_GrowingMap[tileY * m_MapWidth + tileX] & 2 && p->GetPlayer())
		{
			p->Die(p->GetCid(), EDamageType::GAME_FINAL_EXPLOSION);
		}
	}

	if(!NewExplosion)
	{
		m_FinalExplosionState = EFinalExplosionState::Finished;
	}
}

void CIcGameController::Snap(int SnappingClient)
{
	CNetObj_GameInfo *pGameInfoObj = Server()->SnapNewItem<CNetObj_GameInfo>(0);
	if(!pGameInfoObj)
		return;

	pGameInfoObj->m_GameFlags = GameFlags_ClampToSix(m_GameFlags);
	pGameInfoObj->m_GameStateFlags = 0;
	if(m_GameOverTick != -1)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(m_SuddenDeath)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(GameWorld()->m_Paused)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	pGameInfoObj->m_RoundStartTick = m_RoundStartTick;
	pGameInfoObj->m_WarmupTimer = m_Warmup;

	pGameInfoObj->m_ScoreLimit = 0;
	if(GetRoundType() == ERoundType::Survival)
	{
		const bool KillBased = Config()->m_InfSurvivalMode != SURVIVAL_MODE_TIME_BASED;

		if(KillBased)
		{
			const SurvivalWaveConfiguration *pWave = GetCurrentSurvivalWaveConfiguration();
			pGameInfoObj->m_ScoreLimit = pWave->GetTotalInfectedLives();
		}
	}

	const int WholeMinutes = GetTimeLimitMinutes();
	const float FractionalPart = GetTimeLimitMinutes() - WholeMinutes;

	pGameInfoObj->m_TimeLimit = WholeMinutes + (FractionalPart ? 1 : 0);
	if(FractionalPart)
	{
		pGameInfoObj->m_RoundStartTick -= (1 - FractionalPart) * 60 * Server()->TickSpeed();
	}

	pGameInfoObj->m_RoundNum = (GameServer()->m_MapRotationList.size() && Config()->m_SvRoundsPerMap) ? Config()->m_SvRoundsPerMap : 0;
	pGameInfoObj->m_RoundCurrent = m_RoundCount + 1;

	SnapMapMenu(SnappingClient, pGameInfoObj);

	CNetObj_InfClassGameInfo *pInfclassGameInfoObj = Server()->SnapNewItem<CNetObj_InfClassGameInfo>(0);
	pInfclassGameInfoObj->m_Version = 2;
	pInfclassGameInfoObj->m_Flags = 0;
	pInfclassGameInfoObj->m_TimeLimitInSeconds = GetTimeLimitSeconds();
	pInfclassGameInfoObj->m_HeroGiftTick = m_HeroGiftTick;

	const int InfClassVersion = Server()->GetClientInfclassVersion(SnappingClient);
	if((InfClassVersion == 0) || (InfClassVersion > VERSION_INFC_160))
	{
		CNetObj_GameInfoEx *pGameInfoEx = Server()->SnapNewItem<CNetObj_GameInfoEx>(0);
		if(!pGameInfoEx)
			return;

		pGameInfoEx->m_Flags =
			GAMEINFOFLAG_PREDICT_VANILLA |
			GAMEINFOFLAG_DONT_MASK_ENTITIES |
			GAMEINFOFLAG_ALLOW_HOOK_COLL;

		if(InfClassVersion == 0)
		{
			// We use DDRace entities to show infection and other custom infclass tiles (see CClientGameTileGetter::GetClientGameTileIndex)
			pGameInfoEx->m_Flags |= GAMEINFOFLAG_ENTITIES_DDNET;
		}

		pGameInfoEx->m_Flags2 = m_GameInfoFlags2 | GAMEINFOFLAG2_HUD_DDRACE;
		pGameInfoEx->m_Flags2 |= GAMEINFOFLAG2_NO_WEAK_HOOK;
		pGameInfoEx->m_Flags2 |= GAMEINFOFLAG2_NO_SKIN_CHANGE_FOR_FROZEN;
		pGameInfoEx->m_Version = GAMEINFO_CURVERSION;
	}

	SendServerParams(SnappingClient);

	CNetObj_GameData *pGameDataObj = Server()->SnapNewItem<CNetObj_GameData>(0);
	if(!pGameDataObj)
		return;

	pGameDataObj->m_FlagCarrierRed = FLAG_ATSTAND;
	pGameDataObj->m_FlagCarrierBlue = FLAG_ATSTAND;
}

CPlayer *CIcGameController::CreatePlayer(int ClientId, bool IsSpectator, void *pData)
{
	UPlayerClass *pPlayer = nullptr;
	if(IsSpectator)
	{
		pPlayer = new(ClientId) UPlayerClass(this, GetNextClientUniqueId(), ClientId, TEAM_SPECTATORS);
	}
	else
	{
		const int StartTeam = Config()->m_SvTournamentMode ? TEAM_SPECTATORS : GetAutoTeam(ClientId);
		pPlayer = new(ClientId) UPlayerClass(this, GetNextClientUniqueId(), ClientId, StartTeam);
	}

#ifdef DEBUG_PLAYER
	pPlayer->SetBotUtilsData(*m_pBotUtilsData);
#endif

	if(pData)
	{
		const InfclassPlayerPersistantData *pPersistent = static_cast<InfclassPlayerPersistantData *>(pData);
		pPlayer->SetPreferredClass(pPersistent->m_PreferredClass);
		pPlayer->SetPreviouslyPickedClass(pPersistent->m_PreviouslyPickedClass);
		pPlayer->SetScoreMode(pPersistent->m_ScoreMode);
		pPlayer->SetAntiPingEnabled(pPersistent->m_AntiPing);
		pPlayer->SetInfectionTimestamp(pPersistent->m_LastInfectionTime);
	}

	PreparePlayerToJoin(pPlayer);

	return pPlayer;
}

int CIcGameController::PersistentClientDataSize() const
{
	return sizeof(InfclassPlayerPersistantData);
}

bool CIcGameController::GetClientPersistentData(int ClientId, void *pData) const
{
	const CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
		return false;

	InfclassPlayerPersistantData *pPersistent = static_cast<InfclassPlayerPersistantData *>(pData);
	pPersistent->m_ScoreMode = pPlayer->GetScoreMode();
	pPersistent->m_PreferredClass = pPlayer->GetPreferredClass();
	pPersistent->m_PreviouslyPickedClass = pPlayer->GetPreviouslyPickedClass();
	pPersistent->m_AntiPing = pPlayer->GetAntiPingEnabled();
	pPersistent->m_LastInfectionTime = pPlayer->GetInfectionTimestamp();
	return true;
}

void CIcGameController::GetHelpText(dynamic_string *pBuffer, int ClientId, const char *pHelpPage) const
{
	dynamic_string &Buffer = *pBuffer;
	const char *pLanguage = GetPlayer(ClientId)->GetLanguage();

	if(!pHelpPage || str_comp_nocase(pHelpPage, "game") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Rules of the game", _(nullptr)).c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "InfectionClass is a team game between humans and the infected.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "All players start as a human.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "10 seconds later, a few players become infected.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "The goal for the humans is to survive until the army cleans the map.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "The goal for the infected is to infect all humans.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "See also `/help pages`", _(nullptr)).c_str());
	}
	else if(str_comp_nocase(pHelpPage, "translate") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "How to translate the mod", _(nullptr)).c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Create an account on Crowdin and join the translation team:", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, Config()->m_AboutTranslationUrl, nullptr).c_str());
	}
	else if(str_comp_nocase(pHelpPage, "whitehole") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "White hole", _(nullptr)).c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "White hole pulls the infected into its center.", _C("White hole", nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_LP(pLanguage, g_Config.m_InfWhiteHoleMinimalKills, _CP("White hole", "Receive it by killing at least {int:NumKills} infected as a Scientist.", "Receive it by killing at least {int:NumKills} of the infected as a Scientist.", g_Config.m_InfWhiteHoleMinimalKills),
												  "NumKills",
												  &g_Config.m_InfWhiteHoleMinimalKills, nullptr)
				.c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Use the laser rifle to place it.", _C("White hole", nullptr)).c_str());
	}
	else if(str_comp_nocase(pHelpPage, "msg") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Targeted chat messages").c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Use “/w <PlayerName> <My Message>” to send a private message to this player.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Use “/msg !<ClassName> <My Message>” to send a private message to all players with a specific class.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Example: “/msg !medic I'm wounded!”", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Use “/msg !near” to send a private message to all players near you.", _(nullptr)).c_str());
	}
	else if(str_comp_nocase(pHelpPage, "mute") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Persistent player mute").c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Use “/mute <PlayerName>” to mute this player.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Unlike a client mute this will persist between map changes and wears off when either you or the muted player disconnects.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Example: “/mute nameless tee”", _(nullptr)).c_str());
		Buffer.append("\n\n");
	}
	else if(str_comp_nocase(pHelpPage, "taxi") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "How to use taxi mode", _(nullptr)).c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Two or more humans can form a taxi.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "In order to use it, both humans have to disable hook protection (usually, with F3). The human being hooked becomes the driver.", _(nullptr)).c_str());
		Buffer.append("\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "To get off the taxi, jump. To drop off your passengers, enable hook protection (usually, with F3).", _(nullptr)).c_str());
	}
	else if(str_comp_nocase(pHelpPage, "fast_round") == 0)
	{
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "Fast round", _(nullptr)).c_str());
		Buffer.append(" ~~\n\n");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, "In the fast rounds *more* humans become infected initially, "
																	"the spawning rate is increased and the round time limit is decreased. "
																	"White hole is also disabled.",
												  _(nullptr))
				.c_str());
	}
	else
	{
		bool Ok = true;
		const EPlayerClass PlayerClass = CIcGameController::GetClassByName(pHelpPage, &Ok);
		if(Ok)
		{
			GetClassHelpPage(&Buffer, pLanguage, PlayerClass);
		}
	}
}

bool CIcGameController::GetClassHelpPage(dynamic_string *pOutput, const char *pLanguage, EPlayerClass PlayerClass) const
{
	dynamic_string &Buffer = *pOutput;

	auto MakeHeader = [this, &Buffer, pLanguage](const char *pText) {
		Buffer.append("~~ ");
		Buffer.append(Server()->Localization()->Format_L(pLanguage, pText, nullptr).c_str());
		Buffer.append(" ~~");
	};

	auto AddText = [this, &Buffer, pLanguage](const char *pSeparator, const char *pText, const char *pArgName = nullptr, const void *pArgValue = nullptr) {
		Buffer.append(pSeparator);

		if(pArgName && pArgValue)
		{
			Buffer.append(Server()->Localization()->Format_L(pLanguage, pText, pArgName, pArgValue, nullptr).c_str());
		}
		else
		{
			Buffer.append(Server()->Localization()->Format_L(pLanguage, pText, nullptr).c_str());
		}
	};

	auto AddText_Plural = [this, &Buffer, pLanguage](const char *pSeparator, int Number, const char *pText, const char *pArgName, const void *pArgValue) {
		Buffer.append(pSeparator);

		Buffer.append(Server()->Localization()->Format_LP(pLanguage, Number, pText, pArgName, pArgValue, nullptr).c_str());
	};

	auto AddLine = [AddText](const char *pText, const char *pArgName = nullptr, const void *pArgValue = nullptr) {
		AddText("\n\n", pText, pArgName, pArgValue);
	};

	auto AddLine_Plural = [AddText_Plural](int Number, const char *pText, const char *pArgName, const void *pArgValue) {
		AddText_Plural("\n\n", Number, pText, pArgName, pArgValue);
	};

	auto ConLine = [AddText](const char *pText, const char *pArgName = nullptr, const void *pArgValue = nullptr) {
		AddText(" ", pText, pArgName, pArgValue);
	};

	static constexpr int HeroNumArmorGift = 4;

	MakeHeader(CIcGameController::GetClassDisplayName(PlayerClass));

	switch(PlayerClass)
	{
	case EPlayerClass::Invalid:
	case EPlayerClass::None:
	case EPlayerClass::Count:
		return false;

	case EPlayerClass::Mercenary:
		AddLine(_C("Mercenary", "The Mercenary can fly in the air using their machine gun."));
		AddLine(_C("Mercenary", "They can also create a powerful bomb with their hammer that can"
								" be charged by hitting it or with a laser rifle."));
		AddLine_Plural(g_Config.m_InfPoisonDamage,
			_CP("Mercenary",
				"Mercenary can also throw poison grenades that deal {int:NumDamagePoints} damage point"
				" and prevent the infected from healing.",
				"Mercenary can also throw poison grenades that deal {int:NumDamagePoints} damage points"
				" and prevent the infected from healing.",
				g_Config.m_InfPoisonDamage),
			"NumDamagePoints", &g_Config.m_InfPoisonDamage);
		break;
	case EPlayerClass::Medic:
		AddLine(_C("Medic", "The Medic can protect humans with the hammer by giving them armor."));
		AddLine(_C("Medic", "Grenades with medicine give armor to everybody in their range,"
							" including Heroes and the Medic themself."));
		if(g_Config.m_InfEnableTranquilizerRifle)
		{
			const int Duration = g_Config.m_InfTranquilizerDose;
			AddLine(_C("Medic", "Tranqulizer rifle makes the infected fall asleep for {sec:Duration}."), "Duration", &Duration);
		}
		else
		{
			AddLine(
				_C("Medic", "Laser rifle revives the infected, but at the cost of {int:Damage} hp and armor."),
				"Damage", &g_Config.m_InfRevivalDamage);
		}
		AddLine(_C("Medic", "Medic also has a powerful shotgun that can knock back the infected."));
		break;
	case EPlayerClass::Hero:
		AddLine(_C("Hero", "The Hero has all standard weapons."));
		AddLine(_C("Hero", "The Hero has to find a flag only visible to them. Stand still to be pointed towards it."));
		ConLine(_C("Hero", "The flag gifts a health point, {int:NumArmorGift} armor and full ammo to all humans."),
			"NumArmorGift", &HeroNumArmorGift);
		ConLine(_C("Hero", "It fully heals the Hero and it can grant a turret which you can place down with the hammer."));
		ConLine(_C("Hero", "The gift to all humans is only applied when the flag is surrounded by hearts and armor."));
		ConLine(_C("Hero", "The Hero cannot be healed by a Medic, but it can withstand a hit from an infected."));
		AddLine(_C("Hero", "The Hero can refresh the position of the flag by /rflag in survival mode."));
		break;
	case EPlayerClass::Engineer:
		AddLine(_C("Engineer", "The Engineer can build walls with the hammer to block the infected."));
		AddLine(_C("Engineer", "When an infected touches the wall, it dies."));
		AddLine(_C("Engineer", "The lifespan of a wall is {sec:LifeSpan}, and walls are limited to"
							   " one per player at the same time."),
			"LifeSpan", &g_Config.m_InfBarrierLifeSpan);
		break;
	case EPlayerClass::Soldier:
		AddLine(_C("Soldier", "The Soldier creates floating bombs with the hammer."));
		AddLine_Plural(g_Config.m_InfSoldierBombs,
			_CP("Soldier",
				"Each bomb can explode {int:NumBombs} time.",
				"Each bomb can explode {int:NumBombs} times.", g_Config.m_InfSoldierBombs),
			"NumBombs", &g_Config.m_InfSoldierBombs);

		AddLine(_("Use the hammer to place the bomb and explode it multiple times."));
		break;
	case EPlayerClass::Ninja:
		AddLine(_C("Ninja", "The Ninja can throw flash grenades that can freeze the infected for"
							" three seconds."));
		AddLine_Plural(
			g_Config.m_InfNinjaJump,
			_CP("Ninja",
				"Their hammer is replaced with a katana, allowing them to dash {int:NinjaJump}"
				" time before touching the ground.",
				"Their hammer is replaced with a katana, allowing them to dash {int:NinjaJump}"
				" times before touching the ground.",
				g_Config.m_InfNinjaJump),
			"NinjaJump", &g_Config.m_InfNinjaJump);
		AddLine(_("They also have a laser rifle that blinds the target for a short period of time."));
		AddLine(_("Ninja gets special targets. For killing a target, extra points and abilities"
				  " are awarded."));
		break;
	case EPlayerClass::Sniper:
		AddLine(_C("Sniper", "The Sniper can lock the position in mid-air for 15 seconds with the"
							 " hammer."));
		AddLine(_C("Sniper", "The locked position increases the Sniper's rifle damage from usual"
							 " 10-13 to 30 damage points."));
		AddLine(_C("Sniper", "They can also jump two times in the air."));
		break;
	case EPlayerClass::Scientist:
		AddLine(_C("Scientist", "The Scientist can pose floating mines with the hammer."));
		AddLine_Plural(g_Config.m_InfMineLimit,
			_CP("Scientist",
				"Mines are limited to {int:NumMines} per player at the same time.",
				"Mines are limited to {int:NumMines} per player at the same time.", g_Config.m_InfMineLimit),
			"NumMines", &g_Config.m_InfMineLimit);
		AddLine(_C("Scientist", "Scientist has also grenades that teleport them."));
		AddLine(_C("Scientist", "A lucky Scientist devoted to killing can get a white hole that"
								" sucks the infected in which can be placed with the laser rifle."));
		break;
	case EPlayerClass::Biologist:
		AddLine(_C("Biologist", "The Biologist has a shotgun with bouncing bullets and can create a"
								" spring laser trap by shooting with the laser rifle."));
		AddLine(_C("Biologist", "They are also immune to continuous poison effect, and can stop it"
								" on teammates with the Hammer."));
		break;
	case EPlayerClass::Looper:
		AddLine(_C("Looper", "The Looper has a laser wall that slows down the infected and a"
							 " low-range laser rifle with a high fire rate."));
		AddLine(_C("Looper", "They can also jump two times in the air."));
		break;
	case EPlayerClass::Artillery:
		AddLine(_C("Artillery", "我是1号占位符"
							 "我是2号占位符"));
		AddLine(_C("Artillery", "我是3号占位符"));
		break;
	case EPlayerClass::Smoker:
		AddLine(_C("Smoker", "Smoker has a powerful hook that hurts humans and sucks their blood,"
							 " restoring the Smoker's health."));
		AddLine(_C("Smoker", "It can also infect humans and heal the infected with the hammer."));
		break;
	case EPlayerClass::Boomer:
		AddLine(_C("Boomer", "The Boomer explodes when it attacks."));
		AddLine(_C("Boomer", "All humans affected by the explosion become infected."));
		AddLine(_C("Boomer", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Hunter:
		AddLine(_C("Hunter", "The Hunter can jump two times in the air and has some resistance to"
							 " knock-backs."));
		AddLine(_C("Hunter", "It can infect humans and heal the infected with the hammer."));
		AddLine(_C("Hunter", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Bat:
		AddLine(_C("Bat", "Bat can jump endlessly in the air but it cannot infect humans."));
		AddLine(_C("Bat", "Instead, it can hammer humans to steal their health and heal itself."));
		AddLine(_C("Bat", "The hammer is also useful for healing the infected."));
		AddLine(_C("Bat", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Ghost:
		AddLine(_C("Ghost", "The Ghost is invisible until a human comes nearby, it takes damage,"
							" or it uses the hammer."));
		AddLine(_C("Ghost", "It can infect humans and heal the infected with the hammer."));
		AddLine(_C("Ghost", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Spider:
		AddLine(_C("Spider", "The Spider has a web hook that automatically grabs any human touching it."));
		AddLine(_C("Spider", "The web hook mode can be toggled by switching the weapon."));
		AddLine(_C("Spider", "In both modes the hook inflicts 1 damage point per second and can"
							 " grab a human for longer."));
		break;
	case EPlayerClass::Ghoul:
		AddLine(_C("Ghoul", "The Ghoul can devour anything that has died nearby, which makes it"
							" stronger, faster and more resistant."));
		AddLine(_C("Ghoul", "It digests the fodder over time, going back to the normal state."
							" Some nourishment is also lost on death."));
		AddLine(_C("Ghoul", "Ghoul can infect humans and heal the infected with the hammer."));
		AddLine(_C("Ghoul", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Slug:
		AddLine(_C("Slug", "The Slug can make the ground and walls toxic by spreading slime with the hammer."));
		AddLine(_C("Slug", "The slime heals the infected and deals damage to humans."));
		AddLine(_C("Slug", "Slug can infect humans and heal the infected with the hammer."));
		AddLine(_C("Slug", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Voodoo:
		AddLine(_C("Voodoo", "The Voodoo does not die immediately when killed but instead enters"
							 " Spirit mode for a short time."));
		AddLine(_C("Voodoo", "While in Spirit mode it cannot be killed. When the time is up it finally dies."));
		AddLine(_C("Voodoo", "Voodoo can infect humans and heal the infected with the hammer."));
		AddLine(_C("Voodoo", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Witch:
		AddLine(_C("Witch", "The Witch can provide a spawn point for the infected."));
		AddLine(_C("Witch", "If the Witch dies, it disappears and is replaced by another class."));
		AddLine(_C("Witch", "Witch can infect humans and heal the infected with the hammer."));
		AddLine(_C("Witch", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Undead:
		AddLine(_C("Undead", "The Undead cannot die. Instead of dying, it gets frozen for 10 seconds."));
		AddLine(_C("Undead", "If an infected heals it, the freeze effect disappears."));
		AddLine(_C("Undead", "Undead can infect humans and heal the infected with the hammer."));
		AddLine(_C("Undead", "It can also inflict 1 damage point per second by hooking humans."));
		break;
	case EPlayerClass::Tank:
		AddLine(_C("Tank", "The Tank has a damage resistance and a stunning hammer with increased range and force."));
		AddLine(_C("Tank", "On the other hand, the movement speed and the hook lenght are reduced."));
		break;
	case EPlayerClass::Spitter:
		AddLine(_C("Spitter", "The Tank has a damage resistance and a stunning hammer with increased range and force."));
		AddLine(_C("Spitter", "On the other hand, the movement speed and the hook lenght are reduced."));
		break;
	}

	return true;
}

void CIcGameController::SnapMapMenu(int SnappingClient, CNetObj_GameInfo *pGameInfoObj)
{
	if(SnappingClient < 0)
		return;

	const CIcPlayer *pPlayer = GetPlayer(SnappingClient);
	if(!pPlayer)
		return;

	if(pPlayer->MapMenu() != 1)
		return;

	// Generate class mask
	int ClassMask = 0;
	{
		int Defender = 0;
		int Medic = 0;
		int Hero = 0;
		int Support = 0;

		CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
		while(Iter.Next())
		{
			switch(Iter.Player()->GetClass())
			{
			case EPlayerClass::Ninja:
			case EPlayerClass::Mercenary:
			case EPlayerClass::Sniper:
				Support++;
				break;
			case EPlayerClass::Engineer:
			case EPlayerClass::Soldier:
			case EPlayerClass::Scientist:
			case EPlayerClass::Biologist:
				Defender++;
				break;
			case EPlayerClass::Medic:
				Medic++;
				break;
			case EPlayerClass::Hero:
				Hero++;
				break;
			case EPlayerClass::Looper:
			case EPlayerClass::Artillery:
				Defender++;
				break;
			default:
				break;
			}
		}

		if(Defender < Config()->m_InfDefenderLimit)
			ClassMask |= CMapConverter::MASK_DEFENDER;
		if(Medic < Config()->m_InfMedicLimit)
			ClassMask |= CMapConverter::MASK_MEDIC;
		if(Hero < Config()->m_InfHeroLimit)
			ClassMask |= CMapConverter::MASK_HERO;
		if(Support < Config()->m_InfSupportLimit)
			ClassMask |= CMapConverter::MASK_SUPPORT;
	}

	const int Item = pPlayer->m_MapMenuItem;
	const int Page = CMapConverter::TIMESHIFT_MENUCLASS + 3 * ((Item + 1) + ClassMask * CMapConverter::TIMESHIFT_MENUCLASS_MASK) + 1;

	const double PageShift = static_cast<double>(Page * Server()->GetTimeShiftUnit()) / 1000.0f;
	const double SecondsPassed = static_cast<double>(GetRoundTick()) / Server()->TickSpeed();
	const double CycleShift = fmod(SecondsPassed, Server()->GetTimeShiftUnit() / 1000.0);
	const int TimeShift = (PageShift + CycleShift) * Server()->TickSpeed();

	pGameInfoObj->m_RoundStartTick = Server()->Tick() - TimeShift;
	pGameInfoObj->m_TimeLimit += (TimeShift / Server()->TickSpeed()) / 60;
}

void CIcGameController::FallInLoveIfInfectedEarly(CIcCharacter *pCharacter)
{
	if(!pCharacter)
		return;

	if(IsInfectionStarted())
		return;

	const int RemainingTicks = m_RoundStartTick + Server()->TickSpeed() * GetInfectionDelay() - Server()->Tick();
	const float LoveDuration = RemainingTicks / static_cast<float>(Server()->TickSpeed()) + 0.25;

	pCharacter->LoveEffect(LoveDuration);
}

void CIcGameController::RewardTheKillers(CIcCharacter *pVictim, const DeathContext &Context)
{
	// do scoreing
	if(Context.Killer < 0)
		return;

	const int Weapon = DamageTypeToWeapon(Context.DamageType);
	if(Weapon == WEAPON_GAME)
		return;

	CIcPlayer *pKiller = GetPlayer(Context.Killer);
	if(!pKiller)
		return;

	CIcPlayer *pAssistant = GetPlayer(Context.Assistant);

	if(pAssistant && (pAssistant->IsHuman() == pVictim->IsHuman()))
	{
		// Do not reward the victim teammates-assistants
		pAssistant = nullptr;
	}

	if(pKiller == pVictim->GetPlayer())
	{
		if(pKiller->IsHuman())
		{
			Server()->RoundStatistics()->OnScoreEvent(pKiller->GetCid(), EScoreEvent::HUMAN_SUICIDE, pKiller->GetClass(), Server()->ClientName(pKiller->GetCid()), Console());
		}
	}
	else
	{
		if(CIcCharacter *pKillerCharacter = pKiller->GetCharacter())
		{
			// set attacker's face to happy (taunt!)
			pKillerCharacter->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
		}
	}

	if(pKiller->IsHuman() == pVictim->IsHuman())
	{
		// Do not process self or team kills
		return;
	}

	if(pVictim->IsInfected())
	{
		const EPlayerClass VictimClass = static_cast<EPlayerClass>(pVictim->GetPlayerClass());
		EScoreEvent ScoreEvent = EScoreEvent::KILL_INFECTED;
		const bool ClassSpecialProcessingEnabled = (GetRoundType() != ERoundType::Fun) || (GetPlayerClassProbability(VictimClass) == 0);
		if(ClassSpecialProcessingEnabled)
		{
			switch(VictimClass)
			{
			case EPlayerClass::Witch:
				ScoreEvent = EScoreEvent::KILL_WITCH;
				GameServer()->SendChatTarget_Localization(pKiller->GetCid(), CHATCATEGORY_SCORE, _("You have killed a witch, +5 points"), nullptr);
				break;
			case EPlayerClass::Undead:
				ScoreEvent = EScoreEvent::KILL_UNDEAD;
				GameServer()->SendChatTarget_Localization(pKiller->GetCid(), CHATCATEGORY_SCORE, _("You have killed an undead! +5 points"), nullptr);
				break;
			default:
				break;
			}
		}

		Server()->RoundStatistics()->OnScoreEvent(pKiller->GetCid(), ScoreEvent, pKiller->GetClass(), Server()->ClientName(pKiller->GetCid()), Console());
		GameServer()->SendScoreSound(pKiller->GetCid());
	}

	pKiller->GetCharacterClass()->OnKilledCharacter(pVictim, Context);
	if(pAssistant)
	{
		pAssistant->GetCharacterClass()->OnKilledCharacter(pVictim, Context);
	}

	// Always reward the freezer
	const int VictimFreezer = pVictim->GetFreezer();
	if(VictimFreezer >= 0 && VictimFreezer != Context.Killer && VictimFreezer != Context.Assistant)
	{
		if(CIcPlayer *pFreezer = GetPlayer(VictimFreezer))
		{
			pFreezer->GetCharacterClass()->OnKilledCharacter(pVictim, Context);
		}
	}
}

int CIcGameController::OnCharacterDeath(class CCharacter *pAbstractVictim, class CPlayer *pAbstractKiller, int Weapon)
{
	dbg_msg("server", "CRITICAL ERROR: CIcGameController::OnCharacterDeath(class CCharacter *, class CPlayer *, int) must never be called");
	return 0;
}

void CIcGameController::OnIcCharacterDeath(CIcCharacter *pVictim, DeathContext *pContext)
{
	const EDamageType DamageType = pContext->DamageType;
	const int Killer = pContext->Killer;
	const int Assistant = pContext->Assistant;
	const char *pDamageTypeStr = toString(DamageType);

	dbg_msg("server", "OnCharacterDeath: victim=%d damage_type=%s killer=%d assistant=%d", pVictim->GetCid(), pDamageTypeStr, Killer, Assistant);

	RunCallback(Lua()->GetLuaState(), "on_character_death", pVictim->GetCid(), pContext->Killer, pDamageTypeStr);

	RewardTheKillers(pVictim, *pContext);

	const int Weapon = DamageTypeToWeapon(DamageType);
	static constexpr icArray<EDamageType, 4> BadReasonsToDie = {
		EDamageType::GAME, // Disconnect, joining spec, etc
		EDamageType::KILL_COMMAND, // Self kill
		EDamageType::GAME_FINAL_EXPLOSION,
	};
	if(!BadReasonsToDie.Contains(DamageType) && (Killer != pVictim->GetCid()))
	{
		if(pVictim->IsHuman())
		{
			const CIcPlayer *pKiller = GetPlayer(pContext->Killer);
			if(pKiller && pKiller->IsInfected() && pKiller->GetCharacter())
			{
				pVictim->GetPlayer()->SetSpecialCameraTargetCid(pKiller->GetCid(), 5.0);
			}
		}

		// Find the nearest ghoul
		for(TEntityPtr<CIcCharacter> p = GameWorld()->FindFirst<CIcCharacter>(); p; ++p)
		{
			if(p->GetPlayerClass() != EPlayerClass::Ghoul || p.data() == pVictim)
				continue;
			if(p->GetClass() && p->GetClass()->GetGhoulPercent() >= 1.0f)
				continue;

			const float Len = distance(p->m_Pos, pVictim->m_Pos);

			if(p && Len < 800.0f)
			{
				const int Points = (pVictim->IsInfected() ? 8 : 14);
				new CFlyingPoint(GameServer(), pVictim->m_Pos, p->GetCid(), Points, pVictim->Velocity());
			}
		}
	}

	static constexpr icArray<EDamageType, 2> ReasonsForNoDrop = {
		EDamageType::GAME_INFECTION,
		EDamageType::GAME,
	};
	if(!ReasonsForNoDrop.Contains(DamageType))
	{
		MaybeDropPickup(pVictim);
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%s' victim='%s' weapon=%d",
		Server()->ClientName(Killer),
		Server()->ClientName(pVictim->GetCid()), Weapon);
	Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// It is important to SendKillMessage before GetClass()->OnCharacterDeath() to keep the correct kill order
	SendKillMessage(pVictim->GetCid(), *pContext);

	if(pVictim->GetClass())
	{
		pVictim->GetClass()->OnCharacterDeath(DamageType);
	}

	INFECTION_TYPE InfectionType = INFECTION_TYPE::REGULAR;
	bool ClassSpecialProcessingEnabled = true;

	const EPlayerClass VictimClass = static_cast<EPlayerClass>(pVictim->GetPlayerClass());
	if(DamageType == EDamageType::GAME)
	{
		ClassSpecialProcessingEnabled = false;
	}
	else if((GetRoundType() == ERoundType::Fun) && !pVictim->IsHuman() && GetPlayerClassProbability(VictimClass))
	{
		ClassSpecialProcessingEnabled = false;
	}

	if(ClassSpecialProcessingEnabled)
	{
		switch(VictimClass)
		{
		case EPlayerClass::Witch:
			GameServer()->SendBroadcast_Localization(-1, EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, _("The witch is dead"), nullptr);
			GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN);
			InfectionType = INFECTION_TYPE::RESTORE_INF_CLASS;
			break;
		case EPlayerClass::Undead:
			GameServer()->SendBroadcast_Localization(-1, EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, _("The undead is finally dead"), nullptr);
			GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN);
			InfectionType = INFECTION_TYPE::RESTORE_INF_CLASS;
			break;
		default:
			break;
		}
	}
	else
	{
		// Still send the traditional 'whoosh' sound
		if(VictimClass == EPlayerClass::Witch)
		{
			GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN);
		}
	}

	// Do not infect on disconnect or joining spec
	bool Infect = DamageType != EDamageType::GAME;
	if(GetRoundType() == ERoundType::Survival)
	{
		float DisabledForDuration = Config()->m_InfSurvivalDeadSeconds;
		if(pVictim->IsHuman() && !pVictim->GetPlayer()->IsBot() && DisabledForDuration)
		{
			pContext->KeepCharacter = true;
			Infect = false;
			pVictim->SetDeadForDuration(DisabledForDuration);
		}
	}

	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		Infect = false;
	}
	if(Infect)
	{
		pVictim->GetPlayer()->StartInfection(pContext->Killer, InfectionType);
	}

	bool SelfKill = false;
	switch(DamageType)
	{
	case EDamageType::TURRET_DESTRUCTION:
	case EDamageType::DEATH_TILE:
	case EDamageType::KILL_COMMAND:
		SelfKill = true;
		break;
	default:
		SelfKill = Killer == pVictim->GetCid();
		break;
	}

	int RespawnDelay = 0;
	if(SelfKill)
	{
		// Wait 3.0 secs in a case of selfkill
		RespawnDelay = Server()->TickSpeed() * 3.0f;
	}
	else
	{
		RespawnDelay = Server()->TickSpeed() * 0.5f;
	}

	if(GetRoundType() == ERoundType::Fast)
	{
		RespawnDelay *= 0.5;
	}

	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		RespawnDelay *= 0.5;
	}

	if(m_Warmup > 0)
	{
		RespawnDelay = Server()->TickSpeed() * 0.2f;
	}

	pVictim->GetPlayer()->m_RespawnTick = Server()->Tick() + RespawnDelay;

	if(pContext->DamageType == EDamageType::INFECTION_TILE)
	{
		const int FreezeDuration = m_Warmup > 0 ? 0 : Config()->m_InfInfzoneFreezeDuration;
		if(FreezeDuration > 0)
		{
			pVictim->Freeze(FreezeDuration, pContext->Killer, FREEZEREASON_INFECTION);
		}
	}

	CIcPlayer *pVictimPlayer = pVictim->GetPlayer();
	if(pVictimPlayer && (DamageType != EDamageType::GAME))
	{
		if(pVictimPlayer->IsBot())
		{
			CBotPlayer *pBot = static_cast<CBotPlayer *>(pVictimPlayer);
			if(GetRoundType() == ERoundType::Survival)
			{
				float Delay = pBot->GetRespawnInterval();
				if(!Delay)
				{
					Delay = Config()->m_InfSurvivalInfectedSpawningDelay;
				}
				Delay += -0.5 + random_float();
				Delay = maximum(0.5f, Delay);
				pBot->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() * Delay;
			}

			pBot->OnKilled();
		}
	}
}

void CIcGameController::OnIcCharacterSpawned(CIcCharacter *pCharacter, const SpawnContext &Context)
{
	RunCallback(Lua()->GetLuaState(), "on_character_spawned", pCharacter->GetCid(), toString(Context.SpawnType));

	CIcPlayer *pPlayer = pCharacter->GetPlayer();
	if(!IsInfectionStarted() && pPlayer->RandomClassChoosen())
	{
		pCharacter->GiveRandomClassSelectionBonus();
	}

	if(pCharacter->IsInfected() && (GetRoundType() != ERoundType::HideAndSeek))
	{
		FallInLoveIfInfectedEarly(pCharacter);
		pCharacter->SetHealthArmor(10, InfectedBonusArmor());
		if(Context.SpawnType == SpawnContext::MapSpawn)
		{
			const float Duration = g_Config.m_InfSpawnProtectionTime / 1000.0f;
			pCharacter->GrantSpawnProtection(Duration);
		}
	}

	if(GetRoundType() == ERoundType::Fun)
	{
		int InfectionTick = GetInfectionStartTick();
		if((Server()->Tick() < InfectionTick) && pCharacter->GetPlayerClass() == EPlayerClass::None)
		{
			pPlayer->SetClass(ChooseHumanClass(pPlayer));
		}
	}
	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		if(!IsInfectionStarted() && pCharacter->GetPlayerClass() == EPlayerClass::None)
		{
			pPlayer->SetClass(ChooseHumanClass(pPlayer));
		}

		ApplyHideAndSeekAttributes(pPlayer);
	}
}

void CIcGameController::OnCharacterBackFromDead(CIcCharacter *pCharacter)
{
	pCharacter->SetHealthArmor(1, 0);
}

void CIcGameController::OnClassChooserRequested(CIcCharacter *pCharacter) const
{
	if(Config()->m_InfSurvivalMode)
	{
		if(GetRoundType() != ERoundType::Survival)
		{
			return;
		}
	}

	CIcPlayer *pPlayer = pCharacter->GetPlayer();
	if(GetRoundType() == ERoundType::Fun)
	{
		pPlayer->SetRandomClassChoosen();
		// Read this as "player didn't choose this class"
		pCharacter->GiveRandomClassSelectionBonus();
		pPlayer->CloseMapMenu();
		return;
	}

	const EPlayerClass PreferredClass = pPlayer->GetPreferredClass();
	if(!IsClassChooserEnabled() || (PreferredClass != EPlayerClass::Invalid))
	{
		pPlayer->SetClass(ChooseHumanClass(pPlayer));

		if(PreferredClass == EPlayerClass::Random)
		{
			pPlayer->SetRandomClassChoosen();

			if(IsClassChooserEnabled())
			{
				pCharacter->GiveRandomClassSelectionBonus();
			}
		}
	}
	else
	{
		pPlayer->OpenMapMenu(1);
	}
}

void CIcGameController::CheckRoundFailed()
{
	if(m_Warmup)
		return;

	if(!IsWinCheckEnabled())
		return;

	if(IsGameOver())
		return;

	if(Config()->m_InfTrainingMode)
		return;

	if(GetRoundType() == ERoundType::Survival)
		return;

	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	if((NumHumans == 0) && (NumInfected == 0))
		return;

	ROUND_CANCELATION_REASON Reason = ROUND_CANCELATION_REASON::INVALID;

	if(NumInfected == 0)
	{
		if(m_RoundStartTick + Server()->TickSpeed() * (GetInfectionDelay() + 1) <= Server()->Tick())
		{
			Reason = ROUND_CANCELATION_REASON::ALL_INFECTED_LEFT_THE_GAME;
		}
	}

	if(NumHumans == 0)
	{
		CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
		bool HasNonGameInfectionCauses = false;
		while(Iter.Next())
		{
			const CIcPlayer *pPlayer = Iter.Player();
			if(pPlayer->InfectionCause() != INFECTION_CAUSE::GAME)
			{
				HasNonGameInfectionCauses = true;
				break;
			}
		}

		if(HasNonGameInfectionCauses)
		{
			// Okay, inf won
		}
		else
		{
			Reason = ROUND_CANCELATION_REASON::EVERYONE_INFECTED_BY_THE_GAME;
		}
	}

	if(Reason != ROUND_CANCELATION_REASON::INVALID)
	{
		// Round failed: all infected left the game
		// The infected didn't infect anyone. Cancel the round.
		CancelTheRound(Reason);
	}
}

float CIcGameController::GetMaxInactiveTimeSeconds(const CPlayer *pPlayer) const
{
	const CIcPlayer *pInfPlayer = CIcPlayer::GetInstance(pPlayer);

	const int HumanMaxInactiveTimeSecs = Config()->m_InfInactiveHumansKickTime ? Config()->m_InfInactiveHumansKickTime : Config()->m_SvInactiveKickTime * 60;
	const int InfectedMaxInactiveTimeSecs = Config()->m_InfInactiveInfectedKickTime ? Config()->m_InfInactiveInfectedKickTime : Config()->m_SvInactiveKickTime * 60;

	return pInfPlayer->IsHuman() ? HumanMaxInactiveTimeSecs : InfectedMaxInactiveTimeSecs;
}

void CIcGameController::DoWincheck()
{
	if(!IsWinCheckEnabled())
		return;

	if(Config()->m_InfTrainingMode)
		return;

	if(!m_InfectedStarted)
	{
		return;
	}

	int NumHumans = 0;
	int NumInfected = 0;
	GetPlayerCounter(-1, NumHumans, NumInfected);

	bool VictoryConditionsMet = false;
	bool NeedFinalExplosion = false;
	bool TimeIsOut = false;
	const int Seconds = (Server()->Tick() - m_RoundStartTick) / static_cast<float>(Server()->TickSpeed());
	if(GetTimeLimitMinutes() > 0 && Seconds >= GetTimeLimitSeconds())
	{
		TimeIsOut = true;
	}

	switch(GetRoundType())
	{
	case ERoundType::Survival:
		if(SurvivalHumansWinConditionsMet())
		{
			VictoryConditionsMet = true;
			NeedFinalExplosion = true;
		}
		else if((NumHumans == 0) || SurvivalInfectedWinConditionsMet())
		{
			VictoryConditionsMet = true;
		}
		break;
	case ERoundType::HideAndSeek:
		if(TimeIsOut || (m_FinalExplosionState == EFinalExplosionState::Finished))
		{
			VictoryConditionsMet = true;
		}
		break;
	default:
		// One infected can win in some rounds; we have a check if this is a valid situation in CheckRoundFailed()
		if(NumHumans == 0 && NumInfected >= 1)
		{
			VictoryConditionsMet = true;
		}
		else if(TimeIsOut)
		{
			VictoryConditionsMet = true;
			NeedFinalExplosion = true;
		}
		break;
	}

	if(!VictoryConditionsMet)
	{
		return;
	}

	// Start the final explosion if the time is over
	if(NeedFinalExplosion)
	{
		EnsureFinalExplosionIsStarted();
	}

	// If no more explosions, game over, decide who win
	if(!NeedFinalExplosion || (m_FinalExplosionState == EFinalExplosionState::Finished))
	{
		AnnounceTheWinner(NumHumans);
	}
}

bool CIcGameController::IsSpawnable(vec2 Pos, EZoneTele TeleZoneIndex)
{
	// First check if there is a tee too close
	CCharacter *aEnts[MAX_CLIENTS];
	const int Num = GameWorld()->FindEntities(Pos, 64, reinterpret_cast<CEntity **>(aEnts), MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

	for(int c = 0; c < Num; ++c)
	{
		if(distance(aEnts[c]->m_Pos, Pos) <= 60)
			return false;
	}

	// Check the center
	EZoneTele TeleIndex = GetTeleportZoneValueAt(Pos);
	if(GameServer()->Collision()->CheckPoint(Pos))
		return false;
	if((TeleZoneIndex != EZoneTele::Null) && (TeleIndex == TeleZoneIndex))
		return false;

	// Check the border of the tee. Kind of extrem, but more precise
	for(int i = 0; i < 16; i++)
	{
		const float Angle = i * (2.0f * pi / 16.0f);
		vec2 CheckPos = Pos + vec2(cos(Angle), sin(Angle)) * 30.0f;
		TeleIndex = GetTeleportZoneValueAt(CheckPos);
		if(GameServer()->Collision()->CheckPoint(CheckPos))
			return false;
		if((TeleZoneIndex != EZoneTele::Null) && (TeleIndex == TeleZoneIndex))
			return false;
	}

	return true;
}

bool CIcGameController::TryRespawn(CIcPlayer *pPlayer, SpawnContext *pContext)
{
	// spectators can't spawn
	if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		return false;

	// Deny any spawns during the World ResetRequested because the new Characters
	// are going to be destroyed during this IGameServer::Tick().
	// (and it may break the auto class selection)
	if(GameWorld()->m_ResetRequested)
	{
		return false;
	}

	if(m_InfectedStarted && (GetRoundType() != ERoundType::HideAndSeek))
		pPlayer->StartInfection();

	if(pPlayer->IsInfected() && (m_FinalExplosionState != EFinalExplosionState::NotStarted))
		return false;

	std::optional<std::uint16_t> WantedSpawnIndex;
	std::optional<std::uint8_t> WantedWitchCid;
	if(GetRoundType() == ERoundType::Survival && pPlayer->IsInfected())
	{
		if(!IsInfectionStarted())
		{
			return false;
		}

		if(!pPlayer->IsBot())
		{
			GameServer()->SendBroadcast(pPlayer->GetCid(), "You are dead and have to wait for others",
				EBroadcastPriority::GAMEANNOUNCE, BROADCAST_DURATION_REALTIME);
			return false;
		}

		const SurvivalWaveConfiguration *pWaveConf = GetCurrentSurvivalWaveConfiguration();
		const CBaseBotPlayer *pBot = static_cast<CBaseBotPlayer *>(pPlayer);
		const std::optional<std::size_t> ConfigId = pBot->GetBotConfigId();
		if(ConfigId.has_value() && ConfigId.value() < pWaveConf->BotConfigurations.Size())
		{
			const SurvivalBotConfiguration &BotConf = pWaveConf->BotConfigurations[ConfigId.value()];

			if(BotConf.ScriptedSpawn)
			{
				if(Lua()->HasGlobalCallable("Get_character_spawn_position"))
				{
					std::optional<vec2> Pos = RunCallbackWithResult<vec2>(Lua()->GetLuaState(), "Get_character_spawn_position", pPlayer);
					if(Pos.has_value())
					{
						pContext->SpawnPos = Pos.value();
						pContext->SpawnType = SpawnContext::Scripted;
						return true;
					}
					else
					{
						return false;
					}
				}
				else
				{
					// warning
				}
			}
			WantedSpawnIndex = BotConf.SpawnPointId;
			WantedWitchCid = BotConf.SpawnWitchId;
		}
	}

	if(m_InfectedStarted && pPlayer->IsInfected())
	{
		if(WantedWitchCid.has_value())
		{
			const CIcCharacter *pCharacter = GetCharacter(WantedWitchCid.value());
			if(pCharacter && pCharacter->IsAlive())
			{
				if(pCharacter->IsFrozen() || pCharacter->IsSleeping())
					return false;

				const CInfClassInfected *pInfected = CInfClassInfected::GetInstance(pCharacter);

				if(pInfected->FindWitchSpawnPosition(pContext->SpawnPos))
				{
					pContext->SpawnType = SpawnContext::WitchSpawn;
					return true;
				}
			}
		}

		if(random_prob(Config()->m_InfProbaSpawnNearWitch / 100.0f))
		{
			CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
			while(Iter.Next())
			{
				if(Iter.Player()->GetCid() == pPlayer->GetCid())
					continue;
				if(Iter.Player()->GetClass() != EPlayerClass::Witch)
					continue;

				const CIcCharacter *pCharacter = Iter.Player()->GetCharacter();
				if(!pCharacter || !pCharacter->IsAlive())
					continue;

				if(pCharacter->IsFrozen() || pCharacter->IsSleeping())
					continue;

				const CInfClassInfected *pInfected = CInfClassInfected::GetInstance(pCharacter);

				if(pInfected->FindWitchSpawnPosition(pContext->SpawnPos))
				{
					pContext->SpawnType = SpawnContext::WitchSpawn;
					return true;
				}
			}
		}
	}

	const int Type = (pPlayer->IsInfected() ? 0 : 1);

	if(m_avSpawnPoints[Type].size() == 0)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", "The map has no spawn points");
		return false;
	}

	icArray<CControlPoint *, 10> aControlPoints;

	// Find control points
	for(TEntityPtr<CControlPoint> p = GameWorld()->FindFirst<CControlPoint>(); p; ++p)
	{
		if(p->IsMarkedForDestroy())
			continue;
		if(!p->IsTaken() || !p->IsReady() || p->IsBlocked())
			continue;
		if(pPlayer->IsInfected() != p->IsInfected())
			continue;

		aControlPoints.Add(p);
		if(aControlPoints.Size() == aControlPoints.Capacity())
			break;
	}

	std::size_t Val = aControlPoints.IsEmpty() ? 0 : random_int(0, aControlPoints.Size());
	if(Val < aControlPoints.Size())
	{
		// Picked a CP
		pContext->SpawnPos = aControlPoints.At(Val)->GetPos();
		pContext->SpawnType = SpawnContext::ControlPoint;
		return true;
	}

	// get spawn point
	const std::vector<vec2> &vSpawnPoints = m_avSpawnPoints[Type];
	const std::vector<int> &vEnablements = m_EnabledSpawnPoints[Type];
	const bool CustomPoints = !vEnablements.empty();
	const std::size_t Count = CustomPoints ? vEnablements.size() : vSpawnPoints.size();
	const int RandomShift = random_int(0, Count - 1);
	for(std::size_t i = 0; i < Count; i++)
	{
		const int RandomPointOffset = (i + RandomShift) % Count;
		std::size_t PosIndex = 0;
		if(WantedSpawnIndex.has_value())
		{
			if(WantedSpawnIndex.value() < Count)
			{
				PosIndex = WantedSpawnIndex.value();
			}
		}
		else if(CustomPoints)
		{
			PosIndex = vEnablements.at(RandomPointOffset);
		}
		else
		{
			PosIndex = RandomPointOffset;
		}
		const vec2 &Pos = m_avSpawnPoints[Type][PosIndex];
		if(!IsSpawnable(Pos, EZoneTele::Null))
			continue;

		pContext->SpawnPos = Pos;
		pContext->SpawnType = SpawnContext::MapSpawn;
		return true;
	}

	return false;
}

EPlayerClass CIcGameController::ChooseHumanClass(const CIcPlayer *pPlayer) const
{
	if(Lua()->HasGlobalCallable("choose_human_class"))
	{
		std::optional<std::string> c = RunCallbackWithResult<std::string>(Lua()->GetLuaState(), "choose_human_class", pPlayer);
		if(c.has_value() && !c.value().empty())
		{
			EPlayerClass Class = fromString<EPlayerClass>(c.value().c_str());
			if(IsHumanClass(Class))
			{
				return Class;
			}
			else
			{
				dbg_msg("lua/cb", "choose_human_class() exists but return invalid value '%s'", c.value().c_str());
			}
		}
	}

	double Probability[NB_PLAYERCLASS]{};
	auto GetClassProbabilityRef = [&Probability](EPlayerClass PlayerClass) -> double & {
		return Probability[static_cast<int>(PlayerClass)];
	};

	int AvailableClasses = 0;
	for(const EPlayerClass PlayerClass : AllHumanClasses)
	{
		double &ClassProbability = GetClassProbabilityRef(PlayerClass);
		ClassProbability = GetPlayerClassEnabled(PlayerClass) ? 1.0f : 0.0f;
		if(GetRoundType() != ERoundType::Fun)
		{
			const CLASS_AVAILABILITY Availability = GetPlayerClassAvailability(PlayerClass, pPlayer);
			switch(Availability)
			{
			case CLASS_AVAILABILITY::PICKED_PREVIOUSLY:
				ClassProbability *= 0.125f;
				break;
			case CLASS_AVAILABILITY::AVAILABLE:
				break;
			default:
				ClassProbability = 0.0f;
			}
		}

		if(ClassProbability > 0)
		{
			AvailableClasses++;
		}
	}

	const EPlayerClass PreferredClass = pPlayer->GetPreferredClass();
	if(PreferredClass != EPlayerClass::Invalid)
	{
		if(PreferredClass != EPlayerClass::Random)
		{
			if(GetClassProbabilityRef(PreferredClass) > 0)
			{
				return PreferredClass;
			}
		}
	}

	// Random is not fair enough. We keep the last classes took by the player, and avoid to give those again
	if(GetRoundType() != ERoundType::Fun)
	{
		if(AvailableClasses > 1)
		{
			// if normal round is being played
			const EPlayerClass PrevClass = pPlayer->GetPreviouslyPickedClass();
			if(PrevClass != EPlayerClass::Invalid)
			{
				GetClassProbabilityRef(PrevClass) = 0.0f;
			}
		}
	}

	int Result = random_distribution(Probability, Probability + NB_PLAYERCLASS);
	return static_cast<EPlayerClass>(Result);
}

EPlayerClass CIcGameController::ChooseInfectedClass(const CIcPlayer *pPlayer) const
{
	if(Lua()->HasGlobalCallable("choose_infected_class"))
	{
		std::optional<std::string> c = RunCallbackWithResult<std::string>(Lua()->GetLuaState(), "choose_infected_class", pPlayer);
		if(c.has_value() && !c.value().empty())
		{
			EPlayerClass Class = fromString<EPlayerClass>(c.value().c_str());
			if(IsInfectedClass(Class))
			{
				return Class;
			}
			else
			{
				dbg_msg("lua/cb", "choose_infected_class() exists but return invalid value '%s'", c.value().c_str());
			}
		}
	}

	// if(pPlayer->InfectionType() == INFECTION_TYPE::RESTORE_INF_CLASS)
	{
		const EPlayerClass PrevClass = pPlayer->GetPreviousInfectedClass();
		if(PrevClass != EPlayerClass::Invalid)
		{
			return PrevClass;
		}
	}

	// Get information about existing infected
	int nbInfected = 0;
	icArray<int, NB_PLAYERCLASS> nbClass;
	nbClass.Resize(NB_PLAYERCLASS);

	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	int PlayersCount = 0;
	while(Iter.Next())
	{
		++PlayersCount;
		const EPlayerClass AnotherPlayerClass = Iter.Player()->GetClass();
		const int Index = static_cast<int>(AnotherPlayerClass);
		if(Iter.Player()->IsInfected())
			nbInfected++;
		nbClass[Index]++;
	}

	const int InitiallyInfected = GetMinimumInfectedForPlayers(PlayersCount);

	double Probability[NB_PLAYERCLASS]{};
	auto GetClassProbabilityRef = [&Probability](EPlayerClass PlayerClass) -> double & {
		return Probability[static_cast<int>(PlayerClass)];
	};

	for(EPlayerClass PlayerClass : AllInfectedClasses)
	{
		double &ClassProbability = GetClassProbabilityRef(PlayerClass);
		ClassProbability = GetPlayerClassProbability(PlayerClass);
		if(GetRoundType() == ERoundType::Fun)
		{
			// We care only about the class enablement
			continue;
		}

		switch(PlayerClass)
		{
		case EPlayerClass::Bat:
			if(nbInfected <= InitiallyInfected)
			{
				// We can't just set the proba to 0, because it would break a config
				// with all classes except the Bat set to 0.
				ClassProbability = ClassProbability / 10000.0;
			}
			break;
		case EPlayerClass::Ghoul:
			if(nbInfected < Config()->m_InfGhoulThreshold)
				ClassProbability = 0;
			break;
		case EPlayerClass::Witch:
		case EPlayerClass::Undead:
			if((nbInfected <= 2) || nbClass[static_cast<int>(PlayerClass)] > 0)
				ClassProbability = 0;
			break;
		default:
			break;
		}
	}

	int Result = random_distribution(Probability, Probability + NB_PLAYERCLASS);
	const EPlayerClass Class = static_cast<EPlayerClass>(Result);

	const int Seconds = (Server()->Tick() - m_RoundStartTick) / static_cast<float>(Server()->TickSpeed());
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "infected victim='%s' duration='%d' class='%s'",
		Server()->ClientName(pPlayer->GetCid()), Seconds, toString(Class));
	Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	return Class;
}

float CIcGameController::GetWeaponForce(EInfclassWeapon WID) const
{
	return m_aInfWeaponForce[static_cast<std::size_t>(WID)];
}

void CIcGameController::SetWeaponForce(EInfclassWeapon WID, float Force)
{
	m_aInfWeaponForce[static_cast<std::size_t>(WID)] = Force;
}

int CIcGameController::GetFireDelay(EInfclassWeapon WID) const
{
	return m_InfFireDelay[static_cast<int>(WID)];
}

void CIcGameController::SetFireDelay(EInfclassWeapon WID, int Time)
{
	m_InfFireDelay[static_cast<int>(WID)] = Time;
}

int CIcGameController::GetAmmoRegenTime(EInfclassWeapon WID) const
{
	return m_InfAmmoRegenTime[static_cast<int>(WID)];
}

void CIcGameController::SetAmmoRegenTime(EInfclassWeapon WID, int Time)
{
	m_InfAmmoRegenTime[static_cast<int>(WID)] = Time;
}

int CIcGameController::GetMaxAmmo(EInfclassWeapon WID) const
{
	return m_InfMaxAmmo[static_cast<int>(WID)];
}

void CIcGameController::SetMaxAmmo(EInfclassWeapon WID, int n)
{
	m_InfMaxAmmo[static_cast<int>(WID)] = n;
}

void CIcGameController::InitWeapons()
{
	SetWeaponForce(EInfclassWeapon::NONE, 0);
	SetWeaponForce(EInfclassWeapon::HAMMER, 10);
	SetWeaponForce(EInfclassWeapon::GUN, 0);
	SetWeaponForce(EInfclassWeapon::SHOTGUN, 2);
	SetWeaponForce(EInfclassWeapon::GRENADE, 0);
	SetWeaponForce(EInfclassWeapon::LASER, 0);
	SetWeaponForce(EInfclassWeapon::NINJA, 10);
	SetWeaponForce(EInfclassWeapon::ENGINEER_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::SURVIVAL_NO_HOOK_GUN, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::SOLDIER_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::EXPLOSIVE_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::TELEPORT_GUN, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::HEALING_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::MEDIC_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::MEDIC_SHOTGUN, 6);
	SetWeaponForce(EInfclassWeapon::HERO_SHOTGUN, GetWeaponForce(EInfclassWeapon::SHOTGUN));
	SetWeaponForce(EInfclassWeapon::RICOCHET_SHOTGUN, GetWeaponForce(EInfclassWeapon::SHOTGUN));
	SetWeaponForce(EInfclassWeapon::BIOLOGIST_MINE_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::BIOLOGIST_GRENADE, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::LOOPER_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::LOOPER_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::HERO_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::HERO_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::SNIPER_RIFLE, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::NINJA_KATANA, GetWeaponForce(EInfclassWeapon::NINJA));
	SetWeaponForce(EInfclassWeapon::NINJA_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::POISON_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::MERCENARY_GUN, GetWeaponForce(EInfclassWeapon::GUN));
	SetWeaponForce(EInfclassWeapon::MERCENARY_UPGRADE_LASER, 0);
	SetWeaponForce(EInfclassWeapon::BLINDING_LASER, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::TRANQUILIZER_RIFLE, GetWeaponForce(EInfclassWeapon::LASER));
	SetWeaponForce(EInfclassWeapon::ARTILLERY_SHOTGUN, 7);
	SetWeaponForce(EInfclassWeapon::ARTILLERY_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));
	SetWeaponForce(EInfclassWeapon::ARTILLERY_LASER, GetWeaponForce(EInfclassWeapon::LASER));

	SetFireDelay(EInfclassWeapon::NONE, 0);
	SetFireDelay(EInfclassWeapon::HAMMER, 125);
	SetFireDelay(EInfclassWeapon::GUN, 125);
	SetFireDelay(EInfclassWeapon::SHOTGUN, 500);
	SetFireDelay(EInfclassWeapon::GRENADE, 500);
	SetFireDelay(EInfclassWeapon::LASER, 800);
	SetFireDelay(EInfclassWeapon::NINJA, 800);
	SetFireDelay(EInfclassWeapon::ENGINEER_LASER, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::SURVIVAL_NO_HOOK_GUN, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::SOLDIER_GRENADE, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::EXPLOSIVE_LASER, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::TELEPORT_GUN, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::HEALING_GRENADE, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::MEDIC_LASER, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::MEDIC_SHOTGUN, 250);
	SetFireDelay(EInfclassWeapon::HERO_SHOTGUN, 250);
	SetFireDelay(EInfclassWeapon::RICOCHET_SHOTGUN, 250);
	SetFireDelay(EInfclassWeapon::BIOLOGIST_MINE_LASER, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::BIOLOGIST_GRENADE, 1000);
	SetFireDelay(EInfclassWeapon::LOOPER_LASER, 250);
	SetFireDelay(EInfclassWeapon::LOOPER_GRENADE, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::HERO_LASER, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::HERO_GRENADE, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::SNIPER_RIFLE, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::NINJA_KATANA, GetFireDelay(EInfclassWeapon::NINJA));
	SetFireDelay(EInfclassWeapon::NINJA_GRENADE, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::POISON_GRENADE, GetFireDelay(EInfclassWeapon::GRENADE));
	SetFireDelay(EInfclassWeapon::MERCENARY_GUN, 50);
	SetFireDelay(EInfclassWeapon::MERCENARY_UPGRADE_LASER, 200);
	SetFireDelay(EInfclassWeapon::BLINDING_LASER, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::TRANQUILIZER_RIFLE, GetFireDelay(EInfclassWeapon::LASER));
	SetFireDelay(EInfclassWeapon::ARTILLERY_SHOTGUN, 500);
	SetFireDelay(EInfclassWeapon::ARTILLERY_GRENADE, 400);
	SetFireDelay(EInfclassWeapon::ARTILLERY_LASER, 500);

	SetAmmoRegenTime(EInfclassWeapon::NONE, 0);
	SetAmmoRegenTime(EInfclassWeapon::HAMMER, 0);
	SetAmmoRegenTime(EInfclassWeapon::GUN, 500);
	SetAmmoRegenTime(EInfclassWeapon::SHOTGUN, 0);
	SetAmmoRegenTime(EInfclassWeapon::GRENADE, 0);
	SetAmmoRegenTime(EInfclassWeapon::LASER, 0);
	SetAmmoRegenTime(EInfclassWeapon::NINJA, 0);

	SetAmmoRegenTime(EInfclassWeapon::ENGINEER_LASER, 6000);
	SetAmmoRegenTime(EInfclassWeapon::SURVIVAL_NO_HOOK_GUN, 15000);
	SetAmmoRegenTime(EInfclassWeapon::SOLDIER_GRENADE, 7000);
	SetAmmoRegenTime(EInfclassWeapon::EXPLOSIVE_LASER, 6000);
	SetAmmoRegenTime(EInfclassWeapon::TELEPORT_GUN, 10000);
	SetAmmoRegenTime(EInfclassWeapon::HEALING_GRENADE, 0);
	SetAmmoRegenTime(EInfclassWeapon::MEDIC_LASER, 600);
	SetAmmoRegenTime(EInfclassWeapon::MEDIC_SHOTGUN, 750);
	SetAmmoRegenTime(EInfclassWeapon::HERO_SHOTGUN, 750);
	SetAmmoRegenTime(EInfclassWeapon::HERO_LASER, 3000);
	SetAmmoRegenTime(EInfclassWeapon::HERO_GRENADE, 3000);
	SetAmmoRegenTime(EInfclassWeapon::SNIPER_RIFLE, 2000);
	SetAmmoRegenTime(EInfclassWeapon::POISON_GRENADE, 5000);
	SetAmmoRegenTime(EInfclassWeapon::MERCENARY_GUN, 125);
	SetAmmoRegenTime(EInfclassWeapon::MERCENARY_UPGRADE_LASER, 4000);
	SetAmmoRegenTime(EInfclassWeapon::NINJA_KATANA, 0);
	SetAmmoRegenTime(EInfclassWeapon::NINJA_GRENADE, 15000);
	SetAmmoRegenTime(EInfclassWeapon::BIOLOGIST_MINE_LASER, 175);
	SetAmmoRegenTime(EInfclassWeapon::BIOLOGIST_GRENADE, 15000);
	SetAmmoRegenTime(EInfclassWeapon::RICOCHET_SHOTGUN, 675);
	SetAmmoRegenTime(EInfclassWeapon::LOOPER_LASER, 500);
	SetAmmoRegenTime(EInfclassWeapon::LOOPER_GRENADE, 5000);
	SetAmmoRegenTime(EInfclassWeapon::BLINDING_LASER, 10000);
	SetAmmoRegenTime(EInfclassWeapon::TRANQUILIZER_RIFLE, 1000);
	SetAmmoRegenTime(EInfclassWeapon::ARTILLERY_SHOTGUN, 1500);
	SetAmmoRegenTime(EInfclassWeapon::ARTILLERY_GRENADE, 2500);
	SetAmmoRegenTime(EInfclassWeapon::ARTILLERY_LASER, 5000);

	SetMaxAmmo(EInfclassWeapon::NONE, -1);
	SetMaxAmmo(EInfclassWeapon::HAMMER, -1);
	SetMaxAmmo(EInfclassWeapon::GUN, 10);
	SetMaxAmmo(EInfclassWeapon::SHOTGUN, 10);
	SetMaxAmmo(EInfclassWeapon::GRENADE, 10);
	SetMaxAmmo(EInfclassWeapon::LASER, 10);
	SetMaxAmmo(EInfclassWeapon::NINJA, 10);
	SetMaxAmmo(EInfclassWeapon::ENGINEER_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::SURVIVAL_NO_HOOK_GUN, 3);
	SetMaxAmmo(EInfclassWeapon::EXPLOSIVE_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::TELEPORT_GUN, 3);
	SetMaxAmmo(EInfclassWeapon::SOLDIER_GRENADE, 10);
	SetMaxAmmo(EInfclassWeapon::HEALING_GRENADE, 10);
	SetMaxAmmo(EInfclassWeapon::MEDIC_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::MEDIC_SHOTGUN, 10);
	SetMaxAmmo(EInfclassWeapon::HERO_SHOTGUN, 10);
	SetMaxAmmo(EInfclassWeapon::HERO_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::HERO_GRENADE, 10);
	SetMaxAmmo(EInfclassWeapon::SNIPER_RIFLE, 10);
	SetMaxAmmo(EInfclassWeapon::NINJA_KATANA, -1);
	SetMaxAmmo(EInfclassWeapon::NINJA_GRENADE, 5);
	SetMaxAmmo(EInfclassWeapon::POISON_GRENADE, 8);
	SetMaxAmmo(EInfclassWeapon::MERCENARY_GUN, 40);
	SetMaxAmmo(EInfclassWeapon::MERCENARY_UPGRADE_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::BIOLOGIST_MINE_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::BIOLOGIST_GRENADE, 5);
	SetMaxAmmo(EInfclassWeapon::RICOCHET_SHOTGUN, 10);
	SetMaxAmmo(EInfclassWeapon::LOOPER_LASER, 20);
	SetMaxAmmo(EInfclassWeapon::LOOPER_GRENADE, 10);
	SetMaxAmmo(EInfclassWeapon::BLINDING_LASER, 10);
	SetMaxAmmo(EInfclassWeapon::TRANQUILIZER_RIFLE, 10);
	SetMaxAmmo(EInfclassWeapon::ARTILLERY_SHOTGUN, 10);
	SetMaxAmmo(EInfclassWeapon::ARTILLERY_GRENADE, 8);
	SetMaxAmmo(EInfclassWeapon::ARTILLERY_LASER, 5);

	// Infected weapons
	SetWeaponForce(EInfclassWeapon::JAWS, GetWeaponForce(EInfclassWeapon::HAMMER));
	SetWeaponForce(EInfclassWeapon::SLIME, GetWeaponForce(EInfclassWeapon::HAMMER));
	SetWeaponForce(EInfclassWeapon::INFECTED_HAMMER, GetWeaponForce(EInfclassWeapon::HAMMER));
	SetWeaponForce(EInfclassWeapon::STUNNING_HAMMER, GetWeaponForce(EInfclassWeapon::HAMMER));
	SetWeaponForce(EInfclassWeapon::BOOMER_EXPLOSION, 52);
	SetWeaponForce(EInfclassWeapon::INFECTED_GRENADE, GetWeaponForce(EInfclassWeapon::GRENADE));

	SetFireDelay(EInfclassWeapon::JAWS, GetFireDelay(EInfclassWeapon::HAMMER));
	SetFireDelay(EInfclassWeapon::SLIME, GetFireDelay(EInfclassWeapon::HAMMER));
	SetFireDelay(EInfclassWeapon::INFECTED_HAMMER, GetFireDelay(EInfclassWeapon::HAMMER));
	SetFireDelay(EInfclassWeapon::STUNNING_HAMMER, GetFireDelay(EInfclassWeapon::HAMMER));
	SetFireDelay(EInfclassWeapon::BOOMER_EXPLOSION, GetFireDelay(EInfclassWeapon::HAMMER));
	SetFireDelay(EInfclassWeapon::INFECTED_GRENADE, 450);

	SetAmmoRegenTime(EInfclassWeapon::JAWS, 0);
	SetAmmoRegenTime(EInfclassWeapon::SLIME, 0);
	SetAmmoRegenTime(EInfclassWeapon::INFECTED_HAMMER, 0);
	SetAmmoRegenTime(EInfclassWeapon::STUNNING_HAMMER, 0);
	SetAmmoRegenTime(EInfclassWeapon::BOOMER_EXPLOSION, 0);
	SetAmmoRegenTime(EInfclassWeapon::INFECTED_GRENADE, 5000);

	SetMaxAmmo(EInfclassWeapon::JAWS, -1);
	SetMaxAmmo(EInfclassWeapon::SLIME, -1);
	SetMaxAmmo(EInfclassWeapon::INFECTED_HAMMER, -1);
	SetMaxAmmo(EInfclassWeapon::STUNNING_HAMMER, -1);
	SetMaxAmmo(EInfclassWeapon::BOOMER_EXPLOSION, -1);
	SetMaxAmmo(EInfclassWeapon::INFECTED_GRENADE, 10);
}

bool CIcGameController::GetPlayerClassEnabled(EPlayerClass PlayerClass) const
{
	const int Index = static_cast<int>(PlayerClass);
	if(Index < 0)
		return false;

	if(m_aClassEnabled[Index].has_value())
		return m_aClassEnabled[Index].value();

	if(GetRoundType() == ERoundType::Fun)
	{
		return PlayerClass == m_FunRoundConfiguration.HumanClass;
	}
	if(GetRoundType() == ERoundType::Survival)
	{
		switch(PlayerClass)
		{
		case EPlayerClass::Soldier:
			return false;
		default:
			break;
		}
	}

	switch(PlayerClass)
	{
	case EPlayerClass::Engineer:
		return Config()->m_InfEnableEngineer;
	case EPlayerClass::Soldier:
		return Config()->m_InfEnableSoldier;
	case EPlayerClass::Scientist:
		return Config()->m_InfEnableScientist;
	case EPlayerClass::Biologist:
		return Config()->m_InfEnableBiologist;
	case EPlayerClass::Medic:
		return Config()->m_InfEnableMedic;
	case EPlayerClass::Hero:
		return Config()->m_InfEnableHero;
	case EPlayerClass::Ninja:
		return Config()->m_InfEnableNinja;
	case EPlayerClass::Mercenary:
		return Config()->m_InfEnableMercenary;
	case EPlayerClass::Sniper:
		return Config()->m_InfEnableSniper;
	case EPlayerClass::Looper:
		return Config()->m_InfEnableLooper;
	case EPlayerClass::Artillery:
		return Config()->m_InfEnableLooper; // 我是4号占位符
	default:
		break;
	}

	return false;
}

bool CIcGameController::SetPlayerClassEnabled(EPlayerClass PlayerClass, bool Enabled)
{
	const int Index = static_cast<int>(PlayerClass);
	if(Index < 0)
		return false;

	m_aClassEnabled[Index] = Enabled;
	return true;
}

bool CIcGameController::ResetPlayerClassEnabled(EPlayerClass PlayerClass)
{
	const int Index = static_cast<int>(PlayerClass);
	if(Index < 0)
		return false;

	m_aClassEnabled[Index].reset();
	return true;
}

void CIcGameController::ResetPlayerClassesEnablement()
{
	for(std::optional<bool> &Enabled : m_aClassEnabled)
		Enabled.reset();
}

bool CIcGameController::SetPlayerClassProbability(EPlayerClass PlayerClass, int Probability) const
{
	switch(PlayerClass)
	{
	case EPlayerClass::Smoker:
		Config()->m_InfProbaSmoker = Probability;
		break;
	case EPlayerClass::Boomer:
		Config()->m_InfProbaBoomer = Probability;
		break;
	case EPlayerClass::Hunter:
		Config()->m_InfProbaHunter = Probability;
		break;
	case EPlayerClass::Bat:
		Config()->m_InfProbaBat = Probability;
		break;
	case EPlayerClass::Ghost:
		Config()->m_InfProbaGhost = Probability;
		break;
	case EPlayerClass::Spider:
		Config()->m_InfProbaSpider = Probability;
		break;
	case EPlayerClass::Ghoul:
		Config()->m_InfProbaGhoul = Probability;
		break;
	case EPlayerClass::Slug:
		Config()->m_InfProbaSlug = Probability;
		break;
	case EPlayerClass::Voodoo:
		Config()->m_InfProbaVoodoo = Probability;
		break;
	case EPlayerClass::Witch:
		Config()->m_InfProbaWitch = Probability;
		break;
	case EPlayerClass::Undead:
		Config()->m_InfProbaUndead = Probability;
		break;
	case EPlayerClass::Tank:
	case EPlayerClass::Spitter:
		return false;
	default:
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "controller", "WARNING: Invalid SetPlayerClassProbability() call");
		return false;
	}

	return true;
}

uint32_t CIcGameController::GetMinPlayersForClass(EPlayerClass PlayerClass) const
{
	switch(PlayerClass)
	{
	case EPlayerClass::Engineer:
		if(GetRoundType() == ERoundType::Survival)
			return 0;
		return Config()->m_InfMinPlayersForEngineer;
	default:
		return 0;
	}
}

uint32_t CIcGameController::GetClassPlayerLimit(EPlayerClass PlayerClass) const
{
	switch(PlayerClass)
	{
	case EPlayerClass::Medic:
		return Config()->m_InfMedicLimit;
	case EPlayerClass::Hero:
		return Config()->m_InfHeroLimit;
	case EPlayerClass::Witch:
		return Config()->m_InfWitchLimit;
	default:
		return Config()->m_SvMaxClients;
	}
}

int CIcGameController::GetPlayerClassProbability(EPlayerClass PlayerClass) const
{
	if(GetRoundType() == ERoundType::Fun)
	{
		return PlayerClass == m_FunRoundConfiguration.InfectedClass;
	}

	switch(PlayerClass)
	{
	case EPlayerClass::Smoker:
		return Config()->m_InfProbaSmoker;
	case EPlayerClass::Boomer:
		return Config()->m_InfProbaBoomer;
	case EPlayerClass::Hunter:
		return Config()->m_InfProbaHunter;
	case EPlayerClass::Bat:
		return Config()->m_InfProbaBat;
	case EPlayerClass::Ghost:
		return Config()->m_InfProbaGhost;
	case EPlayerClass::Spider:
		return Config()->m_InfProbaSpider;
	case EPlayerClass::Ghoul:
		return Config()->m_InfProbaGhoul;
	case EPlayerClass::Slug:
		return Config()->m_InfProbaSlug;
	case EPlayerClass::Voodoo:
		return Config()->m_InfProbaVoodoo;
	case EPlayerClass::Witch:
		return Config()->m_InfProbaWitch;
	case EPlayerClass::Undead:
		return Config()->m_InfProbaUndead;
	case EPlayerClass::Tank:
	case EPlayerClass::Spitter:
		return 0;
	default:
		break;
	}

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "WARNING: Invalid GetPlayerClassProbability() call");
	return 0;
}

int CIcGameController::GetInfectedCount(EPlayerClass InfectedPlayerClass) const
{
	int Count = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const CIcPlayer *pPlayer = GetPlayer(i);
		if(!pPlayer || !pPlayer->IsInGame())
			continue;

		if(!pPlayer->IsInfected())
			continue;

		if(InfectedPlayerClass != EPlayerClass::Invalid)
		{
			if(pPlayer->GetClass() != InfectedPlayerClass)
				continue;
		}

		Count++;
	}
	return Count;
}

bool CIcGameController::IsWinCheckEnabled() const
{
	return m_WinCheckEnabled.value_or(true);
}

void CIcGameController::SetWinCheckEnabled(bool Enabled)
{
	m_WinCheckEnabled = Enabled;
}

bool CIcGameController::GetVotesEnabled() const
{
	return m_VotesEnabled.value_or(!m_InfectedStarted);
}

void CIcGameController::SetVotesEnabled(bool Enabled)
{
	m_VotesEnabled = Enabled;
}

int CIcGameController::GetMinPlayers() const
{
	return m_RoundMinimumPlayers.value_or(Config()->m_InfMinPlayers);
}

void CIcGameController::SetRoundMinimumPlayers(int Number)
{
	m_RoundMinimumPlayers = Number;
}

int CIcGameController::GetMinimumInfected() const
{
	if(m_RoundMinimumInfected.has_value())
		return m_RoundMinimumInfected.value();

	int NumPlayers = 0;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		const CIcPlayer *pPlayer = GetPlayer(i);
		if(!pPlayer || !pPlayer->m_IsInGame || pPlayer->IsSpectator())
		{
			continue;
		}
		++NumPlayers;
	}

	return GetMinimumInfectedForPlayers(NumPlayers);
}

void CIcGameController::SetRoundMinimumInfected(int Number)
{
	m_RoundMinimumInfected = Number;
}

void CIcGameController::ResetRoundMinimumInfected()
{
	m_RoundMinimumInfected.reset();
}

ERoundType CIcGameController::GetDefaultRoundType() const
{
	// Never return invalid
	ERoundType RoundType = fromString<ERoundType>(Config()->m_InfDefaultRoundType);
	if(RoundType == ERoundType::Invalid)
		RoundType = ERoundType::Normal;

	return RoundType;
}

ERoundType CIcGameController::GetRoundType() const
{
	return m_RoundType;
}

void CIcGameController::QueueRoundType(ERoundType RoundType)
{
	dbg_msg("controller", "Queued round: %s", toString(RoundType));
	m_QueuedRoundType = RoundType;
}

CLASS_AVAILABILITY CIcGameController::GetPlayerClassAvailability(EPlayerClass PlayerClass, const CIcPlayer *pForPlayer) const
{
	if(!GetPlayerClassEnabled(PlayerClass))
		return CLASS_AVAILABILITY::DISABLED;

	const uint32_t ActivePlayerCount = Server()->GetActivePlayerCount();
	const uint32_t MinPlayersForClass = GetMinPlayersForClass(PlayerClass);
	if(ActivePlayerCount < MinPlayersForClass)
		return CLASS_AVAILABILITY::NEED_MORE_PLAYERS;

	int nbSupport = 0;
	int nbDefender = 0;
	icArray<int, NB_PLAYERCLASS> nbClass;
	nbClass.Resize(NB_PLAYERCLASS);

	CIcPlayerIterator<PLAYERITER_INGAME> Iter(GameServer()->m_apPlayers);
	while(Iter.Next())
	{
		const EPlayerClass AnotherPlayerClass = Iter.Player()->GetClass();
		const int Index = static_cast<int>(AnotherPlayerClass);
		if(IsDefenderClass(AnotherPlayerClass))
			nbDefender++;
		if(IsSupportClass(AnotherPlayerClass))
			nbSupport++;
		nbClass[Index]++;
	}

	if(IsDefenderClass(PlayerClass) && (nbDefender >= Config()->m_InfDefenderLimit))
		return CLASS_AVAILABILITY::LIMIT_EXCEEDED;

	if(IsSupportClass(PlayerClass) && (nbSupport >= Config()->m_InfSupportLimit))
		return CLASS_AVAILABILITY::LIMIT_EXCEEDED;

	int ClassLimit = GetClassPlayerLimit(PlayerClass);
	if(GetRoundType() == ERoundType::Survival)
	{
		static constexpr EPlayerClass EarlyClasses[] = {
			EPlayerClass::Medic,
			EPlayerClass::Mercenary,
			EPlayerClass::Sniper,
		};

		icArray<EPlayerClass, NB_HUMANCLASS> EnabledEarlyClasses;

		uint32_t EnabledHumansClasses = 0;
		for(EPlayerClass HumanClass : AllHumanClasses)
		{
			if(GetPlayerClassEnabled(HumanClass))
			{
				EnabledHumansClasses++;
			}
			const auto FoundEarly = std::ranges::find(EarlyClasses, HumanClass);
			if(FoundEarly != std::cend(EarlyClasses))
			{
				EnabledEarlyClasses.Add(HumanClass);
			}
		}

		if(EnabledHumansClasses == 0)
		{
			dbg_msg("server/ic", "Error: No human class enabled");
			EnabledHumansClasses = 1;
		}

		ClassLimit = Config()->m_InfSurvivalClassLimit ? std::ceil(ActivePlayerCount / static_cast<float>(EnabledHumansClasses)) : Config()->m_SvMaxClients;
		const uint32_t ExtraPlayers = Config()->m_InfSurvivalClassLimit ? ActivePlayerCount % EnabledHumansClasses : 0;
		if((ClassLimit > 1) && ExtraPlayers)
		{
			if(ExtraPlayers <= EnabledEarlyClasses.Size())
			{
				if(!EnabledEarlyClasses.Contains(PlayerClass))
					ClassLimit -= 1;
			}
		}

		if((EnabledHumansClasses > 1) && !HumanCanPickSameClass())
		{
			EPlayerClass PrevClass = pForPlayer ? pForPlayer->GetPreviouslyPickedClass() : EPlayerClass::Invalid;
			if(PlayerClass == PrevClass)
			{
				return CLASS_AVAILABILITY::PICKED_PREVIOUSLY;
			}
		}
	}

	if(nbClass[static_cast<int>(PlayerClass)] >= ClassLimit)
		return CLASS_AVAILABILITY::LIMIT_EXCEEDED;

	if(PlayerClass == EPlayerClass::Hero)
	{
		if(m_HeroFlagPositions.size() == 0)
		{
			return CLASS_AVAILABILITY::DISABLED;
		}
	}

	return CLASS_AVAILABILITY::AVAILABLE;
}

bool CIcGameController::CanVote()
{
	return GetVotesEnabled();
}

void CIcGameController::OnPlayerVoteCommand(int ClientId, int Vote)
{
	CIcPlayer *pPlayer = GetPlayer(ClientId);
	if(!pPlayer)
	{
		return;
	}

	if(pPlayer->m_PlayerFlags & PLAYERFLAG_SCOREBOARD)
	{
		EPlayerScoreMode ScoreMode = pPlayer->GetScoreMode();
		if(Vote < 0)
		{
			// F3, next mode
			int ScoreModeValue = static_cast<int>(ScoreMode);
			ScoreModeValue++;
			ScoreMode = static_cast<EPlayerScoreMode>(ScoreModeValue);
			if(ScoreMode == EPlayerScoreMode::Count)
			{
				ScoreMode = static_cast<EPlayerScoreMode>(0);
			}
		}
		else
		{
			// F4, prev mode
			int ScoreModeValue = static_cast<int>(ScoreMode);
			if(ScoreModeValue == 0)
			{
				ScoreMode = static_cast<EPlayerScoreMode>(static_cast<int>(EPlayerScoreMode::Count) - 1);
			}
			else
			{
				ScoreModeValue--;
				ScoreMode = static_cast<EPlayerScoreMode>(ScoreModeValue);
			}
		}

		pPlayer->SetScoreMode(ScoreMode);
	}
	else
	{
		if(GetRoundType() == ERoundType::HideAndSeek)
		{
			GameServer()->SendChatTarget_Localization(ClientId, CHATCATEGORY_DEFAULT,
				_("Hook protection is always disabled in this round type"), nullptr);
			return;
		}

		pPlayer->SetHookProtection(!pPlayer->HookProtectionEnabled(), false);
	}
}

void CIcGameController::OnPlayerClassChanged(CIcPlayer *pPlayer)
{
	SetPlayerInfected(pPlayer->GetCid(), pPlayer->IsInfected());

	if(GetRoundType() == ERoundType::HideAndSeek)
	{
		ApplyHideAndSeekAttributes(pPlayer);
	}

	Server()->ExpireServerInfo();
}
