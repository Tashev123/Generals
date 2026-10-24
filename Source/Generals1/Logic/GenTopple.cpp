// Things that are knocked over by what drives into them: lamp posts, signs and fences (ToppleUpdate.cpp)
// and trees (the toppling W3DTreeBuffer.cpp does for W3DTreeDraw's trees - here they are objects like any
// other, so one piece of code does for both).

#include "Logic/GenObject.h"
#include "Core/GenPerf.h"
#include "Logic/GenConditions.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Generals1.h"

namespace
{
	// "this is our "bounce" limit -- slightly less that 90 degrees, to account for slop."
	constexpr float AngularLimit = PI / 2.f - PI / 64.f;
	constexpr float VelocityBounceLimit = 0.01f;       // "if the velocity after a bounce will be this or lower, just stop at zero"
	constexpr float VelocityBounceSoundLimit = 0.03f;  // "and if this low, then skip the bounce sound"
	constexpr float TreeRadiusApprox = 7.f;            // TREE_RADIUS_APPROX: "Value to assume for the tree radius."
	constexpr uint32 ToppleNoBounce = 1, ToppleNoFX = 2;

	/** angleClosestTo */
	float AngleClosestTo(float A1, float A2, float Desired)
	{
		A1 = GenLogic::NormalizeAngle(A1);
		A2 = GenLogic::NormalizeAngle(A2);
		return FMath::Abs(GenLogic::AngleDiff(Desired, A1)) < FMath::Abs(GenLogic::AngleDiff(Desired, A2)) ? A1 : A2;
	}
}

bool FGenObject::IsAbleToBeToppled() const
{
	return Template->Topple.bValid && ToppleState == EToppleState::Upright && !bDead && !bPendingDestroy;
}

void FGenObject::ToppleWhatIsInTheWay()
{
	// W3DTreeBuffer::unitMoved: whatever of this kind the unit has run into goes over, away from it.
	GEN_PERF_SCOPE("Obj.Topple");
	float Radius = Template->GeometryMajorRadius;
	if (Template->IsBox())
	{
		Radius = FMath::Min(Radius, Template->GeometryMinorRadius);
	}
	const float Speed = FVector2f(Velocity.X, Velocity.Y).Size();
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), Radius + TreeRadiusApprox + 10.f, [&](FGenObject& Other)
	{
		if (&Other == this || !Other.IsAbleToBeToppled())
		{
			return;
		}
		const FGenTopple& T = Other.Template->Topple;
		// (A tree is taken for so thick; anything else has to be touched.)
		const float Reach = Radius + (T.bTree ? TreeRadiusApprox : Other.Template->GeometryMajorRadius);
		if (FVector3f::DistSquared(Position, Other.Position) >= Reach * Reach || (!T.bTree && !GeometryOverlaps(Other, 1.f)))
		{
			return;
		}
		// "Give a vector with direction to thing" - and, for all but trees, "my speed".
		Other.ApplyTopplingForce(FVector2f(Other.Position.X - Position.X, Other.Position.Y - Position.Y), T.bTree ? 0.f : Speed, 0);
	});
}

void FGenObject::ApplyTopplingForce(const FVector2f& Direction, float Speed, uint32 Options)
{
	// ToppleUpdate::applyTopplingForce / W3DTreeBuffer::applyTopplingForce
	if (!IsAbleToBeToppled())
	{
		return;
	}
	const FGenTopple& T = Template->Topple;
	if (T.bKillWhenStartToppling)
	{
		Kill();
		return;
	}
	Speed = FMath::Max(Speed, T.MinimumToppleSpeed);
	ToppleDirection = Direction.GetSafeNormal();
	if (ToppleDirection.IsZero())
	{
		ToppleDirection = FVector2f(1.f, 0.f);
	}
	ToppleVelocity = Speed * T.InitialVelocityPercent;
	ToppleAcceleration = Speed * T.InitialAccelPercent;
	ToppleAccumulation = 0.f;
	ToppleState = EToppleState::Falling;
	ToppleOptions = Options;
	bFreeRotation = true;

	if (T.bTree)
	{
		Logic->DoFXPos(T.ToppleFX, Position, nullptr, 0.f, nullptr, 0.f);
		return;
	}

	// "rotate around the z-axis so that our x-axis is perpendicular to the topple direction. this is really
	// a trick to ensure that relatively planar things (eg, streetlights) fall parallel to the ground"
	const float CurAngle = GenLogic::NormalizeAngle(Orientation);
	float ToppleAngle = FMath::Atan2(ToppleDirection.Y, ToppleDirection.X);
	if (T.bLeftOrRightOnly)
	{
		// "it's a fence or such, and can only topple left or right, so pick the closest"
		ToppleAngle = AngleClosestTo(CurAngle + PI / 2.f, CurAngle - PI / 2.f, ToppleAngle);
		ToppleDirection = FVector2f(FMath::Cos(ToppleAngle), FMath::Sin(ToppleAngle));
		Logic->OnObjectDied(*this); // "go ahead and remove it from the pathfinder now, rather than waiting for the topple to finish"
	}
	const float DesiredAngle = AngleClosestTo(ToppleAngle + PI / 2.f, ToppleAngle - PI / 2.f, CurAngle);
	ToppleNumAngleDeltaX = ToppleVelocity > 1e-5f ? FMath::Clamp(FMath::FloorToInt32(AngularLimit / (ToppleVelocity * 2.f)), 1, 600) : 1;
	ToppleAngleDeltaX = GenLogic::AngleDiff(DesiredAngle, CurAngle) / float(ToppleNumAngleDeltaX);

	static const int32 Toppled = GenConditions::Index(TEXT("TOPPLED"));
	SetModelCondition(Toppled, true);
	Logic->DoFXObj(T.ToppleFX, *this, nullptr);

	// "if this is a tree, create a stump"
	if (const FGenThingTemplate* StumpT = T.StumpName.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(T.StumpName))
	{
		if (FGenObject* Stump = Logic->CreateObject(StumpT, 0, Position, Orientation))
		{
			ToppleStump = Stump->ID;
			Stump->SnapshotPrevious();
		}
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [topple f%u] #%u %s goes over (speed %.2f)"), Logic->GetFrame(), ID, *Template->Name, Speed);
}

void FGenObject::DieByToppling()
{
	// deathByToppling: "use a special "topppling" damage type here so that toppled stuff can have different
	// damage/die modules for toppled-death vs other-death"
	if (bDead || bPendingDestroy)
	{
		return;
	}
	Health = 0.f;
	if (!Template->BodyType.IsEmpty() && !Template->BodyType.Equals(TEXT("InactiveBody"), ESearchCase::IgnoreCase))
	{
		SetModelCondition(GenConditions::ReallyDamaged(), false);
		SetModelCondition(GenConditions::Damaged(), false);
		SetModelCondition(GenConditions::Rubble(), true);
	}
	Die(TEXT("TOPPLED"), GenInvalidObjectID);
}

void FGenObject::UpdateTopple(uint32 Frame)
{
	const FGenTopple& T = Template->Topple;
	if (TreeSinkFramesLeft > 0)
	{
		// W3DTreeBuffer: "Toppled trees sink into the terrain & disappear"
		Position.Z -= T.SinkDistance / float(T.SinkFrames);
		if (--TreeSinkFramesLeft == 0)
		{
			bPendingDestroy = true;
		}
		return;
	}

	// ToppleUpdate::update / W3DTreeBuffer::updateTopplingTree
	if (ToppleNumAngleDeltaX > 0)
	{
		Rotation = (FQuat4f(FVector3f::UpVector, ToppleAngleDeltaX) * Rotation).GetNormalized();
		--ToppleNumAngleDeltaX;
	}
	float Step = ToppleVelocity;
	if (ToppleAccumulation + Step > AngularLimit)
	{
		Step = AngularLimit - ToppleAccumulation;
	}
	// (Pre_Rotate_X(-v * dir.y), Pre_Rotate_Y(v * dir.x): about the level axis square to the way it falls.)
	const FVector3f Axis(-ToppleDirection.Y, ToppleDirection.X, 0.f);
	Rotation = (FQuat4f(Axis, Step) * Rotation).GetNormalized();
	ToppleAccumulation += Step;

	if (ToppleAccumulation >= AngularLimit && ToppleVelocity > 0.f)
	{
		// "Hit so either bounce or stop if too little remaining velocity."
		ToppleVelocity *= -T.BounceVelocityPercent;
		if ((ToppleOptions & ToppleNoBounce) != 0 || FMath::Abs(ToppleVelocity) < VelocityBounceLimit)
		{
			ToppleVelocity = 0.f;
			ToppleState = EToppleState::Down;
			if (T.bTree)
			{
				if (T.bKillWhenToppled)
				{
					TreeSinkFramesLeft = T.SinkFrames;
				}
				return;
			}
			FGenObject* Stump = T.bKillStumpWhenToppled ? Logic->FindObject(ToppleStump) : nullptr;
			if (Logic->IsTracing())
			{
				const FVector3f Up = Rotation.RotateVector(FVector3f::UpVector);
				UE_LOG(LogGenerals, Log, TEXT("  [topple f%u] #%u %s is down: its top points (%.2f, %.2f, %.2f), fell towards (%.2f, %.2f)"), Frame, ID, *Template->Name,
					Up.X, Up.Y, Up.Z, ToppleDirection.X, ToppleDirection.Y);
			}
			if (T.bKillWhenToppled)
			{
				DieByToppling();
				if (T.bReorientToppledRubble)
				{
					// "we have a separate rubble state that needs to be upright, and centered on the new "center" pos..."
					Position = Position + Rotation.RotateVector(FVector3f(0.f, 0.f, Template->GetMaxHeightAbovePosition()));
					SetOrientation(Orientation);
					bFreeRotation = false;
				}
			}
			if (Stump)
			{
				Stump->DieByToppling();
			}
		}
		else if (FMath::Abs(ToppleVelocity) >= VelocityBounceSoundLimit && (ToppleOptions & ToppleNoFX) == 0)
		{
			// "fast enough bounce to warrant the bounce fx" (a tree's: "Kinda towards the top of the tree.")
			if (T.bTree)
			{
				Logic->DoFXPos(T.BounceFX, Position + Rotation.RotateVector(FVector3f(0.f, 0.f, 3.f * TreeRadiusApprox)), nullptr, 0.f, nullptr, 0.f);
			}
			else
			{
				Logic->DoFXObj(T.BounceFX, *this, nullptr);
			}
		}
	}
	else
	{
		ToppleVelocity += ToppleAcceleration;
	}
}
