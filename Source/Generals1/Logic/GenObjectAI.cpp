// Standing orders: hunt, guard, follow a route (computer players), attack-move and guard (anybody).
// Simplified from AIStates.cpp (AIHuntState, AIFollowWaypointPathState, AIAttackMoveToState) and AIGuard.cpp.

#include "Logic/GenObject.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenSkirmishAI.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

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
	GuardRadius = Radius > 0.f ? Radius : FMath::Max(GetVisionRange(), 100.f) * AI.GuardInnerModifierAI;
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
	const float Vision = FMath::Max(GetVisionRange(), 100.f);
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

bool FGenObject::AppendToRoute(const FVector2f& Point)
{
	// AIUpdateInterface::privateFollowPathAppend: "getAIStateType() == AI_FOLLOW_PATH && getGoalPathSize() > 0 &&
	// effectivelyMoving". (Between two legs - one point reached, the next not yet set off for - it is still on it.)
	if (bDead || AIMode != EGenAIMode::FollowPath || RoutePoints.Num() == 0 || (!bMoving && !IsWaitingForPath() && !RoutePoints.IsValidIndex(RouteIndex)))
	{
		return false;
	}
	RoutePoints.Add(Point);
	return true;
}

bool FGenObject::StartNextRouteLeg()
{
	// AIFollowPathState::update: on to the next point of the path, past whatever points lie within a cell of here.
	const FVector2f Here(Position.X, Position.Y);
	while (RoutePoints.IsValidIndex(RouteIndex) && FVector2f::DistSquared(RoutePoints[RouteIndex], Here) < FMath::Square(GenLogic::PathfindCellSize))
	{
		++RouteIndex;
	}
	if (!RoutePoints.IsValidIndex(RouteIndex))
	{
		PathExtraDistance = 0.f;
		return false; // "reached the end of the path"
	}
	const FVector2f P = RoutePoints[RouteIndex++];
	const bool bLast = !RoutePoints.IsValidIndex(RouteIndex);
	if (bLast)
	{
		PathExtraDistance = 0.f;
	}
	else
	{
		// "We are in the middle of a path, so don't set the final goal location yet": the way goes on for
		// as far as the next point is, and then some if there is one after that.
		PathExtraDistance = FVector2f::Distance(RoutePoints[RouteIndex], P) + (RoutePoints.IsValidIndex(RouteIndex + 1) ? 4.f * GenLogic::PathfindCellSize : 0.f);
	}
	return StartPath(FVector3f(P.X, P.Y, Logic->GetGroundHeight(P.X, P.Y)), false, nullptr, false, !bLast);
}

FGenObject* FGenObject::FindEnemyNear(const FVector2f& Center, float Radius, const FGenPolygonTrigger* Area) const
{
	GEN_PERF_SCOPE("Obj.FindEnemyNear");
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	const FVector2f Self(Position.X, Position.Y);
	auto Consider = [&](FGenObject& Other)
	{
		FGenObject* O = &Other;
		if (O == this || !O->IsAttackable() || Logic->GetRelationship(Owner, O->Owner) != EGenRelationship::Enemies)
		{
			return;
		}
		const FVector2f P(O->Position.X, O->Position.Y);
		const float DistSq = FVector2f::DistSquared(P, Self);
		if (DistSq > BestDistSq || (DistSq == BestDistSq && Best && Best->ID < O->ID))
		{
			return;
		}
		if ((Radius >= 0.f && FVector2f::DistSquared(P, Center) > Radius * Radius) || (Area && !Area->Contains(P.X, P.Y)))
		{
			return;
		}
		if (O->Template->IsKindOf(TEXT("HULK")) || IgnoresAsAutoTarget(*O) || ChooseWeapon(*O) == INDEX_NONE)
		{
			return;
		}
		BestDistSq = DistSq;
		Best = O;
	};
	if (Radius >= 0.f)
	{
		// Within a circle: what is in the cells the circle covers.
		Logic->GetPartition().ForEachInRange(Center, Radius, Consider);
	}
	else
	{
		// Anywhere at all (a hunt): outward from where we are, until nothing nearer can be left.
		Logic->GetPartition().ForEachByRings(Self, -1.f, Consider, [&](float SeenRadius) { return Best && BestDistSq <= SeenRadius * SeenRadius; });
	}
	return Best;
}

void FGenObject::UpdateAIMode(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.AIMode");
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
			const FGenObject* Enemy = HasWeapons() ? FindEnemyNear(FVector2f(Position.X, Position.Y), GetVisionRange()) : nullptr;
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
			if (!StartNextRouteLeg() && !bMoving)
			{
				if (!RoutePoints.IsValidIndex(RouteIndex))
				{
					AIMode = EGenAIMode::None;
					RoutePoints.Reset();
				}
			}
		}
		break;

	default:
		break;
	}
}

// ---- DeployStyleAIUpdate: unpack before firing, pack up before moving ------------------------------

bool FGenObject::IsDeployBusy() const
{
	return Template->Deploy.bValid && (DeployState == EDeploy::Deploy || DeployState == EDeploy::Undeploy || DeployState == EDeploy::AligningTurrets);
}

bool FGenObject::IsDeployedToFire() const
{
	return !Template->Deploy.bValid || DeployState == EDeploy::ReadyToAttack;
}

void FGenObject::UpdateDeploy(uint32 Frame)
{
	// DeployStyleAIUpdate::update
	const FGenThingTemplate::FDeploy& D = Template->Deploy;
	const bool bTryingToMove = bMoving && (Path.Num() > 0 || IsWaitingForPath());
	bool bInRange = false;
	if (const FGenObject* Victim = TargetID != GenInvalidObjectID ? Logic->FindObject(TargetID) : nullptr)
	{
		const int32 Slot = ChooseWeapon(*Victim);
		if (Slot != INDEX_NONE)
		{
			const FGenWeaponTemplate& W = *Weapons[Slot].Template;
			const float Dist = AttackDistance(*Victim);
			bInRange = Dist <= GetAttackRange(W) && !(W.MinimumAttackRange > 0.f && Dist < W.MinimumAttackRange);
		}
	}
	else if (bAttackingPosition && Weapons[AttackPositionSlot].Template)
	{
		bInRange = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(AttackPosition.X, AttackPosition.Y)) <= GetAttackRange(*Weapons[AttackPositionSlot].Template);
	}
	const bool bGuardIdle = AIMode == EGenAIMode::Guard && !bTryingToMove && TargetID == GenInvalidObjectID && !bAttackingPosition;

	if (DeployDoneFrame != 0 && Frame >= DeployDoneFrame)
	{
		if (DeployState == EDeploy::Deploy) { SetDeployState(EDeploy::ReadyToAttack); }
		else if (DeployState == EDeploy::Undeploy) { SetDeployState(EDeploy::ReadyToMove); }
	}
	const int32 Turret = Template->Turrets[0].bValid ? 0 : INDEX_NONE;
	if (bInRange || bGuardIdle)
	{
		switch (DeployState)
		{
		case EDeploy::ReadyToMove: SetDeployState(EDeploy::Deploy); break;
		case EDeploy::Undeploy: if (DeployDoneFrame != 0) { SetDeployState(EDeploy::Deploy, true); } break;
		case EDeploy::AligningTurrets: SetDeployState(EDeploy::ReadyToAttack); break;
		default: break;
		}
	}
	else if (bTryingToMove)
	{
		switch (DeployState)
		{
		case EDeploy::ReadyToAttack:
			SetDeployState(Turret != INDEX_NONE && D.bTurretsMustCenter ? EDeploy::AligningTurrets : EDeploy::Undeploy);
			break;
		case EDeploy::Deploy:
			if (DeployDoneFrame != 0) { SetDeployState(EDeploy::Undeploy, true); }
			break;
		case EDeploy::AligningTurrets:
			if (Turret == INDEX_NONE || FMath::Abs(GenLogic::AngleDiff(Template->Turrets[Turret].NaturalAngle, TurretAngle[Turret])) < 0.01f)
			{
				SetDeployState(EDeploy::Undeploy);
			}
			break;
		default:
			break;
		}
	}
	if (DeployState == EDeploy::AligningTurrets && Turret != INDEX_NONE)
	{
		// TurretAI::recenterTurret: round to its natural angle before it folds away.
		const FGenTurretData& TD = Template->Turrets[Turret];
		TurretAngle[Turret] = GenLogic::NormalizeAngle(TurretAngle[Turret] + FMath::Clamp(GenLogic::AngleDiff(TD.NaturalAngle, TurretAngle[Turret]), -TD.TurnRate, TD.TurnRate));
		TurnTurretTowardsPitch(Turret, TD.NaturalPitch, 1.f);
	}
	if (D.bManualAnimations && (DeployState == EDeploy::Deploy || DeployState == EDeploy::Undeploy) && DeployDoneFrame != 0)
	{
		// ManualDeployAnimations: "draw->setAnimationFrame( totalFrames - framesLeft )" - the logic steps the
		// animation (frames of 30 a second); packing runs it backwards.
		const int32 Total = DeployState == EDeploy::Deploy ? D.UnpackFrames : D.PackFrames;
		const int32 Left = int32(DeployDoneFrame - FMath::Min(Frame, DeployDoneFrame));
		const float Scale = 30.f / float(GenLogic::GetLogicFPS());
		DrawManualFrame = float(DeployState == EDeploy::Deploy ? Total - Left : Left) * Scale;
		++DrawManualFrameVersion;
	}
}

void FGenObject::SetDeployState(EDeploy State, bool bReverse)
{
	// DeployStyleAIUpdate::setMyState
	static const int32 Packing = GenConditions::Index(TEXT("PACKING")), Unpacking = GenConditions::Index(TEXT("UNPACKING")), Deployed = GenConditions::Index(TEXT("DEPLOYED"));
	const FGenThingTemplate::FDeploy& D = Template->Deploy;
	const uint32 Now = Logic->GetFrame();
	DeployState = State;
	switch (State)
	{
	case EDeploy::Deploy:
	case EDeploy::Undeploy:
	{
		const bool bDeploy = State == EDeploy::Deploy;
		SetModelCondition(Packing, !bDeploy);
		SetModelCondition(Unpacking, bDeploy);
		if (!bDeploy)
		{
			SetModelCondition(Deployed, false);
		}
		const int32 Total = bDeploy ? D.UnpackFrames : D.PackFrames;
		if (bReverse)
		{
			// Turned round half way: as far back as it had got ("totalFrames - framesLeft").
			const int32 Left = int32(DeployDoneFrame - FMath::Min(Now, DeployDoneFrame));
			DeployDoneFrame = Now + uint32(FMath::Max(0, Total - Left));
		}
		else
		{
			DeployDoneFrame = Now + uint32(Total);
		}
		if (!D.bManualAnimations)
		{
			SetDrawAnimDuration(Total);
		}
		Logic->PlayObjectSound(*this, { bDeploy ? TEXT("Deploy") : TEXT("Undeploy") });
		break;
	}
	case EDeploy::ReadyToMove:
		DeployDoneFrame = 0;
		SetModelCondition(Packing, false);
		break;
	case EDeploy::ReadyToAttack:
		DeployDoneFrame = 0;
		SetModelCondition(Unpacking, false);
		SetModelCondition(Deployed, true);
		break;
	case EDeploy::AligningTurrets:
		DeployDoneFrame = 0;
		break;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [deploy f%u] #%u %s %s%s"), Now, ID, *Template->Name,
		State == EDeploy::Deploy ? TEXT("unpacks") : State == EDeploy::Undeploy ? TEXT("packs up") : State == EDeploy::ReadyToAttack ? TEXT("is ready to fire")
		: State == EDeploy::ReadyToMove ? TEXT("is ready to move") : TEXT("lines its turret up"), bReverse ? TEXT(" (turning back half way)") : TEXT(""));
}

// ---- AssaultTransportAIUpdate: the Troop Crawler's riders -------------------------------------------

void FGenObject::AssaultReset()
{
	AssaultMembers.Reset();
	AssaultTarget = GenInvalidObjectID;
	AssaultMoveGoal = FVector3f::ZeroVector;
	bAssaultAttackMove = false;
	bAssaultAttackObject = false;
	bAssaultNewOccupantsAreNew = false;
}

void FGenObject::AssaultRetrieveMembers()
{
	// AssaultTransportAIUpdate::retrieveMembers: everyone outside comes back aboard.
	for (const FAssaultMember& M : AssaultMembers)
	{
		FGenObject* Member = Logic->FindObject(M.ID);
		if (Member && !Member->bDead && !Member->IsContained() && Member->GetEnterTarget() != ID)
		{
			Member->CommandEnter(*this);
		}
	}
}

void FGenObject::AssaultGiveFinalOrders()
{
	// AssaultTransportAIUpdate::giveFinalOrders: with the crawler gone, those outside finish what they were sent to do.
	FGenObject* Victim = Logic->FindObject(AssaultTarget);
	for (const FAssaultMember& M : AssaultMembers)
	{
		FGenObject* Member = Logic->FindObject(M.ID);
		if (!Member || Member->bDead || Member->IsContained())
		{
			continue;
		}
		if (bAssaultAttackObject && Victim && !Victim->bDead)
		{
			Member->CommandAttack(Victim->ID);
		}
		else if (bAssaultAttackMove)
		{
			Member->CommandAttackMove(AssaultMoveGoal);
		}
	}
}

void FGenObject::UpdateAssaultTransport(uint32 Frame)
{
	// AssaultTransportAIUpdate::update
	const float HealRatio = Template->AssaultTransport.HealRatio;
	// Those who died, or whom the player has given an order of his own, are no longer in the assault.
	for (int32 i = AssaultMembers.Num() - 1; i >= 0; --i)
	{
		const FGenObject* Member = Logic->FindObject(AssaultMembers[i].ID);
		if (!Member || Member->bDead || Member->LastPlayerOrderFrame > AssaultMembers[i].JoinFrame)
		{
			AssaultMembers.RemoveAtSwap(i);
		}
	}
	// Everyone aboard is a member ("new" ones - those who came aboard since the assault began - stay in).
	for (FGenObjectID RiderId : Logic->GetRiders(*this))
	{
		if (!AssaultMembers.ContainsByPredicate([RiderId](const FAssaultMember& M) { return M.ID == RiderId; }))
		{
			FAssaultMember& M = AssaultMembers.AddDefaulted_GetRef();
			M.ID = RiderId;
			M.bNew = bAssaultNewOccupantsAreNew;
			M.JoinFrame = Frame;
		}
	}
	bAssaultNewOccupantsAreNew = true;

	// isAttackPointless: attacking with nobody to send - it gives up.
	if (TargetID != GenInvalidObjectID && !AssaultMembers.ContainsByPredicate([](const FAssaultMember& M) { return !M.bNew; }))
	{
		ClearAttack();
		CommandStop();
		return;
	}
	FGenObject* Victim = Logic->FindObject(AssaultTarget);
	if (Victim && (Victim->bDead || Victim->bPendingDestroy))
	{
		Victim = nullptr;
	}
	if (Victim)
	{
		for (const FAssaultMember& M : AssaultMembers)
		{
			FGenObject* Member = Logic->FindObject(M.ID);
			if (!Member || Member->bDead)
			{
				continue;
			}
			const bool bWounded = Member->Health / FMath::Max(Member->MaxHealth, 1.f) < HealRatio;
			if (Member->IsContained())
			{
				// Out to fight: the whole and the old hands (not who came aboard to heal or since).
				if (Member->Health >= Member->MaxHealth && !M.bNew && !Member->bWantsExit)
				{
					Member->CommandExit();
				}
			}
			else if (bWounded)
			{
				if (Member->GetEnterTarget() != ID)
				{
					Member->CommandEnter(*this); // back in to heal
				}
			}
			else if (!Member->bMoving && Member->TargetID != Victim->ID)
			{
				Member->CommandAttack(Victim->ID);
			}
		}
	}
	else if (bAssaultAttackMove && AIMode != EGenAIMode::AttackMove)
	{
		CommandAttackMove(AssaultMoveGoal);
	}
	else if (bAssaultAttackObject)
	{
		AssaultRetrieveMembers();
	}
}

// ---- RebuildHoleBehavior: a GLA building's hole --------------------------------------------------

void FGenObject::HoleNewWorkerProcess(FGenObject* ExistingWorker)
{
	// RebuildHoleBehavior::newWorkerRespawnProcess
	if (ExistingWorker)
	{
		ExistingWorker->bPendingDestroy = true;
	}
	HoleWorker = GenInvalidObjectID;
	HoleWorkerWait = Template->RebuildHole.WorkerRespawnFrames;
	bMasked = false;
}

void FGenObject::UpdateRebuildHole(uint32 Frame)
{
	// RebuildHoleBehavior::update
	const FGenThingTemplate::FRebuildHole& R = Template->RebuildHole;
	FGenObject* Worker = HoleWorker != GenInvalidObjectID ? Logic->FindObject(HoleWorker) : nullptr;
	if (HoleWorker != GenInvalidObjectID && (!Worker || Worker->bDead))
	{
		HoleNewWorkerProcess(nullptr); // the worker was killed: another one, after the wait
		Worker = nullptr;
	}
	FGenObject* Rebuilding = HoleReconstructing != GenInvalidObjectID ? Logic->FindObject(HoleReconstructing) : nullptr;
	if (HoleReconstructing != GenInvalidObjectID && (!Rebuilding || Rebuilding->bDead))
	{
		HoleNewWorkerProcess(Worker); // the site was destroyed: start over
		HoleReconstructing = GenInvalidObjectID;
		Worker = Rebuilding = nullptr;
	}
	if (!Worker && HoleWorkerWait > 0 && --HoleWorkerWait == 0 && HoleRebuildTemplate)
	{
		const FGenThingTemplate* WorkerT = FGenGameData::Get().FindTemplate(R.WorkerName);
		Worker = WorkerT ? Logic->CreateObject(WorkerT, Owner, Position, Orientation) : nullptr;
		if (Worker)
		{
			HoleWorker = Worker->ID;
			if (!Rebuilding)
			{
				Rebuilding = Logic->ConstructBuilding(Worker, *HoleRebuildTemplate, Position, Orientation, Owner, true);
			}
			else
			{
				Logic->ResumeConstruction(*Worker, *Rebuilding);
			}
			if (Rebuilding)
			{
				// Whoever was attacking the hole attacks the building going up instead.
				for (FGenObjectID Id : Logic->GetObjectIDs())
				{
					FGenObject* O = Logic->FindObject(Id);
					if (O && O->TargetID == ID)
					{
						O->TargetID = Rebuilding->ID;
					}
				}
				HoleReconstructing = Rebuilding->ID;
				Rebuilding->Producer = ID;
			}
			Worker->bUnselectable = true;
			bMasked = true;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hole f%u] #%u %s: worker #%u builds %s again (#%u)"), Frame, ID, *Template->Name, Worker->ID,
				*HoleRebuildTemplate->Name, HoleReconstructing);
		}
	}
	// "HoleHealthRegen%PerSecond" of its health back every second.
	if (Health < MaxHealth)
	{
		AttemptHealing(R.RegenPercentPerSecond / float(GenLogic::GetLogicFPS()) * MaxHealth);
	}
	if (Rebuilding && !Rebuilding->IsUnderConstruction())
	{
		// Finished: the worker and the hole are done with.
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hole f%u] #%u %s is built again: the hole is gone"), Frame, Rebuilding->ID, *Rebuilding->Template->Name);
		if (Worker)
		{
			Worker->bPendingDestroy = true;
		}
		HoleWorker = GenInvalidObjectID;
		bPendingDestroy = true;
	}
}

// ---- FireOCLAfterWeaponCooldownUpdate: the toxin truck's puddles -----------------------------------

void FGenObject::UpdateFireOCLAfterCooldown(uint32 Frame)
{
	const TArray<FGenThingTemplate::FFireOCLAfterCooldown>& Modules = Template->FireOCLAfterCooldown;
	if (CooldownOCL.Num() != Modules.Num())
	{
		CooldownOCL.SetNum(Modules.Num());
	}
	// (getCurrentWeapon: the weapon in hand - while it fires at a spot, the one it fires; a FIRE_WEAPON button locks it.)
	const int32 Current = bAttackingPosition ? AttackPositionSlot : GetCurrentWeaponSlot();
	for (int32 m = 0; m < Modules.Num(); ++m)
	{
		const FGenThingTemplate::FFireOCLAfterCooldown& D = Modules[m];
		FCooldownOCLState& S = CooldownOCL[m];
		// UpgradeMux::testUpgradeConditions: nothing it conflicts with, and (if it names any) what triggers it.
		bool bUpgradeOk = true;
		for (const FString& U : D.ConflictsWith) { bUpgradeOk = bUpgradeOk && !HasUpgrade(U); }
		if (bUpgradeOk && D.TriggeredBy.Num() > 0)
		{
			int32 NumIn = 0;
			for (const FString& U : D.TriggeredBy) { NumIn += HasUpgrade(U) ? 1 : 0; }
			bUpgradeOk = D.bRequiresAllTriggers ? NumIn == D.TriggeredBy.Num() : NumIn > 0;
		}
		const FGenWeaponInstance* WI = Weapons[D.Slot].Template ? &Weapons[D.Slot] : nullptr;
		const bool bValidThisFrame = WI && Current == D.Slot && bUpgradeOk;
		auto FireOCL = [&]()
		{
			// "seconds *= m_oclLifetimePerSecond * 0.001": the longer the spray went on, the longer it lasts.
			const float Seconds = float(Frame - S.StartFrame) / float(GenLogic::GetLogicFPS()) * D.LifetimeMsPerSecond * 0.001f;
			const int32 Life = FMath::Min(FMath::TruncToInt32(Seconds * float(GenLogic::GetLogicFPS())), D.MaxFrames);
			TArray<FGenObjectID> Made;
			Logic->DoOCL(D.OCL, *this, nullptr, &Made);
			for (FGenObjectID Id : Made)
			{
				FGenObject* O = Logic->FindObject(Id);
				if (O && Life > 0 && O->Template->bHasLifetime)
				{
					O->LifetimeDieFrame = Frame + uint32(Life); // LifetimeUpdate::setLifetimeRange( lifetimeFrames, lifetimeFrames )
				}
			}
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ocl f%u] #%u %s leaves %s after %d shots, for %d frames"), Frame, ID, *Template->Name, *D.OCL, S.Shots, Life);
			S.Shots = 0;
			S.StartFrame = 0;
		};
		if (bValidThisFrame)
		{
			if (WI->LastShotFrame != 0 && WI->LastShotFrame + 1 == Frame)
			{
				if (++S.Shots == 1)
				{
					S.StartFrame = Frame;
				}
			}
			else if (WI->ReadyFrame < Frame && D.MinShots <= S.Shots)
			{
				FireOCL(); // it has stopped: what it sprayed is left behind
			}
		}
		else if (bUpgradeOk && WI && D.MinShots <= S.Shots)
		{
			FireOCL();
		}
		if (bValidThisFrame != S.bValid)
		{
			S.bValid = bValidThisFrame;
			S.Shots = 0;
			S.StartFrame = 0;
		}
	}
}

// ---- AutoFindHealingUpdate: a computer player's hurt units go to be healed -------------------------

void FGenObject::UpdateAutoFindHealing(uint32 Frame)
{
	// AutoFindHealingUpdate::update - only for a computer player's units ("PLAYER_HUMAN: return").
	if (Frame < HealScanFrame || (Logic->GetPlayers().IsValidIndex(Owner) && Logic->GetPlayers()[Owner].bHuman))
	{
		return;
	}
	const FGenThingTemplate::FAutoFindHealing& D = Template->AutoFindHealing;
	HealScanFrame = Frame + uint32(FMath::Max(1, D.ScanFrames));
	if (Health > MaxHealth * D.NeverHeal || !IsAIIdle() || IsContained() || EnterTarget != GenInvalidObjectID)
	{
		return;
	}
	// scanClosestTarget: the nearest HEAL_PAD (a barracks) within ScanRange - one it may go into.
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), D.ScanRange, [&](FGenObject& O)
	{
		if (O.bDead || !O.IsKindOf(TEXT("HEAL_PAD")) || !Logic->CanEnterObject(*this, O, true))
		{
			return;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(Position.X, Position.Y), FVector2f(O.Position.X, O.Position.Y));
		if (DistSq <= FMath::Square(D.ScanRange) && (DistSq < BestDistSq || (DistSq == BestDistSq && Best && O.ID < Best->ID)))
		{
			Best = &O;
			BestDistSq = DistSq;
		}
	});
	if (Best)
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [heal f%u] #%u p%d %s (%.0f / %.0f) goes to be healed at #%u %s"), Frame, ID, Owner, *Template->Name,
			Health, MaxHealth, Best->ID, *Best->Template->Name);
		CommandEnter(*Best); // aiGetHealed
	}
}
