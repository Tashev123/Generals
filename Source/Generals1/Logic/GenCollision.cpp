// Unit-vs-unit collision. As in the original, units are not shoved apart by physics ("AI objects move
// under their own initiative, not by getting bounced"): a moving unit that finds another in its way
// slows down or waits, asks it to step aside, or paths around it, and nobody ends a move on ground
// another unit holds. Ports AIUpdateInterface::processCollision / blockedBy / hasHigherPathPriority /
// calculateMaxBlockedSpeed, SquishCollide and the crush test of PhysicsBehavior::checkForOverlapCollision.

#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenPathfinder.h"

namespace
{
	constexpr float CellSize = GenLogic::PathfindCellSize;

	/** ThePartitionManager->getRelativeAngle2D: signed angle from the object's heading to a position. */
	float RelativeAngle2D(const FGenObject& Obj, const FVector3f& Pos)
	{
		return GenLogic::AngleDiff(FMath::Atan2(Pos.Y - Obj.GetPosition().Y, Pos.X - Obj.GetPosition().X), Obj.GetOrientation());
	}
}

float FGenObject::GetCollisionRadius() const
{
	return Template->IsBox() ? (Template->GeometryMajorRadius + Template->GeometryMinorRadius) * 0.5f : Template->GeometryMajorRadius;
}

bool FGenObject::CanCrushOrSquish(const FGenObject& Other) const
{
	// Object::canCrushOrSquish: friends don't crush friends; infantry (SquishCollide) is squished by any
	// crusher, vehicles only by a crusher above their crushable level.
	if (Template->CrusherLevel <= 0 || Logic->GetRelationship(Owner, Other.Owner) == EGenRelationship::Allies)
	{
		return false;
	}
	return Other.Template->bSquishable || Template->CrusherLevel > Other.Template->CrushableLevel;
}

void FGenObject::TryCrush(FGenObject& Other)
{
	// Unless I am moving right now, I may not crush anything.
	if (!CanCrushOrSquish(Other) || (FMath::Abs(Velocity.X) < 0.01f && FMath::Abs(Velocity.Y) < 0.01f))
	{
		return;
	}
	// "ok, add this to our list-of-overlapped-things" (addOverlap: the body rides up over it).
	if (Other.GetID() != CurrentOverlap)
	{
		CurrentOverlap = Other.GetID();
	}
	const FVector2f To(Other.Position.X - Position.X, Other.Position.Y - Position.Y);
	bool bCrush;
	if (Other.Template->bSquishable)
	{
		// SquishCollide: the crusher's body has to actually reach the soldier while driving towards him.
		bCrush = ContainsPoint(Other.Position + FVector3f(0.f, 0.f, 0.5f)) && (Velocity.X * To.X + Velocity.Y * To.Y) > 0.f;
	}
	else
	{
		// PhysicsBehavior::checkForOverlapCollision: a vehicle has three crush points - its middle and, half
		// its length out, its front and its back. The one whose line the crusher's course passes closest to
		// is the one it is going for ("perpsLogicallyEqual": on a tie, the nearer), and the vehicle is
		// flattened once the crusher's center has driven past that point, close by.
		const FVector2f Dir = GetDirection2D();
		const float Offset = Other.Template->GeometryMajorRadius * 0.5f;
		const FVector2f OtherDir = Other.GetDirection2D();
		const FVector2f Points[3] = { To, To + OtherDir * Offset, To - OtherDir * Offset }; // middle, front, back - from the crusher
		float Perp[3], Dist[3];
		for (int32 i = 0; i < 3; ++i)
		{
			const float Along = Points[i].X * Dir.X + Points[i].Y * Dir.Y;
			Perp[i] = (Dir * Along - Points[i]).Size();
			Dist[i] = Points[i].Size();
		}
		int32 Aim = 0;
		for (int32 i = 1; i < 3; ++i)
		{
			const bool bEqual = FMath::Abs(Perp[i] - Perp[Aim]) <= 0.15f;
			if (bEqual ? Dist[i] < Dist[Aim] : Perp[i] < Perp[Aim])
			{
				Aim = i;
			}
		}
		bCrush = (Dir.X * Points[Aim].X + Dir.Y * Points[Aim].Y) < 0.f && Points[Aim].SizeSquared() < 2.25f * Offset * Offset;
	}
	if (bCrush)
	{
		Other.CrushedBy = ID;
		Other.Kill(TEXT("CRUSHED"));
	}
}

bool FGenObject::HasHigherPathPriority(const FGenObject& Other) const
{
	// Vehicles always have priority over infantry.
	const bool bVehicle = IsKindOf(TEXT("VEHICLE")), bOtherVehicle = Other.IsKindOf(TEXT("VEHICLE"));
	if (bVehicle && Other.IsKindOf(TEXT("INFANTRY"))) { return true; }
	if (IsKindOf(TEXT("INFANTRY")) && bOtherVehicle) { return false; }

	// Otherwise the unit in front along the common direction of travel goes first.
	const FVector2f OurDir = GetDirection2D(), OtherDir = Other.GetDirection2D();
	if (FVector2f::DotProduct(OurDir, OtherDir) <= 0.f)
	{
		return ID < Other.ID;
	}
	const FVector2f ToOther(Other.Position.X - Position.X, Other.Position.Y - Position.Y);
	const float Dot = FVector2f::DotProduct(OurDir + OtherDir, ToOther);
	if (Dot > 0.f) { return false; } // the other is ahead of us
	if (Dot < 0.f) { return true; }
	return ID < Other.ID;
}

float FGenObject::CalculateMaxBlockedSpeed(const FGenObject& Other) const
{
	// The fastest we can go without running into the unit that blocks us: how fast it moves away from us.
	const FVector2f ToOther = FVector2f(Other.Position.X - Position.X, Other.Position.Y - Position.Y).GetSafeNormal();
	const float Away = FVector2f::DotProduct(ToOther, Other.GetDirection2D());
	if (Away < 0.f)
	{
		return 0.f; // it is coming at us
	}
	const float AwaySpeed = FVector2f(Other.Velocity.X, Other.Velocity.Y).Size() * Away;
	const float Towards = FVector2f::DotProduct(ToOther, GetDirection2D());
	if (Towards <= 0.f)
	{
		return CurMaxBlockedSpeed;
	}
	float MaxSpeed = AwaySpeed / Towards;
	if (Other.FormationID != 0 && FormationID == Other.FormationID)
	{
		MaxSpeed *= 0.55f; // "don't let formations crowd each other."
	}
	return FMath::Min(MaxSpeed, CurMaxBlockedSpeed);
}

bool FGenObject::NeedToRotate() const
{
	// Still turning towards the path: not stuck yet.
	if (!bMoving || !Path.IsValidIndex(PathIndex))
	{
		return false;
	}
	const FVector3f Next(Path[PathIndex].X, Path[PathIndex].Y, 0.f);
	return FMath::Abs(RelativeAngle2D(*this, Next)) > PI / 30.f;
}

bool FGenObject::BlockedBy(const FGenObject& Other) const
{
	// If we are about to arrive, don't get stuck.
	if (FMath::Abs(Goal.X - Position.X) < CellSize && FMath::Abs(Goal.Y - Position.Y) < CellSize)
	{
		return false;
	}
	if (CanCrushOrSquish(Other))
	{
		return false; // just run over them
	}
	if (bLocoBackwards)
	{
		return false;
	}
	const bool bOtherMoving = Other.bMoving;
	float DX = Position.X - Other.Position.X, DY = Position.Y - Other.Position.Y;
	const float DistSqr = DX * DX + DY * DY;
	const FVector2f OurDir = GetDirection2D(), TheirDir = Other.GetDirection2D();
	const float DirDot = FVector2f::DotProduct(OurDir, TheirDir);

	if (IsKindOf(TEXT("INFANTRY")) && Other.IsKindOf(TEXT("INFANTRY")) && DirDot <= 0.25f)
	{
		return false; // infantry crossing other infantry just passes through
	}
	if (DistSqr < CellSize * CellSize * 0.0001f)
	{
		return HasHigherPathPriority(Other); // two units on the very same spot
	}
	// Blocked for a while already: if we are merely crossing, move through.
	if (BlockedFrames > uint32(GenLogic::GetLogicFPS()) && DirDot <= 0.f)
	{
		return false;
	}

	const float CollisionAngle = RelativeAngle2D(*this, Other.Position);
	const float OtherAngle = RelativeAngle2D(Other, Position);
	float AngleLimit = PI / 4.f;
	if (FMath::Abs(CollisionAngle) > PI / 2.f)
	{
		return false; // it is behind us: we are moving away
	}
	if (!bOtherMoving)
	{
		AngleLimit *= 0.75f;
	}
	if (FMath::Abs(CollisionAngle) > AngleLimit)
	{
		if (DirDot <= 0.f)
		{
			return false;
		}
		if (bOtherMoving && FMath::Abs(OtherAngle) > AngleLimit)
		{
			// Side by side: only a problem if we are converging, and then the lower priority unit yields.
			DX += OurDir.X - TheirDir.X;
			DY += OurDir.Y - TheirDir.Y;
			if (DistSqr > DX * DX + DY * DY)
			{
				if (HasHigherPathPriority(Other)) { return false; }
			}
			else
			{
				return false; // drifting apart
			}
		}
		else
		{
			return false; // off to the side and not moving into us
		}
	}
	return !Other.bDead;
}

bool FGenObject::GeometryOverlaps(const FGenObject& Other, float Shrink) const
{
	// Separating axis test on the two footprints (cylinders are treated as squares of their diameter).
	struct FBox2 { FVector2f C, U, V; float A, B; };
	auto Make = [Shrink](const FGenObject& O)
	{
		const FVector2f U = O.GetDirection2D();
		const float Major = O.Template->GeometryMajorRadius;
		return FBox2{ FVector2f(O.Position.X, O.Position.Y), U, FVector2f(-U.Y, U.X), Major * Shrink,
			(O.Template->IsBox() ? O.Template->GeometryMinorRadius : Major) * Shrink };
	};
	const FBox2 P = Make(*this), Q = Make(Other);
	const FVector2f D = Q.C - P.C;
	for (const FVector2f& L : { P.U, P.V, Q.U, Q.V })
	{
		const float RP = P.A * FMath::Abs(FVector2f::DotProduct(P.U, L)) + P.B * FMath::Abs(FVector2f::DotProduct(P.V, L));
		const float RQ = Q.A * FMath::Abs(FVector2f::DotProduct(Q.U, L)) + Q.B * FMath::Abs(FVector2f::DotProduct(Q.V, L));
		if (FMath::Abs(FVector2f::DotProduct(D, L)) > RP + RQ)
		{
			return false;
		}
	}
	return true;
}

void FGenObject::MoveToFreeGround()
{
	// Pathfinder::adjustToPossibleDestination + aiMoveToPosition: shuffle over to the nearest free spot
	// (one we can still shoot from, if we are attacking something).
	if (!bFlies && Logic->ShouldQueuePath())
	{
		FPendingPath Request;
		Request.Kind = FPendingPath::FreeGround;
		DeferPath(Request);
		return;
	}
	const FGenObject* Target = TargetID != GenInvalidObjectID ? Logic->FindObject(TargetID) : nullptr;
	const int32 Slot = Target ? ChooseWeapon(*Target) : INDEX_NONE;
	const float Range = Slot != INDEX_NONE ? GetAttackRange(*Weapons[Slot].Template) : 0.f;
	const TFunction<bool(const FVector2f&)> Accept = [this, Target, Slot, Range](const FVector2f& P)
	{
		// Not where we are: the point is to get off this spot.
		return FVector2f::DistSquared(P, FVector2f(Position.X, Position.Y)) > CellSize * CellSize * 0.25f
			&& (Slot == INDEX_NONE || IsWithinRangeFrom(P, *Target, Range));
	};
	if (StartPath(Position, false, &Accept))
	{
		bMovingOutOfWay = true;
		bHasResumeGoal = false;
		MoveOutOfWayOf = GenInvalidObjectID;
	}
}

void FGenObject::MoveAwayFromUnit(const FGenObject& Unit)
{
	// AIUpdateInterface::privateMoveAwayFromUnit
	if (bDead || !CanMove())
	{
		return;
	}
	const uint32 Frame = Logic->GetFrame();
	if (bMovingOutOfWay && MoveOutOfWayOf == Unit.ID)
	{
		if (bBlocked)
		{
			IgnoreCollisionsUntil = Frame + uint32(GenLogic::GetLogicFPS() * 2); // already on our way: cheat for 2 seconds
		}
		return;
	}
	// Step sideways off the other unit's line of travel, to the side we are already on.
	const FVector2f Dir = Unit.GetDirection2D();
	FVector2f Perp(-Dir.Y, Dir.X);
	const FVector2f FromUnit(Position.X - Unit.Position.X, Position.Y - Unit.Position.Y);
	if (FVector2f::DotProduct(FromUnit, Perp) < 0.f)
	{
		Perp = -Perp;
	}
	const float Lateral = FMath::Abs(FVector2f::DotProduct(FromUnit, Perp));
	const float Step = FMath::Max(GetCollisionRadius() + Unit.GetCollisionRadius() + CellSize - Lateral, CellSize);
	const FVector3f Dest(Position.X + Perp.X * Step, Position.Y + Perp.Y * Step, Position.Z);

	const bool bResume = bMoving && !bMovingOutOfWay;
	const FVector3f Resume = RequestedGoal;
	if (StartPath(Dest, false))
	{
		bMovingOutOfWay = true;
		MoveOutOfWayOf = Unit.ID;
		if (bResume)
		{
			bHasResumeGoal = true;
			ResumeGoal = Resume;
		}
	}
}

void FGenObject::ProcessCollision(FGenObject& Other)
{
	// AIUpdateInterface::processCollision
	const uint32 Frame = Logic->GetFrame();
	if (IgnoreCollisionsUntil > Frame || Other.ID == IgnoreObstacle)
	{
		return;
	}
	const bool bOtherMoving = Other.bMoving;
	if (bMoving)
	{
		if (!BlockedBy(Other))
		{
			return;
		}
		bBlocked = true;
		CurMaxBlockedSpeed = FMath::Min(CurMaxBlockedSpeed, CalculateMaxBlockedSpeed(Other));

		const bool bOtherMovingAway = Other.bMovingOutOfWay && Other.MoveOutOfWayOf == ID;
		if (!bOtherMovingAway && Other.IsKindOf(TEXT("INFANTRY")) && !IsKindOf(TEXT("INFANTRY")))
		{
			// Vehicles don't queue behind infantry: the soldier steps aside, unless he is busy fighting.
			if (Other.TargetID == GenInvalidObjectID)
			{
				Other.MoveAwayFromUnit(*this);
			}
			return;
		}

		if (BlockedFrames == 0)
		{
			BlockedFrames = 1;
		}
		if (NeedToRotate())
		{
			BlockedFrames = 1; // still turning onto the path: don't count this as being held up
			return;
		}
		if (!bOtherMoving)
		{
			bBlockedAndStuck = true; // it isn't going anywhere: path around it
			return;
		}
		if (Other.BlockedBy(*this) && !Other.NeedToRotate() && !HasHigherPathPriority(Other))
		{
			MoveAwayFromUnit(Other); // deadlock: the lower priority unit gets out of the way
		}
		return;
	}

	// Neither is moving and they stand in each other: the later-created one shuffles to free ground.
	// (The original only did this for units within half a cell; the wider test also untangles units that
	// stopped side by side overlapping, e.g. after closing in on the same target.)
	if (!bOtherMoving && ID > Other.ID && GeometryOverlaps(Other, 0.85f))
	{
		MoveToFreeGround();
	}
}

void UGenGameLogic::UpdateCollisions()
{
	GEN_PERF_SCOPE("Collisions");
	// PartitionManager collision pass: find overlapping pairs of ground units with a coarse hash grid.
	constexpr float Bucket = 64.f;
	struct FEntry { FGenObject* Obj; FVector2f Pos; float Radius; };
	TArray<FEntry, TInlineAllocator<256>> Units;
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* O = Objects[Id].Get();
		if (!O->IsGroundUnit())
		{
			continue;
		}
		const FVector2f Pos(O->GetPosition().X, O->GetPosition().Y);
		// Parked units hold their cells; moving ones only their destination.
		Pathfinder.UpdateUnitPos(Id, Pos, O->GetTemplate()->GetBoundingCircleRadius(), O->IsMoving(), O->GetLayer());
		Units.Add({ O, Pos, O->GetTemplate()->GetBoundingCircleRadius() });
	}

	TMap<FIntPoint, TArray<int32, TInlineAllocator<8>>> Grid;
	for (int32 i = 0; i < Units.Num(); ++i)
	{
		if ((i & 63) == 63)
		{
			SimYield(TEXT("collisions"));
		}
		const FEntry& A = Units[i];
		const FIntPoint Cell(FMath::FloorToInt32(A.Pos.X / Bucket), FMath::FloorToInt32(A.Pos.Y / Bucket));
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const TArray<int32, TInlineAllocator<8>>* Others = Grid.Find(FIntPoint(Cell.X + DX, Cell.Y + DY));
				if (!Others)
				{
					continue;
				}
				for (int32 j : *Others)
				{
					const FEntry& B = Units[j];
					// Bounding circles first, then the actual footprints (geomCollidesWithGeom): units passing
					// alongside each other must not count as colliding.
					const float Reach = A.Radius + B.Radius;
					if (FVector2f::DistSquared(A.Pos, B.Pos) > Reach * Reach || A.Obj->IsDead() || B.Obj->IsDead()
						|| !A.Obj->GeometryOverlaps(*B.Obj, 1.f))
					{
						continue;
					}
					// One on a bridge and one under it don't meet.
					if (A.Obj->GetLayer() != B.Obj->GetLayer() && FMath::Abs(A.Obj->GetPosition().Z - B.Obj->GetPosition().Z) > 10.f)
					{
						continue;
					}
					A.Obj->TryCrush(*B.Obj);
					B.Obj->TryCrush(*A.Obj);
					if (A.Obj->IsDead() || B.Obj->IsDead())
					{
						continue;
					}
					A.Obj->ProcessCollision(*B.Obj);
					B.Obj->ProcessCollision(*A.Obj);
				}
			}
		}
		Grid.FindOrAdd(Cell).Add(i);
	}
}
