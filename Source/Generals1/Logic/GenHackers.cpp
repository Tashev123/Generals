// Hackers at the internet, the charges that units stick on things, and what else the unit abilities
// need besides their state machine (GenAbilities.cpp): who may use which ability on what, the things
// an ability leaves in the world, being hacked. Follows HackInternetAIUpdate.cpp, InternetHackContain.cpp,
// StickyBombUpdate.cpp, Object::checkAndDetonateBoobyTrap, ActionManager::canDoSpecialPowerAtObject /
// canDisableVehicleViaHacking / canStealCashViaHacking / canDisableBuildingViaHacking.

#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenUpgrades.h"
#include "Generals1.h"

namespace
{
	int32 HackUnpackingBit() { static const int32 I = GenConditions::Index(TEXT("UNPACKING")); return I; }
	int32 HackPackingBit() { static const int32 I = GenConditions::Index(TEXT("PACKING")); return I; }
	int32 HackFiringBit() { static const int32 I = GenConditions::Index(TEXT("FIRING_A")); return I; }

	constexpr float BoobyTrapScanRange = 20.f; // BOOBY_TRAP_SCAN_RANGE

	/** "If the enemy unit is stealthed and not detected, then we can't attack it!" */
	bool HackHidden(const FGenObject& Target)
	{
		return Target.IsStealthedAndUndetected();
	}
}

// ---- HackInternetAIUpdate -----------------------------------------------------------------------

void FGenObject::CommandHackInternet()
{
	const FGenHackInternet& D = Template->HackInternet;
	bHackInterrupt = false;
	if (!D.bValid || bDead || HackState == EHackState::Unpacking || HackState == EHackState::Hacking)
	{
		return;
	}
	if (HackState == EHackState::Packing)
	{
		bHackAgain = true; // (the pending command of HackInternetAIUpdate::aiDoCommand)
		return;
	}
	// Whatever else it was doing ends here.
	AIMode = EGenAIMode::None;
	ExitAbility(false);
	HuntButton.Reset();
	ClearAttack();
	StopEntering();
	if (bMoving)
	{
		CommandStop();
	}
	// UnpackingState::onEnter
	HackState = EHackState::Unpacking;
	bHackWasContained = IsContained();
	bHackAgain = false;
	HackFrames = FMath::RoundToInt32(float(D.UnpackFrames) * Logic->GetRandom().FRandRange(1.f - D.PackUnpackVariation, 1.f + D.PackUnpackVariation));
	SetModelCondition(HackPackingBit(), false);
	SetModelCondition(HackFiringBit(), false);
	SetModelCondition(HackUnpackingBit(), true);
	Logic->PlayObjectSound(*this, { TEXT("UnitUnpack") });
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hack f%u] #%u p%d %s unpacks to hack the internet (%d frames)%s"), Logic->GetFrame(), ID, Owner,
		*Template->Name, HackFrames, bHackWasContained ? TEXT(", inside") : TEXT(""));
}

void FGenObject::EndHackInternet()
{
	HackState = EHackState::None;
	HackFrames = 0;
	bHackWasContained = false;
	SetModelCondition(HackUnpackingBit(), false);
	SetModelCondition(HackPackingBit(), false);
	SetModelCondition(HackFiringBit(), false);
}

void FGenObject::UpdateHackInternet(uint32 Frame)
{
	const FGenHackInternet& D = Template->HackInternet;
	const bool bContained = IsContained();
	if (bHackWasContained && !bContained)
	{
		// Out of the Internet Center: "We don't need to pack if exiting a building".
		EndHackInternet();
		return;
	}
	// HackInternetAIUpdate::aiDoCommand: told to do something else?
	const bool bOrdered = !bContained && (bHackInterrupt || bMoving || TargetID != GenInvalidObjectID || EnterTarget != GenInvalidObjectID
		|| DockTarget != GenInvalidObjectID || AIMode != EGenAIMode::None || AbilityIndex != INDEX_NONE);
	bHackInterrupt = false;
	switch (HackState)
	{
	case EHackState::Unpacking:
		if (bOrdered)
		{
			EndHackInternet(); // (only a hacker that is hacking or packing makes the order wait)
			return;
		}
		// "The unpacking modelconditionflag gets cleared by specialability::cleanup() after my onEnter() sets it!"
		SetModelCondition(HackUnpackingBit(), true);
		if (HackFrames > 0)
		{
			--HackFrames;
			break;
		}
		// HackInternetState::onEnter
		HackState = EHackState::Hacking;
		SetModelCondition(HackUnpackingBit(), false);
		SetModelCondition(HackFiringBit(), true);
		HackFrames = bContained ? D.CashDelayFast : D.CashDelay;
		break;

	case EHackState::Hacking:
		if (bOrdered)
		{
			// The order waits until the laptop is packed away (PackingState::onEnter).
			HackState = EHackState::Packing;
			HackFrames = FMath::RoundToInt32(float(D.PackFrames) * Logic->GetRandom().FRandRange(1.f - D.PackUnpackVariation, 1.f + D.PackUnpackVariation));
			SetModelCondition(HackFiringBit(), false);
			SetModelCondition(HackPackingBit(), true);
			Logic->PlayObjectSound(*this, { TEXT("UnitPack") });
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hack f%u] #%u p%d %s packs up (%d frames)"), Frame, ID, Owner, *Template->Name, HackFrames);
			break;
		}
		if (IsHacked())
		{
			break; // "Don't hack while hacked, hehe."
		}
		if (HackFrames > 0)
		{
			--HackFrames;
			break;
		}
		{
			// "We have waited the full amount of the delay, so hack some cash from the heavens!"
			int32 Amount = 0;
			for (int32 Level = FMath::Clamp(VeterancyLevel, 0, 3); Level >= 0 && Amount == 0; --Level)
			{
				Amount = D.Cash[Level]; // ("If entry missing, fall through!")
			}
			Amount = Amount > 0 ? Amount : 1;
			Logic->AddMoney(Owner, Amount);
			Logic->ScoreMoneyEarned(Owner, Amount);
			AddExperience(D.XpPerCash);
			const FGenObject* Center = bContained ? Logic->FindObject(ContainedBy) : nullptr;
			const bool bLocal = Owner == Logic->GetLocalPlayer();
			const bool bShow = !(bStealthed && !bDetected && !bLocal) && !(Center && Center->IsStealthedAndUndetected() && Center->GetOwner() != Logic->GetLocalPlayer());
			if (bShow)
			{
				FVector3f Pos = Position + FVector3f(0.f, 0.f, 20.f);
				if (Center)
				{
					// (a client-side random in the original)
					Pos.X += FMath::FRandRange(-0.3f, 0.3f) * Center->GetTemplate()->GeometryMajorRadius;
					Pos.Y += FMath::FRandRange(-0.3f, 0.3f) * Center->GetTemplate()->GeometryMinorRadius;
				}
				Logic->AddFloatingText(FString::Printf(TEXT("+$%d"), Amount), Pos, FLinearColor(0.f, 1.f, 0.f));
			}
			Logic->PlayObjectSound(*this, { TEXT("UnitCashPing") });
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hack f%u] #%u p%d %s hacks $%d (level %d)"), Frame, ID, Owner, *Template->Name, Amount, VeterancyLevel);
			HackFrames = bContained ? D.CashDelayFast : D.CashDelay;
		}
		break;

	case EHackState::Packing:
		if (HackFrames > 0)
		{
			--HackFrames;
			break;
		}
		{
			const bool bAgain = bHackAgain;
			EndHackInternet();
			if (bAgain)
			{
				CommandHackInternet();
			}
		}
		break;

	default:
		break;
	}
}

void UGenGameLogic::IssueHackInternet(const TArray<FGenObjectID>& Units)
{
	FGenCommand C(EGenCommandType::HackInternet);
	C.Units = Units;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecHackInternet(const FGenCommand& C)
{
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (U && !U->IsDead() && !U->IsContained() && U->GetTemplate()->HackInternet.bValid)
		{
			U->OnPlayerCommand(false);
			U->CommandHackInternet();
		}
	}
}

// ---- being hacked -------------------------------------------------------------------------------

bool FGenObject::IsHacked() const
{
	return Logic->GetFrame() < HackedUntilFrame;
}

// ---- StickyBombUpdate ---------------------------------------------------------------------------

void FGenObject::InitStickyBomb(FGenObject* Target, const FGenObject* Bomber)
{
	const FGenStickyBomb& D = Template->StickyBomb;
	const uint32 Now = Logic->GetFrame();
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	StickyTarget = Target ? Target->GetID() : GenInvalidObjectID;
	// "Set the target as the producer ID -- so we can access it from other areas."
	Producer = StickyTarget;
	bStickyActive = true;
	if (LifetimeDieFrame > Now)
	{
		// A timer bomb: it ticks on the second, counting down to the bang.
		const uint32 Pings = (LifetimeDieFrame - Now) / FPS;
		StickyNextPingFrame = LifetimeDieFrame - Pings * FPS;
	}
	else
	{
		StickyNextPingFrame = Now + FPS; // remotely triggered: "make our ping in a second"
	}
	if (!Target)
	{
		return;
	}
	FVector3f Pos = Target->GetPosition();
	if (Target->bImmobile && Bomber)
	{
		// "if bomber has placed bomb on a structure let the bomb just stay where it was first put, so a
		// mine clearing unit can get to it later"
		Pos = Bomber->GetPosition();
		Pos.Z = Logic->GetGroundHeight(Pos.X, Pos.Y);
	}
	else
	{
		Pos.Z += D.OffsetZ; // "ride on the roof of the truck/tank"
	}
	SetPosition(Pos);
	SnapshotPrevious();
	if (IsKindOf(TEXT("BOOBY_TRAP")))
	{
		Target->bBoobyTrapped = true;
	}
	Logic->PlayObjectSound(*this, { TEXT("StickyBombCreated") });
}

void FGenObject::UpdateStickyBomb(uint32 Frame)
{
	if (StickyTarget != GenInvalidObjectID)
	{
		const FGenObject* Target = Logic->FindObject(StickyTarget);
		if (!Target || Target->IsDead() || Target->IsDestroyed())
		{
			// What it sat on is gone, and the charge with it.
			bPendingDestroy = true;
			return;
		}
		FVector3f Pos = Position;
		if (Target->bImmobile)
		{
			Pos.Z = Logic->GetGroundHeight(Pos.X, Pos.Y); // "keep it at ground height for mine clearing units to reach"
		}
		else
		{
			Pos = Target->GetPosition() + FVector3f(0.f, 0.f, Template->StickyBomb.OffsetZ); // "make the bomb follow the target around"
		}
		Position = Pos;
		Velocity = FVector3f::ZeroVector;
	}
	if (Frame >= StickyNextPingFrame)
	{
		StickyNextPingFrame += uint32(GenLogic::GetLogicFPS());
		Logic->PlayObjectSound(*this, { TEXT("UnitBombPing") });
	}
}

void FGenObject::DetonateStickyBomb()
{
	if (bDead || bPendingDestroy)
	{
		return;
	}
	const FGenStickyBomb& D = Template->StickyBomb;
	FGenObject* Trapped = Logic->FindObject(StickyTarget);
	const FGenWeaponTemplate* W = D.GeometryWeapon.IsEmpty() ? nullptr : FGenGameData::Get().FindWeapon(D.GeometryWeapon);
	if (W && Trapped)
	{
		// "We need to hurt people based on the size of the thing we are on. The radius in our weapon is
		// the radius beyond our borders"
		const float Bounding = Trapped->GetTemplate()->GetBoundingCircleRadius();
		const float PrimaryRange = W->PrimaryDamageRadius + Bounding;
		const float SecondaryRange = W->SecondaryDamageRadius + Bounding;
		const float Radius = FMath::Max(PrimaryRange, SecondaryRange);
		const FVector3f Center = Trapped->GetPosition();
		TArray<FGenObject*> Victims;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			FGenObject* O = Logic->FindObject(Id);
			if (!O || O->IsDead() || O->IsDestroyed() || O->IsContained() || O->IsProjectile())
			{
				continue;
			}
			// (FROM_BOUNDINGSPHERE_3D: to the nearest point of the victim's bounding sphere)
			const float Dist = FMath::Max(0.f, FVector3f::Distance(Center, O->GetPosition()) - O->GetTemplate()->GetBoundingCircleRadius());
			if (Dist <= Radius)
			{
				Victims.Add(O);
			}
		}
		for (FGenObject* O : Victims)
		{
			const float Dist = FMath::Max(0.f, FVector3f::Distance(Center, O->GetPosition()) - O->GetTemplate()->GetBoundingCircleRadius());
			O->AttemptDamage(Dist <= PrimaryRange ? W->PrimaryDamage : W->SecondaryDamage, W->DamageType, W->DeathType, ID);
		}
		if (!D.GeometryFX.IsEmpty())
		{
			Logic->DoFXPos(D.GeometryFX, Center, nullptr, 0.f, nullptr, SecondaryRange);
		}
	}
	if (IsKindOf(TEXT("BOOBY_TRAP")) && Trapped)
	{
		Trapped->bBoobyTrapped = false;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [charge f%u] #%u p%d %s on #%u goes off"), Logic->GetFrame(), ID, Owner, *Template->Name, StickyTarget);
	Kill(); // "Most things just fire weapons in their death modules"
}

bool FGenObject::CheckAndDetonateBoobyTrap(const FGenObject* Victim)
{
	if (!bBoobyTrapped)
	{
		return false;
	}
	const float Range = BoobyTrapScanRange + Template->GetBoundingCircleRadius();
	FGenObject* Trap = nullptr;
	float BestDistSq = FLT_MAX;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		FGenObject* O = Logic->FindObject(Id);
		// "Sticky bombs call the thing they are on their producer for just such an occasion"
		if (!O || O->IsDead() || O->IsDestroyed() || !O->IsKindOf(TEXT("BOOBY_TRAP")) || O->GetProducer() != ID)
		{
			continue;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(Position.X, Position.Y));
		if (DistSq <= Range * Range && DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Trap = O;
		}
	}
	if (!Trap || !Trap->GetTemplate()->StickyBomb.bValid)
	{
		return false;
	}
	if (Victim && Logic->GetRelationship(Trap->GetOwner(), Victim->GetOwner()) == EGenRelationship::Allies)
	{
		return false; // "Friends don't touch friends boobies."
	}
	Trap->DetonateStickyBomb();
	return true; // (the trapped status was cleared by the bomb, as it set it)
}

// ---- what an ability leaves in the world --------------------------------------------------------

FGenObject* FGenObject::CreateAbilityObject(int32 InAbility)
{
	// SpecialAbilityUpdate::createSpecialObject
	if (!Template->Abilities.IsValidIndex(InAbility))
	{
		return nullptr;
	}
	const FGenAbilityData& D = Template->Abilities[InAbility];
	const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(D.SpecialObject);
	if (!Thing)
	{
		return nullptr;
	}
	if (Thing->LaserDraw.bValid)
	{
		// A beam is nothing but its picture: the unit holds it, and the client draws it from there.
		AbilityBeam = Thing;
		return nullptr;
	}
	AbilityObjects.SetNum(Template->Abilities.Num());
	TArray<FGenObjectID>& List = AbilityObjects[InAbility];
	if (GetAbilityObjectCount(InAbility) >= D.MaxSpecialObjects)
	{
		if (D.bSpecialObjectsPersistent)
		{
			return nullptr; // "we have reached our limit we can have, then don't allow any more to be created"
		}
		KillAbilityObjects(InAbility);
	}
	FGenObject* Obj = Logic->CreateObject(Thing, Owner, Position, Orientation);
	if (Obj)
	{
		List.Add(Obj->GetID());
		Obj->XPSink = ID; // "So we can get experience from it when it blows up"
	}
	return Obj;
}

void FGenObject::KillAbilityObjects(int32 InAbility)
{
	// SpecialAbilityUpdate::killSpecialObjects
	if (AbilityIndex == InAbility || InAbility == INDEX_NONE)
	{
		AbilityBeam = nullptr;
	}
	if (!AbilityObjects.IsValidIndex(InAbility))
	{
		return;
	}
	for (FGenObjectID Id : AbilityObjects[InAbility])
	{
		if (FGenObject* O = Logic->FindObject(Id))
		{
			if (O->IsKindOf(TEXT("BOOBY_TRAP")))
			{
				if (FGenObject* Trapped = Logic->FindObject(O->StickyTarget)) { Trapped->bBoobyTrapped = false; }
			}
			O->bPendingDestroy = true;
		}
	}
	AbilityObjects[InAbility].Reset();
}

void FGenObject::PruneAbilityObjects()
{
	// SpecialAbilityUpdate::validateSpecialObjects: "Sometimes special objects can be destroyed or removed,
	// or detonate, etc! When they go missing, they ... free up slots"
	for (TArray<FGenObjectID>& List : AbilityObjects)
	{
		for (int32 i = List.Num() - 1; i >= 0; --i)
		{
			const FGenObject* O = Logic->FindObject(List[i]);
			if (!O || O->IsDestroyed())
			{
				List.RemoveAt(i);
			}
		}
	}
}

int32 FGenObject::GetAbilityObjectCount(int32 InAbility) const
{
	return AbilityObjects.IsValidIndex(InAbility) ? AbilityObjects[InAbility].Num() : 0;
}

bool FGenObject::HasAbilityObjectOn(int32 InAbility, FGenObjectID Target) const
{
	if (!AbilityObjects.IsValidIndex(InAbility))
	{
		return false;
	}
	for (FGenObjectID Id : AbilityObjects[InAbility])
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && O->GetProducer() == Target)
		{
			return true;
		}
	}
	return false;
}

const FGenThingTemplate* FGenObject::GetAbilityBeam(FGenObjectID& OutTarget, FString& OutBone) const
{
	if (!AbilityBeam || AbilityIndex == INDEX_NONE || bDead)
	{
		return nullptr;
	}
	OutTarget = AbilityTarget;
	OutBone = Template->Abilities[AbilityIndex].SpecialObjectBone;
	return AbilityBeam;
}

// ---- weapons picked by a button or an ability ---------------------------------------------------

void FGenObject::CommandAttackWithSlot(FGenObjectID Target, int32 Slot)
{
	if (Slot < 0 || Slot > 2 || !Weapons[Slot].Template)
	{
		return;
	}
	CommandAttack(Target);
	if (TargetID == Target)
	{
		WeaponLockSlot = Slot; // LOCKED_TEMPORARILY: "just till ... the attack is done"
	}
}

bool FGenObject::IgnoresAsAutoTarget(const FGenObject& Other) const
{
	if (!Other.IsKindOf(TEXT("STRUCTURE")))
	{
		return false;
	}
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	if (!Players.IsValidIndex(Owner) || !Players[Owner].bHuman || Template->bAutoAcquireBuildings)
	{
		return false; // "if I am a computer-controlled opponent, auto-aquire enemy buildings"
	}
	if (Other.IsKindOf(TEXT("FS_BASE_DEFENSE")))
	{
		return false; // "Don't reject base defenses."
	}
	// "Don't reject garrisoned buildings that can attack"
	if (Other.GetContain() && (Other.HasWeapons() || (Logic->IsPassengerAllowedToFireFrom(Other) && Logic->GetRiders(Other).Num() > 0)))
	{
		return false;
	}
	return true;
}

// ---- who may use which ability on what ----------------------------------------------------------

bool UGenGameLogic::CanUseAbilityOn(const FGenObject& Unit, int32 Module, const FGenObject* Target, bool bCheckReady) const
{
	const TArray<FGenSpecialPowerModule>& Modules = Unit.GetTemplate()->PowerModules;
	if (!Modules.IsValidIndex(Module) || Modules[Module].Type != EGenPowerModule::SpecialAbility)
	{
		return false;
	}
	const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(Modules[Module].Power);
	const int32 Ability = Unit.FindAbility(Modules[Module].Power);
	if (!Power || Ability == INDEX_NONE || (bCheckReady && !IsPowerReady(Unit, Module)))
	{
		return false;
	}
	const EGenAbilityKind Kind = Power->AbilityKind;
	if (!Target)
	{
		// ActionManager::canDoSpecialPower: only the remote charges do anything without a target - they go off.
		return Kind == EGenAbilityKind::RemoteCharges;
	}
	if (Target->IsDead() || Target->IsDestroyed() || Target == &Unit)
	{
		return false;
	}
	const EGenRelationship R = GetRelationship(Unit.GetOwner(), Target->GetOwner());
	const bool bStructure = Target->IsKindOf(TEXT("STRUCTURE"));
	const bool bVehicle = Target->IsKindOf(TEXT("VEHICLE"));
	const bool bHackable = Target->IsKindOf(TEXT("CAPTURABLE")) && !Target->IsKindOf(TEXT("REBUILD_HOLE"));
	switch (Kind)
	{
	case EGenAbilityKind::CaptureBuilding:
	case EGenAbilityKind::LotusCapture:
		return CanCaptureBuilding(Unit, *Target, false);

	case EGenAbilityKind::LotusVehicleHack:
		// canDisableVehicleViaHacking
		return R == EGenRelationship::Enemies && bVehicle && !Target->IsKindOf(TEXT("AIRCRAFT")) && !Target->IsAirborneTarget() && !HackHidden(*Target);

	case EGenAbilityKind::LotusCashHack:
		// canStealCashViaHacking
		return R == EGenRelationship::Enemies && Target->IsKindOf(TEXT("CASH_GENERATOR")) && bHackable && !Target->IsUnderConstruction() && !HackHidden(*Target);

	case EGenAbilityKind::HackerDisableBuilding:
		return R == EGenRelationship::Enemies && bStructure && bHackable && !Target->IsUnderConstruction() && !HackHidden(*Target);

	case EGenAbilityKind::TankHunterTNT:
		return bStructure || (bVehicle && !Target->IsKindOf(TEXT("AIRCRAFT")));

	case EGenAbilityKind::BoobyTrap:
		// "We can booby trap any building that is allied or neutral" (and one trap is enough)
		return bStructure && R != EGenRelationship::Enemies && !Target->IsBoobyTrapped();

	case EGenAbilityKind::LaserGuidedMissiles:
		return bVehicle && R == EGenRelationship::Enemies && !HackHidden(*Target); // "Can only use laser guided missiles on vehicles!"

	case EGenAbilityKind::Disguise:
		// ActionManager::canDoSpecialPowerAtObject: any vehicle on wheels or tracks (no aircraft, boat, cliff
		// jumper or train) - "don't allow it to disguise as another bomb truck -- that's just plain dumb" was
		// left out. (Trains aren't in the port.)
		return bVehicle && !Target->IsKindOf(TEXT("AIRCRAFT")) && !Target->IsKindOf(TEXT("BOAT")) && !Target->IsKindOf(TEXT("CLIFF_JUMPER"))
;

	case EGenAbilityKind::RemoteCharges:
	case EGenAbilityKind::TimedCharges:
	{
		if (Target->IsKindOf(TEXT("BRIDGE")) || Target->IsKindOf(TEXT("BRIDGE_TOWER")) || !(bStructure || bVehicle))
		{
			return false;
		}
		// "Make sure we have enough equipment to place an additional charge", and one charge of either
		// kind per target.
		const FGenAbilityData& D = Unit.GetTemplate()->Abilities[Ability];
		if (Unit.GetAbilityObjectCount(Ability) >= D.MaxSpecialObjects || Unit.HasAbilityObjectOn(Ability, Target->GetID()))
		{
			return false;
		}
		const EGenAbilityKind OtherKind = Kind == EGenAbilityKind::RemoteCharges ? EGenAbilityKind::TimedCharges : EGenAbilityKind::RemoteCharges;
		for (int32 i = 0; i < Unit.GetTemplate()->Abilities.Num(); ++i)
		{
			const FGenSpecialPowerTemplate* Other = FGenUpgradeData::Get().FindPower(Unit.GetTemplate()->Abilities[i].Power);
			if (Other && Other->AbilityKind == OtherKind && Unit.HasAbilityObjectOn(i, Target->GetID()))
			{
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}
