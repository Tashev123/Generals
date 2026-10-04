#include "Logic/GenObject.h"
#include "Client/GenFXData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPathfinder.h"

namespace
{
	constexpr float MaxBrakingFactor = 5.f;           // Locomotor.cpp MAX_BRAKING_FACTOR
	constexpr float CellSize = GenLogic::PathfindCellSize;
	enum { TurnNone = 0, TurnPositive = 1, TurnNegative = -1 };

	FORCEINLINE float Sqr(float V) { return V * V; }
}

FGenObject::FGenObject(FGenObjectID InID, const FGenThingTemplate* InTemplate, int32 InOwner, UGenGameLogic* InLogic)
	: ID(InID), Template(InTemplate), Owner(InOwner), Logic(InLogic)
{
	MaxHealth = Template->MaxHealth > 0.f ? Template->MaxHealth : 1.f;
	Health = Template->InitialHealth > 0.f ? Template->InitialHealth : MaxHealth;
	bHasAI = Template->bHasAI;
	bImmobile = Template->IsKindOf(TEXT("IMMOBILE")) || Template->IsKindOf(TEXT("STRUCTURE"));
	Mass = FMath::Max(0.01f, Template->Mass);
	// The default frictions are per-frame values at 30 fps; rescale for the configured rate.
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS());
	ForwardFriction = Template->bHasPhysics ? Template->ForwardFriction : 0.15f * RateScale;
	LateralFriction = Template->bHasPhysics ? Template->LateralFriction : 0.15f * RateScale;

	if (const TArray<FString>* Set = Template->LocomotorSets.Find(TEXT("SET_NORMAL")))
	{
		for (const FString& Name : *Set)
		{
			if (const FGenLocomotorTemplate* L = FGenGameData::Get().FindLocomotor(Name))
			{
				Locomotor = L;
				break;
			}
		}
	}
	bFlies = Locomotor && Locomotor->IsAirOnly();
	InitJet();
	bSpawnPending = !Template->SpawnTemplate.IsEmpty() && Template->SpawnNumber > 0;
	InitSupply();
	InitContain();
	bAllowBounce = Template->bAllowBouncing;
	if (Template->bHasLifetime)
	{
		// LifetimeUpdate: dies after a random time in [Min, Max], at least one frame from now.
		const int32 Delay = Logic->GetRandom().RandRange(Template->MinLifetime, FMath::Max(Template->MinLifetime, Template->MaxLifetime));
		LifetimeDieFrame = Logic->GetFrame() + uint32(FMath::Max(1, Delay));
	}
	InitCombat();
	InitUpgrades();
	InitPayload();
	InitVeterancy();
}

bool FGenObject::IsDisabled() const
{
	return Logic->GetFrame() < DisabledUntilFrame;
}

void FGenObject::SetRotation(const FQuat4f& Q)
{
	Rotation = Q.GetNormalized();
	Orientation = GetYawFromRotation();
}

float FGenObject::GetYawFromRotation() const
{
	// Matrix3D::Get_Z_Rotation: heading of the local X axis.
	const FVector3f X = Rotation.GetAxisX();
	return (FMath::Abs(X.X) < 1e-5f && FMath::Abs(X.Y) < 1e-5f) ? Orientation : FMath::Atan2(X.Y, X.X);
}

bool FGenObject::IsKindOf(const TCHAR* Kind) const
{
	return Template->IsKindOf(Kind);
}

bool FGenObject::IsSelectable() const
{
	return !bDead && !bPendingDestroy && ContainedBy == GenInvalidObjectID && Template->IsKindOf(TEXT("SELECTABLE"));
}

// ---- orders -------------------------------------------------------------------------------------

void FGenObject::CommandMoveTo(const FVector3f& InGoal)
{
	if (!CanMove() || bDead)
	{
		return;
	}
	if (bJetAI)
	{
		// A parked aircraft takes off first; one that is taking off or landing remembers the order.
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::Move;
		Cmd.Pos = InGoal;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	MoveOutOfWayOf = GenInvalidObjectID;
	StartPath(InGoal, false);
}

void FGenObject::StartDirectMove(const FVector3f& InGoal)
{
	Path.Reset();
	Path.Add(FVector2f(InGoal.X, InGoal.Y));
	PathIndex = 0;
	PathStart = FVector2f(Position.X, Position.Y);
	Goal = FVector3f(InGoal.X, InGoal.Y, Logic->GetGroundHeight(InGoal.X, InGoal.Y));
	RequestedGoal = InGoal;
	if (!bFlies)
	{
		Logic->GetPathfinder().UpdateUnitGoal(ID, Path[0], Template->GetBoundingCircleRadius());
	}
	bMoving = true;
	StuckFrames = 0;
	BlockedFrames = 0;
	bBlocked = false;
	bBlockedAndStuck = false;
	bSidestepPath = false;
	bLocoBraking = false;
	BrakingFactor = 1.f;
}

bool FGenObject::StartPath(const FVector3f& InGoal, bool bAroundUnits, const TFunction<bool(const FVector2f&)>* Accept)
{
	if (bFlies)
	{
		// Aircraft need no path: they fly straight there (Pathfinder::findPath for an AIR-only locomotor set),
		// ending where no other aircraft hovers or is headed.
		FVector2f Spot(InGoal.X, InGoal.Y);
		Logic->AdjustAirDestination(*this, Spot);
		StartDirectMove(FVector3f(Spot.X, Spot.Y, InGoal.Z));
		RequestedGoal = InGoal;
		return true;
	}
	FGenPathfinder& PF = Logic->GetPathfinder();
	const float Footprint = Template->GetBoundingCircleRadius();
	const FVector2f From(Position.X, Position.Y);

	// Pathfinder::adjustDestination: end the move on ground no other unit has claimed or is parked on.
	FVector2f Dest(InGoal.X, InGoal.Y);
	if (!Accept || !PF.AdjustUnitDestination(ID, Footprint, Locomotor->Surfaces, Dest, *Accept))
	{
		Dest = FVector2f(InGoal.X, InGoal.Y);
		PF.AdjustUnitDestination(ID, Footprint, Locomotor->Surfaces, Dest);
	}

	TArray<FVector2f> NewPath;
	if (bAroundUnits)
	{
		PF.FindPathAroundUnits(ID, Footprint, From, Dest, Locomotor->Surfaces, NewPath);
	}
	if (NewPath.Num() == 0)
	{
		PF.FindPath(From, Dest, Locomotor->Surfaces, NewPath);
	}
	if (NewPath.Num() == 0)
	{
		return false;
	}
	Path = MoveTemp(NewPath);
	PathIndex = 0;
	PathStart = From;
	const FVector2f End = Path.Last();
	Goal = FVector3f(End.X, End.Y, Logic->GetGroundHeight(End.X, End.Y));
	RequestedGoal = InGoal;
	PF.UpdateUnitGoal(ID, End, Footprint);
	bMoving = true;
	StuckFrames = 0;
	BlockedFrames = 0;
	bBlocked = false;
	bBlockedAndStuck = false;
	bSidestepPath = false;
	// Locomotor::startMove
	bLocoBraking = false;
	BrakingFactor = 1.f;
	return true;
}

void FGenObject::CommandStop()
{
	bMoving = false;
	Path.Reset();
	bLocoBraking = false;
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	if (Locomotor && !bDead && !bFlies)
	{
		// Where we stand is now our destination.
		Logic->GetPathfinder().UpdateUnitGoal(ID, FVector2f(Position.X, Position.Y), Template->GetBoundingCircleRadius());
	}
}

// ---- update -------------------------------------------------------------------------------------

void FGenObject::Update(uint32 Frame)
{
	if (bPendingDestroy)
	{
		return;
	}
	if (Projectile.bActive)
	{
		UpdateProjectile(Frame);
		return;
	}
	if (bDead)
	{
		UpdateDying(Frame);
		return;
	}
	if (bHeldByParachute)
	{
		return; // hanging under a parachute: the parachute moves us
	}
	if (ContainedBy != GenInvalidObjectID)
	{
		UpdateContained(Frame); // rides along; shoots out if the container lets it
		return;
	}
	if (LifetimeDieFrame != 0 && Frame >= LifetimeDieFrame)
	{
		Kill();
		return;
	}
	if (SellFrame != 0)
	{
		// BuildAssistant sell list: the building is taken down, then its owner is refunded.
		if (Frame >= SellFrame)
		{
			Logic->FinishSell(*this);
			bPendingDestroy = true;
		}
		return;
	}
	if (bUnderConstruction)
	{
		// A construction site does nothing by itself; without a dozer at work it just waits.
		static const int32 ActivelyBeingConstructed = GenConditions::Index(TEXT("ACTIVELY_BEING_CONSTRUCTED"));
		SetModelCondition(ActivelyBeingConstructed, Frame <= LastBuiltFrame + 1);
		return;
	}
	if (bSpawnPending)
	{
		// SpawnBehavior: a finished building's own units come out (a supply center's first gatherer).
		bSpawnPending = false;
		Logic->SpawnBuildingUnits(*this);
	}
	if (Contain)
	{
		Logic->UpdateContain(*this);
	}
	if (bAutoHealActive)
	{
		UpdateAutoHeal(Frame);
	}
	if (AreaWeapons.Num() > 0)
	{
		UpdateAreaWeapons(Frame);
	}
	if ((Template->bHasDeletion || Template->bHeightDie || bFalling || ParachuteRider != GenInvalidObjectID) && !UpdatePayload(Frame))
	{
		return; // deleted, gone off, or a parachute on its way down
	}
	if (Frame < DisabledUntilFrame)
	{
		// EMP: nothing works. Whatever was rolling comes to rest.
		if (CanMove())
		{
			bMoving = false;
			Path.Reset();
			if (IsUsingAirLoco()) { UpdateFlightPhysics(Frame); } else { UpdatePhysics(Frame); }
			SetModelCondition(GenConditions::Moving(), false);
		}
		return;
	}
	if (Dock)
	{
		UpdateDock(Frame);
	}
	if (Parking)
	{
		Parking->Update(Frame);
	}
	if (Production.Num() > 0)
	{
		UpdateProduction(Frame);
	}
	bJetDriving = false;
	if (bJetAI)
	{
		UpdateJet(Frame); // taxiing, takeoff, landing, reloading; may take over this frame's movement
		if (bDead)
		{
			return; // ran out of fuel with nowhere to land
		}
	}
	if (Delivery && !UpdateDelivery(Frame))
	{
		return; // a power's aircraft that hasn't started yet, or has just left
	}
	if (AIMode != EGenAIMode::None)
	{
		UpdateAIMode(Frame);
	}
	if (HasWeapons())
	{
		UpdateCombat(Frame);
	}
	if (CanMove())
	{
		if (Template->bChinookAI && Contain)
		{
			UpdateChinookFlight(Frame);
		}
		if (EnterTarget != GenInvalidObjectID)
		{
			UpdateEnter(Frame);
		}
		if (bHasExitRally && !bMoving)
		{
			// Out of the factory door: now on to the rally point.
			bHasExitRally = false;
			CommandMoveTo(ExitRally);
		}
		if (BuildTarget != GenInvalidObjectID || RepairTarget != GenInvalidObjectID)
		{
			UpdateDozer(Frame);
		}
		else if (Template->bIsDozer)
		{
			UpdateDozerBored(Frame);
		}
		if (Template->bIsSupplyTruck)
		{
			UpdateSupplyTruck(Frame);
		}
		if (DockTarget != GenInvalidObjectID)
		{
			UpdateDocking(Frame);
		}
		if (!bJetDriving)
		{
			UpdateAI(Frame);
		}
		// (An aircraft on the ground at its airfield rolls on its taxi locomotor, like a ground unit.)
		if (IsUsingAirLoco())
		{
			UpdateFlightPhysics(Frame);
		}
		else
		{
			UpdatePhysics(Frame);
		}
		// A unit held up behind another one stops its move animation after a quarter second.
		SetModelCondition(GenConditions::Moving(), bMoving && BlockedFrames <= uint32(GenLogic::GetLogicFPS() / 4));
	}
	else if (bFreePhysics)
	{
		UpdateFreePhysics(Frame);
	}
}

void FGenObject::SetModelCondition(int32 Bit, bool bOn)
{
	if (ModelConditions.Test(Bit) != bOn)
	{
		ModelConditions.Set(Bit, bOn);
		++ConditionVersion;
	}
}

float FGenObject::PathDistanceFrom(int32 Index) const
{
	float D = 0.f;
	FVector2f Prev(Position.X, Position.Y);
	for (int32 i = Index; i < Path.Num(); ++i)
	{
		D += FVector2f::Distance(Prev, Path[i]);
		Prev = Path[i];
	}
	return D;
}

void FGenObject::UpdateAI(uint32 Frame)
{
	// AIUpdateInterface::doLocomotor: collisions found after the last frame say whether we are blocked,
	// and how fast we may go without running into the unit ahead.
	BlockedFrames = bBlocked ? BlockedFrames + 1 : 0;
	bBlocked = false;
	const float MaxBlockedSpeed = CurMaxBlockedSpeed;
	CurMaxBlockedSpeed = 99999.f;
	const bool bStuck = bBlockedAndStuck;
	bBlockedAndStuck = false;

	if (!bMoving || Path.Num() == 0)
	{
		if (bFlies)
		{
			MaintainPosition(Frame); // hovering takes work even when going nowhere
		}
		return;
	}

	// AIInternalMoveToState: blocked by a unit that isn't going anywhere, or blocked for two seconds:
	// find a new path that goes around the parked units.
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	if ((bStuck || BlockedFrames > 2 * FPS) && Frame >= NextBlockedRepathFrame)
	{
		NextBlockedRepathFrame = Frame + FPS;
		const FVector3f Dest = RequestedGoal;
		const uint32 KeepStuckFrames = StuckFrames, KeepBlockedFrames = BlockedFrames;
		if (StartPath(Dest, true))
		{
			// Re-pathing is not progress, and we are still being held up: keep both counters running
			// (after a second of this, units that merely cross each other are let through).
			StuckFrames = KeepStuckFrames;
			BlockedFrames = KeepBlockedFrames;
			bSidestepPath = true;
		}
	}
	const FVector2f Pos2(Position.X, Position.Y);

	// Advance through intermediate waypoints once we're close (the original projects ahead on the path).
	// The first waypoint of a path around a parked unit is the sidestep itself: really go there.
	while (PathIndex < Path.Num() - 1
		&& FVector2f::Distance(Pos2, Path[PathIndex]) < CellSize * ((bSidestepPath && PathIndex == 0) ? 0.5f : 1.5f))
	{
		++PathIndex;
	}

	// A plane measures what is left along its line of flight: it cannot stop on the goal, it flies past it.
	const bool bPlane = HasMinSpeed() && IsUsingAirLoco();
	const float DistToGoal = bPlane ? FlightDistToGoal() : PathDistanceFrom(PathIndex);
	const FVector2f Target = Path[PathIndex];
	const float CloseEnough = FMath::Max(Locomotor->CloseEnoughDist, 1.f);

	// Arrived: AIUpdate's move state ends when within CloseEnoughDist and stopped (or nearly).
	if (PathIndex == Path.Num() - 1 && DistToGoal <= CloseEnough)
	{
		bMoving = false;
		Path.Reset();
		if (bPlane)
		{
			return; // keeps flying; with nowhere to go it circles here
		}
		Velocity.X = Velocity.Y = 0.f;
		bStatusBraking = false;
		if (bMovingOutOfWay)
		{
			// Stepped aside: carry on with what we were doing before.
			bMovingOutOfWay = false;
			MoveOutOfWayOf = GenInvalidObjectID;
			if (bHasResumeGoal)
			{
				bHasResumeGoal = false;
				const FVector3f Resume = ResumeGoal;
				StartPath(Resume, false);
			}
		}
		return;
	}

	const FVector3f TargetPos(Target.X, Target.Y, Logic->GetGroundHeight(Target.X, Target.Y));
	const float DesiredSpeed = BlockedFrames > 0 ? FMath::Min(Locomotor->Speed, MaxBlockedSpeed) : Locomotor->Speed;
	LocoMoveTowardsPosition(TargetPos, DistToGoal, DesiredSpeed, Frame);

	// Give up if we haven't made progress for a long time. Waiting behind another unit is normal, so a
	// blocked unit is given much longer (it keeps trying to path around in the meantime).
	if (Velocity.SizeSquared() < Sqr(0.01f))
	{
		if (++StuckFrames > FPS * (BlockedFrames > 0 ? 12u : 4u))
		{
			CommandStop();
		}
	}
	else
	{
		StuckFrames = 0;
	}
}

// ---- Locomotor ----------------------------------------------------------------------------------

float FGenObject::GetBraking() const
{
	return FMath::Max(Locomotor->Braking, 1e-4f);
}

float FGenObject::GetForwardSpeed2D() const
{
	const FVector2f Dir = GetDirection2D();
	return Velocity.X * Dir.X + Velocity.Y * Dir.Y;
}

void FGenObject::LocoMoveTowardsPosition(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame)
{
	const float MaxSpeed = Locomotor->Speed;
	DesiredSpeed = FMath::Min(DesiredSpeed, MaxSpeed);
	bMaintainPosValid = false;

	const float DistToStopAtMaxSpeed = (MaxSpeed / GetBraking()) * MaxSpeed / 2.f;
	if (OnPathDistToGoal > CellSize && OnPathDistToGoal > DistToStopAtMaxSpeed)
	{
		bLocoBraking = false;
		BrakingFactor = 1.f;
	}

	const float DX = GoalPos.X - Position.X;
	const float DY = GoalPos.Y - Position.Y;
	float Dist = FMath::Sqrt(DX * DX + DY * DY);
	if (Dist > OnPathDistToGoal)
	{
		if (Dist > 2.f * OnPathDistToGoal)
		{
			bLocoBraking = true;
		}
		OnPathDistToGoal = Dist;
	}

	// A zero motive force marks the object as driven by its locomotor.
	ApplyMotiveForce(FVector3f::ZeroVector, Frame);

	if (Locomotor->Appearance == EGenLocoAppearance::Wings)
	{
		bLocoBraking = false; // wings never "brake into place"
	}
	const bool bWasBraking = bStatusBraking;
	// Up in the air only locomotors that say so can still push themselves along.
	if (Locomotor->bAllowAirborneMotiveForce || !bFlies || !IsAirborneTarget())
	{
		switch (Locomotor->Appearance)
		{
		case EGenLocoAppearance::TwoLegs: MoveLegs(GoalPos, OnPathDistToGoal, DesiredSpeed); break;
		case EGenLocoAppearance::FourWheels:
		case EGenLocoAppearance::Motorcycle: MoveWheels(GoalPos, OnPathDistToGoal, DesiredSpeed, Frame); break;
		case EGenLocoAppearance::Treads: MoveTreads(GoalPos, OnPathDistToGoal, DesiredSpeed); break;
		default: MoveOther(GoalPos, OnPathDistToGoal, DesiredSpeed, Frame); break; // hover, wings, other (and thrust for now)
		}
	}
	if (bFlies)
	{
		HandleBehaviorZ(Frame);
	}

	// Objects that are braking don't follow the normal physics, so they end up at their destination exactly.
	bStatusBraking = bLocoBraking;
	if (bWasBraking && Dist > 0.001f)
	{
		const float MinVel = CellSize / float(GenLogic::GetLogicFPS());
		float Vel = FMath::Abs(GetForwardSpeed2D());
		Vel = FMath::Clamp(Vel, MinVel, Dist);
		Position.X += DX / Dist * Vel;
		Position.Y += DY / Dist * Vel;
	}
}

int32 FGenObject::RotateAroundLocoPivot(const FVector3f& GoalPos, float MaxTurnRate, float* OutRelAngle)
{
	float Offset = Locomotor->TurnPivotOffset;
	if (bLocoBraking)
	{
		Offset = 0.f;
	}
	int32 Turn = TurnNone;
	if (Offset != 0.f)
	{
		const float Radius = Template->GetBoundingCircleRadius();
		const FVector2f Dir = GetDirection2D();
		const FVector2f TurnPos = FVector2f(Position.X, Position.Y) + Dir * (Offset * Radius);
		const float DX = GoalPos.X - TurnPos.X, DY = GoalPos.Y - TurnPos.Y;
		if (FMath::Abs(DX) < 0.1f && FMath::Abs(DY) < 0.1f)
		{
			return TurnNone;
		}
		float Amount = GenLogic::AngleDiff(FMath::Atan2(DY, DX), Orientation);
		if (OutRelAngle) { *OutRelAngle = Amount; }
		if (Amount > MaxTurnRate) { Amount = MaxTurnRate; Turn = TurnPositive; }
		else if (Amount < -MaxTurnRate) { Amount = -MaxTurnRate; Turn = TurnNegative; }
		// Rotate the object around the pivot point.
		const float C = FMath::Cos(Amount), S = FMath::Sin(Amount);
		const FVector2f Rel = FVector2f(Position.X, Position.Y) - TurnPos;
		Position.X = TurnPos.X + Rel.X * C - Rel.Y * S;
		Position.Y = TurnPos.Y + Rel.X * S + Rel.Y * C;
		SetOrientation(Orientation + Amount);
	}
	else
	{
		const float DesiredAngle = FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X);
		float Amount = GenLogic::AngleDiff(DesiredAngle, Orientation);
		if (OutRelAngle) { *OutRelAngle = Amount; }
		if (Amount > MaxTurnRate) { Amount = MaxTurnRate; Turn = TurnPositive; }
		else if (Amount < -MaxTurnRate) { Amount = -MaxTurnRate; Turn = TurnNegative; }
		SetOrientation(Orientation + Amount);
	}
	return Turn;
}

void FGenObject::ApplyGoalSpeed(float GoalSpeed, float ActualSpeed, float MaxAccel, float InBrakingFactor, bool bBackwards)
{
	float SpeedDelta = GoalSpeed - ActualSpeed;
	if (bBackwards)
	{
		SpeedDelta = -GoalSpeed + ActualSpeed;
	}
	if (SpeedDelta == 0.f)
	{
		return;
	}
	float Acceleration;
	if (bBackwards)
	{
		Acceleration = SpeedDelta < 0.f ? -MaxAccel : InBrakingFactor * GetBraking();
	}
	else
	{
		Acceleration = SpeedDelta > 0.f ? MaxAccel : -InBrakingFactor * GetBraking();
	}
	float AccelForce = Mass * Acceleration;
	// Don't accelerate/brake more than necessary to reach the goal speed.
	const float MaxForceNeeded = Mass * SpeedDelta;
	if (FMath::Abs(AccelForce) > FMath::Abs(MaxForceNeeded))
	{
		AccelForce = MaxForceNeeded;
	}
	const FVector2f Dir = GetDirection2D();
	ApplyForce(FVector3f(AccelForce * Dir.X, AccelForce * Dir.Y, 0.f));
}

void FGenObject::MoveTreads(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed)
{
	DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed);
	float RelAngle = 0.f;
	RotateAroundLocoPivot(GoalPos, Locomotor->TurnRate, &RelAngle);

	// The more we have to turn, the slower we go.
	const float AngleCoeff = FMath::Min(FMath::Abs(RelAngle) / (PI / 4.f), 1.f);
	const float DX = Position.X - GoalPos.X, DY = Position.Y - GoalPos.Y;
	float GoalSpeed = (1.f - AngleCoeff) * DesiredSpeed;

	const float ActualSpeed = GetForwardSpeed2D();
	const float SlowDownTime = ActualSpeed / GetBraking();
	const float SlowDownDist = (ActualSpeed / 1.5f) * SlowDownTime;

	if (Sqr(DX) + Sqr(DY) < Sqr(2.f * CellSize) && AngleCoeff > 0.05f)
	{
		GoalSpeed = ActualSpeed * 0.6f;
	}
	if (OnPathDistToGoal < SlowDownDist && !bLocoBraking)
	{
		bLocoBraking = true;
		BrakingFactor = 1.1f;
	}
	if (OnPathDistToGoal > CellSize && OnPathDistToGoal > 2.f * SlowDownDist)
	{
		bLocoBraking = false;
	}
	if (bLocoBraking)
	{
		BrakingFactor = FMath::Min(Sqr(SlowDownDist / FMath::Max(OnPathDistToGoal, 0.001f)), MaxBrakingFactor);
		if (SlowDownDist > OnPathDistToGoal) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking(), 0.f); }
		else if (SlowDownDist > OnPathDistToGoal * 0.75f) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking() / 2.f, 0.f); }
		else { GoalSpeed = ActualSpeed; }
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, Locomotor->Acceleration, BrakingFactor);
}

void FGenObject::MoveWheels(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame)
{
	const float MaxSpeed = Locomotor->Speed;
	const float MaxTurnRate = Locomotor->TurnRate;
	const float MaxAccel = Locomotor->Acceleration;
	DesiredSpeed = FMath::Min(DesiredSpeed, MaxSpeed);

	const float TurnSpeed = FMath::Max(Locomotor->MinTurnSpeed, MaxSpeed / 4.f);
	float DesiredAngle = FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X);
	float RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);
	bool bMoveBackwards = false;
	float ActualSpeed = GetForwardSpeed2D();
	bool bThreePoint = false;

	if (ActualSpeed == 0.f)
	{
		bLocoBackwards = false;
		if (Locomotor->bCanMoveBackwards && FMath::Abs(RelAngle) > PI / 2.f)
		{
			bLocoBackwards = true;
			bLocoThreePointTurn = OnPathDistToGoal > 5.f * Template->GeometryMajorRadius;
		}
	}
	if (bLocoBackwards)
	{
		if (FMath::Abs(RelAngle) < PI / 2.f)
		{
			bLocoBackwards = false;
		}
		else
		{
			bMoveBackwards = true;
			bLocoThreePointTurn = OnPathDistToGoal > 5.f * Template->GeometryMajorRadius;
			bThreePoint = bLocoThreePointTurn;
			if (!bThreePoint)
			{
				DesiredAngle = GenLogic::AngleDiff(DesiredAngle, PI);
				RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);
			}
		}
	}

	if (FMath::Abs(RelAngle) > PI / 20.f && DesiredSpeed > TurnSpeed)
	{
		DesiredSpeed = TurnSpeed;
	}
	float GoalSpeed = DesiredSpeed;
	if (bMoveBackwards)
	{
		ActualSpeed = -ActualSpeed;
	}
	const float SlowDownTime = ActualSpeed / GetBraking() + 1.f;
	const float SlowDownDist = (ActualSpeed / 1.5f) * SlowDownTime + ActualSpeed;
	const float EffectiveSlowDownDist = FMath::Max(SlowDownDist, CellSize);

	// If a sharp turn would take us onto impassable terrain, just turn in place.
	if (FMath::Abs(RelAngle) > PI / 12.f)
	{
		const float ProjectFrames = float(GenLogic::GetLogicFPS()) / 2.f;
		const float Distance = ProjectFrames * (GoalSpeed + ActualSpeed) / 2.f;
		const float TurnFactor = FMath::Min(((GoalSpeed + ActualSpeed) / 2.f) / TurnSpeed, 1.f);
		const float TurnAmount = ProjectFrames * TurnFactor * MaxTurnRate / 4.f;
		const float TargetAngle = Orientation + (RelAngle < 0.f ? -TurnAmount : TurnAmount);
		const FVector2f Offset(FMath::Cos(TargetAngle) * Distance, FMath::Sin(TargetAngle) * Distance);
		const FVector2f P(Position.X, Position.Y);
		FGenPathfinder& PF = Logic->GetPathfinder();
		if (!PF.IsPassable(P + Offset * 0.5f, Locomotor->Surfaces) || !PF.IsPassable(P + Offset, Locomotor->Surfaces))
		{
			RotateAroundLocoPivot(GoalPos, Locomotor->TurnRate);
			ApplyMotiveForce(FVector3f::ZeroVector, Frame);
			return;
		}
	}

	if (OnPathDistToGoal < EffectiveSlowDownDist && !bLocoBraking)
	{
		bLocoBraking = true;
		BrakingFactor = 1.1f;
	}
	if (OnPathDistToGoal > CellSize && OnPathDistToGoal > 2.f * SlowDownDist)
	{
		bLocoBraking = false;
	}
	const float DonutDistance = 4.f * CellSize;
	if (OnPathDistToGoal > DonutDistance)
	{
		DonutTimer = Frame + uint32(2.5f * GenLogic::GetLogicFPS());
	}
	else if (DonutTimer < Frame)
	{
		bLocoBraking = true; // stop circling around the goal ("donuts")
	}
	if (bLocoBraking)
	{
		BrakingFactor = 1.f;
		if (SlowDownDist > OnPathDistToGoal) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking(), 0.f); }
		else if (SlowDownDist > OnPathDistToGoal * 0.75f) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking() / 2.f, 0.f); }
		else { GoalSpeed = ActualSpeed; }
	}

	const float TurnFactor = FMath::Min(FMath::Abs(ActualSpeed / TurnSpeed), 1.f);
	const float TurnAmount = TurnFactor * MaxTurnRate;
	if (bMoveBackwards && !bThreePoint)
	{
		const FVector3f BackwardPos(2.f * Position.X - GoalPos.X, 2.f * Position.Y - GoalPos.Y, Position.Z);
		RotateAroundLocoPivot(BackwardPos, TurnAmount);
	}
	else
	{
		RotateAroundLocoPivot(GoalPos, TurnAmount);
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, MaxAccel, BrakingFactor, bMoveBackwards);
}

void FGenObject::MoveLegs(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed)
{
	DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed);
	const float ActualSpeed = GetForwardSpeed2D();
	const float DesiredAngle = FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X);
	const float RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);

	// locoUpdate_moveTowardsAngle: legs turn in place towards the goal.
	const float Turn = FMath::Clamp(RelAngle, -Locomotor->TurnRate, Locomotor->TurnRate);
	SetOrientation(Orientation + Turn);

	const float AngleCoeff = FMath::Min(FMath::Abs(RelAngle) / (PI / 4.f), 1.f);
	float GoalSpeed = (1.f - AngleCoeff) * DesiredSpeed;

	// calcSlowDownDist
	const float Delta = ActualSpeed - Locomotor->MinSpeed;
	const float SlowDownDist = Delta > 0.f ? (Sqr(Delta) / GetBraking()) * 0.5f * 1.05f : 0.f;
	if (OnPathDistToGoal < SlowDownDist)
	{
		GoalSpeed = Locomotor->MinSpeed;
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, Locomotor->Acceleration, 1.f);
}

// ---- PhysicsBehavior ----------------------------------------------------------------------------

void FGenObject::ApplyMotiveForce(const FVector3f& Force, uint32 Frame)
{
	ApplyForce(Force);
	MotiveForceExpires = Frame + uint32(GenLogic::GetLogicFPS() / 3); // MOTIVE_FRAMES
}

void FGenObject::UpdatePhysics(uint32 Frame)
{
	// applyFrictionalForces (on the ground)
	const float Extra = (Locomotor ? Locomotor->Extra2DFriction : 0.f) + (bLocoUltraAccurate ? 0.5f * 30.f / float(GenLogic::GetLogicFPS()) : 0.f);
	if (Velocity.X != 0.f || Velocity.Y != 0.f)
	{
		const FVector2f Dir = GetDirection2D();
		const float LateralDot = Velocity.X * (-Dir.Y) + Velocity.Y * Dir.X;
		const float LF = FMath::Clamp(LateralFriction + Extra, 0.01f, 0.99f);
		FVector3f A(-(LF * LateralDot * -Dir.Y), -(LF * LateralDot * Dir.X), 0.f);
		if (Frame >= MotiveForceExpires)
		{
			const float ForwardDot = Velocity.X * Dir.X + Velocity.Y * Dir.Y;
			const float FF = FMath::Clamp(ForwardFriction + Extra, 0.01f, 0.99f);
			A.X += -(FF * ForwardDot * Dir.X);
			A.Y += -(FF * ForwardDot * Dir.Y);
		}
		Accel += A;
	}

	// integrate
	Velocity += Accel;
	Accel = FVector3f::ZeroVector;
	if (FMath::Abs(Velocity.X) < 0.001f) { Velocity.X = 0.f; }
	if (FMath::Abs(Velocity.Y) < 0.001f) { Velocity.Y = 0.f; }
	Velocity.Z = 0.f;
	if (!bStatusBraking)
	{
		Position.X += Velocity.X;
		Position.Y += Velocity.Y;
	}

	// Ground units stay on the terrain.
	Position.Z = Logic->GetGroundHeight(Position.X, Position.Y);
}

void FGenObject::UpdateFreePhysics(uint32 Frame)
{
	// PhysicsBehavior::update for objects that are not stuck to the ground: debris and hulks thrown by
	// an ObjectCreationList fall under gravity, bounce, tumble and come to rest.
	constexpr float SignificantlyAboveTerrain = 9.f;  // SIGNIFICANTLY_ABOVE_TERRAIN_THRESHOLD
	const FGenGameData& GD = FGenGameData::Get();
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS());
	float GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	auto DampSpin = [this](float F) { YawRate *= F; PitchRate *= F; RollRate *= F; };

	// applyGravitationalForces
	Accel.Z += GD.Gravity;

	// applyFrictionalForces: ground friction near the terrain, air resistance above it
	if (Position.Z - GroundZ <= SignificantlyAboveTerrain)
	{
		DampSpin(1.f - 0.15f * RateScale);
		if (Velocity.X != 0.f || Velocity.Y != 0.f)
		{
			const float Yaw = GetYawFromRotation();
			const FVector2f Dir(FMath::Cos(Yaw), FMath::Sin(Yaw));
			const float LateralDot = Velocity.X * (-Dir.Y) + Velocity.Y * Dir.X;
			const float ForwardDot = Velocity.X * Dir.X + Velocity.Y * Dir.Y;
			const float LF = FMath::Clamp(LateralFriction, 0.01f, 0.99f);
			const float FF = FMath::Clamp(ForwardFriction, 0.01f, 0.99f);
			Accel.X += -(LF * LateralDot * -Dir.Y) - FF * ForwardDot * Dir.X;
			Accel.Y += -(LF * LateralDot * Dir.X) - FF * ForwardDot * Dir.Y;
		}
	}
	else
	{
		const float Aero = FMath::Clamp(Template->AerodynamicFriction + ExtraFriction, 0.f, 0.99f);
		Accel -= Velocity * Aero;
		DampSpin(1.f - Aero);
	}

	// integrate
	Velocity += Accel;
	Accel = FVector3f::ZeroVector;
	if (FMath::Abs(Velocity.X) < 0.001f) { Velocity.X = 0.f; }
	if (FMath::Abs(Velocity.Y) < 0.001f) { Velocity.Y = 0.f; }
	if (FMath::Abs(Velocity.Z) < 0.001f) { Velocity.Z = 0.f; }
	const float OldZ = Position.Z;
	Position += Velocity;

	if (YawRate != 0.f || PitchRate != 0.f || RollRate != 0.f)
	{
		// mtx.Rotate_X(roll); Rotate_Y(pitch); Rotate_Z(yaw): each turns about the object's own axes.
		const float F = Template->PitchRollYawFactor;
		Rotation = (Rotation * FQuat4f(FVector3f::ForwardVector, RollRate * F) * FQuat4f(FVector3f::RightVector, PitchRate * F)
			* FQuat4f(FVector3f::UpVector, YawRate * F)).GetNormalized();
		bFreeRotation = true;
	}

	// handleBounce
	GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	float BounceAccel = 0.f;
	if (bAllowBounce && Position.Z <= GroundZ)
	{
		const float Stiffness = FMath::Clamp(FGenFXData::Get().GroundStiffness, 0.01f, 0.99f);
		if (OldZ > GroundZ && Velocity.Z < 0.f)
		{
			BounceAccel = FMath::Abs(Velocity.Z) * Stiffness;
		}
		DampSpin(0.7f);
		if (BounceAccel <= 0.f)
		{
			bAllowBounce = Template->bAllowBouncing;
		}
	}

	// do not pass through the ground
	if (Position.Z <= GroundZ)
	{
		Velocity.Z += GroundZ - Position.Z; // remove the excess z velocity
		if (Velocity.Z > 0.f) { Velocity.Z = 0.f; }
		Position.Z = GroundZ;
	}

	if (BounceAccel > 0.f)
	{
		// The object is righted on impact (pitch and roll may have been anything), then bounces back up.
		SetOrientation(GetYawFromRotation());
		Accel.Z += BounceAccel;
	}
	else
	{
		Orientation = GetYawFromRotation();
	}

	const bool bResting = Position.Z <= GroundZ && FMath::Abs(Velocity.X) < 0.01f && FMath::Abs(Velocity.Y) < 0.01f && FMath::Abs(Velocity.Z) < 0.01f;
	if (Template->bKillWhenRestingOnGround && bResting && !bDead)
	{
		Kill();
	}
}
