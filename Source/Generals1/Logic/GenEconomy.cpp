// Money, power, unit production and building construction.
// Follows ProductionUpdate.cpp, DefaultProductionExitUpdate.cpp, ParkingPlaceBehavior::exitObjectViaDoor,
// DozerAIUpdate.cpp, BuildAssistant.cpp and ThingTemplate::calcTimeToBuild.

#include "Logic/GenGameLogic.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	int32 AwaitingConstruction() { static const int32 I = GenConditions::Index(TEXT("AWAITING_CONSTRUCTION")); return I; }
	int32 PartiallyConstructed() { static const int32 I = GenConditions::Index(TEXT("PARTIALLY_CONSTRUCTED")); return I; }
	int32 ActivelyBeingConstructed() { static const int32 I = GenConditions::Index(TEXT("ACTIVELY_BEING_CONSTRUCTED")); return I; }
	int32 ActivelyConstructing() { static const int32 I = GenConditions::Index(TEXT("ACTIVELY_CONSTRUCTING")); return I; }
	int32 Sold() { static const int32 I = GenConditions::Index(TEXT("SOLD")); return I; }

	/** Distance from a point to an object's footprint (0 inside). */
	float DistanceToFootprint(const FGenObject& Obj, const FVector3f& P)
	{
		const FGenThingTemplate* T = Obj.GetTemplate();
		const float DX = P.X - Obj.GetPosition().X, DY = P.Y - Obj.GetPosition().Y;
		if (!T->IsBox())
		{
			return FMath::Max(0.f, FMath::Sqrt(DX * DX + DY * DY) - T->GeometryMajorRadius);
		}
		const float C = FMath::Cos(Obj.GetOrientation()), S = FMath::Sin(Obj.GetOrientation());
		const float OX = FMath::Max(0.f, FMath::Abs(DX * C + DY * S) - T->GeometryMajorRadius);
		const float OY = FMath::Max(0.f, FMath::Abs(-DX * S + DY * C) - T->GeometryMinorRadius);
		return FMath::Sqrt(OX * OX + OY * OY);
	}

	/** Do two oriented rectangles (center, heading, half sizes) overlap? Separating axis test. */
	bool RectanglesOverlap(const FVector2f& PosA, float AngleA, const FVector2f& HalfA, const FVector2f& PosB, float AngleB, const FVector2f& HalfB)
	{
		const FVector2f AxesA[2] = { FVector2f(FMath::Cos(AngleA), FMath::Sin(AngleA)), FVector2f(-FMath::Sin(AngleA), FMath::Cos(AngleA)) };
		const FVector2f AxesB[2] = { FVector2f(FMath::Cos(AngleB), FMath::Sin(AngleB)), FVector2f(-FMath::Sin(AngleB), FMath::Cos(AngleB)) };
		const FVector2f Delta = PosB - PosA;
		for (const FVector2f& Axis : { AxesA[0], AxesA[1], AxesB[0], AxesB[1] })
		{
			const float ReachA = HalfA.X * FMath::Abs(FVector2f::DotProduct(AxesA[0], Axis)) + HalfA.Y * FMath::Abs(FVector2f::DotProduct(AxesA[1], Axis));
			const float ReachB = HalfB.X * FMath::Abs(FVector2f::DotProduct(AxesB[0], Axis)) + HalfB.Y * FMath::Abs(FVector2f::DotProduct(AxesB[1], Axis));
			if (FMath::Abs(FVector2f::DotProduct(Delta, Axis)) > ReachA + ReachB)
			{
				return false;
			}
		}
		return true;
	}
}

// ---- objects ------------------------------------------------------------------------------------

void FGenObject::UpdateProduction(uint32 Frame)
{
	// ProductionUpdate::update: the head of the queue advances one frame; when done the unit leaves by the door.
	FProduction& P = Production[0];
	P.FramesDone += 1.f;
	if (P.Upgrade)
	{
		// UpgradeTemplate::calcTimeToBuild: research takes its BuildTime, power or no power.
		if (P.FramesDone >= P.Upgrade->BuildTime * float(GenLogic::GetLogicFPS()))
		{
			const FGenUpgradeTemplate* Upgrade = P.Upgrade;
			Production.RemoveAt(0);
			Logic->FinishUpgrade(*this, *Upgrade);
		}
		return;
	}
	if (P.FramesDone >= Logic->CalcTimeToBuild(*P.Unit, Owner))
	{
		const FGenThingTemplate* Unit = P.Unit;
		const int32 ExitDoor = P.ExitDoor;
		Production.RemoveAt(0);
		Logic->SpawnProducedUnit(*this, *Unit, ExitDoor);
	}
}

FVector3f FGenObject::GetNaturalRallyPoint(bool bOffset) const
{
	// DefaultProductionExitUpdate::getNaturalRallyPoint (a building without an exit: where it stands)
	if (!Template->bHasExit)
	{
		return Position;
	}
	FVector3f P = Template->NaturalRallyPoint;
	if (bOffset)
	{
		P += P.GetSafeNormal() * (2.f * GenLogic::PathfindCellSize); // a little beyond the door
	}
	return GetTransform().TransformPositionNoScale(P);
}

void FGenObject::StopConstructing()
{
	BuildTarget = GenInvalidObjectID;
	RepairTarget = GenInvalidObjectID;
	SetModelCondition(ActivelyConstructing(), false);
}

FGenObjectID FGenObject::GetSoleHealingBenefactor() const
{
	return Logic->GetFrame() > SoleHealerExpireFrame ? GenInvalidObjectID : SoleHealer;
}

bool FGenObject::AttemptHealingFromSoleBenefactor(float Amount, FGenObjectID Source, uint32 Duration)
{
	// First come first served: while one benefactor is at it the others are turned away.
	const uint32 Now = Logic->GetFrame();
	if (Now > SoleHealerExpireFrame || SoleHealer == Source)
	{
		SoleHealer = Source;
		SoleHealerExpireFrame = Now + Duration;
		AttemptHealing(Amount);
		return true;
	}
	return false;
}

bool FGenObject::StartRepair(FGenObject& Building)
{
	// DozerAIUpdate::privateRepair
	if (RepairTarget == Building.ID)
	{
		return true; // already on it
	}
	if (!Logic->CanRepairObject(*this, Building))
	{
		return false;
	}
	const FGenObjectID Other = Building.GetSoleHealingBenefactor();
	if (Other != GenInvalidObjectID && Other != ID)
	{
		return false; // somebody else is mending it
	}
	StopDocking();
	ClearAttack();
	StopConstructing();
	AIMode = EGenAIMode::None;
	bHasExitRally = false;
	RepairTarget = Building.ID;
	NextRepathFrame = 0;
	CommandStop();
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [repair f%u] #%u %s goes to mend #%u %s (%.0f / %.0f)"), Logic->GetFrame(), ID, *Template->Name,
		Building.ID, *Building.Template->Name, Building.Health, Building.MaxHealth);
	return true;
}

void FGenObject::UpdateDozerBored(uint32 Frame)
{
	// DozerPrimaryIdleState::update: a dozer with nothing to do for BoredTime mends the nearest damaged
	// building within BoredRange.
	if (!IsAIIdle() || Template->DozerBoredTime <= 0)
	{
		DozerIdleSince = Frame;
		return;
	}
	if (Frame - DozerIdleSince <= uint32(Template->DozerBoredTime))
	{
		return;
	}
	DozerIdleSince = Frame; // (so the search runs once in a while, not every frame)
	FGenObject* Closest = nullptr;
	float ClosestDistSq = FMath::Square(Template->DozerBoredRange);
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		FGenObject* O = Logic->FindObject(Id);
		const float DistSq = O ? FVector2f::DistSquared(FVector2f(O->Position.X, O->Position.Y), FVector2f(Position.X, Position.Y)) : 0.f;
		if (O && DistSq <= ClosestDistSq && Logic->CanRepairObject(*this, *O))
		{
			Closest = O;
			ClosestDistSq = DistSq;
		}
	}
	if (Closest)
	{
		StartRepair(*Closest);
	}
}

void FGenObject::UpdateDozer(uint32 Frame)
{
	// DozerAIUpdate build / repair task: drive up to the building, then raise or mend it frame by frame.
	const bool bRepair = RepairTarget != GenInvalidObjectID;
	FGenObject* B = Logic->FindObject(bRepair ? RepairTarget : BuildTarget);
	if (bRepair)
	{
		// (Somebody beat us to it: one repairer per building.)
		const FGenObjectID Other = B ? B->GetSoleHealingBenefactor() : GenInvalidObjectID;
		if (!B || !Logic->CanRepairObject(*this, *B) || (Other != GenInvalidObjectID && Other != ID))
		{
			StopConstructing();
			return;
		}
	}
	else if (!B || B->bDead || !B->bUnderConstruction || B->SellFrame != 0)
	{
		StopConstructing();
		return;
	}
	const float Reach = DistanceToFootprint(*B, Position) - Template->GetBoundingCircleRadius();
	const bool bAtSite = Reach <= 8.f || (!bMoving && Reach <= 45.f);
	if (!bAtSite)
	{
		SetModelCondition(ActivelyConstructing(), false);
		if (!bMoving && Frame >= NextRepathFrame)
		{
			StartPath(B->Position, false); // ends on the nearest free ground beside the footprint
			NextRepathFrame = Frame + uint32(GenLogic::GetLogicFPS());
		}
		return;
	}
	if (bMoving)
	{
		CommandStop();
		Velocity.X = Velocity.Y = 0.f;
		bStatusBraking = false;
	}
	// Face the work.
	const float ToSite = FMath::Atan2(B->Position.Y - Position.Y, B->Position.X - Position.X);
	const float Turn = Locomotor ? Locomotor->TurnRate : 0.1f;
	SetOrientation(Orientation + FMath::Clamp(GenLogic::AngleDiff(ToSite, Orientation), -Turn, Turn));

	SetModelCondition(ActivelyConstructing(), true); // (repairs borrow the constructing animation)
	if (bRepair)
	{
		const float Heal = B->MaxHealth * Template->DozerRepairPerSecond / float(GenLogic::GetLogicFPS());
		if (!B->AttemptHealingFromSoleBenefactor(Heal, ID, 2))
		{
			StopConstructing(); // the building is already being mended by somebody else
		}
		else if (B->Health >= B->MaxHealth)
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [repair f%u] #%u %s has mended #%u %s"), Frame, ID, *Template->Name, B->ID, *B->Template->Name);
			StopConstructing();
		}
		return;
	}
	B->SetModelCondition(AwaitingConstruction(), false);
	B->SetModelCondition(PartiallyConstructed(), true);
	B->LastBuiltFrame = Frame;

	const float FramesToBuild = Logic->CalcTimeToBuild(*B->Template, Owner);
	B->ConstructionPercent += 100.f / FramesToBuild;
	B->Health = FMath::Min(B->MaxHealth, B->Health + B->MaxHealth / FramesToBuild);
	if (B->ConstructionPercent >= 100.f)
	{
		B->ConstructionPercent = 100.f;
		B->bUnderConstruction = false;
		B->Health = B->MaxHealth;
		B->SetModelCondition(AwaitingConstruction(), false);
		B->SetModelCondition(PartiallyConstructed(), false);
		B->SetModelCondition(ActivelyBeingConstructed(), false);
		StopConstructing();
		Logic->OnBuildingCompleted(*B);
	}
}

// ---- queries ------------------------------------------------------------------------------------

void UGenGameLogic::GetEnergy(int32 Player, int32& OutProduction, int32& OutConsumption) const
{
	OutProduction = OutConsumption = 0;
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		const int32 Energy = O->GetEnergyProduction();
		if (Energy == 0 || O->GetOwner() != Player || O->IsDead() || O->IsUnderConstruction() || O->IsBeingSold())
		{
			continue;
		}
		(Energy > 0 ? OutProduction : OutConsumption) += FMath::Abs(Energy);
	}
}

float UGenGameLogic::CalcTimeToBuild(const FGenThingTemplate& Template, int32 Player) const
{
	const FGenGameData& GD = FGenGameData::Get();
	const float Frames = Template.BuildTime * float(GenLogic::GetLogicFPS());

	// A power shortage slows everything down: 20% short with a 40% modifier is an 8% penalty, bounded so
	// that any shortage costs something and a total blackout doesn't stop production.
	int32 Produced = 0, Consumed = 0;
	GetEnergy(Player, Produced, Consumed);
	const float Supply = Consumed > 0 ? FMath::Min(1.f, float(Produced) / float(Consumed)) : 1.f;
	float Rate = FMath::Max(1.f - (1.f - Supply) * GD.LowEnergyPenaltyModifier, GD.MinLowEnergyProductionSpeed);
	if (Supply < 1.f)
	{
		Rate = FMath::Min(Rate, GD.MaxLowEnergyProductionSpeed);
	}
	return FMath::Max(1.f, Frames / FMath::Max(Rate, 0.01f));
}

bool UGenGameLogic::CanMake(int32 Player, const FGenThingTemplate& Template, FString* OutWhy, bool bCheckMoney) const
{
	const FGenGameData& GD = FGenGameData::Get();
	for (const TArray<FString>& AnyOf : Template.Prerequisites)
	{
		bool bHave = false;
		for (FGenObjectID Id : ObjectOrder)
		{
			const FGenObject* O = Objects[Id].Get();
			if (O->GetOwner() == Player && !O->IsDead() && !O->IsUnderConstruction()
				&& AnyOf.ContainsByPredicate([O](const FString& Name) { return Name.Equals(O->GetTemplate()->Name, ESearchCase::IgnoreCase); }))
			{
				bHave = true;
				break;
			}
		}
		if (!bHave)
		{
			if (OutWhy)
			{
				const FGenThingTemplate* Needed = GD.FindTemplate(AnyOf[0]);
				*OutWhy = FString::Printf(TEXT("Requires %s"), Needed ? *FGenCommandData::Get().GetText(Needed->DisplayName) : *AnyOf[0]);
			}
			return false;
		}
	}
	// A general's promotion unlocks some units (the Paladin, the Stealth Fighter, the Scud Launcher ...).
	for (const FString& Science : Template.PrerequisiteSciences)
	{
		if (!HasScience(Player, Science))
		{
			if (OutWhy)
			{
				const FGenScience* S = FGenUpgradeData::Get().FindScience(Science);
				*OutWhy = FString::Printf(TEXT("Requires the general's promotion %s"), S && !S->DisplayName.IsEmpty() ? *FGenCommandData::Get().GetText(S->DisplayName) : *Science);
			}
			return false;
		}
	}
	if (bCheckMoney && GetMoney(Player) < int32(Template.BuildCost))
	{
		if (OutWhy) { *OutWhy = TEXT("Not enough money"); }
		return false;
	}
	return true;
}

bool UGenGameLogic::IsLocationLegalToBuild(const FGenThingTemplate& Template, const FVector3f& Pos, float Angle) const
{
	const FGenMapData* Map = GetMap();
	if (!Map)
	{
		return false;
	}
	const bool bBox = Template.IsBox();
	const float Major = Template.GeometryMajorRadius;
	const float Minor = bBox ? Template.GeometryMinorRadius : Major;
	const FVector2f Center(Pos.X, Pos.Y);
	if (!Pathfinder.IsAreaClear(Center, Angle, Major, Minor, bBox))
	{
		return false; // cliff, water, map edge or another building
	}

	// Flat enough: the footprint's corners and edge midpoints stay within the allowed height variation.
	const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);
	float MinZ = FLT_MAX, MaxZ = -FLT_MAX;
	for (int32 i = -1; i <= 1; ++i)
	{
		for (int32 j = -1; j <= 1; ++j)
		{
			const float LX = Major * float(i), LY = Minor * float(j);
			const float Z = Map->GetGroundHeight(Pos.X + LX * C - LY * S, Pos.Y + LX * S + LY * C);
			MinZ = FMath::Min(MinZ, Z);
			MaxZ = FMath::Max(MaxZ, Z);
		}
	}
	if (MaxZ - MinZ > FGenGameData::Get().AllowedHeightVariationForBuilding)
	{
		return false;
	}

	// Supply centers keep a border free around supply piles, so the gatherers can get at them.
	const float Border = FGenGameData::Get().SupplyBuildBorder;
	if (Border > 0.f && Template.IsKindOf(TEXT("CANNOT_BUILD_NEAR_SUPPLIES")))
	{
		for (FGenObjectID Id : ObjectOrder)
		{
			const FGenObject* O = Objects[Id].Get();
			if (O->IsDead() || O->IsDestroyed() || !O->IsKindOf(TEXT("SUPPLY_SOURCE")))
			{
				continue;
			}
			const FGenThingTemplate* ST = O->GetTemplate();
			const FVector2f Half(ST->GeometryMajorRadius + Border, (ST->IsBox() ? ST->GeometryMinorRadius : ST->GeometryMajorRadius) + Border);
			if (RectanglesOverlap(Center, Angle, FVector2f(Major, Minor), FVector2f(O->GetPosition().X, O->GetPosition().Y), O->GetOrientation(), Half))
			{
				return false;
			}
		}
	}

	// Nothing alive standing in the way, apart from mobile units (they get out of the way).
	const float Reach = FMath::Sqrt(Major * Major + Minor * Minor);
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (!O->IsTargetable() || O->CanMove())
		{
			continue;
		}
		const float DX = O->GetPosition().X - Pos.X, DY = O->GetPosition().Y - Pos.Y;
		const float ObjRadius = O->GetTemplate()->GetBoundingCircleRadius();
		if (DX * DX + DY * DY > FMath::Square(Reach + ObjRadius))
		{
			continue;
		}
		if (FMath::Abs(DX * C + DY * S) <= Major + ObjRadius * 0.7f && FMath::Abs(-DX * S + DY * C) <= Minor + ObjRadius * 0.7f)
		{
			return false;
		}
	}
	return true;
}

// ---- orders -------------------------------------------------------------------------------------

void UGenGameLogic::IssueQueueUnit(FGenObjectID Factory, const FString& UnitTemplate)
{
	PendingCommands.Add([this, Factory, UnitTemplate]()
	{
		FGenObject* F = FindObject(Factory);
		const FGenThingTemplate* Unit = FGenGameData::Get().FindTemplate(UnitTemplate);
		if (F && Unit)
		{
			QueueUnit(*F, *Unit);
		}
	});
}

bool UGenGameLogic::QueueUnit(FGenObject& Factory, const FGenThingTemplate& Unit)
{
	if (Factory.IsDead() || Factory.IsUnderConstruction() || Factory.IsBeingSold() || !Factory.GetTemplate()->bHasProduction
		|| Factory.Production.Num() >= Factory.GetTemplate()->MaxQueueEntries || !CanMake(Factory.GetOwner(), Unit))
	{
		return false;
	}
	// An airfield builds only as many planes as it can park: the space is taken when the plane is ordered.
	int32 ExitDoor = FGenParkingPlace::DoorNoneNeeded;
	if (FGenParkingPlace* PP = Factory.GetParking())
	{
		ExitDoor = PP->ReserveDoorForExit(Unit);
		if (ExitDoor == FGenParkingPlace::DoorNoneAvailable)
		{
			return false;
		}
	}
	// The unit is paid for when it is queued (and refunded if cancelled).
	const int32 Cost = int32(Unit.BuildCost);
	Players[Factory.GetOwner()].Money -= Cost;
	FGenObject::FProduction& P = Factory.Production.AddDefaulted_GetRef();
	P.Unit = &Unit;
	P.Cost = Cost;
	P.ExitDoor = ExitDoor;
	return true;
}

bool UGenGameLogic::CanFactoryMake(const FGenObject& Factory, const FGenThingTemplate& Unit) const
{
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenCommandSet* Set = CD.FindSet(Factory.GetCommandSet());
	for (int32 s = 0; Set && s < FGenCommandSet::NumSlots; ++s)
	{
		const FGenCommandButton* Button = CD.FindButton(Set->Slots[s]);
		if (Button && Button->Command == EGenCommand::UnitBuild && Button->Object.Equals(Unit.Name, ESearchCase::IgnoreCase))
		{
			if (Factory.GetParking() && !Factory.GetParking()->HasAvailableSpaceFor(Unit))
			{
				return false; // the airfield is full
			}
			return CanMake(Factory.GetOwner(), Unit, nullptr, false);
		}
	}
	return false;
}

void UGenGameLogic::AddMoney(int32 Player, int32 Amount)
{
	if (Players.IsValidIndex(Player))
	{
		Players[Player].Money = FMath::Max(0, Players[Player].Money + Amount);
	}
}

void UGenGameLogic::IssueCancelUnit(FGenObjectID Factory, int32 QueueIndex)
{
	PendingCommands.Add([this, Factory, QueueIndex]()
	{
		FGenObject* F = FindObject(Factory);
		if (F && F->Production.IsValidIndex(QueueIndex))
		{
			Players[F->GetOwner()].Money += F->Production[QueueIndex].Cost;
			if (F->Production[QueueIndex].Upgrade)
			{
				CancelUpgradeInProduction(F->GetOwner(), *F->Production[QueueIndex].Upgrade);
			}
			if (F->GetParking())
			{
				F->GetParking()->UnreserveDoorForExit(F->Production[QueueIndex].ExitDoor);
			}
			F->Production.RemoveAt(QueueIndex);
		}
	});
}

void UGenGameLogic::IssueConstruct(FGenObjectID Dozer, const FString& BuildingTemplate, const FVector3f& Pos, float Angle)
{
	PendingCommands.Add([this, Dozer, BuildingTemplate, Pos, Angle]()
	{
		FGenObject* D = FindObject(Dozer);
		const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(BuildingTemplate);
		if (!D || !T || D->IsDead() || !D->GetTemplate()->bIsDozer || !CanMake(D->GetOwner(), *T) || !IsLocationLegalToBuild(*T, Pos, Angle))
		{
			return;
		}
		D->OnPlayerCommand(false); // a worker on a supply run drops it
		ConstructBuilding(D, *T, Pos, Angle, D->GetOwner());
	});
}

FGenObject* UGenGameLogic::ConstructBuilding(FGenObject* Dozer, const FGenThingTemplate& Template, const FVector3f& Pos, float Angle, int32 Player)
{
	// BuildAssistant::buildObjectNow
	if (!Players.IsValidIndex(Player))
	{
		return nullptr;
	}
	Players[Player].Money -= int32(Template.BuildCost);

	// Shrubs, rocks and other lifeless props under the footprint are cleared away.
	const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);
	const float Minor = Template.IsBox() ? Template.GeometryMinorRadius : Template.GeometryMajorRadius;
	auto InFootprint = [&](const FVector3f& P, float Margin)
	{
		const float DX = P.X - Pos.X, DY = P.Y - Pos.Y;
		return FMath::Abs(DX * C + DY * S) <= Template.GeometryMajorRadius + Margin && FMath::Abs(-DX * S + DY * C) <= Minor + Margin;
	};
	for (int32 i = ObjectOrder.Num() - 1; i >= 0; --i)
	{
		const FGenObject* O = Objects[ObjectOrder[i]].Get();
		if (!O->IsTargetable() && !O->IsDead() && !O->IsProjectile() && !O->CanMove() && InFootprint(O->GetPosition(), 2.f))
		{
			DestroyObject(ObjectOrder[i]);
		}
	}

	FGenObject* B = CreateObject(&Template, Player, FVector3f(Pos.X, Pos.Y, GetGroundHeight(Pos.X, Pos.Y)), Angle);
	if (!B)
	{
		return nullptr;
	}
	if (Dozer)
	{
		// A new site starts as scaffolding with one hit point.
		B->bUnderConstruction = true;
		B->ConstructionPercent = 0.f;
		B->Health = 1.f;
		B->SetModelCondition(AwaitingConstruction(), true);
	}

	// Units standing where the building goes move out.
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* U = Objects[Id].Get();
		if (U != Dozer && U->IsGroundUnit() && InFootprint(U->GetPosition(), U->GetCollisionRadius()))
		{
			U->MoveToFreeGround();
		}
	}
	if (Dozer)
	{
		ResumeConstruction(*Dozer, *B);
	}
	return B;
}

void UGenGameLogic::ResumeConstruction(FGenObject& Dozer, FGenObject& Building)
{
	Dozer.ClearAttack();
	Dozer.StopDocking();
	Dozer.ClearAIMode();
	Dozer.RepairTarget = GenInvalidObjectID;
	Dozer.BuildTarget = Building.GetID();
	Dozer.NextRepathFrame = 0;
	Dozer.CommandStop();
}

bool UGenGameLogic::CanRepairObject(const FGenObject& Dozer, const FGenObject& Target) const
{
	// ActionManager::canRepairObject: dozers mend damaged buildings that aren't an enemy's.
	if (!Dozer.GetTemplate()->bIsDozer || Dozer.IsDead() || Target.IsDead() || Target.IsDestroyed())
	{
		return false;
	}
	if (GetRelationship(Dozer.GetOwner(), Target.GetOwner()) == EGenRelationship::Enemies)
	{
		return false;
	}
	if (Target.IsKindOf(TEXT("BRIDGE")) || Target.IsKindOf(TEXT("BRIDGE_TOWER")) || Target.IsKindOf(TEXT("REBUILD_HOLE")))
	{
		return false;
	}
	if (Dozer.IsUnderConstruction() || Target.IsUnderConstruction() || Target.IsBeingSold() || !Target.IsKindOf(TEXT("STRUCTURE")))
	{
		return false;
	}
	return Target.GetHealth() < Target.GetMaxHealth();
}

bool UGenGameLogic::CanGetRepairedAt(const FGenObject& Unit, const FGenObject& RepairDest) const
{
	// ActionManager::canGetRepairedAt: damaged ground vehicles at a repair pad of their own side.
	// (Aircraft are mended at their airfield: see the land order.)
	if (Unit.IsDead() || RepairDest.IsDead() || GetRelationship(Unit.GetOwner(), RepairDest.GetOwner()) != EGenRelationship::Allies)
	{
		return false;
	}
	if (!Unit.CanMove() || Unit.IsUnderConstruction() || RepairDest.IsUnderConstruction() || RepairDest.IsBeingSold())
	{
		return false;
	}
	if (!Unit.IsKindOf(TEXT("VEHICLE")) || Unit.IsKindOf(TEXT("AIRCRAFT")) || !RepairDest.IsKindOf(TEXT("REPAIR_PAD")) || !RepairDest.GetTemplate()->bRepairDock)
	{
		return false;
	}
	return Unit.GetHealth() < Unit.GetMaxHealth();
}

void UGenGameLogic::IssueRepair(const TArray<FGenObjectID>& Dozers, FGenObjectID Building)
{
	PendingCommands.Add([this, Dozers, Building]()
	{
		FGenObject* B = FindObject(Building);
		for (FGenObjectID Id : Dozers)
		{
			FGenObject* D = FindObject(Id);
			if (B && D && CanRepairObject(*D, *B))
			{
				D->OnPlayerCommand(false);
				// (One repairer per building: the first to be told goes, the others stay where they are.)
				if (D->StartRepair(*B))
				{
					break;
				}
			}
		}
	});
}

void UGenGameLogic::IssueGetRepaired(const TArray<FGenObjectID>& Units, FGenObjectID RepairDest)
{
	PendingCommands.Add([this, Units, RepairDest]()
	{
		FGenObject* D = FindObject(RepairDest);
		for (FGenObjectID Id : Units)
		{
			FGenObject* U = FindObject(Id);
			if (D && U && D->GetDock() && CanGetRepairedAt(*U, *D))
			{
				U->OnPlayerCommand(true);
				U->StopConstructing();
				U->StartDock(*D);
			}
		}
	});
}

bool UGenGameLogic::CanSetRallyPoint(const FGenObject& Obj) const
{
	// InGameUI ACTIONTYPE_SET_RALLY_POINT
	return Obj.IsKindOf(TEXT("AUTO_RALLYPOINT")) && !Obj.IsDead() && !Obj.IsUnderConstruction() && !Obj.IsBeingSold();
}

bool UGenGameLogic::SetRallyPoint(FGenObject& Obj, const FVector3f& InPos)
{
	// GameLogicDispatch doSetRallyPoint: there has to be a way on foot from the building to the spot.
	FVector3f Pos = InPos;
	Pos.Z = GetGroundHeight(Pos.X, Pos.Y);
	const FVector3f Door = Obj.GetNaturalRallyPoint(true);
	FVector2f Start(Door.X, Door.Y);
	if (!Pathfinder.IsPassable(Start, GenSurface::Ground))
	{
		Pathfinder.AdjustDestination(FVector2f(Door.X, Door.Y), GenSurface::Ground, Start);
	}
	TArray<FVector2f> Path;
	const bool bOk = CanSetRallyPoint(Obj) && Pathfinder.IsPassable(FVector2f(Pos.X, Pos.Y), GenSurface::Ground)
		&& Pathfinder.FindPath(Start, FVector2f(Pos.X, Pos.Y), GenSurface::Ground, Path);
	if (bOk)
	{
		Obj.SetRallyPoint(Pos);
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [rally f%u] #%u %s rally point (%.0f, %.0f): %s"), Frame, Obj.GetID(), *Obj.GetTemplate()->Name, Pos.X, Pos.Y,
		bOk ? TEXT("set") : TEXT("no way there"));
	if (Obj.GetOwner() == LocalPlayer)
	{
		FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
		Text.Text = bOk ? TEXT("Rally point set") : TEXT("Units can't reach that spot");
		Text.Color = bOk ? FLinearColor(0.6f, 1.f, 0.6f) : FLinearColor(1.f, 0.5f, 0.4f);
		Text.Frame = Frame;
		Text.bScreen = true;
	}
	return bOk;
}

void UGenGameLogic::IssueSetRallyPoint(const TArray<FGenObjectID>& Buildings, const FVector3f& Pos)
{
	PendingCommands.Add([this, Buildings, Pos]()
	{
		for (FGenObjectID Id : Buildings)
		{
			FGenObject* B = FindObject(Id);
			if (B && CanSetRallyPoint(*B) && !SetRallyPoint(*B, Pos))
			{
				break; // (one refusal says it for all of them)
			}
		}
	});
}

void UGenGameLogic::IssueResumeConstruction(const TArray<FGenObjectID>& Dozers, FGenObjectID Building)
{
	PendingCommands.Add([this, Dozers, Building]()
	{
		FGenObject* B = FindObject(Building);
		if (!B || !B->IsUnderConstruction() || B->IsDead())
		{
			return;
		}
		// One dozer per building.
		for (FGenObjectID Id : Dozers)
		{
			FGenObject* D = FindObject(Id);
			if (D && !D->IsDead() && D->GetTemplate()->bIsDozer && D->GetOwner() == B->GetOwner())
			{
				D->OnPlayerCommand(false);
				ResumeConstruction(*D, *B);
				break;
			}
		}
	});
}

void UGenGameLogic::OnBuildingCompleted(FGenObject& Building)
{
	if (FGenSkirmishAI* AI = GetAI(Building.GetOwner()))
	{
		AI->OnStructureProduced(Building);
	}
}

void UGenGameLogic::IssueSell(FGenObjectID Building)
{
	PendingCommands.Add([this, Building]()
	{
		FGenObject* B = FindObject(Building);
		if (!B || B->IsDead() || B->IsBeingSold() || !B->GetTemplate()->IsKindOf(TEXT("STRUCTURE")))
		{
			return;
		}
		OnContainerSold(*B);
		// Whatever was queued is refunded.
		for (const FGenObject::FProduction& P : B->Production)
		{
			Players[B->GetOwner()].Money += P.Cost;
			if (P.Upgrade)
			{
				CancelUpgradeInProduction(B->GetOwner(), *P.Upgrade);
			}
			if (B->GetParking())
			{
				B->GetParking()->UnreserveDoorForExit(P.ExitDoor);
			}
		}
		B->Production.Reset();
		B->ClearAttack();
		B->SetModelCondition(Sold(), true);
		// BuildAssistant: the building comes down over three seconds, then the refund is paid.
		B->SellFrame = Frame + uint32(GenLogic::GetLogicFPS() * 3);
	});
}

void UGenGameLogic::FinishSell(const FGenObject& Building)
{
	if (!Players.IsValidIndex(Building.GetOwner()))
	{
		return;
	}
	// A site that was never finished gives everything back.
	const float Share = Building.IsUnderConstruction() ? 1.f : FGenGameData::Get().SellPercentage;
	Players[Building.GetOwner()].Money += FMath::RoundToInt32(Building.GetTemplate()->BuildCost * Share);
}

void UGenGameLogic::SpawnProducedUnit(const FGenObject& Factory, const FGenThingTemplate& Unit, int32 ExitDoor)
{
	if (FGenParkingPlace* PP = Factory.GetParking())
	{
		// ParkingPlaceBehavior::exitObjectViaDoor: a plane appears in its hangar and taxis to its parking
		// space; a helicopter appears on the helipad and lifts off.
		const bool bHelipad = Unit.IsKindOf(TEXT("PRODUCED_AT_HELIPAD"));
		FVector3f Pos = Factory.GetPosition();
		float Angle = Factory.GetOrientation();
		if (bHelipad)
		{
			PP->GetHeliPark(Pos, Angle);
		}
		Pos.Z = GetGroundHeight(Pos.X, Pos.Y);
		FGenObject* U = CreateObject(&Unit, Factory.GetOwner(), Pos, Angle);
		if (!U)
		{
			PP->UnreserveDoorForExit(ExitDoor);
			return;
		}
		U->Producer = Factory.GetID();
		FGenParkingInfo Info;
		if (bHelipad)
		{
			// ... to the rally point, or to a free spot in the air above
			U->CommandMoveTo(Factory.HasRallyPoint() ? Factory.GetRallyPoint() : Pos);
		}
		else if (PP->ClaimDoor(U->GetID(), ExitDoor) && PP->ReserveSpace(U->GetID(), Unit.JetParkingOffset, &Info))
		{
			U->SetPosition(FVector3f(Info.HangarInternal.X, Info.HangarInternal.Y, GetGroundHeight(Info.HangarInternal.X, Info.HangarInternal.Y)));
			U->SetOrientation(Info.HangarInternalOrient);
			U->SnapshotPrevious();
			if (U->bJetAI)
			{
				U->SetJetState(EGenJetState::TaxiFromHangar, Frame);
			}
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [jet f%u] #%u %s built at #%u %s (space %d)"), Frame, U->GetID(), *Unit.Name, Factory.GetID(),
			*Factory.GetTemplate()->Name, ExitDoor);
		if (FGenSkirmishAI* AI = GetAI(Factory.GetOwner()))
		{
			AI->OnUnitProduced(Factory, *U);
		}
		return;
	}
	// DefaultProductionExitUpdate::exitObjectViaDoor: appear at the create point inside the building,
	// facing the way the building faces, and drive out to the natural rally point.
	const FGenThingTemplate* FT = Factory.GetTemplate();
	const FTransform3f Xf = Factory.GetTransform();
	FVector3f Create = Factory.GetPosition();
	FVector3f Rally = Factory.GetPosition();
	if (FT->bHasExit)
	{
		Create = Xf.TransformPositionNoScale(FT->UnitCreatePoint);
		// A little beyond the door, so the next unit has room to come out.
		const FVector3f Out = FT->NaturalRallyPoint + FT->NaturalRallyPoint.GetSafeNormal() * (2.f * GenLogic::PathfindCellSize);
		Rally = Xf.TransformPositionNoScale(Out);
	}
	else
	{
		const float R = FT->GetBoundingCircleRadius();
		Create = Rally = FindPositionAround(Factory.GetPosition(), R + 10.f, R + 60.f, GenSurface::Ground);
	}
	Create.Z = GetGroundHeight(Create.X, Create.Y);
	if (FGenObject* U = CreateObject(&Unit, Factory.GetOwner(), Create, Factory.GetOrientation()))
	{
		// SupplyCenterProductionExitUpdate: gatherers leaving a supply center go straight to work, and
		// the flying ones don't bother with the rally point first.
		const bool bGatherer = FT->bSupplyCenter && Unit.bIsSupplyTruck;
		if (!(bGatherer && U->IsFlying()))
		{
			U->CommandMoveTo(FVector3f(Rally.X, Rally.Y, GetGroundHeight(Rally.X, Rally.Y)));
			// aiFollowExitProductionPath: out by the door first, then on to the building's rally point
			// (only what moves on the ground; the move there ends on the nearest free spot).
			if (Factory.HasRallyPoint() && !U->IsFlying())
			{
				U->bHasExitRally = true;
				U->ExitRally = Factory.GetRallyPoint();
			}
		}
		U->bForceWanting = bGatherer;
		if (FGenSkirmishAI* AI = GetAI(Factory.GetOwner()))
		{
			AI->OnUnitProduced(Factory, *U);
		}
	}
}
