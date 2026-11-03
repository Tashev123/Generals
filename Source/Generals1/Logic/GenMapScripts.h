#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenScriptTypes.h"
#include "Logic/GenTeams.h"

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
	TArray<FVector2f> Path;                     // the waypoints of the path it names (MOVE_CAMERA_ALONG_WAYPOINT_PATH)
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

/** A team of a map: an instance of one of the map's team definitions, in the game's team registry (GenTeams.h) —
 *  the same registry a computer player's AI builds its teams in, so that a team the AI builds is the one a script names. */
using FGenMapTeam = FGenTeam;

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
	/** (A test step: this script runs from now on.) */
	void TestEnableScript(const FString& Name) { if (FGenScript* S = FindScript(Name)) { S->bActive = true; } }
	/** (Tests.) That script's actions, whatever its conditions say. */
	void TestRunScript(const FString& Name) { if (FGenScript* S = FindScript(Name)) { ExecuteActions(S->Actions); } }
	/** PLAYER_EXCLUDE_FROM_SCORE_SCREEN */
	bool IsExcludedFromScore(int32 Player) const { return ExcludedFromScore.Contains(Player); }
	/** COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE: buttons taken out of a command set (lower-case set name -> slots). */
	bool IsButtonRemoved(const FString& CommandSet, int32 Slot) const;
	/** COMMANDBAR_ADD_BUTTON_OBJECTTYPE_SLOT: the button a script put in that slot (GameLogic::setControlBarOverride), if any. */
	const FString* GetButtonAdded(const FString& CommandSet, int32 Slot) const;
	/** NAMED_HIDE_SPECIAL_POWER_DISPLAY: the object's superweapon timers aren't shown. */
	bool IsPowerDisplayHidden(FGenObjectID Object) const { return bPowerDisplaysHidden || HiddenPowerDisplays.Contains(Object); }
	/** DISPLAY_COUNTER: this named timer shows a count, not a time left. */
	bool IsNamedCounter(const FString& Name) const { return NamedCounters.Contains(Name); }
	/** CAMEO_FLASH: a script is making this button flash. */
	bool IsCameoFlashing(const FString& Button) const { const uint32* Until = CameoFlashes.Find(Button); return Until && Frame < *Until; }
	/** ScriptEngine::adjustToppleDirection: the way NAMED_SET_TOPPLE_DIRECTION wants this one to fall. */
	bool GetToppleDirection(const FGenObject& Obj, FVector2f& Out) const;
	/** DISPLAY_COUNTDOWN_TIMER: the timers the player is shown, name -> text key. */
	const TMap<FString, FString>& GetNamedTimers() const { return NamedTimers; }
	bool AreNamedTimersShown() const { return bNamedTimersShown; }
	/** What a timer stands at, in frames. */
	int32 GetCounterValue(const FString& Name) const { const FCounter* C = Counters.Find(Name); return C ? C->Value : 0; }
	/** ScriptEngine::notifyOfTriggeredSpecialPower: a player has fired a power (PLAYER_TRIGGERED_SPECIAL_POWER). */
	void NoteSpecialPowerTriggered(int32 Player, const FString& Power) { TriggeredPowers.FindOrAdd(Player).Add(Power); }
	/** ScriptEngine::notifyOfCompletedSpecialPower: what a power made has died (SpecialPowerCompletionDie). */
	void NoteSpecialPowerCompleted(int32 Player, const FString& Power) { CompletedPowers.FindOrAdd(Player).Add(Power); }
	/** What the local player has selected, as an order brought it (NAMED_SELECTED). */
	void SetSelectedForScripts(const TArray<FGenObjectID>& Units) { SelectedUnits = Units; }
	/** PLAYER_SET_RANKLEVELLIMIT (0: none). */
	int32 GetRankLevelLimit() const { return RankLevelLimit; }
	/** PLAYER_SCIENCE_AVAILABILITY: "Hidden" (not in the menu) / "Disabled" (there, greyed) sciences of a player. */
	bool IsScienceHidden(int32 Player, const FString& Science) const;
	bool IsScienceDisabled(int32 Player, const FString& Science) const;
	/** SCRIPTING_OVERRIDE_HULK_LIFETIME in frames (-1: none). */
	int32 GetHulkLifetimeOverride() const { return HulkLifetimeOverride; }
	/** OPTIONS_SET_OCCLUSION_MODE: what stands behind a building shows there. */
	bool ShowsBehindBuildingMarkers() const { return bShowBehindMarkers; }
	/** OBJECT_ALLOW_BONUSES: what is made gets its difficulty's bonuses (ScriptEngine::getObjectsShouldReceiveDifficultyBonus). */
	bool ObjectsReceiveDifficultyBonus() const { return bObjectsReceiveBonus; }
	/** OPTIONS_SET_DRAWICON_UI_MODE: health bars and icons are drawn. */
	bool ShouldDrawIconUI() const { return bDrawIconUI; }
	/** RADAR_FORCE_ENABLE / RADAR_UNFORCE_ENABLE: the radar is on whatever the player has. */
	bool IsRadarForced() const { return bRadarForced; }
	/** TeamPrototype::evaluateProductionCondition: in a mission the map's engine owns the scripts, so a computer
	 *  side's AI asks here. Evaluated as the definition's own player, not as the script that runs. */
	bool EvaluateProductionCondition(int32 ProtoIndex);
	/** ScriptEngine::notifyOfObjectCreationOrDestruction: the frame something was created or destroyed, so that the
	 *  conditions that keep an answer cached know when it could have changed. */
	void NoteObjectChange() { FrameObjectCountChanged = Frame; }

	/** What the game thread is to show or play: taken inside the logic gate. */
	void TakeClientEvents(TArray<FGenScriptClientEvent>& Out) { Out.Append(MoveTemp(ClientEvents)); ClientEvents.Reset(); }

	/** For traces and tests: a line about where the scripts stand. */
	FString Describe() const;
	/** The object a script knows by that name (null if there is none, or it is gone). */
	FGenObject* GetUnitNamed(const FString& Name) const;
	/** The team an object is on (its owner's default team if no other). */
	int32 GetObjectTeam(const FGenObject& Obj) const;
	/** ScriptEngine::transferObjectName: the name of one object goes to another (a terrorist's to the car bomb it made). */
	void TransferName(const FGenObject& From, FGenObject& To);
	/** The camera's scripted movement is under way until this frame (CAMERA_MOVEMENT_FINISHED). */
	void NoteCameraBusyUntil(uint32 InFrame) { CameraBusyUntil = FMath::Max(CameraBusyUntil, InFrame); }
	/** Team::notifyTeamOfObjectDeath: the game has destroyed one of a team's members. */
	void NotifyTeamOfObjectDeath(FGenObject& Obj);

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
	/** ScriptEngine::appendSequentialScript: a sequential script goes behind the ones already waiting for the same
	 *  team or the same unit, with its instruction pointer to the beginning. */
	void AppendSequential(int32 TeamId, FGenObjectID Object, const FGenScript* Script, int32 TimesToLoop);
	/** ScriptEngine::setSequentialTimer: the sequence running on this team waits that many frames. */
	void SetSequentialTimer(int32 TeamId, int32 Frames);
	/** evaluateTeamIsContained (all): anything of the team still inside something. */
	bool TeamAnyContained(const FGenMapTeam& Team) const;
	/** Team::killTeam: containers empty, then everything on it dies - tech buildings just go neutral. */
	void KillTeam(FGenMapTeam& Team);
	/** ScriptEngine::runScript: a team's own script (on-create, on-idle, a team member died), run as that team,
	 *  with its player as <This Player>. The team is given by id: an action can disband it. */
	void RunTeamScript(int32 TeamId, const FString& Name);
	void Client(const FGenScriptAction& A, const FString* Waypoint = nullptr, const FString* Waypoint2 = nullptr);

	// --- names: players, teams, objects, places ---
	int32 GetPlayerNamed(const FString& Name) const;
	FGenMapTeam* GetTeamNamed(const FString& Name);   // <This Team> first, then the registry
	FGenMapTeam* FindTeam(int32 Id) const { return TF.FindTeam(Id); }
	FGenMapTeam& CreateTeam(int32 ProtoIndex, int32 Player) { return TF.CreateTeam(ProtoIndex, Player); }
	FGenMapTeam* GetOrCreateTeamNamed(const FString& Name);
	int32 FindProto(const FString& Name) const { return TF.FindProto(Name); }
	void SetObjectTeam(FGenObject& Obj, FGenMapTeam& Team) { TF.SetObjectTeam(Obj, Team); }
	void GetTeamMembers(const FGenMapTeam& Team, TArray<FGenObject*>& Out, bool bIncludeDead = false) const { TF.GetTeamMembers(Team, Out, bIncludeDead); }
	void DeleteTeamMembers(FGenMapTeam& Team);
	bool GetTeamCenter(const FGenMapTeam& Team, FVector2f& Out) const { return TF.GetTeamCenter(Team, Out); }
	void NameObject(FGenObject& Obj, const FString& Name);
	const FGenPolygonTrigger* GetTrigger(const FString& Name) const;
	bool GetWaypoint(const FString& Name, FVector3f& Out) const;
	const FGenMapObject* GetClosestWaypointOnPath(const FVector2f& From, const FString& Label) const;
	/** FGenMapData::GetWaypointPath: the path, and where it comes back to (-1: it ends). */
	int32 GetWaypointPath(const FGenMapObject& Start, TArray<FVector2f>& Out) const;
	const TArray<const FGenThingTemplate*>& ResolveTypes(const FString& TypeOrList) const;

	// --- conditions ---
	bool PlayerUnitComparison(const FGenScriptCondition& C, int32 Player, int32 Comparison, int32 Count, const FString& Types) const;
	int32 CountInArea(int32 Player, const FGenPolygonTrigger& Area, TFunctionRef<bool(const FGenObject&)> Wanted) const;
	bool NamedEnteredArea(const FGenScriptCondition& C, const FString& Unit, const FString& Area) const;
	/** SKIRMISH_PLAYER_HAS_UNITS_IN_AREA / SKIRMISH_PLAYER_IS_OUTSIDE_AREA, which share the answer the condition
	 *  keeps and only work it out again when a team came in or out, or something was created or destroyed. */
	bool PlayerUnitsInArea(const FGenScriptCondition& C, int32 Player, const FGenPolygonTrigger& Area) const;
	bool AttackedByPlayer(const FGenObject& Victim, int32 Player) const;
	/** AIGroup::isIdle: every member that can move is standing idle. */
	bool TeamIsIdle(const FGenMapTeam& Team) const;
	bool IsSoundComplete(const FString& Event, TMap<FString, uint32>& Pending) const;
	bool TeamInside(const FGenMapTeam& Team, const FGenPolygonTrigger& Area, uint32 Surfaces, bool bAll) const;
	bool HasObjectStatus(const FGenObject& Obj, const FString& Status) const;
	/** Team::didPartialExit: a member that was inside the area has come out of it. */
	bool TeamMemberExited(const FGenScriptCondition& C, const FGenMapTeam& Team, const FGenPolygonTrigger& Area, uint32 Surfaces);
	/** evaluateSkirmishCommandButtonIsReady (all): every member with that power has it ready. */
	bool TeamCommandButtonReady(const FGenMapTeam& Team, const FString& ButtonName) const;
	bool ObjectHasButton(const FGenObject& Obj, const FString& ButtonName) const;
	/** Player::countObjectsByThingTemplate */
	int32 CountOfTypes(int32 Player, const FString& Types) const;
	/** AIUpdateInterface::getCompletedWaypoint: who got to the end of a path they were sent along, this frame. */
	void UpdateCompletedRoutes();
	/** TerrainLogic::update: the water tables a script is raising or lowering. */
	void UpdateWaterMoves();

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

	// The game's team registry, shared with the computer players' AI (TeamFactory).
	FGenTeamFactory& TF;
	TArray<FGenTeamProto>& Protos = TF.Protos;
	TArray<int32>& ProtoPlayers = TF.ProtoPlayers;  // the player each definition's teams belong to
	TArray<TUniquePtr<FGenTeam>>& Teams = TF.Teams; // pointers stay valid as the list grows
	TMap<int32, int32>& DefaultTeams = TF.DefaultTeams; // player -> its default team's id
	TMap<FGenObjectID, int32>& ObjectTeams = TF.ObjectTeams; // objects on a team that isn't their owner's default one
	int32& NextTeamId = TF.NextTeamId;

	TMap<FString, FGenObjectID> Named;              // lower-case name -> object
	TSet<FString> NamesThatExisted;                 // ScriptEngine::didUnitExist
	TMap<FString, TArray<FString>> ObjectTypeLists;
	mutable TMap<FString, TArray<const FGenThingTemplate*>> ResolvedTypes;

	/** ScriptEngine::SequentialScript: the actions of one script, run one at a time, each waiting for the team it
	 *  runs on - or for the unit it names - to stand idle. Nothing is ever set to wait for here except by a WAIT
	 *  action, so in practice -1 means "when the team or the unit is idle". */
	struct FSequential
	{
		int32 Team = 0;                       // 0: this one runs on a unit
		FGenObjectID Object = GenInvalidObjectID;   // nothing: it runs on the team
		const FGenScript* Script = nullptr;
		int32 Instruction = -1;
		int32 FramesToWait = -1;
		int32 TimesToLoop = 0;                // the count the action gave; -1 for ever
		int32 Uid = 0;                        // (an action can add or take away entries while it runs)
	};
	TArray<FSequential> SequentialScripts;
	struct FRouteWatch { FGenObjectID Object = GenInvalidObjectID; FString Label; FVector2f End = FVector2f::ZeroVector; };
	TArray<FRouteWatch> RouteWatches;               // who is on the way along a path that has an end
	TMap<FGenObjectID, FString> CompletedRoutes;    // the path it finished this frame
	/** What was inside the area when TEAM_EXITED_AREA_PARTIALLY last asked, for each team of the condition. */
	TMap<TPair<const FGenScriptCondition*, int32>, TSet<FGenObjectID>> AreaMembers;
	TMap<FString, int32> LostObjectCounts;          // ScriptEngine::getObjectCount: "player|types" -> how many there were
	TMap<FString, FVector2f> ToppleDirections;      // ScriptEngine::m_toppleDirections, by object name
	TSet<FString> NamedCounters;
	TMap<FString, uint32> CameoFlashes;             // button -> the frame it stops flashing
	bool bPowerDisplaysHidden = false;              // InGameUI::setSuperweaponDisplayEnabledByScript
	struct FWaterMove { FString Name; float From = 0.f; float To = 0.f; uint32 Start = 0; int32 Frames = 1; };
	TArray<FWaterMove> WaterMoves;
	TMap<FGenObjectID, int32> Flashing;             // NAMED_FLASH_WHITE: flashes still to come

	// ScriptEngine's own state
	FGenScriptFade Fade;
	int32 FadeFrame = 0, FadeIncrease = 0, FadeHold = 0, FadeDecrease = 0;
	float FadeMin = 1.f, FadeMax = 0.f;
	bool bTimeFrozen = false;
	int32 FpsLimit = 0;
	bool bInputDisabled = false;
	bool bDrawIconUI = true;
	bool bShowBehindMarkers = true;
	bool bObjectsReceiveBonus = true;
	bool bEnded = false;
	FGenObjectID ThisObject = GenInvalidObjectID;   // <This Object>: the unit the sequence runs on
	int32 NextSequentialId = 1;
	struct FNamedReveal { int32 Player = INDEX_NONE; FVector2f Pos = FVector2f::ZeroVector; float Radius = 0.f; };
	TMap<FString, FNamedReveal> NamedReveals;       // ScriptEngine::createNamedMapReveal
	/** SpecialPowerModule::pauseCountdown: what is left stays what is left. */
	struct FPausedPower { FGenObjectID Object = GenInvalidObjectID; int32 Module = 0; uint32 Left = 0; };
	TArray<FPausedPower> PausedPowers;
	TSet<FGenObjectID> HiddenPowerDisplays;
	TMap<FString, FString> NamedTimers;
	bool bNamedTimersShown = true;
	TMap<int32, TArray<FString>> TriggeredPowers;   // player -> powers fired, until a condition has seen them
	bool bThePlayerIsLocal = false;                 // the Challenge: "ThePlayer" and "teamThePlayer" stand for the local player's
	TMap<int32, TArray<FString>> CompletedPowers;   // player -> powers finished, until a condition has seen them
	struct FTransportStatus { uint32 Frame = 0; int32 Count = 0; };
	TMap<FGenObjectID, FTransportStatus> TransportStatuses; // UNIT_EMPTIED: what each was last seen carrying
	TArray<FGenObjectID> SelectedUnits;
	TMap<FString, TMap<int32, FString>> AddedButtons; // lower-case command set -> slot -> button
	TMap<FGenObjectID, FLinearColor> FlashColors;   // NAMED_FLASH: in the owner's colour, not white
	bool bRadarForced = false;                // Radar::m_radarForceOn
	uint32 FrameObjectCountChanged = 0;       // ScriptEngine::m_frameObjectCountChanged: the frame of the last create or destroy
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
	TMap<int32, TSet<FString>> DisabledSciences;
	TArray<FGenScriptClientEvent> ClientEvents;
};
