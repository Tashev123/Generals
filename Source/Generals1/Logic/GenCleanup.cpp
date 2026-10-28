// CleanupHazardUpdate and CleanupAreaPower: the US ambulance cleans up toxins, anthrax and radiation - the
// CLEANUP_HAZARD objects a toxin truck's puddles, a poison or nuclear bomb leave behind - with its hose
// (HAZARD_CLEANUP damage).
//
// By itself it sprays the nearest one within ScanRange while it has nothing else to do (idle). Sent to clean up
// round a spot (the button), it goes there and does every one within ScanRange + MaxMoveDistanceFromLocation of it,
// moving as it needs to, then comes back to the spot and is done - unless the player gives it another order first.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Generals1.h"

void FGenObject::SetCleanupAreaParameters(const FVector3f& Pos, float Range)
{
	// "Setting the move range triggers that passive conditions for it to be allowed to move from the specified
	// position." (CMD_FROM_AI: "it'll abort when other types are used".)
	CleanupMoveRange = Range;
	CleanupPos = Pos;
	CommandMoveTo(Pos);
	LastAICommandFrame = Logic->GetFrame() + 1;
}

void FGenObject::UpdateCleanupHazard(uint32 Frame)
{
	// CleanupHazardUpdate::update
	const FGenThingTemplate::FCleanupHazard& D = Template->CleanupHazard;
	if (CleanupMoveRange > 0.f && !IsAIIdle() && LastAICommandFrame < LastPlayerOrderFrame)
	{
		// "Either the player or a script gave a NEW order so abandon the cleanup area cause." (Idle meanwhile, the
		// original keeps it aiBusy for the scripts: here that is simply being idle while CleanupMoveRange holds.)
		CleanupMoveRange = 0.f;
		return;
	}
	// "Optimized firing at acquired target"
	if (CleanupNextScanFrames > 0)
	{
		--CleanupNextScanFrames;
		CleanupFireWhenReady(Frame);
		return;
	}
	CleanupNextScanFrames = D.ScanFrames;
	// "Periodic scanning (expensive)"
	if (CleanupScanClosestTarget())
	{
		CleanupFireWhenReady(Frame); // "1 frame can make a big difference so fire ASAP!"
	}
	else if (CleanupMoveRange > 0.f && IsAIIdle())
	{
		// Nothing left round the spot: back to it, and done there.
		if (FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(CleanupPos.X, CleanupPos.Y)) < 25.f)
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [cleanup f%u] #%u %s: all clean round (%.0f, %.0f)"), Frame, ID, *Template->Name, CleanupPos.X, CleanupPos.Y);
			CleanupMoveRange = 0.f; // "Abort clean area because there's nothing left to clean!"
		}
		else
		{
			CommandMoveTo(CleanupPos); // "Abort clean area AFTER we move back to our initial position!"
			LastAICommandFrame = Frame + 1;
		}
	}
}

void FGenObject::CleanupFireWhenReady(uint32 Frame)
{
	// CleanupHazardUpdate::fireWhenReady: "this code prevents the object from moving beyond range" - unless it is
	// cleaning up an area, where it may go to them.
	const FGenThingTemplate::FCleanupHazard& D = Template->CleanupHazard;
	FGenObject* Target = Logic->FindObject(CleanupTarget);
	if (Target && (Target->IsDead() || Target->IsDestroyed()))
	{
		Target = nullptr;
	}
	const FGenWeaponTemplate* W = Weapons[D.WeaponSlot].Template;
	if (Target && CleanupMoveRange == 0.f && W)
	{
		const float Dist = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Target->Position.X, Target->Position.Y));
		if (Dist < W->AttackRange)
		{
			bCleanupInRange = true;
		}
		else if (bCleanupInRange)
		{
			// "We were in range last frame, but the target has moved out of firing range, so re-evaluate by
			// forcing a new scan."
			CleanupNextScanFrames = Logic->GetRandom().RandRange(0, 3);
			CleanupTarget = GenInvalidObjectID;
			if (CleanupNextScanFrames == 0)
			{
				CleanupScanClosestTarget();
				CleanupNextScanFrames = D.ScanFrames;
				Target = nullptr; // "so we don't shoot at it (might be out of range)"
			}
		}
		else
		{
			bCleanupInRange = false;
		}
	}
	// "Fire control!" - only with nothing else to do. (An order from the AI cares nothing for whose the hazard
	// is: a force attack here. The weapon - never chosen by itself - is locked "just till the weapon is empty
	// or the attack is done".)
	if (Target && W && IsAIIdle())
	{
		CommandAttack(Target->ID, true);
		if (TargetID == Target->ID)
		{
			WeaponLockSlot = D.WeaponSlot;
			LastAICommandFrame = Frame + 1;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [cleanup f%u] #%u %s cleans up #%u %s at (%.0f, %.0f)"), Frame, ID, *Template->Name, Target->ID,
				*Target->Template->Name, Target->Position.X, Target->Position.Y);
		}
	}
}

FGenObject* FGenObject::CleanupScanClosestTarget()
{
	// CleanupHazardUpdate::scanClosestTarget: the nearest CLEANUP_HAZARD - round the spot (ScanRange plus the move
	// range) when cleaning up an area, otherwise round itself.
	const FGenThingTemplate::FCleanupHazard& D = Template->CleanupHazard;
	CleanupTarget = GenInvalidObjectID;
	const FVector2f Center = CleanupMoveRange > 0.f ? FVector2f(CleanupPos.X, CleanupPos.Y) : FVector2f(Position.X, Position.Y);
	const float Range = D.ScanRange + CleanupMoveRange;
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	Logic->GetPartition().ForEachInRange(Center, Range, [&](FGenObject& O)
	{
		if (O.bDead || O.bPendingDestroy || !O.IsKindOf(TEXT("CLEANUP_HAZARD")))
		{
			return;
		}
		const float DistSq = FVector2f::DistSquared(Center, FVector2f(O.Position.X, O.Position.Y));
		if (DistSq <= FMath::Square(Range) && (DistSq < BestDistSq || (DistSq == BestDistSq && Best && O.ID < Best->ID)))
		{
			Best = &O;
			BestDistSq = DistSq;
		}
	});
	if (Best)
	{
		CleanupTarget = Best->ID;
	}
	return Best;
}
