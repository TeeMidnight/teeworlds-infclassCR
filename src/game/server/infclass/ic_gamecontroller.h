/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_INFCLASS_GAMECONTROLLER_H
#define GAME_SERVER_INFCLASS_GAMECONTROLLER_H

#include <game/infclass/ic_classes.h>
#include <game/infclass/weapons.h>
#include <game/server/gamecontroller.h>

#include <engine/console.h>

#include <base/tl/ic_array.h>

class CDoor;
class CGameWorld;
class CHintMessage;
class CIcCharacter;
class CIcPlayer;
struct CNetObj_GameInfo;
struct SpawnContext;
struct DeathContext;
struct ZoneData;

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

static const int MaxWaves = 20;

using ClientsArray = icArray<int, MAX_CLIENTS>;

enum class ERoundType
{
	Normal,
	Fun,
	Fast,
	Survival,
	Count,
	Invalid = Count,
};
const char *toString(ERoundType RoundType);

enum class CLASS_AVAILABILITY
{
	AVAILABLE,
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

class CIcGameController : public IGameController
{
public:
	CIcGameController(CGameContext *pGameServer);
	~CIcGameController() override;

	const char *GameType() const override;

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

	bool OnEntity(const char* pName, vec2 Pivot, vec2 P0, vec2 P1, vec2 P2, vec2 P3, int PosEnv) override;
	void HandleCharacterTiles(CIcCharacter *pCharacter);
	void HandleLastHookers();

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
	void CreateExplosionDiskGfx(vec2 Pos, float InnerRadius, float DamageRadius, int Owner);

	void SendHammerDot(const vec2 &Pos, int SnapId);
	void SendServerParams(int ClientId) const;

	int OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon) override;
	void OnIcCharacterDeath(CIcCharacter *pVictim, DeathContext *pContext);
	void OnIcCharacterSpawned(CIcCharacter *pCharacter, const SpawnContext &Context);
	void OnClassChooserRequested(CIcCharacter *pCharacter);
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
	bool SetPlayerClassProbability(EPlayerClass PlayerClass, int Probability);

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

	void DoPlayerInfection(CIcPlayer *pPlayer, CIcPlayer *pInfectiousPlayer, EPlayerClass PreviousClass);
	void MaybeDropPickup(CIcCharacter *pVictim);

	void OnHeroFlagCollected(int ClientId);
	float GetHeroFlagCooldown() const;

	bool IsInfectionStarted() const;
	bool MapRotationEnabled() const override;
	void OnTeamChangeRequested(int ClientId, int Team) override;
	bool CanJoinTeam(int Team, int ClientId) override;
	bool AreTurretsEnabled() const;
	int InfTurretDuration() const;
	bool MercBombsEnabled() const;
	bool WhiteHoleEnabled() const;
	bool AirstrikeeEnabled() const;
	float GetWhiteHoleLifeSpan() const;
	int MinimumInfectedForRevival() const;
	bool IsClassChooserEnabled() const;
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

	void StartFunRound();
	void EndFunRound();

	void StartSurvivalRound();
	void EndSurvivalRound();

	void EnsureFinalExplosionIsStarted();
	void StartFinalExplosion();
	void ProgressFinalExplosion();
	void ResetFinalExplosion();
	void SaveRoundRules();
	void StartSurvivalGame();
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
	static const char *GetClassDisplayNameForKilledBy(EPlayerClass PlayerClass, const char *pDefaultText = nullptr);
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

	static void ConSetClientName(IConsole::IResult *pResult, void *pUserData);
	static void ConRestoreClientName(IConsole::IResult *pResult, void *pUserData);
	static void ConLockClientName(IConsole::IResult *pResult, void *pUserData);
	static void ConPreferClass(IConsole::IResult *pResult, void *pUserData);
	static void ConAlwaysRandom(IConsole::IResult *pResult, void *pUserData);
	void SetPreferredClass(int ClientId, const char *pClassName);
	void SetPreferredClass(int ClientId, EPlayerClass Class);
	static void ConAntiPing(IConsole::IResult *pResult, void *pUserData);

	static void ConUserSetClass(IConsole::IResult *pResult, void *pUserData);
	void ConUserSetClass(IConsole::IResult *pResult);
	static void ConSetClass(IConsole::IResult *pResult, void *pUserData);
	void ConSetClass(IConsole::IResult *pResult);

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

	static void ChatWitch(IConsole::IResult *pResult, void *pUserData);
	void ChatWitch(IConsole::IResult *pResult);

	CDoor *AddDoor(const vec2 &From, const vec2 &To);

	using IGameController::GameServer;
	CGameWorld *GameWorld();
	IConsole *Console() const;
	CIcPlayer *GetPlayer(int ClientId) const;
	CIcCharacter *GetCharacter(int ClientId) const;
	int GetPlayerOwnCursorId(int ClientId) const;

	void SortCharactersByDistance(ClientsArray *pCharacterIds, const vec2 &Center, const float MaxDistance = 0);
	void SortCharactersByDistance(const ClientsArray &Input, ClientsArray *pOutput, const vec2 &Center, const float MaxDistance = 0);
	void GetSortedTargetsInRange(const vec2 &Center, const float Radius, const ClientsArray &SkipList, ClientsArray *pOutput);

	int InfectedBonusArmor() const;

	void SendKillMessage(int Victim, const DeathContext &Context);
	void OnKillOrInfection(int Victim, const DeathContext &Context);

protected:
	void RoundTickBeforeInitialInfection();
	void RoundTickAfterInitialInfection();

	void PreparePlayerToJoin(CIcPlayer *pPlayer);
	void SetPlayerPickedTimestamp(CIcPlayer *pPlayer, int Timestamp) const;

	uint32_t InfectHumans(uint32_t NumHumansToInfect);
	void ForcePlayersBalance(uint32_t PlayersToBalance);
	void UpdateBalanceFactors();

	void MaybeSendStatistics();
	void CancelTheRound(ROUND_CANCELATION_REASON Reason);
	void AnnounceTheWinner(int NumHumans);
	void BroadcastInfectionComing(int InfectionTick);

private:
	void UpdateNinjaTargets();

	void ReservePlayerOwnSnapItems();
	void FreePlayerOwnSnapItems();

	void SendHintMessage();
	void FormatHintMessage(const CHintMessage &Message, dynamic_string *pBuffer, const char *pLanguage) const;

	void OnInfectionTriggered();

	void StartInfectionGameplay(int PlayersToInfect);

	void MaybeSuggestMoreRounds();
	void SnapMapMenu(int SnappingClient, CNetObj_GameInfo *pGameInfoObj);
	void FallInLoveIfInfectedEarly(CIcCharacter *pCharacter);
	void RewardTheKillers(CIcCharacter *pVictim, const DeathContext &Context);
	void GetPlayerCounter(int ClientException, int& NumHumans, int& NumInfected);
	int GetMinimumInfectedForPlayers(int PlayersNumber) const;

	int GetClientIdForNewWitch() const;
	bool IsSafeWitchCandidate(int ClientId) const;
	ClientsArray m_WitchCallers;

	struct PlayerScore
	{
		char aPlayerName[MAX_NAME_LENGTH];
		int Kills;
		int ClientId;
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

	bool m_AllowSurvivalAutostart = false;

	PlayerScore *GetSurvivalPlayerScore(int ClientId);
	PlayerScore *EnsureSurvivalPlayerScore(int ClientId);

private:
	int m_ZoneHandle_icDamage;
	int m_ZoneHandle_icTeleport;
	int m_ZoneHandle_icBonus;

	int m_MapWidth;
	int m_MapHeight;
	int* m_GrowingMap;
	EFinalExplosionState m_FinalExplosionState{};

	std::optional<bool> m_WinCheckEnabled;
	std::optional<bool> m_VotesEnabled;
	std::optional<int> m_RoundMinimumPlayers;
	std::optional<int> m_RoundMinimumInfected;
	std::optional<float> m_RoundTimeLimitSeconds;
	std::optional<int> m_RoundInfectionDelaySeconds;
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

	bool m_InfectedStarted;
	bool m_RoundStarted = false;
	bool m_SuggestMoreRounds = false;
	bool m_MoreRoundsSuggested = false;
	bool m_VanillaMapLoaded = false;

	int m_InfAmmoRegenTime[NB_INFWEAPON]{};
	int m_InfFireDelay[NB_INFWEAPON]{};
	int m_InfMaxAmmo[NB_INFWEAPON]{};
	float m_aInfWeaponForce[NB_INFWEAPON]{};
	static int64_t m_LastTipTime;
};

#endif
