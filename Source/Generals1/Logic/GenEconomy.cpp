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
	int32 ConstructionComplete() { static const int32 I = GenConditions::Index(TEXT("CONSTRUCTION_COMPLETE")); return I; }

	/** MODELCONDITION_DOOR_n_OPENING / _WAITING_OPEN / _CLOSING */
	enum { DoorOpening, DoorWaitingOpen, DoorClosing };
	int32 DoorCondition(int32 Door, int32 Which)
	{
		static int32 Index[4][3];
		static bool bInit = false;
		if (!bInit)
		{
			static const TCHAR* const Suffix[3] = { TEXT("OPENING"), TEXT("WAITING_OPEN"), TEXT("CLOSING") };
			for (int32 d = 0; d < 4; ++d)
			{
				for (int32 w = 0; w < 3; ++w)
				{
					Index[d][w] = GenConditions::Index(FString::Printf(TEXT("DOOR_%d_%s"), d + 1, Suffix[w]));
				}
			}
			bInit = true;
		}
		return Index[FMath::Clamp(Door, 0, 3)][Which];
	}

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

void FGenObject::UpdateFactoryDoors(uint32 Frame)
{
	// ProductionUpdate::updateDoors: opening for DoorOpeningTime, then standing open for DoorWaitOpenTime,
	// then closing for DoorCloseTime.
	for (int32 i = 0; i < 4; ++i)
	{
		FFactoryDoor& D = FactoryDoors[i];
		if (D.OpenedFrame != 0)
		{
			if (Frame - D.OpenedFrame > uint32(Template->DoorOpeningTime))
			{
				D.OpenedFrame = 0;
				D.WaitOpenFrame = Frame;
				SetModelCondition(DoorCondition(i, DoorOpening), false);
				SetModelCondition(DoorCondition(i, DoorWaitingOpen), true);
			}
		}
		else if (D.WaitOpenFrame != 0)
		{
			if (Frame - D.WaitOpenFrame > uint32(Template->DoorWaitOpenTime))
			{
				D.WaitOpenFrame = 0;
				D.ClosedFrame = Frame;
				SetModelCondition(DoorCondition(i, DoorWaitingOpen), false);
				SetModelCondition(DoorCondition(i, DoorClosing), true);
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [door f%u] #%u %s: door %d closes"), Frame, ID, *Template->Name, i + 1);
			}
		}
		else if (D.ClosedFrame != 0)
		{
			if (Frame - D.ClosedFrame > uint32(Template->DoorCloseTime))
			{
				D.ClosedFrame = 0;
				SetModelCondition(DoorCondition(i, DoorClosing), false);
			}
		}
	}
}

void FGenObject::UpdateProduction(uint32 Frame)
{
	// ProductionUpdate::update. (An airfield's hangar doors are its parking spaces' business: they stand
	// open for as long as an aircraft is at home - FGenParkingPlace::UpdateDoors.)
	const bool bDoors = Template->NumDoorAnimations > 0 && !Parking;
	if (bDoors)
	{
		UpdateFactoryDoors(Frame);
	}
	if (ConstructionCompleteFrame != 0 && Frame - ConstructionCompleteFrame > uint32(Template->ConstructionCompleteDuration))
	{
		ConstructionCompleteFrame = 0;
		SetModelCondition(ConstructionComplete(), false);
	}
	// QueueProductionExitUpdate::update: the door is free again one ExitDelay after somebody left by it.
	if (ExitDelayLeft > 0 && ExitBurstLeft <= 0)
	{
		--ExitDelayLeft;
	}
	// "when something is in the queue ... we are in the actively constructing state"
	SetModelCondition(ActivelyConstructing(), Production.Num() > 0);
	if (Production.Num() == 0)
	{
		return;
	}

	// The head of the queue advances one frame.
	Production[0].FramesDone += 1.f;
	if (Production[0].Upgrade)
	{
		// UpgradeTemplate::calcTimeToBuild: research takes its BuildTime, power or no power.
		if (Production[0].FramesDone >= Production[0].Upgrade->BuildTime * float(GenLogic::GetLogicFPS()))
		{
			const FGenUpgradeTemplate* Upgrade = Production[0].Upgrade;
			Production.RemoveAt(0);
			Logic->FinishUpgrade(*this, *Upgrade);
		}
		return;
	}
	const FGenThingTemplate* Unit = Production[0].Unit;
	if (Production[0].FramesDone < Logic->CalcTimeToBuild(*Unit, Owner))
	{
		return;
	}
	if (Production[0].QuantityRemaining < 0)
	{
		// ProductionUpdate's QuantityModifier: some things come in numbers for the price of one.
		Production[0].QuantityRemaining = 1;
		for (const TPair<FString, int32>& Q : Template->QuantityModifiers)
		{
			if (Q.Key.Equals(Unit->Name, ESearchCase::IgnoreCase) || Q.Key.Equals(Unit->Canonical()->Name, ESearchCase::IgnoreCase))
			{
				Production[0].QuantityRemaining = Q.Value;
			}
		}
	}
	// It is finished, and leaves when the door lets it: "if we can't we'll just keep this unit in the
	// production and keep "overbuilding" it, every frame we will check to see if we can exit".
	const int32 ToTry = Production[0].QuantityRemaining;
	const int32 ReservedDoor = Production[0].ExitDoor;
	for (int32 n = 0; n < ToTry; ++n)
	{
		int32 ExitDoor = n == 0 ? ReservedDoor : int32(FGenParkingPlace::DoorNoneNeeded);
		if (!Parking)
		{
			// The one door there is: always (DefaultProductionExitUpdate and the like), or - a barracks'
			// QueueProductionExitUpdate - when nobody has just gone out by it.
			const bool bFree = !Template->bQueueExit || ExitBurstLeft > 0 || ExitDelayLeft == 0;
			ExitDoor = bFree ? 0 : int32(FGenParkingPlace::DoorNoneAvailable);
		}
		if (ExitDoor == FGenParkingPlace::DoorNoneAvailable)
		{
			continue;
		}
		FFactoryDoor* Door = bDoors && ExitDoor >= 0 && ExitDoor < 4 ? &FactoryDoors[ExitDoor] : nullptr;
		if (Door)
		{
			if (Door->OpenedFrame == 0 && Door->WaitOpenFrame == 0 && Door->ClosedFrame == 0)
			{
				// closed: open it
				Door->OpenedFrame = Frame;
				SetModelCondition(DoorCondition(ExitDoor, DoorOpening), true);
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [door f%u] #%u %s: door %d opens for a finished %s"), Frame, ID, *Template->Name, ExitDoor + 1, *Unit->Name);
			}
			else if (Door->WaitOpenFrame != 0)
			{
				Door->WaitOpenFrame = Frame; // standing open: it stays so
			}
			else if (Door->ClosedFrame != 0)
			{
				// "if the door is closing, for now, pop it to waiting open"
				Door->ClosedFrame = 0;
				Door->WaitOpenFrame = Frame;
				SetModelCondition(DoorCondition(ExitDoor, DoorOpening), false);
				SetModelCondition(DoorCondition(ExitDoor, DoorClosing), false);
				SetModelCondition(DoorCondition(ExitDoor, DoorWaitingOpen), true);
			}
		}
		if (ConstructionCompleteFrame == 0)
		{
			ConstructionCompleteFrame = Frame;
			SetModelCondition(ConstructionComplete(), true);
		}
		// "for production buildings that have door animations we will not make the object until the door
		// has been totally opened"
		if (!Door || Door->WaitOpenFrame != 0)
		{
			Logic->SpawnProducedUnit(*this, *Unit, ExitDoor);
			UE_CLOG(Logic->IsTracing() && !Parking, LogGenerals, Log, TEXT("  [door f%u] #%u %s: a %s leaves"), Frame, ID, *Template->Name, *Unit->Name);
			if (Template->bQueueExit)
			{
				ExitDelayLeft = Template->ProductionExitDelay;
				ExitBurstLeft = FMath::Max(0, ExitBurstLeft - 1);
			}
			if (Production.Num() == 0)
			{
				return; // (the building changed hands, or was sold, by what the new unit set off)
			}
			--Production[0].QuantityRemaining;
		}
	}
	if (Production.Num() > 0 && Production[0].QuantityRemaining == 0)
	{
		Production.RemoveAt(0);
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
	// "the object is now no longer awaiting construction, it is being constructed" (and stays flagged as
	// that until it is finished, or the dozer is no more: the original never clears it when one drives off)
	B->SetModelCondition(AwaitingConstruction(), false);
	B->SetModelCondition(PartiallyConstructed(), true);
	B->SetModelCondition(ActivelyBeingConstructed(), true);
	B->LastBuiltFrame = Frame;

	const float FramesToBuild = Logic->CalcTimeToBuild(*B->Template, Owner);
	B->ConstructionPercent += 100.f / FramesToBuild;
	B->Health = FMath::Min(B->MaxHealth, B->Health + B->MaxHealth / FramesToBuild);
	if (B->ConstructionPercent >= 100.f)
	{
		B->ConstructionPercent = 100.f;
		B->bUnderConstruction = false;
		B->Health = B->MaxHealth;
		Logic->ClearMinesUnder(*B);
		Logic->ScoreObjectBuilt(*B);
		Logic->ScoreMoneySpent(B->GetOwner(), Logic->CalcCostToBuild(B->GetOwner(), *B->Template));
		B->SetModelCondition(AwaitingConstruction(), false);
		B->SetModelCondition(PartiallyConstructed(), false);
		B->SetModelCondition(ActivelyBeingConstructed(), false);
		if (Owner == Logic->GetLocalPlayer())
		{
			// DozerAIUpdate: "message the the building player" ("DOZER:ConstructionComplete") and the dozer's
			// VoiceTaskComplete.
			const FGenCommandData& CD = FGenCommandData::Get();
			FString Name = CD.GetText(B->Template->DisplayName);
			if (Name.IsEmpty() || Name == B->Template->DisplayName)
			{
				Name = B->Template->Name;
			}
			FString Message = CD.GetText(TEXT("DOZER:ConstructionComplete"));
			if (Message.IsEmpty() || Message.StartsWith(TEXT("DOZER:")))
			{
				Message = TEXT("%s construction complete.");
			}
			Message.ReplaceInline(TEXT("%ls"), TEXT("%s"));
			Message.ReplaceInline(TEXT("%s"), *Name);
			Logic->AddScreenMessage(Message);
			Logic->PlayObjectSound(*this, { TEXT("VoiceTaskComplete") });
		}
		StopConstructing();
		Logic->OnBuildingCompleted(*B);
	}
}

// ---- queries ------------------------------------------------------------------------------------

void UGenGameLogic::GetEnergy(int32 Player, int32& OutProduction, int32& OutConsumption) const
{
	OutProduction = OutConsumption = 0;
	for (const FGenObject* O : GetObjectsOf(Player))
	{
		const int32 Energy = O->GetEnergyProduction();
		if (Energy == 0 || O->IsDead() || O->IsUnderConstruction() || O->IsBeingSold())
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
	float Time = Frames / FMath::Max(Rate, 0.01f);
	// "Multiple build facilities can have an added production bonus": the time is multiplied by
	// MultipleFactory for every factory of the kind beyond the first. (GameData.ini has 1.0: none.)
	if (GD.MultipleFactory > 0.f && GD.MultipleFactory != 1.f && !Template.IsKindOf(TEXT("STRUCTURE")))
	{
		int32 Factories = 0;
		const FGenThingTemplate* Facility = nullptr;
		for (const FGenObject* O : GetObjectsOf(Player))
		{
			if (!O->IsDead() && !O->IsUnderConstruction() && O->GetTemplate()->bHasProduction
				&& (Facility ? O->GetTemplate()->Canonical() == Facility : CanFactoryMake(*O, Template)))
			{
				Facility = O->GetTemplate()->Canonical(); // ThingTemplate::getBuildFacilityTemplate: the first kind of building that can make it
				++Factories;
			}
		}
		for (int32 i = 1; i < Factories; ++i)
		{
			Time *= GD.MultipleFactory;
		}
	}
	return FMath::Max(1.f, Time);
}

bool UGenGameLogic::CanMake(int32 Player, const FGenThingTemplate& Template, FString* OutWhy, bool bCheckMoney) const
{
	const FGenGameData& GD = FGenGameData::Get();
	// Player::canBuild: "Buildable = No" never; Only_By_AI for the computer alone; Ignore_Prerequisites without them.
	if (Template.Buildable == 1 || (Template.Buildable == 2 && Players.IsValidIndex(Player) && !Players[Player].bSkirmishAI))
	{
		if (OutWhy) { *OutWhy = TEXT("Not available"); }
		return false;
	}
	static const TArray<TArray<FString>> NoPrerequisites;
	for (const TArray<FString>& AnyOf : Template.Buildable == 3 ? NoPrerequisites : Template.Prerequisites)
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
	if (bCheckMoney && GetMoney(Player) < CalcCostToBuild(Player, Template))
	{
		if (OutWhy) { *OutWhy = TEXT("Not enough money"); }
		return false;
	}
	return true;
}

bool UGenGameLogic::IsLocationLegalToBuild(const FGenThingTemplate& Template, const FVector3f& Pos, float Angle, const TCHAR** OutWhy) const
{
	auto Refuse = [OutWhy](const TCHAR* Why)
	{
		if (OutWhy) { *OutWhy = Why; }
		return false;
	};
	const FGenMapData* Map = GetMap();
	if (!Map)
	{
		return Refuse(TEXT("GUI:CantBuildThere"));
	}
	const bool bBox = Template.IsBox();
	const float Major = Template.GeometryMajorRadius;
	const float Minor = bBox ? Template.GeometryMinorRadius : Major;
	const FVector2f Center(Pos.X, Pos.Y);
	const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);

	// Nothing alive standing in the way, apart from mobile units (they get out of the way).
	const float Reach = FMath::Sqrt(Major * Major + Minor * Minor);
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (!O->IsTargetable() || O->CanMove())
		{
			continue;
		}
		// BuildAssistant::isRemovableForConstruction: "all shrubbery can be removed", "anything with a kindof
		// marked as cleared by build can be removed" (the site clears them when the building is laid down).
		if (O->IsKindOf(TEXT("SHRUBBERY")) || O->IsKindOf(TEXT("CLEARED_BY_BUILD")))
		{
			continue;
		}
		// "ignore land mines, cluster mines and demo traps, since you can build on them"; "same story for
		// inert things: ie, radiation fields, pings, and projectile streams"
		if (O->IsKindOf(TEXT("MINE")) || O->IsKindOf(TEXT("INERT")))
		{
			continue;
		}
		// BuildAssistant: something stealthed that hasn't been found doesn't give itself away by being in the way.
		if (O->IsStealthedAndUndetected() && GetRelationship(LocalPlayer, O->GetOwner()) != EGenRelationship::Allies)
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
			return Refuse(TEXT("GUI:CantBuildObjectsInTheWay"));
		}
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
				return Refuse(TEXT("GUI:CantBuildTooCloseToSupplies"));
			}
		}
	}

	// The terrain. "if (TheTerrainLogic->getLayerForDestination(worldPos) != LAYER_GROUND) { // we're on a
	// bridge. This is somewhat restricted." (The cursor's point is the deck's where it is in front of the ground.)
	if (Pathfinder.GetLayerForDestination(Pos, GetGroundHeight(Pos.X, Pos.Y)) != 0)
	{
		return Refuse(TEXT("GUI:CantBuildRestrictedTerrain"));
	}
	if (!Pathfinder.IsAreaClear(Center, Angle, Major, Minor, bBox))
	{
		return Refuse(TEXT("GUI:CantBuildRestrictedTerrain")); // cliff, water, map edge (another building: in the way, above)
	}

	// Flat enough: the footprint's corners and edge midpoints stay within the allowed height variation.
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
		return Refuse(TEXT("GUI:CantBuildNotFlatEnough"));
	}
	return true;
}

// ---- orders -------------------------------------------------------------------------------------

void UGenGameLogic::IssueQueueUnit(FGenObjectID Factory, const FString& UnitTemplate)
{
	FGenCommand C(EGenCommandType::QueueUnit);
	C.Units = { Factory };
	C.Name = UnitTemplate;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecQueueUnit(const FGenCommand& C)
{
	FGenObject* F = C.Units.Num() > 0 ? FindObject(C.Units[0]) : nullptr;
	const FGenThingTemplate* Unit = FGenGameData::Get().FindTemplate(C.Name);
	if (F && Unit)
	{
		QueueUnit(*F, *Unit);
	}
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
	const int32 Cost = CalcCostToBuild(Factory.GetOwner(), Unit);
	Withdraw(Factory.GetOwner(), Cost);
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
	if (Amount >= 0)
	{
		Deposit(Player, Amount);
	}
	else
	{
		Withdraw(Player, -Amount);
	}
}

void UGenGameLogic::Deposit(int32 Player, int32 Amount, bool bPlaySound)
{
	if (!Players.IsValidIndex(Player) || Amount <= 0)
	{
		return;
	}
	if (bPlaySound)
	{
		PlayMiscSound(TEXT("MoneyDepositSound"), Player);
	}
	Players[Player].Money += Amount;
}

int32 UGenGameLogic::Withdraw(int32 Player, int32 Amount, bool bPlaySound)
{
	if (!Players.IsValidIndex(Player))
	{
		return 0;
	}
	Amount = FMath::Min(Amount, Players[Player].Money);
	if (Amount <= 0)
	{
		return 0;
	}
	if (bPlaySound)
	{
		PlayMiscSound(TEXT("MoneyWithdrawSound"), Player);
	}
	Players[Player].Money -= Amount;
	return Amount;
}

void UGenGameLogic::IssueCancelUnit(FGenObjectID Factory, int32 QueueIndex)
{
	FGenCommand C(EGenCommandType::CancelUnit);
	C.Units = { Factory };
	C.A = QueueIndex;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecCancelUnit(const FGenCommand& C)
{
	FGenObject* F = C.Units.Num() > 0 ? FindObject(C.Units[0]) : nullptr;
	const int32 QueueIndex = C.A;
	if (F && F->Production.IsValidIndex(QueueIndex))
	{
		Deposit(F->GetOwner(), F->Production[QueueIndex].Cost);
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
}

void UGenGameLogic::IssueConstruct(FGenObjectID Dozer, const FString& BuildingTemplate, const FVector3f& Pos, float Angle)
{
	FGenCommand C(EGenCommandType::Construct);
	C.Units = { Dozer };
	C.Name = BuildingTemplate;
	C.Pos = Pos;
	C.F = Angle;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecConstruct(const FGenCommand& C)
{
	FGenObject* D = C.Units.Num() > 0 ? FindObject(C.Units[0]) : nullptr;
	const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(C.Name);
	if (!D || !T || D->IsDead() || !D->GetTemplate()->bIsDozer || !CanMake(D->GetOwner(), *T) || !IsLocationLegalToBuild(*T, C.Pos, C.F))
	{
		return;
	}
	D->OnPlayerCommand(false); // a worker on a supply run drops it
	ConstructBuilding(D, *T, C.Pos, C.F, D->GetOwner());
}

FGenObject* UGenGameLogic::ConstructBuilding(FGenObject* Dozer, const FGenThingTemplate& Template, const FVector3f& Pos, float Angle, int32 Player)
{
	// BuildAssistant::buildObjectNow
	if (!Players.IsValidIndex(Player))
	{
		return nullptr;
	}
	Withdraw(Player, CalcCostToBuild(Player, Template));

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
		if (O->IsDead() || O->IsProjectile() || O->CanMove())
		{
			continue;
		}
		// BuildAssistant::clearRemovableForConstruction: KINDOF_SHRUBBERY and KINDOF_CLEARED_BY_BUILD - lamp
		// posts, fences - make way wherever their own shape reaches into the building's ("iteratePotentialCollisions").
		bool bClear = !O->IsTargetable() && InFootprint(O->GetPosition(), 2.f);
		if (!bClear && (O->IsKindOf(TEXT("CLEARED_BY_BUILD")) || O->IsKindOf(TEXT("SHRUBBERY"))))
		{
			const FGenThingTemplate* OT = O->GetTemplate();
			const FVector2f Half(OT->GeometryMajorRadius, OT->IsBox() ? OT->GeometryMinorRadius : OT->GeometryMajorRadius);
			bClear = RectanglesOverlap(FVector2f(Pos.X, Pos.Y), Angle, FVector2f(Template.GeometryMajorRadius, Minor),
				FVector2f(O->GetPosition().X, O->GetPosition().Y), O->GetOrientation(), Half);
		}
		if (bClear)
		{
			DestroyObject(ObjectOrder[i]);
		}
	}

	if (Dozer)
	{
		// DozerAIUpdate::construct: "Flatten the terrain underneath the object, then adjust to the flattened height."
		FlattenTerrain(Template, Pos, Angle);
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
		ClearMinesUnder(*B);
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
	FGenCommand C(EGenCommandType::Repair);
	C.Units = Dozers;
	C.Target = Building;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecRepair(const FGenCommand& C)
{
	FGenObject* B = FindObject(C.Target);
	for (FGenObjectID Id : C.Units)
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
}

void UGenGameLogic::IssueGetRepaired(const TArray<FGenObjectID>& Units, FGenObjectID RepairDest)
{
	FGenCommand C(EGenCommandType::GetRepaired);
	C.Units = Units;
	C.Target = RepairDest;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecGetRepaired(const FGenCommand& C)
{
	FGenObject* D = FindObject(C.Target);
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (D && U && D->GetDock() && CanGetRepairedAt(*U, *D))
		{
			U->OnPlayerCommand(true);
			U->StopConstructing();
			U->StartDock(*D);
		}
	}
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
	FGenCommand C(EGenCommandType::SetRallyPoint);
	C.Units = Buildings;
	C.Pos = Pos;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecSetRallyPoint(const FGenCommand& C)
{
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* B = FindObject(Id);
		if (B && CanSetRallyPoint(*B) && !SetRallyPoint(*B, C.Pos))
		{
			break; // (one refusal says it for all of them)
		}
	}
}

void UGenGameLogic::IssueResumeConstruction(const TArray<FGenObjectID>& Dozers, FGenObjectID Building)
{
	FGenCommand C(EGenCommandType::ResumeConstruction);
	C.Units = Dozers;
	C.Target = Building;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecResumeConstruction(const FGenCommand& C)
{
	FGenObject* B = FindObject(C.Target);
	if (!B || !B->IsUnderConstruction() || B->IsDead())
	{
		return;
	}
	// One dozer per building.
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* D = FindObject(Id);
		if (D && !D->IsDead() && D->GetTemplate()->bIsDozer && D->GetOwner() == B->GetOwner())
		{
			D->OnPlayerCommand(false);
			ResumeConstruction(*D, *B);
			break;
		}
	}
}

void UGenGameLogic::OnBuildingCompleted(FGenObject& Building)
{
	// Player::onStructureConstructionComplete: "This object may require us to play some EVA sounds."
	for (const FGenSpecialPowerModule& M : Building.GetTemplate()->PowerModules)
	{
		if (const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(M.Power))
		{
			EvaSuperweapon(TEXT("Detected"), Building, *Power);
		}
	}
	if (Building.GetOwner() == LocalPlayer && Frame > 1)
	{
		// DozerAIUpdate: "make radar neat-o attention grabber event at build location"
		CreateRadarEvent(Building.GetPosition(), EGenRadarEvent::Construction);
	}
	if (FGenSkirmishAI* AI = GetAI(Building.GetOwner()))
	{
		AI->OnStructureProduced(Building);
	}
}

void UGenGameLogic::IssueCancelConstruction(FGenObjectID Building)
{
	FGenCommand C(EGenCommandType::CancelConstruction);
	C.Units = { Building };
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecCancelConstruction(const FGenCommand& C)
{
	// GameLogicDispatch MSG_DOZER_CANCEL_CONSTRUCT: "refund the money to the player", then "killing the building
	// will automatically cause the dozer also cancel its own building behavior".
	FGenObject* B = C.Units.Num() > 0 ? FindObject(C.Units[0]) : nullptr;
	if (!B || B->IsDead() || !B->IsUnderConstruction())
	{
		return;
	}
	Deposit(B->GetOwner(), CalcCostToBuild(B->GetOwner(), *B->GetTemplate()));
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [build f%u] #%u %s: construction cancelled"), Frame, B->GetID(), *B->GetTemplate()->Name);
	B->Kill();
}

void UGenGameLogic::IssueToggleOvercharge(FGenObjectID Building)
{
	FGenCommand C(EGenCommandType::ToggleOvercharge);
	C.Units = { Building };
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecToggleOvercharge(const FGenCommand& C)
{
	if (FGenObject* B = C.Units.Num() > 0 ? FindObject(C.Units[0]) : nullptr)
	{
		B->SetOvercharge(!B->IsOverchargeActive());
	}
}

void UGenGameLogic::IssueSell(FGenObjectID Building)
{
	FGenCommand C(EGenCommandType::Sell);
	C.Units = { Building };
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecSell(const FGenCommand& C)
{
	const FGenObjectID Building = C.Units.Num() > 0 ? C.Units[0] : GenInvalidObjectID;
	FGenObject* B = FindObject(Building);
	if (!B || B->IsDead() || B->IsBeingSold() || !B->GetTemplate()->IsKindOf(TEXT("STRUCTURE")))
	{
		return;
	}
	OnContainerSold(*B);
	// BuildAssistant::sellObject: "destroy any mines that are owned by this structure, right now"
	for (int32 i = bAnyMines ? ObjectOrder.Num() - 1 : -1; i >= 0; --i)
	{
		const FGenObject* MineObj = Objects[ObjectOrder[i]].Get();
		if (MineObj->GetProducer() == Building && MineObj->IsKindOf(TEXT("MINE")))
		{
			DestroyObject(ObjectOrder[i]);
		}
	}
	// Whatever was queued is refunded.
	for (const FGenObject::FProduction& P : B->Production)
	{
		Deposit(B->GetOwner(), P.Cost);
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
	// BuildAssistant::sellObject: its construction percent starts just under a hundred and is counted
	// down (FGenObject::Update); the scaffolding goes up as for a building site, at twice the pace.
	B->ConstructionPercent = 99.9f;
	B->SellFrame = FMath::Max(Frame, 1u);
	B->SetModelCondition(PartiallyConstructed(), true);
	B->SetModelCondition(ActivelyBeingConstructed(), true);
	B->SetDrawAnimDuration(GenLogic::GetLogicFPS() * 3 / 2);
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [build f%u] #%u %s is being sold"), Frame, B->GetID(), *B->GetTemplate()->Name);
}

void UGenGameLogic::FinishSell(const FGenObject& Building)
{
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [build f%u] #%u %s is sold"), Frame, Building.GetID(), *Building.GetTemplate()->Name);
	if (!Players.IsValidIndex(Building.GetOwner()))
	{
		return;
	}
	// A site that was never finished gives everything back.
	const float Share = Building.IsUnderConstruction() ? 1.f : FGenGameData::Get().SellPercentage;
	Deposit(Building.GetOwner(), FMath::RoundToInt32(float(CalcCostToBuild(Building.GetOwner(), *Building.GetTemplate())) * Share));
}

void UGenGameLogic::CancelAllProduction(FGenObject& Building)
{
	// ProductionUpdate::cancelAndRefundAllProduction
	if (!Players.IsValidIndex(Building.GetOwner()))
	{
		return;
	}
	for (const FGenObject::FProduction& P : Building.Production)
	{
		Deposit(Building.GetOwner(), P.Cost);
		if (P.Upgrade)
		{
			CancelUpgradeInProduction(Building.GetOwner(), *P.Upgrade);
		}
		if (Building.GetParking())
		{
			Building.GetParking()->UnreserveDoorForExit(P.ExitDoor);
		}
	}
	Building.Production.Reset();
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
		PlayObjectSound(*U, { TEXT("VoiceCreated") });
		PlayObjectSound(*U, { TEXT("VoiceCreate") });
		ScoreObjectBuilt(*U);
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
		PlayObjectSound(*U, { TEXT("VoiceCreated") });
		PlayObjectSound(*U, { TEXT("VoiceCreate") });
		ScoreObjectBuilt(*U);
		const bool bGatherer = FT->bSupplyCenter && Unit.bIsSupplyTruck;
		if (!(bGatherer && U->IsFlying()))
		{
			if (FT->bHasExit && !U->IsFlying() && U->CanMove())
			{
				// aiFollowExitProductionPath: straight out of the door along the exit path, the factory it was made
				// in no obstacle on the way (no path is looked for: one from inside the building went round
				// whichever side came first).
				U->StartDirectMove(FVector3f(Rally.X, Rally.Y, GetGroundHeight(Rally.X, Rally.Y)));
				U->IgnoreObstacle = Factory.GetID();
			}
			else
			{
				U->CommandMoveTo(FVector3f(Rally.X, Rally.Y, GetGroundHeight(Rally.X, Rally.Y)));
			}
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
