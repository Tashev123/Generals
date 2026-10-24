// The body of a vehicle on the ground: how it sits on the slope under it, rocks when it speeds up, brakes
// and turns, bounces along rough ground and rides up over what it crushes - and its wheels' springs and
// steering. Follows Drawable::calcPhysicsXformTreads, calcPhysicsXformWheels and calcPhysicsXformMotorcycle
// (Drawable.cpp). The original works this out as it draws, once per 30 Hz frame; here it is done at the
// end of each logic step, after the collisions (which find what is being driven over), and the screen
// blends it between steps like the rest of the object. Nothing the logic decides depends on it.

#include "Logic/GenObject.h"
#include "Core/GenPerf.h"
#include "Logic/GenConditions.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPathfinder.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

bool FGenObject::IsMotive() const
{
	return Logic->GetFrame() < MotiveForceExpires;
}

bool FGenObject::IsSignificantlyAboveTerrain() const
{
	return Position.Z - Logic->GetGroundHeight(Position.X, Position.Y) > -9.f * FGenGameData::Get().Gravity;
}

bool FGenObject::PeekNextPathPoint(FVector3f& Out) const
{
	if (!bMoving || !Path.IsValidIndex(PathIndex))
	{
		return false;
	}
	Out = FVector3f(Path[PathIndex].X, Path[PathIndex].Y, 0.f);
	return true;
}

bool FGenObject::HasChassis() const
{
	if (!Locomotor || bDead || ContainedBy != GenInvalidObjectID || IsUsingAirLoco())
	{
		return false;
	}
	const EGenLocoAppearance A = Locomotor->Appearance;
	return A == EGenLocoAppearance::Treads || A == EGenLocoAppearance::FourWheels || A == EGenLocoAppearance::Motorcycle;
}

void UGenGameLogic::UpdateChassis()
{
	GEN_PERF_SCOPE("Chassis");
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject& O = *Objects[Id];
		if (O.HasChassis())
		{
			O.UpdateChassis(ClientRandom);
		}
	}
}

void FGenObject::UpdateChassis(FRandomStream& ClientRandom)
{
	const FGenLocomotorTemplate& L = *Locomotor;
	const bool bTreads = L.Appearance == EGenLocoAppearance::Treads;
	const bool bBike = L.Appearance == EGenLocoAppearance::Motorcycle;
	const FVector2f Dir = GetDirection2D();
	const FVector2f Perp(-Dir.Y, Dir.X);

	// "find pitch and roll of terrain under chassis" (on a bridge: the deck's slope)
	FVector3f Normal = FVector3f::UpVector;
	float GroundZ;
	if (Layer != 0)
	{
		const FGenPathfinder& PF = Logic->GetPathfinder();
		auto Deck = [this, &PF](float X, float Y) { return PF.GetLayerHeight(X, Y, Layer, Logic->GetGroundHeight(X, Y)); };
		constexpr float D = 5.f;
		GroundZ = Deck(Position.X, Position.Y);
		Normal = FVector3f(-(Deck(Position.X + D, Position.Y) - Deck(Position.X - D, Position.Y)),
			-(Deck(Position.X, Position.Y + D) - Deck(Position.X, Position.Y - D)), 2.f * D).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
	}
	else
	{
		GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
		if (const FGenMapData* Map = Logic->GetMap())
		{
			Normal = Map->GetGroundNormal(Position.X, Position.Y);
		}
	}

	float NewOverlapZ = 0.f;
	const FGenObject* Over = nullptr;
	if (bTreads)
	{
		// "override surface normal if we are overlapping another object - crushing it"
		constexpr float OverlapShrinkFactor = 0.8f;
		constexpr float FlattenedObjectHeight = 0.5f;
		constexpr float LeaveOverlapPitchKick = PI / 128.f;
		constexpr float OverlapRoughVibrationFactor = 5.f;
		constexpr float MaxRoughVibration = 0.5f;
		Over = CurrentOverlap != GenInvalidObjectID ? Logic->FindObject(CurrentOverlap) : nullptr;
		if (Over && Over->IsKindOf(TEXT("SHRUBBERY")))
		{
			Over = nullptr; // "We just smash through shrubbery."
		}
		if (Over)
		{
			const float DX = Over->Position.X - Position.X, DY = Over->Position.Y - Position.Y;
			const float CenterDistSqr = DX * DX + DY * DY;
			const float MaxCenterDist = (Template->GetBoundingCircleRadius() + Over->Template->GetBoundingCircleRadius()) * OverlapShrinkFactor;
			if (CenterDistSqr < MaxCenterDist * MaxCenterDist)
			{
				const float CenterDist = FMath::Sqrt(CenterDistSqr);
				const float Amount = FMath::Clamp(1.f - CenterDist / MaxCenterDist, 0.f, 1.f);
				// "rough vibrations proportional to speed when we drive over something"
				const float Rough = FMath::Min((Velocity.X * Velocity.X + Velocity.Y * Velocity.Y) * OverlapRoughVibrationFactor, MaxRoughVibration);
				float Height = Over->Template->GetMaxHeightAbovePosition();
				// "do not 'go up' flattened crushed things"
				static const int32 FrontCrushed = GenConditions::Index(TEXT("FRONTCRUSHED")), BackCrushed = GenConditions::Index(TEXT("BACKCRUSHED"));
				const bool bFlat = Over->IsKindOf(TEXT("LOW_OVERLAPPABLE")) || Over->IsKindOf(TEXT("INFANTRY"))
					|| (Over->GetModelConditions().Test(FrontCrushed) && Over->GetModelConditions().Test(BackCrushed));
				if (bFlat)
				{
					Height = FlattenedObjectHeight;
				}
				if (Amount < FlattenedObjectHeight && !bFlat)
				{
					NewOverlapZ = Height * 2.f * Amount;
					// "compute vector along 'surface'"
					const FVector3f V(DX / CenterDist, DY / CenterDist, 0.2f);
					const FVector3f Up = FVector3f(ClientRandom.FRandRange(-Rough, Rough), ClientRandom.FRandRange(-Rough, Rough), 1.f).GetSafeNormal();
					const FVector3f Prp = FVector3f::CrossProduct(V, Up);
					Normal = FVector3f::CrossProduct(Prp, V).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
				}
				else
				{
					// "sitting on top of object"
					NewOverlapZ = Height;
					Normal = FVector3f(ClientRandom.FRandRange(-Rough, Rough), ClientRandom.FRandRange(-Rough, Rough), 1.f).GetSafeNormal();
				}
			}
		}
		else if (PreviousOverlap != GenInvalidObjectID && OverlapZ > 0.f)
		{
			// "if we had an overlap last frame, and we're now in the air, give a kick to the pitch for effect"
			ChassisPitchRate += LeaveOverlapPitchKick;
		}
	}
	const float GroundPitch = (Normal.X * Dir.X + Normal.Y * Dir.Y) * HALF_PI;
	const float GroundRoll = (Normal.X * Perp.X + Normal.Y * Perp.Y) * HALF_PI;
	const float Length = Template->GeometryMajorRadius;
	const float Width = Template->IsBox() ? Template->GeometryMinorRadius : Template->GeometryMajorRadius;

	// isSignificantlyAboveTerrain: "more than 3 frames to return to the ground"
	const bool bAirborne = !bTreads && Position.Z - GroundZ > -9.f * FGenGameData::Get().Gravity;
	if (bAirborne && L.bHasSuspension)
	{
		// "Wheels extend when airborne."
		Wheels.FramesAirborne = 0;
		++Wheels.FramesAirborneCounter;
		const float Target = Position.Z - GroundZ > -L.MaxWheelExtension ? L.MaxWheelExtension : 0.f;
		Wheels.RearLeft += (Target - Wheels.RearLeft) / 2.f;
		Wheels.RearRight = bBike ? Wheels.RearLeft : Wheels.RearRight + (Target - Wheels.RearRight) / 2.f;
	}
	if (bAirborne && !bBike)
	{
		// "maintain the same orientation while we fly through the air"
		const float PitchHeight = Length * FMath::Sin(ChassisPitch + AccelPitch - GroundPitch);
		const float RollHeight = Width * FMath::Sin(ChassisRoll + AccelRoll - GroundRoll);
		Chassis.Z = FMath::Abs(PitchHeight) / 4.f + FMath::Abs(RollHeight) / 4.f;
		return;
	}

	if (!bTreads && !bAirborne)
	{
		// "Bouncy."
		const float CurSpeed = Velocity.Size();
		const float MaxSpeed = L.Speed;
		if (MaxSpeed > 0.f && CurSpeed > MaxSpeed / 10.f)
		{
			const float Factor = CurSpeed / MaxSpeed;
			const float Kick = L.BounceKick * Factor;
			if (FMath::Abs(ChassisPitchRate) < Kick / 4.f && FMath::Abs(ChassisRollRate) < Kick / 8.f)
			{
				switch (ClientRandom.RandRange(0, 3))
				{
				case 0: ChassisPitchRate -= Kick; ChassisRollRate -= Kick / 2.f; break;
				case 1: ChassisPitchRate += Kick; ChassisRollRate -= Kick / 2.f; break;
				case 2: ChassisPitchRate -= Kick; ChassisRollRate += Kick / 2.f; break;
				default: ChassisPitchRate += Kick; ChassisRollRate += Kick / 2.f; break;
				}
			}
		}
	}

	// "process chassis suspension dynamics - damp back towards groundPitch; the ground can only push back if
	// we're touching it" (a motorcycle in the air levels itself out).
	if (bTreads ? (Over != nullptr || OverlapZ <= 0.f) : !bAirborne)
	{
		ChassisPitchRate += -L.PitchStiffness * (ChassisPitch - GroundPitch) - L.PitchDamping * ChassisPitchRate;
		if (!bBike && ChassisPitchRate > 0.f)
		{
			ChassisPitchRate *= 0.5f;
		}
		ChassisRollRate += -L.RollStiffness * (ChassisRoll - GroundRoll) - L.RollDamping * ChassisRollRate;
	}
	else if (bBike)
	{
		ChassisPitchRate += -L.PitchStiffness * ChassisPitch - L.PitchDamping * ChassisPitchRate;
		ChassisRollRate += -L.RollStiffness * ChassisRoll - L.RollDamping * ChassisRollRate;
	}
	ChassisPitch += ChassisPitchRate * L.UniformAxialDamping;
	ChassisRoll += ChassisRollRate * L.UniformAxialDamping;

	// "process chassis acceleration dynamics - damp back towards zero"
	AccelPitchRate += -L.PitchStiffness * AccelPitch - L.PitchDamping * AccelPitchRate;
	AccelPitch += AccelPitchRate;
	AccelRollRate += -L.RollStiffness * AccelRoll - L.RollDamping * AccelRollRate;
	AccelRoll += AccelRollRate;

	Chassis.Pitch = ChassisPitch + AccelPitch;
	Chassis.Roll = ChassisRoll + AccelRoll;
	if (bBike)
	{
		// "Now only motorcycles enjoy this constraint" - whose test (> 0.5 and < -0.5 at once) never passes: no roll.
		Chassis.Roll = 0.f;
	}

	if (Logic->GetFrame() < MotiveForceExpires)
	{
		// "cause the chassis to pitch & roll in reaction to acceleration/deceleration"
		const float ForwardAccel = Dir.X * PrevAccel.X + Dir.Y * PrevAccel.Y;
		AccelPitchRate += -(L.ForwardAccelCoef * ForwardAccel);
		const float LateralAccel = -Dir.Y * PrevAccel.X + Dir.X * PrevAccel.Y;
		AccelRollRate += -(L.LateralAccelCoef * LateralAccel);
	}
	AccelPitch = FMath::Clamp(AccelPitch, -L.AccelPitchLimit, L.DecelPitchLimit);
	AccelRoll = FMath::Clamp(AccelRoll, -L.AccelPitchLimit, L.DecelPitchLimit);

	if (bTreads)
	{
		// The fake height over what it is crushing: up at once, down under a fake gravity.
		if (NewOverlapZ > OverlapZ)
		{
			OverlapZ = NewOverlapZ;
			OverlapZVel = 0.f;
		}
		const float ZTmp = OverlapZ / 2.f;
		if (OverlapZ > 0.f)
		{
			OverlapZVel -= 0.2f;
			OverlapZ += OverlapZVel;
		}
		if (OverlapZ <= 0.f)
		{
			OverlapZ = 0.f;
			OverlapZVel = 0.f;
		}
		Chassis.Z = ZTmp;
		return;
	}

	// The wheels: steering, and how far each spring is pushed in or let out.
	Chassis.Z = 0.f;
	const float PitchHeight = Length * FMath::Sin(Chassis.Pitch - GroundPitch);
	const float RollHeight = Width * FMath::Sin(Chassis.Roll - GroundRoll);
	if (L.bHasSuspension)
	{
		Wheels.FramesAirborne = Wheels.FramesAirborneCounter;
		Wheels.FramesAirborneCounter = 0;
		Wheels.LandingSerial += Wheels.FramesAirborne > 0 ? 1 : 0;
		float WantAngle = Turning < 0 ? -L.WheelTurnAngle : Turning > 0 ? L.WheelTurnAngle : 0.f;
		if (GetForwardSpeed2D() < 0.f)
		{
			WantAngle = -WantAngle; // "if we're moving backwards, the wheels rotate in the opposite direction"
		}
		Wheels.Angle += (WantAngle - Wheels.Angle) / 10.f; // WHEEL_SMOOTHNESS
		constexpr float SpringFactor = 0.9f;
		float FL, FR, RL, RR;
		if (PitchHeight < 0.f)
		{
			// "Front raising up"
			FL = FR = SpringFactor * (PitchHeight / 3.f + PitchHeight / 2.f);
			RL = RR = -PitchHeight / 2.f + PitchHeight / 4.f;
		}
		else
		{
			// "Back raising up."
			FL = FR = -PitchHeight / 4.f + PitchHeight / 2.f;
			RL = RR = SpringFactor * (-PitchHeight / 2.f + -PitchHeight / 3.f);
		}
		if (!bBike)
		{
			if (RollHeight > 0.f)
			{
				// "Right raising up"
				FR += -SpringFactor * (RollHeight / 3.f + RollHeight / 2.f);
				RR += -SpringFactor * (RollHeight / 3.f + RollHeight / 2.f);
				RL += RollHeight / 2.f - RollHeight / 4.f;
				FL += RollHeight / 2.f - RollHeight / 4.f;
			}
			else
			{
				// "Left raising up."
				FR += -RollHeight / 2.f + RollHeight / 4.f;
				RR += -RollHeight / 2.f + RollHeight / 4.f;
				RL += SpringFactor * (RollHeight / 3.f + RollHeight / 2.f);
				FL += SpringFactor * (RollHeight / 3.f + RollHeight / 2.f);
			}
		}
		// "If it's going down, dampen the movement a bit"
		auto Spring = [](float& Cur, float Want) { Cur = Want < Cur ? Cur + (Want - Cur) / 2.f : Want; };
		Spring(Wheels.FrontLeft, FL);
		Spring(Wheels.RearLeft, RL);
		if (bBike)
		{
			Wheels.FrontRight = Wheels.FrontLeft;
			Wheels.RearRight = Wheels.RearLeft;
		}
		else
		{
			Spring(Wheels.FrontRight, FR);
			Spring(Wheels.RearRight, RR);
		}
		const float MaxExt = L.MaxWheelExtension;
		Wheels.FrontLeft = FMath::Max(Wheels.FrontLeft, MaxExt);
		Wheels.FrontRight = FMath::Max(Wheels.FrontRight, MaxExt);
		Wheels.RearLeft = FMath::Max(Wheels.RearLeft, MaxExt);
		Wheels.RearRight = FMath::Max(Wheels.RearRight, MaxExt);
	}
	// "If we are > 22 degrees, need to raise height"
	float Divisor = 4.f;
	const float Pitch = FMath::Abs(Chassis.Pitch - GroundPitch);
	if (Pitch > PI / 8.f)
	{
		Divisor = (4.f * PI / 8.f + (Pitch - PI / 8.f)) / Pitch;
	}
	if (!bAirborne)
	{
		Chassis.Z += FMath::Abs(PitchHeight) / Divisor + FMath::Abs(RollHeight) / Divisor;
	}
}
