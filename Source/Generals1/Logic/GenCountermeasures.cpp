// What keeps an aircraft from being hit, and what becomes of its pilot when it is.
// Follows CountermeasuresBehavior.cpp (flares), JetAIUpdate::getSneakyTargetingOffset (the Aurora's attack
// run), EjectPilotDie.cpp, VeterancyCrateCollide.cpp (as far as pilots go) and PilotFindVehicleUpdate.cpp.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Logic/GenSkirmishAI.h"
#include "Audio/GenAudioData.h"
#include "Generals1.h"

namespace
{
	constexpr float AirborneThreshold = 9.f; // SIGNIFICANTLY_ABOVE_TERRAIN_THRESHOLD
}

// ---- flares -------------------------------------------------------------------------------------

void FGenObject::ReportMissileForCountermeasures(FGenObject& Missile)
{
	// CountermeasuresBehavior::reportMissileForCountermeasures: whether this missile will be fooled is
	// decided the moment it is fired.
	const FGenCountermeasures& D = Template->Countermeasures;
	if (CMAvailable + CMFlares.Num() <= 0 || Logic->GetRandom().FRand() >= D.EvasionRate)
	{
		return;
	}
	const uint32 Frame = Logic->GetFrame();
	Missile.Projectile.DecoyFrame = Frame + uint32(FMath::Max(1, D.MissileDecoyFrames));
	if (CMFlares.Num() == 0 && CMReactionFrame == 0)
	{
		// "We need to launch our first volley of countermeasures, but we can't do it now. If we do,
		// it'll look too artificial."
		CMReactionFrame = Frame + uint32(FMath::Max(1, D.ReactionFrames));
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [flare f%u] #%u %s: missile #%u will go for a flare (%d flares on board)"), Frame, ID, *Template->Name,
		Missile.ID, CMAvailable);
}

FGenObjectID FGenObject::CalculateCountermeasureToDivertTo() const
{
	// "Flares are pushed to the front of the list, but we only want to acquire the "newest" of the flares":
	// the nearest one of the last volley.
	const int32 First = FMath::Max(0, CMFlares.Num() - FMath::Max(Template->Countermeasures.VolleySize, 1));
	FGenObjectID Best = GenInvalidObjectID;
	float BestDistSq = FLT_MAX;
	for (int32 i = CMFlares.Num() - 1; i >= First; --i)
	{
		if (const FGenObject* Flare = Logic->FindObject(CMFlares[i]))
		{
			const float DistSq = FVector2f::DistSquared(FVector2f(Flare->Position.X, Flare->Position.Y), FVector2f(Position.X, Position.Y));
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = CMFlares[i];
			}
		}
	}
	return Best;
}

void FGenObject::UpdateCountermeasures(uint32 Frame)
{
	// CountermeasuresBehavior::update
	const FGenCountermeasures& D = Template->Countermeasures;
	CMFlares.RemoveAll([this](FGenObjectID Id)
	{
		const FGenObject* Flare = Logic->FindObject(Id);
		return !Flare || Flare->bDead || Flare->bPendingDestroy;
	});

	if (IsAirborneTarget() && CMAvailable > 0)
	{
		// The first volley once the reaction time is up, then one every so often until they are spent.
		if ((CMReactionFrame != 0 && Frame >= CMReactionFrame) || (CMNextVolleyFrame != 0 && Frame >= CMNextVolleyFrame))
		{
			CMReactionFrame = 0;
			LaunchCountermeasureVolley(Frame);
			CMNextVolleyFrame = CMAvailable > 0 ? Frame + uint32(FMath::Max(1, D.FramesBetweenVolleys)) : 0;
		}
	}

	// "Aircraft that don't auto-reload require landing at an airfield for resupply."
	if (CMAvailable <= 0 && D.ReloadFrames > 0)
	{
		if (CMReloadFrame == 0)
		{
			CMReloadFrame = Frame + uint32(D.ReloadFrames);
		}
		else if (Frame >= CMReloadFrame)
		{
			ReloadCountermeasures();
		}
	}
}

void FGenObject::ReloadCountermeasures()
{
	const FGenCountermeasures& D = Template->Countermeasures;
	if (!D.bValid)
	{
		return;
	}
	CMAvailable = D.NumberOfVolleys * D.VolleySize;
	CMReloadFrame = 0;
	CMReactionFrame = 0;
	CMNextVolleyFrame = 0;
}

void FGenObject::LaunchCountermeasureVolley(uint32 Frame)
{
	// CountermeasuresBehavior::launchVolley: "Each flare in a volley will calculate a different vector to
	// fly out. ... With only one flare, it'll come straight out the back. Two flares will launch at the
	// extreme positive and negative angle."
	const FGenCountermeasures& D = Template->Countermeasures;
	const FGenThingTemplate* FlareT = FGenGameData::Get().FindTemplate(D.FlareTemplate);
	if (!FlareT)
	{
		CMAvailable = 0;
		return;
	}
	const FVector2f Dir = GetDirection2D();
	float Speed = Velocity.Size();
	if (Speed < 1.f)
	{
		Speed = -10.f;
	}
	for (int32 i = 0; i < D.VolleySize && CMAvailable > 0; ++i)
	{
		const float Ratio = D.VolleySize > 1 ? float(i) / float(D.VolleySize - 1) * 2.f - 1.f : 0.f;
		const float Angle = Ratio * D.VolleyArcAngle;
		const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);
		const FVector3f Push = FVector3f(Dir.X * C - Dir.Y * S, Dir.X * S + Dir.Y * C, 0.f) * (Speed * D.VolleyVelocityFactor);

		FGenObject* Flare = Logic->CreateObject(FlareT, Owner, Position, Orientation);
		if (!Flare)
		{
			break;
		}
		Flare->Producer = ID;
		Flare->Velocity = Velocity; // physics->transferVelocityTo
		Flare->ApplyMotiveForce(Push, Frame);
		Flare->SnapshotPrevious();
		--CMAvailable;
		CMFlares.Add(Flare->ID);
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [flare f%u] #%u %s launches a volley (%d flares left, %d in the air)"), Frame, ID, *Template->Name,
		CMAvailable, CMFlares.Num());
}

// ---- the Aurora's attack run ---------------------------------------------------------------------

bool FGenObject::GetSneakyTargetingOffset(FVector3f* OutOffset) const
{
	// JetAIUpdate::getSneakyTargetingOffset
	if (JetAttackersMissExpireFrame == 0 || Logic->GetFrame() >= JetAttackersMissExpireFrame || bDead)
	{
		return false;
	}
	if (OutOffset)
	{
		const FVector2f Dir = GetDirection2D();
		*OutOffset = FVector3f(Dir.X * Template->JetSneakyOffset, Dir.Y * Template->JetSneakyOffset, 0.f);
	}
	return true;
}

// ---- pilots -------------------------------------------------------------------------------------

void FGenObject::EjectPilot(const FGenDieModule& Module)
{
	// EjectPilotDie::onDie
	const bool bInAir = Position.Z - Logic->GetGroundHeight(Position.X, Position.Y) > AirborneThreshold;
	EjectPilotBy(bInAir ? Module.AirOCL : Module.GroundOCL);
}

void FGenObject::EjectPilotBy(const FString& OCL)
{
	// EjectPilotDie::ejectPilot
	if (OCL.IsEmpty())
	{
		return;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [pilot f%u] #%u %s (rank %d) loses its pilot at height %.0f: %s"), Logic->GetFrame(), ID, *Template->Name,
		VeterancyLevel, Position.Z - Logic->GetGroundHeight(Position.X, Position.Y), *OCL);
	Logic->DoOCL(OCL, *this);
	Logic->PlayObjectSound(*this, { TEXT("VoiceEject") });
	Logic->PlayObjectSound(*this, { TEXT("SoundEject") });
}

void FGenObject::MakeUnmanned(FGenObjectID Sniper)
{
	if (bUnmanned || bDead)
	{
		return;
	}
	// "Make it unmanned, so units can easily check the ability to take control of it" (and aiIdle).
	bUnmanned = true;
	ClearAttack();
	StopEntering();
	StopConstructing();
	CommandStop();
	// Object::setDisabled: "We've been sniped! Play a splatter sound for the pilot losing his face."
	Logic->PlaySoundAt(FGenAudioData::Get().GetMisc(TEXT("SplatterVehiclePilotsBrain")), *this);
	// TransportContain::onCapture: "If this vehicle was sniped, then instantly eject everyone (otherwise, the
	// next guy to eject will recapture it)."
	if (Contain)
	{
		const TArray<FGenObjectID> Riders = Logic->GetRiders(*this);
		for (FGenObjectID Id : Riders)
		{
			if (FGenObject* R = Logic->FindObject(Id))
			{
				FGenObject* In = Logic->FindObject(R->GetContainedBy());
				Logic->RemoveFromContain(In ? *In : *this, *R);
			}
		}
	}
	// Object::setDisabled: "strange but true: If I am a carbomb, my driver actually has a dead-man's trigger
	// for my dynamite... If he gets sniped, I blow up! Wheeee!"
	if (bCarBomb && Weapons[0].Template)
	{
		Logic->DealDamage(*Weapons[0].Template, ID, GenInvalidObjectID, Position, false);
		if (bDead)
		{
			return;
		}
	}
	// "Convert it to the neutral team so it renders gray giving visual representation that it is unmanned."
	const int32 Neutral = Logic->GetNeutralPlayer();
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [pilot f%u] #%u %s of p%d loses its driver to #%u"), Logic->GetFrame(), ID, *Template->Name, Owner, Sniper);
	if (Neutral != INDEX_NONE)
	{
		SetOwner(Neutral);
	}
}

void FGenObject::ClaimUnmanned(FGenObject& NewDriver)
{
	// "Before we return, check for a very special case of an infantry colliding with an unmanned vehicle.
	// If this is the case, it'll become its new pilot!" - the vehicle defects to the soldier's side and
	// the soldier is gone (he is the driver now).
	if (!bUnmanned || NewDriver.IsDead())
	{
		return;
	}
	bUnmanned = false;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [pilot f%u] #%u %s climbs into the empty #%u %s: p%d's now"), Logic->GetFrame(), NewDriver.ID,
		*NewDriver.Template->Name, ID, *Template->Name, NewDriver.Owner);
	SetOwner(NewDriver.Owner);
	NewDriver.bPendingDestroy = true;
}

bool FGenObject::CanPilotEnter(const FGenObject& Vehicle) const
{
	if (!Template->bPilot || &Vehicle == this || bDead || Vehicle.bDead || Vehicle.bPendingDestroy)
	{
		return false;
	}
	// CrateCollide::isValidToExecute: all of the kinds asked for, none of those forbidden.
	const FGenThingTemplate& VT = *Vehicle.Template;
	for (const FString& Kind : Template->PilotRequiredKindOf)
	{
		if (!VT.IsKindOf(*Kind)) { return false; }
	}
	for (const FString& Kind : Template->PilotForbiddenKindOf)
	{
		if (VT.IsKindOf(*Kind)) { return false; }
	}
	//	"Note that we only want to do this if the plane is on the ground."
	if (Vehicle.Position.Z - Logic->GetGroundHeight(Vehicle.Position.X, Vehicle.Position.Y) > AirborneThreshold)
	{
		return false;
	}
	const int32 Levels = Template->bPilotAddsOwnVeterancy ? VeterancyLevel : 1;
	if (Levels <= 0 || !VT.bIsTrainable || Vehicle.VeterancyLevel >= 3)
	{
		return false; // "If the other unit can't gain experience, then we can't help promote it!"
	}
	// "This is a pilot and we are checking to make sure the pilot is entering a vehicle on the same team."
	// isUsingAirLoco() is the locomotor in use, not the one in the data: an aircraft that has parked at its
	// airfield has chosen its taxi locomotor, so a pilot can climb into it there. bFlies comes from the data
	// and stays true on the ground, so it is not what the original checks here.
	return Vehicle.Owner == Owner && !Vehicle.IsUsingAirLoco();
}

void FGenObject::PilotEnterVehicle(FGenObject& Vehicle)
{
	// VeterancyCrateCollide::executeCrateBehavior with ExperienceTracker::gainExpForLevel: the vehicle
	// gains as many ranks as the pilot holds, and the pilot is gone into it.
	const int32 Levels = Template->bPilotAddsOwnVeterancy ? VeterancyLevel : 1;
	const int32 NewLevel = FMath::Min(3, Vehicle.VeterancyLevel + Levels);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [pilot f%u] #%u %s (rank %d) climbs into #%u %s: rank %d -> %d"), Logic->GetFrame(), ID, *Template->Name,
		VeterancyLevel, Vehicle.ID, *Vehicle.Template->Name, Vehicle.VeterancyLevel, NewLevel);
	if (NewLevel > Vehicle.VeterancyLevel)
	{
		Vehicle.AddExperience(Vehicle.Template->ExperienceRequired[NewLevel] - Vehicle.Experience, false);
	}
	StopEntering();
	CommandStop();
	bPendingDestroy = true;
}

void FGenObject::UpdatePilotFindVehicle(uint32 Frame)
{
	// PilotFindVehicleUpdate::update: "This is an ai only behavior."
	PilotNextScanFrame = Frame + uint32(Template->PilotScanFrames);
	const FGenSkirmishAI* AI = Logic->GetAI(Owner);
	if (!AI || !IsAIIdle() || EnterTarget != GenInvalidObjectID)
	{
		return;
	}
	// scanClosestTarget: the nearest vehicle of ours in fair shape that would take him.
	FGenObject* Best = nullptr;
	float BestDistSq = FMath::Square(Template->PilotScanRange);
	const FVector2f Here(Position.X, Position.Y);
	Logic->GetPartition().ForEachInRange(Here, Template->PilotScanRange, [&](FGenObject& Other)
	{
		if (Other.Owner != Owner || !Other.IsKindOf(TEXT("VEHICLE")) || Other.Health < Other.MaxHealth * Template->PilotMinHealth || !CanPilotEnter(Other))
		{
			return;
		}
		const float DistSq = FVector2f::DistSquared(Here, FVector2f(Other.Position.X, Other.Position.Y));
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = &Other;
		}
	});
	if (Best)
	{
		CommandEnter(*Best);
		bPilotMovedToBase = false;
		return;
	}
	// "Try moving to base 1 time."
	FVector3f Base;
	if (!bPilotMovedToBase && AI->GetBaseCenter(Base))
	{
		CommandMoveTo(Base);
		bPilotMovedToBase = true;
	}
}
