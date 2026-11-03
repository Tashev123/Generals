#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenMatch.h"
#include "GenGameMode.generated.h"

class AGenMapActor;
struct FGenSetupData;

/** Where the game is between starting the program and leaving a finished game. */
enum class EGenGamePhase : uint8
{
	Menu,       // the menus before a game: the main menu, the skirmish setup, the online screen (EGenMenuScreen)
	Loading,    // "Loading" is on screen; the map is read on the next tick
	Playing,
	Score,      // the score screen, over the game
	NetLobby,   // a network game being set up (or being reached)
	Server,     // this program is a network game's server (-GenServer): no game, no screen
};

/** Which of the menus before a game is up (the phase is Menu for all of them). */
enum class EGenMenuScreen : uint8
{
	Main,       // the main menu: skirmish, online, options, exit
	Skirmish,   // the skirmish setup
	Online,     // host a network game or join one
	Campaign,   // the campaigns, their missions and the difficulty
	Challenge,  // the Generals' Challenge: a general, an opponent and the difficulty
	LoadGame,   // the saved games
};

/**
 * Skirmish game mode: the main menu, the skirmish menu, then the game it sets up, then the score screen and back
 * to the menu (the original's Shell -> MainMenu -> SkirmishGameOptionsMenu -> game -> ScoreScreen).
 * The program starts on the main menu; after a game the menu comes back on the screen it was left from.
 *
 * A run with test switches skips the menu and starts at once: the map of the URL option "?GenMap=Name"
 * or the command line "-GenMap=Name" (default: Tournament Desert), the local player (-GenFaction=,
 * default FactionAmerica) against -GenEnemy= (default FactionChina). -GenNoMenu does the same without
 * any other switch; -GenTestMenu= shows the menu in a test run and works it (from the skirmish setup, or
 * from the main menu if the script has "Main." ids).
 *
 * Test automation (command line):
 *   -GenShot=<file.png>        take a screenshot after the map loads, then quit
 *   -GenShotDelay=<sec>        wait before the screenshot (default 4)
 *   -GenCam=X,Y,Yaw,Height     camera look-at in Generals units, yaw degrees, height
 *   -GenObject=T1,T2           spawn units for the local player at the camera target (or -GenObjectAt=X,Y)
 *   -GenObjectRank=N           those units start at that rank (0 regular ... 3 heroic)
 *   -GenTestMove=X,Y           after 1 s, order all local mobile units to move there
 *   -GenEnemyObject=T1,T2      spawn units for the enemy player (at -GenEnemyAt=X,Y)
 *   -GenTestAttack             after 1 s, order all local armed units to attack the first enemy unit
 *   -GenTestFX=Name[,Name]     after 1 s, play FX lists / particle systems at the camera target
 *   -GenTestBuild=Building     at logic frame 30, the first local dozer constructs it at the camera target
 *   -GenTestQueue=Unit[,Unit]  at logic frame 30, queue units at the local building that produces them
 *   -GenTestSelect=Template    at logic frame 30, select the first local object of that type
 *   -GenTrace                  log unit state once per second
 *   -GenSeed=N                 fixed simulation random seed
 *   -GenPauseFrame=N           freeze the simulation at logic frame N (and take the -GenShot there)
 *   -GenModel=<name>           spawn a bare W3D model (preview)
 *   -GenTestMenu="sec,click,Id;..."  the menu is shown; at that many seconds since the program started the
 *                              interface element with that id is clicked (ids: see GenShell.cpp)
 */
UCLASS()
class GENERALS1_API AGenGameMode : public AGameModeBase
{
	GENERATED_BODY()

	/** -GenRecordAudio: writes what the mixer has put out since the start to that file. */
	void SaveAudioRecording();
	FString RecordAudioPath;
	double RecordStartSeconds = 0.0;

public:
	AGenGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	AGenMapActor* GetMapActor() const { return MapActor; }

	// --- the shell (driven by the interface in GenShell.cpp) ---
	EGenGamePhase GetPhase() const { return Phase; }
	/** The interface has the mouse: the menu, the score screen, or the in-game menu is up. */
	bool IsShellActive() const { return Phase != EGenGamePhase::Playing || bQuitMenuOpen; }
	/** The menu screen shown while the phase is Menu. */
	EGenMenuScreen GetMenuScreen() const { return MenuScreen; }
	void SetMenuScreen(EGenMenuScreen Screen);
	/** Why the game couldn't start, to show in the menu. */
	const FString& GetShellError() const { return ShellError; }
	void ClearShellError() { ShellError.Reset(); }
	/** The menu's "play game": settles what was left to chance and loads the map (or sets the error). */
	void RequestStart();
	/** Esc / F10 during a game: the in-game menu, with the game standing still. */
	void ToggleQuitMenu();
	bool IsQuitMenuOpen() const { return bQuitMenuOpen; }
	/** The pause hotkey: the game stands still with no menu up. A network game can't be stopped - the host
	 *  keeps the clock - so this does nothing there. */
	void TogglePause();
	/** The campaign menu's "start": that mission of that campaign (Difficulty: 0 easy, 1 normal, 2 hard). */
	void RequestCampaignStart(const FString& Campaign, const FString& Mission, int32 Difficulty);
	/** The in-game menu's "restart": the same game again from the beginning. */
	void RestartGame();
	/** The Generals' Challenge: on to that mission of the same campaign, as the same general. */
	void PlayNextMission(const FString& Mission);
	const FString& GetPendingCampaign() const { return PendingCampaign; }
	const FString& GetPendingMission() const { return PendingMission; }
	/** The in-game menu's "save game": the game as it stands, to that file (empty: a new one). */
	bool SaveGame(const FString& Path);
	bool CanSaveGame() const;
	/** "Load game": a fresh start of the level, which plays that saved game. */
	void RequestLoadGame(const FString& Path);
	/** The saved game is being played up to where it was left. */
	bool IsResumingSave() const { return bResumingSave; }
	const FString& GetResumeTitle() const { return ResumeSave.Title; }
	/** The in-game menu's "exit": to the score screen. */
	void EndGame();
	/** The score screen's "continue": back to the menu, on the screen the game was started from. */
	void ExitToMenu();
	/** The score screen after a defeat while others fight on: back to the game, as a spectator. */
	void KeepWatching();
	bool CanKeepWatching() const;
	/** Seconds since the game was decided for the local player (0 while it isn't). */
	float GetSecondsSinceDecided() const { return DecidedSeconds; }

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Put the pause as it stands (the menu is up, or the hotkey is on) onto the logic. */
	void ApplyPause();
	/** Loads the map and starts the game with these players (and the command line's test switches). False (and
	 *  the error set) if that fails. */
	bool BeginGame(const FString& MapName, const TArray<FGenSkirmishSlot>& Slots, int32 StartingCash, bool bFromMenu);
	/** Loads the match's map, starts it and puts the camera over the local player's base. */
	bool BeginMatch(const FGenMatchSettings& Settings, bool bFromMenu, int32 InLocalPlayer = INDEX_NONE);
	/** A campaign mission (GenGameModeCampaign.cpp): its map, its players and its scripts. */
	bool BeginCampaign(const FString& Campaign, const FString& Mission, int32 Difficulty, bool bFromMenu);
	/** During a campaign mission: what its scripts want seen and heard, and the camera and input they hold. */
	void TickCampaign(float DeltaSeconds);
public:
	/** A mission's intro can be skipped: how far Space has been held (0 to 1), below 0 when it can't be now. */
	float GetIntroSkipHold() const { return IntroPhase == 1 ? IntroSkipHeld / 1.f : -1.f; }
	bool IsSkippingIntro() const { return bSkippingIntro; }
private:
	int32 IntroPhase = 0;        // 0: not begun, 1: the scripts have the input for the first time, 2: over
	float IntroSkipHeld = 0.f;   // seconds
	bool bSkippingIntro = false;
	bool bTestHoldSpace = false; // (-TestPowers "frame,holdspace")
	FString SkippedTrack;        // the music the skipped part asked for last
	TArray<FGenScriptClientEvent> ScriptEvents;
	TArray<FGenObjectID> LastScriptSelection;
	FString ScriptCameraFollow;              // CAMERA_FOLLOW_NAMED: who the camera stays over
	FString ScriptCameraLook;                // CAMERA_LOOK_TOWARD_OBJECT: who the camera keeps looking at
	uint32 ScriptCameraLookFrom = 0, ScriptCameraLookUntil = 0;
	/** -GenReplay=file: a recorded game played again from its start with its own orders - to watch, or to check
	 *  that it comes out the same (-GenReplayViewer=N watches it as player N). */
	bool StartReplay(const FString& Path);
	/** Starts the match of a saved game and plays it up to where it was left. */
	bool BeginFromSave(const FString& Path, bool bFromMenu);
	/** ... and there: the view, the selection and the player's groups as they were. */
	void FinishResumeSave();
	bool bResumingSave = false;
	FGenSavedGame ResumeSave;      // (its replay gone to the logic)
	/** Writes the recording of the game so far (Saved/Replays/LastReplay.genrep, or -GenRecord=file; and with
	 *  -GenChecksumLog=file its checksums, a frame to a line). */
	void SaveReplay();
	float ReplaySaveSeconds = 0.f;
	/** A network game: the lobby's news, the start of the match, test automation of the lobby. */
	void TickNetwork(float DeltaSeconds);
	/** -GenNetStart=N (tests): the host sets the lobby up from the command line and starts once N players are in. */
	int32 NetAutoStage = 0;
	float NetAutoSeconds = 0.f;
	bool bNetTeamSent = false;
	bool bMismatchKept = false;

public:
	/** Lines about the network game for the screen, with the time they came. */
	TArray<TPair<double, FString>> NetNotices;
	/** A network game's lobby, edited by the menu: what this player wants (sent to the host). */
	void NetLobbyEdit(const FGenSetupData& Wanted);
	/** The menu's network buttons: host a game here (the server starts in the background), join one by address,
	 *  go back to the last match this game played, leave the lobby. */
	void HostNetworkGame();
	void JoinNetworkGame(const FString& Address);
	void RejoinNetworkGame();
	void LeaveNetworkGame();
	void ReloadLevel();
	void LogScores() const;
	void ParseTestSwitches();
	void RunTestAutomation(float DeltaSeconds);
	/** Places a battle group for the local player in front of its base and one for the enemy beyond it. */
	void SpawnTestUnits(class UGenGameLogic* Logic);

	UPROPERTY(Transient)
	TObjectPtr<AGenMapActor> MapActor;

	EGenGamePhase Phase = EGenGamePhase::Playing;
	EGenMenuScreen MenuScreen = EGenMenuScreen::Main;
	FString ShellError;
	TArray<FGenSkirmishSlot> PendingSlots;   // settled by RequestStart, used when the loading tick comes
	FString PendingCampaign, PendingMission;   // ... or the campaign mission the campaign menu asked for
	int32 PendingDifficulty = 1;
	int32 LoadingTicks = 0;
	bool bStartedFromMenu = false;
	bool bReloading = false;           // the level is on its way out (ReloadLevel)
	bool bQuitMenuOpen = false;
	bool bPauseRequested = false;             // the pause hotkey, on its own (the HUD says it on screen)
	bool bWatching = false;                  // defeated, looking on: no score screen until the game is over
	float DecidedSeconds = 0.f;

	FString ShotPath;
	float ShotDelay = 4.f;
	float Elapsed = 0.f;
	int32 ShotState = 0;
	bool bTestMovePending = false;
	FVector2f TestMoveGoal = FVector2f::ZeroVector;
	uint32 ShotFrame = 0;        // -GenPauseFrame: screenshot at this logic frame
	int32 FramesAtShotFrame = 0;
	bool bTestFXDone = false;
	bool bTestEconomyDone = false;
	bool bTestGatherDone = false;
	int32 TestJetOrdersDone = 0;
	int32 TestPowerOrdersDone = 0;
	bool bTestAttackPending = false;
	uint32 TestAttackTarget = 0; // first -GenEnemyObject unit
};
