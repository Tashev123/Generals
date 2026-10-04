#include "Logic/GenGameLogic.h"
#include "Client/GenDrawable.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

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

void UGenGameLogic::Deinitialize()
{
	Objects.Reset();
	ObjectOrder.Reset();
	Drawables.Reset();
	DrawStates.Reset();
	DelayedDamage.Reset();
	FloatingTexts.Reset();
	AIPlayers.Reset();
	ObstacleOwners.Reset();
	PendingCommands.Reset();
	MapActor = nullptr;
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

float UGenGameLogic::GetGroundHeight(float X, float Y) const
{
	const FGenMapData* Map = GetMap();
	return Map ? Map->GetGroundHeight(X, Y) : 0.f;
}

// ---- setup --------------------------------------------------------------------------------------

bool UGenGameLogic::StartSkirmish(AGenMapActor* InMapActor, const TArray<FGenSkirmishSlot>& Slots)
{
	MapActor = InMapActor;
	const FGenMapData* Map = GetMap();
	if (!Map)
	{
		UE_LOG(LogGenerals, Error, TEXT("StartSkirmish: no map loaded"));
		MapActor = nullptr;
		return false;
	}
	FGenGameData& GD = FGenGameData::Get();
	bGameOver = false;
	GameOverFrame = 0;
	WinningPlayer = INDEX_NONE;

	Pathfinder.Build(*Map);
	bNight = Map->TimeOfDay == 4; // TIME_OF_DAY_NIGHT

	// The simulation's own random stream; -GenSeed= makes a run repeatable.
	int32 Seed = int32(FDateTime::Now().GetTicks() & 0x7fffffff);
	FParse::Value(FCommandLine::Get(), TEXT("GenSeed="), Seed);
	Random.Initialize(Seed);
	bTrace = FParse::Param(FCommandLine::Get(), TEXT("GenTrace"));
	FParse::Value(FCommandLine::Get(), TEXT("GenPauseFrame="), PauseAtFrame);
	float Speed = 1.f;
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
		P.Name = FString::Printf(TEXT("Player %d"), P.Index);
		P.bHuman = S.bHuman;
		P.Team = S.Team;
		P.StartPosition = S.StartPosition;
		if (P.StartPosition < 0 && FreeStarts.Num() > 0)
		{
			P.StartPosition = FreeStarts[0];
			FreeStarts.RemoveAt(0);
		}
		P.Color = S.Color.A > 0.f ? S.Color : SlotPalette[SlotIdx % UE_ARRAY_COUNT(SlotPalette)];
		P.Money = 10000; // default skirmish starting cash
		InitPlayerSciences(P);
		if (P.bHuman && LocalPlayer == INDEX_NONE)
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

	// Computer players: every slot that isn't played by a human (-GenNoAI leaves them passive).
	if (!FParse::Param(FCommandLine::Get(), TEXT("GenNoAI")))
	{
		FGenAIData::Get().Initialize();
		EGenDifficulty Difficulty = EGenDifficulty::Normal;
		FString DifficultyName;
		if (FParse::Value(FCommandLine::Get(), TEXT("GenDifficulty="), DifficultyName))
		{
			Difficulty = DifficultyName.Equals(TEXT("Easy"), ESearchCase::IgnoreCase) ? EGenDifficulty::Easy
				: (DifficultyName.Equals(TEXT("Hard"), ESearchCase::IgnoreCase) ? EGenDifficulty::Hard : EGenDifficulty::Normal);
		}
		for (FGenPlayer& P : Players)
		{
			if (!P.bHuman && !P.bNeutral && P.Template && P.StartPosition >= 0)
			{
				P.bSkirmishAI = true;
				AIPlayers.Add(MakeUnique<FGenSkirmishAI>(this, P.Index, Difficulty));
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
	UpdateDrawables();
	UE_LOG(LogGenerals, Log, TEXT("Skirmish started: %d players, %d objects, logic %d fps"), Players.Num(), ObjectOrder.Num(), GenLogic::GetLogicFPS());
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
		CreateObject(T, 0, FVector3f(O.Location.X, O.Location.Y, O.Location.Z + Ground), O.Angle);
	}
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
	// ThingFactory::newObject: a template that only lists build variations becomes one of them.
	if (Template->BuildVariations.Num() > 0)
	{
		const FString& Pick = Template->BuildVariations[Random.RandRange(0, Template->BuildVariations.Num() - 1)];
		if (const FGenThingTemplate* Variation = FGenGameData::Get().FindTemplate(Pick))
		{
			Template = Variation;
		}
	}
	const FGenObjectID Id = NextID++;
	TUniquePtr<FGenObject> Obj = MakeUnique<FGenObject>(Id, Template, OwnerPlayer, this);
	Obj->SetPosition(Position);
	Obj->SetOrientation(Angle);
	Obj->SnapshotPrevious();
	Obj->SetModelCondition(GenConditions::Night(), bNight);
	FGenObject* Raw = Obj.Get();
	Objects.Add(Id, MoveTemp(Obj));
	ObjectOrder.Add(Id);

	// classifyObjectFootprint: immobile, non-small structures block pathing.
	if (Template->IsKindOf(TEXT("STRUCTURE")) && !Template->bGeometryIsSmall && !Raw->CanMove())
	{
		Pathfinder.AddObstacle(*Raw);
		ObstacleOwners.Add(Id);
	}

	if (MapActor)
	{
		const FLinearColor Color = Players.IsValidIndex(OwnerPlayer) && !Players[OwnerPlayer].bNeutral
			? Players[OwnerPlayer].Color : FLinearColor(0.8f, 0.8f, 0.8f);
		if (UGenModelComponent* MC = MapActor->CreateObjectVisual(*Template, Color))
		{
			MC->SetRelativeLocation(GenCoords::ToUE(Position.X, Position.Y, Position.Z));
			MC->SetRelativeRotation(FRotator(0.f, GenCoords::AngleToYaw(Angle), 0.f));
			Drawables.Add(Id, MC);
			FDrawState& DS = DrawStates.Add(Id, FDrawState()); // evaluated on the next drawable update
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
					FExtraDraw& E = DS.Extras.AddDefaulted_GetRef();
					E.DrawIndex = i;
					E.Model = Extra;
				}
			}
		}
	}
	return Raw;
}

void UGenGameLogic::DestroyObject(FGenObjectID ID)
{
	TUniquePtr<FGenObject>* Found = Objects.Find(ID);
	if (!Found)
	{
		return;
	}
	const FGenObject& Obj = **Found;
	if (ObstacleOwners.Remove(ID) > 0)
	{
		Pathfinder.RemoveObstacle(Obj);
	}
	Pathfinder.RemoveUnit(ID);
	if (const FDrawState* DS = DrawStates.Find(ID))
	{
		for (const FExtraDraw& E : DS->Extras)
		{
			if (UGenModelComponent* Extra = E.Model.Get()) { Extra->DestroyComponent(); }
		}
	}
	if (TObjectPtr<UGenModelComponent>* D = Drawables.Find(ID))
	{
		if (*D) { (*D)->DestroyComponent(); }
		Drawables.Remove(ID);
	}
	DrawStates.Remove(ID);
	if (FX)
	{
		FX->OnObjectDestroyed(ID); // attached particle systems (exhaust, trails) finish and go away
	}
	Objects.Remove(ID);
	ObjectOrder.Remove(ID);
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
	PendingCommands.Add([this, Units, Goal]()
	{
		// Spread a group around the goal so units don't all fight for the same spot.
		TArray<FGenObject*> Movers;
		float Spacing = 0.f;
		for (FGenObjectID Id : Units)
		{
			if (FGenObject* O = FindObject(Id))
			{
				if (O->CanMove())
				{
					Movers.Add(O);
					Spacing = FMath::Max(Spacing, O->GetTemplate()->GetBoundingCircleRadius() * 2.2f);
				}
			}
		}
		if (Movers.Num() == 0)
		{
			return;
		}
		Spacing = FMath::Max(Spacing, GenLogic::PathfindCellSize);
		const int32 Cols = FMath::CeilToInt32(FMath::Sqrt(float(Movers.Num())));
		for (int32 i = 0; i < Movers.Num(); ++i)
		{
			const int32 Row = i / Cols, Col = i % Cols;
			FVector3f Dest = Goal;
			if (Movers.Num() > 1)
			{
				Dest.X += (Col - (Cols - 1) * 0.5f) * Spacing;
				Dest.Y += (Row - (Cols - 1) * 0.5f) * Spacing;
			}
			Movers[i]->ClearAttack(); // a move order overrides the current attack
			Movers[i]->StopConstructing(); // ... and a dozer's building job
			Movers[i]->OnPlayerCommand(false); // ... and a gatherer's supply run
			Movers[i]->CommandMoveTo(Dest);
		}
	});
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
	PendingCommands.Add([this, Units, Goal]()
	{
		for (const TPair<FGenObject*, FVector3f>& It : SpreadGroup(Units, Goal))
		{
			It.Key->StopConstructing();
			It.Key->OnPlayerCommand(false);
			It.Key->CommandAttackMove(It.Value);
		}
	});
}

void UGenGameLogic::IssueGuard(const TArray<FGenObjectID>& Units, const FVector3f& Pos, FGenObjectID GuardObject, EGenGuardMode Mode)
{
	PendingCommands.Add([this, Units, Pos, GuardObject, Mode]()
	{
		for (const TPair<FGenObject*, FVector3f>& It : SpreadGroup(Units, Pos))
		{
			if (It.Key->GetID() == GuardObject)
			{
				continue; // (nothing guards itself)
			}
			It.Key->StopConstructing();
			It.Key->OnPlayerCommand(false);
			It.Key->CommandGuardPosition(FVector2f(It.Value.X, It.Value.Y), Mode, GuardObject);
		}
	});
}

void UGenGameLogic::IssueStop(const TArray<FGenObjectID>& Units)
{
	PendingCommands.Add([this, Units]()
	{
		for (FGenObjectID Id : Units)
		{
			if (FGenObject* O = FindObject(Id))
			{
				O->StopConstructing();
				O->OnPlayerCommand(false);
				O->CommandIdle();
			}
		}
	});
}

void UGenGameLogic::IssueLand(const TArray<FGenObjectID>& Units, FGenObjectID Airfield)
{
	PendingCommands.Add([this, Units, Airfield]()
	{
		for (FGenObjectID Id : Units)
		{
			FGenObject* O = FindObject(Id);
			if (O && O->bJetAI && !O->IsDead())
			{
				O->OnPlayerCommand(false);
				O->CommandLandAt(Airfield);
			}
		}
	});
}

void UGenGameLogic::IssueAttack(const TArray<FGenObjectID>& Units, FGenObjectID Target)
{
	PendingCommands.Add([this, Units, Target]()
	{
		const FGenObject* Victim = FindObject(Target);
		if (!Victim || !Victim->IsTargetable())
		{
			return;
		}
		for (FGenObjectID Id : Units)
		{
			if (FGenObject* O = FindObject(Id))
			{
				O->OnPlayerCommand(false);
				O->CommandStop();
				O->CommandAttack(Target);
			}
		}
	});
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
	// Index loop: damage can kill objects, but never adds or removes any.
	for (int32 i = 0, Num = ObjectOrder.Num(); i < Num; ++i)
	{
		FGenObject* O = Objects[ObjectOrder[i]].Get();
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
				if ((Affects & GenWeaponAffects::NotAirborne) != 0 && O->GetPosition().Z - GetGroundHeight(O->GetPosition().X, O->GetPosition().Y) > 9.f)
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

void UGenGameLogic::SpawnProjectile(const FGenWeaponTemplate& Weapon, const FGenObject& Shooter, FGenObjectID Victim, const FVector3f& TargetPos,
	const FVector3f& LaunchPos, float Angle)
{
	const FGenThingTemplate* ProjectileT = FGenGameData::Get().FindTemplate(Weapon.ProjectileObject);
	FGenObject* P = ProjectileT ? CreateObject(ProjectileT, Shooter.GetOwner(), LaunchPos, Angle) : nullptr;
	if (!P)
	{
		// Unknown projectile: fall back to an instant hit so the weapon still works.
		DealDamage(Weapon, Shooter.GetID(), Victim, TargetPos, false);
		return;
	}
	P->LaunchProjectile(&Weapon, Shooter.GetID(), Victim, TargetPos);
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [fire f%u] #%u %s launches %s (#%u) at #%u"), Frame, Shooter.GetID(), *Weapon.Name,
		*ProjectileT->Name, P->GetID(), Victim);
}

void UGenGameLogic::OnObjectDied(const FGenObject& Obj)
{
	// A wreck no longer blocks pathing.
	if (ObstacleOwners.Remove(Obj.GetID()) > 0)
	{
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

// ---- tick ---------------------------------------------------------------------------------------

void UGenGameLogic::Tick(float DeltaTime)
{
	if (!MapActor)
	{
		return;
	}
	const double Step = 1.0 / double(GenLogic::GetLogicFPS());
	Accumulator += double(DeltaTime) * TimeScale;
	// Avoid a spiral of death after hitches: never run more than a quarter second of logic per frame.
	Accumulator = FMath::Min(Accumulator, Step * FMath::Max(1, GenLogic::GetLogicFPS() / 4));
	while (Accumulator >= Step)
	{
		if (PauseAtFrame != 0 && Frame >= PauseAtFrame)
		{
			Accumulator = Step; // frozen: show exactly the state of this logic frame
			break;
		}
		StepLogic();
		Accumulator -= Step;
	}
	Alpha = float(FMath::Min(Accumulator / Step, 1.0));
	UpdateDrawables();
	if (FX)
	{
		// Effects run on the simulation clock, so they pause and speed up with the game.
		FX->Update(double(Frame) + double(Alpha));
	}
}

void UGenGameLogic::StepLogic()
{
	// Apply queued player commands at the start of the frame.
	TArray<TFunction<void()>> Commands = MoveTemp(PendingCommands);
	PendingCommands.Reset();
	for (TFunction<void()>& C : Commands)
	{
		C();
	}

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
	}

	// Objects created during this pass (projectiles) start updating next frame.
	for (int32 i = 0, Num = ObjectOrder.Num(); i < Num; ++i)
	{
		FGenObject* O = Objects[ObjectOrder[i]].Get();
		O->SnapshotPrevious();
		O->Update(Frame);
	}

	// Collisions between units are found after everything has moved and acted on next frame.
	UpdateCollisions();
	UpdateVictory();

	const uint32 FloatingTextFrames = uint32(GenLogic::GetLogicFPS() * 3);
	FloatingTexts.RemoveAll([this, FloatingTextFrames](const FGenFloatingText& F) { return Frame > F.Frame + FloatingTextFrames; });
}

FVector3f UGenGameLogic::GetRenderPosition(const FGenObject& Obj) const
{
	return FMath::Lerp(Obj.GetPrevPosition(), Obj.GetPosition(), Alpha);
}

float UGenGameLogic::GetRenderOrientation(const FGenObject& Obj) const
{
	return Obj.GetPrevOrientation() + GenLogic::AngleDiff(Obj.GetOrientation(), Obj.GetPrevOrientation()) * Alpha;
}

void UGenGameLogic::UpdateDrawables()
{
	// W3DModelDraw shows a muzzle flash only while its barrel is in the RECOIL_START state (about two logic
	// frames). Timed on the simulation clock so flashes follow the game speed and freeze when it is paused.
	const double MuzzleFlashSeconds = 2.0 / double(GenLogic::GetLogicFPS());
	const double FrameNow = double(Frame) + double(Alpha);
	const double Now = FrameNow / double(GenLogic::GetLogicFPS());
	const float DeltaFrames = float(FMath::Clamp(FrameNow - LastDrawFrameTime, 0.0, 8.0));
	LastDrawFrameTime = FrameNow;

	// W3DModelDraw recoil defaults, per 30 Hz frame (InitialRecoilSpeed, MaxRecoilDistance, RecoilDamping, RecoilSettleSpeed).
	constexpr float InitialRecoil = 2.f, MaxRecoil = 3.f, RecoilDamping = 0.4f, RecoilSettle = 0.065f, TinyRecoil = 0.01f;
	const float RecoilStepsPerFrame = 30.f / float(GenLogic::GetLogicFPS());

	for (TPair<uint32, TObjectPtr<UGenModelComponent>>& It : Drawables)
	{
		const FGenObject* O = FindObject(It.Key);
		UGenModelComponent* MC = It.Value;
		if (!O || !MC)
		{
			continue;
		}
		const FGenDrawModule* Draw = O->GetTemplate()->PrimaryDraw();

		// Condition state (model / animation / sub-objects) follows the object's model condition flags.
		FDrawState& DS = DrawStates.FindOrAdd(It.Key);
		// What is inside a container isn't drawn; a garrisoned building wears its occupants' colour.
		if (DS.bHidden != O->IsHidden())
		{
			DS.bHidden = O->IsHidden();
			// (Hidden-in-game rather than visibility: the model's sub-objects keep their own shown / hidden state.)
			MC->SetHiddenInGame(DS.bHidden, true);
		}
		if (DS.Owner != O->GetOwner())
		{
			if (DS.Owner != MIN_int32 && Players.IsValidIndex(O->GetOwner()))
			{
				MC->SetHouseColor(Players[O->GetOwner()].Color);
				for (const FExtraDraw& E : DS.Extras)
				{
					if (UGenModelComponent* Extra = E.Model.Get()) { Extra->SetHouseColor(Players[O->GetOwner()].Color); }
				}
			}
			DS.Owner = O->GetOwner();
		}
		if (DS.AppliedVersion != O->GetConditionVersion())
		{
			DS.AppliedVersion = O->GetConditionVersion();
			if (Draw)
			{
				const int32 Best = Draw->FindBestState(O->GetModelConditions());
				if (Best != DS.StateIndex)
				{
					DS.StateIndex = Best;
					GenDrawable::ApplyState(MC, *Draw, Best, bNight);
					DS.AppliedTurret = 1e9f; // the model may have changed: re-apply the turret pose
				}
			}
			for (FExtraDraw& E : DS.Extras)
			{
				UGenModelComponent* Extra = E.Model.Get();
				const FGenDrawModule& ExtraDraw = O->GetTemplate()->Draws[E.DrawIndex];
				const int32 Best = ExtraDraw.FindBestState(O->GetModelConditions());
				if (Extra && Best != E.StateIndex)
				{
					E.StateIndex = Best;
					GenDrawable::ApplyState(Extra, ExtraDraw, Best, bNight);
				}
			}
			DS.SubObjectVersion = MAX_uint32; // the state was re-applied: upgrade overrides have to go on top again
		}
		// SubObjectsUpgrade and payload on pylons: parts an upgrade shows or hides, whatever the state says.
		if (DS.SubObjectVersion != O->GetSubObjectVersion())
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
		if (O->GetDock() && O->GetTemplate()->bSupplyWarehouse && !O->GetTemplate()->SupplyBonePrefix.IsEmpty())
		{
			const FString& Prefix = O->GetTemplate()->SupplyBonePrefix;
			UGenModelComponent* Boxes = Draw && Draw->Type.Equals(TEXT("W3DSupplyDraw"), ESearchCase::IgnoreCase) ? MC : nullptr;
			for (const FExtraDraw& E : DS.Extras)
			{
				if (!Boxes && O->GetTemplate()->Draws[E.DrawIndex].Type.Equals(TEXT("W3DSupplyDraw"), ESearchCase::IgnoreCase))
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
		const FGenModelConditionState* State = Draw && Draw->States.IsValidIndex(DS.StateIndex) ? &Draw->States[DS.StateIndex] : nullptr;

		if (FX)
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
			if (DS.StateSystemsFor != DS.StateIndex)
			{
				DS.StateSystemsFor = DS.StateIndex;
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
		}

		if (State && O->HasWeapons())
		{
			// Turret yaw, interpolated between logic frames.
			if (!State->Turret.IsEmpty())
			{
				const float Prev = O->GetPrevTurretAngle(0);
				const float Angle = Prev + GenLogic::AngleDiff(O->GetTurretAngle(0), Prev) * Alpha;
				if (FMath::Abs(Angle - DS.AppliedTurret) > 1e-4f)
				{
					DS.AppliedTurret = Angle;
					MC->SetBoneRotationOverride(State->Turret, FQuat(FVector::UpVector, double(Angle)));
				}
			}

			for (int32 Slot = 0; Slot < 3; ++Slot)
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

		if (O->IsDynamic() || !DS.bPlaced)
		{
			DS.bPlaced = true;
			FVector3f P = GetRenderPosition(*O);
			P.Z += O->GetDrawLift(P); // an aircraft's wheels stand on the tarmac, not in it
			if (O->HasFreeRotation())
			{
				// Projectiles and tumbling debris: full orientation, interpolated between logic frames.
				const FQuat4f Q = FQuat4f::Slerp(O->GetPrevRotation(), O->GetRotation(), Alpha);
				MC->SetRelativeLocationAndRotation(GenCoords::ToUE(P.X, P.Y, P.Z), GenCoords::QuatToUE(Q.X, Q.Y, Q.Z, Q.W));
			}
			else
			{
				MC->SetRelativeLocationAndRotation(GenCoords::ToUE(P.X, P.Y, P.Z), FRotator(0.f, GenCoords::AngleToYaw(GetRenderOrientation(*O)), 0.f));
			}
		}
	}
}
