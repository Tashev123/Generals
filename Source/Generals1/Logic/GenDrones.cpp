// Drones: SlavedUpdate - a unit that keeps to the one that bought it.
//
// Four times a second the drone looks at what its master is doing, and does the first of these that
// applies: mends the master when its health is below RepairWhenBelowHealth% (a battle drone); flies to
// the master's target, as near as AttackRange from the master allows (a scout drone that gets within
// DistToTargetToGrantRangeBonus of the target lets its master shoot further: DRONE_SPOTTING); flies
// ahead to where the master is going, as far as ScoutRange allows; mends the master of what little
// harm it has; or keeps within GuardMaxRange of it, drifting from spot to spot. When the master dies
// the drone is "unmanned": it drops out of the sky.
//
// Mending: over the master (RepairMin/MaxAltitude up) the arm unpacks, and then turn by turn it
// waits (ready), reaches out, welds - sparks, and the master's health goes up by RepairRatePerSecond -
// draws back and zips to another spot RepairRange away on its fast locomotor.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Audio/GenAudioData.h"
#include "W3D/GenAssets.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

using FGenSlavedUpdate = FGenThingTemplate::FSlaved;

namespace
{
	// RepairStates
	enum : uint8 { RepairNone, RepairUnpacking, RepairPacking, RepairReady, RepairExtending, RepairWelding, RepairRetracting };

	constexpr float StrayMultiplier = 2.f;   // STRAY_MULTIPLIER: this many times the guard range from the master is too far whatever it is doing
	constexpr float CloseEnough = 15.f;      // CLOSE_ENOUGH: "our moveTo commands can't handle people in the way, so quit trying"
	constexpr float RepairCloseEnough = 12.f;

	const FString DroneSpotting(TEXT("DRONE_SPOTTING"));

	/** SLAVED_UPDATE_RATE: "a low priority module" - four times a second. */
	int32 SlavedUpdateRate() { return FMath::Max(1, GenLogic::GetLogicFPS() / 4); }
	/** A count of the original's 30 Hz frames, in ours. */
	int32 Frames30(int32 N) { return FMath::Max(1, N * GenLogic::GetLogicFPS() / 30); }

	/** FROM_BOUNDINGSPHERE_2D, from an object to a point. */
	float GapToPoint(const FGenObject& Obj, const FVector3f& P)
	{
		return FMath::Max(0.f, FVector2f::Distance(FVector2f(Obj.GetPosition().X, Obj.GetPosition().Y), FVector2f(P.X, P.Y)) - Obj.GetTemplate()->GetBoundingSphereRadius());
	}
}

void FGenObject::Enslave(const FGenObject& Slaver)
{
	// SlavedUpdate::onEnslave / startSlavedEffects: "decide where our pinned stray point is".
	const FGenSlavedUpdate& D = Template->Slaved;
	Slave = MakeUnique<FGenSlaveState>();
	Slave->Slaver = Slaver.ID;
	const float Direction = Logic->GetRandom().FRandRange(0.f, 2.f * PI);
	Slave->GuardOffset = FVector3f(float(D.GuardMaxRange) * FMath::Cos(Direction), float(D.GuardMaxRange) * FMath::Sin(Direction), 0.f);
	if (D.RepairRatePerSecond > 0.f)
	{
		static const int32 Packing = GenConditions::Index(TEXT("PACKING"));
		SetModelCondition(Packing, true); // onObjectCreated: "if this object can repair, pack it up at init"
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drone f%u] #%u %s keeps to #%u %s"), Logic->GetFrame(), ID, *Template->Name, Slaver.ID, *Slaver.Template->Name);
}

void FGenObject::SetSlaveRepairCondition(int32 Bit)
{
	// SlavedUpdate::setRepairModelConditionStates
	static const int32 Bits[] = { GenConditions::Index(TEXT("PACKING")), GenConditions::Index(TEXT("UNPACKING")), GenConditions::Index(TEXT("FIRING_B")),
		GenConditions::Index(TEXT("FIRING_C")), GenConditions::Index(TEXT("BETWEEN_FIRING_SHOTS_B")), GenConditions::Index(TEXT("BETWEEN_FIRING_SHOTS_C")),
		GenConditions::Index(TEXT("RELOADING_B")), GenConditions::Index(TEXT("RELOADING_C")) };
	for (int32 B : Bits)
	{
		SetModelCondition(B, B == Bit);
	}
}

void FGenObject::SlaveEndRepair()
{
	// SlavedUpdate::endRepair
	FGenSlaveState& S = *Slave;
	if (S.RepairState != RepairNone)
	{
		static const int32 Packing = GenConditions::Index(TEXT("PACKING"));
		S.RepairState = RepairNone;
		S.FramesToWait = SlavedUpdateRate();
		S.bRepairing = false;
		SetSlaveRepairCondition(Packing); // "to force the arm to fully retract"
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drone f%u] #%u %s stops mending"), Logic->GetFrame(), ID, *Template->Name);
	}
	if (S.bFastLoco)
	{
		S.bFastLoco = false;
		ChooseLocomotorSet(TEXT("SET_NORMAL"));
	}
	bLocoUltraAccurate = false;
	bLocoPreciseZ = false;
}

void FGenObject::SlaveMoveNear(const FGenObject& Master, const FVector3f& Spot, int32 Range, int32 Wander, float DistToSpot)
{
	// SlavedUpdate::doAttackLogic / doScoutLogic: to the spot itself, or as near to it as it may get from
	// the master - and from there a wander's length in some direction.
	FVector3f To = Spot;
	if (DistToSpot > float(Range))
	{
		FVector3f Dir = Spot - Master.Position;
		To = Master.Position + Dir.GetSafeNormal() * float(Range);
	}
	FGenSlaveState& S = *Slave;
	if (Wander != 0)
	{
		const float Direction = Logic->GetRandom().FRandRange(0.f, 2.f * PI);
		S.GuardOffset = FVector3f(float(Wander) * FMath::Cos(Direction), float(Wander) * FMath::Sin(Direction), 0.f);
		To.X += S.GuardOffset.X;
		To.Y += S.GuardOffset.Y;
	}
	To.Z = Logic->GetGroundHeight(To.X, To.Y);
	CommandMoveTo(To);
}

void FGenObject::SlaveGuard(FVector3f Pinned)
{
	// SlavedUpdate::doGuardLogic: "recalc where we want to be if we wander around" (the new offset comes
	// on top of the old one, which Pinned has in it already).
	const FGenSlavedUpdate& D = Template->Slaved;
	if (D.GuardWanderRange != 0)
	{
		const float Direction = Logic->GetRandom().FRandRange(0.f, 2.f * PI);
		Slave->GuardOffset = FVector3f(float(D.GuardMaxRange) * FMath::Cos(Direction), float(D.GuardMaxRange) * FMath::Sin(Direction), 0.f);
		Pinned.X += Slave->GuardOffset.X;
		Pinned.Y += Slave->GuardOffset.Y;
	}
	Pinned.Z = Logic->GetGroundHeight(Pinned.X, Pinned.Y);
	CommandMoveTo(Pinned);
}

void FGenObject::SetSlaveRepairState(uint8 State, const FGenObject& Master)
{
	// SlavedUpdate::setRepairState
	static const int32 Unpacking = GenConditions::Index(TEXT("UNPACKING"));
	static const int32 FiringB = GenConditions::Index(TEXT("FIRING_B"));
	static const int32 FiringC = GenConditions::Index(TEXT("FIRING_C"));
	FGenSlaveState& S = *Slave;
	const FGenSlavedUpdate& D = Template->Slaved;
	FRandomStream& Random = Logic->GetRandom();
	if (State == S.RepairState)
	{
		return;
	}
	if (State == RepairReady)
	{
		if (S.RepairState == RepairNone)
		{
			// "We're not in a repair state - so assume we need to unpack."
			SetSlaveRepairCondition(Unpacking);
			S.RepairState = RepairUnpacking;
			S.FramesToWait = Frames30(15);
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drone f%u] #%u %s starts mending #%u (%.0f / %.0f)"), Logic->GetFrame(), ID, *Template->Name,
				Master.ID, Master.Health, Master.MaxHealth);
		}
		else if (S.RepairState == RepairWelding)
		{
			// "We are welding so retract before going into ready state" - and off to another spot.
			S.RepairState = RepairRetracting;
			S.FramesToWait = Frames30(5);
			SetSlaveRepairCondition(FiringC);
			if (D.RepairRange != 0)
			{
				// SlavedUpdate::moveToNewRepairSpot
				const float Direction = Random.FRandRange(0.f, 2.f * PI);
				FVector3f Spot = Master.Position;
				Spot.X += float(D.RepairRange) * FMath::Cos(Direction);
				Spot.Y += float(D.RepairRange) * FMath::Sin(Direction);
				Spot.Z = Logic->GetGroundHeight(Spot.X, Spot.Y) + Random.FRandRange(D.RepairMinAltitude, FMath::Max(D.RepairMinAltitude, D.RepairMaxAltitude));
				if (!S.bFastLoco)
				{
					S.bFastLoco = true;
					ChooseLocomotorSet(TEXT("SET_PANIC")); // "used for repairing master (zippy)"
				}
				bLocoUltraAccurate = true;
				bLocoPreciseZ = true;
				PreciseGoalZ = Spot.Z;
				CommandMoveTo(Spot);
			}
		}
		else
		{
			S.RepairState = RepairReady;
			S.FramesToWait = Random.RandRange(D.MinReadyFrames, FMath::Max(D.MinReadyFrames, D.MaxReadyFrames));
		}
	}
	else if (State == RepairWelding)
	{
		if (S.RepairState == RepairReady)
		{
			S.RepairState = RepairExtending;
			S.FramesToWait = Frames30(5);
			SetSlaveRepairCondition(FiringB);
		}
		else
		{
			S.RepairState = RepairWelding;
			S.FramesToWait = Random.RandRange(D.MinWeldFrames, FMath::Max(D.MinWeldFrames, D.MaxWeldFrames));
			// "Make sparks!"
			FVector3f At = Position;
			UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
			const FGenDrawModule* Draw = Template->PrimaryDraw();
			TArray<FTransform3f> Bone;
			if (Assets && Draw && !D.WeldingFXBone.IsEmpty() && Assets->GetPristineBones(Draw->DefaultModel(), D.WeldingFXBone, 0, 1, Bone) > 0)
			{
				At += Bone[0].GetTranslation(); // (the bone's place in the model, not turned with it - as the original has it)
			}
			if (!D.WeldingSys.IsEmpty())
			{
				Logic->PlayParticleSystemAt(D.WeldingSys, At);
			}
			Logic->PlaySoundAt(FGenAudioData::Get().GetMisc(TEXT("RepairSparks")), *this);
			S.bRepairing = true; // "don't actually start healing until we fire our first sparks"
		}
	}
}

void FGenObject::SlaveRepair(FGenObject& Master)
{
	// SlavedUpdate::doRepairLogic: "there are two major things... either move closer or repair".
	FGenSlaveState& S = *Slave;
	const FGenSlavedUpdate& D = Template->Slaved;
	const float Gap = FMath::Max(0.f, FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Master.Position.X, Master.Position.Y))
		- Template->GetBoundingSphereRadius() - Master.Template->GetBoundingSphereRadius());
	const bool bCloseEnough = Gap < RepairCloseEnough;
	if (bCloseEnough)
	{
		switch (S.RepairState)
		{
		case RepairNone:
			SetSlaveRepairState(RepairReady, Master);
			break;
		case RepairReady:
		case RepairExtending:
			if (S.FramesToWait == 0) { SetSlaveRepairState(RepairWelding, Master); }
			break;
		default: // unpacking, welding, retracting
			if (S.FramesToWait == 0) { SetSlaveRepairState(RepairReady, Master); }
			break;
		}
	}
	else
	{
		// "We're too far away to repair, so get closer."
		S.bRepairing = false;
		FVector3f To = Master.Position;
		To.Z += Logic->GetRandom().FRandRange(D.RepairMinAltitude, FMath::Max(D.RepairMinAltitude, D.RepairMaxAltitude));
		bLocoPreciseZ = Gap < Master.Template->GetBoundingSphereRadius() * 2.f; // closeEnoughForZPrecision
		PreciseGoalZ = To.Z;
		CommandMoveTo(To);
		if (S.FramesToWait == 0)
		{
			SetSlaveRepairState(RepairReady, Master); // "retracting the repair arm, so viewers will see its intentions"
		}
	}
	if (bCloseEnough && S.bRepairing)
	{
		Master.AttemptHealing(D.RepairRatePerSecond / float(GenLogic::GetLogicFPS()));
	}
}

void FGenObject::UpdateSlaved(uint32 Frame)
{
	// SlavedUpdate::update
	GEN_PERF_SCOPE("Obj.Slaved");
	FGenSlaveState& S = *Slave;
	const FGenSlavedUpdate& D = Template->Slaved;
	if (S.CrashFrame != 0)
	{
		if (Frame >= S.CrashFrame && !bDead)
		{
			Kill(); // (down by now; if something held it up, this is its end all the same)
		}
		return;
	}
	if (S.FramesToWait > 0)
	{
		--S.FramesToWait;
	}
	if (S.RepairState == RepairNone)
	{
		if (S.FramesToWait > 0)
		{
			return;
		}
		S.FramesToWait = SlavedUpdateRate();
	}
	if (S.Slaver == GenInvalidObjectID || !Locomotor)
	{
		return;
	}
	FGenObject* Master = Logic->FindObject(S.Slaver);
	if (!Master || Master->bDead || Master->bPendingDestroy)
	{
		// "Killing is lame. Let's disable the drone so it crashes instead!" (DISABLED_UNMANNED; "added
		// special case code in physics falling to ensure death".)
		SlaveEndRepair();
		S.Slaver = GenInvalidObjectID;
		ClearAttack();
		CommandStop();
		DisableUntil(MAX_uint32);
		LocoMaxLift = 0.f;
		S.CrashFrame = Frame + uint32(4 * GenLogic::GetLogicFPS());
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drone f%u] #%u %s has lost its master and falls"), Frame, ID, *Template->Name);
		return;
	}
	if (Owner != Master->Owner && Logic->GetRelationship(Owner, Master->Owner) != EGenRelationship::Allies)
	{
		SetOwner(Master->Owner); // "slaver must have been hijacked or something: we will join his team"
	}
	if (D.bStayOnSameLayerAsMaster)
	{
		Layer = Master->Layer;
	}

	// "Clear the drone spotting bonus to the master. Up to the drone to satisfy the conditions to set it
	// again for the next update."
	if (Master->BonusConditions.Num() > 0)
	{
		Master->BonusConditions.Remove(DroneSpotting);
	}
	const FGenObject* Target = Master->TargetID != GenInvalidObjectID ? Logic->FindObject(Master->TargetID) : nullptr;
	if (Target && (Target->bDead || Target->bPendingDestroy))
	{
		Target = nullptr;
	}

	// ("Health percentage will always be 100 should the drone be incapable of repairing.")
	const int32 HealthPercent = D.RepairRatePerSecond > 0.f ? int32(Master->Health / FMath::Max(Master->MaxHealth, 1.f) * 100.f) : 100;
	if (HealthPercent <= D.RepairWhenBelowPercent)
	{
		SlaveRepair(*Master); // first: the master needs it
		return;
	}
	if (D.AttackRange != 0 && Target)
	{
		// Second: to the master's victim.
		SlaveEndRepair();
		const float Dist = GapToPoint(*this, Target->Position);
		SlaveMoveNear(*Master, Target->Position, D.AttackRange, D.AttackWanderRange, Dist);
		if (Dist < float(D.DistToTargetToGrantRangeBonus))
		{
			Master->BonusConditions.AddUnique(DroneSpotting); // "we are close enough to the target: grant our master extended weapon range"
		}
		return;
	}
	if (D.ScoutRange != 0 && Master->bMoving)
	{
		// Third: ahead to where the master is going, unless it is nearly there.
		const FVector3f Dest = Master->RequestedGoal;
		if (GapToPoint(*Master, Dest) > float(D.GuardMaxRange) * 0.5f)
		{
			SlaveEndRepair();
			SlaveMoveNear(*Master, Dest, D.ScoutRange, D.ScoutWanderRange, GapToPoint(*this, Dest));
			return;
		}
	}
	if (HealthPercent < 100)
	{
		SlaveRepair(*Master); // "because we're idle"
		return;
	}
	if (D.GuardMaxRange != 0)
	{
		// Last: keep to the master. ("Our pinned location, based on where our master is.")
		const FVector3f Pinned(Master->Position.X + S.GuardOffset.X, Master->Position.Y + S.GuardOffset.Y, Master->Position.Z);
		const bool bIdle = !bMoving && TargetID == GenInvalidObjectID;
		if ((bIdle && FVector3f::DistSquared(Position, Pinned) > CloseEnough * CloseEnough)
			|| FVector3f::DistSquared(Position, Master->Position) > FMath::Square(StrayMultiplier * float(D.GuardMaxRange)))
		{
			SlaveEndRepair();
			SlaveGuard(Pinned);
		}
	}
}
