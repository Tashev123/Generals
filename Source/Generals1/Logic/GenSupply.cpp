// Supply gathering: warehouses and supply centers (docks), the docking sequence, and the gatherers'
// autopilot. Follows DockUpdate.cpp, SupplyWarehouseDockUpdate.cpp, SupplyCenterDockUpdate.cpp,
// AIDock.cpp, SupplyTruckAIUpdate.cpp, ResourceGatheringManager.cpp, ActionManager::canTransferSuppliesAt
// and SpawnBehavior.cpp (one-shot spawns). The same docking sequence serves RepairDockUpdate.cpp
// (vehicles mended at a war factory).

#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

namespace
{
	constexpr int32 DefaultApproachSlots = 10;      // DEFAULT_APPROACH_VECTOR_SIZE
	constexpr float RegroupSuccessDistance = 15.f;  // REGROUP_SUCCESS_DISTANCE_SQUARED = 225

	int32 CarryingBit() { static const int32 I = GenConditions::Index(TEXT("CARRYING")); return I; }
	int32 DockingBit() { static const int32 I = GenConditions::Index(TEXT("DOCKING")); return I; }
	int32 DockingBeginningBit() { static const int32 I = GenConditions::Index(TEXT("DOCKING_BEGINNING")); return I; }
	int32 DockingActiveBit() { static const int32 I = GenConditions::Index(TEXT("DOCKING_ACTIVE")); return I; }
	int32 DockingEndingBit() { static const int32 I = GenConditions::Index(TEXT("DOCKING_ENDING")); return I; }

	void ClearDockingConditions(FGenObject& Obj)
	{
		Obj.SetModelCondition(DockingBit(), false);
		Obj.SetModelCondition(DockingBeginningBit(), false);
		Obj.SetModelCondition(DockingActiveBit(), false);
		Obj.SetModelCondition(DockingEndingBit(), false);
	}

	const TCHAR* StageName(EGenDockStage Stage)
	{
		static const TCHAR* Names[] = { TEXT("none"), TEXT("approach"), TEXT("wait"), TEXT("advance"), TEXT("enter"), TEXT("dock"),
			TEXT("action"), TEXT("exit") };
		return Names[int32(Stage)];
	}
}

// ---- docks --------------------------------------------------------------------------------------

void FGenObject::InitSupply()
{
	if (!Template->bSupplyWarehouse && !Template->bSupplyCenter && !Template->bRepairDock)
	{
		return;
	}
	Dock = MakeUnique<FGenDock>();
	Dock->Boxes = Template->bSupplyWarehouse ? Template->StartingBoxes : 0;
	// A fixed number of waiting spots, or (-1) a list that grows as needed.
	const int32 Slots = Template->NumberApproachPositions < 0 ? DefaultApproachSlots : Template->NumberApproachPositions;
	Dock->ApproachPositions.Init(FVector3f::ZeroVector, Slots);
	Dock->ApproachOwners.Init(GenInvalidObjectID, Slots);
	Dock->ApproachReached.Init(false, Slots);
	SetModelCondition(CarryingBit(), Dock->Boxes > 0);
}

void FGenObject::LoadDockPositions()
{
	// DockUpdate::loadDockPositions: the dock's lane and its queue come from bones of the building's
	// model (DockStart, DockAction, DockEnd, DockWaitingNN), untouched by animation.
	FGenDock& D = *Dock;
	D.bPositionsLoaded = true;
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Assets || IsKindOf(TEXT("IGNORE_DOCKING_BONES")))
	{
		return;
	}
	for (const FGenDrawModule& Draw : Template->Draws)
	{
		const FString Model = Draw.DefaultModel();
		TArray<FTransform3f> Bones;
		if (Model.IsEmpty() || Assets->GetPristineBones(Model, TEXT("DockStart"), 0, 1, Bones) == 0)
		{
			continue;
		}
		D.bHasDockBones = true;
		D.EnterPos = Bones[0].GetTranslation();
		if (Assets->GetPristineBones(Model, TEXT("DockAction"), 0, 1, Bones) > 0) { D.DockPos = Bones[0].GetTranslation(); }
		if (Assets->GetPristineBones(Model, TEXT("DockEnd"), 0, 1, Bones) > 0) { D.ExitPos = Bones[0].GetTranslation(); }
		if (Template->NumberApproachPositions >= 0)
		{
			D.NumApproachBones = Assets->GetPristineBones(Model, TEXT("DockWaiting"), 1, Template->NumberApproachPositions, Bones);
			for (int32 i = 0; i < Bones.Num() && i < D.ApproachPositions.Num(); ++i)
			{
				D.ApproachPositions[i] = Bones[i].GetTranslation();
			}
		}
		break;
	}
}

bool FGenObject::IsDockOpen() const
{
	return Dock && !bDead && !bPendingDestroy && !bUnderConstruction && SellFrame == 0;
}

bool FGenObject::IsClearToApproach(FGenObjectID Docker) const
{
	if (Template->NumberApproachPositions < 0)
	{
		return true; // the queue grows
	}
	for (FGenObjectID SlotOwner : Dock->ApproachOwners)
	{
		if (SlotOwner == GenInvalidObjectID || SlotOwner == Docker)
		{
			return true;
		}
	}
	return false;
}

FVector3f FGenObject::ComputeApproachPosition(int32 Index, const FGenObject& Docker)
{
	FVector3f Spot = GetTransform().TransformPositionNoScale(Dock->ApproachPositions[Index]);
	if (Dock->NumApproachBones == 0)
	{
		// A dock without waiting bones puts the spot on the side the docker is coming from.
		Spot += (Docker.Position - Position).GetSafeNormal() * (Template->GeometryMajorRadius * 0.5f);
	}
	// (findPositionAround: the move there ends on the nearest free ground, or free air for aircraft.)
	return Spot;
}

bool FGenObject::ReserveApproachPosition(const FGenObject& Docker, FVector3f& OutPos, int32& OutIndex)
{
	FGenDock& D = *Dock;
	if (!D.bPositionsLoaded)
	{
		LoadDockPositions();
	}
	for (int32 i = 0; i < D.ApproachOwners.Num(); ++i)
	{
		if (D.ApproachOwners[i] == Docker.ID || D.ApproachOwners[i] == GenInvalidObjectID)
		{
			D.ApproachOwners[i] = Docker.ID;
			OutPos = ComputeApproachPosition(i, Docker);
			OutIndex = i;
			return true;
		}
	}
	if (Template->NumberApproachPositions < 0)
	{
		OutIndex = D.ApproachOwners.Add(Docker.ID);
		D.ApproachPositions.Add(FVector3f::ZeroVector);
		D.ApproachReached.Add(false);
		OutPos = ComputeApproachPosition(OutIndex, Docker);
		return true;
	}
	return false; // the dock is full
}

bool FGenObject::AdvanceApproachPosition(const FGenObject& Docker, FVector3f& OutPos, int32& InOutIndex)
{
	// DockUpdate::isClearToAdvance + advanceApproachPosition: move up when the spot ahead is free.
	FGenDock& D = *Dock;
	if (InOutIndex <= 0 || !D.ApproachOwners.IsValidIndex(InOutIndex) || D.ApproachOwners[InOutIndex] != Docker.ID
		|| !D.ApproachReached[InOutIndex] || D.ApproachOwners[InOutIndex - 1] != GenInvalidObjectID)
	{
		return false;
	}
	const int32 Ahead = InOutIndex - 1;
	D.ApproachOwners[Ahead] = Docker.ID;
	D.ApproachReached[Ahead] = false;
	D.ApproachOwners[InOutIndex] = GenInvalidObjectID;
	D.ApproachReached[InOutIndex] = false;
	OutPos = ComputeApproachPosition(Ahead, Docker);
	InOutIndex = Ahead;
	return true;
}

FVector3f FGenObject::GetDockPoint(const FVector3f& LocalBone, const FGenObject& Docker, bool bEntry) const
{
	// getEnterPosition / getDockPosition / getExitPosition
	if (!Dock->bHasDockBones)
	{
		// No lane: the docker is fine where it is, except that an aircraft comes in over the middle.
		return (bEntry && Docker.bFlies) ? Position : Docker.Position;
	}
	return GetTransform().TransformPositionNoScale(LocalBone);
}

void FGenObject::OnApproachReached(FGenObjectID Docker)
{
	const int32 Slot = Dock->ApproachOwners.IndexOfByKey(Docker);
	if (Slot != INDEX_NONE)
	{
		Dock->ApproachReached[Slot] = true;
	}
}

void FGenObject::OnEnterReached(FGenObject& Docker)
{
	for (FGenObject* O : { this, &Docker })
	{
		O->SetModelCondition(DockingEndingBit(), false);
		O->SetModelCondition(DockingBeginningBit(), true);
		O->SetModelCondition(DockingBit(), true);
	}
	Dock->bDockerInside = true;
	const int32 Slot = Dock->ApproachOwners.IndexOfByKey(Docker.ID);
	if (Slot != INDEX_NONE)
	{
		Dock->ApproachOwners[Slot] = GenInvalidObjectID;
		Dock->ApproachReached[Slot] = false;
	}
}

void FGenObject::OnExitReached(FGenObject& Docker)
{
	for (FGenObject* O : { this, &Docker })
	{
		O->SetModelCondition(DockingActiveBit(), false);
		O->SetModelCondition(DockingBit(), false);
		O->SetModelCondition(DockingEndingBit(), true);
	}
	Dock->bDockerInside = false;
	if (Dock->ActiveDocker == Docker.ID)
	{
		Dock->ActiveDocker = GenInvalidObjectID;
	}
}

void FGenObject::CancelDocker(FGenObject& Docker)
{
	// DockUpdate::cancelDock
	FGenDock& D = *Dock;
	for (int32 i = 0; i < D.ApproachOwners.Num(); ++i)
	{
		if (D.ApproachOwners[i] == Docker.ID)
		{
			D.ApproachOwners[i] = GenInvalidObjectID;
			D.ApproachReached[i] = false;
		}
	}
	if (D.ActiveDocker == Docker.ID)
	{
		D.ActiveDocker = GenInvalidObjectID;
		D.bDockerInside = false;
		ClearDockingConditions(*this);
	}
	if (D.LastRepair == Docker.ID)
	{
		// (Not in the original, where a vehicle that drives off half mended leaves its repair rate
		// behind for the next one.)
		D.LastRepair = GenInvalidObjectID;
	}
}

void FGenObject::UpdateDock(uint32 Frame)
{
	FGenDock& D = *Dock;
	// Dockers that died or were sent elsewhere give up their place.
	auto StillComing = [this](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return O && !O->bDead && !O->bPendingDestroy && O->DockTarget == ID;
	};
	for (int32 i = 0; i < D.ApproachOwners.Num(); ++i)
	{
		if (D.ApproachOwners[i] != GenInvalidObjectID && !StillComing(D.ApproachOwners[i]))
		{
			D.ApproachOwners[i] = GenInvalidObjectID;
			D.ApproachReached[i] = false;
		}
	}
	if (D.ActiveDocker != GenInvalidObjectID && !StillComing(D.ActiveDocker))
	{
		D.ActiveDocker = GenInvalidObjectID;
		D.bDockerInside = false;
		ClearDockingConditions(*this);
	}

	// DockUpdate::update: with the dock free, the first docker waiting at its spot is let in.
	if (D.ActiveDocker == GenInvalidObjectID)
	{
		for (int32 i = 0; i < D.ApproachReached.Num(); ++i)
		{
			if (D.ApproachReached[i])
			{
				D.ActiveDocker = D.ApproachOwners[i];
				break;
			}
		}
	}
}

bool FGenObject::DockAction(FGenObject& Docker)
{
	if (Template->bRepairDock)
	{
		// RepairDockUpdate::action: however bad the damage, the repair takes TimeForFullHeal; the health
		// to add per frame is worked out when the vehicle arrives.
		FGenDock& D = *Dock;
		if (D.LastRepair == GenInvalidObjectID)
		{
			D.LastRepair = Docker.ID;
			D.HealthPerFrame = (Docker.MaxHealth - Docker.Health) / float(Template->RepairDockFrames);
		}
		if (Docker.Health >= Docker.MaxHealth)
		{
			D.LastRepair = GenInvalidObjectID;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [repair f%u] #%u %s mended at #%u %s"), Logic->GetFrame(), Docker.ID, *Docker.Template->Name,
				ID, *Template->Name);
			return false; // done: the vehicle leaves
		}
		Docker.AttemptHealing(D.HealthPerFrame);
		return true;
	}
	if (Template->bSupplyWarehouse)
	{
		// SupplyWarehouseDockUpdate::action: one box per call, while there are boxes and room for them.
		if (Dock->Boxes <= 0 || Docker.Boxes >= Docker.Template->MaxBoxes)
		{
			return false;
		}
		// The docker has to be reasonably close: no more than its own diameter between the two.
		const float Gap = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Docker.Position.X, Docker.Position.Y))
			- Template->GetBoundingCircleRadius() - Docker.Template->GetBoundingCircleRadius();
		if (Gap > Docker.Template->GetBoundingCircleRadius() * 2.f)
		{
			return false;
		}
		--Dock->Boxes;
		Docker.SetBoxes(Docker.Boxes + 1);
		SetModelCondition(CarryingBit(), Dock->Boxes > 0);
		if (Dock->Boxes == 0 && Template->bDeleteWhenEmpty)
		{
			bPendingDestroy = true; // a used-up pile disappears
			return false;
		}
		return true;
	}

	// SupplyCenterDockUpdate::action: every box turns into money.
	// (SupplyTruckAIUpdate::getUpgradedSupplyBoost: every load is worth more once Supply Lines is researched.)
	const int32 Boost = Logic->HasUpgradeComplete(Docker.Owner, TEXT("Upgrade_AmericaSupplyLines")) ? Docker.Template->UpgradedSupplyBoost : 0;
	const int32 Value = Docker.Boxes * FGenGameData::Get().ValuePerSupplyBox + (Docker.Boxes > 0 ? Boost : 0);
	Docker.SetBoxes(0);
	if (Value > 0)
	{
		Logic->DepositSupplies(Owner, Value, Docker);
	}
	return false;
}

// ---- dockers ------------------------------------------------------------------------------------

bool FGenObject::IsAIIdle() const
{
	// (JetAIUpdate::isIdle: an aircraft busy with its airfield routine, or with an order waiting, is not idle.)
	return !bMoving && DockTarget == GenInvalidObjectID && BuildTarget == GenInvalidObjectID && RepairTarget == GenInvalidObjectID
		&& TargetID == GenInvalidObjectID && AIMode == EGenAIMode::None && JetState == EGenJetState::None && !bJetPendingCommand
		&& !bHasExitRally && HackState == EHackState::None;
}

FString FGenObject::DescribeBusy() const
{
	// (For the test harness: which of IsAIIdle's conditions keeps it from being idle.)
	TArray<FString> Why;
	if (bMoving) { Why.Add(TEXT("moving")); }
	if (DockTarget != GenInvalidObjectID) { Why.Add(TEXT("docking")); }
	if (BuildTarget != GenInvalidObjectID) { Why.Add(TEXT("building")); }
	if (RepairTarget != GenInvalidObjectID) { Why.Add(TEXT("repairing")); }
	if (TargetID != GenInvalidObjectID) { Why.Add(FString::Printf(TEXT("target #%u"), TargetID)); }
	if (AIMode != EGenAIMode::None) { Why.Add(FString::Printf(TEXT("ai mode %d"), int32(AIMode))); }
	if (JetState != EGenJetState::None || bJetPendingCommand) { Why.Add(TEXT("jet")); }
	if (bHasExitRally) { Why.Add(TEXT("exit rally")); }
	if (HackState != EHackState::None) { Why.Add(TEXT("hacking")); }
	if (SupplyState != EGenSupplyState::Idle) { Why.Add(FString::Printf(TEXT("supply %d"), int32(SupplyState))); }
	return Why.Num() > 0 ? FString::Join(Why, TEXT(", ")) : FString(TEXT("idle"));
}

void FGenObject::SetBoxes(int32 NewBoxes)
{
	Boxes = FMath::Max(0, NewBoxes);
	SetModelCondition(CarryingBit(), Boxes > 0); // Drawable::updateDrawableSupplyStatus
}

void FGenObject::StopDocking()
{
	// AIDockMachine::halt: say goodbye, or our spot stays taken forever.
	if (DockTarget == GenInvalidObjectID)
	{
		return;
	}
	FGenObject* D = Logic->FindObject(DockTarget);
	if (D && D->Dock)
	{
		D->CancelDocker(*this);
	}
	DockTarget = GenInvalidObjectID;
	DockStage = EGenDockStage::None;
	DockApproachIndex = -1;
	ClearDockingConditions(*this);
}

bool FGenObject::StartDock(FGenObject& DockObj)
{
	// AIUpdateInterface::aiDock + AIDockApproachState::onEnter
	if (!DockObj.Dock || !CanMove() || bDead)
	{
		return false;
	}
	StopDocking();
	ClearAttack();
	StopConstructing();
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	FVector3f Spot;
	int32 Slot = -1;
	if (!DockObj.IsDockOpen() || !DockObj.ReserveApproachPosition(*this, Spot, Slot))
	{
		return false;
	}
	DockTarget = DockObj.ID;
	DockStage = EGenDockStage::Approach;
	DockApproachIndex = Slot;
	if (FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Spot.X, Spot.Y)) < 2.f)
	{
		CommandStop(); // already there
	}
	else if (!StartPath(Spot, false))
	{
		StopDocking();
		return false;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [supply f%u] #%u %s heads for #%u %s (boxes %d)"), Logic->GetFrame(), ID, *Template->Name,
		DockObj.ID, *DockObj.Template->Name, Boxes);
	return true;
}

void FGenObject::UpdateDocking(uint32 Frame)
{
	// AIDockMachine: approach -> wait for clearance (-> advance) -> entry -> dock -> action -> exit.
	FGenObject* D = Logic->FindObject(DockTarget);
	if (!D || !D->Dock || D->bDead || D->bPendingDestroy)
	{
		StopDocking();
		CommandStop();
		return;
	}
	FGenDock& DK = *D->Dock;
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	// Lanes run through buildings that allow it; aircraft go straight anywhere.
	const bool bThrough = bFlies || D->Template->bDockAllowsPassthrough;
	auto MoveTo = [this](const FVector3f& P, bool bDirect)
	{
		if (FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(P.X, P.Y)) < 1.f)
		{
			CommandStop();
		}
		else if (bDirect)
		{
			StartDirectMove(P);
		}
		else if (!StartPath(P, false))
		{
			CommandStop(); // nowhere closer to go
		}
	};
	auto Fail = [this, Frame, FPS]()
	{
		StopDocking();
		CommandStop();
		NextSupplyThinkFrame = Frame + FPS;
	};
	const EGenDockStage Before = DockStage;

	switch (DockStage)
	{
	case EGenDockStage::Approach:
	case EGenDockStage::AdvancePosition:
		if (!D->IsDockOpen())
		{
			Fail();
		}
		else if (!bMoving)
		{
			D->OnApproachReached(ID);
			DockStage = EGenDockStage::WaitForClearance;
			DockTimer = Frame;
		}
		break;

	case EGenDockStage::WaitForClearance:
	{
		FVector3f Spot;
		if (!D->IsDockOpen())
		{
			Fail();
		}
		else if (DK.ActiveDocker == ID)
		{
			DockStage = EGenDockStage::MoveToEntry;
			DockApproachIndex = -1;
			MoveTo(D->GetDockPoint(DK.EnterPos, *this, true), bFlies);
		}
		else if (D->AdvanceApproachPosition(*this, Spot, DockApproachIndex))
		{
			DockStage = EGenDockStage::AdvancePosition;
			MoveTo(Spot, false);
		}
		else if (Frame > DockTimer + 30 * FPS)
		{
			Fail(); // waited half a minute: try again from scratch
		}
		break;
	}

	case EGenDockStage::MoveToEntry:
		if (!D->IsDockOpen())
		{
			Fail();
		}
		else if (!bMoving)
		{
			D->OnEnterReached(*this);
			DockStage = EGenDockStage::MoveToDock;
			MoveTo(D->GetDockPoint(DK.DockPos, *this, false), bThrough);
		}
		break;

	case EGenDockStage::MoveToDock:
		if (!D->IsDockOpen())
		{
			Fail();
		}
		else if (!bMoving)
		{
			for (FGenObject* O : { D, this })
			{
				O->SetModelCondition(DockingBeginningBit(), false);
				O->SetModelCondition(DockingActiveBit(), true);
			}
			DockStage = EGenDockStage::ProcessDock;
			// SupplyTruckAIUpdate::getActionDelayForDock
			DockTimer = Frame + uint32(D->Template->bSupplyWarehouse ? Template->SupplyWarehouseActionDelay : Template->SupplyCenterActionDelay);
		}
		break;

	case EGenDockStage::ProcessDock:
		if (Frame < DockTimer)
		{
			break;
		}
		DockTimer = Frame + uint32(D->Template->bSupplyWarehouse ? Template->SupplyWarehouseActionDelay : Template->SupplyCenterActionDelay);
		// The dock's action is repeated until it says we are done.
		if (!D->IsDockOpen() || !D->DockAction(*this))
		{
			DockStage = EGenDockStage::MoveToExit;
			MoveTo(D->GetDockPoint(DK.ExitPos, *this, false), bThrough);
		}
		break;

	case EGenDockStage::MoveToExit:
		if (!bMoving)
		{
			D->OnExitReached(*this);
			DockTarget = GenInvalidObjectID;
			DockStage = EGenDockStage::None;
			DockApproachIndex = -1;
			SetModelCondition(DockingEndingBit(), false);
			D->SetModelCondition(DockingEndingBit(), false);
		}
		break;

	default:
		break;
	}

	UE_CLOG(Logic->IsTracing() && DockStage != Before, LogGenerals, Log, TEXT("  [supply f%u] #%u %s at #%u %s: %s -> %s (boxes %d, stock %d)"),
		Frame, ID, *Template->Name, D->ID, *D->Template->Name, StageName(Before), StageName(DockStage), Boxes, DK.Boxes);
}

// ---- gatherers ----------------------------------------------------------------------------------

void FGenObject::OnPlayerCommand(bool bDockOrder)
{
	if (!bDockOrder)
	{
		StopDocking();
	}
	AIMode = EGenAIMode::None;
	if (CombatDrop && !CombatDrop->bDropping)
	{
		EndCombatDrop(); // ... and the flight to a combat drop (once the ropes are out, orders wait)
	}
	bHasExitRally = false; // an order replaces the trip to the rally point
	SetChinookHealAirfield(GenInvalidObjectID); // ... and a helicopter's stay at an airfield (ChinookAIUpdate::aiDoCommand)
	StopEntering();        // ... and the walk to a transport or a building
	ExitAbility(false);    // ... and a capture under way
	HuntButton.Reset();
	bHackInterrupt = HackState != EHackState::None; // ... and a hacker's work at the internet (once it has packed up)
	if (Template->bIsSupplyTruck)
	{
		// SupplyTruckStateMachine: anything the player orders takes the truck off autopilot; it goes back
		// on when it is sent to a warehouse or a supply center.
		SupplyState = EGenSupplyState::Busy;
		bForceWanting = false;
		NextSupplyThinkFrame = 0;
	}
}

bool FGenObject::StartRegroup()
{
	// RegroupingState::onEnter: nothing to gather, so wait near a supply center, failing that the
	// command center, failing that any building of ours.
	const FGenObject* Home = nullptr;
	for (const TCHAR* Kind : { TEXT("CASH_GENERATOR"), TEXT("COMMANDCENTER"), TEXT("STRUCTURE") })
	{
		Home = Logic->FindClosestOwnedByKindOf(*this, Kind);
		if (Home)
		{
			break;
		}
	}
	if (!Home)
	{
		return false;
	}
	const float Gap = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Home->Position.X, Home->Position.Y))
		- Template->GetBoundingCircleRadius() - Home->Template->GetBoundingCircleRadius();
	if (Gap < RegroupSuccessDistance)
	{
		return true; // already home
	}
	return StartPath(Home->Position, false); // ends on the nearest free spot beside it
}

void FGenObject::UpdateSupplyTruck(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Supply");
	// SupplyTruckStateMachine
	const bool bDocking = DockTarget != GenInvalidObjectID;
	const bool bIdle = IsAIIdle();
	switch (SupplyState)
	{
	case EGenSupplyState::Busy:
		if (bDocking) { SupplyState = EGenSupplyState::Docking; }
		else if (bIdle) { SupplyState = EGenSupplyState::Idle; }
		break;
	case EGenSupplyState::Idle:
		if (bForceWanting) { SupplyState = EGenSupplyState::Wanting; }
		else if (bDocking) { SupplyState = EGenSupplyState::Docking; }
		else if (!bIdle) { SupplyState = EGenSupplyState::Busy; }
		break;
	case EGenSupplyState::Docking:
		// Done with one dock: on to the next (warehouse -> supply center -> warehouse ...).
		if (!bDocking) { SupplyState = bIdle ? EGenSupplyState::Wanting : EGenSupplyState::Busy; }
		break;
	case EGenSupplyState::Regrouping:
		if (bIdle) { SupplyState = EGenSupplyState::Wanting; }
		break;
	default:
		break;
	}

	if (SupplyState != EGenSupplyState::Wanting || Frame < NextSupplyThinkFrame)
	{
		return;
	}
	// SupplyTruckWantsToPickUpOrDeliverBoxesState: loaded trucks want a supply center, empty ones a warehouse.
	bForceWanting = false;
	FGenObject* Best = Boxes > 0 ? Logic->FindBestSupplyCenter(*this) : Logic->FindBestSupplyWarehouse(*this);
	if (Best && StartDock(*Best))
	{
		SupplyState = EGenSupplyState::Docking;
		return;
	}
	// Nothing to do right now: go home, and look again in a second.
	NextSupplyThinkFrame = Frame + uint32(GenLogic::GetLogicFPS());
	SupplyState = EGenSupplyState::Regrouping;
	StartRegroup();
}

// ---- services -----------------------------------------------------------------------------------

bool UGenGameLogic::CanTransferSuppliesAt(const FGenObject& Truck, const FGenObject& DockObj) const
{
	// ActionManager::canTransferSuppliesAt
	const FGenThingTemplate* DockT = DockObj.GetTemplate();
	if (!Truck.GetTemplate()->bIsSupplyTruck || !DockObj.GetDock() || Truck.IsDead() || !DockObj.IsDockOpen()
		|| (!DockT->bSupplyWarehouse && !DockT->bSupplyCenter))
	{
		return false;
	}
	if (DockObj.GetTemplate()->bSupplyWarehouse)
	{
		// A warehouse must have boxes left and not belong to an enemy.
		return DockObj.GetDock()->Boxes > 0 && GetRelationship(Truck.GetOwner(), DockObj.GetOwner()) != EGenRelationship::Enemies;
	}
	// A supply center must be ours, and we must have something to deliver.
	return Truck.Boxes > 0 && Truck.GetOwner() == DockObj.GetOwner();
}

FGenObject* UGenGameLogic::FindBestSupplyWarehouse(const FGenObject& Truck) const
{
	// ResourceGatheringManager::findBestSupplyWarehouse: the dock the player picked if it still works,
	// otherwise the nearest one within the truck's scan distance.
	auto Usable = [this, &Truck](const FGenObject* D)
	{
		return D && CanTransferSuppliesAt(Truck, *D) && D->IsClearToApproach(Truck.GetID());
	};
	FGenObject* Preferred = FindObject(Truck.PreferredDock);
	if (Preferred && Preferred->GetTemplate()->bSupplyWarehouse && Usable(Preferred))
	{
		return Preferred;
	}
	// Computer players look twice as far.
	const bool bComputer = Players.IsValidIndex(Truck.GetOwner()) && !Players[Truck.GetOwner()].bHuman;
	const float Scan = Truck.GetTemplate()->SupplyWarehouseScanDistance * (bComputer ? 2.f : 1.f);
	FGenObject* Best = nullptr;
	float BestDistSq = Scan * Scan;
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* D = Objects[Id].Get();
		if (!D->GetTemplate()->bSupplyWarehouse || !Usable(D))
		{
			continue;
		}
		const float DistSq = FVector3f::DistSquared(D->GetPosition(), Truck.GetPosition());
		if (DistSq < BestDistSq)
		{
			Best = D;
			BestDistSq = DistSq;
		}
	}
	return Best;
}

FGenObject* UGenGameLogic::FindBestSupplyCenter(const FGenObject& Truck) const
{
	auto Usable = [this, &Truck](const FGenObject* D)
	{
		return D && CanTransferSuppliesAt(Truck, *D) && D->IsClearToApproach(Truck.GetID());
	};
	FGenObject* Preferred = FindObject(Truck.PreferredDock);
	if (Preferred && Preferred->GetTemplate()->bSupplyCenter && Usable(Preferred))
	{
		return Preferred;
	}
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* D = Objects[Id].Get();
		if (!D->GetTemplate()->bSupplyCenter || !Usable(D))
		{
			continue;
		}
		const float DistSq = FVector3f::DistSquared(D->GetPosition(), Truck.GetPosition());
		if (DistSq < BestDistSq)
		{
			Best = D;
			BestDistSq = DistSq;
		}
	}
	return Best;
}

const FGenObject* UGenGameLogic::FindClosestOwnedByKindOf(const FGenObject& From, const TCHAR* Kind) const
{
	// Player::findClosestByKindOf
	const FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (O == &From || O->GetOwner() != From.GetOwner() || O->IsDead() || O->IsDestroyed() || !O->IsKindOf(Kind))
		{
			continue;
		}
		const float DistSq = FVector3f::DistSquared(O->GetPosition(), From.GetPosition());
		if (DistSq < BestDistSq)
		{
			Best = O;
			BestDistSq = DistSq;
		}
	}
	return Best;
}

void UGenGameLogic::AdjustAirDestination(const FGenObject& Unit, FVector2f& InOutPos) const
{
	// Pathfinder::adjustDestination for aircraft: the map's cells keep aircraft goals apart from the
	// ground layer, so helicopters don't stack on one spot but ignore whatever is below them.
	const float Radius = Unit.GetTemplate()->GetBoundingCircleRadius();
	auto IsFree = [this, &Unit, Radius](const FVector2f& P)
	{
		if (!Pathfinder.IsPassable(P, GenSurface::Air))
		{
			return false;
		}
		for (FGenObjectID Id : ObjectOrder)
		{
			const FGenObject* O = Objects[Id].Get();
			if (O == &Unit || !O->IsFlying() || O->IsParked() || O->IsDead() || O->IsDestroyed() || O->IsProjectile())
			{
				continue;
			}
			const FVector2f Other = O->IsMoving() ? FVector2f(O->Goal.X, O->Goal.Y) : FVector2f(O->GetPosition().X, O->GetPosition().Y);
			if (FVector2f::DistSquared(P, Other) < FMath::Square(Radius + O->GetTemplate()->GetBoundingCircleRadius()))
			{
				return false;
			}
		}
		return true;
	};
	if (IsFree(InOutPos))
	{
		return;
	}
	const float Step = FMath::Max(Radius, GenLogic::PathfindCellSize);
	for (int32 Ring = 1; Ring <= 12; ++Ring)
	{
		const int32 Samples = 8 * Ring;
		for (int32 k = 0; k < Samples; ++k)
		{
			const float A = k * (2.f * PI / float(Samples));
			const FVector2f P = InOutPos + FVector2f(FMath::Cos(A), FMath::Sin(A)) * (Step * float(Ring));
			if (IsFree(P))
			{
				InOutPos = P;
				return;
			}
		}
	}
}

void UGenGameLogic::DepositSupplies(int32 Player, int32 Value, const FGenObject& Docker)
{
	if (!Players.IsValidIndex(Player))
	{
		return;
	}
	Deposit(Player, Value);
	ScoreMoneyEarned(Player, Value);
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [supply f%u] #%u %s delivers $%d -> player %d has $%d"), Frame, Docker.GetID(), *Docker.GetTemplate()->Name,
		Value, Player, Players[Player].Money);

	// "+$600" floating up from the truck, in the owner's color (TheInGameUI->addFloatingText).
	FString Text = FGenCommandData::Get().GetText(TEXT("GUI:AddCash"));
	if (!Text.Contains(TEXT("%d")))
	{
		Text = TEXT("+$%d");
	}
	FGenFloatingText& F = FloatingTexts.AddDefaulted_GetRef();
	F.Text = Text.Replace(TEXT("%d"), *FString::FromInt(Value));
	F.Pos = Docker.GetPosition();
	F.Pos.Z = GetGroundHeight(F.Pos.X, F.Pos.Y);
	F.Color = Players[Player].Color;
	F.Frame = Frame;
}

void UGenGameLogic::SpawnBuildingUnits(const FGenObject& Building)
{
	// SpawnBehavior (OneShot): the units leave by the building's exit, like produced ones.
	const FGenThingTemplate* Unit = FGenGameData::Get().FindTemplate(Building.GetTemplate()->SpawnTemplate);
	if (!Unit)
	{
		return;
	}
	for (int32 i = 0; i < Building.GetTemplate()->SpawnNumber; ++i)
	{
		SpawnProducedUnit(Building, *Unit);
	}
}

void UGenGameLogic::IssueDock(const TArray<FGenObjectID>& Units, FGenObjectID DockId)
{
	FGenCommand C(EGenCommandType::Dock);
	C.Units = Units;
	C.Target = DockId;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecDock(const FGenCommand& C)
{
	FGenObject* D = FindObject(C.Target);
	if (!D || !D->GetDock())
	{
		return;
	}
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (!U || !CanTransferSuppliesAt(*U, *D))
		{
			continue;
		}
		// SupplyTruckAIUpdate::privateDock: a dock picked by the player is remembered and preferred
		// over the nearest one from then on.
		U->OnPlayerCommand(true);
		U->PreferredDock = C.Target;
		U->StartDock(*D);
	}
}
