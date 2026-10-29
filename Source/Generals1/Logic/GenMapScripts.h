#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenScriptTypes.h"

class UGenGameLogic;
class FGenObject;
class FGenMapData;
struct FGenPolygonTrigger;
struct FGenThingTemplate;
struct FGenMapObject;

/**
 * Something a map's script wants seen or heard (the camera, the letterbox, a caption, speech, music): worked out
 * in the step, carried out by the game thread. The action as the script gave it, with the waypoints it names
 * already looked up.
 */
struct GENERALS1_API FGenScriptClientEvent
{
	EGenScriptAction Action = EGenScriptAction::Unknown;
	TArray<FGenScriptParam> Params;
	FVector3f Pos = FVector3f::ZeroVector;      // the (first) waypoint the action names
	FVector3f Pos2 = FVector3f::ZeroVector;     // a second one (SETUP_CAMERA's look-at)
	bool bHasPos = false;
	bool bHasPos2 = false;
	uint32 Frame = 0;

	const FString& Str(int32 Index) const;
	int32 Int(int32 Index) const;
	float Real(int32 Index) const;
};

/** ScriptEngine's fade (CAMERA_FADE_ADD / _MULTIPLY): worked out per logic frame, drawn over the view. */
struct GENERALS1_API FGenScriptFade
{
	enum EType : uint8 { None, Add, Subtract, Saturate, Multiply };
	EType Type = None;
	float Value = 0.f;
};

/** A team of a map (Team.cpp / TeamPrototype): an instance of one of the map's team definitions. */
struct GENERALS1_API FGenMapTeam
{
	int32 Id = 0;
	int32 Proto = INDEX_NONE;           // index into the map's team definitions
	int32 Player = INDEX_NONE;          // whose team it is (the controlling player)
	bool bPlayerDefault = false;        // "team<PlayerName>": everything of the player's that is on no other team
	bool bActive = true;
	TArray<FGenObjectID> Members;       // (not for a player's default team: its members are worked out)
	TMap<int32, EGenRelationship> OverrideRelations; // Team::setOverrideTeamRelationship, by team id
	bool bEnteredOrExited = false;
};

/**
 * The script engine of a map (ScriptEngine.cpp with ScriptConditions.cpp and ScriptActions.cpp): every side's
 * scripts, evaluated each logic frame, with the map's teams and named objects, global flags, counters and
 * timers. Campaign missions are driven by it; the skirmish computer players have their own copy of the parts
 * they need (GenSkirmishScripts.cpp).
 *
 * Simulation side: it runs inside the step. What is only seen or heard is handed to the game thread as
 * FGenScriptClientEvent (TakeClientEvents). The conditions that wait on sound (speech, music) are worked out
 * from the length of the sound files, as the original's are, so a replay goes the same way.
 */
class GENERALS1_API FGenMapScripts
{
public:
	explicit FGenMapScripts(UGenGameLogic* InLogic);
	FGenMapScripts(const FGenMapScripts&) = delete;
	FGenMapScripts& operator=(const FGenMapScripts&) = delete;

	/** The map's scripts and teams (ScriptEngine::newMap, TeamFactory::initFromSides). Players are numbered as the
	 *  map's sides are. */
	void NewMap(const FGenMapData& Map, EGenDifficulty InDifficulty);
	/** The player a map object belongs to: its team's (originalOwner). */
	int32 GetMapObjectOwner(const FGenMapObject& MapObj) const;
	/** Before the first step: a map object joins its team (originalOwner) and gets its name and properties. */
	void OnMapObjectCreated(FGenObject& Obj, const FGenMapObject& MapObj);
	/** ScriptEngine::update, once per logic frame. */
	void Update(uint32 InFrame);

	/** FREEZE_TIME: only the scripts run (GameLogic::update stops after ScriptEngine::update). */
	bool IsTimeFrozen() const { return bTimeFrozen; }
	/** SET_FPS_LIMIT: the original's game slowed to the given frame rate during a cinematic (0: as usual). */
	int32 GetFpsLimit() const { return FpsLimit; }
	/** The fade over the view this frame. */
	const FGenScriptFade& GetFade() const { return Fade; }
	/** DISABLE_INPUT / ENABLE_INPUT (and VICTORY / DEFEAT, which take the input away too). */
	bool IsInputDisabled() const { return bInputDisabled; }
	/** PLAYER_EXCLUDE_FROM_SCORE_SCREEN */
	bool IsExcludedFromScore(int32 Player) const { return ExcludedFromScore.Contains(Player); }
	/** COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE: buttons taken out of a command set (lower-case set name -> slots). */
	bool IsButtonRemoved(const FString& CommandSet, int32 Slot) const;
	/** PLAYER_SET_RANKLEVELLIMIT (0: none). */
	int32 GetRankLevelLimit() const { return RankLevelLimit; }
	/** PLAYER_SCIENCE_AVAILABILITY: "Hidden" / "Disabled" sciences of a player (lower-case names). */
	bool IsScienceHidden(int32 Player, const FString& Science) const;
	/** SCRIPTING_OVERRIDE_HULK_LIFETIME in frames (-1: none). */
	int32 GetHulkLifetimeOverride() const { return HulkLifetimeOverride; }
	/** OPTIONS_SET_DRAWICON_UI_MODE: health bars and icons are drawn. */
	bool ShouldDrawIconUI() const { return bDrawIconUI; }

	/** What the game thread is to show or play: taken inside the logic gate. */
	void TakeClientEvents(TArray<FGenScriptClientEvent>& Out) { Out.Append(MoveTemp(ClientEvents)); ClientEvents.Reset(); }

	/** For traces and tests: a line about where the scripts stand. */
	FString Describe() const;
	/** The object a script knows by that name (null if there is none, or it is gone). */
	FGenObject* GetUnitNamed(const FString& Name) const;
	/** The team an object is on (its owner's default team if no other). */
	int32 GetObjectTeam(const FGenObject& Obj) const;
	/** Team::getRelationship's overrides: how a team's members see another team's (Neutral as the default: no override). */
	bool GetTeamRelationOverride(const FGenObject& From, const FGenObject& To, EGenRelationship& OutRelation) const;
	/** ScriptEngine::transferObjectName: the name of one object goes to another (a terrorist's to the car bomb it made). */
	void TransferName(const FGenObject& From, FGenObject& To);
	/** The camera's scripted movement is under way until this frame (CAMERA_MOVEMENT_FINISHED). */
	void NoteCameraBusyUntil(uint32 InFrame) { CameraBusyUntil = FMath::Max(CameraBusyUntil, InFrame); }

private:
	// --- engine ---
	struct FCounter { int32 Value = 0; bool bCountdown = false; };
	void ExecuteScripts(int32 Side, TArray<FGenScript>& Scripts);
	void ExecuteScript(FGenScript& Script);
	bool EvaluateConditions(const FGenScript& Script);
	bool EvaluateCondition(const FGenScriptCondition& C);
	void ExecuteActions(const TArray<FGenScriptAction>& Actions);
	void ExecuteAction(const FGenScriptAction& A);
	bool IsForDifficulty(const FGenScript& S) const;
	void CallSubroutine(const FString& Name);
	FGenScript* FindScript(const FString& Name);
	FGenScriptGroup* FindGroup(const FString& Name);
	void UpdateFade();
	void SetFade(const FGenScriptAction& A);
	void UpdateSequentialScripts();
	void Client(const FGenScriptAction& A, const FString* Waypoint = nullptr, const FString* Waypoint2 = nullptr);

	// --- names: players, teams, objects, places ---
	int32 GetPlayerNamed(const FString& Name) const;
	FGenMapTeam* GetTeamNamed(const FString& Name);
	FGenMapTeam* FindTeam(int32 Id);
	FGenMapTeam& CreateTeam(int32 ProtoIndex, int32 Player);
	FGenMapTeam* GetOrCreateTeamNamed(const FString& Name);
	int32 FindProto(const FString& Name) const;
	void SetObjectTeam(FGenObject& Obj, FGenMapTeam& Team);
	void GetTeamMembers(const FGenMapTeam& Team, TArray<FGenObject*>& Out, bool bIncludeDead = false) const;
	void DeleteTeamMembers(FGenMapTeam& Team);
	bool GetTeamCenter(const FGenMapTeam& Team, FVector2f& Out) const;
	void NameObject(FGenObject& Obj, const FString& Name);
	const FGenPolygonTrigger* GetTrigger(const FString& Name) const;
	bool GetWaypoint(const FString& Name, FVector3f& Out) const;
	const FGenMapObject* GetClosestWaypointOnPath(const FVector2f& From, const FString& Label) const;
	void GetWaypointPath(const FGenMapObject& Start, TArray<FVector2f>& Out) const;
	const TArray<const FGenThingTemplate*>& ResolveTypes(const FString& TypeOrList) const;

	// --- conditions ---
	bool PlayerUnitComparison(const FGenScriptCondition& C, int32 Player, int32 Comparison, int32 Count, const FString& Types) const;
	int32 CountInArea(int32 Player, const FGenPolygonTrigger& Area, TFunctionRef<bool(const FGenObject&)> Wanted) const;
	bool NamedEnteredArea(const FGenScriptCondition& C, const FString& Unit, const FString& Area) const;
	bool AttackedByPlayer(const FGenObject& Victim, int32 Player) const;
	bool IsSoundComplete(const FString& Event, TMap<FString, uint32>& Pending) const;
	bool TeamInside(const FGenMapTeam& Team, const FGenPolygonTrigger& Area, uint32 Surfaces, bool bAll) const;
	bool HasObjectStatus(const FGenObject& Obj, const FString& Status) const;

	// --- actions on units ---
	void FollowWaypoints(FGenObject& Unit, const FString& Label, bool bExact, const FVector2f* From = nullptr);
	void TeamFollowWaypoints(FGenMapTeam& Team, const FString& Label, bool bExact);
	void UseCommandButton(FGenObject& Obj, const FString& ButtonName, const FGenObject* Target, const FVector3f* Pos);
	FGenObject* CreateObjectOnTeam(const FString& Name, const FString& Thing, const FString& TeamName, const FVector3f& Pos, float Angle);
	void CreateReinforcements(const FString& TeamName, const FString& Waypoint);
	void IdleUnit(FGenObject& Obj);
	bool IsDisabledForScripts(const FGenObject& Obj) const;

	UGenGameLogic* Logic = nullptr;
	const FGenMapData* Map = nullptr;
	EGenDifficulty Difficulty = EGenDifficulty::Normal;
	uint32 Frame = 0;

	TArray<FGenScriptList> Lists;                   // by side (= player index)
	TMap<FString, FGenScript*> ScriptsByName;
	TMap<FString, FGenScriptGroup*> GroupsByName;
	TMap<FString, FCounter> Counters;
	TMap<FString, bool> Flags;
	int32 CurrentPlayer = INDEX_NONE;               // whose scripts run (<This Player>)
	FGenMapTeam* CallingTeam = nullptr;             // <This Team>
	const FGenScript* RunningScript = nullptr;
	int32 SubroutineDepth = 0;

	TArray<FGenTeamProto> Protos;
	TArray<int32> ProtoPlayers;                     // the player each definition's teams belong to
	TArray<TUniquePtr<FGenMapTeam>> Teams;          // pointers stay valid as the list grows
	TMap<int32, int32> DefaultTeams;                // player -> its default team's id
	TMap<FGenObjectID, int32> ObjectTeams;          // objects on a team that isn't their owner's default one
	int32 NextTeamId = 1;

	TMap<FString, FGenObjectID> Named;              // lower-case name -> object
	TSet<FString> NamesThatExisted;                 // ScriptEngine::didUnitExist
	TMap<FString, TArray<FString>> ObjectTypeLists;
	mutable TMap<FString, TArray<const FGenThingTemplate*>> ResolvedTypes;

	struct FSequential { int32 Team = 0; const FGenScript* Script = nullptr; int32 Instruction = 0; int32 FramesToWait = 0; };
	TArray<FSequential> SequentialScripts;
	TMap<FGenObjectID, int32> Flashing;             // NAMED_FLASH_WHITE: flashes still to come

	// ScriptEngine's own state
	FGenScriptFade Fade;
	int32 FadeFrame = 0, FadeIncrease = 0, FadeHold = 0, FadeDecrease = 0;
	float FadeMin = 1.f, FadeMax = 0.f;
	bool bTimeFrozen = false;
	int32 FpsLimit = 0;
	bool bInputDisabled = false;
	bool bDrawIconUI = true;
	bool bEnded = false;
	int32 RankLevelLimit = 0;
	int32 HulkLifetimeOverride = -1;
	uint32 CameraBusyUntil = 0;
	mutable TMap<FString, uint32> TestingSpeech;    // isSpeechComplete: name -> frame it is over
	mutable TMap<FString, uint32> TestingAudio;
	FString MusicTrack;                             // setCurrentTrackName
	uint32 MusicStartFrame = 0;
	TSet<int32> ExcludedFromScore;
	TMap<FString, TArray<int32>> RemovedButtons;    // lower-case command set -> slots
	TMap<int32, TSet<FString>> HiddenSciences;      // player -> lower-case sciences
	TArray<FGenScriptClientEvent> ClientEvents;
};
