#include "Game/GenGameMode.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Game/GenHUD.h"
#include "Game/GenPlayerController.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
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

	// The human starts with a battle group to play with (and so does a passive -GenNoAI enemy).
	// Scripted test runs (which place their own units or only take a screenshot) and -GenNoTestUnits skip it.
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
				bool bQueued = false;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					const FGenObject* O = Logic->FindObject(Id);
					const FGenCommandSet* Set = (O && O->GetOwner() == Local && O->GetTemplate()->bHasProduction && !O->IsUnderConstruction())
						? FGenCommandData::Get().FindSet(O->GetCommandSet()) : nullptr;
					for (int32 s = 0; Set && s < FGenCommandSet::NumSlots && !bQueued; ++s)
					{
						const FGenCommandButton* Btn = FGenCommandData::Get().FindButton(Set->Slots[s]);
						if (Btn && Btn->Command == EGenCommand::UnitBuild && Btn->Object.Equals(Unit, ESearchCase::IgnoreCase))
						{
							Logic->IssueQueueUnit(Id, Unit);
							bQueued = true;
						}
					}
					if (bQueued) { break; }
				}
				UE_LOG(LogGenerals, Log, TEXT("Test queue: %s %s"), *Unit, bQueued ? TEXT("queued") : TEXT("has no factory"));
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
	//        skill,Points       experience towards the next rank (as if that much had been destroyed)
	//        aiskill,Points     the same for every computer player
	//        science,Name       bought with general's points, like a click in the promotions panel
	//        power,Name[,x,y]   fired at x,y (or the camera target) from whichever building has it ready
	//        money,Amount       cash
	//        generals           opens the promotions panel (for a screenshot)
	//        click,Button       a left click on that command button, wherever the interface shows it
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
			const int32 Local = Logic->GetLocalPlayer();
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
			const FVector CamGen = Cam ? GenCoords::FromUE(Cam->GetTarget()) : FVector::ZeroVector;
			const FString& What = Parts[1];
			const FString Arg = Parts.Num() >= 3 ? Parts[2] : FString();
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
			else if (What.Equals(TEXT("skill"), ESearchCase::IgnoreCase))
			{
				Logic->AddSkillPoints(Local, FCString::Atoi(*Arg));
				const FGenPlayer& P = Logic->GetPlayers()[Local];
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: +%s skill points -> rank %d, %d skill points, %d general's points"), Logic->GetFrame(), *Arg,
					P.RankLevel, P.SkillPoints, P.SciencePurchasePoints);
			}
			else if (What.Equals(TEXT("aiskill"), ESearchCase::IgnoreCase))
			{
				// The same for every computer player (they then spend the points on their skill set).
				for (const FGenPlayer& P : Logic->GetPlayers())
				{
					if (Logic->GetAI(P.Index))
					{
						Logic->AddSkillPoints(P.Index, FCString::Atoi(*Arg));
					}
				}
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
			else if (What.Equals(TEXT("rally"), ESearchCase::IgnoreCase) || What.Equals(TEXT("damage"), ESearchCase::IgnoreCase)
				|| What.Equals(TEXT("repair"), ESearchCase::IgnoreCase) || What.Equals(TEXT("fix"), ESearchCase::IgnoreCase))
			{
				// These act on our first living object of the named type.
				FGenObject* Subject = nullptr;
				TArray<FGenObjectID> Dozers, Vehicles;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					FGenObject* O = Logic->FindObject(Id);
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
				else if (What.Equals(TEXT("damage"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
				{
					const float Keep = Subject->GetMaxHealth() * FCString::Atof(*Parts[3]) / 100.f;
					Subject->AttemptDamage(FMath::Max(0.f, Subject->GetHealth() - Keep), TEXT("UNRESISTABLE"), TEXT("NORMAL"), GenInvalidObjectID);
					UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: #%u %s damaged to %.0f / %.0f"), Logic->GetFrame(), Subject->GetID(), *Arg,
						Subject->GetHealth(), Subject->GetMaxHealth());
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
			else if (What.Equals(TEXT("enter"), ESearchCase::IgnoreCase) || What.Equals(TEXT("evacuate"), ESearchCase::IgnoreCase))
			{
				const bool bEnter = What.Equals(TEXT("enter"), ESearchCase::IgnoreCase);
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
					if (O && !O->IsDead() && O->GetContain() && O->GetTemplate()->Canonical()->Name.Equals(ContainerType, ESearchCase::IgnoreCase) && DistSq < BestDistSq)
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
						if (O && O->GetOwner() == Local && (Arg == TEXT("*") || O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
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
			else if (What.Equals(TEXT("destroy"), ESearchCase::IgnoreCase) && Parts.Num() >= 5)
			{
				const FVector2f Near(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]));
				FGenObject* Victim = nullptr;
				float BestDistSq = FLT_MAX;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					FGenObject* O = Logic->FindObject(Id);
					const float DistSq = O ? FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Near) : 0.f;
					if (O && !O->IsDead() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase) && DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						Victim = O;
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: destroy %s: %s"), Logic->GetFrame(), *Arg, Victim ? TEXT("done") : TEXT("none found"));
				if (Victim)
				{
					Victim->Kill();
				}
			}
			else if (What.Equals(TEXT("vet"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				int32 Count = 0;
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && !O->IsDead() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
					{
						O->SetVeterancyLevel(FCString::Atoi(*Parts[3]));
						++Count;
					}
				}
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d %s set to veterancy level %s"), Logic->GetFrame(), Count, *Arg, *Parts[3]);
			}
			else if (What.Equals(TEXT("money"), ESearchCase::IgnoreCase))
			{
				Logic->AddMoney(Local, FCString::Atoi(*Arg));
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
			else if (What.Equals(TEXT("target"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
			{
				AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
				const FVector2f At(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
				const bool bFired = GenPC && GenPC->TestFireAt(FVector3f(At.X, At.Y, Logic->GetGroundHeight(At.X, At.Y)));
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: target (%.0f, %.0f) %s"), Logic->GetFrame(), At.X, At.Y, bFired ? TEXT("fired") : TEXT("NOTHING BEING AIMED"));
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
			if (O->IsContained()) { Inside += FString::Printf(TEXT(" in=#%u"), O->GetContainedBy()); }
			else if (O->GetContain()) { Inside += FString::Printf(TEXT(" riders=%d"), Logic->GetRiders(*O).Num()); }
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
		FPlatformMisc::RequestExit(false, TEXT("GenShot complete"));
	}
}
