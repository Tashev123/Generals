// Flight for air-only locomotors: helicopters (HOVER) and planes (WINGS).
// Follows Locomotor.cpp (moveTowardsPositionOther / Hover / Wings, handleBehaviorZ, calcLiftToUseAtPt,
// maintainCurrentPositionHover / Wings), Path::computeFlightDistToGoal, PhysicsBehavior::update for
// airborne objects and Drawable::calcPhysicsXformHoverOrWings for the way the body leans.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"

namespace
{
	constexpr float AirborneThreshold = 9.f;  // SIGNIFICANTLY_ABOVE_TERRAIN_THRESHOLD
}

bool FGenObject::IsAirborneTarget() const
{
	// OBJECT_STATUS_AIRBORNE_TARGET: above the locomotor's AirborneTargetingHeight (aircraft whose locomotor
	// doesn't say count as soon as they are clear of the ground).
	if (!bFlies)
	{
		return false;
	}
	const float Threshold = (Locomotor && Locomotor->AirborneTargetingHeight != MAX_int32) ? float(Locomotor->AirborneTargetingHeight)
		: (IsUsingAirLoco() ? AirborneThreshold : FLT_MAX);
	return Position.Z - Logic->GetGroundHeight(Position.X, Position.Y) > Threshold;
}

bool FGenObject::AreEnginesOn() const
{
	// JetAIUpdate::update: "we just started moving, therefore turn on the engines".
	if (!bJetAI || IsKindOf(TEXT("PRODUCED_AT_HELIPAD")))
	{
		return true;
	}
	return bJetTakeoffInProgress || bJetLandingInProgress || bMoving
		|| Position.Z - Logic->GetGroundHeight(Position.X, Position.Y) > AirborneThreshold;
}

bool FGenObject::IsUsingAirLoco() const
{
	return Locomotor && Locomotor->IsAirOnly();
}

bool FGenObject::HasMinSpeed() const
{
	return Locomotor && Locomotor->MinSpeed > 0.f;
}

float FGenObject::FlightDistToGoal()
{
	// Path::computeFlightDistToGoal: every leg counts for as much of it as still lies ahead of the aircraft,
	// so a plane that cannot stop "arrives" as it passes abeam of its goal. Legs already flown past are dropped.
	const FVector2f Pos(Position.X, Position.Y);
	float Distance = 0.f;
	bool bUseNext = true;
	for (int32 i = PathIndex; i < Path.Num(); ++i)
	{
		const FVector2f Start = i == 0 ? PathStart : Path[i - 1];
		const FVector2f Dir = (Path[i] - Start).GetSafeNormal();
		const float Ahead = FVector2f::DotProduct(Path[i] - Pos, Dir);
		if (Ahead >= 0.f)
		{
			Distance += Ahead;
			bUseNext = false;
		}
		else if (bUseNext && i < Path.Num() - 1)
		{
			PathIndex = i + 1;
		}
	}
	// (Never further than it really is.)
	return FMath::Min(Distance, FVector2f::Distance(Pos, Path.Last()));
}

void FGenObject::MoveOther(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame)
{
	// Locomotor::moveTowardsPositionOther (also the 2D part of hovering): turn towards the goal, push
	// along the nose, and start braking when the stopping distance reaches the distance left.
	DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed);
	float GoalSpeed = DesiredSpeed;
	const float ActualSpeed = GetForwardSpeed2D();
	RotateAroundLocoPivot(GoalPos, Locomotor->TurnRate);

	// calcSlowDownDist
	const float Delta = ActualSpeed - Locomotor->MinSpeed;
	const float SlowDownDist = Delta > 0.f ? (FMath::Square(Delta) / GetBraking()) * 0.5f * 1.05f : 0.f;
	if (OnPathDistToGoal < SlowDownDist)
	{
		GoalSpeed = Locomotor->MinSpeed;
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, Locomotor->Acceleration, 1.f);
}

void FGenObject::HandleBehaviorZ(uint32 Frame)
{
	// Locomotor::handleBehaviorZ for Z_SURFACE_RELATIVE_HEIGHT / Z_ABSOLUTE_HEIGHT: lift holds the
	// preferred height.
	if (!Locomotor->UsesLift())
	{
		return;
	}
	const float LiftToUse = CalcLiftToUse();
	if (LiftToUse != 0.f)
	{
		ApplyMotiveForce(FVector3f(0.f, 0.f, LiftToUse * Mass), Frame);
	}
}

float FGenObject::CalcLiftToUse() const
{
	// (calcLiftToUseAtPt solves x = x0 + v t + a t^2 / 2 for the acceleration.)
	const float Gravity = FGenGameData::Get().Gravity;
	const bool bSurfaceRelative = Locomotor->ZAxisBehavior.Equals(TEXT("SURFACE_RELATIVE_HEIGHT"), ESearchCase::IgnoreCase);
	float Preferred = Locomotor->PreferredHeight + (bSurfaceRelative ? Logic->GetFlightSurfaceHeight(Position.X, Position.Y)
		: (Locomotor->IsLayerRelative() ? GetSurfaceHeight() : 0.f));
	Preferred = Position.Z + (Preferred - Position.Z) * Locomotor->PreferredHeightDamping;

	if (bLocoPreciseZ)
	{
		// PRECISE_Z_POS: taking off and landing, the height to hold is that of the point being flown to.
		Preferred = Position.Z + (PreciseGoalZ - Position.Z) * Locomotor->PreferredHeightDamping;
	}
	const float MaxGrossLift = FMath::Min(Locomotor->Lift, LocoMaxLift);
	const float MaxNetLift = FMath::Max(0.f, MaxGrossLift + Gravity);
	const float VelZ = Velocity.Z;
	// Going down, braking is limited by the net lift; going up, by gravity. Taking off and landing
	// (ULTRA_ACCURATE) the aircraft is allowed to cheat: twice the braking, lift that may push it down.
	const float MaxAccel = bLocoUltraAccurate ? (VelZ < 0.f ? 2.f * MaxNetLift : -2.f * MaxNetLift) : (VelZ < 0.f ? MaxNetLift : Gravity);
	float DesiredAccel = 0.f;
	if (FMath::Abs(MaxAccel) > 0.001f)
	{
		const float DeltaZ = Preferred - Position.Z;
		const float BrakeDist = FMath::Square(VelZ) / FMath::Abs(MaxAccel);
		if (BrakeDist > FMath::Abs(DeltaZ))
		{
			DesiredAccel = MaxAccel;
		}
		else if (FMath::Abs(VelZ) > Locomotor->SpeedLimitZ)
		{
			DesiredAccel = Locomotor->SpeedLimitZ - VelZ;
		}
		else
		{
			DesiredAccel = 2.f * (DeltaZ - VelZ);
		}
	}
	return bLocoUltraAccurate ? FMath::Clamp(DesiredAccel - Gravity, -MaxGrossLift, 3.f * MaxGrossLift)
		: FMath::Clamp(DesiredAccel - Gravity, 0.f, MaxGrossLift);
}

void FGenObject::MaintainPositionWings(uint32 Frame)
{
	// Locomotor::maintainCurrentPositionWings: a plane cannot stop, so it circles the spot, always aiming
	// for the point on the far side of the circle.
	if (Frame >= MotiveForceExpires || Position.Z <= Logic->GetGroundHeight(Position.X, Position.Y))
	{
		return; // sitting on the ground: nothing to hold
	}
	float Radius = Locomotor->CirclingRadius;
	if (Radius == 0.f)
	{
		Radius = Locomotor->MinSpeed / FMath::Max(Locomotor->TurnRate, 1e-4f); // calcMinTurnRadius
	}
	float AimDir = PI - PI / 8.f;
	if (Radius < 0.f)
	{
		Radius = -Radius;
		AimDir = -AimDir;
	}
	const float DX = MaintainPos.X - Position.X, DY = MaintainPos.Y - Position.Y;
	const float Angle = ((FMath::Abs(DX) < 1e-4f && FMath::Abs(DY) < 1e-4f) ? Orientation : FMath::Atan2(DY, DX)) + AimDir;
	const FVector3f Desired(MaintainPos.X + FMath::Cos(Angle) * Radius, MaintainPos.Y + FMath::Sin(Angle) * Radius, MaintainPos.Z);
	MoveOther(Desired, 0.f, Locomotor->MinSpeed, Frame);
}

void FGenObject::MaintainPosition(uint32 Frame)
{
	// Locomotor::locoUpdate_maintainCurrentPosition
	if (!bMaintainPosValid)
	{
		MaintainPos = Position;
		bMaintainPosValid = true;
	}
	if (Locomotor->Appearance == EGenLocoAppearance::Wings)
	{
		MaintainPositionWings(Frame);
	}
	else if (Frame < MotiveForceExpires)
	{
		if (Locomotor->Appearance == EGenLocoAppearance::Hover)
		{
			// maintainCurrentPositionHover: brake to a stop.
			const float ActualSpeed = GetForwardSpeed2D();
			const float MinSpeed = FMath::Max(1e-10f, Locomotor->MinSpeed);
			if (FMath::Abs(MinSpeed - ActualSpeed) > MinSpeed)
			{
				ApplyGoalSpeed(MinSpeed, ActualSpeed, Locomotor->Acceleration, 1.f);
			}
		}
		else
		{
			Velocity.X = Velocity.Y = 0.f; // maintainCurrentPositionOther: scrubVelocity2D
		}
	}
	HandleBehaviorZ(Frame);
}

void FGenObject::UpdateFlightPhysics(uint32 Frame)
{
	// PhysicsBehavior::update: unlike ground units, nothing sticks an aircraft to the terrain.
	float GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	const bool bMotive = Frame < MotiveForceExpires;

	// applyGravitationalForces
	Accel.Z += FGenGameData::Get().Gravity;

	// applyFrictionalForces
	if (Locomotor->bApply2DFrictionWhenAirborne || Position.Z - GroundZ <= AirborneThreshold)
	{
		if (Velocity.X != 0.f || Velocity.Y != 0.f)
		{
			const FVector2f Dir = GetDirection2D();
			const float LateralDot = Velocity.X * (-Dir.Y) + Velocity.Y * Dir.X;
			// (Locomotor::setPhysicsOptions: ULTRA_ACCURATE cranks the friction up, for precise movement.)
			const float Ultra = bLocoUltraAccurate ? 0.5f * 30.f / float(GenLogic::GetLogicFPS()) : 0.f;
			const float LF = FMath::Clamp(LateralFriction + Locomotor->Extra2DFriction + Ultra, 0.01f, 0.99f);
			Accel.X += -(LF * LateralDot * -Dir.Y);
			Accel.Y += -(LF * LateralDot * Dir.X);
			if (!bMotive)
			{
				const float ForwardDot = Velocity.X * Dir.X + Velocity.Y * Dir.Y;
				const float FF = FMath::Clamp(ForwardFriction + Locomotor->Extra2DFriction + Ultra, 0.01f, 0.99f);
				Accel.X += -(FF * ForwardDot * Dir.X);
				Accel.Y += -(FF * ForwardDot * Dir.Y);
			}
		}
	}
	else
	{
		Accel -= Velocity * FMath::Clamp(Template->AerodynamicFriction, 0.f, 0.99f);
	}

	// integrate
	Velocity += Accel;
	Accel = FVector3f::ZeroVector;
	if (FMath::Abs(Velocity.X) < 0.001f) { Velocity.X = 0.f; }
	if (FMath::Abs(Velocity.Y) < 0.001f) { Velocity.Y = 0.f; }
	if (FMath::Abs(Velocity.Z) < 0.001f) { Velocity.Z = 0.f; }
	Position += Velocity;

	// do not pass through the ground
	GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	if (Position.Z <= GroundZ)
	{
		Velocity.Z += GroundZ - Position.Z;
		if (Velocity.Z > 0.f) { Velocity.Z = 0.f; }
		Position.Z = GroundZ;
	}

	UpdateBodyTilt(Frame);
}

void FGenObject::UpdateBodyTilt(uint32 Frame)
{
	// Drawable::calcPhysicsXformHoverOrWings: pitch and roll hang on a spring / damper and are pushed by
	// the forward and sideways speed, so a helicopter dips its nose when it flies and banks into turns.
	// The original steps this once per 30 Hz frame with per-frame speeds.
	const FGenLocomotorTemplate& L = *Locomotor;
	BodyPitchRate += -L.PitchStiffness * BodyPitch - L.PitchDamping * BodyPitchRate;
	BodyRollRate += -L.RollStiffness * BodyRoll - L.RollDamping * BodyRollRate;
	BodyPitch += BodyPitchRate * L.UniformAxialDamping;
	BodyRoll += BodyRollRate * L.UniformAxialDamping;
	const float TotalPitch = BodyPitch;
	const float TotalRoll = BodyRoll;

	if (Frame < MotiveForceExpires)
	{
		const float PerFrame30 = float(GenLogic::GetLogicFPS()) / 30.f;
		const FVector2f Dir = GetDirection2D();
		if (L.PitchInDirectionOfZVelFactor != 0.f && FMath::Abs(Velocity.Z) > 0.001f)
		{
			// Nose up in a climb, down in a descent.
			BodyPitch -= L.PitchInDirectionOfZVelFactor * FMath::Atan2(Velocity.Z, FMath::Sqrt(FMath::Square(Velocity.X) + FMath::Square(Velocity.Y)));
		}
		const float ForwardVel = (Dir.X * Velocity.X + Dir.Y * Velocity.Y) * PerFrame30;
		const float LateralVel = (-Dir.Y * Velocity.X + Dir.X * Velocity.Y) * PerFrame30;
		BodyPitch += -(L.ForwardVelocityPitchFactor * ForwardVel);
		BodyRoll += -(L.LateralVelocityRollFactor * LateralVel);
	}

	if (bFreeRotation || FMath::Abs(TotalPitch) > 1e-4f || FMath::Abs(TotalRoll) > 1e-4f)
	{
		// Drawable::applyPhysicsXform: Rotate_Y(pitch), then Rotate_X(-roll), in the body's own frame.
		Rotation = (FQuat4f(FVector3f::UpVector, Orientation) * FQuat4f(FVector3f::RightVector, TotalPitch)
			* FQuat4f(FVector3f::ForwardVector, -TotalRoll)).GetNormalized();
		bFreeRotation = true;
	}
}
