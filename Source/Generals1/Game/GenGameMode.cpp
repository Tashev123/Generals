#include "Game/GenGameMode.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Game/GenHUD.h"
#include "Game/GenPlayerController.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"
#include "Engine/World.h"
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

void AGenGameMode::StartPlay()
{
	Super::StartPlay();

	FString MapName = UGameplayStatics::ParseOption(OptionsString, TEXT("GenMap"));
	if (MapName.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("GenMap="), MapName);
	}
	if (MapName.IsEmpty())
	{
		MapName = TEXT("Tournament Desert");
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
	if (!MapActor->LoadMapByName(MapName))
	{
		UE_LOG(LogGenerals, Error, TEXT("Failed to load map '%s'"), *MapName);
		return;
	}

	// Skirmish setup (until the shell's skirmish menu exists).
	FString LocalFaction = TEXT("FactionAmerica");
	FString EnemyFaction = TEXT("FactionChina");
	FParse::Value(FCommandLine::Get(), TEXT("GenFaction="), LocalFaction);
	FParse::Value(FCommandLine::Get(), TEXT("GenEnemy="), EnemyFaction);
	TArray<FGenSkirmishSlot> Slots;
	FGenSkirmishSlot& Me = Slots.AddDefaulted_GetRef();
	Me.Faction = LocalFaction;
	Me.bHuman = true;
	Me.StartPosition = 0;
	FGenSkirmishSlot& Enemy = Slots.AddDefaulted_GetRef();
	Enemy.Faction = EnemyFaction;
	Enemy.StartPosition = 1;

	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !Logic->StartSkirmish(MapActor, Slots))
	{
		return;
	}

	// Camera start: local player's start waypoint.
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Cam)
	{
		return;
	}
	FVector Start;
	if (!MapActor->GetWaypointLocation(TEXT("Player_1_Start"), Start))
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
			Cam->ResetView();
		}
		if (Parts.Num() >= 3)
		{
			Cam->AddYaw(FCString::Atof(*Parts[2]));
		}
	}
	Cam->SetTarget(Start);

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
			if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Names[i]))
			{
				const float X = float(Gen.X) + (float(i) - (Names.Num() - 1) * 0.5f) * 40.f;
				const float Y = float(Gen.Y);
				Logic->CreateObject(T, Logic->GetLocalPlayer(), FVector3f(X, Y, Logic->GetGroundHeight(X, Y)), FMath::DegreesToRadians(-90.f));
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
			if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Names[i]))
			{
				const float X = At.X + (float(i) - (Names.Num() - 1) * 0.5f) * 40.f;
				if (const FGenObject* O = Logic->CreateObject(T, EnemyPlayer, FVector3f(X, At.Y, Logic->GetGroundHeight(X, At.Y)), FMath::DegreesToRadians(-90.f)))
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

	// Until units can be produced, both sides get a battle group to play with. Scripted test runs
	// (which place their own units or only take a screenshot) and -GenNoTestUnits skip it.
	FString Scripted;
	const bool bScriptedRun = !ObjectList.IsEmpty() || !EnemyList.IsEmpty()
		|| FParse::Value(FCommandLine::Get(), TEXT("GenShot="), Scripted) || FParse::Value(FCommandLine::Get(), TEXT("GenTestFX="), Scripted)
		|| FParse::Value(FCommandLine::Get(), TEXT("GenModel="), Scripted) || FParse::Param(FCommandLine::Get(), TEXT("GenNoTestUnits"));
	if (!bScriptedRun || FParse::Param(FCommandLine::Get(), TEXT("GenTestUnits")))
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

	FParse::Value(FCommandLine::Get(), TEXT("GenShot="), ShotPath);
	FParse::Value(FCommandLine::Get(), TEXT("GenShotDelay="), ShotDelay);
	FParse::Value(FCommandLine::Get(), TEXT("GenPauseFrame="), ShotFrame);
	if (!ShotPath.IsEmpty())
	{
		if (AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC))
		{
			GenPC->SetGenInputEnabled(false);
		}
	}
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
		if (Logic->GetRelationship(Local, P.Index) == EGenRelationship::Enemies)
		{
			SpawnGroup(P.Index, 560.f, Facing + PI);
			break;
		}
	}
}

void AGenGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	RunTestAutomation(DeltaSeconds);
}

void AGenGameMode::RunTestAutomation(float DeltaSeconds)
{
	Elapsed += DeltaSeconds;
	UGenGameLogic* Logic = UGenGameLogic::Get(this);

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
		const bool bCombat = FParse::Param(FCommandLine::Get(), TEXT("GenTestAttack")) || TestAttackTarget != GenInvalidObjectID;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (!O || !O->CanMove() || (O->GetOwner() != Logic->GetLocalPlayer() && !(bCombat && O->GetOwner() != 0)))
			{
				continue;
			}
			UE_LOG(LogGenerals, Log, TEXT("  [trace f%u] #%u p%d %s pos=(%.1f, %.1f) angle=%.0f moving=%d blocked=%u hp=%.0f/%.0f target=%u turret=%.0f shots=%u%s"),
				Logic->GetFrame(), O->GetID(), O->GetOwner(), *O->GetTemplate()->Name, O->GetPosition().X, O->GetPosition().Y,
				FMath::RadiansToDegrees(O->GetOrientation()), O->IsMoving() ? 1 : 0, O->GetBlockedFrames(), O->GetHealth(), O->GetMaxHealth(), O->GetTargetID(),
				FMath::RadiansToDegrees(O->GetTurretAngle(0)), O->GetWeapon(0).FireCount + O->GetWeapon(1).FireCount, O->IsDead() ? TEXT(" DEAD") : TEXT(""));
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
		FPlatformMisc::RequestExit(false, TEXT("GenShot complete"));
	}
}
