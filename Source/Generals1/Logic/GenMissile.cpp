// How a missile flies: MissileAIUpdate.cpp (launch, ignition, the attack with and without turning, the
// kill, killing itself), Locomotor::moveTowardsPositionThrust with calcDirectionToApplyThrust and
// tryToRotateVector3D, and what PhysicsBehavior::update does to something in the air.
//
// A missile leaves its launcher along the launch bone with InitialVelocity and coasts until its engine
// lights (IgnitionDelay). From then on it is armed and pushes itself along: the thrust points where it
// has to, within MaxThrustAngle of the nose, to bring the velocity round to the target - the nose follows
// the velocity at the locomotor's turn rate - but not before it has flown DistanceToTravelBeforeTurning.
// With a PreferredHeight it climbs to that height and keeps it until DistanceToTargetBeforeDiving. Within
// DistanceToTargetForLock it "locks": from there it is moved straight at the target, so that it hits.
// Out of fuel it falls; its target gone, it is taken away.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

static TAutoConsoleVariable<int32> CVarGenMissileTrace(
	TEXT("gen.MissileTrace"), 0,
	TEXT("1: with -GenTrace, every missile logs where it is, how it flies and where it pushes every half second."));

namespace
{
	/** APPROACH_HEIGHT: a shot at the ground makes for a spot this far above it until it locks. */
	constexpr float ApproachHeight = 10.f;
	constexpr float BigNumber = 99999.f;

	/** tryToRotateVector3D: Cur turned towards Goal, by MaxAngle at most. Returns the angle turned by. */
	float RotateTowards(float MaxAngle, const FVector3f& InCur, const FVector3f& InGoal, FVector3f& Out)
	{
		if (FMath::IsNearlyZero(MaxAngle))
		{
			Out = InCur;
			return 0.f;
		}
		const FVector3f Cur = InCur.GetSafeNormal();
		const FVector3f Goal = InGoal.GetSafeNormal();
		float Between = FMath::Acos(FMath::Clamp(FVector3f::DotProduct(Cur, Goal), -1.f, 1.f));
		if (FMath::Abs(Between) <= MaxAngle)
		{
			Out = Goal;
			return Between;
		}
		// "we need to rotate around the axis perpendicular to these two vecs"
		FVector3f Axis = FVector3f::CrossProduct(Cur, Goal).GetSafeNormal();
		if (Axis.IsZero())
		{
			// (Straight back: any axis across the nose will do.)
			Axis = FVector3f::CrossProduct(Cur, FMath::Abs(Cur.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector).GetSafeNormal();
		}
		Out = FQuat4f(Axis, MaxAngle).RotateVector(Cur);
		return MaxAngle;
	}

	/**
	 * calcDirectionToApplyThrust: "calculate the direction we should apply our motive force in order to
	 * minimize the angle between (our velocity) and (direction towards goalpos)", gravity counted in.
	 */
	FVector3f ThrustDirection(const FVector3f& Pos, const FVector3f& Forward, const FVector3f& Velocity, const FVector3f& GoalPos, float MaxAccel, float Gravity)
	{
		const FVector3f ToGoal = GoalPos - Pos;
		if (FMath::IsNearlyZero(ToGoal.SizeSquared()))
		{
			return Forward;
		}
		FVector3f Vel = Velocity;
		Vel.Z += Gravity;
		const float Dist = ToGoal.Size();
		const float Speed = Vel.Size();
		const float Denom = Speed * Speed - MaxAccel * MaxAccel;
		if (!FMath::IsNearlyZero(Denom))
		{
			// "solve the (greatly simplified) quadratic... choose the smallest positive t"
			float T = (Dist * (Speed + MaxAccel)) / Denom;
			const float T2 = (Dist * (Speed - MaxAccel)) / Denom;
			if (T >= 0.f || T2 >= 0.f)
			{
				if (T < 0.f || (T2 >= 0.f && T2 < T))
				{
					T = T2;
				}
				if (!FMath::IsNearlyZero(T))
				{
					const FVector3f Dir = (ToGoal / T - Vel).GetSafeNormal();
					if (!Dir.IsZero())
					{
						return Dir;
					}
				}
			}
		}
		return ToGoal / Dist; // "no (useful) solution. revert to dumb."
	}
}

void FGenObject::LaunchMissile(uint32 Frame, const FQuat4f* LaunchRotation)
{
	// MissileAIUpdate::projectileFireAtObjectOrPosition
	FGenProjectileState& M = Projectile;
	const FGenWeaponTemplate& W = *M.Weapon;
	M.bThrust = true;
	M.State = EGenMissileState::Launch;
	M.StateFrame = Frame;
	M.NoTurnDistLeft = Template->MissileInitialDist;
	M.PrevPos = Position;
	M.bArmed = false;
	M.bBraking = false;
	M.bPreciseZ = false;
	M.FuelExpires = MAX_uint32;
	M.OriginalTarget = M.End;
	M.MaxAccel = BigNumber;
	M.MaxSpeed = Locomotor->Speed;
	float InitialVelocity = Template->MissileInitialVelocity;
	if (Template->bMissileUseWeaponSpeed)
	{
		InitialVelocity = W.WeaponSpeed;
		M.MaxAccel = W.WeaponSpeed;
		M.MaxSpeed = W.WeaponSpeed;
	}
	// "Missiles do their thing by colliding with something and exploding. So they Move to the target
	// instead of Attacking the target" - a thing that is followed, or a spot.
	const FGenObject* Victim = M.Victim != GenInvalidObjectID ? Logic->FindObject(M.Victim) : nullptr;
	M.bTracking = Victim != nullptr && Template->bMissileTryToFollowTarget;
	if (!M.bTracking)
	{
		M.Victim = GenInvalidObjectID; // "Otherwise, we are just a Coord shot."
	}
	if (Template->bHeightDie)
	{
		HeightDieArmedFrame = Frame + uint32(Template->HeightDieInitialDelay);
	}

	// Out along the launch bone (or the way the launcher faces), raised towards a target that is higher up.
	FVector3f Dir = (LaunchRotation ? *LaunchRotation : Rotation).RotateVector(FVector3f::ForwardVector).GetSafeNormal();
	if (Dir.IsZero())
	{
		Dir = FVector3f(FMath::Cos(Orientation), FMath::Sin(Orientation), 0.f);
	}
	const float DeltaZ = M.End.Z - Position.Z;
	const float XYDist = FMath::Max(1.f, FVector2f(M.End.X - Position.X, M.End.Y - Position.Y).Size());
	Dir.Z += 2.f * (DeltaZ > 0.f ? DeltaZ / XYDist : 0.f);
	Dir = Dir.GetSafeNormal();
	bFreeRotation = true;
	SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, Dir));
	Velocity = InitialVelocity > 0.f ? Dir * InitialVelocity : FVector3f::ZeroVector;
	Accel = FVector3f::ZeroVector;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [missile f%u] #%u %s leaves along (%.2f, %.2f, %.2f) at %.2f a frame for (%.0f, %.0f, %.0f)%s"),
		Frame, ID, *Template->Name, Dir.X, Dir.Y, Dir.Z, InitialVelocity, M.End.X, M.End.Y, M.End.Z, M.bTracking ? TEXT(", following its target") : TEXT(""));
}

void FGenObject::DetonateMissile(uint32 Frame)
{
	// MissileAIUpdate::detonate / WeaponStore::handleProjectileDetonation: effects and damage are positional.
	FGenProjectileState& M = Projectile;
	const FGenWeaponTemplate& W = *M.Weapon;
	const FTransform3f Xf = GetTransform();
	Logic->DoFXPos(W.ProjectileDetonationFX, Position, &Xf, W.WeaponSpeed, &Position, W.PrimaryDamageRadius);
	if (!W.ProjectileDetonationOCL.IsEmpty())
	{
		Logic->DoOCL(W.ProjectileDetonationOCL, *this);
	}
	// (With the launcher gone the shot itself answers for the damage, so its player still gets the credit.)
	if (!M.bNoDamage)
	{
		Logic->DealDamage(W, Logic->FindObject(M.Launcher) ? M.Launcher : ID, M.Victim, Position, true);
	}
	// "Delay destroying the object ... to let the contrail catch up."
	bHidden = true;
	Velocity = FVector3f::ZeroVector;
	M.State = EGenMissileState::KillSelf;
	M.StateFrame = Frame;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [missile f%u] #%u %s goes off at (%.0f, %.0f, %.0f)"), Frame, ID, *Template->Name, Position.X, Position.Y, Position.Z);
}

void FGenObject::UpdateMissile(uint32 Frame, const FGenObject* Victim)
{
	GEN_PERF_SCOPE("Obj.Missile");
	FGenProjectileState& M = Projectile;
	const FGenLocomotorTemplate& L = *Locomotor;
	const FGenGameData& GD = FGenGameData::Get();
	const float Cell = GenLogic::PathfindCellSize;

	if (M.NoTurnDistLeft > 0.f && M.State >= EGenMissileState::Ignition)
	{
		M.NoTurnDistLeft -= (Position - M.PrevPos).Size();
		M.PrevPos = Position;
	}
	if (Position.Z < 0.f)
	{
		bPendingDestroy = true; // "we ended up under the world. go away."
		return;
	}

	// What it makes for: the thing it follows, or a spot (a little above it until it locks).
	const FGenObject* GoalObject = M.bTracking ? Victim : nullptr;
	FVector3f GoalPos = M.End;
	if (!GoalObject)
	{
		GoalPos = M.OriginalTarget;
		if (Template->MissileLockDistance > 0.f && M.State != EGenMissileState::Kill)
		{
			GoalPos.Z += ApproachHeight;
		}
	}
	const float GoalDist2DSq = FVector2f::DistSquared(FVector2f(Position.X, Position.Y), FVector2f(GoalPos.X, GoalPos.Y));

	auto TargetGone = [&]()
	{
		// MissileAIUpdate::airborneTargetGone: "The safest thing for me to do in this state is to just run out of gas."
		M.FuelExpires = Frame;
		M.State = EGenMissileState::KillSelf;
		M.StateFrame = Frame;
	};

	float StateMaxAccel = 0.f, StateMaxTurn = 0.f;
	switch (M.State)
	{
	case EGenMissileState::Launch:
		if (Frame - M.StateFrame < uint32(Template->MissileIgnitionDelay))
		{
			break;
		}
		[[fallthrough]];
	case EGenMissileState::Ignition:
		// doIgnitionState: the engine lights, the exhaust shows, the warhead is armed.
		StateMaxAccel = M.MaxAccel;
		Logic->DoFXObj(Template->MissileIgnitionFX, *this);
		if (!M.Weapon->ProjectileExhaust.IsEmpty())
		{
			Logic->AttachParticleSystem(M.Weapon->ProjectileExhaust, ID);
		}
		M.bArmed = true;
		M.FuelExpires = Template->MissileFuelLifetime > 0 ? Frame + uint32(Template->MissileFuelLifetime) : MAX_uint32;
		M.State = EGenMissileState::AttackNoTurn;
		M.StateFrame = Frame;
		break;

	case EGenMissileState::AttackNoTurn:
	case EGenMissileState::Attack:
	{
		// doAttackState
		if (Frame >= M.FuelExpires)
		{
			if (Template->bMissileDetonateOnNoFuel)
			{
				DetonateMissile(Frame);
				return;
			}
			// (out of fuel: no push and no steering any more)
		}
		else
		{
			StateMaxAccel = M.MaxAccel;
			StateMaxTurn = M.State == EGenMissileState::Attack ? BigNumber : 0.f;
		}
		if (Template->MissileLockDistance > 0.f)
		{
			// "Immobile or ground target. Halve the lock distance."
			const float Lock = Template->MissileLockDistance * (GoalObject ? 1.f : 0.5f);
			if (GoalDist2DSq < Lock * Lock)
			{
				M.State = EGenMissileState::Kill; // (a spot: "Change to original goal" - it is, from the next frame)
				M.StateFrame = Frame;
				break;
			}
		}
		if (L.PreferredHeight > 0.f && GoalDist2DSq < FMath::Square(Template->MissileDiveDistance))
		{
			M.bPreciseZ = true; // "close enough to the target to ignore my preferred height setting"
		}
		if (M.NoTurnDistLeft <= 0.f)
		{
			M.State = EGenMissileState::Attack;
		}
		if (M.bTracking && !GoalObject)
		{
			TargetGone();
		}
		break;
	}

	case EGenMissileState::Kill:
		// doKillState
		if (Frame >= M.FuelExpires)
		{
			if (Template->bMissileDetonateOnNoFuel)
			{
				DetonateMissile(Frame);
				return;
			}
			TargetGone(); // "killstate missiles should not be allowed to live forever"
			break;
		}
		// "Kill state uses the OBJECT_STATUS_BRAKING, which is a flag to the locomotor to 'Cheat' and move
		// directly towards the target. This ensures that the missile will hit."
		M.bBraking = true;
		StateMaxAccel = M.MaxAccel;
		StateMaxTurn = BigNumber;
		if (M.bTracking && !GoalObject)
		{
			TargetGone();
		}
		break;

	case EGenMissileState::KillSelf:
		// doKillSelfState
		if (Frame - M.StateFrame >= uint32(Template->MissileKillSelfDelay))
		{
			if (Template->bMissileDetonateCallsKill && !bDead)
			{
				M.bActive = false; // (dies like anything else from here: its Die modules run)
				bHidden = true;
				Kill();
			}
			else
			{
				bPendingDestroy = true;
			}
		}
		return;

	default:
		return;
	}

	const FVector3f OldPos = Position;
	if (M.State != EGenMissileState::KillSelf)
	{
		// ---- Locomotor::moveTowardsPositionThrust ----
		const float MaxForwardSpeed = FMath::Max(M.MaxSpeed, 0.01f);
		float DesiredSpeed = FMath::Clamp(MaxForwardSpeed, FMath::Min(L.MinSpeed, MaxForwardSpeed), MaxForwardSpeed);
		const FVector3f Forward = Rotation.RotateVector(FVector3f::ForwardVector);
		const float ActualForward = FVector3f::DotProduct(Velocity, Forward);
		const float Braking = L.Braking >= 9999.f ? 0.f : L.Braking; // (9999: none given)
		if (Braking > 0.f)
		{
			// calcSlowDownDist
			const float Delta = ActualForward - L.MinSpeed;
			const float SlowDownDist = Delta > 0.f ? (Delta * Delta) / FMath::Abs(Braking) * 0.5f : 0.f;
			if ((GoalPos - Position).Size() < SlowDownDist)
			{
				DesiredSpeed = FMath::Min(L.MinSpeed, MaxForwardSpeed);
			}
		}
		FVector3f LocalGoal = GoalPos;
		if (L.PreferredHeight != 0.f && !M.bPreciseZ)
		{
			// "If we have a preferred flight height, and we haven't been told explicitly to ignore it..."
			const float Want = L.PreferredHeight + Logic->GetFlightSurfaceHeight(Position.X, Position.Y);
			LocalGoal.Z = Position.Z + (Want - Position.Z) * L.PreferredHeightDamping;
		}
		const float SpeedDelta = DesiredSpeed - ActualForward;
		const float MaxAccel = (SpeedDelta > 0.f || Braking == 0.f) ? FMath::Min(L.Acceleration, StateMaxAccel) : -Braking;
		float MaxTurnRate = FMath::Min(L.TurnRate, StateMaxTurn);
		const FVector3f Desired = ThrustDirection(Position, Forward, Velocity, LocalGoal, MaxAccel, GD.Gravity);
		// "we might not be able to thrust in that dir, so thrust as closely as we can"
		FVector3f ThrustDir;
		const float ThrustAngle = RotateTowards(MaxTurnRate > 0.f ? L.MaxThrustAngle : 0.f, Forward, Desired, ThrustDir);
		// "note that we are trying to orient in the direction of our vel, not the dir of our thrust."
		if (!FMath::IsNearlyZero(Velocity.SizeSquared()))
		{
			FVector3f Along = Velocity;
			bool bAdjust = true;
			if (M.bBraking)
			{
				Along = GoalPos - Position; // "align to target, cause that's where we're going anyway."
				bAdjust = !FMath::IsNearlyZero(Along.SizeSquared());
				MaxTurnRate *= 3.f;
			}
			FVector3f NewDir;
			if (bAdjust && RotateTowards(MaxTurnRate, Forward, Along, NewDir) != 0.f)
			{
				SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, NewDir.GetSafeNormal()));
			}
		}
		if (SpeedDelta != 0.f || ThrustAngle != 0.f)
		{
			const float Damping = FMath::Clamp(MaxAccel / MaxForwardSpeed, 0.f, 1.f);
			Accel += ThrustDir * MaxAccel - Velocity * Damping;
			MotiveForceExpires = Frame + uint32(GenLogic::GetLogicFPS() / 3);
		}
		UE_CLOG(Logic->IsTracing() && CVarGenMissileTrace.GetValueOnAnyThread() != 0 && (Frame % 15) == 0, LogGenerals, Log,
			TEXT("  [missile f%u] #%u state %d pos (%.0f, %.0f, %.0f) vel (%.2f, %.2f, %.2f) nose (%.2f, %.2f, %.2f) wants (%.2f, %.2f, %.2f) thrust (%.2f, %.2f, %.2f) accel %.2f turn %.2f goal (%.0f, %.0f, %.0f)"),
			Frame, ID, int32(M.State), Position.X, Position.Y, Position.Z, Velocity.X, Velocity.Y, Velocity.Z, Forward.X, Forward.Y, Forward.Z,
			Desired.X, Desired.Y, Desired.Z, ThrustDir.X, ThrustDir.Y, ThrustDir.Z, MaxAccel, MaxTurnRate, LocalGoal.X, LocalGoal.Y, LocalGoal.Z);
		// "Objects that are braking don't follow the normal physics, so they end up at their destination
		// exactly": "Projectiles cheat in 3 dimensions."
		if (M.bBraking)
		{
			const FVector3f ToGoal = GoalPos - Position;
			const float Dist = ToGoal.Size();
			const float Step = FMath::Min(FMath::Max(Velocity.Size(), Cell / float(GenLogic::GetLogicFPS())), Dist);
			if (Dist > 0.001f)
			{
				Position += ToGoal * (Step / Dist);
			}
		}
	}

	// ---- PhysicsBehavior::update ----
	Accel.Z += GD.Gravity;
	const float GroundBefore = Logic->GetGroundHeight(Position.X, Position.Y);
	if (Position.Z - GroundBefore > 9.f) // isSignificantlyAboveTerrain
	{
		Accel -= Velocity * Template->AerodynamicFriction;
	}
	else if (Velocity.X != 0.f || Velocity.Y != 0.f)
	{
		// applyFrictionalForces, near the ground: no drifting sideways, and no coasting without a push.
		FVector2f Dir(Rotation.RotateVector(FVector3f::ForwardVector));
		Dir = Dir.GetSafeNormal();
		const float LateralDot = Velocity.X * (-Dir.Y) + Velocity.Y * Dir.X;
		Accel.X -= LateralFriction * LateralDot * -Dir.Y;
		Accel.Y -= LateralFriction * LateralDot * Dir.X;
		if (Frame >= MotiveForceExpires)
		{
			const float ForwardDot = Velocity.X * Dir.X + Velocity.Y * Dir.Y;
			Accel.X -= ForwardFriction * ForwardDot * Dir.X;
			Accel.Y -= ForwardFriction * ForwardDot * Dir.Y;
		}
	}
	Velocity += Accel;
	Accel = FVector3f::ZeroVector;
	if (FMath::Abs(Velocity.X) < 0.001f) { Velocity.X = 0.f; }
	if (FMath::Abs(Velocity.Y) < 0.001f) { Velocity.Y = 0.f; }
	if (FMath::Abs(Velocity.Z) < 0.001f) { Velocity.Z = 0.f; }
	if (!M.bBraking)
	{
		Position += Velocity;
	}

	// ---- what it runs into ----
	if (M.bArmed)
	{
		// The thing it is after: it goes off where it enters its shape, so the blast sits on the hull.
		// (A shot scattered to a spot near what it was aimed at still runs into that.)
		const FGenObject* Collider = GoalObject ? GoalObject
			: (Template->DumbProjectile.GarrisonHitKillCount > 0 ? Logic->FindObject(M.Aimed) : nullptr);
		if (Collider)
		{
			for (int32 i = 1; i <= 4; ++i)
			{
				const FVector3f Probe = FMath::Lerp(OldPos, Position, float(i) / 4.f);
				if (Collider->ContainsPoint(Probe))
				{
					Position = Probe;
					if (!ProjectileClearsGarrison(Collider->ID))
					{
						DetonateMissile(Frame);
					}
					return;
				}
			}
		}
		if (M.State == EGenMissileState::Kill)
		{
			// doKillState, the move over: there (a spot), or within a frame's flight of the thing.
			const float Left = (GoalPos - Position).Size();
			if (GoalObject ? Left < FMath::Max(M.MaxSpeed, 1.f) : Left <= 0.01f)
			{
				Position = GoalPos;
				if (!(GoalObject && ProjectileClearsGarrison(GoalObject->ID)))
				{
					DetonateMissile(Frame);
				}
				return;
			}
		}
	}
	// MissileAIUpdate::update: "comanche rocket pod missiles do, so this fix is needed for them to target bridges
	// correctly" - armed, it goes off on a bridge's deck it has come down through.
	if (ProjectileHitsBridgeDeck(M.bArmed))
	{
		DetonateMissile(Frame);
		return;
	}
	const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
	if (Position.Z <= Ground)
	{
		Position.Z = Ground;
		// projectileHandleCollision with the ground: "check if our warhead is 'armed' - if not, we are
		// inert"; well above the goal and far from it "we're somewhere else".
		const FVector3f Delta = Position - GoalPos;
		if (M.bArmed && !(Delta.Z > Cell && Delta.Size() > 3.f * Cell))
		{
			DetonateMissile(Frame);
			return;
		}
		Velocity.Z = FMath::Max(Velocity.Z, 0.f);
	}

	// HeightDieUpdate: goes off this far above the ground (on the way down).
	if (Template->bHeightDie && Frame >= HeightDieArmedFrame)
	{
		if (Position.Z - Ground <= Template->HeightDieTarget && (!Template->bHeightDieOnlyWhenMovingDown || Position.Z < OldPos.Z))
		{
			if (Template->bHeightDieSnapToGround)
			{
				Position.Z = Ground;
			}
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [missile f%u] #%u %s is down to %.0f above the ground at (%.0f, %.0f): dies"), Frame, ID,
				*Template->Name, Template->HeightDieTarget, Position.X, Position.Y);
			M.bActive = false; // (dies like anything else from here: its Die modules run)
			Velocity = FVector3f::ZeroVector;
			Kill();
		}
	}
}
