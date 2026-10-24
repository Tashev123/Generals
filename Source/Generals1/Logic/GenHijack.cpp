// Taking a vehicle over: the hijacker (ConvertToHijackedVehicleCrateCollide and HijackerUpdate) and the
// terrorist's car bomb (ConvertToCarBombCrateCollide).
//
// Both are "crates" carried by the soldier: sent into a vehicle (an enter order), he converts it when he
// touches it. A hijacked enemy vehicle is his side's from then on; he rides in it, hidden, and when it
// is destroyed he is set down where it was (under a parachute if it was in the air) - unless the
// vehicle can't eject a pilot, in which case he is simply gone into it. A car bomb is any vehicle with a
// CARBOMB weapon set (the civilian cars; some of the GLA's own): it becomes his side's with that set - a
// bomb that goes off on whatever it is driven into - and he is gone.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Audio/GenAudioData.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	const FString CarBombFlag(TEXT("CARBOMB"));

	/** WeaponTemplateSetVector::findWeaponTemplateSet with CARBOMB: the best set has that flag itself. */
	bool HasCarBombSet(const FGenThingTemplate& T)
	{
		const FGenWeaponSetEntry* Set = T.FindWeaponSet({ CarBombFlag });
		return Set && Set->Conditions.ContainsByPredicate([](const FString& C) { return C.Equals(CarBombFlag, ESearchCase::IgnoreCase); });
	}
}

bool UGenGameLogic::CanTakeOverVehicle(const FGenObject& Unit, const FGenObject& Vehicle) const
{
	const FGenThingTemplate::FVehicleTakeover& D = Unit.GetTemplate()->VehicleTakeover;
	if (D.Kind == FGenThingTemplate::FVehicleTakeover::None || &Unit == &Vehicle || Vehicle.IsDead() || Vehicle.IsDestroyed() || Unit.IsDead())
	{
		return false;
	}
	// CrateCollide::isValidToExecute: a real unit (it has an AI), of the kinds the soldier's module asks
	// for, on the ground, no parachute.
	const FGenThingTemplate& VT = *Vehicle.GetTemplate();
	if (!VT.bHasAI || Vehicle.IsContained() || Vehicle.IsKindOf(TEXT("PARACHUTE"))
		|| Vehicle.GetPosition().Z - Vehicle.GetSurfaceHeight() > 9.f)
	{
		return false;
	}
	for (const FString& K : D.RequiredKindOf) { if (!Vehicle.IsKindOf(*K)) { return false; } }
	for (const FString& K : D.ForbiddenKindOf) { if (Vehicle.IsKindOf(*K)) { return false; } }
	if (Vehicle.IsKindOf(TEXT("AIRCRAFT")) || Vehicle.IsKindOf(TEXT("BOAT")))
	{
		return false; // "Can't hijack planes and boats!" / "Can't make carbombs out of planes and boats!"
	}
	if (D.Kind == FGenThingTemplate::FVehicleTakeover::Hijack)
	{
		// ConvertToHijackedVehicleCrateCollide::isValidToExecute, ActionManager::canHijackVehicle
		if (Vehicle.IsKindOf(TEXT("IMMUNE_TO_CAPTURE")) || Vehicle.IsKindOf(TEXT("DRONE")) || !Vehicle.IsKindOf(TEXT("VEHICLE")) || Vehicle.bHijacked
			|| GetRelationship(Unit.GetOwner(), Vehicle.GetOwner()) != EGenRelationship::Enemies)
		{
			return false;
		}
		// "Do not jack vehicles that may carry hostile passengers" ("allow empty transports to be hijacked").
		return !(Vehicle.IsKindOf(TEXT("TRANSPORT")) && Vehicle.GetContain() && GetContainCount(Vehicle) > 0);
	}
	// ConvertToCarBombCrateCollide::isValidToExecute: "check to see if this other object has a carbomb weapon
	// set that isn't in use".
	return !Vehicle.bCarBomb && HasCarBombSet(VT);
}

void FGenObject::TakeOverVehicle(FGenObject& Vehicle)
{
	const FGenThingTemplate::FVehicleTakeover& D = Template->VehicleTakeover;
	const uint32 Frame = Logic->GetFrame();
	if (D.Kind == FGenThingTemplate::FVehicleTakeover::CarBomb)
	{
		// ConvertToCarBombCrateCollide::executeCrateBehavior
		if (Vehicle.CheckAndDetonateBoobyTrap(this) && (Vehicle.IsDead() || bDead))
		{
			return; // "Whoops, it was mined."
		}
		static const int32 CarBombBit = GenConditions::Index(TEXT("WEAPONSET_CARBOMB"));
		Vehicle.SetFlags.AddUnique(CarBombFlag);
		Vehicle.RefreshWeaponSet();
		Vehicle.SetModelCondition(CarBombBit, true);
		if (!D.FX.IsEmpty())
		{
			Logic->DoFXObj(D.FX, Vehicle);
		}
		Vehicle.ClearAttack();
		Vehicle.CommandStop();
		Vehicle.SetOwner(Owner); // defect
		Vehicle.bCarBomb = true;
		Vehicle.SetVeterancyLevel(VeterancyLevel);
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hijack f%u] #%u %s makes #%u %s a car bomb for p%d (%s)"), Frame, ID, *Template->Name, Vehicle.ID,
			*Vehicle.Template->Name, Owner, Vehicle.Weapons[0].Template ? *Vehicle.Weapons[0].Template->Name : TEXT("-"));
		bPendingDestroy = true;
		return;
	}

	// ConvertToHijackedVehicleCrateCollide::executeCrateBehavior: "I claim this car in the name of the GLA".
	Logic->EvaEvent(TEXT("VehicleStolen"), Vehicle.Owner);
	Vehicle.ClearAttack();
	Vehicle.StopConstructing(); // ("just in case this target is a dozer ... so the previous owner does not benefit")
	Vehicle.StopDocking();
	Vehicle.CommandStop();
	Vehicle.SetOwner(Owner);
	Vehicle.bHijacked = true;
	Logic->PlaySoundAt(TEXT("HijackDriver"), *this);
	const int32 Highest = FMath::Max(VeterancyLevel, Vehicle.VeterancyLevel);
	SetVeterancyLevel(Highest, false);
	Vehicle.SetVeterancyLevel(Highest, false);
	bool bCanEject = false;
	for (const FGenDieModule& M : Vehicle.Template->DieModules)
	{
		bCanEject |= M.Type == EGenDieModule::EjectPilot;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hijack f%u] #%u %s hijacks #%u %s for p%d%s"), Frame, ID, *Template->Name, Vehicle.ID,
		*Vehicle.Template->Name, Owner, bCanEject ? TEXT(", and rides in it") : TEXT(""));
	if (!bCanEject)
	{
		bPendingDestroy = true;
		return;
	}
	// "We are going to ride in this vehicle for a while": hidden, out of everybody's way.
	HijackedVehicle = Vehicle.ID;
	bHidden = true;
	CommandStop();
	ClearAttack();
	Velocity = FVector3f::ZeroVector;
	Logic->GetPathfinder().RemoveUnit(ID);
	SetPosition(Vehicle.Position);
	SnapshotPrevious();
}

void FGenObject::UpdateHijackerRide(uint32 Frame)
{
	// HijackerUpdate::update: along with the vehicle, sharing its rank; when it is gone, out again.
	FGenObject* Vehicle = Logic->FindObject(HijackedVehicle);
	if (Vehicle && !Vehicle->IsDead() && !Vehicle->IsDestroyed() && Vehicle->bHijacked)
	{
		Position = Vehicle->Position;
		HijackEjectPos = Vehicle->Position;
		bHijackVehicleAirborne = Vehicle->Position.Z - Vehicle->GetSurfaceHeight() > 9.f;
		const int32 Highest = FMath::Max(VeterancyLevel, Vehicle->VeterancyLevel);
		if (Highest != VeterancyLevel) { SetVeterancyLevel(Highest, false); }
		if (Highest != Vehicle->VeterancyLevel) { Vehicle->SetVeterancyLevel(Highest, false); }
		SnapshotPrevious();
		return;
	}
	// "The car we have been driving is dead now, and has safely ejected us."
	HijackedVehicle = GenInvalidObjectID;
	bHidden = false;
	FVector3f At = HijackEjectPos;
	At.Z = Logic->GetGroundHeight(At.X, At.Y);
	SetPosition(bHijackVehicleAirborne ? HijackEjectPos : At);
	SnapshotPrevious();
	CommandStop();
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hijack f%u] #%u %s is out again at (%.0f, %.0f)"), Frame, ID, *Template->Name, HijackEjectPos.X, HijackEjectPos.Y);
	if (bHijackVehicleAirborne)
	{
		// (Out of an aircraft in the air: under a parachute - not that a hijacker can take one.)
		const FGenThingTemplate* ChuteT = FGenGameData::Get().FindTemplate(Template->HijackerParachute);
		if (ChuteT && ChuteT->bParachute)
		{
			if (FGenObject* Chute = Logic->CreateObject(ChuteT, Owner, HijackEjectPos, Orientation))
			{
				Chute->CarryUnderParachute(*this, nullptr, FVector3f::ZeroVector);
				Chute->SnapshotPrevious();
			}
		}
	}
	bHijackVehicleAirborne = false;
}

// ---- the saboteur ---------------------------------------------------------------------------------
//
// Sabotage...CrateCollide: sent into an enemy building (an enter order, like the hijacker's), the saboteur does
// whatever his modules do to that kind of building - a power plant cuts its owner's power for a while, a
// barracks, war factory or airfield shuts down, a superweapon's or command center's powers start recharging
// over, a supply center gives up money, an internet center loses its hackers and satellite hacks for a while,
// a fake building blows up - and is gone.

bool UGenGameLogic::CanSabotage(const FGenObject& Unit, const FGenObject& Building) const
{
	const FGenThingTemplate::FSabotage& D = Unit.GetTemplate()->Sabotage;
	if (!D.Any() || Unit.IsDead() || Building.IsDead() || Building.IsDestroyed() || !Building.IsKindOf(TEXT("STRUCTURE")) || Building.IsUnderConstruction()
		|| !Players.IsValidIndex(Building.GetOwner()) || Players[Building.GetOwner()].bNeutral
		|| GetRelationship(Unit.GetOwner(), Building.GetOwner()) != EGenRelationship::Enemies)
	{
		return false; // (CrateCollide::isValidToExecute: "Nothing Neutral"; BuildingPickup; ENEMIES)
	}
	return (D.PowerFrames > 0 && Building.IsKindOf(TEXT("FS_POWER")))
		|| (D.bSuperweapon && (Building.IsKindOf(TEXT("FS_SUPERWEAPON")) || Building.IsKindOf(TEXT("FS_STRATEGY_CENTER"))))
		|| (D.bCommandCenter && Building.IsKindOf(TEXT("COMMANDCENTER")))
		|| (D.StealCash > 0 && Building.IsKindOf(TEXT("FS_SUPPLY_CENTER")))
		|| (D.FactoryFrames > 0 && !Building.IsKindOf(TEXT("AIRCRAFT_CARRIER"))
			&& (Building.IsKindOf(TEXT("FS_BARRACKS")) || Building.IsKindOf(TEXT("FS_WARFACTORY")) || Building.IsKindOf(TEXT("FS_AIRFIELD"))))
		|| (D.bFakeBuilding && Building.IsKindOf(TEXT("FS_FAKE")))
		|| (D.InternetFrames > 0 && Building.IsKindOf(TEXT("FS_INTERNET_CENTER")));
}

void UGenGameLogic::SabotageBuilding(FGenObject& Saboteur, FGenObject& Building)
{
	const FGenThingTemplate::FSabotage& D = Saboteur.GetTemplate()->Sabotage;
		const int32 Victim = Building.GetOwner();
	const bool bVictimLocal = Victim == LocalPlayer;
	const FGenAudioData& AD = FGenAudioData::Get();
	TArray<FString, TInlineAllocator<4>> Done;
	// TheRadar->tryInfiltrationEvent, and doSabotageFeedbackFX: the building flashes as if selected.
	if (bVictimLocal)
	{
		TryRadarEvent(EGenRadarEvent::Infiltration, Building.GetPosition());
	}
	Building.FlashAs(FLinearColor::White);
	auto Sabotaged = [&](const TCHAR* Misc)
	{
		PlaySoundAt(AD.GetMisc(Misc), Building);
		if (bVictimLocal)
		{
			EvaEvent(TEXT("BuildingSabotaged"), Victim);
		}
	};
	auto RechargePowers = [&]()
	{
		// SpecialPowerModule::startPowerRecharge, for every power the building has.
		const TArray<FGenSpecialPowerModule>& Modules = Building.GetTemplate()->PowerModules;
		for (int32 m = 0; m < Modules.Num(); ++m)
		{
			const FGenSpecialPowerTemplate* T = FGenUpgradeData::Get().FindPower(Modules[m].Power);
			if (!T)
			{
				continue;
			}
			if (Building.PowerReady.IsValidIndex(m))
			{
				Building.PowerReady[m] = Frame + uint32(T->ReloadTime);
			}
			if (T->bSharedSyncedTimer)
			{
				Players[Victim].PowerReadyFrame.Add(T->Name.ToLower(), Frame + uint32(T->ReloadTime));
			}
		}
	};
	if (D.PowerFrames > 0 && Building.IsKindOf(TEXT("FS_POWER")))
	{
		Sabotaged(TEXT("SabotageShutDownBuilding"));
		Players[Victim].PowerSabotagedUntil = FMath::Max(Players[Victim].PowerSabotagedUntil, Frame + uint32(D.PowerFrames));
		Done.Add(TEXT("power cut"));
	}
	if (D.bSuperweapon && (Building.IsKindOf(TEXT("FS_SUPERWEAPON")) || Building.IsKindOf(TEXT("FS_STRATEGY_CENTER"))))
	{
		Sabotaged(TEXT("SabotageResetTimeBuilding"));
		RechargePowers();
		Done.Add(TEXT("powers reset"));
	}
	if (D.bCommandCenter && Building.IsKindOf(TEXT("COMMANDCENTER")))
	{
		Sabotaged(TEXT("SabotageResetTimeBuilding"));
		RechargePowers();
		Done.Add(TEXT("powers reset"));
	}
	if (D.StealCash > 0 && Building.IsKindOf(TEXT("FS_SUPPLY_CENTER")))
	{
		PlaySoundAt(AD.GetMisc(TEXT("MoneyWithdrawSound")), Building);
		const int32 Cash = Withdraw(Victim, FMath::Min(D.StealCash, GetMoney(Victim)));
		if (Cash > 0)
		{
			Deposit(Saboteur.GetOwner(), Cash);
			ScoreMoneyEarned(Saboteur.GetOwner(), Cash);
			if (bVictimLocal)
			{
				EvaEvent(TEXT("CashStolen"), Victim);
			}
			AddFloatingText(FString::Printf(TEXT("+$%d"), Cash), Saboteur.GetPosition() + FVector3f(0.f, 0.f, 20.f), FLinearColor(0.f, 1.f, 0.f));
			AddFloatingText(FString::Printf(TEXT("-$%d"), Cash), Building.GetPosition() + FVector3f(0.f, 0.f, 30.f), FLinearColor(1.f, 0.f, 0.f));
		}
		else if (bVictimLocal)
		{
			EvaEvent(TEXT("BuildingSabotaged"), Victim);
		}
		Done.Add(FString::Printf(TEXT("$%d stolen"), Cash));
	}
	if (D.FactoryFrames > 0 && !Building.IsKindOf(TEXT("AIRCRAFT_CARRIER"))
		&& (Building.IsKindOf(TEXT("FS_BARRACKS")) || Building.IsKindOf(TEXT("FS_WARFACTORY")) || Building.IsKindOf(TEXT("FS_AIRFIELD"))))
	{
		Sabotaged(TEXT("SabotageShutDownBuilding"));
		Building.HackUntil(Frame + uint32(D.FactoryFrames)); // DISABLED_HACKED
		Done.Add(TEXT("shut down"));
	}
	if (D.InternetFrames > 0 && Building.IsKindOf(TEXT("FS_INTERNET_CENTER")))
	{
		// (Its satellite hacks are off while it is: they only work from a building that isn't disabled.)
		Sabotaged(TEXT("SabotageShutDownBuilding"));
		const uint32 Until = Frame + uint32(D.InternetFrames);
		Building.HackUntil(Until);
		for (FGenObjectID Id : GetRiders(Building))
		{
			if (FGenObject* Hacker = FindObject(Id))
			{
				Hacker->HackUntil(Until); // "disableHacker"
			}
		}
		Done.Add(TEXT("hackers off"));
	}
	if (D.bFakeBuilding && Building.IsKindOf(TEXT("FS_FAKE")))
	{
		Building.AttemptDamage(Building.GetMaxHealth(), TEXT("UNRESISTABLE"), TEXT("DETONATED"), Saboteur.GetID()); // "a fake is blown up"
		Done.Add(TEXT("fake blown up"));
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [sabotage f%u] #%u p%d %s sabotages #%u p%d %s: %s"), Frame, Saboteur.GetID(), Saboteur.GetOwner(), *Saboteur.GetTemplate()->Name, Building.GetID(), Victim,
		*Building.GetTemplate()->Name, *FString::Join(Done, TEXT(", ")));
	Saboteur.bPendingDestroy = true; // ("TheGamedestroyObject( getObject() )": the saboteur is used up)
}
