// Standing orders: hunt, guard, follow a route (computer players), attack-move and guard (anybody).
// Simplified from AIStates.cpp (AIHuntState, AIFollowWaypointPathState, AIAttackMoveToState) and AIGuard.cpp.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenSkirmishAI.h"
#include "Map/GenMapData.h"

void FGenObject::CommandHunt()
{
	if (bDead || !CanMove())
	{
		return;
	}
	if (bJetAI)
	{
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::Hunt;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	StopDocking();
	AIMode = EGenAIMode::Hunt;
	NextAIModeFrame = 0;
}

void FGenObject::CommandGuard(const FVector2f& Pos, float Radius, const FGenPolygonTrigger* Area)
{
	if (bDead || !CanMove())
	{
		return;
	}
	if (bJetAI)
	{
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::Guard;
		Cmd.Pos = FVector3f(Pos.X, Pos.Y, 0.f);
		Cmd.Radius = Radius;
		Cmd.Area = Area;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	StopDocking();
	AIMode = EGenAIMode::Guard;
	GuardPos = Pos;
	GuardArea = Area;
	// AIGuardMachine::getStdGuardRange: vision range times the AI's inner guard modifier.
	const FGenAIData& AI = FGenAIData::Get();
	GuardRadius = Radius > 0.f ? Radius : FMath::Max(Template->VisionRange, 100.f) * AI.GuardInnerModifierAI;
	GuardOuterRadius = GuardRadius * (AI.GuardOuterModifierAI / FMath::Max(AI.GuardInnerModifierAI, 0.01f));
	GuardMode = EGenGuardMode::Normal;
	GuardObject = GenInvalidObjectID;
	NextAIModeFrame = 0;
	ClearAttack();
	StartPath(FVector3f(Pos.X, Pos.Y, Position.Z), false);
}

void FGenObject::CommandGuardPosition(const FVector2f& Pos, EGenGuardMode Mode, FGenObjectID InGuardObject)
{
	// AIGuardMachine with the human player's radii: the unit's vision range times GuardInnerModifierHuman
	// (attack what comes inside) and GuardOuterModifierHuman (chase it that far).
	if (bDead || !CanMove())
	{
		return;
	}
	CommandGuard(Pos, 0.f, nullptr);
	if (AIMode != EGenAIMode::Guard)
	{
		return; // (an aircraft on the ground keeps the order for when it is up)
	}
	const FGenAIData& AI = FGenAIData::Get();
	const float Vision = FMath::Max(Template->VisionRange, 100.f);
	GuardRadius = Vision * AI.GuardInnerModifierHuman;
	GuardOuterRadius = Vision * AI.GuardOuterModifierHuman;
	GuardMode = Mode;
	GuardObject = InGuardObject;
}

void FGenObject::CommandAttackMove(const FVector3f& InGoal)
{
	if (bDead || !CanMove())
	{
		return;
	}
	if (bJetAI)
	{
		// (Aircraft fly there; what they meet on the way is up to their own guard / hunt logic.)
		CommandMoveTo(InGoal);
		return;
	}
	StopDocking();
	ClearAttack();
	AIMode = EGenAIMode::AttackMove;
	AttackMoveGoal = InGoal;
	NextAIModeFrame = 0;
	StartPath(InGoal, false);
}

void FGenObject::CommandFollowPath(const TArray<FVector2f>& Points)
{
	if (bDead || !CanMove())
	{
		return;
	}
	if (bJetAI)
	{
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::FollowPath;
		Cmd.Points = Points;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	StopDocking();
	AIMode = EGenAIMode::FollowPath;
	RoutePoints = Points;
	RouteIndex = 0;
	ClearAttack();
	CommandStop();
}

FGenObject* FGenObject::FindEnemyNear(const FVector2f& Center, float Radius, const FGenPolygonTrigger* Area) const
{
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	const FVector2f Self(Position.X, Position.Y);
	for (FGenObjectID OtherId : Logic->GetObjectIDs())
	{
		FGenObject* O = Logic->FindObject(OtherId);
		if (!O || O == this || !O->IsTargetable() || Logic->GetRelationship(Owner, O->Owner) != EGenRelationship::Enemies
			|| O->Template->IsKindOf(TEXT("HULK")) || ChooseWeapon(*O) == INDEX_NONE)
		{
			continue;
		}
		const FVector2f P(O->Position.X, O->Position.Y);
		if ((Radius >= 0.f && FVector2f::DistSquared(P, Center) > Radius * Radius) || (Area && !Area->Contains(P.X, P.Y)))
		{
			continue;
		}
		const float DistSq = FVector2f::DistSquared(P, Self);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = O;
		}
	}
	return Best;
}

void FGenObject::UpdateAIMode(uint32 Frame)
{
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	switch (AIMode)
	{
	case EGenAIMode::Hunt:
		// AIHuntState: no victim -> look for the closest one, anywhere on the map, and go after it.
		if (TargetID == GenInvalidObjectID && Frame >= NextAIModeFrame)
		{
			NextAIModeFrame = Frame + FPS + (ID % 11);
			if (!HasWeapons())
			{
				break;
			}
			if (const FGenObject* Enemy = FindEnemyNear(FVector2f(Position.X, Position.Y), -1.f))
			{
				TargetID = Enemy->ID;
				bExplicitAttack = true;
				NextRepathFrame = 0;
			}
		}
		break;

	case EGenAIMode::Guard:
	{
		// AIGuardMachine: idle at the post; whatever hostile comes within the guarded radius is attacked
		// and chased a little beyond it, then it is back to the post.
		const float Outer = GuardOuterRadius;
		if (GuardObject != GenInvalidObjectID)
		{
			// Escorting: the post is wherever the guarded object is.
			const FGenObject* Guarded = Logic->FindObject(GuardObject);
			if (Guarded && !Guarded->bDead)
			{
				GuardPos = FVector2f(Guarded->Position.X, Guarded->Position.Y);
			}
			else
			{
				GuardObject = GenInvalidObjectID;
			}
		}
		if (TargetID != GenInvalidObjectID)
		{
			const FGenObject* Victim = Logic->FindObject(TargetID);
			if (Victim && GuardMode != EGenGuardMode::WithoutPursuit
				&& FVector2f::DistSquared(FVector2f(Victim->Position.X, Victim->Position.Y), GuardPos) > Outer * Outer)
			{
				ClearAttack();
				StartPath(FVector3f(GuardPos.X, GuardPos.Y, Position.Z), false);
			}
		}
		else if (Frame >= NextAIModeFrame)
		{
			NextAIModeFrame = Frame + FPS / 2 + (ID % 7);
			const FGenObject* Enemy = HasWeapons() ? FindEnemyNear(GuardPos, GuardRadius, GuardArea) : nullptr;
			if (Enemy && GuardMode == EGenGuardMode::FlyingOnly && !Enemy->IsAirborneTarget())
			{
				Enemy = nullptr;
			}
			if (Enemy)
			{
				TargetID = Enemy->ID;
				// (Without pursuit: it is fired at from the post when the weapons reach it, never chased.)
				bExplicitAttack = GuardMode != EGenGuardMode::WithoutPursuit;
				NextRepathFrame = 0;
			}
			else if (!bMoving && FVector2f::DistSquared(FVector2f(Position.X, Position.Y), GuardPos)
				> FMath::Square(GuardObject != GenInvalidObjectID ? 60.f : GuardRadius * 0.5f))
			{
				StartPath(FVector3f(GuardPos.X, GuardPos.Y, Position.Z), false); // wandered off after a chase
			}
		}
		break;
	}

	case EGenAIMode::AttackMove:
		// AIAttackMoveToState: on the way, whatever hostile comes into view is attacked; with it dealt
		// with the move goes on, and the order is done on arrival.
		if (TargetID != GenInvalidObjectID)
		{
			break; // fighting
		}
		if (Frame >= NextAIModeFrame)
		{
			NextAIModeFrame = Frame + FPS / 2 + (ID % 7);
			const FGenObject* Enemy = HasWeapons() ? FindEnemyNear(FVector2f(Position.X, Position.Y), Template->VisionRange) : nullptr;
			if (Enemy)
			{
				TargetID = Enemy->ID;
				bExplicitAttack = true;
				NextRepathFrame = 0;
				break;
			}
		}
		if (!bMoving)
		{
			if (FVector2f::DistSquared(FVector2f(Position.X, Position.Y), FVector2f(AttackMoveGoal.X, AttackMoveGoal.Y)) <= FMath::Square(30.f)
				|| !StartPath(AttackMoveGoal, false))
			{
				AIMode = EGenAIMode::None; // there (or as near as it gets)
			}
		}
		break;

	case EGenAIMode::FollowPath:
		// AIFollowWaypointPathState: one leg after the other; done (idle) after the last.
		if (!bMoving && TargetID == GenInvalidObjectID)
		{
			if (RoutePoints.IsValidIndex(RouteIndex))
			{
				const FVector2f P = RoutePoints[RouteIndex++];
				StartPath(FVector3f(P.X, P.Y, Position.Z), false);
			}
			else
			{
				AIMode = EGenAIMode::None;
				RoutePoints.Reset();
			}
		}
		break;

	default:
		break;
	}
}
