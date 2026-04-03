/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_INFCLASS_GAMECONTROLLER_H
#define GAME_SERVER_INFCLASS_GAMECONTROLLER_H

#include <game/infclass/ic_classes.h>
#include <game/infclass/weapons.h>
#include <game/server/gamecontroller.h>

#include <engine/console.h>
#include <engine/shared/protocol.h>

#include <base/tl/ic_array.h>

class CBaseBotPlayer;
class CBotUtils;
class CBotUtilsSharedData;
class CControlPoint;
class CDoor;
class CGameWorld;
class CHintMessage;
class CIcCharacter;
class CIcPlayer;
class IDebugSink;
struct CNetObj_GameInfo;
struct DeathContext;
struct SpawnContext;
struct ZoneData;

#if CONF_LUA
struct CLuaPlayersNumber;
#endif // CONF_LUA

enum class TAKEDAMAGEMODE;
enum class EDamageType;
enum class ROUND_CANCELATION_REASON;
enum class EPlayerScoreMode;

enum class ERoundEndReason
{
	FINISHED,
	CANCELED,

	COUNT,
	INVALID = COUNT
};
const char *toString(ERoundEndReason Reason);

static const int MaxBots = MAX_CLIENTS - 8;

using ClientsArray = icArray<int, MAX_CLIENTS>;

enum class ERoundType
{
	Normal,
	Fun,
	Fast,
	Survival,
	HideAndSeek,
	Count,
	Invalid = Count,
};
const char *toString(ERoundType RoundType);

enum class CLASS_AVAILABILITY
{
	AVAILABLE,
	PICKED_PREVIOUSLY,
	DISABLED,
	NEED_MORE_PLAYERS,
	LIMIT_EXCEEDED,
};

enum class EFinalExplosionState
{
	NotStarted,
	Started,
	Finished,
};

struct FunRoundConfiguration
{
	FunRoundConfiguration() = default;
	FunRoundConfiguration(EPlayerClass Infected, EPlayerClass Human) :
		InfectedClass(Infected),
		HumanClass(Human)
	{
	}

	EPlayerClass InfectedClass = EPlayerClass::Invalid;
	EPlayerClass HumanClass = EPlayerClass::Invalid;
};

class SurvivalBotConfiguration;
class SurvivalWaveConfiguration;
class SurvivalGameConfiguration;

enum SURVIVAL_MODE
{
	SURVIVAL_MODE_OFF,
	SURVIVAL_MODE_KILL_BASED,
	SURVIVAL_MODE_TIME_BASED,
};

enum class ETextArticle
{
	Indefinite,
	Definite,
};

class CIcGameController : public IGameController
{
public:
	CIcGameController(CGameContext *pGameServer);
	~CIcGameController() override;

	const char *GameType() const override;
	void SetGameType(const char *pGameType);

	void RegisterLuaBindings();
	void IncreaseCurrentRoundCounter() override;

	void DoTeamBalance() override;

	void TickBeforeWorld() override;
	void Tick() override;
	void Snap(int SnappingClient) override;

	CPlayer *CreatePlayer(int ClientId, bool IsSpectator, void *pData) override;
	int PersistentClientDataSize() const override;
	bool GetClientPersistentData(int ClientId, void *pData) const override;

	void GetHelpText(dynamic_string *pBuffer, int ClientId, const char *pHelpPage) const override;
	bool GetClassHelpPage(dynamic_string *pOutput, const char *pLanguage, EPlayerClass PlayerClass) const;

	bool OnEntity(const char *pName, vec2 Pivot, vec2 P0, vec2 P1, vec2 P2, vec2 P3, int PosEnv) override;
	void HandleCharacterTiles(CIcCharacter *pCharacter) const;
	void HandleLastHookers();

	float GetSecondsElapsed() const;
	float GetSecondsRemaining() const;

	bool CanSeeDetails(int Who, int Whom) const;
	CClientMask GetBlindCharactersMask(int ExcludeCid) const;
	CClientMask GetMaskForPlayerWorldEvent(int Asker, int ExceptId = -1);

	bool HumanWallAllowedInPos(const vec2 &Pos) const;
	int GetZoneValueAt(int ZoneHandle, const vec2 &Pos, ZoneData *pData = nullptr) const;
	int GetDamageZoneValueAt(const vec2 &Pos, ZoneData *pData = nullptr) const;
	EZoneTele GetTeleportZoneValueAt(const vec2 &Pos, ZoneData *pData = nullptr) const;
	int GetBonusZoneValueAt(const vec2 &Pos, ZoneData *pData = nullptr) const;

	void ExecuteFileEx(const char *pBaseName);

	void CreateExplosion(const vec2 &Pos, int Owner, EDamageType DamageType, float DamageFactor = 1.0f);
	void CreateExplosionDisk(vec2 Pos, float InnerRadius, float DamageRadius, int Damage, float Force, int Owner, EDamageType DamageType);
	void CreateExplosionDiskGfx(vec2 Pos, float InnerRadius, float DamageRadius, int Owner) const;
	void CreateDeathEffectDiskGfx(vec2 Pos, float InnerRadius, float DamageRadius, int Owner);

	void SendHammerDot(const vec2 &Pos, int SnapId);
	void SendServerParams(int ClientId) const;

	int OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon) override;
	void OnIcCharacterDeath(CIcCharacter *pVictim, DeathContext *pContext);
	void OnIcCharacterSpawned(CIcCharacter *pCharacter, const SpawnContext &Context);
	void OnCharacterBackFromDead(CIcCharacter *pCharacter);
	void OnClassChooserRequested(CIcCharacter *pCharacter) const;
	void CheckRoundFailed();
	float GetMaxInactiveTimeSeconds(const CPlayer *pPlayer) const override;
	void DoWincheck() override;
	void StartRound() override;
	void ResetRoundData();
	void EndRound() override;
	void EndRound(ERoundEndReason Reason);
	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg = true) override;
	bool TryRespawn(CIcPlayer *pPlayer, SpawnContext *pContext);
	EPlayerClass ChooseHumanClass(const CIcPlayer *pPlayer) const;
	EPlayerClass ChooseInfectedClass(const CIcPlayer *pPlayer) const;

	float GetWeaponForce(EInfclassWeapon WID) const;
	void SetWeaponForce(EInfclassWeapon WID, float Force);

	int GetFireDelay(EInfclassWeapon WID) const;
	void SetFireDelay(EInfclassWeapon WID, int Time);

	int GetAmmoRegenTime(EInfclassWeapon WID) const;
	void SetAmmoRegenTime(EInfclassWeapon WID, int Time);

	int GetMaxAmmo(EInfclassWeapon WID) const;
	void SetMaxAmmo(EInfclassWeapon WID, int n);

	void RegisterEntityTypes();
	void InitWeapons();

	void DestroyChildEntities(int OwnerId);
	bool GetPlayerClassEnabled(EPlayerClass PlayerClass) const;
	bool SetPlayerClassEnabled(EPlayerClass PlayerClass, bool Enabled);
	bool ResetPlayerClassEnabled(EPlayerClass PlayerClass);
	void ResetPlayerClassesEnablement();
	bool SetPlayerClassProbability(EPlayerClass PlayerClass, int Probability) const;

	uint32_t GetMinPlayersForClass(EPlayerClass PlayerClass) const;
	uint32_t GetClassPlayerLimit(EPlayerClass PlayerClass) const;
	int GetPlayerClassProbability(EPlayerClass PlayerClass) const;

	int GetInfectedCount(EPlayerClass InfectedPlayerClass = EPlayerClass::Invalid) const;

	bool IsWinCheckEnabled() const;
	void SetWinCheckEnabled(bool Enabled);

	bool GetVotesEnabled() const;
	void SetVotesEnabled(bool Enabled);

	int GetMinPlayers() const;
	void SetRoundMinimumPlayers(int Number);

	int GetMinimumInfected() const;
	void SetRoundMinimumInfected(int Number);
	void ResetRoundMinimumInfected();

	ERoundType GetDefaultRoundType() const;
	ERoundType GetRoundType() const;
	void QueueRoundType(ERoundType RoundType);

	CLASS_AVAILABILITY GetPlayerClassAvailability(EPlayerClass PlayerClass, const CIcPlayer *pForPlayer = nullptr) const;
	bool CanVote() override;

	void OnPlayerVoteCommand(int ClientId, int Vote) override;
	void OnPlayerClassChanged(CIcPlayer *pPlayer);

	void OnPlayerConnect(CPlayer *pPlayer) override;
	void OnPlayerDisconnect(CPlayer *pBasePlayer, EClientDropType Type, const char *pReason) override;

	void OnReset() override;
	void OnShutdown() override;

	void DoPlayerInfection(CIcPlayer *pPlayer, CIcPlayer *pInfectiousPlayer, EPlayerClass PreviousClass);
	void MaybeDropPickup(CIcCharacter *pVictim);

	void OnHeroFlagCollected(int ClientId);
	float GetHeroFlagCooldown() const;

	void ApplyControlPointEffect(CControlPoint *pControlPoint);
	void OnControlPointCaptured(CControlPoint *pControlPoint);

	bool IsInfectionStarted() const;
	bool MapRotationEnabled() const override;
	void OnTeamChangeRequested(int ClientId, int Team) override;
	bool CanJoinTeam(int Team, int ClientId) override;
	bool AreTurretsEnabled() const;
	int InfTurretDuration() const;
	bool MercBombsEnabled() const;
	bool WhiteHoleEnabled() const;
	bool AirstrikeEnabled() const;
	float GetWhiteHoleLifeSpan() const;
	int MinimumInfectedForRevival() const;
	bool IsClassChooserEnabled() const;
	int HardMode() const;
	bool HumanCanPickSameClass() const;
	int GetTaxiMode() const;
	int GetGhoulStomackSize() const;
	EPlayerScoreMode GetPlayerScoreMode(int SnappingClient) const;

	float GetTimeLimitMinutes() const;

	int GetTimeLimitSeconds() const;
	void SetTimeLimitSeconds(float Seconds);

	int GetInfectionDelay() const;
	void SetInfectionDelay(int Seconds);

	bool IsSpawnable(vec2 Pos, EZoneTele TeleZoneIndex);

	const ClientsArray &GetValidNinjaTargets() const { return m_NinjaTargets; }

	bool HeroGiftAvailable() const;
	std::optional<vec2> GetHeroFlagPosition() const;
	bool IsPositionAvailableForHumans(const vec2 &FlagPosition) const;

	void StartHideAndSeekRound();
	void EndHideAndSeekRound();
	void ApplyHideAndSeekAttributes(CIcPlayer *pPlayer);
	void OnHideAndSeekTick();

	void StartFunRound();
	void EndFunRound();

	void StartSurvivalRound();
	void EndSurvivalRound(ERoundEndReason Reason);
	bool StartSurvivalWave();
	void EndSurvivalWave();

	void EnsureFinalExplosionIsStarted();
	void StartFinalExplosion();
	void ProgressFinalExplosion();
	void ResetFinalExplosion();
	void SaveRoundRules();
	void EndSurvivalGame();

	int GetRoundStartTick() const { return m_RoundStartTick; }
	int GetRoundTick() const;
	int GetInfectionTick() const;
	int GetInfectionStartTick() const;

	static bool IsDefenderClass(EPlayerClass PlayerClass);
	static bool IsSupportClass(EPlayerClass PlayerClass);
	static EPlayerClass GetClassByName(const char *pClassName, bool *pOk = nullptr);
	static const char *GetClassName(EPlayerClass PlayerClass);
	static const char *GetClassPluralName(EPlayerClass PlayerClass);
	static const char *GetClassDisplayName(EPlayerClass PlayerClass, const char *pDefaultText = nullptr);
	static const char *GetClassDisplayNameForKilledBy(EPlayerClass PlayerClass, ETextArticle Article = ETextArticle::Indefinite);
	static const char *GetClanForClass(EPlayerClass PlayerClass, const char *pDefaultText = nullptr);
	static const char *GetClassPluralDisplayName(EPlayerClass PlayerClass);
	static EPlayerClass MenuClassToPlayerClass(int MenuClass);

	int GetPlayerTeam(int ClientId) const override;
	void SetPlayerInfected(int ClientId, bool Infected);

	void RegisterChatCommands(IConsole *pConsole) override;

	static EInfclassWeapon GetWeaponIdFromConArgument(IConsole::IResult *pResult, unsigned Index);
	static void ConSetWeaponFireDelay(IConsole::IResult *pResult, void *pUserData);
	static void ConSetWeaponAmmoRegen(IConsole::IResult *pResult, void *pUserData);
	static void ConSetWeaponMaxAmmo(IConsole::IResult *pResult, void *pUserData);
	static void ConWeaponForce(IConsole::IResult *pResult, void *pUserData);
	static void ConListWeapons(IConsole::IResult *pResult, void *pUserData);

	static void ConGetActivePlayersNumber(IConsole::IResult *pResult, void *pUserData);

	static void ConStartSurvivalScenario(IConsole::IResult *pResult, void *pUserData);
	void ConStartSurvivalScenario(IConsole::IResult *pResult);

	static void ConSetClientName(IConsole::IResult *pResult, void *pUserData);
	static void ConRestoreClientName(IConsole::IResult *pResult, void *pUserData);
	static void ConLockClientName(IConsole::IResult *pResult, void *pUserData);
	static void ConPreferClass(IConsole::IResult *pResult, void *pUserData);
	static void ConAlwaysRandom(IConsole::IResult *pResult, void *pUserData);
	void SetPreferredClass(int ClientId, const char *pClassName);
	void SetPreferredClass(int ClientId, EPlayerClass Class);
	static void ConAntiPing(IConsole::IResult *pResult, void *pUserData);

	static void ConAddControlPoint(IConsole::IResult *pResult, void *pUserData);

	static void ConUserSetClass(IConsole::IResult *pResult, void *pUserData);
	void ConUserSetClass(IConsole::IResult *pResult);
	static void ConSetClass(IConsole::IResult *pResult, void *pUserData);
	void ConSetClass(IConsole::IResult *pResult);

#if CONF_LUA
	static void ConExecLua(IConsole::IResult *pResult, void *pUserData);
	static void ConLua(IConsole::IResult *pResult, void *pUserData);

	const array<vec2> *GetHeroFlagPositions() const;
	const std::vector<vec2> *GetHumanSpawns() const;
	const std::vector<vec2> *GetInfectedSpawns() const;
	void SetSpawnEnabled(int Index, bool Enabled, int Type);
	void SetHumanSpawnEnabled(int Index, bool Enabled);
	void SetInfectedSpawnEnabled(int Index, bool Enabled);
	CLuaPlayersNumber GetPlayersNumber_Lua(bool IncludeBots = false);
	void UpdateHeroFlags_Lua();
#endif

	static void ConRefreshHeroFlag(IConsole::IResult *pResult, void *pUserData);
	static void ConReviveNear(IConsole::IResult *pResult, void *pUserData);
	void ConReviveNear(const IConsole::IResult *pResult);
	bool ReviveNear(int RevivedPlayerId, int TargetPlayerId);

	static FunRoundConfiguration ParseFunRoundConfigArguments(IConsole::IResult *pResult);

	static void ConQueueSpecialRound(IConsole::IResult *pResult, void *pUserData);
	void ConQueueRound(const char *pRoundTypeName);
	static void ConStartRound(IConsole::IResult *pResult, void *pUserData);
	static void ConStartFunRound(IConsole::IResult *pResult, void *pUserData);
	static void ConQueueFunRound(IConsole::IResult *pResult, void *pUserData);
	static void ConStartSpecialFunRound(IConsole::IResult *pResult, void *pUserData);
	static void ConClearFunRounds(IConsole::IResult *pResult, void *pUserData);
	static void ConAddFunRound(IConsole::IResult *pResult, void *pUserData);

	static void ConStartFastRound(IConsole::IResult *pResult, void *pUserData);
	static void ConQueueFastRound(IConsole::IResult *pResult, void *pUserData);

	static void ConPrintPlayerPickingTimestamp(IConsole::IResult *pResult, void *pUserData);
	void ConPrintPlayerPickingTimestamp(IConsole::IResult *pResult);

	static void ConSurvivalClearConf(IConsole::IResult *pResult, void *pUserData);
	void SurvivalClearConf();
	static void ConSurvivalConf(IConsole::IResult *pResult, void *pUserData);
	void ConSurvivalConf(IConsole::IResult *pResult);

	static void ConSurvivalAddWave(IConsole::IResult *pResult, void *pUserData);
	void SurvivalAddWave(int Wave, const char *pWaveName);

	static void ConSurvivalConfWave(IConsole::IResult *pResult, void *pUserData);
	void ConSurvivalConfWave(IConsole::IResult *pResult);
	SurvivalBotConfiguration *SurvivalAddBot(int Wave, const char *pClassName);
	void ConSurvivalConfWaveAddBots(IConsole::IResult *pResult, SurvivalBotConfiguration *pBotConfiguration) const;
	void ConSurvivalConfWaveCommand(IConsole::IResult *pResult, SurvivalWaveConfiguration *pConfiguration) const;

	static void ConStartSurvival(IConsole::IResult *pResult, void *pUserData);
	void ConStartSurvival(IConsole::IResult *pResult);
	void PrepareSurvival(int Wave = 0);
	bool SurvivalHumansWinConditionsMet() const;
	bool SurvivalInfectedWinConditionsMet() const;

	static void ConMapRotationStatus(IConsole::IResult *pResult, void *pUserData);
	static void ConSaveMapsData(IConsole::IResult *pResult, void *pUserData);
	static void ConPrintMapsData(IConsole::IResult *pResult, void *pUserData);
	static void ConResetMapData(IConsole::IResult *pResult, void *pUserData);
	static void ConAddMapData(IConsole::IResult *pResult, void *pUserData);
	static void ConSetMapMinMaxPlayers(IConsole::IResult *pResult, void *pUserData);
	void ConSetMapMinMaxPlayers(IConsole::IResult *pResult);
	static void ConSavePosition(IConsole::IResult *pResult, void *pUserData);
	void ConSavePosition(IConsole::IResult *pResult);
	static void ConLoadPosition(IConsole::IResult *pResult, void *pUserData);
	void ConLoadPosition(IConsole::IResult *pResult);

	static void ConSetHealthArmor(IConsole::IResult *pResult, void *pUserData);
	void ConSetHealthArmor(IConsole::IResult *pResult);
	static void ConSetInvincible(IConsole::IResult *pResult, void *pUserData);
	void ConSetInvincible(IConsole::IResult *pResult);
	static void ConSetHookProtection(IConsole::IResult *pResult, void *pUserData);
	void ConSetHookProtection(IConsole::IResult *pResult);
	static void ConGiveUpgrade(IConsole::IResult *pResult, void *pUserData);
	void ConGiveUpgrade(IConsole::IResult *pResult);
	static void ConSetDrop(IConsole::IResult *pResult, void *pUserData);
	void ConSetDrop(IConsole::IResult *pResult);
	static void ConChatSurvivalRespawn(IConsole::IResult *pResult, void *pUserData);
	void ConChatSurvivalRespawn(IConsole::IResult *pResult);
	static void ChatHeroRevive(IConsole::IResult *pResult, void *pUserData);
	void ChatHeroRevive(IConsole::IResult *pResult);

	static void ChatWitch(IConsole::IResult *pResult, void *pUserData);
	void ChatWitch(IConsole::IResult *pResult);

	static void ConSayBot(IConsole::IResult *pResult, void *pUserData);
	void ConSayBot(IConsole::IResult *pResult);

	static void ConAddBot(IConsole::IResult *pResult, void *pUserData);
	void ConAddBot(IConsole::IResult *pResult);

	static void ConRemoveBot(IConsole::IResult *pResult, void *pUserData);
	void ConRemoveBot(IConsole::IResult *pResult);

	static void ConDumpBot(IConsole::IResult *pResult, void *pUserData);
	void ChatDumpBot(IConsole::IResult *pResult);

	static void ConCheckAI(IConsole::IResult *pResult, void *pUserData);
	void ConCheckAI(IConsole::IResult *pResult);

	static void ConAiTracePath(IConsole::IResult *pResult, void *pUserData);
	void ConAiTracePath(IConsole::IResult *pResult);

	static void ConAiObjection(IConsole::IResult *pResult, void *pUserData);
	void ConAiObjection(IConsole::IResult *pResult);

	CControlPoint *AddControlPoint(const vec2 &At);
	CDoor *AddDoor(const vec2 &From, const vec2 &To);

	using IGameController::GameServer;
	CGameWorld *GameWorld();
	IConsole *Console() const;
	IDebugSink *GetDebugSink() const;
	CIcPlayer *GetPlayer(int ClientId) const;
	CIcCharacter *GetCharacter(int ClientId) const;
	int GetPlayerOwnCursorId(int ClientId) const;

	void SortCharactersByDistance(ClientsArray *pCharacterIds, const vec2 &Center, const float MaxDistance = 0);
	void SortCharactersByDistance(const ClientsArray &Input, ClientsArray *pOutput, const vec2 &Center, const float MaxDistance = 0);
	void GetSortedTargetsInRange(const vec2 &Center, const float Radius, const ClientsArray &SkipList, ClientsArray *pOutput);

	int InfectedBonusArmor() const;

	void SendKillMessage(int Victim, const DeathContext &Context);
	std::optional<int> GetClientIdByName(const char *pName) const;
	void OnKillOrInfection(int Victim, const DeathContext &Context);

protected:
	void RoundTickBeforeInitialInfection();
	void RoundTickAfterInitialInfection();

	void PreparePlayerToJoin(CIcPlayer *pPlayer);
	void SetPlayerPickedTimestamp(CIcPlayer *pPlayer, int Timestamp) const;

	uint32_t InfectHumans(uint32_t NumHumansToInfect);
	void ForcePlayersBalance(uint32_t PlayersToBalance);
	void UpdateBalanceFactors();

	void CancelTheRound(ROUND_CANCELATION_REASON Reason);
	void AnnounceTheWinner(int NumHumans);
	void AnnounceHideAndSeekWinner();
	void BroadcastInfectionComing(int InfectionTick);

private:
	void UpdateNinjaTargets();

	void ReservePlayerOwnSnapItems();
	void FreePlayerOwnSnapItems();

	void SendHintMessage();
	void FormatHintMessage(const CHintMessage &Message, dynamic_string *pBuffer, const char *pLanguage) const;

	void OnInfectionTriggered();

	void StartInfectionGameplay(int PlayersToInfect);
	void StartHideAndSeekGameplay(int SeekingPlayers);

	void MaybeSuggestMoreRounds();
	void SnapMapMenu(int SnappingClient, CNetObj_GameInfo *pGameInfoObj);
	void FallInLoveIfInfectedEarly(CIcCharacter *pCharacter);
	void RewardTheKillers(CIcCharacter *pVictim, const DeathContext &Context);
	void GetPlayerCounter(int ClientException, int &NumHumans, int &NumInfected);
	int GetMinimumInfectedForPlayers(int PlayersNumber) const;

	int GetClientIdForNewWitch() const;
	bool IsSafeWitchCandidate(int ClientId) const;
	ClientsArray m_WitchCallers;

	void RemoveBots();
	void ResetSpawnedBotsTracking();

	int RequestBotID();
	CBaseBotPlayer *AddBot(int Team = 0);
	CBaseBotPlayer *AddBot(const SurvivalBotConfiguration &Configuration);
	CBaseBotPlayer *AddBot_Lua(const char *pClass);
	CBaseBotPlayer *GetBot(int ClientId);
	bool RemoveBot(CBaseBotPlayer *pBot, const char *pReason = nullptr);
	bool RemoveBot(int ClientId, const char *pReason = nullptr);
	bool RemoveBot_Lua(int ClientId);
	void RegisterBotsContext();

	void AddMoreBotsAccordingToConfiguration();

	icArray<CBaseBotPlayer *, MaxBots> m_Bots;
	CBotUtilsSharedData *m_pBotUtilsData{};

	struct PlayerScore
	{
		char aPlayerName[MAX_NAME_LENGTH]{};
		uint32_t Kills{};
		uint32_t Assists{};
		int ClientId{};

		uint32_t GetScore() const
		{
			return Kills + Assists / 2;
		}
	};

	struct
	{
		uint32_t Wave = 0;
		uint32_t Kills = 0;
		icArray<PlayerScore, MAX_CLIENTS> Scores;
		icArray<int, MAX_CLIENTS> SurvivedPlayers;
		icArray<int, MAX_CLIENTS> KilledPlayers;
	} m_SurvivalState;
	int m_BestSurvivalScore = 0;
	const char *m_LastUsedKillMessage = nullptr;

	int m_WaveStartTick = 0;
	bool m_TriggerSurvivalAutostart = false;

	PlayerScore *GetSurvivalPlayerScore(int ClientId);
	PlayerScore *EnsureSurvivalPlayerScore(int ClientId);

	const SurvivalGameConfiguration *SurvivalGetGameConfiguration() const;
	SurvivalWaveConfiguration *SurvivalGetWaveConfiguration(int WaveIndex);
	const SurvivalWaveConfiguration *GetCurrentSurvivalWaveConfiguration() const;
	SurvivalGameConfiguration *SurvivalGetMutableGameConfiguration();

private:
	int m_ZoneHandle_icDamage;
	int m_ZoneHandle_icTeleport;
	int m_ZoneHandle_icBonus;

	int m_MapWidth;
	int m_MapHeight;
	int *m_GrowingMap;
	EFinalExplosionState m_FinalExplosionState{};

	std::optional<std::string> m_GameType;
	std::optional<bool> m_WinCheckEnabled;
	std::optional<bool> m_VotesEnabled;
	std::optional<int> m_RoundMinimumPlayers;
	std::optional<int> m_RoundMinimumInfected;
	std::optional<float> m_RoundTimeLimitSeconds;
	std::optional<int> m_RoundInfectionDelaySeconds;
	std::vector<int> m_EnabledSpawnPoints[2];
	int m_InfUnbalancedTick;
	float m_InfBalanceBoostFactor = 0;
	array<vec2> m_HeroFlagPositions;
	int m_HeroGiftTick = 0;

	ClientsArray m_NinjaTargets;
	int m_PlayerOwnCursorId = -1;

	ERoundType m_RoundType = ERoundType::Normal;
	ERoundType m_QueuedRoundType = ERoundType::Normal;

	std::optional<bool> m_aClassEnabled[NB_PLAYERCLASS];
	FunRoundConfiguration m_FunRoundConfiguration;
	std::vector<FunRoundConfiguration> m_FunRoundConfigurations;
	int m_FunRoundsPassed = 0;

	bool m_HsFastRound = false;

	bool m_InfectedStarted;
	bool m_RoundStarted = false;
	bool m_SuggestMoreRounds = false;
	bool m_MoreRoundsSuggested = false;
	bool m_ControlPointHintSent[2] = {};
	bool m_VanillaMapLoaded = false;

	int m_InfAmmoRegenTime[NB_INFWEAPON]{};
	int m_InfFireDelay[NB_INFWEAPON]{};
	int m_InfMaxAmmo[NB_INFWEAPON]{};
	float m_aInfWeaponForce[NB_INFWEAPON]{};
	static int64_t m_LastTipTime;
};

#endif
