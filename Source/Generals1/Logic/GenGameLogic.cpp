#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "RenderTimer.h"
#include "RHIStats.h"
#include "DynamicRHI.h"
#include "Audio/GenAudioManager.h"
#include "Audio/GenAudioData.h"
#include "Client/GenDrawable.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "W3D/GenAssets.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Logic/GenSimThread.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"

namespace
{
	// -GenPerf: the engine's garbage collections, timed from outside.
	double GGCStart = 0.0, GGCMax = 0.0, GGCTotal = 0.0;
	int32 GGCRuns = 0;
	bool bGGCHooked = false;

	void HookGarbageCollection()
	{
		if (bGGCHooked)
		{
			return;
		}
		bGGCHooked = true;
		FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda([]() { GGCStart = FPlatformTime::Seconds(); });
		FCoreUObjectDelegates::GetPostGarbageCollect().AddLambda([]()
		{
			const double Took = FPlatformTime::Seconds() - GGCStart;
			GGCMax = FMath::Max(GGCMax, Took);
			GGCTotal += Took;
			++GGCRuns;
		});
	}
}

static TAutoConsoleVariable<float> CVarGenSelectFlashHold(
	TEXT("gen.SelectFlashHold"), 0.f,
	TEXT("Seconds the flash of something just selected stays at its brightest before it fades. 0: as the original (for taking a picture of it)."));

static TAutoConsoleVariable<int32> CVarGenPathCells(
	TEXT("gen.PathCellsPerStep"), 5000,
	TEXT("Path cells searched per logic step before further path requests wait for a later step (the original's PATHFIND_CELLS_PER_FRAME). 0: no limit."));

static TAutoConsoleVariable<float> CVarGenDrawBudget(
	TEXT("gen.DrawBudgetMs"), 0.7f,
	TEXT("How long the drawing pass may spend per frame on building models, and again on changing models' looks (ms). 0: as long as it takes."));

static TAutoConsoleVariable<int32> CVarGenBodyParticles(
	TEXT("gen.BodyParticles"), 1,
	TEXT("1: damaged and burning models carry fire and smoke on the bones named for it. 0: they don't - to measure what it costs."));

static TAutoConsoleVariable<int32> CVarGenSimThread(
	TEXT("gen.SimThread"), 1,
	TEXT("1: logic steps run on a thread of their own, beside the frames (at game speeds up to x2). 0: inside a frame, on the game thread."));

namespace
{
	/** Fallback player colors (approximating multiplayer.ini MultiplayerColor entries). */
	const FLinearColor SlotPalette[] = {
		FLinearColor(FColor(43, 110, 255)), FLinearColor(FColor(220, 20, 20)), FLinearColor(FColor(40, 200, 40)),
		FLinearColor(FColor(255, 140, 0)), FLinearColor(FColor(0, 200, 220)), FLinearColor(FColor(140, 60, 220)),
		FLinearColor(FColor(255, 110, 180)), FLinearColor(FColor(230, 200, 30)),
	};
}

UGenGameLogic* UGenGameLogic::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UGenGameLogic>() : nullptr;
}

bool UGenGameLogic::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UGenGameLogic::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UGenGameLogic::StartSimThread()
{
	if (SimThread || FParse::Param(FCommandLine::Get(), TEXT("GenNoSimThread")))
	{
		return;
	}
	Pathfinder.SetYield([](void* Context) { static_cast<UGenGameLogic*>(Context)->SimYield(TEXT("a path search")); }, this);
	Pathfinder.SetDetach([](void* Context) { return static_cast<UGenGameLogic*>(Context)->BeginDetachedWork(); },
		[](void* Context) { static_cast<UGenGameLogic*>(Context)->EndDetachedWork(); }, this);
	SimThread = new FGenSimThread(this);
	WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddUObject(this, &UGenGameLogic::OnWorldTickStart);
	// (The effects and the sound are torn down with the world, in no particular order: the step must be
	// out of them before any of that begins.)
	WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddWeakLambda(this, [this](UWorld* World)
	{
		if (World == GetWorld())
		{
			StopSimThread();
		}
	});
}

void UGenGameLogic::StopSimThread()
{
	if (SimThread)
	{
		SimThread->WaitForStep();
		if (bGateHeldForFrame)
		{
			bGateHeldForFrame = false;
			SimThread->LeaveGame();
		}
		delete SimThread;
		SimThread = nullptr;
	}
	Pathfinder.SetYield(nullptr, nullptr);
	Pathfinder.SetDetach(nullptr, nullptr, nullptr);
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
}

void UGenGameLogic::OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	// From here to the end of the logic's own tick the game thread's game code runs (the players'
	// input, the test hooks, what is drawn): it is in the logic for all of it.
	if (World == GetWorld() && SimThread && !bGateHeldForFrame)
	{
		SimThread->EnterGame();
		bGateHeldForFrame = true;
	}
}

UGenGameLogic::FGameScope::FGameScope(UGenGameLogic* InLogic)
	: Logic(InLogic && InLogic->SimThread ? InLogic : nullptr)
{
	if (Logic)
	{
		Logic->SimThread->EnterGame();
	}
}

UGenGameLogic::FGameScope::~FGameScope()
{
	if (Logic && Logic->SimThread)
	{
		Logic->SimThread->LeaveGame();
	}
}

void UGenGameLogic::SimYield(const TCHAR* Where)
{
	if (!SimThread)
	{
		return;
	}
	if (GenPerf::bEnabled)
	{
		const double Stretch = FPlatformTime::Seconds() - LastYieldTime;
		if (Stretch > LongestStretch)
		{
			LongestStretch = Stretch;
			LongestStretchWhere = Where;
			LongestStretchDetail = StretchDetail;
		}
		SimThread->Yield();
		LastYieldTime = FPlatformTime::Seconds();
		return;
	}
	SimThread->Yield();
}

bool UGenGameLogic::BeginDetachedWork()
{
	if (!SimThread)
	{
		return false;
	}
	if (!SimThread->BeginDetached())
	{
		return false;
	}
	// (As good as a safe point: the game thread isn't kept out while this lasts.)
	if (GenPerf::bEnabled)
	{
		const double Stretch = FPlatformTime::Seconds() - LastYieldTime;
		if (Stretch > LongestStretch)
		{
			LongestStretch = Stretch;
			LongestStretchWhere = TEXT("a path search");
			LongestStretchDetail = StretchDetail;
		}
	}
	return true;
}

void UGenGameLogic::EndDetachedWork()
{
	if (SimThread)
	{
		SimThread->EndDetached();
		LastYieldTime = FPlatformTime::Seconds();
	}
}

void UGenGameLogic::WaitForStep()
{
	if (SimThread)
	{
		SimThread->WaitForStep();
	}
}

void UGenGameLogic::RunStep()
{
	LastYieldTime = FPlatformTime::Seconds();
	{
		GEN_PERF_SCOPE("Step");
		StepLogic();
		CheckStep();
	}
	SimYield(TEXT("the end of the step (fog of war)"));
	CompletedFrame = Frame;
}

void UGenGameLogic::Deinitialize()
{
	StopSimThread();
	Objects.Reset();
	ObjectOrder.Reset();
	OwnedObjects.Reset();
	bOwnedDirty = true;
	Drawables.Reset();
	DrawStates.Reset();
	SoundOnly.Reset();
	PendingVisuals.Reset();
	PendingRemovals.Reset();
	PendingFireFX.Reset();
	PathQueue.Reset();
	PathQueueHead = 0;
	PooledModels.Reset();
	PoolByName.Reset();
	PoolFree.Reset();
	ClearGhosts();
	Shroud.Reset();
	RadarEvents.Reset();
	ShroudTexture = nullptr;
	ShroudMaterial = nullptr;
	DelayedDamage.Reset();
	FloatingTexts.Reset();
	AIPlayers.Reset();
	ObstacleOwners.Reset();
	PendingCommands.Reset();
	PendingLocal.Reset();
	Recording.Reset();
	Playback.Reset();
	MapActor = nullptr;
	Audio = nullptr;
	Super::Deinitialize();
}

TStatId UGenGameLogic::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGenGameLogic, STATGROUP_Tickables);
}

const FGenMapData* UGenGameLogic::GetMap() const
{
	return MapActor ? MapActor->GetMapData() : nullptr;
}

void UGenGameLogic::FlattenTerrain(const FGenThingTemplate& Template, const FVector3f& Pos, float Angle)
{
	// TerrainLogic::flattenTerrain
	FGenMapData* Map = MapActor ? MapActor->GetMapDataForLogic() : nullptr;
	if (!Map || Template.bGeometryIsSmall)
	{
		return;
	}
	const float Cell = GenCoords::MapXYFactor;
	const bool bBox = Template.IsBox();
	const float HalfX = Template.GeometryMajorRadius, HalfY = bBox ? Template.GeometryMinorRadius : Template.GeometryMajorRadius;
	const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);
	// The grid points the footprint covers (a box turned as the building is, or a circle).
	const float ReachX = bBox ? FMath::Abs(HalfX * C) + FMath::Abs(HalfY * S) : HalfX;
	const float ReachY = bBox ? FMath::Abs(HalfX * S) + FMath::Abs(HalfY * C) : HalfX;
	const int32 MinX = FMath::FloorToInt32((Pos.X - ReachX) / Cell), MaxX = FMath::FloorToInt32((Pos.X + ReachX) / Cell);
	const int32 MinY = FMath::FloorToInt32((Pos.Y - ReachY) / Cell), MaxY = FMath::FloorToInt32((Pos.Y + ReachY) / Cell);
	auto Covers = [&](int32 i, int32 j)
	{
		const float DX = float(i) * Cell - Pos.X, DY = float(j) * Cell - Pos.Y;
		return bBox ? (FMath::Abs(DX * C + DY * S) <= HalfX && FMath::Abs(-DX * S + DY * C) <= HalfY) : (DX * DX + DY * DY < HalfX * HalfX);
	};
	float Total = 0.f;
	int32 Samples = 0;
	for (int32 i = MinX; i <= MaxX; ++i)
	{
		for (int32 j = MinY; j <= MaxY; ++j)
		{
			if (Covers(i, j))
			{
				Total += Map->GetGroundHeight(float(i) * Cell, float(j) * Cell);
				++Samples;
			}
		}
	}
	if (Samples == 0)
	{
		return;
	}
	int32 Raw = FMath::FloorToInt32(0.5f + Total / float(Samples) / GenCoords::MapHeightScale);
	if (bBox)
	{
		// "Compare to the height at the building's origin, because setRawMapHeight will only lower, not raise."
		Raw = FMath::Min(Raw, FMath::FloorToInt32(Map->GetGroundHeight(Pos.X, Pos.Y) / GenCoords::MapHeightScale));
	}
	// Each of those points and the eight around it ("Added the corners so it does a whole 3X3 square").
	FIntRect Changed(MAX_int32, MAX_int32, MIN_int32, MIN_int32);
	for (int32 i = MinX; i <= MaxX; ++i)
	{
		for (int32 j = MinY; j <= MaxY; ++j)
		{
			if (!Covers(i, j))
			{
				continue;
			}
			for (int32 di = -1; di <= 1; ++di)
			{
				for (int32 dj = -1; dj <= 1; ++dj)
				{
					const int32 X = i + di + Map->Border, Y = j + dj + Map->Border;
					if (Map->LowerRawHeight(X, Y, Raw))
					{
						Changed.Min.X = FMath::Min(Changed.Min.X, X);
						Changed.Min.Y = FMath::Min(Changed.Min.Y, Y);
						Changed.Max.X = FMath::Max(Changed.Max.X, X);
						Changed.Max.Y = FMath::Max(Changed.Max.Y, Y);
					}
				}
			}
		}
	}
	if (Changed.Min.X <= Changed.Max.X)
	{
		// The cells those points are corners of, and their neighbours' (whose shading leans on them).
		PendingTerrain.Add(FIntRect(Changed.Min.X - 2, Changed.Min.Y - 2, Changed.Max.X + 1, Changed.Max.Y + 1));
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [build f%u] the ground for %s at (%.0f, %.0f) is levelled to %.1f"), Frame, *Template.Name, Pos.X, Pos.Y,
			float(Raw) * GenCoords::MapHeightScale);
	}
}

void UGenGameLogic::ApplyPendingTerrain()
{
	// (Game thread, in the logic: the step isn't levelling anything meanwhile.)
	if (!MapActor)
	{
		return;
	}
	for (const FIntRect& Cells : PendingTerrain)
	{
		MapActor->MarkTerrainDirty(Cells);
	}
	PendingTerrain.Reset();
	GEN_PERF_SCOPE("R.Terrain");
	MapActor->RebuildDirtyTerrain();
}

float UGenGameLogic::GetGroundHeight(float X, float Y) const
{
	const FGenMapData* Map = GetMap();
	return Map ? Map->GetGroundHeight(X, Y) : 0.f;
}

float UGenGameLogic::GetFlightSurfaceHeight(float X, float Y) const
{
	// Locomotor::getSurfaceHtAtPt: over water, the water is the surface.
	const FGenMapData* Map = GetMap();
	if (!Map)
	{
		return 0.f;
	}
	float WaterZ = 0.f;
	return Map->IsUnderwater(X, Y, &WaterZ) ? WaterZ : Map->GetGroundHeight(X, Y);
}

// ---- setup --------------------------------------------------------------------------------------

bool UGenGameLogic::StartSkirmish(AGenMapActor* InMapActor, const FGenMatchSettings& Settings, int32 InLocalPlayer)
{
	FString Why;
	if (!Settings.ApplyToSimulation(&Why))
	{
		UE_LOG(LogGenerals, Error, TEXT("StartSkirmish: %s"), *Why);
		return false;
	}
	MapActor = InMapActor;
	const FGenMapData* Map = GetMap();
	if (!Map)
	{
		UE_LOG(LogGenerals, Error, TEXT("StartSkirmish: no map loaded"));
		MapActor = nullptr;
		return false;
	}
	FGenGameData& GD = FGenGameData::Get();
	Match = Settings;
	const TArray<FGenSkirmishSlot>& Slots = Match.Slots;
	const int32 StartingCash = Match.StartingCash;
	bGameOver = false;
	GameOverFrame = 0;
	WinningPlayer = INDEX_NONE;

	Pathfinder.Build(*Map, MapActor->GetBridges());
	Partition.Init(Pathfinder.CellCenter(0, 0) - FVector2f(GenLogic::PathfindCellSize * 0.5f),
		Pathfinder.CellCenter(Pathfinder.GetWidth() - 1, Pathfinder.GetHeight() - 1) + FVector2f(GenLogic::PathfindCellSize * 0.5f));
	StepObjects.Reset();
	OwnedObjects.Reset();
	bOwnedDirty = true;
	bNight = Map->TimeOfDay == 4; // TIME_OF_DAY_NIGHT

	// The simulation's own random stream (the match's seed: the same for everybody playing it).
	Random.Initialize(Match.Seed);
	bPowersReady = Match.bPowersReady;
	bTrace = FParse::Param(FCommandLine::Get(), TEXT("GenTrace"));
	FParse::Value(FCommandLine::Get(), TEXT("GenPauseFrame="), PauseAtFrame);
	float Speed = 1.f;
	GenPerf::bEnabled = FParse::Param(FCommandLine::Get(), TEXT("GenPerf"));
	if (GenPerf::bEnabled)
	{
		HookGarbageCollection();
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("GenSpeed="), Speed))
	{
		SetTimeScale(Speed); // tests: fast-forward through build times and supply runs
	}

	FGenCommandData::Get().Initialize();
	FX = GetWorld()->GetSubsystem<UGenFXManager>();
	if (FX)
	{
		FX->Start(MapActor, this);
	}

	// Player 0: neutral (owns the map's civilian objects)
	FGenPlayer& Neutral = Players.AddDefaulted_GetRef();
	Neutral.Index = 0;
	Neutral.Name = TEXT("Neutral");
	Neutral.Template = GD.FindPlayerTemplate(TEXT("FactionCivilian"));
	Neutral.bNeutral = true;

	SpawnMapObjects();

	// Count available start positions (Player_N_Start waypoints).
	TArray<int32> FreeStarts;
	for (int32 i = 0; i < 8; ++i)
	{
		if (Map->FindWaypoint(FString::Printf(TEXT("Player_%d_Start"), i + 1)))
		{
			FreeStarts.Add(i);
		}
	}
	for (const FGenSkirmishSlot& S : Slots)
	{
		FreeStarts.Remove(S.StartPosition);
	}

	LocalPlayer = INDEX_NONE;
	for (int32 SlotIdx = 0; SlotIdx < Slots.Num(); ++SlotIdx)
	{
		const FGenSkirmishSlot& S = Slots[SlotIdx];
		FGenPlayer& P = Players.AddDefaulted_GetRef();
		P.Index = Players.Num() - 1;
		P.Template = GD.FindPlayerTemplate(S.Faction);
		P.Name = S.Name.IsEmpty() ? FString::Printf(TEXT("Player %d"), P.Index) : S.Name;
		P.Difficulty = S.Difficulty;
		P.bHuman = S.bHuman;
		P.Team = S.Team;
		P.StartPosition = S.StartPosition;
		if (P.StartPosition < 0 && FreeStarts.Num() > 0)
		{
			P.StartPosition = FreeStarts[0];
			FreeStarts.RemoveAt(0);
		}
		P.Color = S.Color.A > 0.f ? S.Color : SlotPalette[SlotIdx % UE_ARRAY_COUNT(SlotPalette)];
		P.Money = StartingCash;
		InitPlayerSciences(P);
		if (InLocalPlayer == INDEX_NONE ? (P.bHuman && LocalPlayer == INDEX_NONE) : P.Index == InLocalPlayer)
		{
			LocalPlayer = P.Index;
		}
		if (!P.Template)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Unknown faction '%s'"), *S.Faction);
			continue;
		}
		PlaceStartingObjects(P);
	}
	if (LocalPlayer == INDEX_NONE)
	{
		LocalPlayer = Players.Num() > 1 ? 1 : 0;
	}
	if (Match.RevealedPlayer == -2)
	{
		Match.RevealedPlayer = LocalPlayer; // (-GenNoShroud: whoever plays here)
	}
	InitShroud();
	Audio = GetWorld()->GetSubsystem<UGenAudioManager>();
	if (Audio)
	{
		Audio->Start(MapActor, this);
	}

	// (AIData.ini also says how groups of units move, whoever plays.)
	FGenAIData::Get().Initialize();

	// Computer players: every slot that isn't played by a human (-GenNoAI leaves them passive).
	if (!Match.bNoAI)
	{
		const EGenDifficulty Difficulty = Match.DefaultDifficulty == 0 ? EGenDifficulty::Easy
			: (Match.DefaultDifficulty == 2 ? EGenDifficulty::Hard : EGenDifficulty::Normal);
		for (FGenPlayer& P : Players)
		{
			if (!P.bHuman && !P.bNeutral && P.Template && P.StartPosition >= 0)
			{
				P.bSkirmishAI = true;
				const EGenDifficulty Own = P.Difficulty == 0 ? EGenDifficulty::Easy : (P.Difficulty == 1 ? EGenDifficulty::Normal
					: (P.Difficulty == 2 ? EGenDifficulty::Hard : Difficulty));
				AIPlayers.Add(MakeUnique<FGenSkirmishAI>(this, P.Index, Own));
			}
		}
		for (const TUniquePtr<FGenSkirmishAI>& AI : AIPlayers)
		{
			AI->NewMap();
		}
	}

	for (FGenObjectID Id : ObjectOrder)
	{
		Objects[Id]->SnapshotPrevious();
	}
	UpdateShroud();
	InitShroudView();
	Frame = CompletedFrame = PublishedFrame = 0;
	SimClock = 0.0;
	RenderDelay = 0.0;
	PublishFrame();
	UpdateDrawables();
	UE_LOG(LogGenerals, Log, TEXT("Skirmish started: %d players, %d objects, logic %d fps"), Players.Num(), ObjectOrder.Num(), GenLogic::GetLogicFPS());
	UE_LOG(LogGenerals, Log, TEXT("Match: %s; local player %d"), *Match.Describe(), LocalPlayer);
	// Everything from here on is recorded: the objects the start puts on the map, the orders, the checksums.
	Recording = MakeUnique<FGenReplay>();
	Recording->Date = FDateTime::Now().ToString();
	Recording->RecordedBy = LocalPlayer;
	Recording->Settings = Match;
	bSetupPhase = true;
	PlaybackNext = 0;
	PlaybackMismatchFrame = 0;
	// -GenStateDump=F[,F...]: the whole state written out after those frames (-GenDumpTag= names the run).
	DumpFrames.Reset();
	FString DumpSpec;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenStateDump="), DumpSpec, false))
	{
		TArray<FString> FrameTexts;
		DumpSpec.ParseIntoArray(FrameTexts, TEXT(","));
		for (const FString& F : FrameTexts)
		{
			DumpFrames.Add(uint32(FCString::Atoi(*F)));
		}
	}
	DumpTag = TEXT("run");
	FParse::Value(FCommandLine::Get(), TEXT("GenDumpTag="), DumpTag);
	QueuePreloads();
	StartSimThread();
	return true;
}

void UGenGameLogic::SpawnMapObjects()
{
	const FGenMapData* Map = GetMap();
	const FGenGameData& GD = FGenGameData::Get();
	for (const FGenMapObject& O : Map->Objects)
	{
		if (O.IsWaypoint() || (O.Flags & (FGenMapObject::FlagRoadFlags | FGenMapObject::FlagBridgeFlags)) != 0)
		{
			continue;
		}
		const FGenThingTemplate* T = GD.FindTemplate(O.TemplateName);
		if (!T)
		{
			continue;
		}
		const float Ground = Map->GetGroundHeight(O.Location.X, O.Location.Y);
		FGenObject* New = CreateObject(T, 0, FVector3f(O.Location.X, O.Location.Y, O.Location.Z + Ground), O.Angle);
		if (New)
		{
			// Object::updateObjValuesFromMapProperties: the ambient sound the map gives it, or takes away.
			New->bFromMap = true;
			if (O.Has(TEXT("objectSoundAmbient")))
			{
				New->bMapAmbientOverride = true;
				New->MapAmbientSound = O.GetString(TEXT("objectSoundAmbient"));
			}
			if (const FGenDictValue* Enabled = O.Properties.Find(TEXT("objectSoundAmbientEnabled")))
			{
				New->MapAmbientEnabled = Enabled->Bool ? 1 : 0;
			}
		}
	}
}

FString UGenGameLogic::GetAmbientSound(const FGenObject& O, const FString& TemplateSound) const
{
	if (!O.bFromMap)
	{
		return TemplateSound;
	}
	const FString Sound = O.bMapAmbientOverride ? O.MapAmbientSound : TemplateSound;
	if (Sound.IsEmpty() || O.MapAmbientEnabled == 0)
	{
		return FString();
	}
	if (O.MapAmbientEnabled < 0)
	{
		// "Decide if the sound should start enabled or not, since the map maker didn't record a preference."
		const FGenAudioEventInfo* Info = FGenAudioData::Get().Find(Sound);
		if (!Info || (Info->Control & GenAudioControl::Loop) == 0 || Info->LoopCount != 0) // isPermanentSound
		{
			return FString();
		}
	}
	return Sound;
}

FVector3f UGenGameLogic::FindPositionAround(const FVector3f& Center, float MinRadius, float MaxRadius, uint32 Surfaces) const
{
	// PartitionManager::findPositionAround: try rings between min and max radius.
	for (int32 Ring = 0; Ring < 6; ++Ring)
	{
		const float R = FMath::Lerp(MinRadius, MaxRadius, Ring / 5.f);
		for (int32 k = 0; k < 16; ++k)
		{
			const float A = -PI / 2.f + k * (2.f * PI / 16.f);
			const FVector2f P(Center.X + FMath::Cos(A) * R, Center.Y + FMath::Sin(A) * R);
			if (Pathfinder.IsPassable(P, Surfaces))
			{
				return FVector3f(P.X, P.Y, GetGroundHeight(P.X, P.Y));
			}
		}
	}
	return Center;
}

FVector3f UGenGameLogic::FindFreeGroundAround(const FVector3f& Center, float MaxRadius)
{
	// PartitionManager::findPositionAround / tryPosition: rings 5 apart from the centre outwards, each tried from
	// a random angle to one side and the other in turn, for passable dry ground (no cliff, no building) where a
	// small sphere (5) touches nothing.
	constexpr float RingSpacing = 5.f;
	constexpr float Clearance = 5.f;
	auto IsFree = [this](const FVector2f& P)
	{
		if (!Pathfinder.IsPassable(P, GenSurface::Ground))
		{
			return false;
		}
		bool bHit = false;
		Partition.ForEachInRange(P, Clearance, [&](const FGenObject& O)
		{
			if (bHit || O.IsDestroyed() || O.GetContainedBy() != GenInvalidObjectID)
			{
				return;
			}
			// (iteratePotentialCollisions in 2D: the sphere against the other's footprint.)
			const FGenThingTemplate* T = O.GetTemplate();
			const FVector2f D = P - FVector2f(O.GetPosition().X, O.GetPosition().Y);
			if (T->IsBox())
			{
				const FVector2f U = O.GetDirection2D();
				const float DX = FMath::Max(0.f, FMath::Abs(FVector2f::DotProduct(D, U)) - T->GeometryMajorRadius);
				const float DY = FMath::Max(0.f, FMath::Abs(FVector2f::DotProduct(D, FVector2f(-U.Y, U.X))) - T->GeometryMinorRadius);
				bHit = DX * DX + DY * DY < Clearance * Clearance;
			}
			else
			{
				bHit = D.Size() < T->GeometryMajorRadius + Clearance;
			}
		});
		return !bHit;
	};
	const float StartAngle = Random.FRandRange(0.f, 2.f * PI);
	for (float Dist = 0.f; Dist <= MaxRadius; Dist += RingSpacing)
	{
		const float Spacing = Dist == 0.f ? 2.f * PI : (RingSpacing / (Dist + 1.f)) * (2.f * PI / 6.f);
		const int32 Samples = FMath::CeilToInt32((2.f * PI / Spacing) / 2.f);
		for (int32 i = 0; i < Samples; ++i)
		{
			for (const float Sign : { 1.f, -1.f })
			{
				if (i == 0 && Sign < 0.f)
				{
					continue; // (the same spot)
				}
				const float A = StartAngle + Sign * Spacing * float(i);
				const FVector2f P(Center.X + Dist * FMath::Cos(A), Center.Y + Dist * FMath::Sin(A));
				if (IsFree(P))
				{
					return FVector3f(P.X, P.Y, GetGroundHeight(P.X, P.Y));
				}
			}
		}
	}
	return Center;
}

void UGenGameLogic::PlaceStartingObjects(const FGenPlayer& Player)
{
	// GameLogic.cpp placeNetworkBuildingsForPlayer
	const FGenMapData* Map = GetMap();
	const FGenGameData& GD = FGenGameData::Get();
	const FGenMapObject* Start = Map->FindWaypoint(FString::Printf(TEXT("Player_%d_Start"), Player.StartPosition + 1));
	if (!Start)
	{
		UE_LOG(LogGenerals, Warning, TEXT("No start waypoint for player %d"), Player.Index);
		return;
	}
	FVector3f Pos(Start->Location.X, Start->Location.Y, Map->GetGroundHeight(Start->Location.X, Start->Location.Y));

	const FGenThingTemplate* BuildingT = GD.FindTemplate(Player.Template->StartingBuilding);
	if (!BuildingT)
	{
		return;
	}
	FGenObject* ConYard = CreateObject(BuildingT, Player.Index, Pos, BuildingT->PlacementViewAngle);
	if (!ConYard)
	{
		return;
	}
	const float SphereR = BuildingT->GetBoundingSphereRadius();
	Pos.Y -= SphereR / 2.f;
	if (const FGenMapObject* Rally = Map->FindWaypoint(FString::Printf(TEXT("Player_%d_Rally"), Player.StartPosition + 1)))
	{
		Pos = FVector3f(Rally->Location.X, Rally->Location.Y, 0.f);
	}
	Pos.Z = GetGroundHeight(Pos.X, Pos.Y);

	for (const FString& UnitName : Player.Template->StartingUnits)
	{
		const FGenThingTemplate* UnitT = GD.FindTemplate(UnitName);
		if (!UnitT)
		{
			continue;
		}
		const FVector3f UnitPos = FindPositionAround(Pos, SphereR * 0.7f, SphereR * 1.3f, GenSurface::Ground);
		CreateObject(UnitT, Player.Index, UnitPos, UnitT->PlacementViewAngle);
	}
}

// ---- objects ------------------------------------------------------------------------------------

FGenObject* UGenGameLogic::CreateObject(const FGenThingTemplate* Template, int32 OwnerPlayer, const FVector3f& Position, float Angle)
{
	if (!Template)
	{
		return nullptr;
	}
	if (bSetupPhase && Recording)
	{
		// Put on the map by the start of the game besides the map's own and the players' (a test's units): a
		// replay puts it there again, before its first step. (What its making makes in turn is made again too.)
		bSetupPhase = false;
		FGenSetupObject& Setup = Recording->SetupObjects.AddDefaulted_GetRef();
		Setup.Template = Template->Name;
		Setup.Owner = OwnerPlayer;
		Setup.Pos = Position;
		Setup.Angle = Angle;
		ON_SCOPE_EXIT { bSetupPhase = true; };
		return CreateObject(Template, OwnerPlayer, Position, Angle);
	}
	// ThingFactory::newObject: a template that only lists build variations becomes one of them.
	if (Template->BuildVariations.Num() > 0)
	{
		const FString& Pick = Template->BuildVariations[Random.RandRange(0, Template->BuildVariations.Num() - 1)];
		if (const FGenThingTemplate* Variation = FGenGameData::Get().FindTemplate(Pick))
		{
			Template = Variation;
		}
	}
	// (The game thread changes the simulation only between two steps: WaitForStep.)
	ensureMsgf(!(SimThread && IsInGameThread() && SimThread->IsStepRunning()), TEXT("CreateObject on the game thread while a logic step is under way"));
	const FGenObjectID Id = NextID++;
	TUniquePtr<FGenObject> Obj = MakeUnique<FGenObject>(Id, Template, OwnerPlayer, this);
	Obj->SetPosition(Position);
	Obj->SetOrientation(Angle);
	Obj->SnapshotPrevious();
	Obj->SetModelCondition(GenConditions::Night(), bNight);
	FGenObject* Raw = Obj.Get();
	Objects.Add(Id, MoveTemp(Obj));
	ObjectOrder.Add(Id);
	if (!bOwnedDirty)
	{
		if (OwnedObjects.IsValidIndex(OwnerPlayer))
		{
			OwnedObjects[OwnerPlayer].Add(Raw);
		}
		else if (Players.IsValidIndex(OwnerPlayer))
		{
			bOwnedDirty = true;
		}
	}

	if (Template->ParticleCannon.bValid)
	{
		ParticleCannons.AddUnique(Id);
	}
	bAnyMines |= Template->Minefield.bValid;
	// classifyObjectFootprint: immobile, non-small structures block pathing.
	if (Template->FenceWidth > 0.f && !Template->IsKindOf(TEXT("DEFENSIVE_WALL")))
	{
		// "if (obj->getTemplate()->getFenceWidth() > 0.0f) ... classifyFence"
		Pathfinder.AddFence(*Raw);
		ObstacleOwners.Add(Id);
	}
	else if (Template->IsKindOf(TEXT("STRUCTURE")) && !Template->bGeometryIsSmall && !Raw->CanMove())
	{
		Pathfinder.AddObstacle(*Raw);
		ObstacleOwners.Add(Id);
	}

	// Object::Object: "if (controller->getNumBattlePlansActive() > 0) controller->applyBattlePlanBonusesForObject(this)".
	AddBattlePlanBonuses(*Raw, OwnerPlayer);

	// (Its model is made by the drawing pass: see CreatePendingVisuals.)
	PendingVisuals.Add(Id);
	return Raw;
}

UGenModelComponent* UGenGameLogic::TakePooledModel(const FString& ModelName)
{
	TArray<int32>* Indices = ModelName.IsEmpty() ? nullptr : PoolByName.Find(ModelName.ToLower());
	while (Indices && Indices->Num() > 0)
	{
		const int32 Index = Indices->Pop(EAllowShrinking::No);
		UGenModelComponent* Model = PooledModels.IsValidIndex(Index) ? PooledModels[Index].Get() : nullptr;
		if (PooledModels.IsValidIndex(Index))
		{
			PooledModels[Index] = nullptr;
			PoolFree.Add(Index);
		}
		if (Model && Model->GetModelName().Equals(ModelName, ESearchCase::IgnoreCase))
		{
			Model->SetHiddenInGame(false, true);
			return Model;
		}
		if (Model)
		{
			Model->DestroyComponent();
		}
	}
	return nullptr;
}

bool UGenGameLogic::PoolModel(UGenModelComponent* Model)
{
	constexpr int32 MaxPerModel = 24, MaxPooled = 600;
	if (!Model || Model->GetModelName().IsEmpty() || PooledModels.Num() - PoolFree.Num() >= MaxPooled)
	{
		return false;
	}
	TArray<int32>& Indices = PoolByName.FindOrAdd(Model->GetModelName().ToLower());
	if (Indices.Num() >= MaxPerModel)
	{
		return false;
	}
	Model->ResetForReuse();
	Model->SetHiddenInGame(true, true);
	const int32 Index = PoolFree.Num() > 0 ? PoolFree.Pop(EAllowShrinking::No) : PooledModels.AddDefaulted();
	PooledModels[Index] = Model;
	Indices.Add(Index);
	return true;
}

int32 UGenGameLogic::GetColorPlayer(const FGenObject& O) const
{
	// StealthUpdate::changeVisualDisguise: "neutrals and enemies will see this disguised unit as the team it's
	// disguised as"; "if it's on our team or our ally's team, then show its true colors".
	if (O.IsDisguised() && Players.IsValidIndex(O.GetDisguisePlayer()) && GetRelationship(LocalPlayer, O.GetOwner()) != EGenRelationship::Allies)
	{
		return O.GetDisguisePlayer();
	}
	return O.GetOwner();
}

void UGenGameLogic::CreatePendingVisuals()
{
	// What the step made since the last frame gets its models here, on the game thread.
	TArray<FGenObjectID> Made = MoveTemp(PendingVisuals);
	PendingVisuals.Reset();
	const double CreateStart = FPlatformTime::Seconds();
	for (int32 MadeIndex = 0; MadeIndex < Made.Num(); ++MadeIndex)
	{
		if (DrawBudgetSeconds > 0.0 && MadeIndex > 0 && FPlatformTime::Seconds() - CreateStart > DrawBudgetSeconds)
		{
			// Enough for one frame (a building is twenty or thirty parts to make and register): the rest
			// get their models in the next. A frame or two without one isn't seen; a late frame is.
			PendingVisuals.Append(Made.GetData() + MadeIndex, Made.Num() - MadeIndex);
			break;
		}
		const FGenObjectID Id = Made[MadeIndex];
		const FGenObject* O = FindObject(Id);
		if (!O || !MapActor || Drawables.Contains(Id))
		{
			continue; // (made and gone again in the same breath)
		}
		const FGenThingTemplate* Template = O->GetDrawTemplate(); // (a disguise is drawn as what it pretends to be)
		GenDrawable::NoteTemplate(*Template);
		const int32 OwnerPlayer = GetColorPlayer(*O);
		const bool bOwned = Players.IsValidIndex(OwnerPlayer) && !Players[OwnerPlayer].bNeutral;
		const FLinearColor Color = bOwned ? Players[OwnerPlayer].Color : FLinearColor(0.8f, 0.8f, 0.8f);
		UGenModelComponent* MC = nullptr;
		if (!O->GetDebrisModel().IsEmpty())
		{
			// W3DDebrisDraw::setModelName: a chunk of wreckage, in its owner's colour if the list says so.
			const FLinearColor DebrisColor = O->IsDebrisHouseColored() ? Color : FLinearColor(0.8f, 0.8f, 0.8f);
			MC = TakePooledModel(O->GetDebrisModel());
			if (MC)
			{
				MC->SetHouseColor(DebrisColor);
			}
			else
			{
				MC = MapActor->CreateModelVisual(O->GetDebrisModel(), DebrisColor);
			}
		}
		else
		{
			const FGenDrawModule* PrimaryDraw = Template->PrimaryDraw();
			MC = PrimaryDraw ? TakePooledModel(PrimaryDraw->DefaultModel()) : nullptr;
			if (MC)
			{
				MC->SetHouseColor(Color); // (its condition state is applied by the drawable update, as for a new one)
			}
			else
			{
				MC = MapActor->CreateObjectVisual(*Template, Color);
			}
		}
		if (!MC)
		{
			// (Nothing to draw - but a sound to make: W3DDefaultDraw objects like the map's Amb_... ones.)
			if (Audio && !GetAmbientSound(*O, Audio->GetTemplateSound(*O->GetTemplate(), TEXT("SoundAmbient"))).IsEmpty())
			{
				SoundOnly.Add(Id, FString());
			}
			continue;
		}
		const FVector3f Position = O->GetDrawPosition();
		MC->SetRelativeLocation(GenCoords::ToUE(Position.X, Position.Y, Position.Z));
		MC->SetRelativeRotation(FRotator(0.f, GenCoords::AngleToYaw(O->GetDrawOrientation()), 0.f));
		MC->SetManagedTick(true);
		Drawables.Add(Id, MC);
		FDrawState& DS = DrawStates.Add(Id, FDrawState()); // evaluated by the drawable update that follows
		DS.Object = O;
		DS.Model = MC;
		DS.DrawTemplate = Template;
		if (!O->GetDebrisModel().IsEmpty())
		{
			continue;
		}
		// Every other model draw module gets its own model on the same object; which model (if any)
		// it shows follows the object's condition flags, like the primary one.
		const FGenDrawModule* Primary = Template->PrimaryDraw();
		for (int32 i = 0; i < Template->Draws.Num(); ++i)
		{
			const FGenDrawModule& Draw = Template->Draws[i];
			if (&Draw == Primary || Draw.States.Num() == 0)
			{
				continue;
			}
			if (UGenModelComponent* Extra = MapActor->CreateAttachedVisual(MC, Color))
			{
				Extra->SetManagedTick(true);
				FExtraDraw& E = DS.Extras.AddDefaulted_GetRef();
				E.DrawIndex = i;
				E.Model = Extra;
			}
		}
	}
}

void UGenGameLogic::RemovePendingVisuals()
{
	TArray<FPendingRemoval> Gone = MoveTemp(PendingRemovals);
	PendingRemovals.Reset();
	for (const FPendingRemoval& R : Gone)
	{
		FDrawState* DS = DrawStates.Find(R.ID);
		TObjectPtr<UGenModelComponent>* D = Drawables.Find(R.ID);
		if (DS)
		{
			DestroyVehicleEffects(*DS);
		}
		if (D && *D && DS && !DS->bHidden && R.bFrozenInFog)
		{
			// Destroyed out of sight: for all the local player knows it still stands there. What they last
			// saw of it stays until somebody looks at the place again.
			FGhost& Ghost = Ghosts.AddDefaulted_GetRef();
			Ghost.Model = D->Get();
			for (const FExtraDraw& E : DS->Extras) { Ghost.Extras.Add(E.Model); }
			Ghost.Cells = R.Cells;
			Drawables.Remove(R.ID);
		}
		else
		{
			if (DS)
			{
				for (const FExtraDraw& E : DS->Extras)
				{
					if (UGenModelComponent* Extra = E.Model.Get()) { Extra->DestroyComponent(); }
				}
			}
			if (D)
			{
				// (Kept for the next thing with this model, if it is a plain one; else it goes.)
				if (*D && !((!DS || DS->Extras.Num() == 0) && PoolModel(D->Get()))) { (*D)->DestroyComponent(); }
				Drawables.Remove(R.ID);
			}
		}
		DrawStates.Remove(R.ID);
	}
}

FString UGenGameLogic::DescribeObjectModel(FGenObjectID Id) const
{
	const FDrawState* DS = DrawStates.Find(Id);
	const UGenModelComponent* MC = DS ? DS->Model : nullptr;
	return MC ? FString::Printf(TEXT("model %s anim %s frame %.1f of %d%s%s"), *MC->GetModelName(), *MC->GetAnimationName(), MC->GetAnimFrame(),
		MC->GetAnimNumFrames(), MC->IsAnimationPaused() ? TEXT(" PAUSED") : TEXT(""), DS->bHidden ? TEXT(" HIDDEN") : TEXT("")) : FString(TEXT("no model"));
}

void UGenGameLogic::DestroyObject(FGenObjectID ID)
{
	TUniquePtr<FGenObject>* Found = Objects.Find(ID);
	if (!Found)
	{
		return;
	}
	FGenObject& Obj = **Found;
	Obj.RemovePropagandaInfluence(); // PropagandaTowerBehavior::onDelete
	if (Obj.GetTemplate()->BattlePlan.bValid)
	{
		Obj.StopBattlePlan(); // BattlePlanUpdate::onDelete (a sold strategy center)
	}
	if (Obj.GetTemplate()->Spawner.bValid)
	{
		Obj.EndSpawns(true); // SpawnBehavior::onDelete
	}
	// (Removed in the middle of a step - a shrub under a new building: nobody may come across it any more.)
	Partition.Remove(&Obj);
	const int32 StepIndex = StepObjects.Find(&Obj);
	if (StepIndex != INDEX_NONE)
	{
		StepObjects[StepIndex] = nullptr;
	}
	if (ObstacleOwners.Remove(ID) > 0)
	{
		if (Obj.GetTemplate()->FenceWidth > 0.f && !Obj.GetTemplate()->IsKindOf(TEXT("DEFENSIVE_WALL"))) { Pathfinder.RemoveFence(Obj); } else
		Pathfinder.RemoveObstacle(Obj);
	}
	Pathfinder.RemoveUnit(ID);
	ForgetObjectLook(Obj); // what it was looking at fogs over again
	// Its model goes (or stays as a ghost in the fog) when the drawing pass gets to it: RemovePendingVisuals.
	{
		FPendingRemoval& Gone = PendingRemovals.AddDefaulted_GetRef();
		Gone.ID = ID;
		Gone.bFrozenInFog = IsFrozenInFog(Obj);
		if (Gone.bFrozenInFog)
		{
			TArray<int32, TInlineAllocator<16>> Cells;
			GetObjectCells(*Obj.GetTemplate(), Obj.GetPosition(), Obj.GetOrientation(), Cells);
			Gone.Cells.Append(Cells);
		}
	}
	if (FX)
	{
		FX->OnObjectDestroyed(ID); // attached particle systems (exhaust, trails) finish and go away
	}
	if (Audio)
	{
		Audio->OnObjectDestroyed(ID);
	}
	if (!bOwnedDirty && OwnedObjects.IsValidIndex(Obj.GetOwner()) && OwnedObjects[Obj.GetOwner()].RemoveSingle(&Obj) == 0)
	{
		bOwnedDirty = true; // (it should have been there: make the lists anew)
	}
	Objects.Remove(ID);
	ObjectOrder.Remove(ID);
}

void UGenGameLogic::OwnerChanged(FGenObject& Obj, int32 OldOwner)
{
	if (bOwnedDirty || OldOwner == Obj.GetOwner())
	{
		return;
	}
	if (OwnedObjects.IsValidIndex(OldOwner) && OwnedObjects[OldOwner].RemoveSingle(&Obj) == 0)
	{
		bOwnedDirty = true;
		return;
	}
	if (OwnedObjects.IsValidIndex(Obj.GetOwner()))
	{
		OwnedObjects[Obj.GetOwner()].Add(&Obj);
	}
}

const TArray<FGenObject*>& UGenGameLogic::GetObjectsOf(int32 Player) const
{
	static const TArray<FGenObject*> Nothing;
	if (bOwnedDirty)
	{
		bOwnedDirty = false;
		OwnedObjects.SetNum(Players.Num(), EAllowShrinking::No);
		for (TArray<FGenObject*>& List : OwnedObjects)
		{
			List.Reset();
		}
		for (FGenObjectID Id : ObjectOrder)
		{
			FGenObject* O = Objects[Id].Get();
			if (OwnedObjects.IsValidIndex(O->GetOwner()))
			{
				OwnedObjects[O->GetOwner()].Add(O);
			}
		}
	}
	return OwnedObjects.IsValidIndex(Player) ? OwnedObjects[Player] : Nothing;
}

FGenObject* UGenGameLogic::FindObject(FGenObjectID ID) const
{
	const TUniquePtr<FGenObject>* Found = Objects.Find(ID);
	return Found ? Found->Get() : nullptr;
}

EGenRelationship UGenGameLogic::GetRelationship(int32 A, int32 B) const
{
	if (A == B)
	{
		return EGenRelationship::Allies;
	}
	if (!Players.IsValidIndex(A) || !Players.IsValidIndex(B) || Players[A].bNeutral || Players[B].bNeutral)
	{
		return EGenRelationship::Neutral;
	}
	const int32 TA = Players[A].Team, TB = Players[B].Team;
	return (TA >= 0 && TA == TB) ? EGenRelationship::Allies : EGenRelationship::Enemies;
}

// ---- commands -----------------------------------------------------------------------------------

void UGenGameLogic::IssueMove(const TArray<FGenObjectID>& Units, const FVector3f& Goal)
{
	FGenCommand C(EGenCommandType::Move);
	C.Units = Units;
	C.Pos = Goal;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecMove(const FGenCommand& C)
{
	GroupMoveToPosition(C.Units, C.Pos, true);
}

void UGenGameLogic::IssueAddWaypoint(const TArray<FGenObjectID>& Units, const FVector3f& Pos)
{
	FGenCommand C(EGenCommandType::AddWaypoint);
	C.Units = Units;
	C.Pos = Pos;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecAddWaypoint(const FGenCommand& C)
{
	// GameLogicDispatch, MSG_ADD_WAYPOINT: groupMoveToPosition(&dest, true /*addWaypoint*/, CMD_FROM_PLAYER)
	GroupMoveToPosition(C.Units, C.Pos, true, true);
}

TArray<TPair<FGenObject*, FVector3f>> UGenGameLogic::SpreadGroup(const TArray<FGenObjectID>& Units, const FVector3f& Goal) const
{
	TArray<TPair<FGenObject*, FVector3f>> Out;
	float Spacing = GenLogic::PathfindCellSize;
	for (FGenObjectID Id : Units)
	{
		FGenObject* O = FindObject(Id);
		if (O && O->CanMove() && !O->IsDead() && !O->IsContained())
		{
			Out.Emplace(O, Goal);
			Spacing = FMath::Max(Spacing, O->GetTemplate()->GetBoundingCircleRadius() * 2.2f);
		}
	}
	const int32 Cols = FMath::CeilToInt32(FMath::Sqrt(float(Out.Num())));
	for (int32 i = 0; Out.Num() > 1 && i < Out.Num(); ++i)
	{
		Out[i].Value.X += (i % Cols - (Cols - 1) * 0.5f) * Spacing;
		Out[i].Value.Y += (i / Cols - (Cols - 1) * 0.5f) * Spacing;
	}
	return Out;
}

void UGenGameLogic::IssueAttackMove(const TArray<FGenObjectID>& Units, const FVector3f& Goal)
{
	FGenCommand C(EGenCommandType::AttackMove);
	C.Units = Units;
	C.Pos = Goal;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecAttackMove(const FGenCommand& C)
{
	for (const TPair<FGenObject*, FVector3f>& It : SpreadGroup(C.Units, C.Pos))
	{
		It.Key->StopConstructing();
		It.Key->OnPlayerCommand(false);
		It.Key->CommandAttackMove(It.Value);
		SimYield(TEXT("a group's order"));
	}
}

void UGenGameLogic::IssueGuard(const TArray<FGenObjectID>& Units, const FVector3f& Pos, FGenObjectID GuardObject, EGenGuardMode Mode)
{
	FGenCommand C(EGenCommandType::Guard);
	C.Units = Units;
	C.Pos = Pos;
	C.Target = GuardObject;
	C.A = int32(Mode);
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecGuard(const FGenCommand& C)
{
	for (const TPair<FGenObject*, FVector3f>& It : SpreadGroup(C.Units, C.Pos))
	{
		if (It.Key->GetID() == C.Target)
		{
			continue; // (nothing guards itself)
		}
		It.Key->StopConstructing();
		It.Key->OnPlayerCommand(false);
		It.Key->CommandGuardPosition(FVector2f(It.Value.X, It.Value.Y), EGenGuardMode(C.A), C.Target);
		SimYield(TEXT("a group's order"));
	}
}

void UGenGameLogic::IssueStop(const TArray<FGenObjectID>& Units)
{
	FGenCommand C(EGenCommandType::Stop);
	C.Units = Units;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecStop(const FGenCommand& C)
{
	for (FGenObjectID Id : C.Units)
	{
		if (FGenObject* O = FindObject(Id))
		{
			O->StopConstructing();
			O->OnPlayerCommand(false);
			O->CommandIdle();
			for (FGenObjectID SpawnId : O->GetSpawns()) // (SpawnBehavior::orderSlavesToGoIdle)
			{
				if (FGenObject* Spawn = FindObject(SpawnId))
				{
					Spawn->OnPlayerCommand(false);
					Spawn->CommandIdle();
				}
			}
		}
	}
}

void UGenGameLogic::IssueFireWeaponAt(const TArray<FGenObjectID>& Units, const FVector3f& Pos, int32 Slot, bool bMineClearing, int32 MaxShots)
{
	FGenCommand C(EGenCommandType::FireWeaponAt);
	C.Units = Units;
	C.Pos = Pos;
	C.A = Slot;
	C.B = MaxShots;
	C.bFlag = bMineClearing;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecFireWeaponAt(const FGenCommand& C)
{
	const int32 Slot = C.A;
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* O = FindObject(Id);
		if (!O || O->IsDead() || O->IsContained())
		{
			continue;
		}
		O->OnPlayerCommand(false);
		if (C.bFlag)
		{
			// (AIGroup::groupSetMineClearingDetail first: the tool comes out, then it is used.)
			O->CommandClearMinesAt(C.Pos, Slot);
		}
		else if (Slot >= 0 && Slot < 3 && O->GetWeapon(Slot).Template)
		{
			if (C.Pos.X == FLT_MAX)
			{
				O->CommandAttackPosition(O->GetPosition(), 1, Slot); // where it stands (a "detonate now")
			}
			else
			{
				O->CommandStop();
				O->CommandFireAtPosition(C.Pos, C.B, Slot); // (walked up to, aimed at, fired at)
			}
		}
	}
}

void UGenGameLogic::IssueForceAttack(const TArray<FGenObjectID>& Units, FGenObjectID Target)
{
	FGenCommand C(EGenCommandType::ForceAttack);
	C.Units = Units;
	C.Target = Target;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecForceAttack(const FGenCommand& C)
{
	// GameLogicDispatch, MSG_DO_FORCE_ATTACK_OBJECT: releaseWeaponLockForGroup(LOCKED_TEMPORARILY), then
	// groupForceAttackObject - riders allowed to fire included.
	const FGenObject* Victim = FindObject(C.Target);
	if (!Victim || !Victim->IsTargetable())
	{
		return;
	}
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* O = FindObject(Id);
		if (!O || O->IsDead() || Id == C.Target)
		{
			continue;
		}
		O->OnPlayerCommand(false);
		O->CommandStop();
		O->ClearAttack();
		O->CommandAttack(C.Target, true);
		if (O->GetContain())
		{
			OrderRidersToAttack(*O, C.Target, true);
		}
		SimYield(TEXT("a group's order"));
	}
}

void UGenGameLogic::IssueForceAttackGround(const TArray<FGenObjectID>& Units, const FVector3f& Pos)
{
	FGenCommand C(EGenCommandType::ForceAttackGround);
	C.Units = Units;
	C.Pos = Pos;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecForceAttackGround(const FGenCommand& C)
{
	// GameLogicDispatch, MSG_DO_FORCE_ATTACK_GROUND -> AIGroup::groupAttackPosition(pos, NO_MAX_SHOTS_LIMIT):
	// "this code allows garrisoned buildings to force attack a ground position" - the riders allowed to
	// fire are sent at it too.
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* O = FindObject(Id);
		if (!O || O->IsDead())
		{
			continue;
		}
		if (O->GetContain() && IsPassengerAllowedToFireFrom(*O))
		{
			TArray<FGenObjectID> Who = GetRiders(*O);
			if (O->GetTemplate()->Contain.bOverlord && &ResolveContainer(*O) == O && O->GetContain()->Portable != GenInvalidObjectID)
			{
				Who.Add(O->GetContain()->Portable); // (as OrderRidersToAttack: the add-on it carries)
			}
			for (FGenObjectID RiderId : Who)
			{
				FGenObject* R = FindObject(RiderId);
				const FGenObject* In = R ? FindObject(R->GetContainedBy()) : nullptr;
				if (R && In && !R->IsDead() && !R->IsDisabled() && R->HasWeapons() && IsPassengerAllowedToFire(*In, *R))
				{
					R->CommandFireAtPosition(C.Pos, MAX_int32, INDEX_NONE);
				}
			}
		}
		if (O->GetTemplate()->bJetAI || !O->HasWeapons())
		{
			continue; // (a plane's attack runs on a spot aren't in the port)
		}
		O->OnPlayerCommand(false);
		O->CommandStop();
		O->CommandFireAtPosition(C.Pos, MAX_int32, INDEX_NONE);
		SimYield(TEXT("a group's order"));
	}
}

void UGenGameLogic::IssueSwitchWeapon(const TArray<FGenObjectID>& Units, int32 Slot)
{
	FGenCommand C(EGenCommandType::SwitchWeapon);
	C.Units = Units;
	C.A = Slot;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecSwitchWeapon(const FGenCommand& C)
{
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* O = FindObject(Id);
		if (O && !O->IsDead() && O->SwitchWeapon(C.A))
		{
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [weapon f%u] #%u %s switches to %s"), Frame, Id, *O->GetTemplate()->Name,
				*O->GetWeapon(C.A).Template->Name);
		}
	}
}

void UGenGameLogic::IssueLand(const TArray<FGenObjectID>& Units, FGenObjectID Airfield)
{
	FGenCommand C(EGenCommandType::Land);
	C.Units = Units;
	C.Target = Airfield;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecLand(const FGenCommand& C)
{
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* O = FindObject(Id);
		if (O && O->bJetAI && !O->IsDead())
		{
			O->OnPlayerCommand(false);
			O->CommandLandAt(C.Target);
		}
	}
}

void UGenGameLogic::IssueAttack(const TArray<FGenObjectID>& Units, FGenObjectID Target)
{
	FGenCommand C(EGenCommandType::Attack);
	C.Units = Units;
	C.Target = Target;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecAttack(const FGenCommand& C)
{
	const FGenObject* Victim = FindObject(C.Target);
	if (!Victim || !Victim->IsTargetable())
	{
		return;
	}
	for (FGenObjectID Id : C.Units)
	{
		if (FGenObject* O = FindObject(Id))
		{
			O->OnPlayerCommand(false);
			O->CommandStop();
			O->CommandAttack(C.Target);
			if (O->GetContain())
			{
				OrderRidersToAttack(*O, C.Target);
			}
			// SPAWNS_ARE_THE_WEAPONS: SpawnBehavior::orderSlavesToAttackTarget.
			for (FGenObjectID SpawnId : O->GetSpawns())
			{
				FGenObject* Spawn = FindObject(SpawnId);
				if (Spawn && !Spawn->IsDead() && Spawn->HasWeaponFor(*Victim))
				{
					Spawn->OnPlayerCommand(false);
					Spawn->CommandAttack(C.Target);
				}
			}
			SimYield(TEXT("a group's order"));
		}
	}
}

// ---- combat services ----------------------------------------------------------------------------

void UGenGameLogic::DealDamage(const FGenWeaponTemplate& Weapon, FGenObjectID SourceId, FGenObjectID VictimId, const FVector3f& InPos, bool bProjectileDetonation)
{
	// WeaponTemplate::dealDamageInternal
	const FGenObject* Source = FindObject(SourceId);
	FGenObject* Primary = VictimId != GenInvalidObjectID ? FindObject(VictimId) : nullptr;
	FVector3f Pos = InPos;
	if (Primary && !bProjectileDetonation)
	{
		Pos = Primary->GetPosition();
	}

	// WeaponBonus: what the shooter's upgrades add to this weapon's damage.
	const float DamageBonus = Source ? Source->GetWeaponBonus(Weapon, int32(FGenWeaponBonus::Damage)) : 1.f;
	const float Radius = FMath::Max(Weapon.PrimaryDamageRadius, Weapon.SecondaryDamageRadius);
	if (Radius <= 0.f)
	{
		if (Primary)
		{
			Primary->AttemptDamage(Weapon.PrimaryDamage * DamageBonus, Weapon.DamageType, Weapon.DeathType, SourceId);
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [hit f%u] %s from #%u -> #%u %s hp=%.0f%s"), Frame, *Weapon.Name, SourceId, Primary->GetID(),
				*Primary->GetTemplate()->Name, Primary->GetHealth(), Primary->IsDead() ? TEXT(" DEAD") : TEXT(""));
		}
		return;
	}

	const uint32 Affects = Weapon.RadiusDamageAffects;
	// Only what stands near enough to be reached is looked at (a little wider than the blast: tall things
	// are measured by their bounding sphere), in the order the objects were made, as before. Damage can
	// kill objects, but never adds or removes any.
	TArray<FGenObject*, TInlineAllocator<64>> Near;
	Partition.ForEachInRange(FVector2f(Pos.X, Pos.Y), Radius + 64.f, [&Near](FGenObject& Other) { Near.Add(&Other); });
	Near.Sort([](const FGenObject& A, const FGenObject& B) { return A.GetID() < B.GetID(); });
	for (FGenObject* O : Near)
	{
		if (!O->IsTargetable())
		{
			continue;
		}
		// DAMAGE_RANGE_CALC_TYPE (FROM_BOUNDINGSPHERE_3D)
		const float Dist = FMath::Max(0.f, (O->GetPosition() - Pos).Size() - O->GetTemplate()->GetBoundingSphereRadius());
		if (Dist > Radius)
		{
			continue;
		}
		bool bKillSelf = false;
		// The weapon's direct target ignores the "affects" flags.
		if (Source && O != Primary)
		{
			if ((Affects & GenWeaponAffects::KillsSelf) != 0 && O == Source)
			{
				bKillSelf = true;
			}
			else
			{
				// (A bomb or missile doesn't hurt what launched it either.)
				if ((Affects & GenWeaponAffects::Self) == 0 && (O == Source || Source->Producer == O->GetID()))
				{
					continue;
				}
				if ((Affects & GenWeaponAffects::NotAirborne) != 0 && O->GetPosition().Z - O->GetSurfaceHeight() > 9.f)
				{
					continue; // isSignificantlyAboveTerrain
				}
				const EGenRelationship Rel = GetRelationship(O->GetOwner(), Source->GetOwner());
				if ((Affects & GenWeaponAffects::NotSimilar) != 0 && Rel == EGenRelationship::Allies && O->GetTemplate()->Canonical() == Source->GetTemplate()->Canonical())
				{
					continue;
				}
				const uint32 Required = Rel == EGenRelationship::Allies ? GenWeaponAffects::Allies
					: Rel == EGenRelationship::Enemies ? GenWeaponAffects::Enemies : GenWeaponAffects::Neutrals;
				if ((Affects & Required) == 0)
				{
					continue;
				}
			}
		}
		const float Amount = bKillSelf ? 1e9f : (Dist <= Weapon.PrimaryDamageRadius ? Weapon.PrimaryDamage : Weapon.SecondaryDamage) * DamageBonus;
		O->AttemptDamage(Amount, Weapon.DamageType, Weapon.DeathType, SourceId);
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [hit f%u] %s from #%u -> #%u %s hp=%.0f%s"), Frame, *Weapon.Name, SourceId, O->GetID(),
			*O->GetTemplate()->Name, O->GetHealth(), O->IsDead() ? TEXT(" DEAD") : TEXT(""));
	}
}

void UGenGameLogic::QueueDelayedDamage(const FGenWeaponTemplate& Weapon, FGenObjectID Source, FGenObjectID Victim, const FVector3f& Pos, uint32 WhenFrame)
{
	FDelayedDamage& D = DelayedDamage.AddDefaulted_GetRef();
	D.Weapon = &Weapon;
	D.Source = Source;
	D.Victim = Victim;
	D.Pos = Pos;
	D.Frame = WhenFrame;
}

FGenObject* UGenGameLogic::SpawnProjectile(const FGenWeaponTemplate& Weapon, const FGenObject& Shooter, FGenObjectID Victim, const FVector3f& TargetPos,
	const FVector3f& LaunchPos, float Angle, const FQuat4f* LaunchRotation)
{
	const FGenThingTemplate* ProjectileT = FGenGameData::Get().FindTemplate(Weapon.ProjectileObject);
	FGenObject* P = ProjectileT ? CreateObject(ProjectileT, Shooter.GetOwner(), LaunchPos, Angle) : nullptr;
	if (!P)
	{
		// Unknown projectile: fall back to an instant hit so the weapon still works.
		DealDamage(Weapon, Shooter.GetID(), Victim, TargetPos, false);
		return nullptr;
	}
	P->LaunchProjectile(&Weapon, Shooter.GetID(), Victim, TargetPos, LaunchRotation);
	// WeaponTemplate::fireWeaponTemplate: "If we're launching a missile at a unit with valid countermeasures,
	// then communicate it" - "Only allow jets not currently supersonic to launch countermeasures".
	if (ProjectileT->IsKindOf(TEXT("SMALL_MISSILE")))
	{
		FGenObject* V = FindObject(Victim);
		if (V && V->HasCountermeasures() && !V->bLocoSupersonic)
		{
			V->ReportMissileForCountermeasures(*P);
		}
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [fire f%u] #%u %s launches %s (#%u) at #%u"), Frame, Shooter.GetID(), *Weapon.Name,
		*ProjectileT->Name, P->GetID(), Victim);
	return P;
}

void UGenGameLogic::OnObjectDied(const FGenObject& Obj)
{
	// A wreck no longer blocks pathing.
	if (ObstacleOwners.Remove(Obj.GetID()) > 0)
	{
		if (Obj.GetTemplate()->FenceWidth > 0.f && !Obj.GetTemplate()->IsKindOf(TEXT("DEFENSIVE_WALL"))) { Pathfinder.RemoveFence(Obj); } else
		Pathfinder.RemoveObstacle(Obj);
	}
	// The dead hold no ground: their cells and their reserved destination are free again.
	Pathfinder.RemoveUnit(Obj.GetID());
}

FGenSkirmishAI* UGenGameLogic::GetAI(int32 Player) const
{
	for (const TUniquePtr<FGenSkirmishAI>& AI : AIPlayers)
	{
		if (AI->GetPlayer() == Player)
		{
			return AI.Get();
		}
	}
	return nullptr;
}

void UGenGameLogic::DescribeAI(TArray<FString>& OutLines) const
{
	for (const TUniquePtr<FGenSkirmishAI>& AI : AIPlayers)
	{
		OutLines.Add(AI->Describe());
	}
}

static TAutoConsoleVariable<int32> CVarGenCullTransforms(
	TEXT("gen.CullTransforms"), 1,
	TEXT("1: moving things only tell the renderer where they are while they are in or near the camera's view. 0: always."));

// ---- tick ---------------------------------------------------------------------------------------

void UGenGameLogic::Tick(float DeltaTime)
{
	if (!MapActor)
	{
		return;
	}
	// The game thread's turn in the logic (it has had it since the world's tick began, as a rule).
	if (SimThread && !bGateHeldForFrame)
	{
		SimThread->EnterGame();
		bGateHeldForFrame = true;
	}
	// A network game (Net/GenNet.h): a frame is worked out once the host has closed it. A game that has
	// fallen behind the host - after a hitch, or coming back into a match - hurries: a little faster for a
	// few frames, and more than two seconds behind, as many steps a frame as fit into a few milliseconds.
	const double FramesPerSecond = double(GenLogic::GetLogicFPS());
	double Pace = 1.0;
	bool bCatchingUp = false;
	if (OrderSource)
	{
		const uint32 Ready = OrderSource->GetReadyFrames();
		const double Behind = Ready > Frame ? double(Ready - Frame) : 0.0;
		bCatchingUp = Behind > FramesPerSecond * 2.0;
		Pace = 1.0 + FMath::Clamp((Behind - 2.0) / 6.0, 0.0, 3.0);
	}
	// Fast forward (a test hurrying through an hour of game) has no time to spare between steps: they
	// run here then, as many a frame as it takes, like they did before there was a thread for them.
	const bool bThreaded = SimThread && CVarGenSimThread.GetValueOnGameThread() != 0 && TimeScale <= 2.01f && !bCatchingUp;
	if (SimThread && !bThreaded)
	{
		SimThread->WaitForStep();
	}
	if (bCatchingUp)
	{
		CatchUp();
	}

	// Two clocks, both in logic frames. The step clock says when the next logic frame is due: frame N
	// when it reaches N. The screen's runs RenderDelay behind it and blends from frame N-1 to N while
	// it goes from N to N+1 - so a step has that long to get done before anybody needs to see it.
	{
		// As long as the slowest step lately took and a little more; changed gently, because a change
		// is a moment of everything moving a shade slower or faster.
		// (The usual, not the worst: a step that takes three times as long as the rest holds the screen
		// for the difference - things stand still for a few milliseconds - which is better than
		// everything running that much later all the time.)
		double Want = 0.0;
		if (bThreaded)
		{
			const double Last = SimThread->GetLastStepSeconds();
			StepPeakSeconds = FMath::Max(Last, StepPeakSeconds * FMath::Exp(-double(DeltaTime) / 4.0));
			if (StepStatsFrame != CompletedFrame && Last > 0.0)
			{
				StepStatsFrame = CompletedFrame;
				StepMeanSeconds += (Last - StepMeanSeconds) * (StepMeanSeconds > 0.0 ? 0.08 : 1.0);
				StepDevSeconds += (FMath::Abs(Last - StepMeanSeconds) - StepDevSeconds) * 0.08;
			}
			Want = FMath::Clamp((StepMeanSeconds + 2.0 * StepDevSeconds + 0.0015) * FramesPerSecond * double(TimeScale), 0.15, 0.6);
			if (RenderDelay <= 0.0)
			{
				RenderDelay = FMath::Max(Want, 0.25); // (the first threaded step: no telling yet)
			}
		}
		RenderDelay = FMath::Max(0.0, RenderDelay + FMath::Clamp(Want - RenderDelay, -0.15 * double(DeltaTime), 0.6 * double(DeltaTime)));
		if (!bThreaded && RenderDelay < 0.01)
		{
			RenderDelay = 0.0;
		}
	}
	SimClock += bUserPaused ? 0.0 : double(DeltaTime) * double(TimeScale) * FramesPerSecond * Pace;
	// Avoid a spiral of death after hitches: never more than a quarter second of logic to catch up on.
	const int32 MaxSteps = FMath::Max(1, GenLogic::GetLogicFPS() / 4);
	SimClock = FMath::Min(SimClock, double(PublishedFrame) + RenderDelay + double(MaxSteps));
	for (int32 Guard = 0; Guard < MaxSteps * 2; ++Guard)
	{
		if (CompletedFrame > PublishedFrame && SimClock - RenderDelay >= double(CompletedFrame))
		{
			PublishFrame();
			continue;
		}
		const bool bFrozen = PauseAtFrame != 0 && Frame >= PauseAtFrame; // (-GenPauseFrame: show exactly the state of this logic frame)
		const bool bRunning = SimThread && SimThread->IsStepRunning();
		const bool bOrdersHere = !OrderSource || OrderSource->IsFrameReady(Frame);
		if (!bRunning && CompletedFrame == PublishedFrame && Frame == CompletedFrame && !bFrozen && bOrdersHere && SimClock >= double(Frame + 1))
		{
			if (bThreaded)
			{
				SimThread->RequestStep(); // (it begins when this tick lets go of the logic)
				break;
			}
			RunStep();
			continue;
		}
		break;
	}
	// The screen doesn't run ahead of what has been published: a step that is late (or none to come)
	// holds it at the end of the blend, and the clocks with it.
	if (SimClock > double(PublishedFrame) + 1.0 + RenderDelay && !bUserPaused && !(PauseAtFrame != 0 && Frame >= PauseAtFrame))
	{
		++PerfHolds;
	}
	SimClock = FMath::Min(SimClock, double(PublishedFrame) + 1.0 + RenderDelay);
	Alpha = float(FMath::Clamp(SimClock - RenderDelay - double(PublishedFrame), 0.0, 1.0));
	UpdateShroudView(DeltaTime);
	{
		GEN_PERF_SCOPE("R.Drawables");
		DrawBudgetSeconds = double(CVarGenDrawBudget.GetValueOnGameThread()) / 1000.0;
		UpdateDrawables();
		DrawBudgetSeconds = 0.0;
	}
	if (FX)
	{
		PlayPendingFireFX();
		// Effects run on the simulation clock, so they pause and speed up with the game.
		FX->Update(GetRenderFrameTime());
	}
	if (Audio)
	{
		Audio->Update(DeltaTime); // sound runs in real time, whatever the game's speed
	}
	if (GenPerf::bEnabled)
	{
		ReportPerf(DeltaTime);
	}
	// The step's turn: until the next world tick begins (but for the HUD, which comes in between).
	if (SimThread && bGateHeldForFrame)
	{
		bGateHeldForFrame = false;
		SimThread->LeaveGame();
	}
	// What follows needs nothing of the logic, and so is done while the step gets on with its work:
	// the models' animation, and reading in what isn't loaded yet of the things that may turn up (a few
	// milliseconds' worth per frame, less when frames are already long).
	UGenModelComponent::TickManaged(DeltaTime);
	if (MapActor)
	{
		// (The camera has been moved for this frame by now.)
		if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
		{
			MapActor->UpdateSkyBox(PC->PlayerCameraManager->GetCameraLocation());
		}
	}
	if (UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get())
	{
		if (Assets->GetPreloadPending() > 0)
		{
			GEN_PERF_SCOPE("R.Preload");
			Assets->TickPreload(DeltaTime < 0.014f ? 0.003 : 0.001);
		}
	}
}

void UGenGameLogic::QueuePreloads()
{
	// GameClient::preloadAssets: everything marked PRELOAD that belongs to a side in this game (or to
	// none: the things all sides share). First what each thing looks like to begin with, then its other
	// states - the ones for the night only on a night map, the ones for snow not at all (no map's
	// weather asks for them yet).
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Assets)
	{
		return;
	}
	TSet<FString> Sides;
	for (const FGenPlayer& P : Players)
	{
		if (P.Template && !P.bNeutral)
		{
			Sides.Add(P.Template->Side.ToLower());
		}
	}
	TArray<const FGenThingTemplate*> Things;
	for (const TPair<FString, TSharedPtr<FGenThingTemplate>>& It : FGenGameData::Get().GetTemplates())
	{
		const FGenThingTemplate* T = It.Value.Get();
		if (T && T->IsKindOf(TEXT("PRELOAD")) && (T->Side.IsEmpty() || Sides.Contains(T->Side.ToLower())))
		{
			Things.Add(T);
		}
	}
	Things.Sort([](const FGenThingTemplate& A, const FGenThingTemplate& B) { return A.Name < B.Name; });
	const int32 Before = Assets->GetPreloadPending();
	const int32 NightBit = GenConditions::Night(), SnowBit = GenConditions::Snow();
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (const FGenThingTemplate* T : Things)
		{
			GenDrawable::NoteTemplate(*T); // (what of its models moves: before any of them is built)
			for (const FGenDrawModule& Draw : T->Draws)
			{
				for (int32 s = 0; s < Draw.States.Num(); ++s)
				{
					const FGenModelConditionState& State = Draw.States[s];
					if ((Pass == 0) != (State.bIsDefault || s == 0))
					{
						continue;
					}
					bool bWanted = State.ConditionSets.Num() == 0;
					for (const FGenConditionBits& Bits : State.ConditionSets)
					{
						bWanted |= !Bits.Test(SnowBit) && (bNight || !Bits.Test(NightBit));
					}
					if (!bWanted)
					{
						continue;
					}
					Assets->QueuePreloadModel(State.Model);
					for (const FString& Anim : State.Animations)
					{
						Assets->QueuePreloadAnimation(Anim);
					}
				}
				// What is shown on the way from one state to another (scaffolding going up, a soldier
				// getting to his feet): of the models this map's states show.
				if (Pass == 1 && Draw.Transitions.Num() > 0)
				{
					TSet<FString> Shown;
					for (const FGenModelConditionState& State : Draw.States)
					{
						bool bWanted = State.ConditionSets.Num() == 0;
						for (const FGenConditionBits& Bits : State.ConditionSets)
						{
							bWanted |= !Bits.Test(SnowBit) && (bNight || !Bits.Test(NightBit));
						}
						if (bWanted && !State.Model.IsEmpty())
						{
							Shown.Add(State.Model.ToLower());
						}
					}
					for (const TPair<FString, FGenModelConditionState>& Transition : Draw.Transitions)
					{
						if (Shown.Contains(Transition.Value.Model.ToLower()))
						{
							Assets->QueuePreloadModel(Transition.Value.Model);
							for (const FString& Anim : Transition.Value.Animations)
							{
								Assets->QueuePreloadAnimation(Anim);
							}
						}
					}
				}
			}
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Preload: %d things of %d sides, %d models and animations queued"), Things.Num(), Sides.Num(), Assets->GetPreloadPending() - Before);
}

void UGenGameLogic::ReportPerf(float DeltaTime)
{
	// -GenPerf: every 30 seconds of game time, how the game is keeping up and where the time went.
	const double Now = FPlatformTime::Seconds();
	++PerfFrames;
	PerfMaxFrame = FMath::Max(PerfMaxFrame, double(DeltaTime));
	// What the engine itself measured for the frame before this one (as "stat unit" shows it).
	PerfHitches33 += DeltaTime > 0.0334f ? 1 : 0;
	PerfHitches50 += DeltaTime > 0.050f ? 1 : 0;
	PerfHitches100 += DeltaTime > 0.100f ? 1 : 0;
	PerfOver42 += DeltaTime > 0.0042f ? 1 : 0;
	PerfOver5 += DeltaTime > 0.0050f ? 1 : 0;
	PerfOver83 += DeltaTime > 0.0084f ? 1 : 0;
	PerfGameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
	PerfRenderMs += FPlatformTime::ToMilliseconds(GRenderThreadTime);
	PerfGPUMs += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
	PerfDrawCalls += double(GNumDrawCallsRHI[0]);
	PerfTriangles += double(GNumPrimitivesDrawnRHI[0]);
	auto Clear = [this, Now]()
	{
		PerfStartReal = Now;
		PerfNextFrame = Frame + (TimeScale <= 1.f ? 300 : 900);
		PerfSteps = 0;
		PerfFrames = 0;
		PerfMaxFrame = 0.0;
		PerfHitches33 = PerfHitches50 = PerfHitches100 = 0;
		PerfOver42 = PerfOver5 = PerfOver83 = PerfHolds = 0;
		PerfGameMs = PerfRenderMs = PerfGPUMs = PerfDrawCalls = PerfTriangles = 0.0;
	};
	if (PerfStartReal == 0.0)
	{
		Clear();
		GenPerf::Reset();
		return;
	}
	if (Frame < PerfNextFrame)
	{
		return;
	}
	const double Real = FMath::Max(1.e-6, Now - PerfStartReal);
	int32 Structures = 0, Infantry = 0, Vehicles = 0, Aircraft = 0, Projectiles = 0, Dead = 0, Other = 0, Moving = 0;
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (O->IsDead()) { ++Dead; }
		else if (O->IsProjectile()) { ++Projectiles; }
		else if (O->IsKindOf(TEXT("STRUCTURE"))) { ++Structures; }
		else if (O->IsKindOf(TEXT("AIRCRAFT"))) { ++Aircraft; }
		else if (O->IsKindOf(TEXT("INFANTRY"))) { ++Infantry; }
		else if (O->IsKindOf(TEXT("VEHICLE"))) { ++Vehicles; }
		else { ++Other; }
		Moving += O->IsMoving() ? 1 : 0;
	}
	int32 Shown = 0;
	for (const auto& It : DrawStates)
	{
		Shown += It.Value.bHidden ? 0 : 1;
	}
	UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] %d steps in %.1f s (%.0f%% of the speed asked for), %d frames: %.1f fps, frame %.1f ms avg / %.0f max; objects %d (buildings %d, infantry %d, vehicles %d, aircraft %d, projectiles %d, dead %d, other %d; moving %d), drawables %d (%d shown), particles %d in %d systems"),
		Frame, PerfSteps, Real, 100.0 * double(PerfSteps) / double(GenLogic::GetLogicFPS()) / (Real * FMath::Max(0.01f, TimeScale)), PerfFrames,
		double(PerfFrames) / Real, 1000.0 * Real / FMath::Max(1, PerfFrames), 1000.0 * PerfMaxFrame, ObjectOrder.Num(), Structures, Infantry, Vehicles, Aircraft,
		Projectiles, Dead, Other, Moving, Drawables.Num(), Shown, FX ? FX->GetParticleCount() : 0, FX ? FX->GetSystemCount() : 0);
	const double F = double(FMath::Max(1, PerfFrames));
	UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] engine, per frame: game thread %.1f ms, render thread %.1f ms, GPU %.1f ms, %.0f draw calls, %.0fk triangles; frames over 33 ms: %d, over 50 ms: %d, over 100 ms: %d (speed x%.0f); memory %.0f MB, %d engine objects"),
		Frame, PerfGameMs / F, PerfRenderMs / F, PerfGPUMs / F, PerfDrawCalls / F, PerfTriangles / F / 1000.0, PerfHitches33, PerfHitches50, PerfHitches100, TimeScale,
		double(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0), GUObjectArray.GetObjectArrayNumMinusAvailable());
	{
		double GateTotal = 0.0, GateMax = 0.0;
		if (SimThread)
		{
			SimThread->TakeGateWait(GateTotal, GateMax);
		}
		UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] pacing: frames over 4.2 ms: %.1f%%, over 5 ms: %.1f%%, over 8.3 ms: %.1f%%; steps %s: up to %.1f ms from asked to done, the screen %.1f ms behind, held for a late step in %d frames; the game thread waited at the gate %.3f ms a frame (longest %.2f ms)"),
			Frame, 100.0 * PerfOver42 / F, 100.0 * PerfOver5 / F, 100.0 * PerfOver83 / F,
			SimThread && CVarGenSimThread.GetValueOnAnyThread() != 0 && TimeScale <= 2.01f ? TEXT("on their own thread") : TEXT("on the game thread"),
			StepPeakSeconds * 1000.0, RenderDelay * 1000.0 / (double(GenLogic::GetLogicFPS()) * FMath::Max(0.01, double(TimeScale))), PerfHolds,
			GateTotal * 1000.0 / F, GateMax * 1000.0);
		UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] the step's longest stretch without a safe point: %.2f ms, before %s (%s); the engine collected garbage %d times, %.1f ms at the longest"),
			Frame, LongestStretch * 1000.0, LongestStretchWhere, *LongestStretchDetail, GGCRuns, GGCMax * 1000.0);
		{
			int32 Parts = 0, PartsShown = 0, Groups = 0, Models = 0;
			TMap<FString, FIntVector> ByModel; // how many of the model, their parts, those that show
			for (const TPair<uint32, TObjectPtr<UGenModelComponent>>& It : Drawables)
			{
				if (It.Value && !It.Value->bHiddenInGame)
				{
					++Models;
					int32 P = 0, S = 0;
					It.Value->CountParts(P, S, Groups);
					Parts += P;
					PartsShown += S;
					FIntVector& M = ByModel.FindOrAdd(It.Value->GetModelName());
					M += FIntVector(1, P, S);
				}
			}
			UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] models shown: %d, of %d parts %d show; one part per bone would make them %d"), Frame, Models, Parts, PartsShown, Groups);
			ByModel.ValueSort([](const FIntVector& A, const FIntVector& B) { return A.Z > B.Z; });
			FString Most;
			int32 Listed = 0;
			for (const TPair<FString, FIntVector>& It : ByModel)
			{
				if (++Listed > 14) { break; }
				Most += FString::Printf(TEXT(" %s x%d: %d (%.1f each, of %.1f)"), *It.Key, It.Value.X, It.Value.Z, float(It.Value.Z) / float(It.Value.X), float(It.Value.Y) / float(It.Value.X));
			}
			UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] parts shown, the models with most:%s"), Frame, *Most);
		}
		LongestStretch = 0.0;
		GGCRuns = 0;
		GGCMax = GGCTotal = 0.0;
	}
	GenPerf::Report(*FString::Printf(TEXT("[perf f%u] logic, ms per step:"), Frame), TEXT("R."), false, double(FMath::Max(1, PerfSteps)));
	GenPerf::Report(*FString::Printf(TEXT("[perf f%u] drawing, ms per frame:"), Frame), TEXT("R."), true, F);
	Clear();
}

void UGenGameLogic::StepLogic()
{
	// The step's share of pathfinding starts afresh.
	Pathfinder.ResetCellsSearched();

	// The players' orders for this frame, at its start.
	ApplyCommands();

	// Objects that finished last frame are removed now, so the renderer got to interpolate their final step.
	for (int32 i = ObjectOrder.Num() - 1; i >= 0; --i)
	{
		const FGenObjectID Id = ObjectOrder[i];
		if (Objects[Id]->IsDestroyed())
		{
			DestroyObject(Id);
		}
	}

	++Frame;

	// Where everything is, for whoever asks what is near them during this step.
	StepObjects.Reset(ObjectOrder.Num());
	for (FGenObjectID Id : ObjectOrder)
	{
		StepObjects.Add(Objects[Id].Get());
	}
	// (Sorting the objects into the partition's cells reads where they are and writes only what is the
	// partition's own: the game thread needn't be kept out for it.)
	SimYield(TEXT("the start of the step (removals)"));
	{
		const bool bDetachedForIt = BeginDetachedWork();
		Partition.Rebuild(StepObjects);
		if (bDetachedForIt)
		{
			EndDetachedWork();
		}
	}

	// Units left waiting for a path by earlier steps come first (the orders just given have joined
	// the end of the queue, if there was one).
	if (PathQueueHead < PathQueue.Num())
	{
		ProcessPathQueue();
	}

	if (FallingRopes.Num() > 0)
	{
		UpdateFallingRopes();
	}
	// (The laser objects' LifetimeUpdate: a beam is gone when its time is up.)
	for (int32 i = WeaponBeams.Num() - 1; i >= 0; --i)
	{
		if (Frame >= WeaponBeams[i].ExpireFrame)
		{
			WeaponBeams.RemoveAtSwap(i);
		}
	}

	// Instant-hit weapons whose travel time has elapsed (WeaponStore::update).
	for (int32 i = 0; i < DelayedDamage.Num();)
	{
		if (Frame >= DelayedDamage[i].Frame)
		{
			const FDelayedDamage D = DelayedDamage[i];
			DelayedDamage.RemoveAtSwap(i);
			DealDamage(*D.Weapon, D.Source, D.Victim, D.Pos, false);
		}
		else
		{
			++i;
		}
	}

	// Computer players think (scripts, base building, teams) before their units act.
	for (const TUniquePtr<FGenSkirmishAI>& AI : AIPlayers)
	{
		if (!IsPlayerDefeated(AI->GetPlayer()))
		{
			AI->Update(Frame);
		}
		SimYield(TEXT("a computer player's teams"));
	}

	// Objects created during this pass (projectiles) start updating next frame.
	{
		GEN_PERF_SCOPE("Objects");
		for (int32 i = 0; i < StepObjects.Num(); ++i)
		{
			if (FGenObject* O = StepObjects[i])
			{
				O->Update(Frame);
			}
			SimYield(TEXT("an object's update"));
		}
	}

	// Collisions between units are found after everything has moved and acted on next frame.
	UpdateCollisions();
	UpdateChassis();
	UpdateVictory();
	SimYield(TEXT("collisions"));
	// Who sees what, now that everything has moved (and what the local player sees of each object).
	UpdateShroud();

	const uint32 FloatingTextFrames = uint32(GenLogic::GetLogicFPS() * 3);
	FloatingTexts.RemoveAll([this, FloatingTextFrames](const FGenFloatingText& F) { return Frame > F.Frame + FloatingTextFrames; });
}

void UGenGameLogic::PublishFrame()
{
	// The screen blends between the last two logic frames published - never towards the step that is
	// still being worked out.
	for (FGenObjectID Id : ObjectOrder)
	{
		Objects[Id]->PublishDraw();
	}
	PublishedFrame = CompletedFrame;
	++PerfSteps;
}

bool UGenGameLogic::ShouldQueuePath() const
{
	const int32 Budget = CVarGenPathCells.GetValueOnAnyThread();
	return Budget > 0 && !bProcessingPathQueue && (PathQueueHead < PathQueue.Num() || Pathfinder.GetCellsSearched() >= Budget);
}

void UGenGameLogic::QueueForPath(FGenObjectID Unit)
{
	PathQueue.Add(Unit);
	GEN_PERF_COUNT("Path.Queued#", 1);
}

void UGenGameLogic::ProcessPathQueue()
{
	// Pathfinder::processPathfindQueue: those who have waited longest first, for as long as the step's
	// share of cells lasts (and one in any case, so the queue always moves).
	const int32 Budget = FMath::Max(1, CVarGenPathCells.GetValueOnAnyThread());
	TGuardValue<bool> Guard(bProcessingPathQueue, true);
	int32 Done = 0;
	while (PathQueueHead < PathQueue.Num() && (Done == 0 || Pathfinder.GetCellsSearched() < Budget))
	{
		FGenObject* O = FindObject(PathQueue[PathQueueHead++]);
		if (O && O->DoPendingPath())
		{
			++Done;
		}
		SimYield(TEXT("the path queue"));
	}
	if (PathQueueHead >= PathQueue.Num())
	{
		PathQueue.Reset();
		PathQueueHead = 0;
	}
	else if (PathQueueHead > 256)
	{
		PathQueue.RemoveAt(0, PathQueueHead, EAllowShrinking::No);
		PathQueueHead = 0;
	}
}

FVector3f UGenGameLogic::GetRenderPosition(const FGenObject& Obj) const
{
	return FMath::Lerp(Obj.GetPrevPosition(), Obj.GetDrawPosition(), Alpha);
}

float UGenGameLogic::GetRenderOrientation(const FGenObject& Obj) const
{
	return Obj.GetPrevOrientation() + GenLogic::AngleDiff(Obj.GetDrawOrientation(), Obj.GetPrevOrientation()) * Alpha;
}

FTransform3f UGenGameLogic::GetRenderTransform(const FGenObject& Obj) const
{
	FQuat4f Q = FQuat4f::Slerp(Obj.GetPrevRotation(), Obj.GetDrawRotation(), Alpha);
	FVector3f P = GetRenderPosition(Obj);
	if (!(Obj.GetPrevChassis() == FGenObject::FChassisPose()) || !(Obj.GetDrawChassis() == FGenObject::FChassisPose()))
	{
		// Drawable::applyPhysicsXform: up by the lift, then pitch (Rotate_Y) and roll (Rotate_X(-roll)) in the body's own frame.
		const FGenObject::FChassisPose& A = Obj.GetPrevChassis();
		const FGenObject::FChassisPose& B = Obj.GetDrawChassis();
		P.Z += FMath::Lerp(A.Z, B.Z, Alpha);
		Q = Q * FQuat4f(FVector3f::RightVector, FMath::Lerp(A.Pitch, B.Pitch, Alpha)) * FQuat4f(FVector3f::ForwardVector, -FMath::Lerp(A.Roll, B.Roll, Alpha));
	}
	return FTransform3f(Q, P);
}

void UGenGameLogic::UpdateDrawables()
{
	// Ground the step has levelled for a building is levelled on screen; what the step made gets its
	// models, what it destroyed loses them.
	ApplyPendingTerrain();
	for (FGenObjectID Id : RemakeVisuals)
	{
		// A look of another template's (StealthUpdate::changeVisualDisguise: "get rid of the old instance!"):
		// the old models go, and new ones are made below.
		if (FDrawState* DS = DrawStates.Find(Id))
		{
			for (const FExtraDraw& E : DS->Extras)
			{
				if (UGenModelComponent* Extra = E.Model.Get()) { Extra->DestroyComponent(); }
			}
		}
		if (TObjectPtr<UGenModelComponent>* D = Drawables.Find(Id))
		{
			if (*D) { (*D)->DestroyComponent(); }
			Drawables.Remove(Id);
		}
		DrawStates.Remove(Id);
		PendingVisuals.AddUnique(Id);
	}
	RemakeVisuals.Reset();
	if (PendingVisuals.Num() > 0)
	{
		CreatePendingVisuals();
	}
	if (PendingRemovals.Num() > 0)
	{
		RemovePendingVisuals();
	}
	UpdateGhosts();
	// W3DModelDraw shows a muzzle flash only while its barrel is in the RECOIL_START state (about two logic
	// frames). Timed on the simulation clock so flashes follow the game speed and freeze when it is paused.
	const double MuzzleFlashSeconds = 2.0 / double(GenLogic::GetLogicFPS());
	const double FrameNow = GetRenderFrameTime();
	const double Now = FrameNow / double(GenLogic::GetLogicFPS());
	const float DeltaFrames = float(FMath::Clamp(FrameNow - LastDrawFrameTime, 0.0, 8.0));
	LastDrawFrameTime = FrameNow;

	// What the camera takes in, and a good way beyond (shadows reach in from outside): things that move
	// only tell the renderer where they are while they are in there. A model is some eight parts, each of
	// which the engine carries to its render thread when the model moves - for an army crossing the far
	// side of the map that was most of what a frame cost, to no purpose.
	bool bHaveView = false;
	FVector ViewPos = FVector::ZeroVector, ViewX = FVector::ForwardVector, ViewY = FVector::RightVector, ViewZ = FVector::UpVector;
	double ViewTanX = 1.0, ViewTanY = 1.0;
	FTransform MapXf = FTransform::Identity;
	if (const UWorld* World = GetWorld())
	{
		const APlayerController* PC = World->GetFirstPlayerController();
		if (PC && PC->PlayerCameraManager && MapActor)
		{
			const FRotationMatrix Rot(PC->PlayerCameraManager->GetCameraRotation());
			ViewPos = PC->PlayerCameraManager->GetCameraLocation();
			ViewX = Rot.GetUnitAxis(EAxis::X);
			ViewY = Rot.GetUnitAxis(EAxis::Y);
			ViewZ = Rot.GetUnitAxis(EAxis::Z);
			FVector2D ViewSize(16.0, 9.0);
			if (GEngine && GEngine->GameViewport)
			{
				GEngine->GameViewport->GetViewportSize(ViewSize);
			}
			ViewTanX = FMath::Tan(FMath::DegreesToRadians(double(PC->PlayerCameraManager->GetFOVAngle())) * 0.5);
			ViewTanY = ViewTanX * FMath::Max(1.0, ViewSize.Y) / FMath::Max(1.0, ViewSize.X);
			MapXf = MapActor->GetActorTransform();
			bHaveView = CVarGenCullTransforms.GetValueOnGameThread() != 0;
		}
	}
	auto NearView = [&](const FVector& RelPos, double Radius)
	{
		if (!bHaveView)
		{
			return true;
		}
		const FVector D = MapXf.TransformPosition(RelPos) - ViewPos;
		const double Margin = 2500.0 + Radius; // (cm)
		const double Depth = FVector::DotProduct(D, ViewX);
		return Depth > -Margin && FMath::Abs(FVector::DotProduct(D, ViewY)) <= FMath::Max(Depth, 0.0) * ViewTanX + Margin
			&& FMath::Abs(FVector::DotProduct(D, ViewZ)) <= FMath::Max(Depth, 0.0) * ViewTanY + Margin;
	};

	// W3DModelDraw recoil defaults, per 30 Hz frame (InitialRecoilSpeed, MaxRecoilDistance, RecoilDamping, RecoilSettleSpeed).
	constexpr float InitialRecoil = 2.f, MaxRecoil = 3.f, RecoilDamping = 0.4f, RecoilSettle = 0.065f, TinyRecoil = 0.01f;
	const float RecoilStepsPerFrame = 30.f / float(GenLogic::GetLogicFPS());

	// An object's look - shown or hidden, the state of its model, its sounds and colours - changes when
	// the logic changes something: thirty times a second at the most, and for most objects hardly ever.
	// So a quarter of them are looked over each frame, in turn (at 240 frames a second each still gets
	// looked at sixty times), and only what is in the middle of something the screen has to follow
	// frame by frame - a barrel recoiling, a stealthed unit's shimmer - every frame. Where things are
	// and which way their turrets point is every frame's business, for everything that moves.
	const uint32 Turn = uint32(GFrameCounter);
	int32 NumStateBusy = 0;
	double StateBudgetLeft = DrawBudgetSeconds > 0.0 ? DrawBudgetSeconds : 1.e9;
	for (TPair<uint32, FDrawState>& It : DrawStates)
	{
		FDrawState& DS = It.Value;
		const FGenObject* O = DS.Object;
		UGenModelComponent* MC = DS.Model;
		if (!O || !MC)
		{
			continue;
		}
		const bool bLook = !DS.bPlaced || DS.bBusy || DS.bStatePending || ((It.Key + Turn) & 3) == 0;
		if (!bLook && (DS.bFrozen || !(O->IsDynamic() || DS.bHasTurret || DS.bRider)))
		{
			continue;
		}
		if (bLook && DS.DrawTemplate && O->GetDrawTemplate() != DS.DrawTemplate)
		{
			RemakeVisuals.Add(It.Key); // (disguised, or the disguise dropped: made anew next frame)
			continue;
		}
		const FGenThingTemplate* DrawT = DS.DrawTemplate ? DS.DrawTemplate : O->GetTemplate();
		const FGenDrawModule* Draw = DrawT->PrimaryDraw();

		// Condition state (model / animation / sub-objects) follows the object's model condition flags.
		// What is inside a container isn't drawn, and neither is what the fog of war hides; a garrisoned
		// building wears its occupants' colour.
		const bool bHide = bLook ? (O->IsHidden() || !IsVisibleToLocalPlayer(*O)) : DS.bHidden;
		if (DS.bHidden != bHide)
		{
			DS.bHidden = bHide;
			// (Hidden-in-game rather than visibility: the model's sub-objects keep their own shown / hidden state.)
			MC->SetHiddenInGame(DS.bHidden, true);
		}
		if (O->HasDebrisAnims() && DS.DebrisAnimState < 2)
		{
			// W3DDebrisDraw::doDrawModule: its first animation once, then the one for flying over and over,
			// and when it is down the one for landing - with the effect of hitting the ground.
			const int32 Old = DS.DebrisAnimState;
			const FVector3f At = O->GetDrawPosition();
			if (Old < 0)
			{
				DS.DebrisAnimState = 0;
				DS.DebrisAnimFrame = PublishedFrame;
			}
			else if (PublishedFrame > DS.DebrisAnimFrame + 3 && At.Z <= GetGroundHeight(At.X, At.Y) + 0.01f)
			{
				DS.DebrisAnimState = 2;
			}
			else if (MC->IsAnimationComplete())
			{
				++DS.DebrisAnimState;
			}
			if (DS.DebrisAnimState != Old)
			{
				const bool bFinalStop = DS.DebrisAnimState == 2 && O->GetDebrisAnim(2).Equals(TEXT("STOP"), ESearchCase::IgnoreCase);
				const FString& Anim = bFinalStop ? O->GetDebrisAnim(1) : O->GetDebrisAnim(DS.DebrisAnimState);
				if (!Anim.IsEmpty())
				{
					if (DS.DebrisAnimState == 2 && FX && !O->GetDebrisFXFinal().IsEmpty())
					{
						const FTransform3f Xf = O->GetTransform();
						FX->DoFXPos(FGenFXData::Get().FindFXList(O->GetDebrisFXFinal()), At, &Xf, 0.f, nullptr, 0.f);
					}
					if (bFinalStop)
					{
						MC->SetAnimationPaused(true);
					}
					else
					{
						MC->PlayAnimation(Anim, DS.DebrisAnimState == 1 ? EGenAnimMode::Loop : EGenAnimMode::Once);
					}
				}
			}
		}
		// The sounds an object keeps making (AIUpdate's move sounds, Drawable::startAmbientSound): only what
		// the local player can see is heard.
		if (Audio && bLook)
		{
			if (!DS.bSoundsResolved)
			{
				DS.bSoundsResolved = true;
				const FGenThingTemplate& T = *O->GetTemplate();
				DS.MoveStartSound = Audio->GetTemplateSound(T, TEXT("SoundMoveStart"));
				DS.MoveLoopSound = Audio->GetTemplateSound(T, TEXT("SoundMoveLoop"));
				DS.MoveStartDamagedSound = Audio->GetTemplateSound(T, TEXT("SoundMoveStartDamaged"));
				DS.MoveLoopDamagedSound = Audio->GetTemplateSound(T, TEXT("SoundMoveLoopDamaged"));
				DS.TurretLoopSound = Audio->GetTemplateSound(T, TEXT("TurretMoveLoop"));
				DS.ConstructionSound = Audio->GetTemplateSound(T, TEXT("UnderConstruction"));
				DS.AmbientSound[0] = GetAmbientSound(*O, Audio->GetTemplateSound(T, TEXT("SoundAmbient")));
				DS.AmbientSound[1] = Audio->GetTemplateSound(T, TEXT("SoundAmbientDamaged"));
				DS.AmbientSound[2] = Audio->GetTemplateSound(T, TEXT("SoundAmbientReallyDamaged"));
				DS.AmbientSound[3] = Audio->GetTemplateSound(T, TEXT("SoundAmbientRubble"));
				DS.SoundDamageState = O->GetDamageState();
			}
			const bool bAudible = !bHide && !O->IsDead() && !(bShroudOn && O->GetLocalShroud() > EGenObjectShroud::PartialClear);
			const bool bMovingNow = bAudible && O->IsMoving();
			if (bMovingNow != DS.bSoundMoving)
			{
				DS.bSoundMoving = bMovingNow;
				// The sound of setting off, or - for what has none - the sound of being on the move; something
				// badly damaged has its own pair (and is silent without them).
				const bool bBadlyDamaged = O->GetDamageState() > 1;
				const FString& Start = bBadlyDamaged ? DS.MoveStartDamagedSound : DS.MoveStartSound;
				const FString& Loop = bBadlyDamaged ? DS.MoveLoopDamagedSound : DS.MoveLoopSound;
				if (bMovingNow && !Start.IsEmpty())
				{
					Audio->PlayForObject(Start, *O);
				}
				Audio->SetObjectLoop(It.Key, 0, bMovingNow && Start.IsEmpty() ? Loop : FString());
			}
			if (!DS.TurretLoopSound.IsEmpty())
			{
				// TurretAI: "if (m_playRotSound || m_playPitchSound) startRotOrPitchSound(); else stopRotOrPitchSound();"
				bool bTurning = false;
				for (int32 t = 0; t < 2 && bAudible; ++t)
				{
					bTurning |= !FMath::IsNearlyEqual(O->GetDrawTurretAngle(t), O->GetPrevTurretAngle(t), 1.e-4f)
						|| !FMath::IsNearlyEqual(O->GetDrawTurretPitch(t), O->GetPrevTurretPitch(t), 1.e-4f);
				}
				if (bTurning != DS.bSoundTurret)
				{
					DS.bSoundTurret = bTurning;
					Audio->SetObjectLoop(It.Key, 7, bTurning ? DS.TurretLoopSound : FString()); // (7: the turret)
					UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [audio f%u] #%u %s turret %s"), Frame, It.Key, *O->GetTemplate()->Name, bTurning ? TEXT("turning") : TEXT("still"));
				}
			}
			const int32 DamageState = O->GetDamageState();
			const int32 Ambient = (!bAudible || !O->AreEnginesOn()) ? -1 : (O->IsUnderConstruction() ? 4 : DamageState);
			if (Ambient != DS.SoundAmbient)
			{
				DS.SoundAmbient = Ambient;
				// A wreck has its own sound or none; a damaged thing sounds as it did whole unless it has a sound for that.
				const FString& Sound = Ambient < 0 ? FString() : (Ambient == 4 ? DS.ConstructionSound
					: (Ambient == 3 || !DS.AmbientSound[Ambient].IsEmpty() ? DS.AmbientSound[Ambient] : DS.AmbientSound[0]));
				Audio->SetObjectLoop(It.Key, 1, Sound);
			}
			if (DamageState != DS.SoundDamageState)
			{
				// SoundOnDamaged / SoundOnReallyDamaged: as it gets worse.
				if (bAudible && DamageState > DS.SoundDamageState && DamageState < 3)
				{
					PlayObjectSound(*O, { DamageState == 1 ? TEXT("SoundOnDamaged") : TEXT("SoundOnReallyDamaged") });
				}
				DS.SoundDamageState = DamageState;
			}
		}
		if (bLook)
		{
			DS.bFrozen = DS.bPlaced && !bHide && IsFrozenInFog(*O);
		}
		if (DS.bFrozen)
		{
			continue; // a building in the fog: it keeps the look it had when it was last seen
		}
		// Stealth (Drawable::setStealthLook): our own stealthed things are faint and pulse, a detected one glows.
		if (bLook)
		{
			float Fade = 0.f, Glow = 0.f;
			const EGenStealthLook Look = GetStealthLook(*O);
			// (StealthUpdate::update's "evil hack" for KINDOF_MINE: no pulsing and no heat-vision glow on
			// mines - whoever may see them sees them as they are.)
			if (Look != EGenStealthLook::None && Look != EGenStealthLook::Invisible && !O->IsKindOf(TEXT("MINE")))
			{
				// StealthUpdate::update: "0.5 + sin(phase) * 0.5", the phase moving 0.2 a frame.
				const float Pulse = 0.5f + 0.5f * FMath::Sin(float(FrameNow) * 0.2f * (30.f / float(GenLogic::GetLogicFPS())) + float(It.Key % 16));
				if (Look != EGenStealthLook::VisibleDetected)
				{
					// Drawable::setEffectiveOpacity: between the friendly opacity and solid.
					const float MinOpacity = O->GetTemplate()->Stealth.FriendlyOpacityMin;
					Fade = 1.f - (MinOpacity + (1.f - MinOpacity) * Pulse);
				}
				if (Look != EGenStealthLook::VisibleFriendly)
				{
					Glow = 0.25f + 0.35f * Pulse;
				}
			}
			// Drawable::flashAsSelected (a building being captured): a few frames of light. (In the colour
			// detected units glow in, not the capturer's - it is the same means.)
			if (O->GetFlashFrame() != 0 && FrameNow >= O->GetFlashFrame() && FrameNow - O->GetFlashFrame() < 5)
			{
				Glow = FMath::Max(Glow, 0.9f);
			}
			if (Fade != DS.StealthFade || Glow != DS.StealthGlow)
			{
				DS.StealthFade = Fade;
				DS.StealthGlow = Glow;
				MC->SetStealthLook(Fade, Glow);
				for (const FExtraDraw& E : DS.Extras)
				{
					if (UGenModelComponent* Extra = E.Model.Get()) { Extra->SetStealthLook(Fade, Glow); }
				}
			}
			// Drawable::flashAsSelected: "m_selectionFlashEnvelope->play( &tempColor, 0, 4 )" - white (GameData's
			// SelectionFlashHouseColor = No) at once, gone over four of the original's frames. RTS3DScene adds
			// the colour to the lights and to the ambient the model is drawn with.
			if (DS.FlashStart > 0.0)
			{
				const float Age = FMath::Max(0.f, float((FPlatformTime::Seconds() - DS.FlashStart - double(CVarGenSelectFlashHold.GetValueOnGameThread())) * 30.0));
				const float Flash = Age < 1.f ? 1.f : FMath::Max(0.f, 1.f - (Age - 1.f) / 4.f);
				if (Flash <= 0.f)
				{
					DS.FlashStart = 0.0;
				}
				if (Flash != DS.AppliedFlash)
				{
					DS.AppliedFlash = Flash;
					const FLinearColor Tint(Flash * 1.5f, Flash * 1.5f, Flash * 1.5f, 0.f);
					MC->SetTint(Tint);
					for (const FExtraDraw& E : DS.Extras)
					{
						if (UGenModelComponent* Extra = E.Model.Get()) { Extra->SetTint(Tint); }
					}
				}
			}
		}
		const int32 ColorPlayer = bLook ? GetColorPlayer(*O) : DS.Owner;
		if (bLook && DS.Owner != ColorPlayer)
		{
			if (DS.Owner != MIN_int32 && Players.IsValidIndex(ColorPlayer))
			{
				MC->SetHouseColor(Players[ColorPlayer].Color);
				for (const FExtraDraw& E : DS.Extras)
				{
					if (UGenModelComponent* Extra = E.Model.Get()) { Extra->SetHouseColor(Players[ColorPlayer].Color); }
				}
			}
			DS.Owner = ColorPlayer;
		}
		if (bLook && DS.AppliedVersion != O->GetConditionVersion() && StateBudgetLeft <= 0.0 && DS.bPlaced)
		{
			// (A new look may be a new model, which is as much work as a new object's: this frame has done
			// its share of that. First thing next frame.)
			DS.bStatePending = true;
		}
		else if (bLook && DS.AppliedVersion != O->GetConditionVersion())
		{
			const double StateStart = FPlatformTime::Seconds();
			DS.bStatePending = false;
			DS.AppliedVersion = O->GetConditionVersion();
			auto Show = [this, O](UGenModelComponent* Model, const FGenDrawModule& Module, FGenDrawInstance& Instance)
			{
				if (!Instance.Cur)
				{
					// A drawable begins in the state of no conditions at all (but the night's) and is told the
					// object's from there, as the original's is: so what is new comes up through its
					// transitions - a building site's scaffolding rises, it isn't just there.
					FGenConditionBits Plain;
					Plain.Set(GenConditions::Night(), bNight);
					const int32 First = Module.FindBestState(Plain);
					if (Module.States.IsValidIndex(First))
					{
						GenDrawable::SetModelState(Model, Module, Instance, &Module.States[First], bNight);
					}
				}
				const FGenModelConditionState* Was = Instance.Cur;
				const int32 Best = Module.FindBestState(O->GetModelConditions());
				if (Module.States.IsValidIndex(Best))
				{
					GenDrawable::SetModelState(Model, Module, Instance, &Module.States[Best], bNight);
				}
				return Instance.Cur != Was;
			};
			DS.bStateBusy = false;
			if (Draw)
			{
				if (Show(MC, *Draw, DS.State))
				{
					DS.AppliedTurret = 1e9f; // the model may have changed: re-apply the turret pose
				}
				DS.bStateBusy |= DS.State.IsBusy();
			}
			for (FExtraDraw& E : DS.Extras)
			{
				if (UGenModelComponent* Extra = E.Model.Get())
				{
					Show(Extra, DrawT->Draws[E.DrawIndex], E.State);
					DS.bStateBusy |= E.State.IsBusy();
				}
			}
			DS.SubObjectVersion = MAX_uint32; // the state was re-applied: upgrade overrides have to go on top again
			StateBudgetLeft -= FPlatformTime::Seconds() - StateStart;
		}
		// W3DModelDraw::doDrawModule: a transition that has played out gives way to the state behind it, an
		// animation that is to restart does (every frame, while there is one: it shows).
		if (DS.bStateBusy && !DS.bStatePending)
		{
			++NumStateBusy;
			bool bStillBusy = false, bChanged = false;
			if (Draw)
			{
				if (GenDrawable::UpdateModelState(MC, *Draw, DS.State, bNight))
				{
					bChanged = true;
					DS.AppliedTurret = 1e9f;
				}
				bStillBusy |= DS.State.IsBusy();
			}
			for (FExtraDraw& E : DS.Extras)
			{
				if (UGenModelComponent* Extra = E.Model.Get())
				{
					bChanged |= GenDrawable::UpdateModelState(Extra, DrawT->Draws[E.DrawIndex], E.State, bNight);
					bStillBusy |= E.State.IsBusy();
				}
			}
			DS.bStateBusy = bStillBusy;
			if (bChanged)
			{
				DS.SubObjectVersion = MAX_uint32;
			}
		}
		if (bLook)
		{
			// Drawable::setAnimationLoopDuration (a building being sold: its scaffolding goes up and comes
			// down in half the time), on whatever each module is playing now.
			if (DS.AnimDurationVersion != O->GetDrawAnimDurationVersion())
			{
				DS.AnimDurationVersion = O->GetDrawAnimDurationVersion();
				const float Seconds = float(O->GetDrawAnimDurationFrames()) / float(GenLogic::GetLogicFPS());
				MC->SetAnimDuration(Seconds);
				for (const FExtraDraw& E : DS.Extras)
				{
					if (UGenModelComponent* Extra = E.Model.Get()) { Extra->SetAnimDuration(Seconds); }
				}
			}
			// W3DModelDraw::setPauseAnimation: nothing moves on what an EMP or a hacker has shut down, nor -
			// where the module says its animations need power - on a building that hasn't any.
			// adjustTransformMtx: a state flagged ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT stands as deep in the
			// ground as the building is unfinished (or sold off).
			const bool bStopAll = !O->ShouldAnimate(false), bStopPowered = !O->ShouldAnimate(true);
			const float Percent = O->GetDrawConstructionPercent();
			const float Height = O->GetTemplate()->GetMaxHeightAbovePosition();
			auto Tend = [&](UGenModelComponent* Model, const FGenDrawModule& Module, FGenDrawInstance& Instance)
			{
				const bool bPause = Module.bAnimationsRequirePower ? bStopPowered : bStopAll;
				if (bPause != Instance.bPaused)
				{
					Instance.bPaused = bPause;
					Model->SetAnimationPaused(bPause);
				}
				const bool bAdjust = Instance.Cur && (Instance.Cur->Flags & GenStateFlags::AdjustHeightByConstructionPercent) != 0 && Percent >= 0.f;
				Instance.AdjustZ = bAdjust ? -Height + Height * FMath::Min(Percent, 100.f) / 100.f : 0.f;
			};
			if (Draw)
			{
				Tend(MC, *Draw, DS.State);
			}
			for (FExtraDraw& E : DS.Extras)
			{
				if (UGenModelComponent* Extra = E.Model.Get())
				{
					Tend(Extra, DrawT->Draws[E.DrawIndex], E.State);
					const float RelZ = E.State.AdjustZ - DS.State.AdjustZ;
					if (RelZ != E.AppliedRelZ)
					{
						E.AppliedRelZ = RelZ;
						Extra->SetRelativeLocation(FVector(0.0, 0.0, double(RelZ) * GenCoords::UnitsToCm));
					}
				}
			}
		}
		// SubObjectsUpgrade and payload on pylons: parts an upgrade shows or hides, whatever the state says.
		if (bLook && DS.SubObjectVersion != O->GetSubObjectVersion())
		{
			DS.SubObjectVersion = O->GetSubObjectVersion();
			for (const TPair<FString, bool>& Override : O->GetSubObjectOverrides())
			{
				MC->ShowSubObject(Override.Key, Override.Value);
				for (const FExtraDraw& E : DS.Extras)
				{
					if (UGenModelComponent* Extra = E.Model.Get()) { Extra->ShowSubObject(Override.Key, Override.Value); }
				}
			}
		}

		// W3DSupplyDraw: the boxes of a supply pile sit on bones "<prefix>01..NN"; as the stock runs down,
		// fewer of them show (the highest numbers go first).
		if (bLook && O->GetDock() && O->GetTemplate()->bSupplyWarehouse && !O->GetTemplate()->SupplyBonePrefix.IsEmpty())
		{
			const FString& Prefix = O->GetTemplate()->SupplyBonePrefix;
			UGenModelComponent* Boxes = Draw && Draw->Type.Equals(TEXT("W3DSupplyDraw"), ESearchCase::IgnoreCase) ? MC : nullptr;
			for (const FExtraDraw& E : DS.Extras)
			{
				if (!Boxes && DrawT->Draws[E.DrawIndex].Type.Equals(TEXT("W3DSupplyDraw"), ESearchCase::IgnoreCase))
				{
					Boxes = E.Model.Get();
				}
			}
			if (Boxes && DS.SupplyBones < 0)
			{
				TArray<FTransform3f> Bones;
				DS.SupplyBones = Boxes->GetNumberedBoneTransformsW3D(Prefix, Bones);
				DS.SupplyShown = DS.SupplyBones;
			}
			if (Boxes && DS.SupplyBones > 0)
			{
				const float Stock = float(O->GetBoxes()) / float(FMath::Max(1, O->GetTemplate()->StartingBoxes));
				const int32 Show = FMath::Clamp(FMath::CeilToInt32(float(DS.SupplyBones) * Stock), 0, DS.SupplyBones);
				for (int32 i = FMath::Min(Show, DS.SupplyShown); i < FMath::Max(Show, DS.SupplyShown); ++i)
				{
					Boxes->SetSubObjectsOnBonePrefixHidden(FString::Printf(TEXT("%s%02d"), *Prefix, i + 1), i >= Show);
				}
				DS.SupplyShown = Show;
			}
		}
		const FGenModelConditionState* State = Draw ? DS.State.Cur : nullptr;

		if (FX && bLook)
		{
			auto DestroySystems = [this](TArray<uint32>& Ids)
			{
				for (uint32 Id : Ids) { FX->DestroySystem(Id); }
				Ids.Reset();
			};
			auto Attach = [this, &It](const FString& System, const FTransform3f& BoneXf, TArray<uint32>& Ids)
			{
				if (FGenParticleSystem* Sys = FX->CreateAttachedSystem(System, It.Key))
				{
					Sys->SetLocalTransform(BoneXf);
					Ids.Add(Sys->GetID());
				}
			};

			// ParticleSysBone: particle systems (fire, smoke, exhaust) that sit on bones while this state shows.
			bool bNewStateSystems = false;
			if (DS.StateSystemsFor != State)
			{
				bNewStateSystems = true;
				DS.StateSystemsFor = State;
				DestroySystems(DS.StateSystems);
				if (State)
				{
					for (const TPair<FString, FString>& PB : State->ParticleBones)
					{
						FTransform3f BoneXf;
						if (MC->GetBoneTransformW3D(PB.Key, BoneXf))
						{
							Attach(PB.Value, BoneXf, DS.StateSystems);
						}
					}
				}
			}
			// ... and of the object's other draw modules (the sparks and smoke of scaffolding).
			for (FExtraDraw& E : DS.Extras)
			{
				if (E.StateSystemsFor == E.State.Cur)
				{
					continue;
				}
				E.StateSystemsFor = E.State.Cur;
				bNewStateSystems = true;
				DestroySystems(E.StateSystems);
				UGenModelComponent* Extra = E.Model.Get();
				for (int32 b = 0; Extra && E.State.Cur && b < E.State.Cur->ParticleBones.Num(); ++b)
				{
					const TPair<FString, FString>& PB = E.State.Cur->ParticleBones[b];
					FTransform3f BoneXf;
					if (Extra->GetBoneTransformW3D(PB.Key, BoneXf))
					{
						Attach(PB.Value, BoneXf, E.StateSystems);
					}
				}
			}

			// W3DModelDraw::doStartOrStopParticleSys: the bone systems of what is hidden - inside something, or
			// stealthed and not ours to see (the lights of their mines) - emit nothing.
			if (DS.bSystemsStopped != DS.bHidden || (bNewStateSystems && DS.bHidden))
			{
				DS.bSystemsStopped = DS.bHidden;
				for (uint32 Id : DS.StateSystems) { FX->SetSystemStopped(Id, DS.bHidden); }
				for (const FExtraDraw& E : DS.Extras)
				{
					for (uint32 Id : E.StateSystems) { FX->SetSystemStopped(Id, DS.bHidden); }
				}
			}

			// TransitionDamageFX particle systems: started when the damage state gets worse, and gone
			// again as soon as it changes (repair, or death).
			const int32 DamageState = O->GetDamageState();
			if (DamageState != DS.DamageState)
			{
				DestroySystems(DS.TransitionSystems);
				if (DamageState > DS.DamageState)
				{
					for (const FGenTransitionFX& X : O->GetTemplate()->TransitionFX)
					{
						if (X.State != DamageState || X.Kind != FGenTransitionFX::ParticleSystem)
						{
							continue;
						}
						FTransform3f BoneXf = FTransform3f::Identity;
						TArray<FTransform3f> Bones;
						if (!X.Bone.IsEmpty() && MC->GetNumberedBoneTransformsW3D(X.Bone, Bones) > 0)
						{
							BoneXf = Bones[X.bRandomBone ? FMath::RandRange(0, Bones.Num() - 1) : 0];
						}
						Attach(X.Name, BoneXf, DS.TransitionSystems);
					}
				}
				DS.DamageState = DamageState;
			}

			// ActiveBody::updateBodyParticleSystems: fire and smoke on the bones the model showing has for
			// them ("FireS01", "SmokeM02", "Aflame03" ...), made afresh when the damage state changes and when
			// the object catches fire or stops burning.
			const bool bAflame = O->IsAflame();
			const int32 BodyKey = DamageState | (bAflame ? 0x10 : 0);
			if (BodyKey != DS.BodySystemsKey)
			{
				DS.BodySystemsKey = BodyKey;
				DestroySystems(DS.BodySystems);
				const FString& BodyType = O->GetTemplate()->BodyType;
				if (!BodyType.IsEmpty() && !BodyType.Equals(TEXT("InactiveBody"), ESearchCase::IgnoreCase) && CVarGenBodyParticles.GetValueOnGameThread() != 0)
				{
					const FGenGameData& GD = FGenGameData::Get();
					// "when we're aflame, we use a slightly different set of particle systems ... that lends
					// itself to more fire and bigger fire": small fire becomes medium, medium and large are
					// large, all smoke becomes small fire, "we get to make more of them all too".
					static const int32 WhenAflame[6] = { 1, 2, 2, 0, 0, 0 };
					const int32 CountModifier = bAflame ? 2 : 1;
					for (int32 Kind = 0; Kind < (bAflame ? 7 : 6); ++Kind)
					{
						const FGenAutoParticles& AP = GD.AutoParticles[Kind];
						const FString& System = (bAflame && Kind < 6) ? GD.AutoParticles[WhenAflame[Kind]].System : AP.System;
						TArray<FTransform3f> Bones;
						if (AP.Prefix.IsEmpty() || System.IsEmpty() || MC->GetNumberedBoneTransformsW3D(AP.Prefix, Bones) == 0)
						{
							continue;
						}
						Bones.SetNum(FMath::Min(Bones.Num(), 16)); // MAX_BONES
						// "if there are more bones than maxSystems then the bones will be randomly selected"
						for (int32 n = FMath::Min(Bones.Num(), AP.Max * CountModifier); n > 0; --n)
						{
							const int32 Pick = FMath::RandRange(0, Bones.Num() - 1);
							Attach(System, Bones[Pick], DS.BodySystems);
							Bones.RemoveAtSwap(Pick);
						}
					}
				}
			}
		}

		if (State && (O->HasWeapons() || Draw->ProjectileBoneFeedbackSlots != 0))
		{
			// Turret yaw, interpolated between logic frames.
			if (!State->Turret.IsEmpty())
			{
				const float Prev = O->GetPrevTurretAngle(0);
				const float Angle = Prev + GenLogic::AngleDiff(O->GetDrawTurretAngle(0), Prev) * Alpha;
				if (FMath::Abs(Angle - DS.AppliedTurret) > 1e-4f)
				{
					DS.AppliedTurret = Angle;
					MC->SetBoneRotationOverride(State->Turret, FQuat(FVector::UpVector, double(Angle)));
				}
			}
			// ... and the barrel's pitch (W3DModelDraw::handleClientTurretPositioning: Rotate_Y(-turretPitch)).
			if (!State->TurretPitch.IsEmpty())
			{
				const float Pitch = FMath::Lerp(O->GetPrevTurretPitch(0), O->GetDrawTurretPitch(0), Alpha);
				if (FMath::Abs(Pitch - DS.AppliedTurretPitch) > 1e-4f)
				{
					DS.AppliedTurretPitch = Pitch;
					MC->SetBoneRotationOverride(State->TurretPitch, FQuat(FVector::RightVector, double(-Pitch)));
				}
			}
			// W3DModelDraw::updateProjectileClipStatus: "Loop through the projectile bones, and start hiding
			// off the front" - a launcher's missiles are gone from it as they are fired.
			if (Draw->ProjectileBoneFeedbackSlots != 0)
			{
				for (int32 Slot = 0; Slot < 3; ++Slot)
				{
					const int32 ClipSize = O->GetWeapon(Slot).Template ? O->GetWeapon(Slot).Template->ClipSize : 0;
					if ((Draw->ProjectileBoneFeedbackSlots & (1 << Slot)) == 0 || State->LaunchBone[Slot].IsEmpty() || ClipSize < 2)
					{
						continue;
					}
					const int32 Spent = O->GetClipSpent(Slot);
					if (Spent != DS.ClipSpent[Slot] || State != DS.ClipSpentState)
					{
						DS.ClipSpent[Slot] = Spent;
						for (int32 i = 0; i < ClipSize; ++i)
						{
							MC->ShowSubObject(State->LaunchBone[Slot] + FString::Printf(TEXT("%02d"), i + 1), i >= Spent);
						}
					}
				}
				DS.ClipSpentState = State;
			}

			for (int32 Slot = 0; bLook && Slot < 3; ++Slot)
			{
				const bool bFired = O->GetWeapon(Slot).FireCount != DS.SeenFireCount[Slot];
				DS.SeenFireCount[Slot] = O->GetWeapon(Slot).FireCount;

				// Muzzle flashes: shown briefly whenever the slot's weapon fires.
				const bool bExpired = DS.MuzzleHideTime[Slot] > 0.0 && Now >= DS.MuzzleHideTime[Slot];
				if (bFired || bExpired)
				{
					DS.MuzzleHideTime[Slot] = bFired ? Now + MuzzleFlashSeconds : 0.0;
					for (int32 f = 0; f < State->MuzzleFlashBones.Num(); ++f)
					{
						if (State->MuzzleFlashSlots.IsValidIndex(f) && State->MuzzleFlashSlots[f] == Slot)
						{
							MC->SetSubObjectsOnBonePrefixHidden(State->MuzzleFlashBones[f], !bFired);
						}
					}
				}

				// Barrel recoil (W3DModelDraw::handleClientRecoil): kick back fast, settle slowly. The original
				// steps once per 30 Hz frame; the steps are replayed here and blended for smooth motion at any fps.
				if (bFired)
				{
					DS.RecoilState[Slot] = 1;
					DS.RecoilRate[Slot] = InitialRecoil;
					DS.RecoilClock[Slot] = 0.f;
				}
				if (DS.RecoilState[Slot] == 0 || !State->RecoilSlots.Contains(Slot))
				{
					continue;
				}
				DS.RecoilClock[Slot] += DeltaFrames * RecoilStepsPerFrame;
				while (DS.RecoilClock[Slot] >= 1.f && DS.RecoilState[Slot] != 0)
				{
					DS.RecoilClock[Slot] -= 1.f;
					DS.RecoilPrevShift[Slot] = DS.RecoilShift[Slot];
					if (DS.RecoilState[Slot] == 1)
					{
						DS.RecoilShift[Slot] += DS.RecoilRate[Slot];
						DS.RecoilRate[Slot] *= RecoilDamping;
						if (DS.RecoilShift[Slot] >= MaxRecoil)
						{
							DS.RecoilShift[Slot] = MaxRecoil;
							DS.RecoilState[Slot] = 2;
						}
						else if (FMath::Abs(DS.RecoilRate[Slot]) < TinyRecoil)
						{
							DS.RecoilState[Slot] = 2;
						}
					}
					else
					{
						DS.RecoilShift[Slot] -= RecoilSettle;
						if (DS.RecoilShift[Slot] <= 0.f)
						{
							DS.RecoilShift[Slot] = 0.f;
							DS.RecoilState[Slot] = 0;
						}
					}
				}
				const float Shift = DS.RecoilState[Slot] == 0 ? 0.f
					: FMath::Lerp(DS.RecoilPrevShift[Slot], DS.RecoilShift[Slot], FMath::Min(DS.RecoilClock[Slot], 1.f));
				for (int32 r = 0; r < State->RecoilBones.Num(); ++r)
				{
					if (State->RecoilSlots[r] == Slot)
					{
						MC->SetBoneOffsetOnPrefix(State->RecoilBones[r], FVector3f(-Shift, 0.f, 0.f));
					}
				}
			}
		}

		if (bLook)
		{
			DS.bHasTurret = State && O->HasWeapons() && !State->Turret.IsEmpty();
			DS.bRider = Draw && !Draw->AttachToBoneInContainer.IsEmpty() && O->IsContained();
			DS.bBusy = DS.StealthFade != 0.f || DS.StealthGlow != 0.f || DS.bStateBusy || DS.FlashStart > 0.0
				|| DS.RecoilState[0] != 0 || DS.RecoilState[1] != 0 || DS.RecoilState[2] != 0
				|| DS.MuzzleHideTime[0] > 0.0 || DS.MuzzleHideTime[1] > 0.0 || DS.MuzzleHideTime[2] > 0.0;
		}

		if (DS.bRider)
		{
			// W3DDependencyModelDraw::adjustTransformMtx: "our client position is determined by the client
			// position of a particular bone in our container object".
			const FDrawState* Their = DrawStates.Find(O->GetContainedBy());
			if (Their && Their->Model && !bHide)
			{
				FTransform Bone = FTransform::Identity;
				Their->Model->GetBoneTransform(Draw->AttachToBoneInContainer, Bone);
				const FTransform Place = Bone * Their->Model->GetRelativeTransform();
				MC->SetRelativeLocationAndRotation(Place.GetLocation(), Place.GetRotation());
				DS.bPlaced = true;
				DS.bAtRest = false;
				DS.AppliedPos = Place.GetLocation();
			}
			continue;
		}
		if (O->IsDynamic() || !DS.bPlaced || DS.State.AdjustZ != DS.AppliedAdjustZ)
		{
			// In the same place, facing the same way, in both logic frames the screen blends between, and
			// its model already there: nothing to do (most units, most of the time).
			const bool bStill = O->GetPrevPosition() == O->GetDrawPosition() && O->GetPrevOrientation() == O->GetDrawOrientation()
				&& O->GetPrevRotation() == O->GetDrawRotation() && O->GetPrevChassis() == O->GetDrawChassis();
			if (bStill && DS.bAtRest && DS.State.AdjustZ == DS.AppliedAdjustZ)
			{
				continue;
			}
			FVector3f P = GetRenderPosition(*O);
			P.Z += O->GetDrawLift(P); // an aircraft's wheels stand on the tarmac, not in it
			P.Z += DS.State.AdjustZ;  // a building under construction, as far up as it has got
			const FVector NewPos = GenCoords::ToUE(P.X, P.Y, P.Z);
			// Out of the camera's reach (and not about to be left standing in it where it was last
			// drawn), or hidden: it is put in its place when it matters again.
			if (DS.bPlaced)
			{
				const double Radius = double(O->GetTemplate()->GetBoundingSphereRadius()) * GenCoords::UnitsToCm;
				if (bHide || !(NearView(NewPos, Radius) || NearView(DS.AppliedPos, Radius)))
				{
					DS.bAtRest = false;
					if (bHide && Draw && Draw->Vehicle.Kind != FGenVehicleDraw::None)
					{
						StopVehicleEffects(DS, *O);
					}
					continue;
				}
			}
			DS.bPlaced = true;
			DS.bAtRest = bStill;
			DS.AppliedPos = NewPos;
			DS.AppliedAdjustZ = DS.State.AdjustZ;
			const FGenObject::FChassisPose& ChassisA = O->GetPrevChassis();
			const FGenObject::FChassisPose& ChassisB = O->GetDrawChassis();
			if (!(ChassisA == FGenObject::FChassisPose()) || !(ChassisB == FGenObject::FChassisPose()))
			{
				// A vehicle's body on the ground (Drawable::applyPhysicsXform): its lift, then pitch and roll in its own frame.
				const FQuat4f Base = O->HasFreeRotation() ? FQuat4f::Slerp(O->GetPrevRotation(), O->GetDrawRotation(), Alpha)
					: FQuat4f(FVector3f::UpVector, GetRenderOrientation(*O));
				const FQuat4f Q = Base * FQuat4f(FVector3f::RightVector, FMath::Lerp(ChassisA.Pitch, ChassisB.Pitch, Alpha))
					* FQuat4f(FVector3f::ForwardVector, -FMath::Lerp(ChassisA.Roll, ChassisB.Roll, Alpha));
				const FVector LiftedPos = GenCoords::ToUE(P.X, P.Y, P.Z + FMath::Lerp(ChassisA.Z, ChassisB.Z, Alpha));
				MC->SetRelativeLocationAndRotation(LiftedPos, GenCoords::QuatToUE(Q.X, Q.Y, Q.Z, Q.W));
			}
			else if (O->HasFreeRotation())
			{
				// Projectiles and tumbling debris: full orientation, interpolated between logic frames.
				const FQuat4f Q = FQuat4f::Slerp(O->GetPrevRotation(), O->GetDrawRotation(), Alpha);
				MC->SetRelativeLocationAndRotation(NewPos, GenCoords::QuatToUE(Q.X, Q.Y, Q.Z, Q.W));
			}
			else
			{
				MC->SetRelativeLocationAndRotation(NewPos, FRotator(0.f, GenCoords::AngleToYaw(GetRenderOrientation(*O)), 0.f));
			}
			if (Draw && Draw->Vehicle.Kind != FGenVehicleDraw::None && !O->IsDead())
			{
				UpdateVehicleDraw(DS, *O, *Draw, DeltaFrames);
			}
		}
	}
	GEN_PERF_COUNT("R.StateBusy#", NumStateBusy);

	// What has a sound but nothing to draw (the map's Amb_ objects): heard where the local player can see it.
	if (Audio && SoundOnly.Num() > 0)
	{
		for (auto It = SoundOnly.CreateIterator(); It; ++It)
		{
			const FGenObject* O = FindObject(It.Key());
			FString Want;
			if (O && !O->IsDead() && !(IsShroudOn() && O->GetLocalShroud() > EGenObjectShroud::PartialClear))
			{
				Want = GetAmbientSound(*O, Audio->GetTemplateSound(*O->GetTemplate(), TEXT("SoundAmbient")));
			}
			if (Want != It.Value())
			{
				Audio->SetObjectLoop(It.Key(), 1, Want);
				It.Value() = Want;
			}
			if (!O)
			{
				It.RemoveCurrent();
			}
		}
	}
}
