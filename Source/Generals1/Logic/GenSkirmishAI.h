#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

class UGenGameLogic;
class FGenObject;
struct FGenThingTemplate;
struct FGenPolygonTrigger;

/** GameDifficulty of a computer player; scripts are tagged with the levels they run on. */
enum class EGenDifficulty : uint8 { Easy, Normal, Hard };

// ---- script data (Scripts.cpp) ----------------------------------------------------------------

// The conditions and actions the skirmish scripts use. Anything else in a script file loads as Unknown
// (conditions: false, actions: nothing).
#define GEN_SCRIPT_CONDITIONS(X) \
	X(CONDITION_FALSE) X(CONDITION_TRUE) X(COUNTER) X(FLAG) X(TIMER_EXPIRED) \
	X(PLAYER_HAS_OBJECT_COMPARISON) X(NAMED_NOT_DESTROYED) X(BUILT_BY_PLAYER) X(PLAYER_HAS_CREDITS) \
	X(START_POSITION_IS) X(SKIRMISH_SPECIAL_POWER_READY) X(TEAM_HAS_UNITS) \
	X(PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA) X(PLAYER_POWER_COMPARE_PERCENT) X(SKIRMISH_VALUE_IN_AREA) \
	X(TEAM_INSIDE_AREA_PARTIALLY) X(PLAYER_HAS_POWER) X(MUSIC_TRACK_HAS_COMPLETED) X(SKIRMISH_PLAYER_FACTION) \
	X(PLAYER_HAS_NO_POWER) X(SUPPLY_SOURCE_SAFE) X(PLAYER_BUILT_UPGRADE) X(SKIRMISH_UNOWNED_FACTION_UNIT_EXISTS) \
	X(TEAM_DESTROYED) X(PLAYER_ALL_DESTROYED) X(SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE) X(PLAYER_LOST_OBJECT_TYPE) \
	X(UNIT_HEALTH) X(PLAYER_ACQUIRED_SCIENCE) X(TEAM_ENTERED_AREA_ENTIRELY) X(PLAYER_TRIGGERED_SPECIAL_POWER) \
	X(PLAYER_COMPLETED_SPECIAL_POWER) X(TEAM_SOME_HAVE_OBJECT_STATUS) X(TEAM_OUTSIDE_AREA_ENTIRELY)

#define GEN_SCRIPT_ACTIONS(X) \
	X(NO_OP) X(SET_FLAG) X(SET_COUNTER) X(INCREMENT_COUNTER) X(DECREMENT_COUNTER) X(SET_TIMER) X(SET_MILLISECOND_TIMER) \
	X(SET_RANDOM_TIMER) X(SET_RANDOM_MSEC_TIMER) X(ENABLE_SCRIPT) X(DISABLE_SCRIPT) X(CALL_SUBROUTINE) \
	X(OBJECTLIST_ADDOBJECTTYPE) X(SET_ATTACK_PRIORITY_THING) X(TEAM_APPLY_ATTACK_PRIORITY_SET) \
	X(TEAM_GUARD_AREA) X(TEAM_HUNT) X(TEAM_ATTACK_AREA) X(TEAM_STOP) X(TEAM_STOP_AND_DISBAND) \
	X(TEAM_EXECUTE_SEQUENTIAL_SCRIPT) X(TEAM_STOP_SEQUENTIAL_SCRIPT) X(TEAM_AVAILABLE_FOR_RECRUITMENT) \
	X(TEAM_MERGE_INTO_TEAM) X(TEAM_DECREASE_PRIORITY) X(TEAM_INCREASE_PRIORITY) X(TEAM_SET_ATTITUDE) \
	X(TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE) X(TEAM_HUNT_WITH_COMMAND_BUTTON) \
	X(SKIRMISH_BUILD_BUILDING) X(SKIRMISH_BUILD_STRUCTURE_FRONT) X(SKIRMISH_BUILD_STRUCTURE_FLANK) \
	X(SKIRMISH_BUILD_BASE_DEFENSE_FRONT) X(SKIRMISH_BUILD_BASE_DEFENSE_FLANK) X(AI_PLAYER_BUILD_SUPPLY_CENTER) \
	X(AI_PLAYER_BUILD_TYPE_NEAREST_TEAM) X(AI_PLAYER_BUILD_UPGRADE) X(BUILD_TEAM) X(RECRUIT_TEAM) \
	X(SKIRMISH_MOVE_TO_APPROACH_PATH) X(SKIRMISH_FOLLOW_APPROACH_PATH) \
	X(PLAYER_GIVE_MONEY) X(PLAYER_SELL_EVERYTHING) X(PLAYER_ENABLE_UNIT_CONSTRUCTION) X(PLAYER_ENABLE_BASE_CONSTRUCTION) \
	X(TEAM_ENTER_NAMED) X(TEAM_USE_COMMANDBUTTON_ABILITY) X(SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST) X(MUSIC_SET_TRACK) \
	X(TEAM_LOAD_TRANSPORTS) X(TEAM_CAPTURE_NEAREST_UNOWNED_FACTION_UNIT) X(TEAM_PARTIAL_USE_COMMANDBUTTON) \
	X(TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_OBJECTTYPE) X(TEAM_EXIT_ALL) X(TEAM_GARRISON_NEAREST_BUILDING) \
	X(SKIRMISH_WAIT_FOR_COMMANDBUTTON_AVAILABLE_ALL) X(TEAM_GUARD_IN_TUNNEL_NETWORK)

enum class EGenScriptCondition : uint8
{
	Unknown,
#define GEN_X(Name) C_##Name,
	GEN_SCRIPT_CONDITIONS(GEN_X)
#undef GEN_X
};

enum class EGenScriptAction : uint8
{
	Unknown,
#define GEN_X(Name) A_##Name,
	GEN_SCRIPT_ACTIONS(GEN_X)
#undef GEN_X
};

/** Parameter of a condition or action: which of the fields matters depends on what it is used for. */
struct GENERALS1_API FGenScriptParam
{
	int32 Int = 0;
	float Real = 0.f;
	FString String;
};

struct GENERALS1_API FGenScriptCondition
{
	EGenScriptCondition Type = EGenScriptCondition::Unknown;
	FString TypeName;
	TArray<FGenScriptParam> Params;
	// Result cache of conditions that are expensive to evaluate (Condition::m_customData / m_customFrame).
	mutable int32 CustomData = 0;
	mutable uint32 CustomFrame = 0;
};

struct GENERALS1_API FGenScriptAction
{
	EGenScriptAction Type = EGenScriptAction::Unknown;
	FString TypeName;
	TArray<FGenScriptParam> Params;
};

/** One script: IF (any of the OR blocks, each an AND of conditions) THEN actions ELSE false-actions. */
struct GENERALS1_API FGenScript
{
	FString Name;
	bool bActive = true;
	bool bOneShot = false;
	bool bEasy = true, bNormal = true, bHard = true;
	bool bSubroutine = false;
	int32 DelaySeconds = 0;                  // evaluate only every N seconds
	TArray<TArray<FGenScriptCondition>> Conditions;
	TArray<FGenScriptAction> Actions;
	TArray<FGenScriptAction> FalseActions;
	uint32 FrameToEvaluate = 0;
	FString ConditionTeam;                   // team named in the conditions: evaluated once per instance of it
};

struct GENERALS1_API FGenScriptGroup
{
	FString Name;
	bool bActive = true;
	bool bSubroutine = false;
	TArray<FGenScript> Scripts;
};

struct GENERALS1_API FGenScriptList
{
	TArray<FGenScript> Scripts;
	TArray<FGenScriptGroup> Groups;
};

/** Team definition (TeamTemplateInfo): which units, when it may be built, which scripts drive it. */
struct GENERALS1_API FGenTeamProto
{
	static constexpr int32 MaxGenericScripts = 16;

	struct FUnits { FString Unit; int32 Min = 0; int32 Max = 0; };

	FString Name;
	FString Owner;                           // "SkirmishChina"
	bool bSingleton = false;
	TArray<FUnits> Units;
	int32 ProductionPriority = 0;
	int32 SuccessIncrease = 0;
	int32 FailureDecrease = 0;
	int32 MaxInstances = 0;
	int32 InitialIdleFrames = 0;
	bool bExecuteActions = false;            // run the production condition's actions on the team while it is built
	bool bAutoReinforce = false;
	bool bRecruitable = false;
	float DestroyedThreshold = 0.f;
	FString ProductionCondition;
	FString OnCreate, OnDestroyed, OnIdle, OnEnemySighted, OnAllClear, OnUnitDestroyed;
	FString Generic[MaxGenericScripts];

	// Runtime state of the prototype (TeamPrototype)
	const FGenScript* ConditionScript = nullptr;
	bool bConditionAlwaysFalse = false;
	uint32 ConditionFrameToEvaluate = 0;
};

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

class GENERALS1_API FGenAIData
{
public:
	static FGenAIData& Get();
	void Initialize();

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
	int32 RebuildDelaySeconds = 30;
	bool bRotateSkirmishBases = false;

	TMap<FString, FGenAISideInfo> Sides;                          // key: side ("America")
	TMap<FString, TArray<FGenAIBuildListEntry>> BuildLists;       // key: side

private:
	bool bInitialized = false;
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

/** Team instance. Id 0 is the player's default team: everything that belongs to no other team. */
struct GENERALS1_API FGenTeam
{
	int32 Id = 0;
	int32 Proto = INDEX_NONE;                   // index into the player's team prototypes
	bool bActive = false;                       // false while it is being assembled
	bool bCreated = false;                      // activated this frame: the on-create script is due
	TArray<FGenObjectID> Members;
	bool bGenericPending[FGenTeamProto::MaxGenericScripts];
	int8 Recruitable = -1;                      // -1: as the prototype says
	int32 CurUnits = 0;
	int32 DestroyThreshold = -1;
	bool bWasIdle = false;
	bool bEnteredOrExited = false;
	TArray<uint8> InsideFlags;                  // per watched trigger: was the whole team inside last frame

	FGenTeam() { for (bool& B : bGenericPending) { B = true; } }
};

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
 * A skirmish computer player: AIPlayer / AISkirmishPlayer (base building from AIData.ini's build list,
 * dozers, gatherers, assembling teams) driven by the original skirmish scripts, with the part of the
 * script engine those scripts need (ScriptEngine, ScriptConditions, ScriptActions, Team).
 */
class GENERALS1_API FGenSkirmishAI
{
public:
	FGenSkirmishAI(UGenGameLogic* InLogic, int32 InPlayer, EGenDifficulty InDifficulty);
	FGenSkirmishAI(const FGenSkirmishAI&) = delete;
	FGenSkirmishAI& operator=(const FGenSkirmishAI&) = delete;

	void NewMap();
	void Update(uint32 Frame);
	void OnUnitProduced(const FGenObject& Factory, FGenObject& Unit);
	void OnStructureProduced(FGenObject& Building);

	int32 GetPlayer() const { return Player; }
	/** The player this one is currently after (AISkirmishPlayer::getAiEnemy), or INDEX_NONE. */
	int32 GetEnemy();
	/** One line for traces: money, buildings, teams, counters. */
	FString Describe() const;

private:
	// --- AIPlayer / AISkirmishPlayer (GenSkirmishAI.cpp) ---
	void AdjustBuildList();
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

	// --- teams ---
	FGenTeam* FindTeam(int32 TeamId);
	FGenTeam& CreateTeam(int32 ProtoIndex);
	int32 CountTeamInstances(int32 ProtoIndex) const;
	int32 FindProto(const FString& Name) const;
	void SetObjectTeam(FGenObjectID Object, int32 TeamId);
	int32 GetObjectTeam(FGenObjectID Object) const;
	void SetTeamActive(FGenTeam& Team);
	void DeleteTeam(int32 TeamId);
	void PurgeDeadMembers();
	FGenObject* TryToRecruit(const FGenTeam& ForTeam, const FGenThingTemplate& Thing, const FVector3f& Home, float MaxDist);
	bool TeamHasUnits(const FGenTeam& Team) const;
	bool TeamIsIdle(const FGenTeam& Team) const;
	bool GetTeamCenter(const FGenTeam& Team, FVector2f& OutCenter) const;
	void ForEachMember(const FGenTeam& Team, TFunctionRef<void(FGenObject&)> Fn) const;
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
	bool EvaluateProductionCondition(FGenTeamProto& Proto);
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

	// AIPlayer
	TArray<FGenBuildListInfo> BuildList;
	TArray<FGenTeamInQueue> TeamBuildQueue;
	TArray<FGenTeamInQueue> TeamReadyQueue;
	bool bReadyToBuildTeam = false;
	bool bReadyToBuildStructure = false;
	int32 TeamTimer = 0;
	int32 StructureTimer = 0;
	int32 BuildDelay = 0;
	int32 TeamDelay = 0;
	FVector3f BaseCenter = FVector3f::ZeroVector;
	float BaseRadius = 0.f;
	bool bBaseCenterSet = false;
	FGenObjectID CurWarehouse = GenInvalidObjectID;
	bool bCanBuildUnits = true;
	bool bCanBuildBase = true;
	int32 CurrentEnemy = INDEX_NONE;
	uint32 FrameToCheckEnemy = 0;
	int32 SkillSetSelector = INDEX_NONE;        // which of the side's skill sets this player buys from
	// AISkirmishPlayer base defense placement
	int32 CurFrontBaseDefense = 0;
	int32 CurFlankBaseDefense = 0;
	float CurFrontLeftDefenseAngle = 0.f, CurFrontRightDefenseAngle = 0.f;
	float CurLeftFlankLeftDefenseAngle = 0.f, CurLeftFlankRightDefenseAngle = 0.f;
	float CurRightFlankLeftDefenseAngle = 0.f, CurRightFlankRightDefenseAngle = 0.f;

	// Teams
	TArray<FGenTeamProto> Protos;
	TArray<TUniquePtr<FGenTeam>> Teams;         // [0] = default team; pointers to teams stay valid as the list grows
	int32 NextTeamId = 1;
	TMap<FGenObjectID, int32> ObjectTeams;

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
	FGenTeam* ConditionTeam = nullptr;          // <This Team> for conditions
	int32 SubroutineDepth = 0;
	// Caches
	struct FCountCache { uint32 Frame = MAX_uint32; TMap<const FGenThingTemplate*, int32> Counts; };
	mutable TMap<int32, FCountCache> CountCaches;                               // per player: finished objects by type, this frame
	mutable TMap<FString, TArray<const FGenThingTemplate*>> ResolvedTypes;
};
