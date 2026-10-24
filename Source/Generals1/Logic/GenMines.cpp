// Mines: MinefieldBehavior (a mine, which stands for NumVirtualMines of them) and
// GenerateMinefieldBehavior (the ring of mines a building lays with its upgrade, the patch a cluster
// bomb leaves where it dies).
//
// A mine goes off under whoever of the kinds named in DetonatedBy drives or walks onto it - once for
// every step of RepeatDetonateMoveThresh the trespasser takes - and each time one of its virtual mines
// is spent: its health stands for how many are left. A mine that regenerates heals itself and has its
// mines back with its health (a spent one lies there as rubble until then); one that doesn't is gone
// with its last. Shot at, it goes off for every mine's worth of health it loses. When what laid it
// dies, it stops regenerating and withers away.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Map/GenMapData.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	/** MIN_HEALTH: what a mine with nothing left keeps, so that it can come back. */
	constexpr float MinMineHealth = 0.1f;
}

/** MinefieldBehavior's members. */
struct FGenMineState
{
	int32 VirtualLeft = 1;
	bool bRegenerates = false;
	bool bDraining = false;
	bool bIgnoreDamage = false;
	uint32 NextDeathCheckFrame = 0;
	int32 ScootFramesLeft = 0;
	FVector3f ScootVel = FVector3f::ZeroVector;
	FVector3f ScootAccel = FVector3f::ZeroVector;
	TArray<TPair<FGenObjectID, FVector3f>, TInlineAllocator<4>> Detonators; // who has set it off, and where he stood then
	TArray<TPair<FGenObjectID, uint32>, TInlineAllocator<3>> Immunes;      // mine clearers touching it, and when they last did
};

void FGenObject::FGenMineDeleter::operator()(FGenMineState* State) const
{
	delete State;
}

void FGenObject::InitMine()
{
	const FGenMinefield& D = Template->Minefield;
	MineState.Reset(new FGenMineState());
	MineState->VirtualLeft = FMath::Max(1, D.NumVirtualMines);
	MineState->bRegenerates = D.bRegenerates;
}

void FGenObject::SetMineScoot(const FVector3f& Start, const FVector3f& End)
{
	// MinefieldBehavior::setScootParms: thrown out from where it was laid, it slides (or falls) to its place.
	FGenMineState& M = *MineState;
	const float Gravity = FGenGameData::Get().Gravity;
	int32 ScootTime = Template->Minefield.ScootFrames;
	FVector3f EndOnGround(End.X, End.Y, Logic->GetGroundHeight(End.X, End.Y));
	if (Start.Z > EndOnGround.Z)
	{
		ScootTime = FMath::Max(ScootTime, FMath::CeilToInt32(FMath::Sqrt(2.f * (Start.Z - EndOnGround.Z) / FMath::Max(FMath::Abs(Gravity), 1e-4f))));
	}
	const FVector2f Delta(EndOnGround.X - Start.X, EndOnGround.Y - Start.Y);
	const float Dist = Delta.Size();
	if (ScootTime == 0 || (Dist <= 0.1f && FMath::Abs(EndOnGround.Z - Start.Z) <= 0.1f))
	{
		Position = EndOnGround;
		M.ScootFramesLeft = 0;
		return;
	}
	const float T = float(ScootTime);
	const float Speed = Dist / T;
	const FVector2f Dir = Dist <= 0.1f ? FVector2f::ZeroVector : Delta / Dist;
	M.ScootVel = FVector3f(Dir.X * Speed, Dir.Y * Speed, 0.f);
	M.ScootAccel = FVector3f(0.f, 0.f, Gravity); // ("accelMag = 2 * (dist - speed * t) / t^2": nothing, as written)
	Position = Start;
	M.ScootFramesLeft = ScootTime;
}

void FGenObject::UpdateMine(uint32 Frame)
{
	// MinefieldBehavior::update
	FGenMineState& M = *MineState;
	const FGenMinefield& D = Template->Minefield;
	if (M.ScootFramesLeft > 0)
	{
		M.ScootVel += M.ScootAccel;
		Position += M.ScootVel;
		const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
		if (Position.Z < Ground || M.ScootFramesLeft <= 1)
		{
			Position.Z = Ground;
		}
		--M.ScootFramesLeft;
	}
	if (Frame >= M.NextDeathCheckFrame && M.bRegenerates && D.bStopsRegenAfterCreatorDies)
	{
		M.NextDeathCheckFrame = Frame + uint32(FMath::Max(1, D.CreatorDeathCheckFrames));
		if (Producer != GenInvalidObjectID)
		{
			const FGenObject* Maker = Logic->FindObject(Producer);
			if (!Maker || Maker->bDead)
			{
				// What laid it is gone: no more healing, and it withers.
				M.bRegenerates = false;
				M.bDraining = true;
				bAutoHealDone = true;
			}
		}
	}
	if (M.bDraining)
	{
		AttemptDamage(MaxHealth * D.DrainPerSecond / float(GenLogic::GetLogicFPS()), TEXT("UNRESISTABLE"), TEXT("NORMAL"), ID);
	}
}

void FGenObject::MineDetonateOnce(const FVector3f& At)
{
	// MinefieldBehavior::detonateOnce
	FGenMineState& M = *MineState;
	const FGenMinefield& D = Template->Minefield;
	if (const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(D.DetonationWeapon))
	{
		Logic->FireTempWeapon(*W, *this, At);
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mine f%u] #%u %s goes off at (%.0f, %.0f): %d of %d left"), Logic->GetFrame(), ID, *Template->Name,
		At.X, At.Y, FMath::Max(0, M.VirtualLeft - 1), D.NumVirtualMines);
	if (M.VirtualLeft > 0)
	{
		--M.VirtualLeft;
	}
	if (!M.bRegenerates && M.VirtualLeft == 0)
	{
		bPendingDestroy = true;
	}
	else
	{
		// Its health says how many are left.
		const float Desired = FMath::Max(MinMineHealth, float(M.VirtualLeft) / float(FMath::Max(1, D.NumVirtualMines)) * MaxHealth);
		if (Health > Desired)
		{
			Health = Desired;
			LastDamageFrame = Logic->GetFrame();
		}
	}
	SetModelCondition(GenConditions::Rubble(), M.VirtualLeft == 0);
	if (!D.CreationList.IsEmpty())
	{
		Logic->DoOCL(D.CreationList, *this);
	}
}

void FGenObject::OnMineCollide(FGenObject& Other, uint32 Frame)
{
	// MinefieldBehavior::onCollide
	FGenMineState& M = *MineState;
	const FGenMinefield& D = Template->Minefield;
	if (Other.bDead || M.VirtualLeft == 0 || M.ScootFramesLeft > 0 || bDead || bPendingDestroy)
	{
		return;
	}
	// "Mine-clearers are granted immunity to us for as long as they continuously collide, even if no
	// longer clearing mines" (one that touches two mines while clearing isn't blown up by the other).
	for (int32 i = M.Immunes.Num() - 1; i >= 0; --i)
	{
		if (M.Immunes[i].Value + 2 < Frame)
		{
			M.Immunes.RemoveAtSwap(i); // (not touching it any more)
		}
		else if (M.Immunes[i].Key == Other.ID)
		{
			M.Immunes[i].Value = Frame;
			return;
		}
	}
	// "Things that are in the process of clearing mines are immune to mine detonation, even if we aren't
	// the specific mine they are trying to clear."
	if (Other.IsClearingMines())
	{
		if (M.Immunes.Num() < 3)
		{
			M.Immunes.Emplace(Other.ID, Frame);
		}
		return;
	}
	if (!D.bWorkersDetonate && Other.IsKindOf(TEXT("INFANTRY")) && Other.IsKindOf(TEXT("DOZER")))
	{
		return;
	}
	const EGenRelationship R = Logic->GetRelationship(Owner, Other.Owner);
	const uint8 Need = R == EGenRelationship::Enemies ? 1 : R == EGenRelationship::Allies ? 2 : 4;
	if ((D.DetonatedByMask & Need) == 0)
	{
		return;
	}
	// Once for every stretch of ground he covers on it, not every frame he stands there.
	bool bFound = false;
	for (TPair<FGenObjectID, FVector3f>& It : M.Detonators)
	{
		if (It.Key == Other.ID)
		{
			bFound = true;
			if (FVector3f::DistSquared(Other.Position, It.Value) <= FMath::Square(D.RepeatDetonateMoveThresh))
			{
				return;
			}
			It.Value = Other.Position;
			break;
		}
	}
	if (!bFound)
	{
		M.Detonators.Emplace(Other.ID, Other.Position);
	}
	// "clipPointToFootprint": under him, but on the mine.
	FVector3f At = Other.Position;
	const FVector2f Off(At.X - Position.X, At.Y - Position.Y);
	const float Radius = Template->GeometryMajorRadius;
	if (Off.SizeSquared() > Radius * Radius)
	{
		const FVector2f Clipped = Off.GetSafeNormal() * Radius;
		At.X = Position.X + Clipped.X;
		At.Y = Position.Y + Clipped.Y;
	}
	MineDetonateOnce(At);
}

bool FGenObject::IsMineMasked() const
{
	return MineState && MineState->VirtualLeft == 0;
}

void FGenObject::DisarmMine()
{
	// MinefieldBehavior::disarm
	if (!MineState)
	{
		bPendingDestroy = true; // ("it's a mine, but doesn't have LandMineInterface": "douse this thing before somebody gets hurt!")
		return;
	}
	FGenMineState& M = *MineState;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mine f%u] #%u %s disarmed%s"), Logic->GetFrame(), ID, *Template->Name,
		M.bRegenerates ? TEXT(" (it grows back)") : TEXT(""));
	if (!M.bRegenerates)
	{
		bPendingDestroy = true;
		return;
	}
	// "Detonation never puts our health below this, since we probably auto-regen."
	M.bIgnoreDamage = true;
	if (Health > MinMineHealth)
	{
		AttemptDamage(Health - MinMineHealth, TEXT("UNRESISTABLE"), TEXT("NONE"), ID);
	}
	M.bIgnoreDamage = false;
	M.VirtualLeft = 0;
	SetModelCondition(GenConditions::Rubble(), true);
}

bool FGenObject::IsClearingMines() const
{
	if (TargetID == GenInvalidObjectID)
	{
		return false;
	}
	// ("If we are attacking with an anti-mine weapon, we are clearing mines, regardless of our target" - reloading
	// or not.)
	const FGenObject* Target = Logic->FindObject(TargetID);
	for (int32 s = 0; Target && s < 3; ++s)
	{
		if (Weapons[s].Template && Weapons[s].Template->bAntiMine && CanWeaponTarget(*Weapons[s].Template, *Target))
		{
			return true;
		}
	}
	return false;
}

void FGenObject::SetMineClearingDetail(bool bOn)
{
	static const FString Flag(TEXT("MINE_CLEARING_DETAIL"));
	if (bOn == bMineClearingDetail)
	{
		return;
	}
	bMineClearingDetail = bOn;
	if (bOn) { SetFlags.AddUnique(Flag); } else { SetFlags.Remove(Flag); }
	RefreshWeaponSet();
}

void FGenObject::UpdateMineClearingDetail()
{
	// DozerAIUpdate ("maybe go clear some mines, if I feel like it" when idle; "no mine clearing fun while
	// I'm on the job") and WorkerAIUpdate (a worker gathering supplies may be diverted to clear mines too).
	SetMineClearingDetail(!HasDozerTask());
}

FGenObject* FGenObject::FindContinueTarget(const FVector3f& Around, float Range, int32 VictimOwner, bool bIgnoreStealth) const
{
	// PartitionFilterPossibleToAttack, of the victim's player, nearest the spot.
	FGenObject* Best = nullptr;
	float BestSq = Range * Range;
	Logic->GetPartition().ForEachInRange(FVector2f(Around.X, Around.Y), Range, [&](FGenObject& O)
	{
		if (&O == this || !(bIgnoreStealth ? O.IsTargetable() : O.IsAttackable()) || Logic->GetRelationship(Owner, O.Owner) != EGenRelationship::Enemies
			|| (VictimOwner != INDEX_NONE && O.Owner != VictimOwner) || ChooseWeapon(O) == INDEX_NONE)
		{
			return;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(O.Position.X, O.Position.Y), FVector2f(Around.X, Around.Y));
		if (DistSq < BestSq || (DistSq == BestSq && Best && O.ID < Best->ID))
		{
			BestSq = DistSq;
			Best = &O;
		}
	});
	return Best;
}

void FGenObject::CommandClearMinesAt(const FVector3f& Pos, int32 Slot)
{
	// Object::doCommandButtonAtPosition (FIRE_WEAPON, USES_MINE_CLEARING_WEAPONSET) and AIUpdateInterface::
	// privateAttackPosition: "rather grody hack for disarming stuff. if we attack a position, but have a
	// continue range for the weapon, try to find a suitable object to attack first" - stealthed or not.
	SetMineClearingDetail(true);
	const FGenWeaponTemplate* W = (Slot >= 0 && Slot < 3) ? Weapons[Slot].Template : nullptr;
	FGenObject* Mine = (W && W->ContinueAttackRange > 0.f) ? FindContinueTarget(Pos, W->ContinueAttackRange, INDEX_NONE, true) : nullptr;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mine f%u] #%u %s told to clear mines at (%.0f, %.0f): %s"), Logic->GetFrame(), ID, *Template->Name, Pos.X, Pos.Y,
		Mine ? *FString::Printf(TEXT("#%u %s first"), Mine->ID, *Mine->Template->Name) : TEXT("none there"));
	if (!Mine)
	{
		CommandMoveTo(Pos); // ("limit 'em to one shot, and fall thru": it goes there, and finds nothing)
		return;
	}
	bIgnoringStealth = true;
	CommandAttackWithSlot(Mine->ID, Slot);
	ContinueOrigin = Mine->Position;
	bHasContinueOrigin = true;
}

void FGenObject::OnMineHealthChanged(bool bHealing, FGenObjectID Source, const FString& DamageType)
{
	// MinefieldBehavior::onDamage / onHealing: as many mines as its health stands for; losing more than
	// that, it goes off for each (unless it is only withering away).
	FGenMineState& M = *MineState;
	if (M.bIgnoreDamage)
	{
		return;
	}
	const FGenMinefield& D = Template->Minefield;
	M.bIgnoreDamage = true; // (what its own going off does to its health is its own business)
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		const float ExpectedF = float(D.NumVirtualMines) * Health / FMath::Max(MaxHealth, 0.001f);
		const int32 Expected = FMath::Min(bHealing ? FMath::FloorToInt32(ExpectedF) : FMath::CeilToInt32(ExpectedF), D.NumVirtualMines);
		if (M.VirtualLeft < Expected)
		{
			M.VirtualLeft = Expected;
		}
		else if (M.VirtualLeft > Expected)
		{
			if (M.bDraining && Source == ID && DamageType.Equals(TEXT("UNRESISTABLE"), ESearchCase::IgnoreCase))
			{
				--M.VirtualLeft;
			}
			else
			{
				MineDetonateOnce(Position);
				if (bPendingDestroy)
				{
					break;
				}
			}
		}
		else
		{
			break;
		}
	}
	M.bIgnoreDamage = false;
	if (M.VirtualLeft == 0 && M.bRegenerates && Health < MinMineHealth)
	{
		Health = MinMineHealth;
	}
	SetModelCondition(GenConditions::Rubble(), M.VirtualLeft == 0);
}

void UGenGameLogic::ClearMinesUnder(const FGenObject& Building)
{
	// Object::setStatus: "CHECK FOR MINES, AND DETONATE THEM NOW ... detonate any enemy mines, or delete friendly ones".
	if (!bAnyMines)
	{
		return;
	}
	TArray<FGenObjectID, TInlineAllocator<16>> Mines;
	const FVector3f At = Building.GetPosition();
	GetPartition().ForEachInRange(FVector2f(At.X, At.Y), Building.GetTemplate()->GetBoundingCircleRadius() + 40.f, [&](FGenObject& Other)
	{
		if (Other.IsKindOf(TEXT("MINE")) && !Other.bDead && !Other.bPendingDestroy && Building.GeometryOverlaps(Other, 1.f))
		{
			Mines.Add(Other.GetID());
		}
	});
	for (FGenObjectID Id : Mines)
	{
		FGenObject* MineObj = FindObject(Id);
		if (!MineObj)
		{
			continue;
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [mine f%u] #%u %s is under #%u %s"), Frame, Id, *MineObj->GetTemplate()->Name, Building.GetID(), *Building.GetTemplate()->Name);
		if (GetRelationship(Building.GetOwner(), MineObj->GetOwner()) == EGenRelationship::Enemies)
		{
			// ("kill": all it has left goes off.)
			MineObj->AttemptDamage(MineObj->MaxHealth * 100.f, TEXT("UNRESISTABLE"), TEXT("NORMAL"), GenInvalidObjectID);
		}
		else
		{
			DestroyObject(Id);
		}
	}
}

void FGenObject::DetonateMinesInTheWay(uint32 Frame)
{
	// (PhysicsBehavior's collisions, asked from the mover's side: what mines its shape is on.)
	GEN_PERF_SCOPE("Obj.Mines");
	const float Reach = Template->GetBoundingCircleRadius() + 40.f;
	TArray<FGenObjectID, TInlineAllocator<8>> Mines;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), Reach, [&](FGenObject& Other)
	{
		if (Other.MineState && !Other.bDead && !Other.bPendingDestroy && FMath::Abs(Other.Position.Z - Position.Z) < 20.f && GeometryOverlaps(Other, 1.f))
		{
			Mines.Add(Other.GetID());
		}
	});
	for (FGenObjectID MineId : Mines)
	{
		FGenObject* MineObj = Logic->FindObject(MineId);
		if (MineObj && MineObj->MineState && !bDead)
		{
			MineObj->OnMineCollide(*this, Frame);
		}
	}
}

FGenObject* FGenObject::PlaceMineAt(const FVector3f& Pt, const FGenThingTemplate& MineT)
{
	// GenerateMinefieldBehavior::placeMineAt: not in water, not on a cliff, not (much) under a structure.
	const FGenGenerateMinefield& D = Template->GenerateMinefield;
	const FGenMapData* Map = Logic->GetMap();
	const FIntPoint Cell = Logic->GetPathfinder().WorldToCell(FVector2f(Pt.X, Pt.Y));
	if (Map && (Map->IsUnderwater(Pt.X, Pt.Y) || Map->IsCliffCellIndex(Cell.X, Cell.Y)))
	{
		return nullptr;
	}
	const float Keep = MineT.GetBoundingCircleRadius() * D.SkipIfThisMuchUnderStructure;
	bool bUnderStructure = false;
	Logic->GetPartition().ForEachInRange(FVector2f(Pt.X, Pt.Y), 200.f, [&](FGenObject& Them)
	{
		if (bUnderStructure || !Them.IsKindOf(TEXT("STRUCTURE")) || Them.bDead)
		{
			return;
		}
		const FGenThingTemplate& TT = *Them.GetTemplate();
		const FVector2f Off(Pt.X - Them.Position.X, Pt.Y - Them.Position.Y);
		if (TT.IsBox())
		{
			const FVector2f Dir = Them.GetDirection2D();
			const float Along = Off.X * Dir.X + Off.Y * Dir.Y, Across = -Off.X * Dir.Y + Off.Y * Dir.X;
			bUnderStructure = FMath::Abs(Along) <= TT.GeometryMajorRadius + Keep && FMath::Abs(Across) <= TT.GeometryMinorRadius + Keep;
		}
		else
		{
			bUnderStructure = Off.SizeSquared() <= FMath::Square(TT.GeometryMajorRadius + Keep);
		}
	});
	if (bUnderStructure)
	{
		return nullptr;
	}
	FGenObject* MineObj = Logic->CreateObject(&MineT, Owner, Pt, Logic->GetRandom().FRandRange(-PI, PI));
	if (!MineObj)
	{
		return nullptr;
	}
	MineObj->Producer = ID;
	if (MineObj->MineState)
	{
		MineObj->SetMineScoot(Position, Pt);
	}
	if (D.bUpgradable)
	{
		GeneratedMines.Add(MineObj->GetID());
	}
	return MineObj;
}

void FGenObject::PlaceMines()
{
	// GenerateMinefieldBehavior::placeMines
	if (bMinesGenerated)
	{
		return;
	}
	bMinesGenerated = true;
	const FGenGenerateMinefield& D = Template->GenerateMinefield;
	const FGenThingTemplate* MineT = FGenGameData::Get().FindTemplate(bMinesUpgraded ? D.UpgradedMineName : D.MineName);
	if (!MineT)
	{
		return;
	}
	FRandomStream& Rand = Logic->GetRandom();
	const FVector3f Target = Position;
	const float MineRadius = MineT->GetBoundingCircleRadius();
	const float Jitter = MineRadius * D.RandomJitter;
	int32 Num = 0;
	auto Ring = [&](float Radius)
	{
		// placeMinesAroundCircle
		const int32 NumMines = FMath::Max(1, FMath::CeilToInt32(2.f * PI * Radius / (MineRadius * 2.f)));
		const float AngleInc = 2.f * PI / float(NumMines);
		for (float Angle = 0.f; Angle < 2.f * PI - AngleInc * 0.5f; Angle += AngleInc)
		{
			FVector3f Pt(Target.X + Radius * FMath::Cos(Angle), Target.Y + Radius * FMath::Sin(Angle), 0.f);
			Pt.X += Rand.FRandRange(-Jitter, Jitter);
			Pt.Y += Rand.FRandRange(-Jitter, Jitter);
			Pt.Z = Logic->GetGroundHeight(Pt.X, Pt.Y);
			Num += PlaceMineAt(Pt, *MineT) ? 1 : 0;
		}
	};
	const float OwnRadius = Template->GetBoundingCircleRadius();
	// (Not given: GameData.ini's StandardMinefieldDistance / StandardMinefieldDensity.)
	const float Distance = D.DistanceAroundObject >= 0.f ? D.DistanceAroundObject : FGenGameData::Get().StandardMinefieldDistance;
	const float Density = D.MinesPerSquareFoot >= 0.f ? D.MinesPerSquareFoot : FGenGameData::Get().StandardMinefieldDensity;
	if (D.bSmartBorder)
	{
		// Rings a mine apart, from the object's edge (or from one mine in the middle) out to DistanceAroundObject.
		float Radius = OwnRadius;
		if (!D.bSmartBorderSkipInterior)
		{
			Radius = MineRadius;
			Num += PlaceMineAt(FVector3f(Target.X, Target.Y, Logic->GetGroundHeight(Target.X, Target.Y)), *MineT) ? 1 : 0;
		}
		Radius += MineRadius;
		do
		{
			Ring(Radius);
			Radius += MineRadius * 2.f;
		} while (Radius < Distance);
	}
	else if (D.bBorderOnly)
	{
		Ring(OwnRadius + Distance);
	}
	else
	{
		// placeMinesInFootprint: so many to the square foot, scattered, none nearer than a mine to another.
		const float Radius = OwnRadius + Distance;
		const int32 NumMines = FMath::Max(1, FMath::CeilToInt32(Density * PI * Radius * Radius));
		TArray<FVector2f> Placed;
		for (int32 i = 0; i < NumMines; ++i)
		{
			FVector2f Pt;
			for (int32 Retry = 0; Retry < 100; ++Retry)
			{
				const float R = Radius * FMath::Sqrt(Rand.FRand());
				const float A = Rand.FRandRange(0.f, 2.f * PI);
				Pt = FVector2f(Target.X + R * FMath::Cos(A), Target.Y + R * FMath::Sin(A));
				if (!Placed.ContainsByPredicate([&](const FVector2f& P) { return FVector2f::DistSquared(P, Pt) < FMath::Square(MineRadius * 2.f); }))
				{
					break;
				}
			}
			if (FVector2f::DistSquared(Pt, FVector2f(Target.X, Target.Y)) > OwnRadius * OwnRadius
				&& PlaceMineAt(FVector3f(Pt.X, Pt.Y, Logic->GetGroundHeight(Pt.X, Pt.Y)), *MineT))
			{
				Placed.Add(Pt);
				++Num;
			}
		}
	}
	Logic->DoFXObj(D.GenerationFX, *this);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mine f%u] #%u %s lays %d %s"), Logic->GetFrame(), ID, *Template->Name, Num, *MineT->Name);
}

void FGenObject::UpdateMinefieldUpgrade()
{
	// GenerateMinefieldBehavior::update: the mines it has laid are taken up and the upgraded kind laid in their place.
	const FGenGenerateMinefield& D = Template->GenerateMinefield;
	if (bMinesUpgraded || !bMinesGenerated || !D.bUpgradable || D.UpgradedTriggeredBy.IsEmpty() || !HasObjectUpgrade(D.UpgradedTriggeredBy))
	{
		return;
	}
	bMinesUpgraded = true;
	for (FGenObjectID MineId : GeneratedMines)
	{
		if (FGenObject* MineObj = Logic->FindObject(MineId))
		{
			MineObj->bPendingDestroy = true;
		}
	}
	GeneratedMines.Reset();
	bMinesGenerated = false;
	PlaceMines();
}
