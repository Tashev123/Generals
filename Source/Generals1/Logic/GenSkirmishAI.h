#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenScriptTypes.h"
#include "Logic/GenTeams.h"
#include "Map/GenMapData.h"

class UGenGameLogic;
class FGenObject;
struct FGenThingTemplate;
struct FGenPolygonTrigger;
class FGenChunkReader;

/** Reading scripts and teams out of a script file or a map (Scripts.cpp, SidesList.cpp). */
namespace GenScriptIO
{
	/** A PlayerScriptsList chunk: one script list per side (or per player of a script file), in order. */
	GENERALS1_API void ReadScriptLists(FGenChunkReader& R, int32 ChunkEnd, TArray<FGenScriptList>& Out, int32* OutNumScripts = nullptr);
	/** A team's dictionary (TeamTemplateInfo). */
	GENERALS1_API void ReadTeamProto(const FGenDict& D, FGenTeamProto& T);
}

/** Data\Scripts\SkirmishScripts.scb: the scripts and teams of every skirmish computer player, by faction. */
class GENERALS1_API FGenScriptLibrary
{
public:
	static FGenScriptLibrary& Get();

	bool Initialize();
	/** Scripts of a skirmish player such as "SkirmishChina" (null if there are none). */
	const FGenScriptList* FindList(const FString& PlayerName) const { return Lists.Find(PlayerName); }
	void GetTeams(const FString& Owner, TArray<FGenTeamProto>& Out) const;

private:
	bool bInitialized = false;
	bool bLoaded = false;
	TMap<FString, FGenScriptList> Lists;
	TArray<FGenTeamProto> Teams;
};

// ---- AIData.ini -------------------------------------------------------------------------------

struct GENERALS1_API FGenAIBuildListEntry
{
	FString Template;
	FVector2f Location = FVector2f::ZeroVector;   // in the build list's own frame; moved to the start position per game
	float Angle = 0.f;                            // radians
	bool bInitiallyBuilt = false;
	bool bAutomaticallyBuild = true;
	int32 Rebuilds = 0;
};

struct GENERALS1_API FGenAISideInfo
{
	int32 Gatherers[3] = { 2, 2, 2 };             // per difficulty
	FString BaseDefenseStructure;
	TArray<FString> SkillSets[5];                 // SkillSet1..5: sciences to buy, in order
};

struct FGenIniBlock;

class GENERALS1_API FGenAIData
{
public:
	static FGenAIData& Get();
	void Initialize();
	/** A map's map.ini: its AIData over this one's, for as long as the map lasts. */
	void ApplyMapIni(const TArray<TSharedPtr<FGenIniBlock>>& Blocks);

	float AttackPriorityDistanceModifier = 100.f; // "The distance required to reduce attack priority by 1."
	float StructureSeconds = 0.f;
	float TeamSeconds = 10.f;
	int32 Wealthy = 7000;
	int32 Poor = 2000;
	float StructuresWealthyRate = 2.f;
	float StructuresPoorRate = 0.6f;
	float TeamsWealthyRate = 2.f;
	float TeamsPoorRate = 0.6f;
	float TeamResourcesToStart = 0.1f;
	float GuardInnerModifierAI = 1.1f;
	float GuardOuterModifierAI = 1.333f;
	float GuardInnerModifierHuman = 1.8f;
	float GuardOuterModifierHuman = 2.2f;
	float MaxRecruitRadius = 500.f;
	float SkirmishBaseDefenseExtraDistance = 150.f;
	float SupplyCenterSafeRadius = 300.f;
	// Group movement (AIGroup): how many of a kind it takes, and how far the move has to be.
	int32 MinInfantryForGroup = 3;
	int32 MinVehiclesForGroup = 4;
	float MinDistanceForGroup = 100.f;
	float DistanceRequiresGroup = 0.f;
	int32 RebuildDelaySeconds = 30;
	bool bRotateSkirmishBases = false;

	TMap<FString, FGenAISideInfo> Sides;                          // key: side ("America")
	TMap<FString, TArray<FGenAIBuildListEntry>> BuildLists;       // key: side

private:
	void ApplyBlocks(const TArray<TSharedPtr<FGenIniBlock>>& Blocks);
	bool bInitialized = false;
	TSharedPtr<FGenAIData> MapIniSaved; // what it was before the loaded map's map.ini
};

// ---- the computer player ----------------------------------------------------------------------

/** BuildListInfo: one building the computer player has, wants or lost, and where it goes. */
struct GENERALS1_API FGenBuildListInfo
{
	const FGenThingTemplate* Template = nullptr;
	FVector3f Location = FVector3f::ZeroVector;
	float Angle = 0.f;
	int32 NumRebuilds = 0;
	bool bInitiallyBuilt = false;
	bool bAutomaticBuild = false;
	bool bPriorityBuild = false;
	bool bUnderConstruction = false;
	FGenObjectID Object = GenInvalidObjectID;
	uint32 ObjectTimestamp = 0;                 // > 0: it was built once; frame it was built / found destroyed
	bool bSupplyBuilding = false;
	int32 DesiredGatherers = 0;
	int32 CurrentGatherers = 0;
};

/** WorkOrder: units of one type still to be produced for a team. */
struct GENERALS1_API FGenWorkOrder
{
	const FGenThingTemplate* Thing = nullptr;
	FGenObjectID Factory = GenInvalidObjectID;  // the factory currently building one
	int32 NumCompleted = 0;
	int32 NumRequired = 1;
	bool bRequired = false;
	bool bResourceGatherer = false;

	bool IsWaitingToBuild() const { return NumCompleted < NumRequired && Factory == GenInvalidObjectID; }
};

/** TeamInQueue: a team being assembled. */
struct GENERALS1_API FGenTeamInQueue
{
	TArray<FGenWorkOrder> Orders;
	int32 Team = 0;
	bool bPriorityBuild = false;
	bool bReinforcement = false;
	FGenObjectID ReinforcementID = GenInvalidObjectID;
	uint32 FrameStarted = 0;
};

/** Team instance: the game's one team registry's (GenTeams.h), the same one a map's script engine uses. A player's default team
 *  holds whatever it has that is on no other team. */

/** A script whose actions run one after the other, each waiting for the team to finish the previous one. */
struct GENERALS1_API FGenSequentialScript
{
	int32 Team = 0;
	TArray<const FGenScript*> Scripts;          // the running one first, then those queued behind it
	int32 CurrentInstruction = -1;
	int32 FramesToWait = -1;
	bool bDontAdvance = false;
};

/**
 * A computer player: AIPlayer / AISkirmishPlayer (base building from a build list, dozers, gatherers,
 * assembling teams) driven by scripts. In a skirmish the build list is AIData.ini's and the player has the
 * part of the script engine its own scripts need (GenSkirmishScripts.cpp); in a campaign mission the build
 * list is the map's own side list and the scripts are the map's, so this is production only.
 */
class GENERALS1_API FGenSkirmishAI
{
public:
	FGenSkirmishAI(UGenGameLogic* InLogic, int32 InPlayer, EGenDifficulty InDifficulty);
	FGenSkirmishAI(const FGenSkirmishAI&) = delete;
	FGenSkirmishAI& operator=(const FGenSkirmishAI&) = delete;

	void NewMap();
	void Update(uint32 Frame);
	/** Team::notifyTeamOfObjectDeath: one of this player's team's members was destroyed. */
	void NotifyTeamOfObjectDeath(FGenObject& Obj);
	void OnUnitProduced(const FGenObject& Factory, FGenObject& Unit);
	void OnStructureProduced(FGenObject& Building);

	int32 GetPlayer() const { return Player; }
	/** Player::getAiBaseCenter */
	bool GetBaseCenter(FVector3f& Out) const { Out = BaseCenter; return bBaseCenterSet; }
	/** The player this one is currently after (AISkirmishPlayer::getAiEnemy), or INDEX_NONE. */
	int32 GetEnemy();
	/** One line for traces: money, buildings, teams, counters. */
	FString Describe() const;

	// --- what a map's script asks this player to do (GenMapScriptActions.cpp) ---
	/** A_BUILD_TEAM: Team::buildSpecificTeam for a team definition the map named. */
	void BuildTeamFromScript(int32 ProtoIndex) { BuildSpecificTeam(ProtoIndex, true); }
	/** What a map's own scripts ask of the computer player of a Generals' Challenge side. */
	bool ScriptSupplySourceSafe(int32 MinSupplies) { return IsSupplySourceSafe(MinSupplies); }
	void ScriptBuildBySupplies(int32 MinimumCash, const FString& ThingName) { BuildBySupplies(MinimumCash, ThingName); }
	void ScriptFollowApproachPath(FGenTeam& Team, const FString& Label, bool bMoveToStartOnly) { TeamFollowApproachPath(Team, Label, bMoveToStartOnly); }
	void ScriptGuardSupplyCenter(FGenTeam& Team, int32 MinSupplies);
	/** A_SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST. */
	void FireSpecialPowerFromScript(const FString& PowerName) { FireSpecialPowerAtMostCost(PowerName); }
	/** A_RECRUIT_TEAM: Player::recruitSpecificTeam. */
	void RecruitTeamFromScript(int32 ProtoIndex, float RecruitRadius) { RecruitSpecificTeam(ProtoIndex, RecruitRadius); }
	/** A_AI_PLAYER_BUILD_UPGRADE */
	void BuildUpgradeFromScript(const FString& UpgradeName) { BuildUpgrade(UpgradeName); }
	/** A_SET_BASE_CONSTRUCTION_SPEED: AIPlayer::setTeamDelaySeconds. */
	void SetTeamSecondsFromScript(int32 Seconds) { ScriptTeamSeconds = Seconds; }
	/** A_PLAYER_SELECT_SKILLSET: Player::friend_setSkillset, numbered from 1. */
	void SetSkillsetFromScript(int32 Skillset) { SkillSetSelector = FMath::Clamp(Skillset - 1, 0, 4); }

private:
	// --- AIPlayer / AISkirmishPlayer (GenSkirmishAI.cpp) ---
	void AdjustBuildList();
	/** Player::addToBuildList: a building the game already placed becomes an entry of its own. */
	void AddToBuildList(FGenObject& Building);
	void ComputeCenterAndRadiusOfBase();
	FGenObject* BuildStructureNow(FGenBuildListInfo& Info);
	FGenObject* BuildStructureWithDozer(FGenBuildListInfo& Info);
	void CheckForSupplyCenter(FGenBuildListInfo& Info, const FGenObject& Building);
	void DoBaseBuilding();
	void ProcessBaseBuilding();
	void DoTeamBuilding();
	void QueueUnits();
	void QueueSupplyTruck();
	void QueueDozer();
	bool DozerInQueue() const;
	FGenObject* FindDozer(const FVector3f& Pos);
	FGenObject* FindFactory(const FGenThingTemplate& Thing, bool bBusyOK);
	bool StartTraining(FGenWorkOrder& Order, bool bBusyOK);
	bool IsPossibleToBuildTeam(const FGenTeamProto& Proto, bool bRequireIdleFactory, bool& bOutNotEnoughMoney);
	bool IsAGoodIdeaToBuildTeam(int32 ProtoIndex);
	bool SelectTeamToBuild();
	bool SelectTeamToReinforce(int32 MinPriority);
	void BuildSpecificTeam(int32 ProtoIndex, bool bPriorityBuild);
	void RecruitSpecificTeam(int32 ProtoIndex, float RecruitRadius);
	void CheckReadyTeams();
	void CheckQueuedTeams();
	void BuildSpecificBuilding(const FString& ThingName);
	void BuildBySupplies(int32 MinimumCash, const FString& ThingName);
	void BuildBaseDefenseStructure(const FString& ThingName, bool bFlank);
	void BuildSpecificBuildingNearestTeam(const FString& ThingName, const FGenTeam& Team);
	void AddToPriorityBuildList(const FGenThingTemplate& Thing, const FVector3f& Pos, float Angle);
	FGenObject* FindSupplyCenter(int32 MinimumCash);
	bool IsSupplySourceSafe(int32 MinSupplies);
	bool IsLocationSafe(const FVector3f& Pos, const FGenThingTemplate& Thing) const;
	bool FindPlaceToBuild(const FGenThingTemplate& Thing, float Angle, float Limit, FVector3f& InOutPos) const;
	void AcquireEnemy();
	void GetPlayerStructureBounds(int32 PlayerIndex, FBox2f& OutBounds) const;
	/** A unit type of the given kind that one of our factories can build right now. */
	const FGenThingTemplate* FindBuildableUnit(TFunctionRef<bool(const FGenThingTemplate&)> Wanted, bool bBusyOK);
	int32 GetMoney() const;
	int32 CountGatherers(FGenObjectID Center, bool bRedock);
	// --- upgrades, general's promotions and special powers ---
	void DoUpgradesAndSkills();
	void BuildUpgrade(const FString& UpgradeName);
	bool DoCommandButton(FGenObject& Obj, const struct FGenCommandButton& Button);
	bool HasCommandButton(const FGenObject& Obj, const struct FGenCommandButton& Button) const;
	void FireSpecialPowerAtMostCost(const FString& PowerName);
	bool ComputeSuperweaponTarget(bool bTargetMilitaryUnits, int32 EnemyIndex, float WeaponRadius, FVector3f& OutPos) const;
	int32 GetPlayerSuperweaponValue(const FVector2f& Center, int32 EnemyIndex, float Radius, bool bIncludeMilitaryUnits) const;

	// --- teams (the game's team registry, GenTeams.h) ---
	FGenTeam* FindTeam(int32 TeamId) const { return TF.FindTeam(TeamId); }
	/** TeamFactory::createInactiveTeam, plus the definition's production condition when it runs its actions. */
	FGenTeam& CreateTeam(int32 ProtoIndex);
	int32 CountTeamInstances(int32 ProtoIndex) const { return TF.CountTeamInstances(ProtoIndex); }
	int32 FindProto(const FString& Name) const { return TF.FindProto(Name); }
	void SetObjectTeam(FGenObjectID Object, int32 TeamId);
	int32 GetObjectTeam(FGenObjectID Object) const { return TF.GetObjectTeam(Object); }
	void SetTeamActive(FGenTeam& Team);
	void DeleteTeam(int32 TeamId);
	void PurgeDeadMembers();
	FGenObject* TryToRecruit(const FGenTeam& ForTeam, const FGenThingTemplate& Thing, const FVector3f& Home, float MaxDist);
	bool TeamHasUnits(const FGenTeam& Team) const;
	bool TeamIsIdle(const FGenTeam& Team) const;
	bool GetTeamCenter(const FGenTeam& Team, FVector2f& OutCenter) const { return TF.GetTeamCenter(Team, OutCenter); }
	void ForEachMember(const FGenTeam& Team, TFunctionRef<void(FGenObject&)> Fn) const;
	/** Whether this player's teams and definitions are its own: in a campaign the registry holds every side's. */
	bool Owns(const FGenTeam& Team) const { return Team.Player == Player; }
	bool Owns(int32 ProtoIndex) const { return TF.ProtoPlayers.IsValidIndex(ProtoIndex) && TF.ProtoPlayers[ProtoIndex] == Player; }
	/** The id of this player's default team (Player::setDefaultTeam). */
	int32 DefaultTeamId() const { return TF.EnsureDefaultTeam(Player).Id; }
	void UpdateTeamStates();
	void UpdateGenericScripts();

	// --- script engine (GenSkirmishScripts.cpp) ---
	void InitScripts();
	void UpdateScripts();
	void ExecuteScripts(TArray<FGenScript>& Scripts);
	void ExecuteScript(FGenScript& Script);
	bool EvaluateConditions(const FGenScript& Script, FGenTeam* ThisTeam);
	bool EvaluateCondition(const FGenScriptCondition& Condition);
	void ExecuteActions(const TArray<FGenScriptAction>& Actions);
	void ExecuteAction(const FGenScriptAction& Action);
	void ExecuteActionsForTeam(const TArray<FGenScriptAction>& Actions, FGenTeam* ThisTeam);
	void RunScript(const FString& ScriptName, FGenTeam* ThisTeam);
	void CallSubroutine(const FString& ScriptName);
	FGenScript* FindScript(const FString& Name);
	FGenScriptGroup* FindGroup(const FString& Name);
	/** TeamPrototype::evaluateProductionCondition. In a skirmish this engine owns its player's scripts; in a
	 *  mission the map's engine does, so it asks there. */
	bool EvaluateProductionCondition(int32 ProtoIndex);
	void UpdateSequentialScripts();
	void AppendSequentialScript(int32 TeamId, const FGenScript* Script);
	void RemoveSequentialScripts(int32 TeamId);
	FGenTeam* GetTeamNamed(const FString& Name);
	int32 GetPlayerNamed(const FString& Name);
	const FGenPolygonTrigger* GetTriggerNamed(const FString& Name);
	int32 CountObjects(int32 PlayerIndex, const FString& TypeOrList, const FGenPolygonTrigger* Area) const;
	bool IsInTypeList(const FString& TypeOrList, const FGenThingTemplate& Thing) const;
	/** The templates an object type parameter stands for: one type, or every type of a named object list. */
	const TArray<const FGenThingTemplate*>& ResolveTypes(const FString& TypeOrList) const;
	void TeamGuardArea(FGenTeam& Team, const FGenPolygonTrigger& Area);
	void TeamFollowApproachPath(FGenTeam& Team, const FString& Label, bool bMoveToStartOnly);

	UGenGameLogic* Logic = nullptr;
	int32 Player = INDEX_NONE;
	EGenDifficulty Difficulty = EGenDifficulty::Normal;
	FString Side;
	uint32 Frame = 0;
	bool bCampaign = false;   // a mission's computer side: the map's scripts move it, this only builds

	// AIPlayer
	TArray<FGenBuildListInfo> BuildList;
	TArray<FGenTeamInQueue> TeamBuildQueue;
	TArray<FGenTeamInQueue> TeamReadyQueue;
	bool bReadyToBuildTeam = false;
	bool bReadyToBuildStructure = false;
	int32 TeamTimer = 0;
	int32 ScriptTeamSeconds = -1;           // AIPlayer::m_teamSeconds, once a script has set it
	int32 StructureTimer = 0;
	int32 BuildDelay = 0;
	int32 TeamDelay = 0;
	FVector3f BaseCenter = FVector3f::ZeroVector;
	float BaseRadius = 0.f;
	bool bBaseCenterSet = false;
	FGenObjectID CurWarehouse = GenInvalidObjectID;
	// (Whether the player may build is on the player record, UGenGameLogic::CanBuildBase / CanBuildUnits -
	// Player::m_canBuildBase / m_canBuildUnits, which a map's scripts set for any side.)
	int32 CurrentEnemy = INDEX_NONE;
	uint32 FrameToCheckEnemy = 0;
	int32 SkillSetSelector = INDEX_NONE;        // which of the side's skill sets this player buys from
	// AISkirmishPlayer base defense placement
	int32 CurFrontBaseDefense = 0;
	int32 CurFlankBaseDefense = 0;
	float CurFrontLeftDefenseAngle = 0.f, CurFrontRightDefenseAngle = 0.f;
	float CurLeftFlankLeftDefenseAngle = 0.f, CurLeftFlankRightDefenseAngle = 0.f;
	float CurRightFlankLeftDefenseAngle = 0.f, CurRightFlankRightDefenseAngle = 0.f;

	// Teams: the game's one team registry (GenTeams.h), shared with a map's script engine, so that the teams this
	// player builds are the ones the scripts name.
	FGenTeamFactory& TF;
	TArray<FGenTeamProto>& Protos = TF.Protos;
	TArray<TUniquePtr<FGenTeam>>& Teams = TF.Teams;         // pointers stay valid as the list grows
	TMap<int32, int32>& DefaultTeams = TF.DefaultTeams;     // player -> its default team's id
	TMap<FGenObjectID, int32>& ObjectTeams = TF.ObjectTeams;
	int32& NextTeamId = TF.NextTeamId;

	// Script engine state (per player: flags, counters and timers of two computer players never mix)
	struct FCounter { int32 Value = 0; bool bCountdown = false; };
	FGenScriptList Scripts;
	TMap<FString, FGenScript*> ScriptsByName;
	TMap<FString, FGenScriptGroup*> GroupsByName;
	TMap<FString, FCounter> Counters;
	TMap<FString, bool> Flags;
	TMap<FString, TArray<FString>> ObjectTypeLists;
	TArray<FGenSequentialScript> SequentialScripts;
	FGenTeam* CallingTeam = nullptr;            // <This Team> for actions
	const FGenScript* RunningScript = nullptr;  // the script whose actions are being carried out
	FGenTeam* ConditionTeam = nullptr;          // <This Team> for conditions
	int32 SubroutineDepth = 0;
	// Caches
	struct FCountCache { uint32 Frame = MAX_uint32; TMap<const FGenThingTemplate*, int32> Counts; };
	mutable TMap<int32, FCountCache> CountCaches;                               // per player: finished objects by type, this frame
	mutable TMap<FString, TArray<const FGenThingTemplate*>> ResolvedTypes;
};
