// Units' special abilities (the state machine: walk up, unpack, prepare, do it, pack; see GenHackers.cpp
// for what they leave in the world), what a change of owner brings with it, and the tech buildings.
// Follows SpecialAbilityUpdate.cpp, SpecialAbility.cpp, ActionManager::canCaptureBuilding,
// Object::defect / onCapture, Player::becomingTeamMember, AutoDepositUpdate.cpp, TechBuildingBehavior.cpp,
// CostModifierUpgrade.cpp and CommandButtonHuntUpdate.cpp.

#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioData.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenSkirmishAI.h"
#include "Logic/GenUpgrades.h"
#include "Generals1.h"

namespace
{
	int32 UnpackingBit() { static const int32 I = GenConditions::Index(TEXT("UNPACKING")); return I; }
	int32 PackingBit() { static const int32 I = GenConditions::Index(TEXT("PACKING")); return I; }
	int32 RaisingFlagBit() { static const int32 I = GenConditions::Index(TEXT("RAISING_FLAG")); return I; }
	int32 FiringABit() { static const int32 I = GenConditions::Index(TEXT("FIRING_A")); return I; }
	int32 CapturedBit() { static const int32 I = GenConditions::Index(TEXT("CAPTURED")); return I; }

	bool IsCapturePower(const FGenSpecialPowerTemplate* Power)
	{
		return Power && (Power->AbilityKind == EGenAbilityKind::CaptureBuilding || Power->AbilityKind == EGenAbilityKind::LotusCapture);
	}

	/** PartitionManager::getDistanceSquared(FROM_BOUNDINGSPHERE_2D): the gap between two objects' bounding circles. */
	float BoundingGap(const FGenObject& A, const FGenObject& B)
	{
		const float Dist = FVector2f::Distance(FVector2f(A.GetPosition().X, A.GetPosition().Y), FVector2f(B.GetPosition().X, B.GetPosition().Y));
		return FMath::Max(0.f, Dist - A.GetTemplate()->GetBoundingCircleRadius() - B.GetTemplate()->GetBoundingCircleRadius());
	}
}

// ---- who may capture what -----------------------------------------------------------------------

bool UGenGameLogic::CanCaptureBuilding(const FGenObject& Unit, const FGenObject& Target, bool bCheckReady) const
{
	// The unit must have the ability, and have it ready.
	int32 Module = INDEX_NONE;
	const TArray<FGenSpecialPowerModule>& Modules = Unit.GetTemplate()->PowerModules;
	for (int32 i = 0; i < Modules.Num(); ++i)
	{
		if (Modules[i].Type == EGenPowerModule::SpecialAbility && IsCapturePower(FGenUpgradeData::Get().FindPower(Modules[i].Power)))
		{
			Module = i;
			break;
		}
	}
	if (Module == INDEX_NONE || (bCheckReady && !IsPowerReady(Unit, Module)))
	{
		return false;
	}
	if (Target.IsKindOf(TEXT("IMMUNE_TO_CAPTURE")) || Target.IsDead() || Target.IsDestroyed() || !Target.IsKindOf(TEXT("STRUCTURE"))
		|| Target.IsUnderConstruction() || Target.IsBeingSold())
	{
		return false;
	}
	// "ensure that it's capturable, and not allied. exception: we can always capture enemy bldgs, regardless of kindof"
	const EGenRelationship R = GetRelationship(Unit.GetOwner(), Target.GetOwner());
	if (Target.GetOwner() == Unit.GetOwner() || !(R == EGenRelationship::Enemies || (Target.IsKindOf(TEXT("CAPTURABLE")) && R != EGenRelationship::Allies)))
	{
		return false;
	}
	if (Target.IsStealthedAndUndetected())
	{
		return false;
	}
	// "if it's garrisoned already, we cannot capture it"
	if (Target.GetContain() && Target.GetTemplate()->Contain.Type == EGenContainType::Garrison && GetRiders(Target).Num() > 0)
	{
		return false;
	}
	return true;
}

// ---- a change of owner --------------------------------------------------------------------------

bool UGenGameLogic::DefectObject(FGenObject& Obj, int32 NewOwner)
{
	// Object::defect
	if (Obj.IsContained() || !Players.IsValidIndex(NewOwner) || Obj.GetOwner() == NewOwner || Obj.IsUnderConstruction() || Obj.IsBeingSold())
	{
		return false;
	}
	const int32 OldOwner = Obj.GetOwner();
	// "Design says": whatever it was making is cancelled and paid back - to the owner it had.
	CancelAllProduction(Obj);
	// "pop it up on the radar, so as to warn those who care" (not when a side that nobody plays is involved)
	if (Players.IsValidIndex(OldOwner) && !Players[OldOwner].bNeutral && !Players[NewOwner].bNeutral && OldOwner == LocalPlayer)
	{
		TryRadarEvent(EGenRadarEvent::Infiltration, Obj.GetPosition());
	}
	ChangeOwner(Obj, NewOwner);
	// "defect any mines that are owned by this structure, right now"
	for (int32 i = bAnyMines ? ObjectOrder.Num() - 1 : -1; i >= 0; --i)
	{
		FGenObject* MineObj = Objects[ObjectOrder[i]].Get();
		if (MineObj->GetProducer() == Obj.GetID() && MineObj->IsKindOf(TEXT("MINE")) && !MineObj->IsDead())
		{
			MineObj->SetOwner(NewOwner);
		}
	}
	// Whoever was sheltering in it is put out, and aircraft parked on it go over too.
	if (Obj.GetContain() && GetRiders(Obj).Num() > 0)
	{
		EvacuateContainer(Obj);
	}
	if (Obj.GetParking())
	{
		// ParkingPlaceBehavior::defectAllParkedUnits
		for (const FGenParkingPlace::FSpace& Space : Obj.GetParking()->GetSpaces())
		{
			FGenObject* Parked = FindObject(Space.Occupant);
			if (Parked && !Parked->IsDead())
			{
				ChangeOwner(*Parked, NewOwner);
			}
		}
	}
	return true;
}

void UGenGameLogic::ChangeOwner(FGenObject& Obj, int32 NewOwner)
{
	const int32 OldOwner = Obj.GetOwner();
	if (OldOwner == NewOwner || !Players.IsValidIndex(NewOwner))
	{
		return;
	}
	// (A strategy center taken over stops working for its old owner and is packed up.)
	if (Obj.GetTemplate()->BattlePlan.bValid)
	{
		Obj.StopBattlePlan();
	}
	RemoveBattlePlanBonuses(Obj, OldOwner);
	Obj.SetOwner(NewOwner);
	AddBattlePlanBonuses(Obj, NewOwner);

	// Object::onCapture: "everybody chills when they are captured"
	Obj.OnPlayerCommand(false);
	Obj.ClearAttack();
	if (Obj.CanMove())
	{
		Obj.CommandStop();
	}
	// ScoreKeeper::addObjectCaptured
	if (!Players[NewOwner].bNeutral && Obj.IsKindOf(TEXT("STRUCTURE")))
	{
		++(Obj.IsKindOf(TEXT("SCORE")) ? Players[NewOwner].Score.FactionBuildingsCaptured : Players[NewOwner].Score.TechBuildingsCaptured);
	}
	// Player::becomingTeamMember: "when we capture a building, we need to see if there's an AutoDepositUpdate
	// hooked to it, if so, award the cash bonus"
	if (!Players[NewOwner].bNeutral)
	{
		Obj.AwardCaptureBonus();
	}
	// TechBuildingBehavior: it shows that somebody owns it.
	if (Obj.GetTemplate()->bTechBuilding)
	{
		Obj.SetModelCondition(CapturedBit(), !Players[NewOwner].bNeutral && !Obj.IsDead());
	}
	// The new owner's upgrades count for it from now on (and an oil refinery's discount changes hands).
	Obj.UpdateUpgradeModules();
	RefreshCostModifiers();
	if (bTrace)
	{
		UE_LOG(LogGenerals, Log, TEXT("  [capture f%u] #%u %s at (%.0f, %.0f) goes from p%d to p%d"), Frame, Obj.GetID(), *Obj.GetTemplate()->Name,
			Obj.GetPosition().X, Obj.GetPosition().Y, OldOwner, NewOwner);
	}
	// "The skirmish ai doesn't know what to do with captured faction buildings except sell them."
	if (Players[NewOwner].bSkirmishAI && Obj.IsKindOf(TEXT("STRUCTURE")) && IsFactionStructure(Obj))
	{
		FGenCommand Sell(EGenCommandType::Sell);
		Sell.Player = NewOwner;
		Sell.Units = { Obj.GetID() };
		QueueSimCommand(MoveTemp(Sell));
	}
}

bool UGenGameLogic::IsFactionStructure(const FGenObject& Obj) const
{
	// Object::isFactionStructure: any of the FS_ kinds.
	for (const FString& Kind : Obj.GetTemplate()->KindOf)
	{
		if (Kind.StartsWith(TEXT("FS_")))
		{
			return true;
		}
	}
	return false;
}

// ---- prices -------------------------------------------------------------------------------------

void UGenGameLogic::RefreshCostModifiers()
{
	// Player::addKindOfProductionCostChange: one entry for each different (kinds, percentage) - two oil
	// refineries are no better than one.
	TArray<int32, TInlineAllocator<16>> Before;
	for (FGenPlayer& P : Players)
	{
		Before.Add(P.CostModifiers.Num());
		P.CostModifiers.Reset();
	}
	ON_SCOPE_EXIT
	{
		for (const FGenPlayer& P : Players)
		{
			if (bTrace && Before.IsValidIndex(P.Index) && Before[P.Index] != P.CostModifiers.Num())
			{
				FString List;
				for (const FGenCostModifier& M : P.CostModifiers)
				{
					List += FString::Printf(TEXT(" %s %+.0f%%"), *FString::Join(M.KindOf, TEXT("+")), M.Percent * 100.f);
				}
				UE_LOG(LogGenerals, Log, TEXT("  [cost f%u] p%d price changes:%s"), Frame, P.Index, List.IsEmpty() ? TEXT(" none") : *List);
			}
		}
	};
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (O->IsDead() || O->IsDestroyed() || !Players.IsValidIndex(O->GetOwner()) || Players[O->GetOwner()].bNeutral)
		{
			continue;
		}
		const TArray<FGenUpgradeModule>& Modules = O->GetTemplate()->UpgradeModules;
		for (int32 i = 0; i < Modules.Num(); ++i)
		{
			if (Modules[i].Effect == EGenUpgradeEffect::CostModifier && O->IsUpgradeModuleDone(i))
			{
				TArray<FGenCostModifier>& List = Players[O->GetOwner()].CostModifiers;
				const bool bKnown = List.ContainsByPredicate([&](const FGenCostModifier& M) { return M.Percent == Modules[i].Percentage && M.KindOf == Modules[i].EffectKindOf; });
				if (!bKnown)
				{
					List.Add({ Modules[i].EffectKindOf, Modules[i].Percentage });
				}
			}
		}
	}
}

int32 UGenGameLogic::CalcCostToBuild(int32 Player, const FGenThingTemplate& Template) const
{
	// ThingTemplate::calcCostToBuild
	float Factor = 1.f;
	if (Players.IsValidIndex(Player))
	{
		for (const FGenCostModifier& M : Players[Player].CostModifiers)
		{
			bool bAll = M.KindOf.Num() > 0;
			for (const FString& Kind : M.KindOf)
			{
				bAll &= Template.KindOf.Contains(Kind);
			}
			if (bAll)
			{
				Factor *= 1.f + M.Percent;
			}
		}
	}
	return int32(Template.BuildCost * Factor);
}

// ---- AutoDepositUpdate --------------------------------------------------------------------------

void FGenObject::UpdateAutoDeposit(uint32 Frame)
{
	const FGenAutoDeposit& D = Template->AutoDeposit;
	if (DepositFrame == 0)
	{
		DepositFrame = Frame + uint32(D.Frames); // (the constructor's)
	}
	if (Frame < DepositFrame)
	{
		return;
	}
	// "we have to set these in update, because during load the team is set, and we don't want to award
	// initial bonus on load": only something that has stood for one period pays a capture bonus.
	bAwardCaptureBonus |= !bDepositInitialized;
	bDepositInitialized = true;
	DepositFrame = Frame + uint32(D.Frames);
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	if (!Players.IsValidIndex(Owner) || Players[Owner].bNeutral || D.Amount <= 0 || bUnderConstruction)
	{
		return;
	}
	int32 Boost = 0;
	for (const TPair<FString, int32>& Up : D.UpgradeBoost)
	{
		if (Logic->HasUpgradeComplete(Owner, Up.Key))
		{
			Boost = Up.Value;
			break;
		}
	}
	const int32 Money = D.Amount + Boost;
	if (D.bActualMoney)
	{
		Logic->AddMoney(Owner, Money);
		Logic->ScoreMoneyEarned(Owner, D.Amount);
	}
	if (Money > 0 && !(bStealthed && !bDetected && Owner != Logic->GetLocalPlayer()))
	{
		FVector3f Pos = Position + FVector3f(0.f, 0.f, 10.f);
		if (IsKindOf(TEXT("STRUCTURE")))
		{
			// (a client-side random in the original; where exactly the number appears is of no consequence)
			Pos.X += FMath::FRandRange(-0.3f, 0.3f) * Template->GeometryMajorRadius;
			Pos.Y += FMath::FRandRange(-0.3f, 0.3f) * Template->GeometryMinorRadius;
		}
		Logic->AddFloatingText(FString::Printf(TEXT("+$%d"), Money), Pos, Players[Owner].Color);
	}
}

void FGenObject::AwardCaptureBonus()
{
	// AutoDepositUpdate::awardInitialCaptureBonus
	const FGenAutoDeposit& D = Template->AutoDeposit;
	if (!D.bValid)
	{
		return;
	}
	DepositFrame = Logic->GetFrame() + uint32(D.Frames);
	if (!bAwardCaptureBonus || D.CaptureBonus <= 0)
	{
		return;
	}
	Logic->AddMoney(Owner, D.CaptureBonus);
	Logic->ScoreMoneyEarned(Owner, D.CaptureBonus);
	Logic->AddFloatingText(FString::Printf(TEXT("+$%d"), D.CaptureBonus), Position + FVector3f(0.f, 0.f, 10.f), Logic->GetPlayers()[Owner].Color);
	bAwardCaptureBonus = false;
}

// ---- OCLUpdate ----------------------------------------------------------------------------------

void FGenObject::UpdateOCL(uint32 Frame)
{
	const FGenOCLUpdate& D = Template->OCLUpdate;
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	auto SetNext = [&]()
	{
		OCLStartFrame = Frame;
		OCLNextFrame = Frame + uint32(Logic->GetRandom().RandRange(D.MinDelay, FMath::Max(D.MinDelay, D.MaxDelay)));
	};
	const bool bPlayable = Players.IsValidIndex(Owner) && !Players[Owner].bNeutral;
	if (D.bFactionTriggered)
	{
		// (A pad nobody holds sends nothing; a new owner waits the whole time for the first delivery.)
		if (bOCLFactionNeutral)
		{
			if (bPlayable)
			{
				OCLOwner = Owner;
				bOCLFactionNeutral = false;
				SetNext();
			}
		}
		else if (!bPlayable)
		{
			bOCLFactionNeutral = true;
		}
		else if (Owner != OCLOwner)
		{
			OCLOwner = Owner;
			SetNext();
		}
		if (bOCLFactionNeutral)
		{
			return;
		}
	}
	if (Frame < OCLNextFrame || bUnderConstruction)
	{
		return;
	}
	if (OCLNextFrame == 0)
	{
		SetNext();
		return;
	}
	SetNext();
	FString OCLName = D.OCL;
	if (D.bFactionTriggered)
	{
		OCLName.Reset();
		const FGenPlayerTemplate* Side = bPlayable ? Players[Owner].Template : nullptr;
		for (const TPair<FString, FString>& F : D.FactionOCL)
		{
			if (Side && F.Key.Equals(Side->Side, ESearchCase::IgnoreCase))
			{
				OCLName = F.Value;
				break;
			}
		}
	}
	if (OCLName.IsEmpty())
	{
		return;
	}
	const FVector3f CreatePos = D.bCreateAtEdge ? Logic->FindEdgePoint(Position, false) : Position;
	Logic->RunPowerOCL(OCLName, *this, CreatePos, Position);
	if (Logic->IsTracing())
	{
		UE_LOG(LogGenerals, Log, TEXT("  [ocl f%u] #%u p%d %s sends for %s (next in %d frames)"), Frame, ID, Owner, *Template->Name, *OCLName, int32(OCLNextFrame - Frame));
	}
}

// ---- SpecialAbilityUpdate -----------------------------------------------------------------------

namespace
{
	EGenAbilityKind AbilityKindOf(const FGenAbilityData& D)
	{
		const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(D.Power);
		return Power ? Power->AbilityKind : EGenAbilityKind::Other;
	}

	bool IsCaptureKind(EGenAbilityKind Kind)
	{
		return Kind == EGenAbilityKind::CaptureBuilding || Kind == EGenAbilityKind::LotusCapture;
	}

	/** endPreparation: the kinds whose special object (a beam) is only there while they prepare. */
	bool BeamEndsWithPreparation(EGenAbilityKind Kind)
	{
		switch (Kind)
		{
		case EGenAbilityKind::LaserGuidedMissiles:
		case EGenAbilityKind::HackerDisableBuilding:
		case EGenAbilityKind::LotusVehicleHack:
		case EGenAbilityKind::LotusCapture:
		case EGenAbilityKind::LotusCashHack:
		case EGenAbilityKind::CaptureBuilding:
			return true;
		default:
			return false; // "No, don't delete placed charges."
		}
	}
}

int32 FGenObject::FindAbility(const FString& Power) const
{
	return Template->Abilities.IndexOfByPredicate([&Power](const FGenAbilityData& A) { return A.Power.Equals(Power, ESearchCase::IgnoreCase); });
}

bool FGenObject::StartAbility(int32 InAbility, FGenObjectID Target, const FVector3f* TargetPos)
{
	// SpecialAbilityUpdate::initiateIntentToDoSpecialPower
	if (!Template->Abilities.IsValidIndex(InAbility) || !CanMove() || bDead)
	{
		return false;
	}
	if (AbilityIndex != INDEX_NONE)
	{
		ExitAbility(false); // "Prevent other mutually exclusive specials from running"
	}
	const FGenAbilityData& D = Template->Abilities[InAbility];
	AbilityIndex = InAbility;
	AbilityTarget = Target;
	bAbilityHasPos = Target == GenInvalidObjectID && TargetPos != nullptr;
	AbilityTargetPos = bAbilityHasPos ? *TargetPos : FVector3f::ZeroVector;
	AbilityPrepFrames = 0;
	AbilityAnimFrames = 0;
	AbilityPacking = EAbilityPacking::Packed;
	bAbilityFacingStarted = false;
	bAbilityFacingDone = false;
	bAbilityInRange = false;
	bAbilityNoTarget = Target == GenInvalidObjectID && !bAbilityHasPos;
	bAbilityAttack = false;
	CaptureFlashPhase = 0.f;
	NextAbilityPathFrame = 0;
	SetModelCondition(UnpackingBit(), false);
	SetModelCondition(PackingBit(), false);
	SetModelCondition(FiringABit(), false);
	SetModelCondition(RaisingFlagBit(), false);
	// "Only unpack if we need to"
	if (D.UnpackFrames == 0 || (bAbilityNoTarget && D.bSkipPackingWithNoTarget))
	{
		AbilityPacking = EAbilityPacking::Unpacked;
	}
	// (Clear any old AI before starting this special ability.)
	ClearAttack();
	if (bMoving)
	{
		CommandStop();
	}
	return true;
}

void FGenObject::EndAbilityPreparation()
{
	// SpecialAbilityUpdate::endPreparation
	bUsingAbility = false;
	if (bAbilityLoopSound)
	{
		bAbilityLoopSound = false;
		Logic->SetAbilityLoopSound(*this, FString()); // "TheAudio->removeAudioEvent( m_prepSoundLoop.getPlayingHandle() )"
	}
	if (AbilityIndex == INDEX_NONE)
	{
		return;
	}
	if (BeamEndsWithPreparation(AbilityKindOf(Template->Abilities[AbilityIndex])))
	{
		KillAbilityObjects(AbilityIndex);
		if (bAbilityAttack)
		{
			// "The instant we remove the laser, reset the weaponset!"
			bAbilityAttack = false;
			ClearAttack();
		}
	}
}

void FGenObject::ExitAbility(bool bCleanup)
{
	// SpecialAbilityUpdate::onExit
	if (AbilityIndex == INDEX_NONE)
	{
		return;
	}
	const int32 Index = AbilityIndex;
	const FGenAbilityData& D = Template->Abilities[Index];
	SetModelCondition(UnpackingBit(), false);
	SetModelCondition(PackingBit(), false);
	SetModelCondition(FiringABit(), false);
	SetModelCondition(RaisingFlagBit(), false);
	EndAbilityPreparation();
	if (!D.bSpecialObjectsPersistent || (bCleanup && !D.bSpecialObjectsPersistWhenOwnerDies))
	{
		// "Delete special objects that aren't considered persistent whenever we ... leave the special ability update."
		KillAbilityObjects(Index);
	}
	AbilityBeam = nullptr;
	bAbilityAttack = false;
	AbilityIndex = INDEX_NONE;
	AbilityPacking = EAbilityPacking::None;
	bAbilityInRange = false;
	AbilityPrepFrames = 0;
	AbilityAnimFrames = 0;
}

void FGenObject::FinishAbility()
{
	// SpecialAbilityUpdate::finishAbility
	if (AbilityIndex == INDEX_NONE)
	{
		return;
	}
	const FGenAbilityData& D = Template->Abilities[AbilityIndex];
	// "The flee range code is used only if the unit has completed a special ability against a target"
	// (Colonel Burton plants his charge and runs; told to set them off, he stays where he is).
	const bool bFlee = D.FleeRange > 0.f && (bAbilityHasPos || AbilityTarget != GenInvalidObjectID);
	FVector3f FleePos = Position;
	if (bFlee)
	{
		const FVector2f Dir = GetDirection2D() * D.FleeRange;
		const float Sign = (D.bFlipAfterUnpacking || D.bFlipAfterPacking) ? 1.f : -1.f;
		FleePos.X += Sign * Dir.X;
		FleePos.Y += Sign * Dir.Y;
		// "Now check for mines. Normally we are fleeing from a bomb we just planted. It is not good to run
		// back towards the previous mine we just planted about the time it explodes."
		const FGenObject* Mine = nullptr;
		float BestDistSq = D.FleeRange * D.FleeRange;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (!O || O->IsDead() || O->IsDestroyed() || O->GetOwner() != Owner || !O->IsKindOf(TEXT("MINE")))
			{
				continue;
			}
			const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(FleePos.X, FleePos.Y));
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Mine = O;
			}
		}
		if (Mine)
		{
			const FVector2f Away = (FVector2f(FleePos.X, FleePos.Y) - FVector2f(Mine->GetPosition().X, Mine->GetPosition().Y)).GetSafeNormal() * D.FleeRange;
			FleePos.X = Mine->GetPosition().X + Away.X;
			FleePos.Y = Mine->GetPosition().Y + Away.Y;
		}
	}
	if (bMoving)
	{
		CommandStop();
	}
	ExitAbility(false);
	if (bFlee)
	{
		StartPath(FleePos, false);
	}
}

bool FGenObject::StartAbilityPreparation(const FGenAbilityData& D, EGenAbilityKind Kind, FGenObject* Target, uint32 Frame)
{
	// SpecialAbilityUpdate::startPreparation
	const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(D.Power);
	const int32 Module = Logic->FindPowerModule(*this, D.Power);
	AbilityPrepFrames = D.PreparationFrames;
	switch (Kind)
	{
	case EGenAbilityKind::LaserGuidedMissiles:
		if (Target)
		{
			CreateAbilityObject(AbilityIndex); // the laser, from the launcher to the vehicle
		}
		break;
	case EGenAbilityKind::CaptureBuilding:
		if (!Target || Target->GetOwner() == Owner)
		{
			return false; // "it's been captured by a colleague! we should stop."
		}
		if (Target->CheckAndDetonateBoobyTrap(this) && (Target->IsDead() || bDead))
		{
			return false; // "Whoops, it was mined."
		}
		SetModelCondition(UnpackingBit(), false);
		SetModelCondition(RaisingFlagBit(), true);
		// "Warn the victim so he might have a chance to react!"
		Logic->EvaEvent(TEXT("BuildingBeingStolen"), Target->GetOwner());
		if (Target->GetOwner() == Logic->GetLocalPlayer())
		{
			Logic->TryRadarEvent(EGenRadarEvent::Infiltration, Target->GetPosition());
		}
		break;
	case EGenAbilityKind::HackerDisableBuilding:
	case EGenAbilityKind::LotusCapture:
	case EGenAbilityKind::LotusVehicleHack:
	case EGenAbilityKind::LotusCashHack:
		if (Target)
		{
			if (Target->GetOwner() == Owner || Logic->GetRelationship(Owner, Target->GetOwner()) == EGenRelationship::Allies)
			{
				return false;
			}
			CreateAbilityObject(AbilityIndex); // the stream of numbers
			if (AbilityBeam)
			{
				// "For the hacker this sets up the looping typing animation."
				SetModelCondition(UnpackingBit(), false);
				SetModelCondition(FiringABit(), true);
			}
			if (Kind == EGenAbilityKind::LotusCapture)
			{
				Logic->EvaEvent(TEXT("BuildingBeingStolen"), Target->GetOwner());
			}
			if (Target->GetOwner() == Logic->GetLocalPlayer())
			{
				Logic->TryRadarEvent(EGenRadarEvent::Infiltration, Target->GetPosition());
			}
		}
		break;
	default:
		break;
	}
	// markSpecialPowerTriggered: the power starts recharging now.
	if (PowerReady.IsValidIndex(Module) && Power)
	{
		PowerReady[Module] = Frame + uint32(Power->ReloadTime);
	}
	if (bMoving)
	{
		CommandStop();
	}
	bUsingAbility = true;
	if (!D.PrepSoundLoop.IsEmpty())
	{
		Logic->SetAbilityLoopSound(*this, D.PrepSoundLoop);
		bAbilityLoopSound = true;
	}
	if (Logic->IsTracing())
	{
		UE_LOG(LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s begins %s on #%u (%d frames)"), Frame, ID, Owner, *Template->Name, *D.Power, AbilityTarget, AbilityPrepFrames);
	}
	return true;
}

bool FGenObject::ContinueAbilityPreparation(const FGenAbilityData& D, EGenAbilityKind Kind, FGenObject* Target, uint32 Frame)
{
	// SpecialAbilityUpdate::continuePreparation
	if (D.AbortRange < 1.e6f)
	{
		// "Check if we are within the abort distance, otherwise abort!"
		const float Gap = Target ? BoundingGap(*this, *Target)
			: bAbilityHasPos ? FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(AbilityTargetPos.X, AbilityTargetPos.Y)) - Template->GetBoundingCircleRadius() : 0.f;
		if (Gap > D.AbortRange)
		{
			return false;
		}
	}
	switch (Kind)
	{
	case EGenAbilityKind::LaserGuidedMissiles:
	case EGenAbilityKind::LotusVehicleHack:
		// (the beam follows its target by itself)
		return Target && Logic->GetRelationship(Owner, Target->GetOwner()) != EGenRelationship::Allies && Target->GetOwner() != Owner;

	case EGenAbilityKind::CaptureBuilding:
	case EGenAbilityKind::LotusCapture:
	{
		if (!Target || Target->GetOwner() == Owner || Logic->GetRelationship(Owner, Target->GetOwner()) == EGenRelationship::Allies)
		{
			return false; // "It's been captured by a colleague, so cancel!"
		}
		if (D.bDoCaptureFX)
		{
			// The building flashes in the capturer's colour, quicker as the moment comes nearer.
			const bool bLastPhase = (int32(CaptureFlashPhase) & 1) != 0;
			CaptureFlashPhase += (1.f - float(AbilityPrepFrames) / float(FMath::Max(1, D.PreparationFrames))) / 3.f;
			if (bLastPhase && (int32(CaptureFlashPhase) & 1) == 0)
			{
				Target->FlashAs(Logic->GetPlayers()[Owner].Color);
				Logic->PlaySoundAt(FGenAudioData::Get().GetMisc(TEXT("DefectorTimerTickSound")), *Target);
			}
		}
		if (Kind == EGenAbilityKind::CaptureBuilding)
		{
			// "these keep resetting the recharge timer while in use ... only for infantry capture, not black
			// lotus which resets in triggerAbilityEffect()"
			const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(D.Power);
			const int32 Module = Logic->FindPowerModule(*this, D.Power);
			if (PowerReady.IsValidIndex(Module) && Power)
			{
				PowerReady[Module] = Frame + uint32(Power->ReloadTime);
			}
		}
		return true;
	}
	default:
		return true;
	}
}

void FGenObject::UpdateAbility(uint32 Frame)
{
	if (AbilityIndex == INDEX_NONE)
	{
		return;
	}
	const FGenAbilityData& D = Template->Abilities[AbilityIndex];
	const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(D.Power);
	const int32 Module = Logic->FindPowerModule(*this, D.Power);
	const EGenAbilityKind Kind = Power ? Power->AbilityKind : EGenAbilityKind::Other;

	// "This check will see if there has been any commands issued by either the player or script. When told
	// to do something else, we need to immediately cleanup our special ability." (Orders from the player
	// end it through OnPlayerCommand; these are the ones scripts give. The shooting a laser lock brings
	// with it is the ability's own.)
	const bool bOwnAttack = bAbilityAttack && TargetID == AbilityTarget;
	if (AIMode != EGenAIMode::None || (bExplicitAttack && !bOwnAttack) || EnterTarget != GenInvalidObjectID || Module == INDEX_NONE || IsContained())
	{
		ExitAbility(false);
		return;
	}

	// handlePackingProcessing: the time it takes to unpack or to pack.
	if (AbilityAnimFrames > 0)
	{
		if (--AbilityAnimFrames > 0)
		{
			// "This is new... the ability to disable stealth before triggering"
			if (D.bLoseStealthOnTrigger && AbilityAnimFrames < D.PreTriggerUnstealthFrames)
			{
				MarkAsDetected(0);
			}
			return;
		}
		SetModelCondition(UnpackingBit(), false);
		SetModelCondition(PackingBit(), false);
		if (AbilityPacking == EAbilityPacking::Unpacking)
		{
			if (D.bFlipAfterUnpacking)
			{
				SetOrientation(Orientation + PI);
			}
			AbilityPacking = EAbilityPacking::Unpacked;
		}
		else if (AbilityPacking == EAbilityPacking::Packing)
		{
			if (D.bFlipAfterPacking)
			{
				SetOrientation(Orientation + PI);
			}
			AbilityPacking = EAbilityPacking::Packed;
			FinishAbility();
			return;
		}
	}

	FGenObject* Target = AbilityTarget != GenInvalidObjectID ? Logic->FindObject(AbilityTarget) : nullptr;
	auto NeedToUnpack = [&]() { return AbilityPacking == EAbilityPacking::Packed && !(D.bSkipPackingWithNoTarget && bAbilityNoTarget) && D.UnpackFrames > 0; };
	auto NeedToPack = [&]() { return AbilityPacking == EAbilityPacking::Unpacked && !(D.bSkipPackingWithNoTarget && bAbilityNoTarget) && D.PackFrames > 0; };

	// "A dead target will end our special (if we are using a target)"; so does a colleague getting there
	// first, and a target that hides.
	bool bAbort = false;
	if (AbilityTarget != GenInvalidObjectID)
	{
		if (!Target || Target->IsDead() || Target->IsDestroyed())
		{
			bAbort = true;
		}
		else
		{
			const bool bTargetHidden = Target->IsStealthedAndUndetected();
			const bool bPreparing = AbilityPrepFrames > 0;
			switch (Kind)
			{
			case EGenAbilityKind::CaptureBuilding:
			case EGenAbilityKind::LotusCapture:
			case EGenAbilityKind::HackerDisableBuilding:
				bAbort = Target->GetOwner() == Owner || (bTargetHidden && bPreparing);
				break;
			case EGenAbilityKind::LotusCashHack:
			case EGenAbilityKind::BoobyTrap:
				bAbort = bTargetHidden && bPreparing;
				break;
			case EGenAbilityKind::RemoteCharges:
			case EGenAbilityKind::TimedCharges:
				bAbort = !NeedToUnpack() && bTargetHidden && bPreparing;
				break;
			case EGenAbilityKind::LaserGuidedMissiles:
				bAbort = Target->IsKindOf(TEXT("STRUCTURE")) || bTargetHidden;
				break;
			case EGenAbilityKind::LotusVehicleHack:
				bAbort = bTargetHidden; // "where'd my target go? 'Twas here just a second ago."
				break;
			default:
				break;
			}
		}
	}
	if (bAbort)
	{
		if (bMoving)
		{
			CommandStop();
		}
		ExitAbility(false);
		return;
	}

	auto StartPacking = [&](bool bSuccess)
	{
		AbilityPacking = EAbilityPacking::Packing;
		AbilityAnimFrames = FMath::Max(1, FMath::RoundToInt32(float(D.PackFrames) * Logic->GetRandom().FRandRange(1.f - D.PackUnpackVariation, 1.f + D.PackUnpackVariation)));
		SetModelCondition(UnpackingBit(), false);
		SetModelCondition(RaisingFlagBit(), false);
		SetModelCondition(FiringABit(), false);
		SetModelCondition(PackingBit(), true);
		Logic->PlaySoundAt(D.PackSound, *this);
		if (bSuccess)
		{
			switch (Kind)
			{
			case EGenAbilityKind::LotusCapture: Logic->PlayObjectSound(*this, { TEXT("VoiceCaptureBuildingComplete"), TEXT("VoiceTaskComplete") }); break;
			case EGenAbilityKind::LotusVehicleHack: Logic->PlayObjectSound(*this, { TEXT("VoiceDisableVehicleComplete"), TEXT("VoiceTaskComplete") }); break;
			case EGenAbilityKind::LotusCashHack: Logic->PlayObjectSound(*this, { TEXT("VoiceStealCashComplete"), TEXT("VoiceTaskComplete") }); break;
			default: Logic->PlayObjectSound(*this, { TEXT("VoiceTaskComplete") }); break;
			}
		}
	};
	auto AfterTrigger = [&](bool bSuccess)
	{
		EndAbilityPreparation();
		if (NeedToPack())
		{
			StartPacking(bSuccess);
		}
		else
		{
			FinishAbility();
		}
	};
	const bool bPersistent = D.PersistentPrepFrames > 0;

	if (AbilityPrepFrames > 0)
	{
		// Preparing: the flag goes up, the laptop works, the laser paints.
		// ("if power requires recharging, lets freeze prep countdown until power is ready")
		const bool bPowerReady = !(bPersistent && D.bPersistenceRequiresRecharge) || !PowerReady.IsValidIndex(Module) || Frame > PowerReady[Module];
		if (bPowerReady)
		{
			--AbilityPrepFrames;
		}
		if (AbilityPrepFrames == 0)
		{
			const int32 Index = AbilityIndex;
			TriggerAbility(D, Kind, Target);
			if (AbilityIndex != Index || bDead)
			{
				return; // (a trap went off under us)
			}
			if (bPersistent)
			{
				// "A persistent special will continually trigger it's effect every so often and never end."
				AbilityPrepFrames = D.PersistentPrepFrames;
				if (D.bPersistenceRequiresRecharge && PowerReady.IsValidIndex(Module) && Power)
				{
					PowerReady[Module] = Frame + uint32(Power->ReloadTime);
				}
			}
			else
			{
				AfterTrigger(true);
			}
			return;
		}
		if (!ContinueAbilityPreparation(D, Kind, Target, Frame))
		{
			AbilityPrepFrames = 0;
			AfterTrigger(false);
		}
		return;
	}

	// Within reach of the target?
	bool bInRange = bAbilityInRange;
	if (!bInRange)
	{
		// "Quickly convert very short range approachs to "contact" class": StartAbilityRange, less a quarter
		// of a cell (FROM_BOUNDINGSPHERE_2D).
		const float Reach = FMath::Max(0.f, D.StartAbilityRange - GenLogic::PathfindCellSize * 0.25f);
		if (Target)
		{
			const float Gap = BoundingGap(*this, *Target);
			bInRange = Gap <= D.StartAbilityRange && Gap <= Reach;
			// (Contact with something that keeps us at arm's length - a vehicle, a building's corner: having
			// walked up as near as the ground allows is as good as touching it.)
			if (!bInRange && D.StartAbilityRange < 10.f && !bMoving && NextAbilityPathFrame != 0 && Gap <= Reach + 15.f)
			{
				bInRange = true;
			}
		}
		else if (bAbilityHasPos)
		{
			bInRange = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(AbilityTargetPos.X, AbilityTargetPos.Y)) - Template->GetBoundingCircleRadius() <= D.StartAbilityRange;
		}
		else
		{
			bInRange = AbilityTarget == GenInvalidObjectID; // "No position, so this step is useless"
		}
	}
	if (bInRange)
	{
		bAbilityInRange = true;
		const FVector3f FacePos = Target ? Target->GetPosition() : AbilityTargetPos;
		if (D.bNeedToFaceTarget && !bAbilityFacingDone && (Target || bAbilityHasPos))
		{
			// startFacing / isFacing: stop, and turn to the target.
			if (!bAbilityFacingStarted)
			{
				bAbilityFacingStarted = true;
				if (bMoving)
				{
					CommandStop();
				}
				Velocity = FVector3f::ZeroVector;
			}
			const float DesiredAngle = FMath::Atan2(FacePos.Y - Position.Y, FacePos.X - Position.X);
			const float Rel = GenLogic::AngleDiff(DesiredAngle, Orientation);
			const float MaxTurn = Locomotor ? FMath::Max(Locomotor->TurnRate, 0.02f) : 0.2f;
			SetOrientation(Orientation + FMath::Clamp(Rel, -MaxTurn, MaxTurn));
			if (FMath::Abs(Rel) > MaxTurn)
			{
				return;
			}
			bAbilityFacingDone = true;
		}
		if (NeedToUnpack())
		{
			// startUnpacking
			AbilityPacking = EAbilityPacking::Unpacking;
			AbilityAnimFrames = FMath::Max(1, FMath::RoundToInt32(float(D.UnpackFrames) * Logic->GetRandom().FRandRange(1.f - D.PackUnpackVariation, 1.f + D.PackUnpackVariation)));
			SetModelCondition(PackingBit(), false);
			SetModelCondition(UnpackingBit(), true);
			Logic->PlaySoundAt(D.UnpackSound, *this);
			if (bMoving)
			{
				CommandStop();
			}
			return;
		}
		if (AbilityPacking == EAbilityPacking::Unpacked)
		{
			if (!StartAbilityPreparation(D, Kind, Target, Frame))
			{
				if (AbilityIndex != INDEX_NONE && !bDead)
				{
					AbilityPrepFrames = 0;
					AfterTrigger(false);
				}
				return;
			}
			if (AbilityPrepFrames == 0)
			{
				// "TRIGGER (skipping preparation)"
				const int32 Index = AbilityIndex;
				TriggerAbility(D, Kind, Target);
				if (AbilityIndex != Index || bDead)
				{
					return;
				}
				if (bPersistent && D.bPersistenceRequiresRecharge)
				{
					AbilityPrepFrames = D.PersistentPrepFrames;
					if (PowerReady.IsValidIndex(Module) && Power)
					{
						PowerReady[Module] = Frame + uint32(Power->ReloadTime);
					}
					return;
				}
				AfterTrigger(true);
			}
		}
		return;
	}

	// approachTarget: "aiMoveToObject" - walk up to it.
	if (!bMoving && Frame >= NextAbilityPathFrame)
	{
		NextAbilityPathFrame = Frame + uint32(GenLogic::GetLogicFPS());
		const FVector3f Dest = Target ? Target->GetPosition() : AbilityTargetPos;
		if (!StartPath(Dest, false))
		{
			FinishAbility();
		}
	}
}

void FGenObject::TriggerAbility(const FGenAbilityData& D, EGenAbilityKind Kind, FGenObject* Target)
{
	// SpecialAbilityUpdate::triggerAbilityEffect
	const uint32 Frame = Logic->GetFrame();
	const int32 Index = AbilityIndex;
	if (D.AwardXP > 0)
	{
		AddExperience(D.AwardXP);
	}
	const int32 Skill = D.SkillPoints != -1 ? D.SkillPoints : D.AwardXP;
	if (Skill > 0)
	{
		Logic->AddSkillPoints(Owner, Skill);
	}
	Logic->PlaySoundAt(D.TriggerSound, *this);
	bool bOkToLoseStealth = true;

	auto PlantCharge = [&]()
	{
		FGenObject* Charge = CreateAbilityObject(Index);
		if (!Charge)
		{
			return;
		}
		if (!Charge->GetTemplate()->StickyBomb.bValid)
		{
			KillAbilityObjects(Index); // (it "requires a StickyBombUpdate module")
			return;
		}
		// "Setting the producer ID allows the sticky bomb update module to initialize and setup timers"
		Charge->InitStickyBomb(Target, this);
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s puts #%u %s on #%u %s (%d of %d out)"), Frame, ID, Owner, *Template->Name,
			Charge->GetID(), *Charge->GetTemplate()->Name, Target->GetID(), *Target->GetTemplate()->Name, GetAbilityObjectCount(Index), D.MaxSpecialObjects);
	};

	switch (Kind)
	{
	case EGenAbilityKind::Disguise:
		if (Target)
		{
			DisguiseAs(Target); // SpecialAbilityUpdate::triggerAbilityEffect, SPECIAL_DISGUISE_AS_VEHICLE
		}
		break;

	case EGenAbilityKind::HelixNapalmBomb:
		// "Couldn't be simpler... the special object is the bomb": it leaves the helicopter at its speed, falls,
		// and goes off on the ground (HeightDieUpdate, FireWeaponWhenDeadBehavior).
		if (FGenObject* Bomb = CreateAbilityObject(Index))
		{
			Bomb->Velocity = Velocity;
			Bomb->bFalling = true;
			Bomb->bFreePhysics = true;
			Bomb->Goal = AbilityTargetPos;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s drops #%u %s at (%.0f, %.0f) alt %.0f"), Frame, ID, Owner, *Template->Name,
				Bomb->GetID(), *Bomb->GetTemplate()->Name, Position.X, Position.Y, Position.Z - Logic->GetGroundHeight(Position.X, Position.Y));
		}
		break;

	case EGenAbilityKind::LaserGuidedMissiles:
		if (Target && Weapons[1].Template)
		{
			// "lock it just till the weapon is empty or the attack is done"
			CommandAttack(Target->GetID());
			if (TargetID == Target->GetID())
			{
				WeaponLockSlot = 1;
				bAbilityAttack = true;
			}
		}
		break;

	case EGenAbilityKind::TankHunterTNT:
	case EGenAbilityKind::TimedCharges:
	case EGenAbilityKind::BoobyTrap:
		if (!Target)
		{
			return;
		}
		if (Target->CheckAndDetonateBoobyTrap(this) && (Target->IsDead() || bDead))
		{
			return; // "Whoops, it was mined. Cancel if it or us is now dead."
		}
		if (Kind == EGenAbilityKind::BoobyTrap && Target->IsBoobyTrapped())
		{
			return; // "we can't double booby trap something"
		}
		PlantCharge();
		break;

	case EGenAbilityKind::HackerDisableBuilding:
	case EGenAbilityKind::LotusVehicleHack:
	{
		if (!Target || Target->GetOwner() == Owner || Logic->GetRelationship(Owner, Target->GetOwner()) == EGenRelationship::Allies)
		{
			return;
		}
		// "Disable the target for a specified amount of time."
		const bool bWasHacked = Target->IsHacked();
		Target->HackUntil(Frame + uint32(D.EffectDuration));
		UE_CLOG(Logic->IsTracing() && !bWasHacked, LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s has #%u %s hacked (until f%u at least)"), Frame, ID, Owner,
			*Template->Name, Target->GetID(), *Target->GetTemplate()->Name, Frame + uint32(D.EffectDuration));
		int32 Interleave = 1;
		const FGenThingTemplate* TT = Target->GetTemplate();
		// (GeometryInfo::getFootprintArea)
		const float Footprint = TT->IsBox() ? 4.f * TT->GeometryMajorRadius * TT->GeometryMinorRadius : PI * TT->GeometryMajorRadius * TT->GeometryMajorRadius;
		if (Target->IsKindOf(TEXT("STRUCTURE")) && Footprint < 300.f)
		{
			bDisableFXToggle = !bDisableFXToggle; // "toggle if small building"
			Interleave = 2;
		}
		if (bDisableFXToggle && !D.DisableFXParticleSystem.IsEmpty())
		{
			// (a client-side random spot inside the footprint in the original)
			const FVector3f Offset(FMath::FRandRange(-0.7f, 0.7f) * TT->GeometryMajorRadius,
				FMath::FRandRange(-0.7f, 0.7f) * (TT->GeometryMinorRadius > 0.f ? TT->GeometryMinorRadius : TT->GeometryMajorRadius), 0.f);
			Logic->AttachParticleSystemFor(D.DisableFXParticleSystem, Target->GetID(), Offset, uint32(D.EffectDuration * Interleave));
		}
		break;
	}

	case EGenAbilityKind::CaptureBuilding:
	case EGenAbilityKind::LotusCapture:
	{
		if (!Target || Target->IsDead())
		{
			return;
		}
		if (Target->CheckAndDetonateBoobyTrap(this) && (Target->IsDead() || bDead))
		{
			return;
		}
		if (Target->GetOwner() == Owner)
		{
			return; // "it's been captured by a colleague! we should stop."
		}
		// "Just in case we are capturing a building which is already garrisoned by other": they are put out,
		// and that is all - "we do not want to set a neutral building to our team if we are not in it".
		if (Target->GetContain() && Target->GetTemplate()->Contain.Type == EGenContainType::Garrison)
		{
			Logic->EvacuateContainer(*Target);
			break;
		}
		Logic->EvaEvent(TEXT("BuildingStolen"), Target->GetOwner());
		if (Logic->IsTracing())
		{
			UE_LOG(LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s captures #%u %s"), Frame, ID, Owner, *Template->Name, Target->GetID(), *Target->GetTemplate()->Name);
		}
		Logic->DefectObject(*Target, Owner);
		if (Kind == EGenAbilityKind::LotusCapture)
		{
			// "only for black lotus, not infantry capture which resets in contunueprep()"
			const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(D.Power);
			const int32 Module = Logic->FindPowerModule(*this, D.Power);
			if (PowerReady.IsValidIndex(Module) && Power)
			{
				PowerReady[Module] = Frame + uint32(Power->ReloadTime);
			}
		}
		break;
	}

	case EGenAbilityKind::LotusCashHack:
	{
		if (!Target)
		{
			return;
		}
		// "Steal a thousand cash from the other team!" ("otherwise, take the remainder")
		const TArray<FGenPlayer>& Players = Logic->GetPlayers();
		const int32 Cash = Players.IsValidIndex(Target->GetOwner()) ? FMath::Min(1000, Players[Target->GetOwner()].Money) : 0;
		if (Cash > 0)
		{
			Logic->AddMoney(Target->GetOwner(), -Cash);
			Logic->AddMoney(Owner, Cash);
			Logic->ScoreMoneyEarned(Owner, Cash);
			Logic->EvaEvent(TEXT("CashStolen"), Target->GetOwner());
			Logic->AddFloatingText(FString::Printf(TEXT("+$%d"), Cash), Position + FVector3f(0.f, 0.f, 20.f), FLinearColor(0.f, 1.f, 0.f));
			Logic->AddFloatingText(FString::Printf(TEXT("-$%d"), Cash), Target->GetPosition() + FVector3f(0.f, 0.f, 30.f), FLinearColor(1.f, 0.f, 0.f));
		}
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s steals $%d from p%d at #%u %s"), Frame, ID, Owner, *Template->Name, Cash,
			Target->GetOwner(), Target->GetID(), *Target->GetTemplate()->Name);
		break;
	}

	case EGenAbilityKind::RemoteCharges:
		if (Target && Target->CheckAndDetonateBoobyTrap(this) && (Target->IsDead() || bDead))
		{
			return;
		}
		if (bAbilityNoTarget)
		{
			// "If there is no target object nor position, then we are detonating the existing charges."
			if (AbilityObjects.IsValidIndex(Index))
			{
				const TArray<FGenObjectID> Charges = AbilityObjects[Index];
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s sets off %d charges"), Frame, ID, Owner, *Template->Name, Charges.Num());
				for (FGenObjectID Id : Charges)
				{
					if (FGenObject* Charge = Logic->FindObject(Id))
					{
						Charge->DetonateStickyBomb(); // "Blow it up!!!"
						bOkToLoseStealth = false;
					}
				}
			}
		}
		else if (Target)
		{
			PlantCharge();
		}
		break;

	default:
		break;
	}

	if (D.bLoseStealthOnTrigger && bOkToLoseStealth)
	{
		MarkAsDetected(0);
	}
}

// ---- CommandButtonHuntUpdate --------------------------------------------------------------------

void FGenObject::SetHuntButton(const FString& ButtonName)
{
	// CommandButtonHuntUpdate::setCommandButton: only a button of the unit's own command set counts.
	HuntButton.Reset();
	const FGenCommandSet* Set = FGenCommandData::Get().FindSet(GetCommandSet());
	if (!Template->bButtonHunt || !Set || bDead)
	{
		return;
	}
	for (const FString& Slot : Set->Slots)
	{
		if (Slot.Equals(ButtonName, ESearchCase::IgnoreCase))
		{
			HuntButton = ButtonName;
			break;
		}
	}
	if (HuntButton.IsEmpty())
	{
		return;
	}
	// "Stop whatever we're doing."
	AIMode = EGenAIMode::None;
	ClearAttack();
	if (bMoving)
	{
		CommandStop();
	}
	NextHuntScanFrame = 0;
}

void FGenObject::UpdateButtonHunt(uint32 Frame)
{
	if (HuntButton.IsEmpty() || Frame < NextHuntScanFrame)
	{
		return;
	}
	// "If a script or the player ... we quit hunting." (The shooting a laser lock brings with it is our own.)
	if (AIMode != EGenAIMode::None || (bExplicitAttack && !bAbilityAttack))
	{
		HuntButton.Reset();
		return;
	}
	NextHuntScanFrame = Frame + uint32(Template->HuntScanFrames);
	const FGenCommandButton* Button = FGenCommandData::Get().FindButton(HuntButton);
	const FGenSpecialPowerTemplate* Power = Button ? FGenUpgradeData::Get().FindPower(Button->SpecialPower) : nullptr;
	const int32 Module = Power ? Logic->FindPowerModule(*this, Power->Name) : INDEX_NONE;
	if (!Button || Button->Command != EGenCommand::SpecialPower || !Power || !Power->bPorted || Module == INDEX_NONE
		|| Template->PowerModules[Module].Type != EGenPowerModule::SpecialAbility || FindAbility(Power->Name) == INDEX_NONE)
	{
		// (Hunting with a weapon or by getting into things isn't ported: such a team simply hunts.)
		HuntButton.Reset();
		if (CanMove() && HasWeapons())
		{
			CommandHunt();
		}
		return;
	}
	if (bMoving || AbilityIndex != INDEX_NONE)
	{
		return; // busy with the last one
	}
	// scanClosestTarget: the best thing in range the ability can be used on - by the team's attack
	// priorities, less a point for every so much distance. (Priority sets aren't ported; the one the
	// scripts give capture teams is "tech buildings 5000", everything else the default 1 - so: the
	// nearest tech building, and failing that the nearest thing of any kind.)
	if (!Logic->IsPowerReady(*this, Module))
	{
		return;
	}
	const EGenAbilityKind Kind = Power->AbilityKind;
	const bool bFlagCapture = Kind == EGenAbilityKind::CaptureBuilding;
	const FGenObject* Best = nullptr;
	float BestScore = 0.f;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || O->IsDead() || O->IsDestroyed() || O->IsStealthedAndUndetected())
		{
			continue;
		}
		// "only consider enemies"; with the flag "It's ok (in fact necessary for oil derricks) to capture
		// special buildings", but not our own or an ally's.
		const EGenRelationship R = Logic->GetRelationship(Owner, O->GetOwner());
		if (bFlagCapture ? (O->GetOwner() == Owner || R == EGenRelationship::Allies) : (R != EGenRelationship::Enemies))
		{
			continue;
		}
		if (Kind == EGenAbilityKind::LotusVehicleHack && O->IsDisabled())
		{
			continue; // "The hack disables the vehicle, so we don't want to do it again."
		}
		if (!Logic->CanUseAbilityOn(*this, Module, O, false))
		{
			continue;
		}
		const float Dist = BoundingGap(*this, *O);
		if (Dist > Template->HuntScanRange)
		{
			continue;
		}
		const float Priority = (IsCaptureKind(Kind) && O->IsKindOf(TEXT("TECH_BUILDING"))) ? 5000.f : 1.f;
		const float Score = FMath::Max(1.f, Priority - Dist / 100.f) + 1.f / (1.f + Dist); // (the nearer of equals)
		if (Score > BestScore)
		{
			BestScore = Score;
			Best = O;
		}
	}
	if (Best)
	{
		Logic->FireSpecialPower(*this, Module, Best->GetPosition(), Best->GetID());
	}
}
