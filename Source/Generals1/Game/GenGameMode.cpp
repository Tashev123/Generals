#include "Game/GenGameMode.h"
#include "Game/GenSettings.h"
#include "Algo/Find.h"
#include "Audio/GenAudioManager.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Game/GenRadar.h"
#include "Game/GenHUD.h"
#include "Game/GenPlayerController.h"
#include "Game/GenShellData.h"
#include "Game/GenSkirmishSetup.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Net/GenNetSubsystem.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"

AGenGameMode::AGenGameMode()
{
	DefaultPawnClass = AGenCameraPawn::StaticClass();
	PlayerControllerClass = AGenPlayerController::StaticClass();
	HUDClass = AGenHUD::StaticClass();
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

namespace
{
	/** How long "victory" or "defeat" stands on the screen before the score screen comes. */
	constexpr float ScoreScreenDelay = 4.f;

	/** The menu screen to come back to after a game (the level, and with it the game mode, is loaded anew). */
	EGenMenuScreen LastMenuScreen = EGenMenuScreen::Skirmish;

	FRandomStream MakeSetupRandom()
	{
		int32 Seed = int32(FPlatformTime::Cycles64() & 0x7fffffff);
		FParse::Value(FCommandLine::Get(), TEXT("GenSeed="), Seed);
		return FRandomStream(Seed);
	}
}

void AGenGameMode::StartPlay()
{
	// (Who is where, and who hits what, is the game logic's own business: the engine's physics has
	// nothing to step, and stepping nothing still takes its share of every frame.)
	if (UWorld* World = GetWorld())
	{
		World->bShouldSimulatePhysics = false;
	}
	Super::StartPlay();
	// The player's settings (resolution and the like are the engine's, put into effect as it started).
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->ApplyAtStartup();
	}

	// The server of a network game (-GenServer, started by the host's game): no game of its own here, no
	// screen; it runs until its match is over and the players have gone.
	if (FParse::Param(FCommandLine::Get(), TEXT("GenServer")))
	{
		Phase = EGenGamePhase::Server;
		int32 Port = GenNet::DefaultPort;
		FParse::Value(FCommandLine::Get(), TEXT("GenPort="), Port);
		FString Why;
		UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
		if (!Net || !Net->StartServerHere(Port, &Why))
		{
			UE_LOG(LogGenerals, Error, TEXT("Server: can't start: %s"), *Why);
			FPlatformMisc::RequestExit(false, TEXT("GenServer failed"));
		}
		if (IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
		{
			MaxFPS->Set(60, ECVF_SetByCode); // (nothing is drawn: no frames to hurry)
		}
		return;
	}

	// Reuse a map actor placed in the level (editor preview), otherwise spawn one.
	for (TActorIterator<AGenMapActor> It(GetWorld()); It; ++It)
	{
		MapActor = *It;
		break;
	}
	if (!MapActor)
	{
		MapActor = GetWorld()->SpawnActor<AGenMapActor>(AGenMapActor::StaticClass(), FTransform::Identity);
	}
	MapActor->bSpawnMapObjects = false; // the simulation creates the map's objects
	ParseTestSwitches();

	// A replay: the recorded game again, nothing else.
	FString ReplayPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenReplay="), ReplayPath))
	{
		StartReplay(ReplayPath);
		return;
	}

	// A network game: -GenHost[=port] starts the server and joins it, -GenJoin=host[:port] joins one,
	// -GenRejoin goes back to the last match this game played (as the same player). Once per run: after a
	// match the level is loaded again and the connection is already there (or has been left).
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	static bool bNetSwitchesTaken = false;
	if (Net && !bNetSwitchesTaken)
	{
		bNetSwitchesTaken = true;
		const TCHAR* NetCmd = FCommandLine::Get();
		UGenSkirmishSetup* NameSetup = UGenSkirmishSetup::Get(this);
		if (NameSetup)
		{
			NameSetup->EnsureLoaded();
		}
		FString PlayerName = NameSetup ? NameSetup->PlayerName : FString(TEXT("Player"));
		if (FParse::Value(NetCmd, TEXT("GenName="), PlayerName) && NameSetup)
		{
			NameSetup->PlayerName = PlayerName; // (the lobby shows the name the host knows)
		}
		FString Address, PortText;
		uint64 Seat = 0;
		if (FParse::Value(NetCmd, TEXT("GenJoin="), Address))
		{
			Net->Join(Address, PlayerName);
		}
		else if (FParse::Param(NetCmd, TEXT("GenHost")) || FParse::Value(NetCmd, TEXT("GenHost="), PortText))
		{
			FString Why;
			const int32 Port = PortText.IsNumeric() ? FCString::Atoi(*PortText) : GenNet::DefaultPort;
			if (!Net->Host(PlayerName, Port, &Why))
			{
				ShellError = Why;
			}
		}
		else if (FParse::Param(NetCmd, TEXT("GenRejoin")) && Net->RecallSeat(Address, PlayerName, Seat))
		{
			Net->Join(Address, PlayerName, Seat);
		}
	}
	if (Net && Net->IsInNetworkGame())
	{
		Phase = EGenGamePhase::NetLobby;
		if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
		{
			Audio->StartShell(MapActor);
		}
		return;
	}

	// A run with test switches goes straight into a game; otherwise (or with -GenTestMenu) the menu first.
	FString MapName = UGameplayStatics::ParseOption(OptionsString, TEXT("GenMap"));
	FString Ignored;
	const TCHAR* Cmd = FCommandLine::Get();
	const bool bDirect = !MapName.IsEmpty() || FParse::Value(Cmd, TEXT("GenMap="), MapName) || !ShotPath.IsEmpty() || FParse::Param(Cmd, TEXT("GenNoMenu"))
		|| FParse::Value(Cmd, TEXT("GenFaction="), Ignored) || FParse::Value(Cmd, TEXT("GenEnemy="), Ignored)
		|| FParse::Value(Cmd, TEXT("GenObject="), Ignored) || FParse::Value(Cmd, TEXT("GenModel="), Ignored);
	const bool bMenuTest = FParse::Value(Cmd, TEXT("GenTestMenu="), Ignored, false);
	UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	if (Setup && (bMenuTest || !bDirect))
	{
		Setup->EnsureLoaded();
		if (Setup->bStartAtOnce)
		{
			// "Restart": the same game again.
			Setup->bStartAtOnce = false;
			FRandomStream Random = MakeSetupRandom();
			TArray<FGenSkirmishSlot> Slots;
			ShellError = Setup->Resolve(Random, Slots);
			if (ShellError.IsEmpty() && BeginGame(Setup->Map, Slots, Setup->StartingCash, true))
			{
				return;
			}
		}
		Phase = EGenGamePhase::Menu;
		// The program starts on the main menu (a test script only if it works the main menu: "Main." ids);
		// after a game the menu comes back where it was left - the skirmish setup or the online screen.
		static bool bMenuShown = false;
		if (!bMenuShown)
		{
			bMenuShown = true;
			FString Spec;
			FParse::Value(Cmd, TEXT("GenTestMenu="), Spec, false);
			SetMenuScreen(bMenuTest && !Spec.Contains(TEXT("Main."), ESearchCase::IgnoreCase) ? EGenMenuScreen::Skirmish : EGenMenuScreen::Main);
		}
		else
		{
			SetMenuScreen(LastMenuScreen);
		}
		if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
		{
			Audio->StartShell(MapActor);
		}
		return;
	}

	// A test run: the local player against one opponent.
	if (MapName.IsEmpty())
	{
		MapName = TEXT("Tournament Desert");
	}
	FString LocalFaction = TEXT("FactionAmerica");
	FString EnemyFaction = TEXT("FactionChina");
	FParse::Value(Cmd, TEXT("GenFaction="), LocalFaction);
	FParse::Value(Cmd, TEXT("GenEnemy="), EnemyFaction);
	TArray<FGenSkirmishSlot> Slots;
	FGenSkirmishSlot& Me = Slots.AddDefaulted_GetRef();
	Me.Faction = LocalFaction;
	Me.bHuman = true;
	Me.StartPosition = 0;
	FGenSkirmishSlot& Enemy = Slots.AddDefaulted_GetRef();
	Enemy.Faction = EnemyFaction;
	Enemy.StartPosition = 1;
	// -GenBots=N: that many computer players in all (the first is the one above), sides in turn.
	int32 Bots = 1;
	FParse::Value(Cmd, TEXT("GenBots="), Bots);
	static const TCHAR* BotSides[] = { TEXT("FactionAmerica"), TEXT("FactionGLA"), TEXT("FactionChina") };
	for (int32 b = 1; b < FMath::Clamp(Bots, 1, 7); ++b)
	{
		FGenSkirmishSlot& Bot = Slots.AddDefaulted_GetRef();
		Bot.Faction = BotSides[(b - 1) % 3];
		Bot.StartPosition = 1 + b;
	}
	BeginGame(MapName, Slots, 10000, false);
}

void AGenGameMode::ParseTestSwitches()
{
	FParse::Value(FCommandLine::Get(), TEXT("GenShot="), ShotPath);
	FParse::Value(FCommandLine::Get(), TEXT("GenShotDelay="), ShotDelay);
	FParse::Value(FCommandLine::Get(), TEXT("GenPauseFrame="), ShotFrame);
	if (!ShotPath.IsEmpty())
	{
		// (An unattended run: the real mouse must not scroll the camera or click anything.)
		if (AGenPlayerController* GenPC = Cast<AGenPlayerController>(GetWorld()->GetFirstPlayerController()))
		{
			GenPC->SetGenInputEnabled(false);
		}
	}
	// -GenRecordAudio=file.wav records what the mixer puts out.
	if (FParse::Value(FCommandLine::Get(), TEXT("GenRecordAudio="), RecordAudioPath))
	{
		if (Audio::FMixerDevice* Mixer = FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(this))
		{
			// A silent submix stops being mixed (and recorded): the recording must keep its silences.
			if (IConsoleVariable* NeverDisable = IConsoleManager::Get().FindConsoleVariable(TEXT("au.NeverDisableSubmixes")))
			{
				NeverDisable->Set(1);
			}
			Mixer->StartRecording(nullptr, 0.f);
			RecordStartSeconds = GetWorld()->GetRealTimeSeconds();
			UE_LOG(LogGenerals, Log, TEXT("Audio recording started (%d Hz)"), int32(Mixer->GetSampleRate()));
		}
		else
		{
			UE_LOG(LogGenerals, Warning, TEXT("Audio recording: no audio mixer device"));
			RecordAudioPath.Reset();
		}
	}
}

bool AGenGameMode::BeginMatch(const FGenMatchSettings& Settings, bool bFromMenu, int32 InLocalPlayer)
{
	bStartedFromMenu = bFromMenu;
	if (!MapActor->LoadMapByName(Settings.Map))
	{
		UE_LOG(LogGenerals, Error, TEXT("Failed to load map '%s'"), *Settings.Map);
		ShellError = FString::Printf(TEXT("The map '%s' can't be loaded"), *Settings.Map);
		return false;
	}
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !Logic->StartSkirmish(MapActor, Settings, InLocalPlayer))
	{
		ShellError = TEXT("The game can't be started on this map");
		return false;
	}
	Phase = EGenGamePhase::Playing;
	bQuitMenuOpen = false;
	bPauseRequested = false;
	bWatching = false;
	DecidedSeconds = 0.f;

	// Camera start: local player's start waypoint.
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Cam)
	{
		return true;
	}
	const int32 LocalStart = Logic->GetPlayers().IsValidIndex(Logic->GetLocalPlayer()) ? Logic->GetPlayers()[Logic->GetLocalPlayer()].StartPosition : 0;
	FVector Start;
	if (!MapActor->GetWaypointLocation(FString::Printf(TEXT("Player_%d_Start"), LocalStart + 1), Start))
	{
		Start = MapActor->GetPlayableBoundsUE().GetCenter();
	}
	FString CamSpec;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenCam="), CamSpec, false))
	{
		TArray<FString> Parts;
		CamSpec.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 2)
		{
			Start = GenCoords::ToUE(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), 0.f);
		}
		if (Parts.Num() >= 4)
		{
			Cam->CameraHeight = FCString::Atof(*Parts[3]);
			// ResetView goes back to the height a match starts at, so the one asked for has to be set after it.
			Cam->ResetView();
			Cam->SetHeight(FCString::Atof(*Parts[3]));
		}
		if (Parts.Num() >= 3)
		{
			Cam->AddYaw(FCString::Atof(*Parts[2]));
		}
	}
	Cam->SetTarget(Start);
	return true;
}

bool AGenGameMode::BeginGame(const FString& MapName, const TArray<FGenSkirmishSlot>& Slots, int32 StartingCash, bool bFromMenu)
{
	FGenMatchSettings Settings = FGenMatchSettings::FromCommandLine();
	Settings.Map = MapName;
	Settings.Slots = Slots;
	Settings.StartingCash = StartingCash;
	if (!BeginMatch(Settings, bFromMenu))
	{
		return false;
	}
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Cam)
	{
		return true;
	}

	// -GenObject=Template1,Template2,... spawns local player units in a row in front of the camera.
	FString ObjectList;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenObject="), ObjectList, false))
	{
		TArray<FString> Names;
		ObjectList.ParseIntoArray(Names, TEXT(","));
		FVector Gen = GenCoords::FromUE(Cam->GetTarget());
		FString AtSpec, AX, AY;
		if (FParse::Value(FCommandLine::Get(), TEXT("GenObjectAt="), AtSpec, false) && AtSpec.Split(TEXT(","), &AX, &AY))
		{
			Gen = FVector(FCString::Atof(*AX), FCString::Atof(*AY), 0.0);
		}
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			// "Template*N": a block of N of them (a crowd to measure with), twelve to a row.
			// "Template@x:y" or "Template@x:y:angle" (after the count, if any): that one stands there rather than in the row.
			FString Entry = Names[i], PlaceText, PlaceX, PlaceY, PlaceAngle;
			const bool bPlaced = Names[i].Split(TEXT("@"), &Entry, &PlaceText) && PlaceText.Split(TEXT(":"), &PlaceX, &PlaceY);
			float SingleAngle = -90.f;
			if (bPlaced && PlaceY.Split(TEXT(":"), &PlaceY, &PlaceAngle))
			{
				SingleAngle = FCString::Atof(*PlaceAngle);
			}
			FString Name = Entry, CountText;
			int32 Count = 1;
			if (Entry.Split(TEXT("*"), &Name, &CountText))
			{
				Count = FMath::Clamp(FCString::Atoi(*CountText), 1, 2000);
			}
			// "neutral:Template": nobody's (a civilian building to occupy).
			// -GenObjectAngle=deg / -GenObjectSpacing=d: how the objects of a block face and how far apart they stand (a fence in a row).
			float BlockAngle = -90.f, BlockSpacing = 0.f;
			FParse::Value(FCommandLine::Get(), TEXT("GenObjectAngle="), BlockAngle);
			FParse::Value(FCommandLine::Get(), TEXT("GenObjectSpacing="), BlockSpacing);
			const int32 Whose = Name.RemoveFromStart(TEXT("neutral:")) ? 0 : Logic->GetLocalPlayer();
			if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Name))
			{
				const float X = bPlaced ? FCString::Atof(*PlaceX) : float(Gen.X) + (float(i) - (Names.Num() - 1) * 0.5f) * 40.f;
				const float Y = bPlaced ? FCString::Atof(*PlaceY) : float(Gen.Y);
				if (Count == 1)
				{
					Logic->CreateObject(T, Whose, FVector3f(X, Y, Logic->GetGroundHeight(X, Y)), FMath::DegreesToRadians(SingleAngle));
				}
				const float Spacing = BlockSpacing > 0.f ? BlockSpacing : FMath::Max(14.f, T->GetBoundingCircleRadius() * 2.5f);
				for (int32 n = 0; n < Count && Count > 1; ++n)
				{
					const float BX = X + (float(n % 12) - 5.5f) * Spacing, BY = Y + (float(n / 12) - float((Count - 1) / 12) * 0.5f) * Spacing;
					Logic->CreateObject(T, Whose, FVector3f(BX, BY, Logic->GetGroundHeight(BX, BY)), FMath::DegreesToRadians(BlockAngle));
				}
			}
			else
			{
				UE_LOG(LogGenerals, Warning, TEXT("GenObject: template '%s' not found"), *Names[i]);
			}
		}
	}

	// -GenEnemyObject=T1,T2,... spawns units for the first enemy player at -GenEnemyAt=X,Y
	// (default: 120 units north of the camera target), facing south.
	FString EnemyList;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenEnemyObject="), EnemyList, false))
	{
		int32 EnemyPlayer = INDEX_NONE;
		for (const FGenPlayer& P : Logic->GetPlayers())
		{
			if (Logic->GetRelationship(Logic->GetLocalPlayer(), P.Index) == EGenRelationship::Enemies)
			{
				EnemyPlayer = P.Index;
				break;
			}
		}
		const FVector Gen = GenCoords::FromUE(Cam->GetTarget());
		FVector2f At(float(Gen.X), float(Gen.Y) + 120.f);
		FString AtSpec, AX, AY;
		if (FParse::Value(FCommandLine::Get(), TEXT("GenEnemyAt="), AtSpec, false) && AtSpec.Split(TEXT(","), &AX, &AY))
		{
			At = FVector2f(FCString::Atof(*AX), FCString::Atof(*AY));
		}
		TArray<FString> Names;
		EnemyList.ParseIntoArray(Names, TEXT(","));
		for (int32 i = 0; i < Names.Num() && EnemyPlayer != INDEX_NONE; ++i)
		{
			// ("Template@x:y" puts that one at its own spot, as -GenObject does.)
			FString Entry = Names[i], PlaceText, PlaceX, PlaceY;
			const bool bPlaced = Names[i].Split(TEXT("@"), &Entry, &PlaceText) && PlaceText.Split(TEXT(":"), &PlaceX, &PlaceY);
			if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(bPlaced ? Entry : Names[i]))
			{
				const float X = bPlaced ? FCString::Atof(*PlaceX) : At.X + (float(i) - (Names.Num() - 1) * 0.5f) * 40.f;
				const float Y = bPlaced ? FCString::Atof(*PlaceY) : At.Y;
				if (const FGenObject* O = Logic->CreateObject(T, EnemyPlayer, FVector3f(X, Y, Logic->GetGroundHeight(X, Y)), FMath::DegreesToRadians(-90.f)))
				{
					if (TestAttackTarget == GenInvalidObjectID) { TestAttackTarget = O->GetID(); }
				}
			}
			else
			{
				UE_LOG(LogGenerals, Warning, TEXT("GenEnemyObject: template '%s' not found"), *Names[i]);
			}
		}
	}
	bTestAttackPending = FParse::Param(FCommandLine::Get(), TEXT("GenTestAttack")) && TestAttackTarget != GenInvalidObjectID;

	// The human starts with a battle group to play with (and so does a passive -GenNoAI enemy).
	// Scripted test runs (which place their own units or only take a screenshot) and -GenNoTestUnits skip it.
	FString Scripted;
	const bool bScriptedRun = !ObjectList.IsEmpty() || !EnemyList.IsEmpty()
		|| FParse::Value(FCommandLine::Get(), TEXT("GenShot="), Scripted) || FParse::Value(FCommandLine::Get(), TEXT("GenTestFX="), Scripted)
		|| FParse::Value(FCommandLine::Get(), TEXT("GenModel="), Scripted) || FParse::Param(FCommandLine::Get(), TEXT("GenNoTestUnits"));
	// (A game set up in the menu has its own switch for them.)
	const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	if (bFromMenu ? (Setup && Setup->bTestUnits) : (!bScriptedRun || FParse::Param(FCommandLine::Get(), TEXT("GenTestUnits"))))
	{
		SpawnTestUnits(Logic);
	}

	FString ModelName;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenModel="), ModelName))
	{
		UGenModelComponent* MC = NewObject<UGenModelComponent>(MapActor, NAME_None, RF_Transient);
		MC->SetupAttachment(MapActor->GetRootComponent());
		MC->SetWorldLocation(Cam->GetTarget());
		MC->RegisterComponent();
		MC->SetModel(ModelName);
		FString Anim;
		if (FParse::Value(FCommandLine::Get(), TEXT("GenAnim="), Anim))
		{
			MC->PlayAnimation(Anim, EGenAnimMode::Loop);
		}
	}

	FString MoveSpec;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenTestMove="), MoveSpec, false))
	{
		FString X, Y;
		if (MoveSpec.Split(TEXT(","), &X, &Y))
		{
			TestMoveGoal = FVector2f(FCString::Atof(*X), FCString::Atof(*Y));
			bTestMovePending = true;
		}
	}

	// Audio tests: -GenAudioSelfTest decodes every sound the game data names; -GenDumpWav=a,b writes sounds
	// out decoded (to Saved/AudioTest).
	if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("GenAudioSelfTest")))
		{
			int32 Failed = 0;
			double Seconds = 0.0;
			Audio->SelfTest(Failed, Seconds);
		}
		FString DumpList;
		if (FParse::Value(FCommandLine::Get(), TEXT("GenDumpWav="), DumpList, false))
		{
			TArray<FString> Names;
			DumpList.ParseIntoArray(Names, TEXT(","));
			for (const FString& Name : Names)
			{
				const FString Out = FPaths::ProjectSavedDir() / TEXT("AudioTest") / (FPaths::GetBaseFilename(Name) + TEXT(".wav"));
				UE_LOG(LogGenerals, Log, TEXT("Audio dump: %s -> %s: %s"), *Name, *Out, Audio->DumpDecoded(Name, Out) ? TEXT("ok") : TEXT("FAILED"));
			}
		}
	}
	return true;
}

// ---- replays --------------------------------------------------------------------------------------

bool AGenGameMode::StartReplay(const FString& InPath)
{
	FString Path = InPath;
	if (FPaths::IsRelative(Path) && !FPaths::FileExists(Path))
	{
		Path = FGenReplay::GetDefaultFolder() / Path;
	}
	TUniquePtr<FGenReplay> Replay = MakeUnique<FGenReplay>();
	FString Why;
	if (!Replay->Load(Path, &Why))
	{
		UE_LOG(LogGenerals, Error, TEXT("Replay '%s': %s"), *Path, *Why);
		ShellError = FString::Printf(TEXT("The replay can't be played: %s"), *Why);
		return false;
	}
	UE_LOG(LogGenerals, Log, TEXT("Replay %s: recorded %s by player %d; %d orders, %d frames%s"), *Path, *Replay->Date, Replay->RecordedBy,
		Replay->Commands.Num(), Replay->Checksums.Num(), Replay->SetupObjects.Num() > 0 ? *FString::Printf(TEXT(", %d objects at the start"), Replay->SetupObjects.Num()) : TEXT(""));
	if (Replay->BuildId != GenBuild::GetBuildId())
	{
		UE_LOG(LogGenerals, Warning, TEXT("Replay: made by another build of the game (%s; this one is %s) - it may come out differently"), *Replay->BuildId, *GenBuild::GetBuildId());
	}
	if (Replay->DataHash != GenBuild::GetDataHash())
	{
		UE_LOG(LogGenerals, Warning, TEXT("Replay: made with other game data (%08x; this is %08x) - it may come out differently"), Replay->DataHash, GenBuild::GetDataHash());
	}
	const FGenMatchSettings Settings = Replay->Settings;
	const TArray<FGenSetupObject> SetupObjects = Replay->SetupObjects;
	int32 Viewer = Replay->RecordedBy;
	FParse::Value(FCommandLine::Get(), TEXT("GenReplayViewer="), Viewer);
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return false;
	}
	Logic->SetPlayback(MoveTemp(Replay));
	if (!BeginMatch(Settings, false, Viewer))
	{
		return false;
	}
	Logic->PlaceSetupObjects(SetupObjects);
	return true;
}

void AGenGameMode::SaveReplay()
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !Logic->IsRunning() || !Logic->GetRecording())
	{
		return;
	}
	FString Path;
	const bool bAsked = FParse::Value(FCommandLine::Get(), TEXT("GenRecord="), Path);
	if (Logic->IsPlayingBack() && !bAsked)
	{
		return; // (the replay is its own recording: LastReplay stays the last game played)
	}
	Logic->WaitForStep();
	Logic->SaveRecording(Path);
	FString ChecksumLog;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenChecksumLog="), ChecksumLog))
	{
		Logic->GetRecording()->SaveChecksumLog(ChecksumLog);
	}
}

void AGenGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SaveReplay();
	Super::EndPlay(EndPlayReason);
}

// ---- network games --------------------------------------------------------------------------------

void AGenGameMode::TickNetwork(float DeltaSeconds)
{
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	if (Phase == EGenGamePhase::Server)
	{
		static bool bExitAsked = false;
		if (!bExitAsked && Net && Net->GetServer() && Net->GetServer()->IsDone())
		{
			bExitAsked = true;
			FPlatformMisc::RequestExit(false, TEXT("GenServer done"));
		}
		return;
	}
	FGenNetClient* Client = Net ? Net->GetClient() : nullptr;
	if (!Client)
	{
		return;
	}
	Client->Update();
	const double Now = GetWorld()->GetRealTimeSeconds();
	for (const FString& Line : Client->TakeNotices())
	{
		UE_LOG(LogGenerals, Log, TEXT("Net: %s"), *Line);
		NetNotices.Emplace(Now, Line);
	}
	while (NetNotices.Num() > 8 || (NetNotices.Num() > 0 && Phase == EGenGamePhase::Playing && Now - NetNotices[0].Key > 15.0))
	{
		NetNotices.RemoveAt(0);
	}
	// The games have come apart: this one's recording (with the checksum of every frame) is kept at once, to
	// find out where and why. (Every game also writes its whole state at one frame: the server's "dump" order.)
	if (Client->GetMismatchFrame() != 0 && !bMismatchKept)
	{
		bMismatchKept = true;
		if (UGenGameLogic* Kept = UGenGameLogic::Get(this); Kept && Kept->IsRunning())
		{
			Kept->WaitForStep();
			Kept->SaveRecording(FGenReplay::GetDefaultFolder() / FString::Printf(TEXT("Mismatch_f%u_%s.genrep"), Client->GetMismatchFrame(),
				*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
		}
	}

	// The host has started the match (or this game comes back to one): it begins here, its orders from the host.
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FGenMatchSettings Settings;
	int32 Player = INDEX_NONE;
	bool bRejoin = false;
	if (Phase == EGenGamePhase::NetLobby && Logic && !Logic->IsRunning() && Client->TakeMatchStart(Settings, Player, bRejoin))
	{
		UE_LOG(LogGenerals, Log, TEXT("Net: %s the match as player %d"), bRejoin ? TEXT("back in") : TEXT("starting"), Player);
		Logic->SetOrderSource(Client);
		if (BeginMatch(Settings, true, Player))
		{
			Client->SendLoaded();
			Net->RememberSeat();
		}
		else
		{
			Logic->SetOrderSource(nullptr);
			NetNotices.Emplace(Now, ShellError);
		}
	}

	// -GenNetStart=N (tests): once N players have joined, the host sets the lobby up from the command line -
	// -GenMap=, -GenNetAI=Hard,Easy (computer players in the next open places), -GenNetTeams=1,1,2 (by place) -
	// and starts the match.
	// -GenNetTeam=N, -GenNetSide=FactionAmerica (tests): this player's team and army, chosen as soon as it is in the lobby.
	int32 OwnTeam = 0;
	FString OwnSide;
	const bool bTeamSwitch = FParse::Value(FCommandLine::Get(), TEXT("GenNetTeam="), OwnTeam);
	const bool bSideSwitch = FParse::Value(FCommandLine::Get(), TEXT("GenNetSide="), OwnSide);
	if (Phase == EGenGamePhase::NetLobby && !bNetTeamSent && Client->HasLobby() && Client->GetLocalSlot() != INDEX_NONE && (bTeamSwitch || bSideSwitch))
	{
		FGenSetupData Wanted = Client->GetLobby();
		if (bTeamSwitch)
		{
			Wanted.Slots[Client->GetLocalSlot()].Team = OwnTeam - 1;
		}
		if (bSideSwitch)
		{
			Wanted.Slots[Client->GetLocalSlot()].Side = FGenShellData::Get().FindSide(OwnSide);
		}
		Client->SendLobbyEdit(Wanted);
		bNetTeamSent = true;
	}
	int32 WantPlayers = 0;
	if (Phase == EGenGamePhase::NetLobby && NetAutoStage < 2 && Client->IsHost() && Client->HasLobby()
		&& FParse::Value(FCommandLine::Get(), TEXT("GenNetStart="), WantPlayers))
	{
		int32 Humans = 0;
		for (int32 s = 0; s < FGenSetupData::MaxSlots; ++s)
		{
			Humans += Client->IsSlotHuman(s) ? 1 : 0;
		}
		if (NetAutoStage == 0 && Humans >= WantPlayers)
		{
			FGenSetupData Wanted = Client->GetLobby();
			FString MapName;
			if (FParse::Value(FCommandLine::Get(), TEXT("GenMap="), MapName))
			{
				Wanted.Map = MapName;
			}
			FString AISpec, TeamSpec;
			TArray<FString> Parts;
			if (FParse::Value(FCommandLine::Get(), TEXT("GenNetAI="), AISpec))
			{
				AISpec.ParseIntoArray(Parts, TEXT(","));
				int32 Next = 0;
				for (const FString& Level : Parts)
				{
					while (Next < FGenSetupData::MaxSlots && Wanted.Slots[Next].State == EGenSlotState::Player) { ++Next; }
					if (Next < FGenSetupData::MaxSlots)
					{
						Wanted.Slots[Next++].State = Level.Equals(TEXT("Easy"), ESearchCase::IgnoreCase) ? EGenSlotState::EasyAI
							: (Level.Equals(TEXT("Hard"), ESearchCase::IgnoreCase) ? EGenSlotState::HardAI : EGenSlotState::MediumAI);
					}
				}
			}
			if (FParse::Value(FCommandLine::Get(), TEXT("GenNetTeams="), TeamSpec))
			{
				TeamSpec.ParseIntoArray(Parts, TEXT(","));
				for (int32 s = 0; s < Parts.Num() && s < FGenSetupData::MaxSlots; ++s)
				{
					Wanted.Slots[s].Team = FCString::Atoi(*Parts[s]) - 1;
				}
			}
			Client->SendLobbyEdit(Wanted);
			NetAutoStage = 1;
			NetAutoSeconds = 0.f;
			UE_LOG(LogGenerals, Log, TEXT("Test net: %d players are in - setting the game up"), Humans);
		}
		else if (NetAutoStage == 1 && (NetAutoSeconds += DeltaSeconds) > 1.5f)
		{
			// (The teams of the players' own places are theirs to choose: everybody sends its own.)
			Client->RequestStart();
			NetAutoStage = 2;
			UE_LOG(LogGenerals, Log, TEXT("Test net: start"));
		}
	}
}

void AGenGameMode::NetLobbyEdit(const FGenSetupData& Wanted)
{
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	if (FGenNetClient* Client = Net ? Net->GetClient() : nullptr)
	{
		Client->SendLobbyEdit(Wanted);
	}
}

void AGenGameMode::HostNetworkGame()
{
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	FString Why;
	if (!Net || !Net->Host(Setup ? Setup->PlayerName : FString(TEXT("Player")), GenNet::DefaultPort, &Why))
	{
		ShellError = Why;
		return;
	}
	NetNotices.Reset();
	NetAutoStage = 0;
	Phase = EGenGamePhase::NetLobby;
}

void AGenGameMode::JoinNetworkGame(const FString& Address)
{
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	if (!Net || Address.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	Net->Join(Address.TrimStartAndEnd(), Setup ? Setup->PlayerName : FString(TEXT("Player")));
	NetNotices.Reset();
	NetAutoStage = 0;
	Phase = EGenGamePhase::NetLobby;
}

void AGenGameMode::RejoinNetworkGame()
{
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	FString Address, Name;
	uint64 Seat = 0;
	if (Net && Net->RecallSeat(Address, Name, Seat))
	{
		Net->Join(Address, Name, Seat);
		NetNotices.Reset();
		Phase = EGenGamePhase::NetLobby;
	}
}

void AGenGameMode::LeaveNetworkGame()
{
	if (UGenNetSubsystem* Net = UGenNetSubsystem::Get(this))
	{
		Net->Leave();
	}
	NetNotices.Reset();
	NetAutoStage = 0;
	bNetTeamSent = false;
	Phase = EGenGamePhase::Menu;
	SetMenuScreen(EGenMenuScreen::Online); // (back where the game was hosted or joined)
}

// ---- the shell: menu, in-game menu, score screen -------------------------------------------------

void AGenGameMode::SetMenuScreen(EGenMenuScreen Screen)
{
	MenuScreen = Screen;
	// (Kept over the reloads of the level between the menu and a game: "continue" comes back to it.)
	LastMenuScreen = Screen == EGenMenuScreen::Main ? EGenMenuScreen::Skirmish : Screen;
}

void AGenGameMode::RequestStart()
{
	UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	if (Phase != EGenGamePhase::Menu || !Setup)
	{
		return;
	}
	FRandomStream Random = MakeSetupRandom();
	ShellError = Setup->Resolve(Random, PendingSlots);
	if (!ShellError.IsEmpty())
	{
		return;
	}
	Setup->Save();
	Phase = EGenGamePhase::Loading;
	LoadingTicks = 0;
}

void AGenGameMode::ToggleQuitMenu()
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (Phase != EGenGamePhase::Playing || !Logic || !Logic->IsRunning())
	{
		return;
	}
	bQuitMenuOpen = !bQuitMenuOpen;
	ApplyPause();
	if (bQuitMenuOpen && GEngine)
	{
		// The game stands still behind the menu: the moment for the engine to collect its garbage, which
		// takes a frame of fifteen or twenty milliseconds (during play it is left alone for an hour: see
		// gc.TimeBetweenPurgingPendingKillObjects in DefaultEngine.ini).
		GEngine->ForceGarbageCollection(false);
	}
}

void AGenGameMode::ApplyPause()
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (Logic)
	{
		// SetPaused refuses where an order source drives the clock (a network game): the host keeps going.
		Logic->SetPaused(bQuitMenuOpen || bPauseRequested);
	}
}

void AGenGameMode::TogglePause()
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (Phase != EGenGamePhase::Playing || !Logic || !Logic->IsRunning())
	{
		return;
	}
	bPauseRequested = !bPauseRequested;
	ApplyPause();
}

void AGenGameMode::ReloadLevel()
{
	// A fresh world: nothing of the old game is left to clean up.
	const FString Level = UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName());
	UGameplayStatics::OpenLevel(this, FName(*Level), true, OptionsString);
}

void AGenGameMode::RestartGame()
{
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (Logic && Logic->IsNetworkGame())
	{
		return; // (a network game isn't one player's to start over)
	}
	if (UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this))
	{
		Setup->bStartAtOnce = bStartedFromMenu;
	}
	ReloadLevel();
}

void AGenGameMode::EndGame()
{
	if (Phase == EGenGamePhase::Playing)
	{
		bQuitMenuOpen = false;
		bPauseRequested = false;
		Phase = EGenGamePhase::Score; // (the game stays as it stands, behind the score)
		if (GEngine)
		{
			GEngine->ForceGarbageCollection(false); // (nobody minds a long frame now)
		}
		UE_LOG(LogGenerals, Log, TEXT("Shell: the score screen (game ended from the menu)"));
		LogScores();
	}
}

void AGenGameMode::LogScores() const
{
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return;
	}
	for (const FGenPlayer& P : Logic->GetPlayers())
	{
		if (!P.bNeutral && P.Template)
		{
			UE_LOG(LogGenerals, Log, TEXT("  [score] p%d %s (%s): units %d built, %d lost, %d destroyed; buildings %d built, %d lost, %d destroyed; supplies %d; spent %d; score %d"),
				P.Index, *P.Name, *P.Template->Side, P.Score.UnitsBuilt, P.Score.UnitsLost, P.Score.GetUnitsDestroyed(), P.Score.BuildingsBuilt, P.Score.BuildingsLost,
				P.Score.GetBuildingsDestroyed(), P.Score.MoneyEarned, P.Score.MoneySpent, P.Score.Calculate(P.Index));
		}
	}
}

void AGenGameMode::ExitToMenu()
{
	if (UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this))
	{
		Setup->bStartAtOnce = false;
	}
	// Out of a network game for good: the connection goes, and the seat isn't come back to.
	if (UGenNetSubsystem* Net = UGenNetSubsystem::Get(this); Net && Net->IsInNetworkGame())
	{
		SaveReplay();
		Net->ForgetSeat();
		Net->Leave();
	}
	UE_LOG(LogGenerals, Log, TEXT("Shell: back to the menu"));
	ReloadLevel();
}

bool AGenGameMode::CanKeepWatching() const
{
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	return Phase == EGenGamePhase::Score && Logic && Logic->IsRunning() && !Logic->IsGameOver();
}

void AGenGameMode::KeepWatching()
{
	if (!CanKeepWatching())
	{
		return;
	}
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	bWatching = true;
	DecidedSeconds = 0.f;
	Phase = EGenGamePhase::Playing;
	bPauseRequested = false;
	Logic->SetPaused(false);
}

void AGenGameMode::SpawnTestUnits(UGenGameLogic* Logic)
{
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	FVector StartUE;
	if (!Players.IsValidIndex(Local) || !MapActor
		|| !MapActor->GetWaypointLocation(FString::Printf(TEXT("Player_%d_Start"), Players[Local].StartPosition + 1), StartUE))
	{
		return;
	}
	// Both groups stand on the line from the player's base towards the middle of the map: ours just in
	// front of the base, the enemy's further out and out of weapon range, facing each other.
	const FVector StartGen = GenCoords::FromUE(StartUE);
	const FVector CenterGen = GenCoords::FromUE(MapActor->GetPlayableBoundsUE().GetCenter());
	const FVector2f Start(float(StartGen.X), float(StartGen.Y));
	FVector2f Dir = FVector2f(float(CenterGen.X), float(CenterGen.Y)) - Start;
	Dir = Dir.IsNearlyZero() ? FVector2f(0.f, 1.f) : Dir.GetSafeNormal();
	const FVector2f Across(-Dir.Y, Dir.X);
	const float Facing = FMath::Atan2(Dir.Y, Dir.X);

	auto SpawnGroup = [&](int32 Player, float Distance, float Angle)
	{
		// Armor in the front row, light vehicles and infantry behind it.
		static const TMap<FString, TArray<FString>> Rosters = []
		{
			TMap<FString, TArray<FString>> M;
			M.Add(TEXT("USA"), TArray<FString>{ TEXT("AmericaTankCrusader"), TEXT("AmericaTankCrusader"), TEXT("AmericaTankPaladin"), TEXT("AmericaTankCrusader"), TEXT("AmericaTankCrusader"),
				TEXT("AmericaVehicleHumvee"), TEXT("AmericaInfantryMissileDefender"), TEXT("AmericaInfantryMissileDefender"), TEXT("AmericaInfantryMissileDefender"), TEXT("AmericaVehicleHumvee"),
				TEXT("AmericaInfantryRanger"), TEXT("AmericaInfantryRanger"), TEXT("AmericaInfantryRanger"), TEXT("AmericaInfantryRanger"), TEXT("AmericaInfantryRanger") });
			M.Add(TEXT("China"), TArray<FString>{ TEXT("ChinaTankBattleMaster"), TEXT("ChinaTankBattleMaster"), TEXT("ChinaTankOverlord"), TEXT("ChinaTankBattleMaster"), TEXT("ChinaTankBattleMaster"),
				TEXT("ChinaTankGattling"), TEXT("ChinaInfantryTankHunter"), TEXT("ChinaInfantryTankHunter"), TEXT("ChinaInfantryTankHunter"), TEXT("ChinaTankGattling"),
				TEXT("ChinaInfantryRedguard"), TEXT("ChinaInfantryRedguard"), TEXT("ChinaInfantryRedguard"), TEXT("ChinaInfantryRedguard"), TEXT("ChinaInfantryRedguard") });
			M.Add(TEXT("GLA"), TArray<FString>{ TEXT("GLATankScorpion"), TEXT("GLATankScorpion"), TEXT("GLATankMarauder"), TEXT("GLATankScorpion"), TEXT("GLATankScorpion"),
				TEXT("GLAVehicleTechnical"), TEXT("GLAInfantryRPGTrooper"), TEXT("GLAInfantryRPGTrooper"), TEXT("GLAInfantryRPGTrooper"), TEXT("GLAVehicleTechnical"),
				TEXT("GLAInfantryRebel"), TEXT("GLAInfantryRebel"), TEXT("GLAInfantryRebel"), TEXT("GLAInfantryRebel"), TEXT("GLAInfantryRebel") });
			return M;
		}();
		const FGenPlayerTemplate* Faction = Players[Player].Template;
		const TArray<FString>* Roster = Faction ? Rosters.Find(Faction->BaseSide) : nullptr;
		if (!Roster)
		{
			Roster = Rosters.Find(TEXT("USA"));
		}
		constexpr int32 Cols = 5;
		constexpr float Spacing = 45.f;
		// Rows stack away from the opposing group.
		const FVector2f Back = FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * -1.f;
		int32 Spawned = 0;
		for (int32 i = 0; i < Roster->Num(); ++i)
		{
			const FGenThingTemplate* T = FGenGameData::Get().FindTemplate((*Roster)[i]);
			if (!T)
			{
				UE_LOG(LogGenerals, Warning, TEXT("Test units: template '%s' not found"), *(*Roster)[i]);
				continue;
			}
			const FVector2f Wanted = Start + Dir * Distance + Across * ((float(i % Cols) - (Cols - 1) * 0.5f) * Spacing) + Back * (float(i / Cols) * Spacing);
			FVector2f Pos = Wanted;
			Logic->GetPathfinder().AdjustDestination(Wanted, GenSurface::Ground, Pos);
			if (Logic->CreateObject(T, Player, FVector3f(Pos.X, Pos.Y, Logic->GetGroundHeight(Pos.X, Pos.Y)), Angle))
			{
				++Spawned;
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Test units: %d for player %d around (%.0f, %.0f)"), Spawned, Player,
			Start.X + Dir.X * Distance, Start.Y + Dir.Y * Distance);
	};

	SpawnGroup(Local, 260.f, Facing);
	for (const FGenPlayer& P : Players)
	{
		// A computer opponent raises its own army; only a passive enemy (-GenNoAI) gets a group to shoot at.
		if (Logic->GetRelationship(Local, P.Index) == EGenRelationship::Enemies && !P.bSkirmishAI)
		{
			SpawnGroup(P.Index, 560.f, Facing + PI);
			break;
		}
	}
}

void AGenGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	TickNetwork(DeltaSeconds);
	if (Phase == EGenGamePhase::Server)
	{
		return;
	}
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	// The recording is written every two minutes as well as at the end: a crash loses no more than that.
	if (Phase == EGenGamePhase::Playing && Logic && Logic->IsRunning())
	{
		ReplaySaveSeconds += DeltaSeconds;
		if (ReplaySaveSeconds >= 120.f)
		{
			ReplaySaveSeconds = 0.f;
			SaveReplay();
		}
	}
	if (Phase == EGenGamePhase::Loading)
	{
		// ("Loading" has been on the screen for a frame or two: now the map is read, which takes a moment.)
		if (++LoadingTicks >= 3)
		{
			const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
			if (!Setup || !BeginGame(Setup->Map, PendingSlots, Setup->StartingCash, true))
			{
				Phase = EGenGamePhase::Menu;
			}
		}
	}
	else if (Phase == EGenGamePhase::Playing && Logic && Logic->IsRunning() && bStartedFromMenu)
	{
		// The game is decided for the local player: "victory" or "defeat" stands for a moment, then the score.
		const bool bLost = Logic->IsPlayerDefeated(Logic->GetLocalPlayer());
		if (Logic->IsGameOver() || (bLost && !bWatching))
		{
			DecidedSeconds += DeltaSeconds;
			if (DecidedSeconds >= ScoreScreenDelay)
			{
				bQuitMenuOpen = false;
				bPauseRequested = false;
				Logic->SetPaused(false);
				Phase = EGenGamePhase::Score;
				if (GEngine)
				{
					GEngine->ForceGarbageCollection(false); // (nobody minds a long frame now)
				}
				UE_LOG(LogGenerals, Log, TEXT("Shell: the score screen (%s)"), Logic->HasLocalPlayerWon() ? TEXT("victory") : TEXT("defeat"));
				LogScores();
				SaveReplay();
				// PlayerTemplate's ScoreScreenMusic for the local side.
				const TArray<FGenPlayer>& Players = Logic->GetPlayers();
				const FGenPlayerTemplate* Side = Players.IsValidIndex(Logic->GetLocalPlayer()) ? Players[Logic->GetLocalPlayer()].Template : nullptr;
				if (UGenAudioManager* Audio = UGenAudioManager::Get(this); Audio && Side && !Side->ScoreScreenMusic.IsEmpty())
				{
					Audio->PlayTrackOnly(Side->ScoreScreenMusic);
				}
			}
		}
		else
		{
			DecidedSeconds = 0.f;
		}
	}
	if (Phase == EGenGamePhase::Menu || Phase == EGenGamePhase::Loading || Phase == EGenGamePhase::NetLobby)
	{
		// (During a game the simulation keeps the sound going; before one, nothing else does.)
		if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
		{
			Audio->Update(DeltaSeconds);
		}
	}
	RunTestAutomation(DeltaSeconds);
}

void AGenGameMode::RunTestAutomation(float DeltaSeconds)
{
	Elapsed += DeltaSeconds;
	UGenGameLogic* Logic = UGenGameLogic::Get(this);

	// -GenTestMenu="seconds,click,Id;...": clicks on the interface, by the clock that runs from the start
	// of the program (the level is loaded anew between a game and the menu; the script carries on).
	if (UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this))
	{
		static TArray<TPair<double, FString>> MenuSteps;
		static bool bMenuStepsParsed = false;
		if (!bMenuStepsParsed)
		{
			bMenuStepsParsed = true;
			FString Spec;
			if (FParse::Value(FCommandLine::Get(), TEXT("GenTestMenu="), Spec, false))
			{
				TArray<FString> Steps;
				Spec.ParseIntoArray(Steps, TEXT(";"));
				for (const FString& Step : Steps)
				{
					TArray<FString> Parts;
					Step.ParseIntoArray(Parts, TEXT(","));
					if (Parts.Num() >= 3 && Parts[1].Equals(TEXT("click"), ESearchCase::IgnoreCase))
					{
						// (An id may itself contain commas: everything after "click,".)
						FString Id = Parts[2];
						for (int32 i = 3; i < Parts.Num(); ++i) { Id += TEXT(",") + Parts[i]; }
						MenuSteps.Emplace(FCString::Atod(*Parts[0]), Id);
					}
				}
			}
		}
		Setup->TestSeconds += double(DeltaSeconds);
		APlayerController* MenuPC = GetWorld()->GetFirstPlayerController();
		AGenHUD* HUD = MenuPC ? Cast<AGenHUD>(MenuPC->GetHUD()) : nullptr;
		if (HUD && MenuSteps.IsValidIndex(Setup->TestStepsDone) && Setup->TestSeconds >= MenuSteps[Setup->TestStepsDone].Key && !HUD->HasShellTestClick())
		{
			UE_LOG(LogGenerals, Log, TEXT("Test menu %.1fs: click '%s'"), Setup->TestSeconds, *MenuSteps[Setup->TestStepsDone].Value);
			HUD->ShellTestClick(MenuSteps[Setup->TestStepsDone].Value);
			++Setup->TestStepsDone;
		}
	}
	if (Logic && !Logic->IsRunning())
	{
		Logic = nullptr; // no game yet (the menu is up): of what follows only the screenshot applies
	}

	if (bTestMovePending && Elapsed > 1.f && Logic)
	{
		bTestMovePending = false;
		TArray<FGenObjectID> Units;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && O->GetOwner() == Logic->GetLocalPlayer() && O->CanMove())
			{
				Units.Add(Id);
			}
		}
		Logic->IssueMove(Units, FVector3f(TestMoveGoal.X, TestMoveGoal.Y, Logic->GetGroundHeight(TestMoveGoal.X, TestMoveGoal.Y)));
		UE_LOG(LogGenerals, Log, TEXT("Test move: %d units -> (%.0f, %.0f)"), Units.Num(), TestMoveGoal.X, TestMoveGoal.Y);
	}

	// -GenTestFX=Name[,Name...]: at logic frame 30, play FX lists / particle systems 6 units above the camera target
	// (a fixed frame, so -GenPauseFrame can freeze the effect at a known age).
	if (!bTestFXDone && Logic && Logic->GetFrame() >= 30)
	{
		bTestFXDone = true;
		FString FXNames;
		UGenFXManager* FXManager = UGenFXManager::Get(this);
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
		if (FXManager && Cam && FParse::Value(FCommandLine::Get(), TEXT("GenTestFX="), FXNames, false))
		{
			const FVector Gen = GenCoords::FromUE(Cam->GetTarget());
			TArray<FString> Names;
			FXNames.ParseIntoArray(Names, TEXT(","));
			for (const FString& Name : Names)
			{
				const bool bOk = FXManager->PlayByName(Name, FVector3f(float(Gen.X), float(Gen.Y), float(Gen.Z) + 6.f));
				UE_LOG(LogGenerals, Log, TEXT("Test FX '%s' at logic frame %u: %s"), *Name, Logic->GetFrame(), bOk ? TEXT("playing") : TEXT("not found"));
			}
		}
	}

	// Economy test hooks, run once at logic frame 30:
	//   -GenTestBuild=Building       the first local dozer constructs it at the camera target
	//   -GenTestQueue=Unit[,Unit]    each is queued at the first local building whose command set builds it
	//   -GenTestSelect=Template      selects the first local object of that type (shows its command bar)
	if (!bTestEconomyDone && Logic && Logic->GetFrame() >= 30)
	{
		bTestEconomyDone = true;
		const int32 Local = Logic->GetLocalPlayer();
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
		FString Arg;
		if (Cam && FParse::Value(FCommandLine::Get(), TEXT("GenTestBuild="), Arg, false))
		{
			const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Arg);
			const FVector Gen = GenCoords::FromUE(Cam->GetTarget());
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (T && O && O->GetOwner() == Local && O->GetTemplate()->bIsDozer && !O->IsDead())
				{
					const FVector3f Pos(float(Gen.X), float(Gen.Y), Logic->GetGroundHeight(float(Gen.X), float(Gen.Y)));
					UE_LOG(LogGenerals, Log, TEXT("Test build: %s at (%.0f, %.0f) by #%u, legal=%d, can make=%d"), *Arg, Pos.X, Pos.Y, Id,
						Logic->IsLocationLegalToBuild(*T, Pos, T->PlacementViewAngle) ? 1 : 0, Logic->CanMake(Local, *T) ? 1 : 0);
					Logic->IssueConstruct(Id, Arg, Pos, T->PlacementViewAngle);
					break;
				}
			}
		}
		if (FParse::Value(FCommandLine::Get(), TEXT("GenTestQueue="), Arg, false))
		{
			TArray<FString> Units;
			Arg.ParseIntoArray(Units, TEXT(","));
			for (const FString& Unit : Units)
			{
				const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Unit);
				bool bQueued = false, bRefused = false;
				FString Why;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					const FGenCommandSet* Set = (O && O->GetOwner() == Local && O->GetTemplate()->bHasProduction && !O->IsUnderConstruction())
						? FGenCommandData::Get().FindSet(O->GetCommandSet()) : nullptr;
					for (int32 s = 0; Set && s < FGenCommandSet::NumSlots && !bQueued && !bRefused; ++s)
					{
						const FGenCommandButton* Btn = FGenCommandData::Get().FindButton(Set->Slots[s]);
						if (Btn && Btn->Command == EGenCommand::UnitBuild && Btn->Object.Equals(Unit, ESearchCase::IgnoreCase))
						{
							// What the control bar would say about it (the limit a type has, prerequisites, money).
							if (T && !Logic->CanMake(Local, *T, &Why)) { bRefused = true; break; }
							Logic->IssueQueueUnit(Id, Unit);
							bQueued = true;
						}
					}
					if (bQueued || bRefused) { break; }
				}
				UE_LOG(LogGenerals, Log, TEXT("Test queue: %s %s"), *Unit,
					bQueued ? TEXT("queued") : bRefused ? *FString::Printf(TEXT("refused: %s"), *Why) : TEXT("has no factory"));
			}
		}
		AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
		if (GenPC && FParse::Value(FCommandLine::Get(), TEXT("GenTestSelect="), Arg, false))
		{
			// The first local object of that type; failing that, anybody's that is nearest the camera target.
			FGenObjectID Pick = GenInvalidObjectID;
			float BestDistSq = FLT_MAX;
			const FVector CamGen = Cam ? GenCoords::FromUE(Cam->GetTarget()) : FVector::ZeroVector;
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (!O || !O->GetTemplate()->Name.Equals(Arg, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (O->GetOwner() == Local)
				{
					Pick = Id;
					break;
				}
				const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(float(CamGen.X), float(CamGen.Y)));
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					Pick = Id;
				}
			}
			if (Pick != GenInvalidObjectID)
			{
				GenPC->SetSelection({ Pick });
			}
		}
	}

	//   -GenTestGather=Frame         at that logic frame every local gatherer is sent to the supply pile nearest the camera target
	int32 GatherFrame = 0;
	if (!bTestGatherDone && Logic && FParse::Value(FCommandLine::Get(), TEXT("GenTestGather="), GatherFrame) && Logic->GetFrame() >= uint32(FMath::Max(GatherFrame, 1)))
	{
		bTestGatherDone = true;
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
		const FVector CamGen = Cam ? GenCoords::FromUE(Cam->GetTarget()) : FVector::ZeroVector;
		FGenObjectID Pile = GenInvalidObjectID;
		float BestDistSq = FLT_MAX;
		TArray<FGenObjectID> Gatherers;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && O->GetOwner() == Logic->GetLocalPlayer() && O->GetTemplate()->bIsSupplyTruck && !O->IsDead())
			{
				Gatherers.Add(Id);
			}
			if (O && O->GetTemplate()->bSupplyWarehouse && O->GetBoxes() > 0)
			{
				const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(float(CamGen.X), float(CamGen.Y)));
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					Pile = Id;
				}
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Test gather: %d gatherers sent to #%u"), Gatherers.Num(), Pile);
		Logic->IssueDock(Gatherers, Pile);
	}

	//   -GenTestJets=Frame,order[,x,y]   at that logic frame every local aircraft is given the order:
	//        attack (the enemy object nearest x,y, or nearest the camera target), move (to x,y), land (at the
	//        nearest airfield of ours), stop. Several, separated by ';', run in turn: "900,attack;2400,move,900,700"
	FString JetSpec;
	if (Logic && FParse::Value(FCommandLine::Get(), TEXT("GenTestJets="), JetSpec, false))
	{
		TArray<FString> Orders;
		JetSpec.ParseIntoArray(Orders, TEXT(";"));
		while (TestJetOrdersDone < Orders.Num())
		{
			TArray<FString> Parts;
			Orders[TestJetOrdersDone].ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() < 2 || Logic->GetFrame() < uint32(FMath::Max(1, FCString::Atoi(*Parts[0]))))
			{
				break;
			}
			++TestJetOrdersDone;
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
			const FVector CamGen = Cam ? GenCoords::FromUE(Cam->GetTarget()) : FVector::ZeroVector;
			const FVector2f At = Parts.Num() >= 4 ? FVector2f(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3])) : FVector2f(float(CamGen.X), float(CamGen.Y));
			const int32 Local = Logic->GetLocalPlayer();
			TArray<FGenObjectID> Jets;
			FGenObjectID Victim = GenInvalidObjectID, Airfield = GenInvalidObjectID;
			float VictimDistSq = FLT_MAX, AirfieldDistSq = FLT_MAX;
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (!O || O->IsDead())
				{
					continue;
				}
				const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), At);
				if (O->GetOwner() == Local && O->GetTemplate()->bJetAI)
				{
					Jets.Add(Id);
				}
				else if (O->GetOwner() == Local && O->GetParking() && DistSq < AirfieldDistSq)
				{
					AirfieldDistSq = DistSq;
					Airfield = Id;
				}
				else if (O->IsTargetable() && Logic->GetRelationship(Local, O->GetOwner()) == EGenRelationship::Enemies && DistSq < VictimDistSq)
				{
					VictimDistSq = DistSq;
					Victim = Id;
				}
			}
			const FString& Order = Parts[1];
			UE_LOG(LogGenerals, Log, TEXT("Test jets f%u: %d aircraft, order '%s' (victim #%u, airfield #%u, at %.0f,%.0f)"), Logic->GetFrame(), Jets.Num(),
				*Order, Victim, Airfield, At.X, At.Y);
			if (Order.Equals(TEXT("attack"), ESearchCase::IgnoreCase)) { Logic->IssueAttack(Jets, Victim); }
			else if (Order.Equals(TEXT("move"), ESearchCase::IgnoreCase)) { Logic->IssueMove(Jets, FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y))); }
			else if (Order.Equals(TEXT("land"), ESearchCase::IgnoreCase)) { Logic->IssueLand(Jets, Airfield); }
			else if (Order.Equals(TEXT("stop"), ESearchCase::IgnoreCase)) { Logic->IssueStop(Jets); }
		}
	}

	//   -GenTestPowers=Frame,what[,args];...   upgrades, promotions and special powers of the local player, in turn:
	//        upgrade,Name       queued at the first building of ours that can research it
	//        grant,Name         that upgrade is ours at once, as if it had been researched
	//        combatdrop,x,y[,Type]  our transport helicopters drop their soldiers there (into the object of that type nearest the spot)
	//        kill,Type[,Player] every object of that type of ours (or of that player's) dies as if shot down
	//        skill,Points       experience towards the next rank (as if that much had been destroyed)
	//        aiskill,Points     the same for every computer player
	//        science,Name       bought with general's points, like a click in the promotions panel
	//        power,Name[,x,y]   fired at x,y (or the camera target) from whichever building has it ready
	//        money,Amount       cash
	//        generals           opens the promotions panel (for a screenshot)
	//        click,Button       a left click on that command button, wherever the interface shows it
	//        key,Key            that key (FGenKeyChord's form: B, Ctrl+F) pressed for the command bar's buttons (GenBarHotkeys)
	//        target,x,y         the left click on the ground that fires the power being aimed
	//        rally,Type,x,y     sets the rally point of our first building of that type
	//        damage,Type,Pct    leaves our first object of that type with that percentage of its health
	//        repair,Type        our dozers go and mend our first object of that type
	//        fix,Type           our damaged vehicles drive to our first building of that type for repairs
	//        attackmove,x,y     everything of ours that can move and fight attack-moves there
	//        guard,x,y          ... guards that spot
	//        vet,Type,Level     our units of that type are set to veterancy level 1..3
	//        destroy,Type,x,y   anybody's object of that type nearest that spot is destroyed
	//        enter,Unit,Type    our units of that type get into the container of that type nearest the camera target
	//                           (anybody's: a civilian building can be garrisoned); Unit "*" = everything of ours that can
	//        evacuate,Type      everybody out of the container of that type nearest the camera target
	//        allymove,x,y       (network games) what of an absent ally's we may command goes there
	//        desync             (network games) $1 for us outside the orders: this game comes apart from the others
	//   (The ones that change the game by hand - grant, kill, skill, aiskill, hunt, damage, vet, destroy, money - are
	//   orders too, UGenGameLogic::ExecCheat: a replay of the test has them.)
	FString PowerSpec;
	if (Logic && FParse::Value(FCommandLine::Get(), TEXT("GenTestPowers="), PowerSpec, false))
	{
		TArray<FString> Orders;
		PowerSpec.ParseIntoArray(Orders, TEXT(";"));
		while (TestPowerOrdersDone < Orders.Num())
		{
			TArray<FString> Parts;
			Orders[TestPowerOrdersDone].ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() < 2 || Logic->GetFrame() < uint32(FMath::Max(1, FCString::Atoi(*Parts[0]))))
			{
				break;
			}
			++TestPowerOrdersDone;
			Logic->WaitForStep(); // (some of these look at the simulation in ways a step under way mustn't change)
			const int32 Local = Logic->GetLocalPlayer();
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
			const FVector CamGen = Cam ? GenCoords::FromUE(Cam->GetTarget()) : FVector::ZeroVector;
			const FString& What = Parts[1];
			const FString Arg = Parts.Num() >= 3 ? Parts[2] : FString();
			// The steps that change the game by hand are orders like the others (UGenGameLogic::ExecCheat), so that
			// a replay of the test has them at the same frame.
			static const TCHAR* const CheatSteps[] = { TEXT("grant"), TEXT("kill"), TEXT("skill"), TEXT("aiskill"), TEXT("hunt"), TEXT("damage"),
				TEXT("vet"), TEXT("destroy"), TEXT("money") };
			if (Algo::FindByPredicate(CheatSteps, [&What](const TCHAR* S) { return What.Equals(S, ESearchCase::IgnoreCase); }))
			{
				Logic->IssueCheat(FString::Join(TArrayView<const FString>(Parts).RightChop(1), TEXT(",")));
				continue;
			}
			if (What.Equals(TEXT("upgrade"), ESearchCase::IgnoreCase))
			{
				const FGenUpgradeTemplate* Upgrade = FGenUpgradeData::Get().FindUpgrade(Arg);
				FString Why = TEXT("no such upgrade");
				bool bQueued = false;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (Upgrade && O && O->GetOwner() == Local && O->GetTemplate()->bHasProduction)
					{
						FString WhyNot;
						if (Logic->CanQueueUpgrade(*O, *Upgrade, &WhyNot))
						{
							Logic->IssueQueueUpgrade(Id, Arg);
							bQueued = true;
							break;
						}
						if (WhyNot != TEXT("Not available here")) { Why = WhyNot; }
						else if (Why == TEXT("no such upgrade")) { Why = TEXT("no building of ours offers it"); }
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: upgrade %s %s"), Logic->GetFrame(), *Arg, bQueued ? TEXT("queued") : *Why);
			}
			else if (What.Equals(TEXT("combatdrop"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				const FVector2f Near(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				TArray<FGenObjectID> Helicopters;
				const FGenObject* Into = nullptr;
				float BestDistSq = FLT_MAX;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->IsDead())
					{
						continue;
					}
					if (O->GetOwner() == Local && O->GetTemplate()->bChinookAI)
					{
						Helicopters.Add(Id);
					}
					const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Near);
					if (Parts.Num() >= 5 && O->GetTemplate()->Canonical()->Name.Equals(Parts[4], ESearchCase::IgnoreCase) && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Into = O;
					}
				}
				const bool bOk = !Into || Logic->CanCombatDropInto(*Into);
				if (bOk)
				{
					Logic->IssueCombatDrop(Helicopters, Into ? Into->GetID() : GenInvalidObjectID, FVector3f(Near.X, Near.Y, Logic->GetGroundHeight(Near.X, Near.Y)));
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: combat drop by %d helicopter(s) at (%.0f, %.0f)%s%s%s"), Logic->GetFrame(), Helicopters.Num(), Near.X, Near.Y,
					Into ? TEXT(" into ") : TEXT(""), Into ? *Into->GetTemplate()->Name : TEXT(""), bOk ? TEXT("") : TEXT(": not allowed"));
			}
			else if (What.Equals(TEXT("science"), ESearchCase::IgnoreCase))
			{
				FString Why;
				const bool bCan = Logic->CanPurchaseScience(Local, Arg, &Why);
				Logic->IssuePurchaseScience(Local, Arg);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: science %s %s"), Logic->GetFrame(), *Arg, bCan ? TEXT("purchased") : *Why);
			}
			else if (What.Equals(TEXT("power"), ESearchCase::IgnoreCase))
			{
				const FVector2f At = Parts.Num() >= 5 ? FVector2f(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4])) : FVector2f(float(CamGen.X), float(CamGen.Y));
				int32 Module = INDEX_NONE;
				const FGenObject* Source = Logic->FindPowerSource(Local, Arg, false, &Module);
				FString Why = TEXT("no building of ours has it");
				const bool bReady = Source && Logic->IsPowerReady(*Source, Module, nullptr, &Why);
				// A power aimed at a building takes the enemy building nearest that spot.
				FGenObjectID Target = GenInvalidObjectID;
				float BestDistSq = FLT_MAX;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					const float DistSq = O ? FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), At) : 0.f;
					if (O && !O->IsDead() && O->IsKindOf(TEXT("FS_SUPPLY_CENTER")) && Logic->GetRelationship(Local, O->GetOwner()) == EGenRelationship::Enemies && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Target = Id;
					}
				}
				const bool bAtObject = Source && Source->GetTemplate()->PowerModules[Module].Type == EGenPowerModule::CashHack;
				if (bReady)
				{
					Logic->IssueSpecialPower(Source->GetID(), Arg, FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)), bAtObject ? Target : GenInvalidObjectID);
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: power %s at (%.0f, %.0f) %s"), Logic->GetFrame(), *Arg, At.X, At.Y, bReady ? TEXT("fired") : *Why);
			}
			else if (What.Equals(TEXT("cost"), ESearchCase::IgnoreCase))
			{
				// "frame,cost,Template": what it costs us right now
				const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(Arg);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: cost of %s: $%d (list price $%d)"), Logic->GetFrame(), *Arg,
					Thing ? Logic->CalcCostToBuild(Local, *Thing) : -1, Thing ? int32(Thing->BuildCost) : -1);
			}
			else if (What.Equals(TEXT("hack"), ESearchCase::IgnoreCase))
			{
				// "frame,hack": our hackers sit down and hack the internet
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && !O->IsDead() && O->GetTemplate()->HackInternet.bValid) { Units.Add(Id); }
				}
				Logic->IssueHackInternet(Units);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d hackers told to hack the internet"), Logic->GetFrame(), Units.Num());
			}
			else if (What.Equals(TEXT("ability"), ESearchCase::IgnoreCase) || What.Equals(TEXT("weapon"), ESearchCase::IgnoreCase))
			{
				// "frame,ability,CommandButton[,x,y]": every unit of ours with that button uses its ability on
				// whatever it can be used on nearest that spot (no spot: on nothing - "detonate").
				// "frame,weapon,CommandButton,x,y": the same for a FIRE_WEAPON button.
				const FGenCommandButton* Btn = FGenCommandData::Get().FindButton(Arg);
				const FGenSpecialPowerTemplate* Power = Btn ? FGenUpgradeData::Get().FindPower(Btn->SpecialPower) : nullptr;
				const bool bWeapon = What.Equals(TEXT("weapon"), ESearchCase::IgnoreCase);
				const bool bHasSpot = Parts.Num() >= 5;
				const FVector2f At = bHasSpot ? FVector2f(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4])) : FVector2f::ZeroVector;
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->GetOwner() != Local || O->IsDead() || !Btn)
					{
						continue;
					}
					const FGenCommandSet* Set = FGenCommandData::Get().FindSet(O->GetCommandSet());
					for (int32 s = 0; Set && s < FGenCommandSet::NumSlots; ++s)
					{
						if (Set->Slots[s].Equals(Btn->Name, ESearchCase::IgnoreCase)) { Units.Add(Id); break; }
					}
				}
				FGenObjectID Target = GenInvalidObjectID;
				float BestDistSq = FLT_MAX;
				const FGenObject* First = Units.Num() > 0 ? Logic->FindObject(Units[0]) : nullptr;
				const int32 Module = (First && Power) ? Logic->FindPowerModule(*First, Power->Name) : INDEX_NONE;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!bHasSpot || !First || !O || O->IsDead() || O->IsProjectile() || Units.Contains(Id))
					{
						continue;
					}
					if (bWeapon ? (Logic->GetRelationship(Local, O->GetOwner()) != EGenRelationship::Enemies || !O->IsAttackable())
					: !Logic->CanUseAbilityOn(*First, Module, O, false))
					{
						continue;
					}
					const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), At);
					if (DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Target = Id;
					}
				}
				const FGenObject* Victim = Logic->FindObject(Target);
				int32 Ready = 0;
				for (FGenObjectID Id : Units)
				{
					const FGenObject* U = Logic->FindObject(Id);
					if (bWeapon)
					{
						++Ready;
					}
					else if (Power)
					{
						const int32 M = Logic->FindPowerModule(*U, Power->Name);
						FString Why;
						const bool bCan = Logic->CanUseAbilityOn(*U, M, Victim, true);
						Ready += bCan ? 1 : 0;
						if (!bCan && M != INDEX_NONE) { Logic->IsPowerReady(*U, M, nullptr, &Why); }
						UE_CLOG(!bCan, LogGenerals, Log, TEXT("Test powers f%u: #%u %s can't: %s"), Logic->GetFrame(), Id, *U->GetTemplate()->Name,
							Why.IsEmpty() ? TEXT("not on that") : *Why);
						Logic->IssueSpecialPower(Id, Power->Name, Victim ? Victim->GetPosition() : U->GetPosition(), Target);
					}
				}
				if (bWeapon && Btn && Victim)
				{
					Logic->IssueFireWeapon(Units, Target, Btn->WeaponSlot);
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %s %s on %s at (%.0f, %.0f): %d of %d units can"), Logic->GetFrame(), *What, *Arg,
					Victim ? *Victim->GetTemplate()->Name : (bHasSpot ? TEXT("(nothing there)") : TEXT("(no target)")), Victim ? Victim->GetPosition().X : At.X,
					Victim ? Victim->GetPosition().Y : At.Y, Ready, Units.Num());
			}
			else if (What.Equals(TEXT("status"), ESearchCase::IgnoreCase))
			{
				// "frame,status,Type": anybody's objects of that type - whose, health, hacked / disabled, trapped; and our money
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && !O->IsDestroyed() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
					{
						UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: status #%u %s p%d at (%.0f, %.0f) hp=%.0f/%.0f%s%s%s%s%s [%s]"), Logic->GetFrame(), Id, *O->GetTemplate()->Name,
							O->GetOwner(), O->GetPosition().X, O->GetPosition().Y, O->GetHealth(), O->GetMaxHealth(), O->IsDead() ? TEXT(" DEAD") : TEXT(""),
							O->IsHacked() ? TEXT(" HACKED") : TEXT(""), O->IsDisabled() ? TEXT(" DISABLED") : TEXT(""), O->IsBoobyTrapped() ? TEXT(" TRAPPED") : TEXT(""),
							O->IsHackingInternet() ? TEXT(" HACKING") : TEXT(""), *O->DescribeBusy());
						UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: status #%u drawn: %s"), Logic->GetFrame(), Id, *Logic->DescribeObjectModel(Id));
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: status money p%d=$%d"), Logic->GetFrame(), Local, Logic->GetPlayers()[Local].Money);
			}
			else if (What.Equals(TEXT("capture"), ESearchCase::IgnoreCase))
			{
				// "frame,capture,x,y": all our units that can are sent to capture the building nearest that spot
				// (anybody's but ours).
				const FVector2f At = Parts.Num() >= 4 ? FVector2f(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3])) : FVector2f(float(CamGen.X), float(CamGen.Y));
				FGenObjectID Target = GenInvalidObjectID;
				float BestDistSq = FLT_MAX;
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->IsDead())
					{
						continue;
					}
					if (O->GetOwner() == Local)
					{
						if (O->GetTemplate()->Abilities.Num() > 0) { Units.Add(Id); }
						continue;
					}
					const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), At);
					if (O->IsKindOf(TEXT("STRUCTURE")) && (O->IsKindOf(TEXT("CAPTURABLE")) || Logic->GetRelationship(Local, O->GetOwner()) == EGenRelationship::Enemies) && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Target = Id;
					}
				}
				const FGenObject* Victim = Logic->FindObject(Target);
				int32 Able = 0;
				for (FGenObjectID Id : Units)
				{
					Able += Victim && Logic->CanCaptureBuilding(*Logic->FindObject(Id), *Victim) ? 1 : 0;
				}
				Logic->IssueCapture(Units, Target);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: capture %s at (%.0f, %.0f): %d of %d units can"), Logic->GetFrame(),
					Victim ? *Victim->GetTemplate()->Name : TEXT("(nothing there)"), Victim ? Victim->GetPosition().X : At.X, Victim ? Victim->GetPosition().Y : At.Y, Able, Units.Num());
			}
			else if (What.Equals(TEXT("rally"), ESearchCase::IgnoreCase) || What.Equals(TEXT("repair"), ESearchCase::IgnoreCase)
				|| What.Equals(TEXT("fix"), ESearchCase::IgnoreCase))
			{
				// These act on our first living object of the named type.
				const FGenObject* Subject = nullptr;
				TArray<FGenObjectID> Dozers, Vehicles;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->GetOwner() != Local || O->IsDead())
					{
						continue;
					}
					if (!Subject && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase)) { Subject = O; }
					if (O->GetTemplate()->bIsDozer) { Dozers.Add(Id); }
					else if (O->CanMove() && O->GetHealth() < O->GetMaxHealth()) { Vehicles.Add(Id); }
				}
				if (!Subject)
				{
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %s: we have no %s"), Logic->GetFrame(), *What, *Arg);
				}
				else if (What.Equals(TEXT("rally"), ESearchCase::IgnoreCase) && Parts.Num() >= 5)
				{
					const FVector2f At(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]));
					Logic->IssueSetRallyPoint({ Subject->GetID() }, FVector3f(At.X, At.Y, 0.f));
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: rally point of #%u %s -> (%.0f, %.0f)"), Logic->GetFrame(), Subject->GetID(), *Arg, At.X, At.Y);
				}
				else if (What.Equals(TEXT("repair"), ESearchCase::IgnoreCase))
				{
					Logic->IssueRepair(Dozers, Subject->GetID());
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d dozer(s) told to mend #%u %s (%.0f / %.0f)"), Logic->GetFrame(), Dozers.Num(), Subject->GetID(),
						*Arg, Subject->GetHealth(), Subject->GetMaxHealth());
				}
				else if (What.Equals(TEXT("fix"), ESearchCase::IgnoreCase))
				{
					Logic->IssueGetRepaired(Vehicles, Subject->GetID());
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d damaged unit(s) sent to #%u %s for repairs"), Logic->GetFrame(), Vehicles.Num(), Subject->GetID(), *Arg);
				}
			}
			else if (What.Equals(TEXT("enter"), ESearchCase::IgnoreCase) || What.Equals(TEXT("evacuate"), ESearchCase::IgnoreCase)
				|| What.Equals(TEXT("enemyenter"), ESearchCase::IgnoreCase))
			{
				// ("enemyenter": the same for the other players' units - to put somebody into a building we mean to clear.)
				const bool bEnemyUnits = What.Equals(TEXT("enemyenter"), ESearchCase::IgnoreCase);
				const bool bEnter = bEnemyUnits || What.Equals(TEXT("enter"), ESearchCase::IgnoreCase);
				const FString ContainerType = bEnter ? (Parts.Num() >= 4 ? Parts[3] : FString()) : Arg;
				// (Nearest the camera target, or nearest the spot given after the type: "enter,Unit,Type,x,y", "evacuate,Type,x,y".)
				const int32 XY = bEnter ? 4 : 3;
				const FVector2f Near = Parts.Num() >= XY + 2 ? FVector2f(FCString::Atof(*Parts[XY]), FCString::Atof(*Parts[XY + 1])) : FVector2f(float(CamGen.X), float(CamGen.Y));
				const FGenObject* Container = nullptr;
				float BestDistSq = FLT_MAX;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					const float DistSq = O ? FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Near) : 0.f;
					// (Something without room inside can be "entered" too: a pilot climbs into a tank.)
					if (O && !O->IsDead() && (bEnter || O->GetContain()) && O->GetTemplate()->Canonical()->Name.Equals(ContainerType, ESearchCase::IgnoreCase) && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Container = O;
					}
				}
				if (!Container)
				{
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %s: no container of type %s"), Logic->GetFrame(), *What, *ContainerType);
				}
				else if (bEnter)
				{
					TArray<FGenObjectID> Units;
					for (FGenObjectID Id : Logic->GetObjectIDs())
					{
						const FGenObject* O = Logic->FindObject(Id);
						if (O && (bEnemyUnits ? (O->GetOwner() != Local && O->GetOwner() != 0) : O->GetOwner() == Local)
							&& (Arg == TEXT("*") || O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
							&& Logic->CanEnterObject(*O, *Container, false))
						{
							Units.Add(Id);
						}
					}
					Logic->IssueEnter(Units, Container->GetID());
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d unit(s) told to enter #%u %s"), Logic->GetFrame(), Units.Num(), Container->GetID(), *ContainerType);
				}
				else
				{
					Logic->IssueEvacuate(Container->GetID());
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: #%u %s evacuates %d rider(s)"), Logic->GetFrame(), Container->GetID(), *ContainerType,
						Logic->GetRiders(*Container).Num());
				}
			}
			else if ((What.Equals(TEXT("attackmove"), ESearchCase::IgnoreCase) || What.Equals(TEXT("guard"), ESearchCase::IgnoreCase)) && Parts.Num() >= 4)
			{
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && !O->IsDead() && O->CanMove() && O->HasWeapons() && !O->GetTemplate()->bJetAI)
					{
						Units.Add(Id);
					}
				}
				const FVector3f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]), 0.f);
				if (What.Equals(TEXT("guard"), ESearchCase::IgnoreCase)) { Logic->IssueGuard(Units, At, GenInvalidObjectID, EGenGuardMode::Normal); }
				else { Logic->IssueAttackMove(Units, At); }
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d unit(s) told to %s (%.0f, %.0f)"), Logic->GetFrame(), Units.Num(), *What, At.X, At.Y);
			}
			else if (What.Equals(TEXT("attack"), ESearchCase::IgnoreCase) && Parts.Num() >= 5)
			{
				// "frame,attack,Type,x,y": everything of ours that moves and fights attacks the object of that type
				// nearest the spot (a player's attack order on it).
				const FVector2f Near(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]));
				const FGenObject* Victim = nullptr;
				float BestDistSq = FLT_MAX;
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->IsDead())
					{
						continue;
					}
					const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Near);
					if (O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase) && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Victim = O;
					}
					if (O->GetOwner() == Local && O->CanMove() && O->HasWeapons())
					{
						Units.Add(Id);
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d unit(s) attack %s #%u"), Logic->GetFrame(), Units.Num(), *Arg, Victim ? Victim->GetID() : 0u);
				if (Victim)
				{
					Logic->IssueAttack(Units, Victim->GetID());
				}
			}
			else if (What.Equals(TEXT("build"), ESearchCase::IgnoreCase) && Parts.Num() >= 5)
			{
				// "frame,build,Building,x,y": a dozer of ours with nothing to do puts it up there
				const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Arg);
				const FVector3f Pos(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]), Logic->GetGroundHeight(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4])));
				FGenObjectID Dozer = GenInvalidObjectID;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && O->GetTemplate()->bIsDozer && !O->IsDead() && !O->HasDozerTask())
					{
						Dozer = Id;
						break;
					}
				}
				const bool bLegal = T && Logic->IsLocationLegalToBuild(*T, Pos, T->PlacementViewAngle);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: build %s at (%.0f, %.0f, %.1f) by #%u, legal=%d, can make=%d"), Logic->GetFrame(), *Arg, Pos.X, Pos.Y, Pos.Z, Dozer,
					bLegal ? 1 : 0, T && Logic->CanMake(Local, *T) ? 1 : 0);
				if (T && Dozer != GenInvalidObjectID)
				{
					Logic->IssueConstruct(Dozer, Arg, Pos, T->PlacementViewAngle);
				}
			}
			else if (What.Equals(TEXT("queue"), ESearchCase::IgnoreCase))
			{
				// "frame,queue,Unit": at the first building of ours whose command set builds it
				bool bQueued = false;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Arg);
					if (T && O && O->GetOwner() == Local && !O->IsDead() && !O->IsUnderConstruction() && Logic->CanFactoryMake(*O, *T))
					{
						Logic->IssueQueueUnit(Id, Arg);
						bQueued = true;
						break;
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: queue %s: %s"), Logic->GetFrame(), *Arg, bQueued ? TEXT("queued") : TEXT("nothing of ours can make it"));
			}
			else if (What.Equals(TEXT("sell"), ESearchCase::IgnoreCase))
			{
				// "frame,sell,Building": the first of ours of that type
				bool bSold = false;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && !O->IsDead() && !O->IsBeingSold() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
					{
						Logic->IssueSell(Id);
						bSold = true;
						break;
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: sell %s: %s"), Logic->GetFrame(), *Arg, bSold ? TEXT("being sold") : TEXT("none of ours"));
			}
			else if (What.Equals(TEXT("sound"), ESearchCase::IgnoreCase))
			{
				// An audio event by name: at x,y if given, otherwise as an interface sound of ours.
				UGenAudioManager* Audio = Logic->GetAudio();
				uint32 Handle = 0;
				if (Audio && Parts.Num() >= 5)
				{
					const FVector2f At(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]));
					Handle = Audio->PlayAt(Arg, FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)), Local);
				}
				else if (Audio)
				{
					Handle = Audio->PlayUI(Arg, Local);
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: sound %s %s"), Logic->GetFrame(), *Arg, Handle ? TEXT("playing") : TEXT("NOT PLAYED"));
			}
			else if (What.Equals(TEXT("voice"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				// One of our objects of that type says one of its lines ("voice,VoiceMove,AmericaTankCrusader").
				const FGenThingTemplate* Type = FGenGameData::Get().FindTemplate(Parts[3]);
				bool bSaid = false;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (Type && O && O->GetOwner() == Local && !O->IsDead() && O->GetTemplate()->Canonical() == Type->Canonical())
					{
						Logic->PlayObjectSound(*O, { *Arg });
						bSaid = true;
						break;
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: voice %s of %s %s"), Logic->GetFrame(), *Arg, *Parts[3], bSaid ? TEXT("asked for") : TEXT("NO SUCH OBJECT"));
			}
			else if (What.Equals(TEXT("eva"), ESearchCase::IgnoreCase))
			{
				Logic->EvaEvent(*Arg, Local);
			}
			else if (What.Equals(TEXT("music"), ESearchCase::IgnoreCase))
			{
				if (UGenAudioManager* Audio = Logic->GetAudio()) { Audio->PlayMusic(Arg); }
			}
			else if (What.Equals(TEXT("exec"), ESearchCase::IgnoreCase))
			{
				// "frame,exec,console command": e.g. "stat dumpave -num=300 -ms=0.1" (the engine's own timers, to the log)
				if (GEngine)
				{
					GEngine->Exec(GetWorld(), *Arg);
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: exec '%s'"), Logic->GetFrame(), *Arg);
			}
			else if (What.Equals(TEXT("speed"), ESearchCase::IgnoreCase))
			{
				// "frame,speed,N": the game runs at N times its speed from here on
				Logic->SetTimeScale(FCString::Atof(*Arg));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: speed x%s"), Logic->GetFrame(), *Arg);
			}
			else if (What.Equals(TEXT("busy"), ESearchCase::IgnoreCase))
			{
				// "frame,busy": the camera goes to where most units are (the thick of it)
				TMap<FIntPoint, int32> Cells;
				FIntPoint Best(0, 0);
				int32 BestNum = 0;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->IsDead() || !O->CanMove() || O->IsProjectile())
					{
						continue;
					}
					const FIntPoint Cell(FMath::FloorToInt32(O->GetPosition().X / 150.f), FMath::FloorToInt32(O->GetPosition().Y / 150.f));
					const int32 Num = ++Cells.FindOrAdd(Cell);
					if (Num > BestNum)
					{
						BestNum = Num;
						Best = Cell;
					}
				}
				const FVector3f At((float(Best.X) + 0.5f) * 150.f, (float(Best.Y) + 0.5f) * 150.f, 0.f);
				if (AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC))
				{
					GenPC->LookAt(At);
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: camera to (%.0f, %.0f), %d units there"), Logic->GetFrame(), At.X, At.Y, BestNum);
			}
			else if (What.Equals(TEXT("formation"), ESearchCase::IgnoreCase))
			{
				// "frame,formation[,Kind or Type]": Ctrl+F for everything of ours that moves (of that kind or type).
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && O->CanMove() && !O->IsDead()
						&& (Parts.Num() < 3 || O->IsKindOf(*Parts[2]) || O->GetTemplate()->Name.Equals(Parts[2], ESearchCase::IgnoreCase)))
					{
						Units.Add(Id);
					}
				}
				Logic->IssueCreateFormation(Units);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: formation of %d units"), Logic->GetFrame(), Units.Num());
			}
			else if (What.Equals(TEXT("move"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				// Everything of ours that moves goes there ("frame,move,x,y[,Kind or Type]": only that kind or type).
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && O->CanMove() && !O->IsDead()
						&& (Parts.Num() < 5 || O->IsKindOf(*Parts[4]) || O->GetTemplate()->Name.Equals(Parts[4], ESearchCase::IgnoreCase)))
					{
						Units.Add(Id);
					}
				}
				Logic->IssueMove(Units, FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: move %d units to (%.0f, %.0f)"), Logic->GetFrame(), Units.Num(), At.X, At.Y);
			}
			else if ((What.Equals(TEXT("selecttype"), ESearchCase::IgnoreCase) && Parts.Num() >= 3) || What.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
			{
				// "frame,selecttype,Type": selects the first of ours of that type, as -GenTestSelect does at the start;
				// "frame,selection": what is selected now (after the controller's own additions, a mob's members).
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				if (GenPC && What.Equals(TEXT("selecttype"), ESearchCase::IgnoreCase))
				{
					for (FGenObjectID Id : Logic->GetObjectIDs())
					{
						const FGenObject* O = Logic->FindObject(Id);
						if (O && O->GetOwner() == Local && !O->IsDead() && O->GetTemplate()->Name.Equals(Parts[2], ESearchCase::IgnoreCase))
						{
							GenPC->SetSelection({ Id });
							break;
						}
					}
				}
				TArray<FString> Names;
				for (FGenObjectID Id : GenPC ? GenPC->GetSelection() : TArray<FGenObjectID>())
				{
					const FGenObject* O = Logic->FindObject(Id);
					Names.Add(O ? FString::Printf(TEXT("#%u %s"), Id, *O->GetTemplate()->Name) : FString::Printf(TEXT("#%u"), Id));
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %s - %d selected: %s"), Logic->GetFrame(), *What, Names.Num(), *FString::Join(Names, TEXT(", ")));
			}
			else if (What.Equals(TEXT("desync"), ESearchCase::IgnoreCase))
			{
				// "frame,desync": a dollar for us here only, not as an order - this game comes apart from the others
				// (a test of the network game's mismatch check).
				Logic->AddMoney(Local, 1);
				UE_LOG(LogGenerals, Warning, TEXT("Test powers f%u: desync - $1 added outside the orders"), Logic->GetFrame());
			}
			else if (What.Equals(TEXT("allymove"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				// "frame,allymove,x,y": what of a dropped-out ally's we may command (a network game) goes there.
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				TArray<FGenObjectID> Units;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() != Local && Logic->CanPlayerCommand(Local, *O) && O->CanMove() && !O->IsDead())
					{
						Units.Add(Id);
					}
				}
				Logic->IssueMove(Units, FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: allymove %d units of an absent ally to (%.0f, %.0f)"), Logic->GetFrame(), Units.Num(), At.X, At.Y);
			}
			else if (What.Equals(TEXT("fog"), ESearchCase::IgnoreCase))
			{
				// What the local player sees of everything of that type (anybody's), or of everything that isn't ours ("*").
				static const TCHAR* StatusNames[] = { TEXT("clear"), TEXT("partly clear"), TEXT("fogged (remembered)"), TEXT("shrouded") };
				static const TCHAR* CellNames[] = { TEXT("clear"), TEXT("fogged"), TEXT("shrouded") };
				const FGenThingTemplate* Type = FGenGameData::Get().FindTemplate(Arg);
				int32 Shown = 0, Hidden = 0;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->IsDestroyed() || (Type ? O->GetTemplate()->Canonical() != Type->Canonical() : (O->GetOwner() == Local || O->GetOwner() == 0)))
					{
						continue;
					}
					const bool bVisible = Logic->IsVisibleToLocalPlayer(*O);
					(bVisible ? Shown : Hidden)++;
					if (Type || Shown + Hidden <= 40)
					{
						UE_LOG(LogGenerals, Log, TEXT("  [fog f%u] #%u p%d %s at (%.0f, %.0f): %s, %s, cell %s, on radar %d%s%s"), Logic->GetFrame(), Id, O->GetOwner(),
							*O->GetTemplate()->Name, O->GetPosition().X, O->GetPosition().Y, StatusNames[int32(O->GetLocalShroud())], bVisible ? TEXT("drawn") : TEXT("hidden"),
							CellNames[int32(Logic->GetShroud().GetStatusAt(Local, O->GetPosition().X, O->GetPosition().Y))], Logic->IsOnRadar(*O) ? 1 : 0,
							O->IsStealthed() ? TEXT(", STEALTHED") : TEXT(""), O->IsDetected() ? TEXT(", DETECTED") : TEXT(""));
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("  [fog f%u] %s: %d drawn, %d hidden; radar %s"), Logic->GetFrame(), Type ? *Arg : TEXT("other players' objects"), Shown, Hidden,
					Logic->HasRadar(Local) ? TEXT("online") : TEXT("offline"));
			}
			else if (What.Equals(TEXT("radar"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				// A left click on the radar at the spot that shows this world position (the HUD's own click path).
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				bool bDone = false;
				if (GenPC && GenPC->GetControlBar().Radar.bIsValid)
				{
					const FGenControlBar& Bar = GenPC->GetControlBar();
					const FBox2D Rect = GenRadar::FindDrawRect(Bar.Radar, Bar.RadarMapSize);
					const FVector2D Pixel = GenRadar::RadarToPixel(FVector2D(At.X / Bar.RadarMapSize.X * GenRadar::CellWidth, At.Y / Bar.RadarMapSize.Y * GenRadar::CellHeight), Rect);
					bDone = GenPC->RadarClick(Pixel, false) && GenPC->RadarClick(Pixel, true);
				}
				const AGenCameraPawn* NowCam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
				const FVector NowGen = NowCam ? GenCoords::FromUE(NowCam->GetTarget()) : FVector::ZeroVector;
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: radar click at (%.0f, %.0f) %s; the view is now at (%.0f, %.0f)"), Logic->GetFrame(), At.X, At.Y,
					bDone ? TEXT("done") : TEXT("NOT POSSIBLE (no radar, or off the map)"), NowGen.X, NowGen.Y);
			}
			else if (What.Equals(TEXT("event"), ESearchCase::IgnoreCase))
			{
				// Space: the view jumps to the last thing the radar reported.
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				FVector3f EventPos;
				const bool bHas = Logic->GetLastRadarEventPos(EventPos);
				if (bHas && GenPC) { GenPC->LookAt(EventPos); }
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: last radar event %s (%.0f, %.0f), %d events so far"), Logic->GetFrame(),
					bHas ? TEXT("at") : TEXT("NONE"), EventPos.X, EventPos.Y, Logic->GetRadarEvents().Num());
			}
			else if (What.Equals(TEXT("generals"), ESearchCase::IgnoreCase))
			{
				if (AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC))
				{
					GenPC->SetGeneralsOpen(true);
				}
			}
			else if (What.Equals(TEXT("click"), ESearchCase::IgnoreCase))
			{
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const bool bClicked = GenPC && GenPC->TestClickButton(Arg);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: click %s %s"), Logic->GetFrame(), *Arg, bClicked ? TEXT("done") : TEXT("NOT POSSIBLE (not shown, or greyed out)"));
			}
			else if (What.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				// (Input is off in a test run: the key goes straight to the command bar, as UpdateBarHotkeys would send it.)
				FGenKeyChord Chord;
				const FGenCommandButton* Pressed = FGenKeyChord::Parse(Arg, Chord) ? GenPressBarHotkey(Cast<AGenPlayerController>(PC), Chord) : nullptr;
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: key %s pressed %s"), Logic->GetFrame(), *Arg, Pressed ? *Pressed->Name : TEXT("NOTHING (no button shown on that key)"));
			}
			else if (What.Equals(TEXT("target"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				// ("frame,target,x,y[,degrees]": a building being placed is turned that way first - the drag that rotates it.)
				const float* Angle = nullptr;
				float Degrees = 0.f;
				if (Parts.Num() >= 5)
				{
					Degrees = FCString::Atof(*Parts[4]);
					Angle = &Degrees;
				}
				const bool bFired = GenPC && GenPC->TestFireAt(FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)), Angle);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: target (%.0f, %.0f) %s"), Logic->GetFrame(), At.X, At.Y, bFired ? TEXT("fired") : TEXT("NOTHING BEING AIMED"));
			}
			else if (What.Equals(TEXT("pick"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				// "frame,pick,x,y[,select|hover]": what the cursor would be on with that spot of the ground under it (the pick
				// itself, by way of the screen); "select" selects it, "hover" leaves the cursor on it.
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				FVector2D Screen(0.0, 0.0);
				const FGenObjectID Hit = GenPC ? GenPC->TestPickAt(FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)), &Screen) : GenInvalidObjectID;
				const FGenObject* O = Logic->FindObject(Hit);
				const FString Mode = Parts.Num() >= 5 ? Parts[4] : FString();
				if (GenPC && Mode.Equals(TEXT("select"), ESearchCase::IgnoreCase))
				{
					TArray<FGenObjectID> Sel;
					if (O) { Sel.Add(Hit); }
					GenPC->SetSelection(Sel);
					Logic->FlashSelected(Hit); // (as the click does: Drawable::onSelected)
				}
				FString Cursor;
				if (GenPC && Mode.Equals(TEXT("hover"), ESearchCase::IgnoreCase))
				{
					GenPC->TestHoverAt(FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)));
					Cursor = TEXT(", cursor ") + GenPC->GetCursorName();
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: pick at (%.0f, %.0f), pixel (%.0f, %.0f): %s%s%s"), Logic->GetFrame(), At.X, At.Y, Screen.X, Screen.Y,
					O ? *FString::Printf(TEXT("#%u %s"), Hit, *O->GetTemplate()->Name) : TEXT("nothing"), Mode.IsEmpty() ? TEXT("") : *(TEXT(" (") + Mode + TEXT(")")), *Cursor);
			}
			else if ((What.Equals(TEXT("forceclick"), ESearchCase::IgnoreCase) || What.Equals(TEXT("leftclick"), ESearchCase::IgnoreCase)
				|| What.Equals(TEXT("waypointclick"), ESearchCase::IgnoreCase)) && Parts.Num() >= 4)
			{
				// "frame,forceclick,x,y" / "frame,waypointclick,x,y" / "frame,leftclick,x,y": a left click with that spot of
				// the ground under the cursor, with Ctrl held (force attack), Alt (a waypoint; Alt stays held from then
				// on, so the routes are drawn) or neither, for whatever is selected
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				const bool bForce = What.Equals(TEXT("forceclick"), ESearchCase::IgnoreCase);
				const bool bWaypoint = What.Equals(TEXT("waypointclick"), ESearchCase::IgnoreCase);
				const FString Done = GenPC ? GenPC->TestClickAt(FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)), bForce, bWaypoint) : FString(TEXT("no controller"));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %s at (%.0f, %.0f) %s"), Logic->GetFrame(), *What, At.X, At.Y, *Done);
			}
			else if (What.Equals(TEXT("mouse"), ESearchCase::IgnoreCase) && Parts.Num() >= 3)
			{
				// "frame,mouse,left|right,x,y" / "frame,mouse,scheme,classic|modern" / "frame,mouse,camera,key|edge|drag|zoom|rotate[,amount]":
				// the mouse as the controls page lays it out (AGenPlayerController::TestMouse)
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				TArray<FString> Args(Parts.GetData() + 2, Parts.Num() - 2);
				const FString Done = GenPC ? GenPC->TestMouse(Args) : FString(TEXT("no controller"));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: mouse %s"), Logic->GetFrame(), *Done);
			}
			else if (What.Equals(TEXT("steer"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				// "frame,steer,x,y": our gunships on their run are told to fire there
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				TArray<FGenObjectID> Ships;
				for (FGenObjectID Id : Logic->GetGunships())
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && O->IsGunshipSteerable()) { Ships.Add(Id); }
				}
				Logic->IssueSteerGunship(Ships, FVector3f(At.X, At.Y, 0.f));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: steer %d gunship(s) to (%.0f, %.0f)"), Logic->GetFrame(), Ships.Num(), At.X, At.Y);
			}
			else if (What.Equals(TEXT("selkey"), ESearchCase::IgnoreCase))
			{
				// "frame,selkey,what[,n]": a selection key - create / select / add / view with a group's number
				// (Ctrl + n, n, Shift + n, Alt + n), match (E), matchmap, all (Q), aircraft (W), double (a double click)
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const int32 Group = Parts.Num() >= 4 ? FCString::Atoi(*Parts[3]) : 1;
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: selkey %s %d: %s"), Logic->GetFrame(), *Arg, Group, GenPC ? *GenPC->TestSelectionKey(Arg, Group) : TEXT("no controller"));
			}
			else if (What.Equals(TEXT("pickscan"), ESearchCase::IgnoreCase))
			{
				// "frame,pickscan,Type": the object of that type nearest the camera target, picked at along two lines through
				// it - across the screen and up it - from two of its radii on one side to two on the other, a quarter
				// of a radius a step: '#' where the cursor would be on it, 'o' on something else, '.' on nothing.
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const FGenObject* Best = nullptr;
				float BestDistSq = FLT_MAX;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					const float DistSq = O ? FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(float(CamGen.X), float(CamGen.Y))) : 0.f;
					if (O && !O->IsDestroyed() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase) && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Best = O;
					}
				}
				if (Best && GenPC)
				{
					const float R = FMath::Max(Best->GetTemplate()->GetBoundingCircleRadius(), 2.f);
					const FVector3f P = Best->GetPosition();
					FString Across, Up;
					int32 OnIt = 0;
					for (int32 i = -8; i <= 8; ++i)
					{
						const float Off = float(i) * R * 0.25f;
						const FGenObjectID A = GenPC->TestPickAt(FVector3f(P.X + Off, P.Y, P.Z));
						const FGenObjectID U = GenPC->TestPickAt(FVector3f(P.X, P.Y + Off, P.Z));
						Across += A == Best->GetID() ? TEXT("#") : (A != GenInvalidObjectID ? TEXT("o") : TEXT("."));
						Up += U == Best->GetID() ? TEXT("#") : (U != GenInvalidObjectID ? TEXT("o") : TEXT("."));
						OnIt += A == Best->GetID() ? 1 : 0;
					}
					const double Began = FPlatformTime::Seconds();
					for (int32 i = 0; i < 20; ++i)
					{
						GenPC->TestPickAt(FVector3f(P.X + float(i - 10) * R * 0.1f, P.Y, P.Z));
					}
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: pickscan #%u %s (radius %.1f, the middle nine are within it): across [%s] up [%s]; a pick takes %.0f microseconds among %d objects"),
						Logic->GetFrame(), Best->GetID(), *Best->GetTemplate()->Name, R, *Across, *Up, (FPlatformTime::Seconds() - Began) * 1.e6 / 20.0, Logic->GetObjectIDs().Num());
				}
				else
				{
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: pickscan %s: NONE THERE"), Logic->GetFrame(), *Arg);
				}
			}
		}
	}

	if (bTestAttackPending && Elapsed > 1.f && Logic)
	{
		bTestAttackPending = false;
		TArray<FGenObjectID> Units;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && O->GetOwner() == Logic->GetLocalPlayer() && O->CanMove() && O->HasWeapons())
			{
				Units.Add(Id);
			}
		}
		Logic->IssueAttack(Units, TestAttackTarget);
		UE_LOG(LogGenerals, Log, TEXT("Test attack: %d units -> object %u"), Units.Num(), TestAttackTarget);
	}

	// -GenTrace: log every player's mobile or armed objects once per second (movement / combat debugging).
	static double LastTrace = 0.0;
	if (Logic && FParse::Param(FCommandLine::Get(), TEXT("GenTrace")) && FPlatformTime::Seconds() - LastTrace >= 1.0)
	{
		LastTrace = FPlatformTime::Seconds();
		{
			// Economy: money, power, and what the local player's buildings are doing.
			const int32 Local = Logic->GetLocalPlayer();
			int32 Produced = 0, Consumed = 0;
			Logic->GetEnergy(Local, Produced, Consumed);
			const FGenPlayer& LocalPlayer = Logic->GetPlayers()[Local];
			UE_LOG(LogGenerals, Log, TEXT("  [econ f%u] money=%d power=%d/%d rank=%d skill=%d points=%d sciences=%d upgrades=%d"), Logic->GetFrame(), Logic->GetMoney(Local),
				Produced, Consumed, LocalPlayer.RankLevel, LocalPlayer.SkillPoints, LocalPlayer.SciencePurchasePoints, LocalPlayer.Sciences.Num(), LocalPlayer.UpgradesComplete.Num());
		TArray<FString> AILines;
		Logic->DescribeAI(AILines);
		for (const FString& Line : AILines)
		{
			UE_LOG(LogGenerals, Log, TEXT("  [ai f%u] %s"), Logic->GetFrame(), *Line);
		}
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (O && O->GetOwner() == Local && (O->IsUnderConstruction() || O->GetProductionQueue().Num() > 0
					|| (O->IsKindOf(TEXT("STRUCTURE")) && O->GetHealth() < O->GetMaxHealth() && !O->IsDead())))
				{
					UE_LOG(LogGenerals, Log, TEXT("  [econ f%u] #%u %s construction=%.0f%% hp=%.0f queue=%d first=%.0f frames"), Logic->GetFrame(), Id,
						*O->GetTemplate()->Name, O->GetConstructionPercent(), O->GetHealth(), O->GetProductionQueue().Num(),
						O->GetProductionQueue().Num() > 0 ? O->GetProductionQueue()[0].FramesDone : 0.f);
				}
			}
		}
		const bool bCombat = FParse::Param(FCommandLine::Get(), TEXT("GenTestAttack")) || TestAttackTarget != GenInvalidObjectID;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (!O || !O->CanMove() || (O->GetOwner() != Logic->GetLocalPlayer() && !(bCombat && O->GetOwner() != 0)))
			{
				continue;
			}
			FString Inside;
			if (O->GetVeterancyLevel() > 0 || O->GetExperience() > 0) { Inside = FString::Printf(TEXT(" vet=%d xp=%d"), O->GetVeterancyLevel(), O->GetExperience()); }
			if (O->GetAIMode() != EGenAIMode::None) { Inside += FString::Printf(TEXT(" mode=%d"), int32(O->GetAIMode())); }
			if (O->IsStealthed()) { Inside += O->IsDetected() ? TEXT(" stealthed+detected") : TEXT(" stealthed"); }
			if (O->IsContained()) { Inside += FString::Printf(TEXT(" in=#%u"), O->GetContainedBy()); }
			else if (O->GetContain()) { Inside += FString::Printf(TEXT(" riders=%d"), Logic->GetRiders(*O).Num()); }
			if (O->HasChassis())
			{
				const FGenObject::FChassisPose& C = O->GetDrawChassis();
				const FGenObject::FWheelInfo& W = O->GetDrawWheels();
				Inside += FString::Printf(TEXT(" body=%.1f/%.1f/%.1f wheels=%.1f %.1f/%.1f/%.1f/%.1f"), FMath::RadiansToDegrees(C.Pitch), FMath::RadiansToDegrees(C.Roll), C.Z,
					FMath::RadiansToDegrees(W.Angle), W.FrontLeft, W.FrontRight, W.RearLeft, W.RearRight);
			}
			const FString Jet = O->GetTemplate()->bJetAI ? FString::Printf(TEXT(" jet=%d%s ammo=%d speed=%.0f"), int32(O->GetJetState()),
				O->IsParked() ? TEXT(" parked") : TEXT(""), O->GetWeapon(0).AmmoInClip,
				FVector2f(O->GetVelocity().X, O->GetVelocity().Y).Size() * float(GenLogic::GetLogicFPS())) : FString();
			UE_LOG(LogGenerals, Log, TEXT("  [trace f%u] #%u p%d %s pos=(%.1f, %.1f) alt=%.0f angle=%.0f moving=%d blocked=%u hp=%.0f/%.0f target=%u turret=%.0f shots=%u boxes=%d supply=%d/%d%s%s%s%s w=%s|%s"),
				Logic->GetFrame(), O->GetID(), O->GetOwner(), *O->GetTemplate()->Name, O->GetPosition().X, O->GetPosition().Y,
				O->GetPosition().Z - Logic->GetGroundHeight(O->GetPosition().X, O->GetPosition().Y),
				FMath::RadiansToDegrees(O->GetOrientation()), O->IsMoving() ? 1 : 0, O->GetBlockedFrames(), O->GetHealth(), O->GetMaxHealth(), O->GetTargetID(),
				FMath::RadiansToDegrees(O->GetTurretAngle(0)), O->GetWeapon(0).FireCount + O->GetWeapon(1).FireCount, O->GetBoxes(),
				int32(O->GetSupplyState()), int32(O->GetDockStage()), *Jet, *Inside, O->IsDead() ? TEXT(" DEAD") : TEXT(""), O->IsDisabled() ? TEXT(" DISABLED") : TEXT(""),
				O->GetWeapon(0).Template ? *O->GetWeapon(0).Template->Name : TEXT("-"), O->GetWeapon(1).Template ? *O->GetWeapon(1).Template->Name : TEXT("-"));
		}
	}

	if (ShotPath.IsEmpty())
	{
		return;
	}
	static double LastReport = FPlatformTime::Seconds();
	static int32 Frames = 0;
	++Frames;
	if (FPlatformTime::Seconds() - LastReport >= 5.0)
	{
		const UGenFXManager* FXStats = UGenFXManager::Get(this);
		UE_LOG(LogGenerals, Log, TEXT("Perf: %.1f fps over last %.1fs (logic frame %u, %d particles in %d systems)"),
			Frames / (FPlatformTime::Seconds() - LastReport), FPlatformTime::Seconds() - LastReport, Logic ? Logic->GetFrame() : 0u,
			FXStats ? FXStats->GetParticleCount() : 0, FXStats ? FXStats->GetSystemCount() : 0);
		LastReport = FPlatformTime::Seconds();
		Frames = 0;
	}
	// With -GenPauseFrame the simulation freezes at that logic frame and the shot is taken there
	// (a few rendered frames later, so the frozen state is on screen); otherwise after the delay.
	bool bShotDue = Elapsed >= ShotDelay;
	if (ShotFrame != 0)
	{
		bShotDue = Elapsed >= 120.f;
		if (Logic && Logic->GetFrame() >= ShotFrame && ++FramesAtShotFrame >= 4)
		{
			bShotDue = true;
		}
	}
	if (ShotState == 0 && bShotDue)
	{
		if (Logic)
		{
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (O && O->GetOwner() == Logic->GetLocalPlayer())
				{
					UE_LOG(LogGenerals, Log, TEXT("  local object %s at (%.1f, %.1f) moving=%d"), *O->GetTemplate()->Name,
						O->GetPosition().X, O->GetPosition().Y, O->IsMoving() ? 1 : 0);
				}
			}
		}
		FScreenshotRequest::RequestScreenshot(ShotPath, false, false);
		UE_LOG(LogGenerals, Log, TEXT("Screenshot requested: %s"), *ShotPath);
		ShotState = 1;
		Elapsed = 0.f;
	}
	else if (ShotState == 1 && Elapsed >= 1.5f)
	{
		ShotState = 2;
		SaveAudioRecording();
		SaveReplay();
		FPlatformMisc::RequestExit(false, TEXT("GenShot complete"));
	}
}

void AGenGameMode::SaveAudioRecording()
{
	if (RecordAudioPath.IsEmpty())
	{
		return;
	}
	Audio::FMixerDevice* Mixer = FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(this);
	if (!Mixer)
	{
		return;
	}
	float NumChannels = 2.f, SampleRate = 48000.f;
	const Audio::FAlignedFloatBuffer& Buffer = Mixer->StopRecording(nullptr, NumChannels, SampleRate);
	// 16-bit PCM WAVE
	TArray<uint8> Out;
	auto U32 = [&Out](uint32 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 4); };
	auto U16 = [&Out](uint16 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 2); };
	const int32 Channels = FMath::Max(1, int32(NumChannels));
	const uint32 DataBytes = uint32(Buffer.Num()) * 2;
	Out.Reserve(44 + int32(DataBytes));
	Out.Append(reinterpret_cast<const uint8*>("RIFF"), 4); U32(36 + DataBytes);
	Out.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8); U32(16); U16(1); U16(uint16(Channels));
	U32(uint32(SampleRate)); U32(uint32(SampleRate) * uint32(Channels) * 2); U16(uint16(Channels * 2)); U16(16);
	Out.Append(reinterpret_cast<const uint8*>("data"), 4); U32(DataBytes);
	float Peak = 0.f;
	for (float Sample : Buffer)
	{
		Peak = FMath::Max(Peak, FMath::Abs(Sample));
		U16(uint16(int16(FMath::Clamp(FMath::RoundToInt32(Sample * 32767.f), -32768, 32767))));
	}
	const bool bSaved = FFileHelper::SaveArrayToFile(Out, *RecordAudioPath);
	UE_LOG(LogGenerals, Log, TEXT("Audio recording: %.1f s (of %.1f s of game time), %d channels, %d Hz, peak %.3f -> %s (%s)"), float(Buffer.Num()) / FMath::Max(1.f, float(Channels) * SampleRate),
		GetWorld()->GetRealTimeSeconds() - RecordStartSeconds, Channels, int32(SampleRate), Peak, *RecordAudioPath, bSaved ? TEXT("saved") : TEXT("NOT SAVED"));
	RecordAudioPath.Reset();
}
