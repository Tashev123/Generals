// Local steering for ground units - beyond the original (the user, 2026-10-27: vehicles bump into each other
// and get stuck; "ground units should move like in modern games").
//
// The original drives a unit straight along its path; when another unit is in the way it stops, waits, asks
// the other to step aside, and after a second lets the two drive through each other
// (AIUpdateInterface::processCollision / blockedBy). Here a moving unit looks a second and a half ahead every
// frame: it tries a handful of ways (as far round as its own locomotor turns in that time) at a couple of
// paces, and takes the one that keeps it going best without running into anybody - round a parked tank,
// keeping right when two meet head on, at the pace of a slower one ahead when there is no room to pass.
// Idle allies sitting in its way are asked to make way (the idea of Pathfinder::moveAllies) and come back
// afterwards; in the last cells of a move a unit eases in among parked allies instead, and the collision pass
// pushes apart whatever overlaps (UGenGameLogic::UpdateCollisions), so nobody stays wedged.
//
// Vehicles steer round vehicles and round enemy soldiers they can't run over; soldiers steer round vehicles.
// Soldiers among soldiers, and units on a docking lane or leaving a factory, move as in the original.
// Everything is worked out from where things are this frame, in the objects' order: the same in every game
// of a network match. gen.Steering 0 (a match setting) gives the original's stop and wait back.

#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenPartition.h"
#include "Logic/GenPathfinder.h"
#include "Generals1.h"

static TAutoConsoleVariable<int32> CVarGenSteering(
	TEXT("gen.Steering"), 1,
	TEXT("1: vehicles steer clear of each other and what overlaps is pushed apart (the user's modern movement). 0: the original's stop and wait."));

bool GenSteeringEnabled()
{
	return CVarGenSteering.GetValueOnAnyThread() != 0;
}

namespace
{
	constexpr float Cell = GenLogic::PathfindCellSize;
	/** How far ahead a unit looks (seconds), in how many steps. */
	constexpr float HorizonSeconds = 1.5f;
	constexpr int32 SubSteps = 6;
	/** Footprints are taken a little smaller than they are: side by side in neighbouring lanes is no collision. */
	constexpr float PredictShrink = 0.92f;
	/** The ways tried, off the way the path goes (radians): straight on, then further and further round on
	 *  each side, the right-hand one first so that a tie keeps right. */
	constexpr float Offsets[] = { 0.f, -0.35f, 0.35f, -0.7f, 0.7f, -1.1f, 1.1f, -1.6f, 1.6f };
	constexpr float Paces[] = { 1.f, 0.5f };
	/** What a choice costs. Going round: per quarter turn off the path. Going slower: standing still. Running
	 *  into somebody: in the look ahead's first step, falling away to little at its end - and in the very first
	 *  step, so much that standing is better. */
	constexpr float CostTurn = 1.f;
	constexpr float CostSlow = 1.2f;
	constexpr float CostCollide = 4.f;
	constexpr float CostImminent = 100.f;
	constexpr float CostSwitchSide = 0.15f; // round the other side from last frame's (no dithering)
	constexpr float CostChange = 0.3f;      // per quarter turn away from last frame's way (it holds a course)
	constexpr float CostLeft = 0.02f;       // a tie keeps right
	/** A unit coming our way steers too: each takes half of it (reciprocal avoidance). */
	constexpr float MovingShare = 0.5f;
	/** A parked ally in the last cells of our move, or a parked group mate: gone round if that is cheap, else
	 *  eased past (it is pushed over), never waited for. */
	constexpr float SoftShare = 0.25f;
	/** Traffic going our way only matters this many steps ahead (it keeps going). */
	constexpr int32 SameWaySteps = 3;
	/** Within this of the end of a move parked allies are soft, and nobody is asked to make way: they would only
	 *  be sent out of places of their own. */
	constexpr float PushInDist = 6.f * Cell;
	/** Frames the way straight on has to be clear before a unit that went round somebody heads back to it. */
	constexpr uint8 SteerHoldFrames = 8;

	/** A footprint: centre, forward axis, half length, half width. */
	struct FFoot
	{
		FVector2f C, U;
		float A, B;
	};

	bool Overlaps(const FFoot& P, const FFoot& Q)
	{
		// Separating axes (as FGenObject::GeometryOverlaps).
		const FVector2f PV(-P.U.Y, P.U.X), QV(-Q.U.Y, Q.U.X);
		const FVector2f D = Q.C - P.C;
		for (const FVector2f& L : { P.U, PV, Q.U, QV })
		{
			const float RP = P.A * FMath::Abs(FVector2f::DotProduct(P.U, L)) + P.B * FMath::Abs(FVector2f::DotProduct(PV, L));
			const float RQ = Q.A * FMath::Abs(FVector2f::DotProduct(Q.U, L)) + Q.B * FMath::Abs(FVector2f::DotProduct(QV, L));
			if (FMath::Abs(FVector2f::DotProduct(D, L)) > RP + RQ)
			{
				return false;
			}
		}
		return true;
	}

	/** (Cylinders are squares of their diameter, as in the collision pass.) */
	FFoot FootOf(const FGenThingTemplate& T, const FVector2f& At, float Angle)
	{
		const float Major = T.GeometryMajorRadius;
		return FFoot{ At, FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)), Major * PredictShrink, (T.IsBox() ? T.GeometryMinorRadius : Major) * PredictShrink };
	}

	struct FNear
	{
		FGenObject* Obj;
		FFoot Foot;          // where it is now
		FVector2f Vel;       // per frame
		float Bound;         // its footprint's circle
		bool bOverlapping;   // already in us
		bool bMoving;        // under way (not just ordered)
		bool bSameWay;       // ... going our way
		bool bSoft;          // a parked ally at the end of our move
	};

	/** Where a way taken at a pace first runs into somebody: when (frames; 0: closing in on one we are already
	 *  in; FLT_MAX: not within the look ahead) and whom (INDEX_NONE: onto ground we can't cross). Soft ones
	 *  are noted apart and looked past. */
	struct FHit
	{
		float T = FLT_MAX;
		int32 Who = INDEX_NONE;
		float SoftT = FLT_MAX;
	};
}

bool FGenObject::UsesSteering() const
{
	return !bFlies && !bDirectMove && Locomotor != nullptr && GenSteeringEnabled(); // (on its way to a dock too: only its lane is driven straight)
}

bool FGenObject::SteersAround(const FGenObject& O) const
{
	if (&O == this || !O.IsGroundUnit() || O.ID == IgnoreObstacle || O.ID == ContactTarget || O.ID == EnterTarget)
	{
		return false;
	}
	// One on a bridge and one under it don't meet.
	if (O.Layer != Layer && FMath::Abs(O.Position.Z - Position.Z) > 10.f)
	{
		return false;
	}
	const bool bInfantry = Template->bKindInfantry, bOtherInfantry = O.Template->bKindInfantry;
	if (bInfantry && bOtherInfantry)
	{
		return false; // soldiers among soldiers: as in the original
	}
	if (!bInfantry && bOtherInfantry && Logic->GetRelationship(Owner, O.Owner) == EGenRelationship::Allies)
	{
		return false; // our soldiers step aside for us
	}
	return !CanCrushOrSquish(O); // what we drive over is no obstacle
}

FGenObject::FSteerChoice FGenObject::Steer(const FVector2f& Toward, float DistToWaypoint, float DistToGoal, float PreferredSpeed, uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Steer");
	FSteerChoice Out;
	const FVector2f Pos(Position.X, Position.Y);
	const FVector2f ToPoint = Toward - Pos;
	const float ToPointLen = ToPoint.Size();
	Out.Dir = ToPointLen > 0.001f ? ToPoint / ToPointLen : GetDirection2D();
	Out.Speed = PreferredSpeed;
	if (ToPointLen < 0.001f || PreferredSpeed <= 0.f || IgnoreCollisionsUntil > Frame)
	{
		SteerSide = 0;
		SteerHold = 0;
		return Out;
	}

	const float FPS = float(GenLogic::GetLogicFPS());
	const float Frames = HorizonSeconds * FPS;
	const float Dt = Frames / float(SubSteps);
	const float Reach = PreferredSpeed * Frames;
	// (No further than a little past the point we make for: the path turns there.)
	const float MaxTravel = FMath::Min(Reach, DistToWaypoint + 2.f * Cell);
	const FFoot Me0 = FootOf(*Template, Pos, Orientation);
	const float MyBound = FMath::Sqrt(FMath::Square(Me0.A) + FMath::Square(Me0.B));
	const bool bPushingIn = DistToGoal < PushInDist;

	// Who is near enough to matter within the look ahead.
	TArray<FNear, TInlineAllocator<32>> Near;
	Logic->GetPartition().ForEachInRange(Pos, MyBound + 30.f + 2.f * Reach, [&](FGenObject& O)
	{
		if (!SteersAround(O))
		{
			return;
		}
		const FVector2f OPos(O.Position.X, O.Position.Y);
		const FVector2f OVel(O.Velocity.X, O.Velocity.Y);
		const FFoot Foot = FootOf(*O.Template, OPos, O.Orientation);
		const float Bound = FMath::Sqrt(FMath::Square(Foot.A) + FMath::Square(Foot.B));
		if (FVector2f::Distance(Pos, OPos) - MyBound - Bound > MaxTravel + OVel.Size() * Frames)
		{
			return; // too far off to meet, whatever either of us does
		}
		const bool bOMoving = OVel.SizeSquared() > FMath::Square(0.05f);
		const bool bSameWay = bOMoving && FVector2f::DotProduct(OVel.GetSafeNormal(), Out.Dir) > 0.5f;
		// (A parked group mate - it got there first - is soft all the way: it is not sent out of its place for us.)
		const bool bSoft = !O.bMoving && (bPushingIn || (MoveGroup != 0 && O.MoveGroup == MoveGroup)) && O.CanBePushed()
			&& Logic->GetRelationship(Owner, O.Owner) == EGenRelationship::Allies;
		Near.Add(FNear{ &O, Foot, OVel, Bound, Overlaps(Me0, Foot), bOMoving, bSameWay, bSoft });
	});
	if (Near.Num() == 0)
	{
		SteerSide = 0;
		SteerHold = 0;
		return Out;
	}

	const float WantAngle = FMath::Atan2(Out.Dir.Y, Out.Dir.X);
	const float TurnRate = Locomotor->TurnRate;
	const float AccelRate = FMath::Max(Locomotor->Acceleration, 0.001f);
	const float Brake = GetBraking();
	const float V0 = FMath::Max(GetForwardSpeed2D(), 0.f);
	const EGenLocoAppearance Look = Locomotor->Appearance;
	const bool bWheels = Look == EGenLocoAppearance::FourWheels || Look == EGenLocoAppearance::Motorcycle;
	const bool bSlowsToTurn = Look == EGenLocoAppearance::Treads || Look == EGenLocoAppearance::TwoLegs;
	const float TurnSpeed = FMath::Max(Locomotor->MinTurnSpeed, Locomotor->Speed / 4.f); // (as MoveWheels)
	const FGenPathfinder& PF = Logic->GetPathfinder();
	const uint32 Surfaces = GetPathSurfaces();
	const int32 NextLayer = PathLayers.IsValidIndex(PathIndex) ? int32(PathLayers[PathIndex]) : int32(Layer);
	// (Standing somewhere no path goes - just out of a factory, say: the ground doesn't rule out any way.)
	const bool bCheckGround = PF.IsPassable(Pos, Surfaces, Layer) || PF.IsPassable(Pos, Surfaces, NextLayer);

	// As the locomotors drive: treads and legs slow down the more they have to turn, wheels turn only as fast
	// as they roll.
	auto Evaluate = [&](float Heading, float Pace, bool bGround) -> FHit
	{
		FHit Hit;
		FVector2f P = Pos;
		float Angle = Orientation, V = V0, Travel = 0.f;
		for (int32 k = 1; k <= SubSteps; ++k)
		{
			const float Rel = GenLogic::AngleDiff(Heading, Angle);
			float Goal = Pace;
			if (bSlowsToTurn)
			{
				Goal *= 1.f - FMath::Min(FMath::Abs(Rel) / (PI / 4.f), 1.f);
			}
			else if (bWheels && FMath::Abs(Rel) > PI / 20.f)
			{
				Goal = FMath::Min(Goal, TurnSpeed);
			}
			const float NewV = Goal > V ? FMath::Min(Goal, V + AccelRate * Dt) : FMath::Max(Goal, V - Brake * Dt);
			const float TurnFactor = bWheels ? FMath::Min(0.5f * (V + NewV) / FMath::Max(TurnSpeed, 0.001f), 1.f) : 1.f;
			Angle += FMath::Clamp(Rel, -TurnRate * TurnFactor * Dt, TurnRate * TurnFactor * Dt);
			const float Step = FMath::Min(0.5f * (V + NewV) * Dt, MaxTravel - Travel);
			V = NewV;
			const FVector2f U(FMath::Cos(Angle), FMath::Sin(Angle));
			if (Step > 0.f)
			{
				P += U * Step;
				Travel += Step;
			}
			const float T = float(k) * Dt;
			const FFoot Me{ P, U, Me0.A, Me0.B };
			for (int32 n = 0; n < Near.Num(); ++n)
			{
				const FNear& N = Near[n];
				if ((N.bSameWay && k > SameWaySteps) || (N.bSoft && Hit.SoftT < FLT_MAX))
				{
					continue;
				}
				const FVector2f NC = N.Foot.C + N.Vel * T;
				bool bHit;
				if (N.bOverlapping)
				{
					// Already in each other (the collision pass is pushing us apart): only closing in counts.
					bHit = k == 1 && FVector2f::DistSquared(P, NC) < FVector2f::DistSquared(Pos, N.Foot.C) - 0.01f;
				}
				else
				{
					bHit = FVector2f::DistSquared(P, NC) <= FMath::Square(MyBound + N.Bound) && Overlaps(Me, FFoot{ NC, N.Foot.U, N.Foot.A, N.Foot.B });
				}
				if (!bHit)
				{
					continue;
				}
				const float When = N.bOverlapping ? 0.f : T;
				if (N.bSoft)
				{
					Hit.SoftT = When;
					continue;
				}
				Hit.T = When;
				Hit.Who = n;
				return Hit;
			}
			if (bGround && Step > 0.f && !PF.IsPassable(P, Surfaces, Layer) && !PF.IsPassable(P, Surfaces, NextLayer))
			{
				Hit.T = T;
				return Hit;
			}
		}
		return Hit;
	};
	auto Penalty = [&](float When, float Share)
	{
		const float Soon = FMath::Clamp(1.f - (When - Dt) / Frames, 0.f, 1.f);
		return Share * CostCollide * FMath::Square(Soon);
	};
	auto Cost = [&](float Offset, float Pace, const FHit& Hit) -> float
	{
		float C = CostTurn * FMath::Abs(Offset) / UE_HALF_PI + CostSlow * (1.f - Pace / PreferredSpeed);
		if (Hit.T < FLT_MAX)
		{
			C += Penalty(Hit.T, Hit.Who != INDEX_NONE && Near[Hit.Who].bMoving ? MovingShare : 1.f) + (Hit.T <= Dt ? CostImminent : 0.f);
		}
		if (Hit.SoftT < FLT_MAX)
		{
			C += Penalty(Hit.SoftT, SoftShare);
		}
		if (Offset > 0.f)
		{
			C += CostLeft;
		}
		if (SteerSide != 0)
		{
			C += CostChange * FMath::Abs(GenLogic::AngleDiff(WantAngle + Offset, SteerHeading)) / UE_HALF_PI;
			if (Offset != 0.f && (Offset > 0.f ? 1 : -1) != SteerSide)
			{
				C += CostSwitchSide;
			}
		}
		return C;
	};

	// Straight on at full pace (the path's own way: the ground there is the pathfinder's business).
	const FHit Hit0 = Evaluate(WantAngle, PreferredSpeed, false);
	// Nobody in the way but parked allies to be eased past: straight on (no swerving for them), no faster than
	// they can be pushed over. After going round somebody, the way has to stay clear a few frames first (no
	// swinging back and forth across where it just comes clear).
	if (Hit0.T == FLT_MAX)
	{
		if (SteerHold > 0)
		{
			--SteerHold;
		}
		if (SteerHold == 0)
		{
			SteerSide = 0;
			if (Hit0.SoftT <= 2.f * Dt)
			{
				Out.bSteered = true;
				Out.Speed = 0.5f * PreferredSpeed;
			}
			return Out;
		}
	}

	// An idle ally sitting in our way within the next second makes way (Pathfinder::moveAllies: "Only move
	// allies"; soldiers don't move vehicles).
	if (Hit0.Who != INDEX_NONE && Hit0.T <= 4.f * Dt && !bPushingIn)
	{
		FGenObject& B = *Near[Hit0.Who].Obj;
		if (B.CanMakeWay() && !(Template->bKindInfantry && !B.Template->bKindInfantry)
			&& Logic->GetRelationship(Owner, B.Owner) == EGenRelationship::Allies)
		{
			B.MakeWayFor(*this, Out.Dir);
		}
	}

	float BestCost = Cost(0.f, PreferredSpeed, Hit0);
	float BestOffset = 0.f, BestPace = PreferredSpeed;
	FHit Best = Hit0;
	// A unit turning far round keeps turning the way it is going: no way that lies round the other side of its
	// back (choosing between the two from frame to frame, it would rock on the spot).
	const float StraightRel = GenLogic::AngleDiff(WantAngle, Orientation);
	const int32 TurnWay = Turning != 0 ? Turning : (FMath::Abs(StraightRel) > UE_HALF_PI ? (StraightRel > 0.f ? 1 : -1) : 0);
	auto Try = [&](float Offset, float Pace)
	{
		const float Rel = GenLogic::AngleDiff(WantAngle + Offset, Orientation);
		if (TurnWay != 0 && FMath::Abs(Rel) > UE_HALF_PI && (Rel > 0.f ? 1 : -1) != TurnWay)
		{
			return;
		}
		const FHit Hit = Evaluate(WantAngle + Offset, Pace, Offset != 0.f && bCheckGround);
		const float C = Cost(Offset, Pace, Hit);
		if (C < BestCost)
		{
			BestCost = C;
			BestOffset = Offset;
			BestPace = Pace;
			Best = Hit;
		}
	};
	// The pace of the unit ahead, if it goes our way (calculateMaxBlockedSpeed's idea).
	if (Hit0.Who != INDEX_NONE)
	{
		const float Ahead = FVector2f::DotProduct(Near[Hit0.Who].Vel, Out.Dir);
		if (Ahead > 0.05f * PreferredSpeed && Ahead < PreferredSpeed)
		{
			Try(0.f, Ahead * 0.95f);
		}
	}
	for (const float Offset : Offsets)
	{
		for (const float Pace : Paces)
		{
			if (Offset != 0.f || Pace != 1.f)
			{
				Try(Offset, PreferredSpeed * Pace);
			}
		}
	}
	if (SteerSide != 0)
	{
		// The way we went last frame, as it lies now.
		const float Widest = Offsets[UE_ARRAY_COUNT(Offsets) - 1];
		const float Held = FMath::Clamp(GenLogic::AngleDiff(SteerHeading, WantAngle), -Widest, Widest);
		if (Held != 0.f)
		{
			for (const float Pace : Paces)
			{
				Try(Held, PreferredSpeed * Pace);
			}
		}
	}
	Try(0.f, 0.f); // stand

	static const uint32 SteerTraceID = []() { uint32 V = 0; FParse::Value(FCommandLine::Get(), TEXT("GenSteerTrace="), V); return V; }();
	UE_CLOG(SteerTraceID == ID, LogGenerals, Log, TEXT("  [steertrace f%u] #%u steer: want %.2f near %d straight T %.0f by #%u soft %.0f -> offset %.2f pace %.3f/%.3f T %.0f soft %.0f cost %.2f"),
		Frame, ID, WantAngle, Near.Num(), Hit0.T < FLT_MAX ? Hit0.T : -1.f, Hit0.Who != INDEX_NONE ? Near[Hit0.Who].Obj->ID : 0u, Hit0.SoftT < FLT_MAX ? Hit0.SoftT : -1.f,
		BestOffset, BestPace, PreferredSpeed, Best.T < FLT_MAX ? Best.T : -1.f, Best.SoftT < FLT_MAX ? Best.SoftT : -1.f, BestCost);

	Out.bSteered = true;
	if (BestOffset != 0.f)
	{
		SteerSide = BestOffset > 0.f ? 1 : -1;
		SteerHeading = WantAngle + BestOffset;
		SteerHold = SteerHoldFrames;
	}
	if (BestPace > 0.f && Best.T > Dt)
	{
		Out.Dir = FVector2f(FMath::Cos(WantAngle + BestOffset), FMath::Sin(WantAngle + BestOffset));
		// Easing past a parked ally: no faster than it can be pushed over.
		Out.Speed = Best.SoftT <= 2.f * Dt ? FMath::Min(BestPace, 0.5f * PreferredSpeed) : BestPace;
		return Out;
	}

	// Nowhere to go for now: stand, turned towards the way on where that is free (Out.Dir is the path's way).
	Out.bWait = true;
	Out.Speed = 0.f;
	if (Hit0.Who != INDEX_NONE)
	{
		FGenObject& B = *Near[Hit0.Who].Obj;
		SteerBlocker = B.ID;
		if (!B.bMoving)
		{
			// It isn't going anywhere (not making way for us either): a path round the parked units (isBlockedAndStuck).
			bBlockedAndStuck = true;
		}
		else
		{
			// It is under way, and in ours. Each waiting on the other: the lower priority unit gets out of the way
			// (processCollision's deadlock). Wedged so that it can't (it is getting out of this one's way already,
			// or still stuck after two seconds), or held up for two seconds by one that doesn't steer (a docking
			// lane, a factory's door): the original's cheat - it drives through for a moment, and the push pass
			// shoves the other over. (Waiting behind a unit that waits on somebody else is a queue: no cheating.)
			const bool bDeadlock = B.SteerBlocker == ID && !HasHigherPathPriority(B)
				&& BlockedFrames > uint32(FPS / 2.f) && B.BlockedFrames > uint32(FPS / 2.f);
			if ((bDeadlock && ((bMovingOutOfWay && MoveOutOfWayOf == B.ID) || BlockedFrames > uint32(2.f * FPS)))
				|| (!B.UsesSteering() && BlockedFrames > uint32(2.f * FPS)))
			{
				IgnoreCollisionsUntil = Frame + uint32(1.5f * FPS);
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [steer f%u] #%u %s drives through #%u (wedged, blocked %u)"), Frame, ID, *Template->Name, B.ID, BlockedFrames);
			}
			else if (bDeadlock)
			{
				MoveAwayFromUnit(B);
			}
		}
	}
	return Out;
}

bool FGenObject::CanBePushed() const
{
	return !(bMoving && bDirectMove) && DockTarget == GenInvalidObjectID && BuildTarget == GenInvalidObjectID && RepairTarget == GenInvalidObjectID
		&& HackState == EHackState::None && !bUsingAbility && IgnoreCollisionsUntil <= Logic->GetFrame();
}

bool FGenObject::CanMakeWay() const
{
	// Idle and not busy (Pathfinder::moveAllies: "Don't move units that are attacking", "Packing or unpacking
	// objects for example"). A guard stands idle too.
	return CanMove() && !bDead && !bFlies && ContainedBy == GenInvalidObjectID && !bMoving && !IsWaitingForPath()
		&& TargetID == GenInvalidObjectID && DockTarget == GenInvalidObjectID && BuildTarget == GenInvalidObjectID
		&& RepairTarget == GenInvalidObjectID && HackState == EHackState::None && !bUsingAbility && !bHasExitRally
		&& (SupplyState == EGenSupplyState::Busy || SupplyState == EGenSupplyState::Idle); // (not a gatherer on its rounds)
}

void FGenObject::MakeWayFor(const FGenObject& Mover, const FVector2f& Along)
{
	if (bDead || !CanMove() || bFlies)
	{
		return;
	}
	// AI_MOVE_OUT_OF_THE_WAY is a temporary state: a unit that was going somewhere goes on there afterwards -
	// and, beyond the original, one that stood idle goes back to where it stood.
	// (Asked again while already making way: what comes after is still what it was.)
	const bool bWasGoing = bMoving && !IsWaitingForPath();
	const bool bResume = bMovingOutOfWay ? bHasResumeGoal : true;
	const FVector3f Resume = bMovingOutOfWay ? ResumeGoal : bWasGoing ? RequestedGoal : Position;
	if (Logic->ShouldQueuePath())
	{
		FPendingPath Request;
		Request.Kind = FPendingPath::MakeWay;
		Request.Target = Mover.ID;
		Request.Along = Along;
		Request.bResume = bResume;
		Request.ResumeGoal = Resume;
		DeferPath(Request);
		return;
	}
	StartMakeWay(Mover.ID, Along, bResume, Resume);
}

bool FGenObject::StartMakeWay(FGenObjectID MoverID, const FVector2f& Along, bool bResume, const FVector3f& Resume)
{
	// Pathfinder::getMoveAwayFromPath: the nearest free spot clear of the mover's way, off to the side we are
	// already on (dead ahead of it: away from the side it goes round).
	const FGenObject* Mover = Logic->FindObject(MoverID);
	if (!Mover || Mover->bDead || bDead || !CanMove())
	{
		return false;
	}
	const FVector2f Pos(Position.X, Position.Y), From(Mover->Position.X, Mover->Position.Y);
	FVector2f U = Along.GetSafeNormal();
	if (U.IsNearlyZero())
	{
		U = Mover->GetDirection2D();
	}
	const FVector2f N(-U.Y, U.X); // the mover's left
	const float Lateral = FVector2f::DotProduct(Pos - From, N);
	const float Side = Lateral > 0.25f * Cell ? 1.f : Lateral < -0.25f * Cell ? -1.f : (Mover->SteerSide != 0 ? -float(Mover->SteerSide) : 1.f);
	const float Clear = Mover->Template->GetBoundingCircleRadius() + Template->GetBoundingCircleRadius() + 0.5f * Cell;
	FVector2f Spot = Pos + N * (Side * Clear - Lateral);
	if (FVector2f::DotProduct(GetDirection2D(), U) > 0.3f)
	{
		Spot += U * (0.5f * Clear); // facing the mover's way: drive off forwards rather than sideways
	}
	const TFunction<bool(const FVector2f&)> Accept = [From, U, N, Clear](const FVector2f& P)
	{
		const FVector2f D = P - From;
		return FMath::Abs(FVector2f::DotProduct(D, N)) >= 0.9f * Clear || FVector2f::DotProduct(D, U) < -Clear;
	};
	if (!StartPath(FVector3f(Spot.X, Spot.Y, Position.Z), false, &Accept))
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [steer f%u] #%u %s finds no spot to make way for #%u"), Logic->GetFrame(), ID, *Template->Name, MoverID);
		return false;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [steer f%u] #%u %s makes way for #%u: (%.0f, %.0f) -> (%.0f, %.0f), then to (%.0f, %.0f)"), Logic->GetFrame(), ID,
		*Template->Name, MoverID, Pos.X, Pos.Y, Goal.X, Goal.Y, Resume.X, Resume.Y);
	bMovingOutOfWay = true;
	MoveOutOfWayOf = MoverID;
	bHasResumeGoal = bResume;
	ResumeGoal = Resume;
	return true;
}

bool FGenObject::IsPivotClear(float NewOrientation) const
{
	const FVector2f Pos(Position.X, Position.Y);
	const FFoot Now = FootOf(*Template, Pos, Orientation), Then = FootOf(*Template, Pos, NewOrientation);
	bool bClear = true;
	Logic->GetPartition().ForEachInRange(Pos, FMath::Sqrt(FMath::Square(Now.A) + FMath::Square(Now.B)) + 30.f, [&](FGenObject& O)
	{
		if (!bClear || !SteersAround(O))
		{
			return;
		}
		// (Already in each other: the collision pass sorts that out, and turning makes it no worse.)
		const FFoot Other = FootOf(*O.Template, FVector2f(O.Position.X, O.Position.Y), O.Orientation);
		if (Overlaps(Then, Other) && !Overlaps(Now, Other))
		{
			bClear = false;
		}
	});
	return bClear;
}
