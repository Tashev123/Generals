// The nuclear missile: NeutronMissileUpdate (how it flies), NeutronMissileSlowDeathBehavior (the blast
// waves where it dies) and MissileLauncherBuildingUpdate (the silo's doors).
//
// The missile appears on the silo's launch bone, lifts off slowly (for SpecialSpeedTime its height is
// set outright: a square-law rise of SpecialSpeedHeight), goes straight on for
// DistanceToTravelBeforeTurning, then makes for a spot TargetFromDirectlyAbove over the target and comes
// straight down from there at half the push. HeightDieUpdate kills it above the ground; dying, it sends
// out up to nine blasts, each after its delay: what stands within a blast's outer radius is toppled,
// hurt (in full inside the inner radius, less further out) and pushed away, and later scorched.
//
// The silo's doors follow the power's timer: they begin to open DoorOpenTime before it is ready, stand
// open while it is, and after the launch wait DoorWaitOpenTime and close.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Logic/GenUpgrades.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	/** STRAIGHT_DOWN_SLOW_FACTOR */
	constexpr float StraightDownSlowFactor = 0.5f;
	/** TOPPLE_OPTIONS_NO_BOUNCE | TOPPLE_OPTIONS_NO_FX (GenTopple.cpp) */
	constexpr uint32 ToppleNoBounceNoFX = 1 | 2;

	enum ELauncherDoor : uint8 { DoorClosed, DoorOpening, DoorOpen, DoorWaitingToClose, DoorClosing };
}

void FGenObject::LaunchNeutronMissile(uint32 Frame)
{
	// NeutronMissileUpdate::projectileFireAtObjectOrPosition
	FGenProjectileState& M = Projectile;
	const FGenNeutronMissile& D = Template->NeutronMissile;
	M.bNeutron = true;
	M.State = EGenMissileState::Launch;
	M.StateFrame = Frame;
	M.OriginalTarget = M.End;
	M.Intermediate = M.End + FVector3f(0.f, 0.f, D.TargetFromDirectlyAbove);
	M.bReachedIntermediate = true;
	M.bNeutronLaunched = false;
	M.NoTurnDistLeft = D.InitialDist;
	M.bArmed = false;
	M.Victim = GenInvalidObjectID;
	Velocity = FVector3f::ZeroVector;
	bFreeRotation = true;
	bHidden = true; // (until it stands on the silo's launch bone, a frame from now)
	if (Template->bHeightDie)
	{
		HeightDieArmedFrame = Frame + uint32(Template->HeightDieInitialDelay);
	}
}

void FGenObject::UpdateNeutronMissile(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Missile");
	FGenProjectileState& M = Projectile;
	const FGenNeutronMissile& D = Template->NeutronMissile;

	if (!M.bReachedIntermediate)
	{
		// Over the target: from here it is straight down.
		const float Bound = FMath::Max(Template->GetBoundingCircleRadius(), Template->GeometryHeight * 0.5f);
		if (FVector3f::DistSquared(Position, M.Intermediate) <= Bound * Bound)
		{
			M.bReachedIntermediate = true;
			Position = M.Intermediate;
			Velocity = FVector3f(0.f, 0.f, -Velocity.Size() * StraightDownSlowFactor);
		}
	}

	const FVector3f OldPos = Position;
	const bool bOldPosValid = M.State == EGenMissileState::Attack; // "not valid till *after* we've launched"
	if (M.State == EGenMissileState::Launch)
	{
		// doLaunch
		if (!M.bNeutronLaunched)
		{
			const FGenObject* LauncherObj = Logic->FindObject(M.Launcher);
			if (!LauncherObj)
			{
				bPendingDestroy = true;
				return;
			}
			FTransform3f Bone;
			if (Logic->GetWeaponLaunchTransform(*LauncherObj, 0, Bone))
			{
				Position = Bone.GetTranslation();
				SetRotation(Bone.GetRotation());
			}
			bHidden = false;
			M.bNeutronLaunched = true;
			M.bReachedIntermediate = D.TargetFromDirectlyAbove == 0.f;
			Logic->DoFXObj(D.LaunchFX, *this);
			M.HeightAtLaunch = Position.Z;
			M.FrameAtLaunch = Frame;
			const FVector3f Nose = Rotation.RotateVector(FVector3f::ForwardVector);
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [nuke f%u] #%u %s lifts off at (%.0f, %.0f, %.0f) pointing (%.2f, %.2f, %.2f) for (%.0f, %.0f)"),
				Frame, ID, *Template->Name, Position.X, Position.Y, Position.Z, Nose.X, Nose.Y, Nose.Z, M.OriginalTarget.X, M.OriginalTarget.Y);
		}
		Position += Velocity;
		Logic->DoFXObj(D.IgnitionFX, *this);
		if (!M.Weapon->ProjectileExhaust.IsEmpty())
		{
			Logic->AttachParticleSystem(M.Weapon->ProjectileExhaust, ID);
		}
		M.State = EGenMissileState::Attack;
		M.StateFrame = Frame;
		M.bArmed = true;
	}
	else if (M.State == EGenMissileState::Attack)
	{
		// doAttack
		float Speed = D.RelativeSpeed;
		if (D.TargetFromDirectlyAbove != 0.f && M.bReachedIntermediate)
		{
			Speed *= StraightDownSlowFactor;
		}
		FVector3f Dir = Rotation.RotateVector(FVector3f::ForwardVector).GetSafeNormal();
		if (M.NoTurnDistLeft <= 0.f)
		{
			// calcTransform: the nose turns towards where it is going, MaxTurnRate at most.
			const FVector3f Want = ((M.bReachedIntermediate ? M.OriginalTarget : M.Intermediate) - Position).GetSafeNormal();
			if (!Want.IsZero())
			{
				const float Angle = FMath::Acos(FMath::Clamp(FVector3f::DotProduct(Dir, Want), -1.f, 1.f));
				if (FMath::Abs(Angle) < D.MaxTurnRate)
				{
					Dir = Want;
				}
				else
				{
					FVector3f Axis = FVector3f::CrossProduct(Dir, Want).GetSafeNormal();
					if (Axis.IsZero())
					{
						Axis = FVector3f::CrossProduct(Dir, FMath::Abs(Dir.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector).GetSafeNormal();
					}
					Dir = FQuat4f(Axis, D.MaxTurnRate).RotateVector(Dir);
				}
				SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, Dir.GetSafeNormal()));
			}
		}
		Velocity += Dir * Speed - Velocity * D.ForwardDamping;
		if (D.SpecialSpeedTime > 0 && Frame < M.FrameAtLaunch + uint32(D.SpecialSpeedTime))
		{
			// The lift-off: "newPos.z = m_heightAtLaunch + (sqr(accelFactor * timeFrac) / accelFactor) * m_specialSpeedHeight"
			const float TimeFrac = float(Frame - M.FrameAtLaunch) / float(D.SpecialSpeedTime);
			const float AccelFactor = FMath::Max(D.SpecialAccelFactor, 0.01f);
			const float NewZ = M.HeightAtLaunch + (FMath::Square(AccelFactor * TimeFrac) / AccelFactor) * D.SpecialSpeedHeight;
			Velocity = FVector3f(0.f, 0.f, NewZ - Position.Z);
		}
		Position += Velocity;
	}
	else
	{
		return;
	}

	if (M.NoTurnDistLeft > 0.f && bOldPosValid)
	{
		M.NoTurnDistLeft -= (Position - OldPos).Size();
	}

	// Into the ground (NeutronMissileUpdate::projectileHandleCollision), or down to the height it goes off at
	// (HeightDieUpdate): it dies, and its death is the blast.
	const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
	bool bDie = M.bArmed && Position.Z <= Ground;
	if (!bDie && Template->bHeightDie && Frame >= HeightDieArmedFrame)
	{
		bDie = Position.Z - Ground <= Template->HeightDieTarget && (!Template->bHeightDieOnlyWhenMovingDown || Position.Z < OldPos.Z);
	}
	if (bDie)
	{
		if (Template->bHeightDieSnapToGround || Position.Z < Ground)
		{
			Position.Z = Ground;
		}
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [nuke f%u] #%u %s goes off at (%.0f, %.0f)"), Frame, ID, *Template->Name, Position.X, Position.Y);
		M.bActive = false; // (dies like anything else from here)
		M.State = EGenMissileState::Dead;
		bHidden = true;
		Velocity = FVector3f::ZeroVector;
		Kill();
	}
}

void FGenObject::UpdateNeutronBlast(uint32 Frame)
{
	// NeutronMissileSlowDeathBehavior::update
	const FGenNeutronBlast& B = Template->NeutronBlast;
	if (NeutronBlastFrame == 0)
	{
		NeutronBlastFrame = Frame;
		const FVector3f GroundPos(Position.X, Position.Y, Logic->GetGroundHeight(Position.X, Position.Y));
		Logic->DoFXPos(B.FXList, GroundPos, nullptr, 0.f, nullptr, 0.f);
	}
	const int32 Elapsed = int32(Frame - NeutronBlastFrame);
	TArray<FGenObjectID, TInlineAllocator<64>> InRange;
	for (int32 i = 0; i < UE_ARRAY_COUNT(B.Blasts); ++i)
	{
		const FGenNeutronBlast::FBlast& Blast = B.Blasts[i];
		if (!Blast.bEnabled)
		{
			continue;
		}
		const uint16 Bit = uint16(1u << i);
		const bool bBlastNow = (NeutronBlastsDone & Bit) == 0 && float(Elapsed) > Blast.Delay;
		const bool bScorchNow = (NeutronScorchDone & Bit) == 0 && float(Elapsed) > Blast.ScorchDelay;
		if ((!bBlastNow && !bScorchNow) || Blast.OuterRadius <= 0.f)
		{
			NeutronBlastsDone |= bBlastNow ? Bit : 0;
			NeutronScorchDone |= bScorchNow ? Bit : 0;
			continue;
		}
		// (Who is in reach is taken down first: what dies leaves wrecks and rubble behind.)
		InRange.Reset();
		Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), Blast.OuterRadius, [&](FGenObject& Other)
		{
			if (&Other != this && !Other.bPendingDestroy)
			{
				InRange.Add(Other.GetID());
			}
		});
		if (bBlastNow)
		{
			// doBlast
			NeutronBlastsDone |= Bit;
			int32 NumHurt = 0;
			for (FGenObjectID OtherId : InRange)
			{
				FGenObject* Other = Logic->FindObject(OtherId);
				if (!Other)
				{
					continue;
				}
				const FVector3f Force = Other->Position - Position;
				if (Other->IsAbleToBeToppled())
				{
					Other->ApplyTopplingForce(FVector2f(Force.X, Force.Y), Blast.ToppleSpeed, ToppleNoBounceNoFX);
				}
				const float Dist = Force.Size();
				float Amount = Blast.MaxDamage;
				if (Dist > Blast.InnerRadius)
				{
					const float Percent = 1.f - ((Dist - Blast.InnerRadius) / (Blast.OuterRadius - Blast.InnerRadius + 0.01f));
					Amount = FMath::Max(Blast.MaxDamage * Percent, Blast.MinDamage);
				}
				if (Amount > 0.f)
				{
					if (!Other->bDead)
					{
						Other->AttemptDamage(Amount, TEXT("EXPLOSION"), TEXT("EXPLODED"), ID);
						++NumHurt;
					}
					if (!bNeutronScorchPlaced)
					{
						bNeutronScorchPlaced = true;
						Logic->AddScorch(Position, B.ScorchSize, 0);
					}
				}
				if (Other->CanMove() && !Other->bDead && Blast.PushForce > 0.f)
				{
					Other->ApplyForce(Force.GetSafeNormal() * Blast.PushForce);
				}
			}
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [nuke f%u] blast %d: %d things within %.0f, %d of them hurt (up to %.0f)"), Frame, i + 1,
				InRange.Num(), Blast.OuterRadius, NumHurt, Blast.MaxDamage);
		}
		if (bScorchNow)
		{
			// doScorchBlast
			NeutronScorchDone |= Bit;
			static const int32 Burned = GenConditions::Index(TEXT("BURNED"));
			for (FGenObjectID OtherId : InRange)
			{
				if (FGenObject* Other = Logic->FindObject(OtherId))
				{
					Other->SetModelCondition(Burned, true);
				}
			}
		}
	}
}

void FGenObject::SwitchLauncherDoor(uint8 NewState, uint32 Frame)
{
	// MissileLauncherBuildingUpdate::switchToState
	if (LauncherDoorState == NewState)
	{
		return;
	}
	const FGenMissileLauncherBuilding& D = Template->MissileLauncher;
	static const int32 Opening = GenConditions::Index(TEXT("DOOR_1_OPENING"));
	static const int32 WaitingOpen = GenConditions::Index(TEXT("DOOR_1_WAITING_OPEN"));
	static const int32 WaitingToClose = GenConditions::Index(TEXT("DOOR_1_WAITING_TO_CLOSE"));
	static const int32 Closing = GenConditions::Index(TEXT("DOOR_1_CLOSING"));
	const int32 Module = Logic->FindPowerModule(*this, D.Power);
	const uint32 ReadyFrame = Module != INDEX_NONE ? GetPowerReadyFrame(Module) : 0;
	const TCHAR* FXName = TEXT("");
	switch (NewState)
	{
	case DoorClosed:
		LauncherTimeoutFrame = 0;
		LauncherTimeoutState = DoorClosed;
		FXName = *D.ClosedFX;
		break;
	case DoorOpening:
		LauncherTimeoutFrame = ReadyFrame > 0 ? ReadyFrame - 1 : 0;
		LauncherTimeoutState = DoorOpen;
		FXName = *D.OpeningFX;
		break;
	case DoorOpen:
		LauncherTimeoutFrame = 0;
		LauncherTimeoutState = DoorOpen;
		FXName = *D.OpenFX;
		break;
	case DoorWaitingToClose:
		LauncherTimeoutFrame = Frame + uint32(D.DoorWaitOpenTime);
		LauncherTimeoutState = DoorClosing;
		FXName = *D.WaitingToCloseFX;
		break;
	case DoorClosing:
	{
		LauncherTimeoutFrame = Frame + uint32(D.DoorCloseTime);
		// (Closed again, at the latest, half way to the next time it is ready.)
		const int32 Delta = int32(ReadyFrame) - int32(Frame);
		if (Delta > 0 && LauncherTimeoutFrame > Frame + uint32(Delta / 2))
		{
			LauncherTimeoutFrame = Frame + uint32(Delta / 2);
		}
		LauncherTimeoutState = DoorClosed;
		FXName = *D.ClosingFX;
		break;
	}
	default:
		break;
	}
	LauncherDoorState = NewState;
	SetModelCondition(Opening, NewState == DoorOpening);
	SetModelCondition(WaitingOpen, NewState == DoorOpen);
	SetModelCondition(WaitingToClose, NewState == DoorWaitingToClose);
	SetModelCondition(Closing, NewState == DoorClosing);
	if (FXName[0] != 0)
	{
		Logic->DoFXPos(FXName, Position, nullptr, 0.f, nullptr, 0.f);
	}
	if (LauncherTimeoutFrame > Frame)
	{
		SetDrawAnimDuration(int32(LauncherTimeoutFrame - Frame)); // "setAnimationLoopDuration": the doors take as long as there is
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [nuke f%u] #%u %s: doors %s"), Frame, ID, *Template->Name,
		NewState == DoorClosed ? TEXT("closed") : NewState == DoorOpening ? TEXT("opening") : NewState == DoorOpen ? TEXT("open")
		: NewState == DoorWaitingToClose ? TEXT("open after the launch") : TEXT("closing"));
}

void FGenObject::UpdateMissileLauncherBuilding(uint32 Frame)
{
	// MissileLauncherBuildingUpdate::update
	const FGenMissileLauncherBuilding& D = Template->MissileLauncher;
	if (IsUnderConstruction())
	{
		return;
	}
	const int32 Module = Logic->FindPowerModule(*this, D.Power);
	if (Module == INDEX_NONE)
	{
		return;
	}
	const uint32 ReadyFrame = GetPowerReadyFrame(Module);
	const uint32 WhenToStartOpening = ReadyFrame >= uint32(D.DoorOpenTime) ? ReadyFrame - uint32(D.DoorOpenTime) : 0;
	if (LauncherTimeoutFrame != 0 && Frame > LauncherTimeoutFrame)
	{
		SwitchLauncherDoor(LauncherTimeoutState, Frame);
	}
	if (LauncherDoorState != DoorOpen && LauncherDoorState != DoorWaitingToClose && Logic->IsPowerReady(*this, Module))
	{
		SwitchLauncherDoor(DoorOpen, Frame);
	}
	else if (LauncherDoorState == DoorClosed && Frame >= WhenToStartOpening)
	{
		SwitchLauncherDoor(DoorOpening, Frame);
	}
}

void FGenObject::OnLauncherPowerFired(const FString& PowerName, uint32 Frame)
{
	// MissileLauncherBuildingUpdate::initiateIntentToDoSpecialPower
	if (Template->MissileLauncher.bValid && Template->MissileLauncher.Power.Equals(PowerName, ESearchCase::IgnoreCase))
	{
		SwitchLauncherDoor(DoorWaitingToClose, Frame);
	}
}
