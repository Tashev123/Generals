// The actions of a map's scripts (ScriptActions.cpp). What is only seen or heard goes to the game thread as a
// client event (FGenMapScripts::Client); everything that changes the game is done here, inside the step.

#include "Logic/GenMapScripts.h"
#include "Logic/GenCommand.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenSkirmishAI.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Logic/GenShroud.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	const FString& Str(const TArray<FGenScriptParam>& P, int32 Index)
	{
		static const FString Empty;
		return P.IsValidIndex(Index) ? P[Index].String : Empty;
	}

	int32 Int(const TArray<FGenScriptParam>& P, int32 Index)
	{
		return P.IsValidIndex(Index) ? P[Index].Int : 0;
	}

	float Real(const TArray<FGenScriptParam>& P, int32 Index)
	{
		return P.IsValidIndex(Index) ? P[Index].Real : 0.f;
	}

	bool IsLiving(const FGenObject* O)
	{
		return O && !O->IsDead() && !O->IsDestroyed();
	}

	/** Relationship as the scripts number it (Relationship: ENEMIES 0, NEUTRAL 1, ALLIES 2). */
	EGenRelationship RelationFromInt(int32 Value)
	{
		return Value == 0 ? EGenRelationship::Enemies : (Value == 2 ? EGenRelationship::Allies : EGenRelationship::Neutral);
	}
}

// ---- helpers ------------------------------------------------------------------------------------

void FGenMapScripts::IdleUnit(FGenObject& Obj)
{
	Obj.ClearAIMode();
	Obj.ClearAttack();
	Obj.CommandIdle();
}

void FGenMapScripts::FollowWaypoints(FGenObject& Unit, const FString& Label, bool bExact, const FVector2f* From)
{
	// doNamedFollowWaypoints(Exact): from the waypoint of the path nearest the unit (or the team), along the path.
	if (!Unit.CanMove() || Unit.IsDead())
	{
		return;
	}
	const FVector2f Here = From ? *From : FVector2f(Unit.GetPosition().X, Unit.GetPosition().Y);
	const FGenMapObject* Way = GetClosestWaypointOnPath(Here, Label);
	if (!Way)
	{
		return;
	}
	TArray<FVector2f> Path;
	const int32 LoopBackTo = GetWaypointPath(*Way, Path);
	if (Path.Num() == 0)
	{
		return;
	}
	Unit.ClearAIMode();
	if (bExact)
	{
		// AIFollowWaypointPathExactState builds the whole path at once, so it only goes round it as far as the
		// points it got; a closed path is repeated there by its own list limit, not by going back.
		Unit.CommandFollowPathExact(Path);
	}
	else
	{
		Unit.CommandFollowPath(Path, LoopBackTo);
	}
	const FGenObjectID Id = Unit.GetID();
	RouteWatches.RemoveAll([Id](const FRouteWatch& W) { return W.Object == Id; });
	if (bExact || LoopBackTo < 0)
	{
		RouteWatches.Add({ Id, Label, Path.Last() });
	}
}

void FGenMapScripts::TeamFollowWaypoints(FGenMapTeam& Team, const FString& Label, bool bExact)
{
	// doTeamFollowWaypoints(Exact): the path's waypoint nearest the middle of the team.
	FVector2f Center;
	if (!GetTeamCenter(Team, Center))
	{
		return;
	}
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members);
	for (FGenObject* O : Members)
	{
		if (!O->bScriptHeld)
		{
			FollowWaypoints(*O, Label, bExact, &Center);
		}
	}
}

void FGenMapScripts::UseCommandButton(FGenObject& Obj, const FString& ButtonName, const FGenObject* Target, const FVector3f* Pos)
{
	// Object::doCommandButton / ...AtObject / ...AtPosition, for a button of the object's command set.
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenCommandButton* Button = CD.FindButton(ButtonName);
	const FGenCommandSet* Set = CD.FindSet(Obj.GetCommandSet());
	if (!Button || !Set || IsDisabledForScripts(Obj))
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] button %s on #%u %s: %s"), Frame, *ButtonName, Obj.GetID(), *Obj.GetTemplate()->Name,
			!Button ? TEXT("NO SUCH BUTTON") : (!Set ? TEXT("NO COMMAND SET") : TEXT("it is dead")));
		return;
	}
	bool bInSet = false;
	for (const FString& Slot : Set->Slots)
	{
		bInSet |= Slot.Equals(ButtonName, ESearchCase::IgnoreCase);
	}
	if (!bInSet)
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] button %s on #%u %s: NOT IN ITS COMMAND SET %s"), Frame, *ButtonName, Obj.GetID(), *Obj.GetTemplate()->Name, *Obj.GetCommandSet());
		return;
	}
	switch (Button->Command)
	{
	case EGenCommand::SpecialPower:
	{
		const int32 Module = Logic->FindPowerModule(Obj, Button->SpecialPower);
		UE_CLOG(Logic->IsTracing() && Module == INDEX_NONE, LogGenerals, Log, TEXT("  [script f%u] button %s on #%u %s: NO POWER MODULE %s"), Frame, *ButtonName, Obj.GetID(), *Obj.GetTemplate()->Name, *Button->SpecialPower);
		if (Module != INDEX_NONE && Obj.GetTemplate()->BattlePlan.bValid && Obj.GetTemplate()->BattlePlan.Power.Equals(Button->SpecialPower, ESearchCase::IgnoreCase))
		{
			// The strategy center's: the button's option says which plan (ExecBattlePlan).
			if (Logic->IsPowerReady(Obj, Module))
			{
				Obj.RequestBattlePlan(Button->CommandOption);
			}
		}
		else if (Module != INDEX_NONE)
		{
			const FVector3f At = Pos ? *Pos : (Target ? Target->GetPosition() : FVector3f(FLT_MAX, 0.f, 0.f));
			Logic->FireSpecialPower(Obj, Module, At, Target ? Target->GetID() : GenInvalidObjectID, true);
		}
		break;
	}
	case EGenCommand::FireWeapon:
		if (Button->WeaponSlot >= 0 && Button->WeaponSlot < 3 && Obj.GetWeapon(Button->WeaponSlot).Template)
		{
			Obj.ClearAIMode();
			if (Target)
			{
				Obj.CommandAttackWithSlot(Target->GetID(), Button->WeaponSlot);
			}
			else if (Pos)
			{
				Obj.CommandFireAtPosition(*Pos, Button->MaxShotsToFire, Button->WeaponSlot);
			}
		}
		break;
	case EGenCommand::CombatDrop:
		if (Target || Pos)
		{
			Obj.CommandCombatDrop(const_cast<FGenObject*>(Target), Pos ? *Pos : Target->GetPosition());
		}
		break;
	case EGenCommand::PlayerUpgrade:
	case EGenCommand::ObjectUpgrade:
		if (const FGenUpgradeTemplate* Upgrade = FGenUpgradeData::Get().FindUpgrade(Button->Upgrade))
		{
			Logic->QueueUpgrade(Obj, *Upgrade);
		}
		break;
	case EGenCommand::UnitBuild:
		if (const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(Button->Object))
		{
			Logic->QueueUnit(Obj, *Thing);
		}
		break;
	case EGenCommand::Evacuate:
		Logic->EvacuateContainer(Obj);
		break;
	case EGenCommand::DozerConstruct:
		// Object::doCommandButtonAtPosition: buildObjectNow, facing 0, with no look at the ground.
		if (const FGenThingTemplate* Thing = Pos ? FGenGameData::Get().FindTemplate(Button->Object) : nullptr)
		{
			Logic->ConstructBuilding(&Obj, *Thing, FVector3f(Pos->X, Pos->Y, Logic->GetGroundHeight(Pos->X, Pos->Y)), 0.f, Obj.GetOwner());
		}
		break;
	case EGenCommand::SwitchWeapon:
		Obj.SwitchWeapon(Button->WeaponSlot);
		break;
	case EGenCommand::HackInternet:
		if (Obj.GetTemplate()->HackInternet.bValid && !Obj.IsContained())
		{
			Obj.CommandHackInternet();
		}
		break;
	default:
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] button %s (%s) isn't ported for scripts"), Frame, *ButtonName, *Button->CommandName);
		break;
	}
}

FGenObject* FGenMapScripts::CreateObjectOnTeam(const FString& Name, const FString& Thing, const FString& TeamName, const FVector3f& Pos, float Angle)
{
	// doCreateObject / createUnitOnTeamAt: on that team (made if it has no instance), named if a name is given -
	// unless something living already has the name.
	if (!Name.IsEmpty() && IsLiving(GetUnitNamed(Name)))
	{
		return nullptr;
	}
	FGenMapTeam* Team = GetOrCreateTeamNamed(TeamName);
	const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Thing);
	if (!Team || !T)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Map scripts: can't create %s on %s"), *Thing, *TeamName);
		return nullptr;
	}
	// (doCreateObject: at the height it says - the cut scenes' flights hang their models in the air - but never under the ground.)
	FGenObject* O = Logic->CreateObject(T, Team->Player, FVector3f(Pos.X, Pos.Y, FMath::Max(Pos.Z, Logic->GetGroundHeight(Pos.X, Pos.Y))), Angle);
	if (!O)
	{
		return nullptr;
	}
	SetObjectTeam(*O, *Team);
	if (!Name.IsEmpty())
	{
		NameObject(*O, Name);
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] #%u %s '%s' created on %s (p%d) at (%.0f, %.0f)"), Frame, O->GetID(), *Thing, *Name, *TeamName,
		Team->Player, Pos.X, Pos.Y);
	return O;
}

void FGenMapScripts::CreateReinforcements(const FString& TeamName, const FString& Waypoint)
{
	// doCreateReinforcements: a new team of the definition's units at its origin (or the waypoint), in a row. With a
	// transport they ride in it: one that delivers payloads drops them over the waypoint (by its module's numbers), any
	// other lands there and lets them out (and, if the team says so, goes back where it came from).
	const int32 P = FindProto(TeamName);
	FVector3f Destination;
	if (P == INDEX_NONE || !GetWaypoint(Waypoint, Destination))
	{
		return;
	}
	const FGenTeamProto& Proto = Protos[P];
	FVector3f Origin = Destination;
	bool bMove = false;
	if (FVector3f O; !Proto.ReinforcementOrigin.IsEmpty() && GetWaypoint(Proto.ReinforcementOrigin, O))
	{
		bMove = O.X != Destination.X || O.Y != Destination.Y;
		Origin = O;
	}
	FGenMapTeam& Team = CreateTeam(P, ProtoPlayers[P]);
	Team.bCreated = true; // (Team::setActive, for its on-create script and TEAM_CREATED)
	const FGenGameData& GD = FGenGameData::Get();
	const FGenThingTemplate* TransportT = GD.FindTemplate(Proto.TransportUnitType);
	TArray<FGenObject*> Transports;
	auto MakeTransport = [&](const FVector3f& At) -> FGenObject*
	{
		FGenObject* New = Logic->CreateObject(TransportT, Team.Player, At, 0.f);
		if (New)
		{
			SetObjectTeam(*New, Team);
			Transports.Add(New);
		}
		return New;
	};
	FGenObject* Transport = TransportT ? MakeTransport(Origin) : nullptr;
	TArray<FGenObject*> Units;
	FVector3f Pos = Origin;
	for (const FGenTeamProto::FUnits& U : Proto.Units)
	{
		const FGenThingTemplate* T = GD.FindTemplate(U.Unit);
		if (!T)
		{
			continue;
		}
		FGenObject* Last = nullptr;
		for (int32 j = 0; j < U.Max; ++j)
		{
			const float X = Origin.X + 2.25f * float(j) * T->GeometryMajorRadius;
			Last = Logic->CreateObject(T, Team.Player, FVector3f(X, Pos.Y, Logic->GetGroundHeight(X, Pos.Y)), 0.f);
			if (Last)
			{
				SetObjectTeam(*Last, Team);
				Units.Add(Last);
			}
		}
		if (Last)
		{
			Pos.Y += 2.f * T->GeometryMajorRadius;
		}
	}
	if (Proto.bStartsFull)
	{
		// "Have them load into transports in the team" (first fit, for the partition solver).
		TArray<FGenObject*> Own;
		for (FGenObject* U : Units)
		{
			if (U->IsKindOf(TEXT("TRANSPORT")) && U->GetContain()) { Own.Add(U); }
		}
		for (FGenObject* U : Units)
		{
			for (FGenObject* Into : Own)
			{
				if (U != Into && !U->IsContained() && !Own.Contains(U) && Logic->IsValidContainerFor(*Into, *U, true))
				{
					Logic->AddToContain(*Into, *U);
					break;
				}
			}
		}
	}
	TArray<FGenObjectID> Movers;
	int32 TransportCount = 1;
	for (FGenObject* U : Units)
	{
		if (U->IsContained())
		{
			continue;
		}
		// (ContainModuleInterface::isValidContainerFor: what the container takes. canEnterObject is the player's question,
		// and no player can send anything into a cargo plane.)
		if (Transport && Transport->GetContain() && Logic->IsValidContainerFor(*Transport, *U, false))
		{
			if (!Logic->IsValidContainerFor(*Transport, *U, true))
			{
				// "full, try building another." (Beside the first: the original makes it at the destination.)
				FGenObject* Another = MakeTransport(Origin + FVector3f(float(TransportCount) * Transport->GetTemplate()->GeometryMajorRadius, 0.f, 0.f));
				Transport = Another ? Another : Transport;
				++TransportCount;
			}
			if (Logic->IsValidContainerFor(*Transport, *U, true))
			{
				Logic->AddToContain(*Transport, *U);
				continue;
			}
		}
		Movers.Add(U->GetID());
	}
	for (FGenObject* T : Transports)
	{
		T->ClearAIMode();
		if (T->GetTemplate()->DeliverModule.IsValid())
		{
			T->StartRiderDelivery(Destination);
		}
		else
		{
			T->CommandMoveAndEvacuate(Destination, Proto.bTransportsExit);
		}
		// (The user's word: what brings the player's reinforcements is no unit of the player's, to select and keep. The
		// original leaves it selectable, with its orders locked till it has let them out.)
		if (Logic->GetPlayers().IsValidIndex(T->GetOwner()) && Logic->GetPlayers()[T->GetOwner()].bHuman)
		{
			T->bUnselectable = true;
		}
	}
	if (Movers.Num() > 0 && (bMove || Transports.Num() > 0))
	{
		Logic->GroupMoveToPosition(Movers, Destination, false);
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] reinforcements %s: %d units in %d transports at (%.0f, %.0f) for (%.0f, %.0f)"), Frame, *TeamName, Units.Num(),
		Transports.Num(), Origin.X, Origin.Y, Destination.X, Destination.Y);
}

bool FGenMapScripts::IsDisabledForScripts(const FGenObject& Obj) const
{
	return Obj.IsDead() || Obj.IsDestroyed();
}

void FGenMapScripts::DeleteTeamMembers(FGenMapTeam& Team)
{
	// Team::deleteTeam: everything on it is removed from the game.
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members, true);
	for (FGenObject* O : Members)
	{
		Logic->RemoveObjectByScript(*O);
	}
	if (!Team.bPlayerDefault && Protos.IsValidIndex(Team.Proto) && !Protos[Team.Proto].bSingleton)
	{
		Team.bActive = false;
	}
}

// ---- actions ------------------------------------------------------------------------------------

void FGenMapScripts::ExecuteAction(const FGenScriptAction& A)
{
	const TArray<FGenScriptParam>& P = A.Params;
	const int32 FPS = GenLogic::GetLogicFPS();
	UE_CLOG(Logic->IsTracing() && A.Type != EGenScriptAction::A_SET_FLAG && A.Type != EGenScriptAction::A_SET_COUNTER, LogGenerals, Log,
		TEXT("  [script f%u]   %s(%s%s%s)"), Frame, *A.TypeName, *Str(P, 0), P.Num() > 1 ? TEXT(", ") : TEXT(""), *Str(P, 1));
	switch (A.Type)
	{
	// --- flags, counters, timers, script control (ScriptEngine) ---
	case EGenScriptAction::A_NO_OP:
		break;
	case EGenScriptAction::A_SET_FLAG:
		Flags.FindOrAdd(Str(P, 0)) = Int(P, 1) != 0;
		break;
	case EGenScriptAction::A_SET_COUNTER:
	{
		FCounter& C = Counters.FindOrAdd(Str(P, 0));
		C.Value = Int(P, 1);
		C.bCountdown = false;
		break;
	}
	case EGenScriptAction::A_INCREMENT_COUNTER:
		Counters.FindOrAdd(Str(P, 1)).Value += Int(P, 0);
		break;
	case EGenScriptAction::A_DECREMENT_COUNTER:
		Counters.FindOrAdd(Str(P, 1)).Value -= Int(P, 0);
		break;
	case EGenScriptAction::A_SET_TIMER:
	case EGenScriptAction::A_SET_RANDOM_TIMER:
	{
		FCounter& Timer = Counters.FindOrAdd(Str(P, 0));
		Timer.Value = A.Type == EGenScriptAction::A_SET_RANDOM_TIMER ? Logic->GetRandom().RandRange(Int(P, 1), FMath::Max(Int(P, 1), Int(P, 2))) : Int(P, 1);
		Timer.bCountdown = true;
		break;
	}
	case EGenScriptAction::A_SET_MILLISECOND_TIMER:
	case EGenScriptAction::A_SET_RANDOM_MSEC_TIMER:
	{
		// (Despite the name the value is in seconds.) ScriptEngine::setTimer: "ceil(seconds * LOGICFRAMES_PER_SECOND)".
		FCounter& Timer = Counters.FindOrAdd(Str(P, 0));
		const float Seconds = A.Type == EGenScriptAction::A_SET_RANDOM_MSEC_TIMER
			? Logic->GetRandom().FRandRange(Real(P, 1), FMath::Max(Real(P, 1), Real(P, 2))) : Real(P, 1);
		Timer.Value = FMath::CeilToInt32(Seconds * float(FPS));
		Timer.bCountdown = true;
		break;
	}
	case EGenScriptAction::A_ENABLE_SCRIPT:
	case EGenScriptAction::A_DISABLE_SCRIPT:
	{
		const bool bEnable = A.Type == EGenScriptAction::A_ENABLE_SCRIPT;
		if (FGenScriptGroup* G = FindGroup(Str(P, 0))) { G->bActive = bEnable; }
		if (FGenScript* S = FindScript(Str(P, 0))) { S->bActive = bEnable; }
		break;
	}
	case EGenScriptAction::A_CALL_SUBROUTINE:
		CallSubroutine(Str(P, 0));
		break;
	case EGenScriptAction::A_OBJECTLIST_ADDOBJECTTYPE:
		ObjectTypeLists.FindOrAdd(Str(P, 0)).AddUnique(Str(P, 1));
		ResolvedTypes.Remove(Str(P, 0));
		break;
	case EGenScriptAction::A_TEAM_EXECUTE_SEQUENTIAL_SCRIPT:
	case EGenScriptAction::A_TEAM_EXECUTE_SEQUENTIAL_SCRIPT_LOOPING:
		// doTeamStartSequentialScript: "Idle the team so the seq script will start executing." The looping one's count
		// is times + 1 in the data: 0 is for ever.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (const FGenScript* S = FindScript(Str(P, 1)))
			{
				TArray<FGenObject*> Members;
				GetTeamMembers(*Team, Members);
				for (FGenObject* O : Members) { if (O->CanMove()) { IdleUnit(*O); } }
				AppendSequential(Team->Id, GenInvalidObjectID, S, A.Type == EGenScriptAction::A_TEAM_EXECUTE_SEQUENTIAL_SCRIPT_LOOPING ? Int(P, 2) - 1 : 0);
			}
		}
		break;
	case EGenScriptAction::A_TEAM_STOP_SEQUENTIAL_SCRIPT:
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			const int32 Id = Team->Id;
			SequentialScripts.RemoveAll([Id](const FSequential& S) { return S.Team == Id && S.Object == GenInvalidObjectID; });
		}
		break;
	case EGenScriptAction::A_UNIT_STOP_SEQUENTIAL_SCRIPT:
		if (const FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			const FGenObjectID Id = O->GetID();
			SequentialScripts.RemoveAll([Id](const FSequential& S) { return S.Object == Id; });
		}
		break;
	case EGenScriptAction::A_TEAM_GUARD_FOR_FRAMECOUNT:
		// The dispatcher's doTeamIdleForFramecount: the team stands idle, and its sequence waits that long.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members) { if (O->CanMove()) { IdleUnit(*O); } }
			SetSequentialTimer(Team->Id, Int(P, 1));
		}
		break;
	case EGenScriptAction::A_TEAM_WAIT_FOR_NOT_CONTAINED_ALL:
		break; // ("should never be used outside of Sequential scripts")
	case EGenScriptAction::A_ADD_TO_MSEC_TIMER:
		Counters.FindOrAdd(Str(P, 1)).Value += FMath::CeilToInt32(Real(P, 0) * float(FPS));
		break;
	case EGenScriptAction::A_STOP_TIMER:
		Counters.FindOrAdd(Str(P, 0)).bCountdown = false;
		break;
	case EGenScriptAction::A_SET_ATTACK_PRIORITY_THING:
	{
		// ScriptEngine::setPriorityThing: the sets belong to the game, whichever player's script fills them.
		FGenAttackPriority& Set = Logic->FindOrAddAttackPriority(Str(P, 0));
		for (const FGenThingTemplate* T : ResolveTypes(Str(P, 1)))
		{
			Set.Things.Add(T->Canonical(), Int(P, 2));
			Set.MaxPriority = FMath::Max(Set.MaxPriority, Int(P, 2));
		}
		break;
	}
	case EGenScriptAction::A_TEAM_APPLY_ATTACK_PRIORITY_SET:
		// updateTeamAttackPrioritySet
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (const FGenAttackPriority* Set = Logic->FindAttackPriority(Str(P, 1)))
			{
				if (Protos.IsValidIndex(Team->Proto))
				{
					Protos[Team->Proto].AttackPriorityName = Set->Name;
				}
				TArray<FGenObject*> Members;
				GetTeamMembers(*Team, Members);
				for (FGenObject* O : Members) { O->AttackInfo = Set; }
			}
		}
		break;
	case EGenScriptAction::A_RECRUIT_TEAM:
	{
		// doRecruitTeam: for the player the definition belongs to.
		const int32 ProtoIndex = FindProto(Str(P, 0));
		if (FGenSkirmishAI* AI = ProtoIndex != INDEX_NONE ? Logic->GetAI(ProtoPlayers[ProtoIndex]) : nullptr)
		{
			AI->RecruitTeamFromScript(ProtoIndex, Real(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_AI_PLAYER_BUILD_UPGRADE:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (FGenSkirmishAI* AI = Player != INDEX_NONE ? Logic->GetAI(Player) : nullptr)
		{
			AI->BuildUpgradeFromScript(Str(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SELECT_SKILLSET:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (FGenSkirmishAI* AI = Player != INDEX_NONE ? Logic->GetAI(Player) : nullptr)
		{
			AI->SetSkillsetFromScript(Int(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_SET_BASE_CONSTRUCTION_SPEED:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (FGenSkirmishAI* AI = Player != INDEX_NONE ? Logic->GetAI(Player) : nullptr)
		{
			AI->SetTeamSecondsFromScript(Int(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_TEAM_DECREASE_PRIORITY:
	case EGenScriptAction::A_TEAM_INCREASE_PRIORITY:
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (Protos.IsValidIndex(Team->Proto))
			{
				FGenTeamProto& Proto = Protos[Team->Proto];
				Proto.ProductionPriority += A.Type == EGenScriptAction::A_TEAM_INCREASE_PRIORITY ? Proto.SuccessIncrease : -Proto.FailureDecrease;
			}
		}
		break;
	case EGenScriptAction::A_TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE:
	{
		// To the nearest object of that type inside the area.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 2));
		FVector2f Center;
		if (!Team || !Area || !GetTeamCenter(*Team, Center))
		{
			break;
		}
		const TArray<const FGenThingTemplate*>& Wanted = ResolveTypes(Str(P, 1));
		const FGenObject* Best = nullptr;
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (!IsLiving(O) || !Wanted.Contains(O->GetTemplate()->Canonical()) || !Area->Contains(O->GetPosition().X, O->GetPosition().Y))
			{
				continue;
			}
			const float DistSq = FVector2f::DistSquared(Center, FVector2f(O->GetPosition().X, O->GetPosition().Y));
			if (DistSq < BestDistSq)
			{
				Best = O;
				BestDistSq = DistSq;
			}
		}
		if (Best)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					O->ClearAIMode();
					O->CommandMoveTo(Best->GetPosition());
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_HUNT_WITH_COMMAND_BUTTON:
		// doTeamHuntWithCommandButton: each member uses that button's ability on whatever is nearest, again and again
		// (a unit's own ability; with any other kind of button the team simply hunts).
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			const FString Button = Str(P, 1);
			const FGenCommandButton* Btn = FGenCommandData::Get().FindButton(Button);
			const FGenSpecialPowerTemplate* Power = Btn ? FGenUpgradeData::Get().FindPower(Btn->SpecialPower) : nullptr;
			const bool bAbility = Btn && Btn->Command == EGenCommand::SpecialPower && Power && Power->bPorted && Power->AbilityKind != EGenAbilityKind::Other;
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (bAbility && O->GetTemplate()->bButtonHunt && O->FindAbility(Power->Name) != INDEX_NONE)
				{
					O->SetHuntButton(Button);
				}
				else if (O->CanMove() && O->HasWeapons())
				{
					O->CommandHunt();
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_STOP_AND_DISBAND:
		// doTeamStop(disband): they stop, and belong to the player's default team from then on.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members, true);
			const int32* DefaultId = DefaultTeams.Find(Team->Player);
			FGenMapTeam* Default = DefaultId ? FindTeam(*DefaultId) : nullptr;
			for (FGenObject* O : Members)
			{
				if (IsLiving(O) && O->CanMove()) { IdleUnit(*O); }
				if (Default && Default != Team) { SetObjectTeam(*O, *Default); }
			}
			if (Default != Team && !Team->bPlayerDefault && Protos.IsValidIndex(Team->Proto) && !Protos[Team->Proto].bSingleton)
			{
				Team->bActive = false;
			}
		}
		break;
	case EGenScriptAction::A_TEAM_GARRISON_NEAREST_BUILDING:
		// doTeamGarrisonNearestBuilding: the infantry fill the buildings nearest the team's first member (a hacker of
		// the kind that makes money looks for internet centers).
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members, Infantry;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (!O->IsContained() && O->IsKindOf(TEXT("INFANTRY")) && !O->IsKindOf(TEXT("NO_GARRISON"))) { Infantry.Add(O); }
			}
			if (Infantry.Num() == 0)
			{
				break;
			}
			const FVector3f From = Infantry[0]->GetPosition();
			const bool bHacker = Infantry[0]->IsKindOf(TEXT("MONEY_HACKER"));
			TArray<FGenObject*> Buildings;
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				FGenObject* B = Logic->FindObject(Id);
				if (IsLiving(B) && B->IsKindOf(TEXT("STRUCTURE")) && B->GetContain() && Logic->CanEnterObject(*Infantry[0], *B, true)
					&& (bHacker ? B->IsKindOf(TEXT("FS_INTERNET_CENTER")) : B->GetTemplate()->Contain.Type == EGenContainType::Garrison))
				{
					Buildings.Add(B);
				}
			}
			Buildings.StableSort([&From](const FGenObject& L, const FGenObject& R)
			{
				return (L.GetPosition() - From).SizeSquared() < (R.GetPosition() - From).SizeSquared();
			});
			int32 Next = 0;
			for (FGenObject* B : Buildings)
			{
				for (int32 Room = Logic->GetContainMax(*B) - Logic->GetContainCount(*B); Room > 0 && Next < Infantry.Num(); --Room, ++Next)
				{
					Infantry[Next]->ClearAIMode();
					Infantry[Next]->CommandEnter(*B);
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_EXIT_ALL:
		// doTeamExitAll: the team's transports and buildings empty, and members that are inside something get out.
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->IsContained()) { O->CommandExit(); }
				else if (O->GetContain()) { Logic->EvacuateContainer(*O); }
			}
		}
		break;
	case EGenScriptAction::A_TEAM_EXIT_ALL_BUILDINGS:
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members) { if (O->IsContained()) { O->CommandExit(); } }
		}
		break;
	case EGenScriptAction::A_NAMED_EXIT_BUILDING:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); IsLiving(O) && O->IsContained())
		{
			O->CommandExit();
		}
		break;
	case EGenScriptAction::A_TEAM_GUARD:
		// doTeamGuard: each where it stands.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					O->CommandGuardPosition(FVector2f(O->GetPosition().X, O->GetPosition().Y), EGenGuardMode::Normal);
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_ATTACK_NAMED:
	{
		// doTeamAttackNamed: groupAttackObject, with no limit on shots.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenObject* Target = GetUnitNamed(Str(P, 1));
		if (Team && IsLiving(Target))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->bHasAI && O->HasWeapons())
				{
					O->ClearAIMode();
					O->CommandAttack(Target->GetID(), true);
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_ATTACK_AREA:
	{
		FGenObject* O = GetUnitNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		if (IsLiving(O) && Area && O->bHasAI)
		{
			O->CommandAttackArea(Area);
		}
		break;
	}
	case EGenScriptAction::A_TEAM_FACE_NAMED:
	{
		// doTeamFaceNamed: aiFaceObject. (Here they stop and are turned that way at once; only a cinematic asks.)
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenObject* Face = GetUnitNamed(Str(P, 1));
		if (Team && Face)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					IdleUnit(*O);
					const FVector3f To = Face->GetPosition() - O->GetPosition();
					O->SetOrientation(FMath::Atan2(To.Y, To.X));
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_DAMAGE:
		// doNamedDamage: unresistable, from nobody.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); IsLiving(O))
		{
			O->AttemptDamage(float(Int(P, 1)), TEXT("UNRESISTABLE"), TEXT("NORMAL"), GenInvalidObjectID);
		}
		break;
	case EGenScriptAction::A_NAMED_FLASH:
		// doNamedFlash without a colour: in the owner's.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); O && Int(P, 1) > 0 && Logic->GetPlayers().IsValidIndex(O->GetOwner()))
		{
			Flashing.Add(O->GetID(), FMath::Max(1, Int(P, 1) * FPS / 15));
			FlashColors.Add(O->GetID(), Logic->GetPlayers()[O->GetOwner()].Color);
		}
		break;
	case EGenScriptAction::A_NAMED_SET_STEALTH_ENABLED:
		// doNamedEnableStealth: OBJECT_STATUS_SCRIPT_UNSTEALTHED
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->bScriptUnstealthed = Int(P, 1) == 0;
		}
		break;
	case EGenScriptAction::A_NAMED_FIRE_SPECIAL_POWER_AT_NAMED:
	{
		FGenObject* O = GetUnitNamed(Str(P, 0));
		const FGenObject* Target = GetUnitNamed(Str(P, 2));
		if (O && Target)
		{
			const int32 Module = Logic->FindPowerModule(*O, Str(P, 1));
			if (Module != INDEX_NONE)
			{
				Logic->FireSpecialPower(*O, Module, Target->GetPosition(), Target->GetID(), true);
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_STOP_SPECIAL_POWER_COUNTDOWN:
	case EGenScriptAction::A_NAMED_START_SPECIAL_POWER_COUNTDOWN:
		// doNamedStopSpecialPowerCountdown: SpecialPowerModule::pauseCountdown
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			const int32 Module = Logic->FindPowerModule(*O, Str(P, 1));
			const FGenObjectID Id = O->GetID();
			PausedPowers.RemoveAll([Id, Module](const FPausedPower& Q) { return Q.Object == Id && Q.Module == Module; });
			if (A.Type == EGenScriptAction::A_NAMED_STOP_SPECIAL_POWER_COUNTDOWN && O->PowerReady.IsValidIndex(Module))
			{
				PausedPowers.Add({ Id, Module, O->PowerReady[Module] > Frame ? O->PowerReady[Module] - Frame : 0u });
			}
		}
		break;
	case EGenScriptAction::A_NAMED_SHOW_SPECIAL_POWER_DISPLAY:
	case EGenScriptAction::A_NAMED_HIDE_SPECIAL_POWER_DISPLAY:
		if (const FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			if (A.Type == EGenScriptAction::A_NAMED_HIDE_SPECIAL_POWER_DISPLAY) { HiddenPowerDisplays.Add(O->GetID()); }
			else { HiddenPowerDisplays.Remove(O->GetID()); }
		}
		break;
	case EGenScriptAction::A_DISPLAY_COUNTDOWN_TIMER:
		NamedTimers.Add(Str(P, 0), Str(P, 1));
		break;
	case EGenScriptAction::A_HIDE_COUNTDOWN_TIMER:
		NamedTimers.Remove(Str(P, 0));
		break;
	case EGenScriptAction::A_TEAM_USE_COMMANDBUTTON_ABILITY_AT_WAYPOINT:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FVector3f Pos;
		if (Team && GetWaypoint(Str(P, 2), Pos))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members) { UseCommandButton(*O, Str(P, 1), nullptr, &Pos); }
		}
		break;
	}
	case EGenScriptAction::A_TEAM_SET_UNMANNED_STATUS:
		// doTeamSetUnmanned: no drivers, and nobody's.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			const int32 Neutral = Logic->GetNeutralPlayer();
			const int32* DefaultId = DefaultTeams.Find(Neutral);
			FGenMapTeam* NeutralTeam = DefaultId ? FindTeam(*DefaultId) : nullptr;
			for (FGenObject* O : Members)
			{
				O->MakeUnmanned(GenInvalidObjectID);
				if (NeutralTeam) { SetObjectTeam(*O, *NeutralTeam); }
			}
		}
		break;
	case EGenScriptAction::A_LOCALDEFEAT:
		// doLocalDefeat: the campaign manager hears the mission is lost (it comes after a DEFEAT on the one map that has it).
		Logic->ScriptGameOver(false);
		break;
	case EGenScriptAction::A_PLAYER_PURCHASE_SCIENCE:
	{
		// doPlayerPurchaseScience -> attemptToPurchaseScience
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE && Logic->CanPurchaseScience(Player, Str(P, 1)))
		{
			Logic->PurchaseScience(Player, Str(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_AI_PLAYER_BUILD_SUPPLY_CENTER:
	{
		// doBuildSupplyCenter -> AIPlayer::buildBySupplies
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (FGenSkirmishAI* AI = Player != INDEX_NONE ? Logic->GetAI(Player) : nullptr)
		{
			AI->ScriptBuildBySupplies(Int(P, 2), Str(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_TEAM_GUARD_SUPPLY_CENTER:
		// doGuardSupplyCenter -> AIPlayer::guardSupplyCenter
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (FGenSkirmishAI* AI = Logic->GetAI(Team->Player))
			{
				AI->ScriptGuardSupplyCenter(*Team, Int(P, 1));
			}
		}
		break;
	case EGenScriptAction::A_SKIRMISH_MOVE_TO_APPROACH_PATH:
	case EGenScriptAction::A_SKIRMISH_FOLLOW_APPROACH_PATH:
		// doTeamMoveToSkirmishApproachPath / doTeamFollowSkirmishApproachPath: the path to the base of its enemy.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (FGenSkirmishAI* AI = Logic->GetAI(Team->Player))
			{
				AI->ScriptFollowApproachPath(*Team, Str(P, 1), A.Type == EGenScriptAction::A_SKIRMISH_MOVE_TO_APPROACH_PATH);
			}
		}
		break;
	case EGenScriptAction::A_NAMED_SET_BOOBYTRAPPED:
		// doNamedSetBoobytrapped (the thing, the unit): a charge on it, on its team.
		if (FGenObject* O = GetUnitNamed(Str(P, 1)); IsLiving(O))
		{
			const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(Str(P, 0));
			FGenObject* Charge = Thing ? Logic->CreateObject(Thing, O->GetOwner(), O->GetPosition(), 0.f) : nullptr;
			if (Charge)
			{
				if (FGenMapTeam* Team = TF.FindTeam(GetObjectTeam(*O))) { SetObjectTeam(*Charge, *Team); }
				if (Thing->StickyBomb.bValid) { Charge->InitStickyBomb(O, nullptr); }
			}
		}
		break;
	case EGenScriptAction::A_SET_ATTACK_PRIORITY_KIND_OF:
	{
		// ScriptEngine::setPriorityKind: every template of that kind.
		FGenAttackPriority& Set = Logic->FindOrAddAttackPriority(Str(P, 0));
		const FString Kind = Str(P, 1).ToUpper();
		for (const TPair<FString, TSharedPtr<FGenThingTemplate>>& T : FGenGameData::Get().GetTemplates())
		{
			if (T.Value->IsKindOf(*Kind))
			{
				Set.Things.Add(T.Value->Canonical(), Int(P, 2));
				Set.MaxPriority = FMath::Max(Set.MaxPriority, Int(P, 2));
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_GUARD_OBJECT:
		// doTeamGuardObject: GUARDMODE_NORMAL; the post moves with the thing guarded.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			const FGenObject* Target = GetUnitNamed(Str(P, 1));
			if (IsLiving(Target))
			{
				TArray<FGenObject*> Members;
				GetTeamMembers(*Team, Members);
				for (FGenObject* O : Members)
				{
					O->CommandGuardPosition(FVector2f(Target->GetPosition().X, Target->GetPosition().Y), EGenGuardMode::Normal, Target->GetID());
				}
			}
		}
		break;
	case EGenScriptAction::A_TECHTREE_MODIFY_BUILDABILITY_OBJECT:
		// doModifyBuildableStatus: BSTATUS_YES, IGNORE_PREREQUISITES, NO, ONLY_BY_AI, in FGenThingTemplate::Buildable's numbers.
		if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Str(P, 0)))
		{
			static const uint8 ToBuildable[] = { 0, 3, 1, 2 };
			if (FMath::IsWithin(Int(P, 1), 0, 4))
			{
				Logic->SetBuildableOverride(*T, ToBuildable[Int(P, 1)]);
			}
		}
		break;
	case EGenScriptAction::A_TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_OBJECTTYPE:
	{
		// doTeamUseCommandButtonOnNearestObjectType: on the enemy or neutral thing of that type (or list) nearest the
		// team's center, if a member has the button.
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FVector2f Center;
		if (!Team || !GetTeamCenter(*Team, Center))
		{
			break;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members);
		if (!Members.ContainsByPredicate([this, &P](const FGenObject* O) { return ObjectHasButton(*O, Str(P, 1)); }))
		{
			break;
		}
		const TArray<const FGenThingTemplate*>& Types = ResolveTypes(Str(P, 2));
		const FGenObject* Best = nullptr;
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (IsLiving(O) && Types.Contains(O->GetTemplate()->Canonical()) && Logic->GetRelationship(Team->Player, *O) != EGenRelationship::Allies)
			{
				const float DistSq = FVector2f::DistSquared(Center, FVector2f(O->GetPosition().X, O->GetPosition().Y));
				if (DistSq < BestDistSq) { BestDistSq = DistSq; Best = O; }
			}
		}
		if (Best)
		{
			for (FGenObject* O : Members) { UseCommandButton(*O, Str(P, 1), Best, nullptr); }
		}
		break;
	}
	case EGenScriptAction::A_TEAM_SET_BOOBYTRAPPED:
		// doTeamSetBoobytrapped (the thing, the team): a charge on each member, on that member's team.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 1)))
		{
			const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(Str(P, 0));
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				FGenObject* Charge = Thing ? Logic->CreateObject(Thing, O->GetOwner(), O->GetPosition(), 0.f) : nullptr;
				if (Charge)
				{
					SetObjectTeam(*Charge, *Team);
					if (Thing->StickyBomb.bValid) { Charge->InitStickyBomb(O, nullptr); }
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_CREATE_RADAR_EVENT:
		// doTeamRadarCreateEvent: at the team's place.
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			FVector2f Center;
			if (GetTeamCenter(*Team, Center))
			{
				Logic->CreateRadarEvent(FVector3f(Center.X, Center.Y, Logic->GetGroundHeight(Center.X, Center.Y)),
					EGenRadarEvent(FMath::Clamp(Int(P, 1) - 1, 0, int32(EGenRadarEvent::Fake))));
			}
		}
		break;
	case EGenScriptAction::A_CREATE_OBJECT:
		// doCreateObject(unnamed, thing, team, pos, angle)
		CreateObjectOnTeam(FString(), Str(P, 0), Str(P, 1), P.IsValidIndex(2) ? P[2].Coord : FVector3f::ZeroVector, Real(P, 3));
		break;
	case EGenScriptAction::A_ENABLE_OBJECT_SOUND:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->bFromMap = true;
			O->MapAmbientEnabled = 1;
			++O->MapAmbientScriptCount;
		}
		break;
	case EGenScriptAction::A_TEAM_KILL:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			KillTeam(*Team);
		}
		break;
	case EGenScriptAction::A_PLAYER_KILL:
	{
		// Player::killPlayer: every team of the player's.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			break;
		}
		TArray<int32> Ids;
		for (const TUniquePtr<FGenMapTeam>& T : Teams)
		{
			if (T->Player == Player) { Ids.Add(T->Id); }
		}
		for (int32 Id : Ids)
		{
			if (FGenMapTeam* T = FindTeam(Id)) { KillTeam(*T); }
		}
		break;
	}
	case EGenScriptAction::A_TEAM_TRANSFER_TO_PLAYER:
	{
		// doTransferTeamToPlayer: Team::setControllingPlayer - what is on the team goes along, and it stays a team.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const int32 Player = GetPlayerNamed(Str(P, 1));
		if (Team && Player != INDEX_NONE)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members, true);
			Team->Player = Player;
			for (FGenObject* O : Members) { Logic->ChangeOwner(*O, Player, false); }
		}
		break;
	}
	case EGenScriptAction::A_NAMED_TRANSFER_OWNERSHIP_PLAYER:
	{
		// doNamedTransferAssetsToPlayer: onto that player's default team.
		FGenObject* O = GetUnitNamed(Str(P, 0));
		const int32 Player = GetPlayerNamed(Str(P, 1));
		const int32* DefaultId = Player != INDEX_NONE ? DefaultTeams.Find(Player) : nullptr;
		FGenMapTeam* Default = DefaultId ? FindTeam(*DefaultId) : nullptr;
		if (O && Default)
		{
			Logic->ChangeOwner(*O, Player, false);
			SetObjectTeam(*O, *Default);
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_TRANSFER_OWNERSHIP_PLAYER:
	{
		// doPlayerTransferAssetsToPlayer: Player::transferAssetsFromThat.
		const int32 From = GetPlayerNamed(Str(P, 0));
		const int32 To = GetPlayerNamed(Str(P, 1));
		const int32* DefaultId = To != INDEX_NONE ? DefaultTeams.Find(To) : nullptr;
		FGenMapTeam* Default = DefaultId ? FindTeam(*DefaultId) : nullptr;
		if (From == INDEX_NONE || !Default)
		{
			break;
		}
		const TArray<FGenObject*> Things(Logic->GetObjectsOf(From));
		for (FGenObject* O : Things)
		{
			if (!O->IsDestroyed())
			{
				Logic->ChangeOwner(*O, To, false);
				SetObjectTeam(*O, *Default);
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_SET_OVERRIDE_RELATION_TO_PLAYER:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const int32 Other = GetPlayerNamed(Str(P, 1));
		if (Team && Other != INDEX_NONE)
		{
			Team->OverridePlayerRelations.Add(Other, RelationFromInt(Int(P, 2)));
			Logic->NoteTeamRelationOverride();
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SET_OVERRIDE_RELATION_TO_TEAM:
	case EGenScriptAction::A_PLAYER_REMOVE_OVERRIDE_RELATION_TO_TEAM:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 1));
		if (Player != INDEX_NONE && Team)
		{
			if (A.Type == EGenScriptAction::A_PLAYER_SET_OVERRIDE_RELATION_TO_TEAM)
			{
				Logic->SetPlayerTeamRelation(Player, Team->Id, RelationFromInt(Int(P, 2)));
			}
			else
			{
				Logic->RemovePlayerTeamRelation(Player, Team->Id);
			}
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_DISABLE_BASE_CONSTRUCTION:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->SetCanBuildBase(Player, false); }
		break;
	}
	case EGenScriptAction::A_PLAYER_DISABLE_UNIT_CONSTRUCTION:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->SetCanBuildUnits(Player, false); }
		break;
	}
	case EGenScriptAction::A_COMMANDBAR_ADD_BUTTON_OBJECTTYPE_SLOT:
		// doAddCommandBarButton: slot numbered from 1 (GameLogic::setControlBarOverride).
		if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Str(P, 1)))
		{
			const int32 Slot = Int(P, 2) - 1;
			if (Slot >= 0 && Slot < FGenCommandSet::NumSlots && FGenCommandData::Get().FindButton(Str(P, 0)))
			{
				AddedButtons.FindOrAdd(T->CommandSet.ToLower()).Add(Slot, Str(P, 0));
			}
		}
		break;
	case EGenScriptAction::A_MAP_REVEAL_ALL:
	{
		// doRevealMapEntire: that player, or (no name) every human player.
		const int32 Player = Str(P, 0).IsEmpty() ? INDEX_NONE : GetPlayerNamed(Str(P, 0));
		for (const FGenPlayer& Pl : Logic->GetPlayers())
		{
			if (Player != INDEX_NONE ? Pl.Index == Player : Pl.bHuman)
			{
				Logic->ScriptRevealMapOnce(Pl.Index);
			}
		}
		break;
	}
	case EGenScriptAction::A_MAP_REVEAL_AT_WAYPOINT:
	{
		FVector3f Pos;
		const int32 Player = Str(P, 2).IsEmpty() ? INDEX_NONE : GetPlayerNamed(Str(P, 2));
		if (GetWaypoint(Str(P, 0), Pos))
		{
			for (const FGenPlayer& Pl : Logic->GetPlayers())
			{
				if (Player != INDEX_NONE ? Pl.Index == Player : Pl.bHuman)
				{
					Logic->ScriptRevealOnce(Pl.Index, FVector2f(Pos.X, Pos.Y), Real(P, 1));
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_MAP_UNDO_REVEAL_PERMANENTLY_AT_WAYPOINT:
		if (const FNamedReveal* Reveal = NamedReveals.Find(Str(P, 0)))
		{
			Logic->ScriptRevealPermanently(Reveal->Player, Reveal->Pos, Reveal->Radius, true);
			NamedReveals.Remove(Str(P, 0));
		}
		break;
	case EGenScriptAction::A_CAMERA_MOTION_BLUR_JUMP:
		// (The zoom blur isn't drawn; "if we failed to apply the filter, we still need to get the camera to the target".)
		Client(A, &Str(P, 0));
		break;
	case EGenScriptAction::A_CAMERA_FOLLOW_NAMED:
	case EGenScriptAction::A_CAMERA_STOP_FOLLOW:
	case EGenScriptAction::A_SHOW_WEATHER:
	case EGenScriptAction::A_SPEECH_SET_VOLUME:
		Client(A);
		break;
	case EGenScriptAction::A_UNIT_EXECUTE_SEQUENTIAL_SCRIPT:
		// doUnitExecuteSequentialScript (ScriptActions.cpp:4681): the actions run on one unit, in order, each when
		// that unit stands idle. 'LCAC01 Deploy Sequence' is the landing craft walking, emptying itself, walking
		// again - which only makes sense with the idle gate. USA02 gives no loop count, but the original's count is
		// kept for the maps that do.
		if (FGenObject* Unit = GetUnitNamed(Str(P, 0)))
		{
			if (const FGenScript* S = FindScript(Str(P, 1)))
			{
				if (Unit->CanMove()) { IdleUnit(*Unit); }
				AppendSequential(GetObjectTeam(*Unit), Unit->GetID(), S, P.Num() > 2 ? Int(P, 2) : 0);
			}
		}
		break;
	case EGenScriptAction::A_BUILD_TEAM:
	{
		// doBuildTeam (ScriptActions.cpp:2308): the name is a team definition, and it is built for the player the
		// definition belongs to - not for the side whose script happens to be running. A human side has nothing to
		// build, so this does nothing for it.
		const int32 ProtoIndex = FindProto(Str(P, 0));
		if (ProtoIndex == INDEX_NONE)
		{
			break;
		}
		const int32 Owner = ProtoPlayers.IsValidIndex(ProtoIndex) ? ProtoPlayers[ProtoIndex] : CurrentPlayer;
		if (FGenSkirmishAI* AI = Logic->GetAI(Owner))
		{
			// Player::buildSpecificTeam -> AIPlayer::buildSpecificAITeam(proto, true): a priority build.
			AI->BuildTeamFromScript(ProtoIndex);
		}
		break;
	}

	// --- units ---
	case EGenScriptAction::A_NAMED_DELETE:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			Logic->RemoveObjectByScript(*O);
		}
		break;
	case EGenScriptAction::A_TEAM_DELETE:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			DeleteTeamMembers(*Team);
		}
		break;
	case EGenScriptAction::A_NAMED_KILL:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); IsLiving(O) && !O->bIndestructible)
		{
			O->Kill();
		}
		break;
	case EGenScriptAction::A_NAMED_FOLLOW_WAYPOINTS:
	case EGenScriptAction::A_NAMED_FOLLOW_WAYPOINTS_EXACT:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			FollowWaypoints(*O, Str(P, 1), A.Type == EGenScriptAction::A_NAMED_FOLLOW_WAYPOINTS_EXACT);
		}
		break;
	case EGenScriptAction::A_TEAM_FOLLOW_WAYPOINTS:
	case EGenScriptAction::A_TEAM_FOLLOW_WAYPOINTS_EXACT:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TeamFollowWaypoints(*Team, Str(P, 1), A.Type == EGenScriptAction::A_TEAM_FOLLOW_WAYPOINTS_EXACT);
		}
		break;
	case EGenScriptAction::A_TEAM_PANIC:
		// doTeamPanic: each member runs along the path from the waypoint of it nearest that member.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				FollowWaypoints(*O, Str(P, 1), false);
			}
		}
		break;
	case EGenScriptAction::A_MOVE_TEAM_TO:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FVector3f Dest;
		if (Team && GetWaypoint(Str(P, 1), Dest))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			TArray<FGenObjectID> Ids;
			for (FGenObject* O : Members)
			{
				if (O->CanMove() && !O->bScriptHeld)
				{
					O->ClearAIMode();
					// getMoodMatrixActionAdjustment: a move is an attack move for what is alert or aggressive.
					if (O->ScriptAttitude > 0) { O->CommandAttackMove(Dest); }
					else { Ids.Add(O->GetID()); }
				}
			}
			Logic->GroupMoveToPosition(Ids, Dest, false);
		}
		break;
	}
	case EGenScriptAction::A_MOVE_NAMED_UNIT_TO:
	{
		FGenObject* O = GetUnitNamed(Str(P, 0));
		FVector3f Dest;
		if (O && GetWaypoint(Str(P, 1), Dest))
		{
			if (O->IsContained())
			{
				// DISABLED_HELD: what rides in a transport isn't mobile, and the order is lost. A scene is timed to have
				// it out by now; landing isn't that exact here, so one on its way out goes there when it is out.
				if (O->bWantsExit)
				{
					O->bHasExitRally = true;
					O->ExitRally = Dest;
				}
				break;
			}
			O->ClearAIMode();
			O->CommandMoveTo(Dest);
		}
		break;
	}
	case EGenScriptAction::A_NAMED_GUARD:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); IsLiving(O) && O->CanMove())
		{
			O->CommandGuardPosition(FVector2f(O->GetPosition().X, O->GetPosition().Y), EGenGuardMode::Normal);
		}
		break;
	case EGenScriptAction::A_TEAM_GUARD_POSITION:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FVector3f Pos;
		if (Team && GetWaypoint(Str(P, 1), Pos))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					O->CommandGuardPosition(FVector2f(Pos.X, Pos.Y), EGenGuardMode::Normal);
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_ATTACK_AREA:
	{
		// doTeamAttackArea (ScriptActions.cpp:1409): AIGroup::groupAttackArea, which puts the team in
		// AIAttackAreaState. It is a hunt limited to the polygon, not a post at its middle: the team goes after
		// whatever is inside the area, and with nothing there it is idle until the scripts order the area again
		// (what mission 2 does for its attack teams, every 10 s). Guarding an area - TEAM_GUARD_AREA below -
		// is the one that holds a post.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		if (Team && Area)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				O->CommandAttackArea(Area);
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_GUARD_AREA:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		if (Team && Area)
		{
			// AIGroup::groupGuardArea: the post is the middle of the area; what is hostile inside it gets attacked.
			FBox2f Bounds(ForceInit);
			for (const FIntVector& Pt : Area->Points) { Bounds += FVector2f(float(Pt.X), float(Pt.Y)); }
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					O->CommandGuard(Bounds.GetCenter(), Bounds.GetSize().Size() * 0.5f, Area);
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_HUNT:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); IsLiving(O) && O->CanMove())
		{
			O->CommandHunt();
		}
		break;
	case EGenScriptAction::A_TEAM_HUNT:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove() && O->HasWeapons()) { O->CommandHunt(); }
			}
		}
		break;
	case EGenScriptAction::A_TEAM_WANDER_IN_PLACE:
		// doTeamWanderInPlace: each member ambles about where it stands (AIWanderInPlaceState: a few steps this way
		// and that, on its WANDER locomotor). Here: one short walk to a spot nearby.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					const float Angle = Logic->GetRandom().FRandRange(0.f, 2.f * PI);
					const float Dist = Logic->GetRandom().FRandRange(10.f, 30.f);
					O->ClearAIMode();
					O->CommandMoveTo(O->GetPosition() + FVector3f(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist, 0.f));
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_STOP:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members) { if (O->CanMove()) { IdleUnit(*O); } }
		}
		break;
	case EGenScriptAction::A_IDLE_ALL_UNITS:
		// doIdleAllPlayerUnits (no player): every human player's units stop what they are doing.
		for (const FGenPlayer& Pl : Logic->GetPlayers())
		{
			if (!Pl.bHuman)
			{
				continue;
			}
			for (FGenObject* O : Logic->GetObjectsOf(Pl.Index))
			{
				if (IsLiving(O) && O->CanMove())
				{
					IdleUnit(*O);
				}
			}
		}
		break;
	case EGenScriptAction::A_NAMED_SET_HELD:
		// doNamedSetHeld: DISABLED_HELD for ever, or no more.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->bScriptHeld = Int(P, 1) != 0;
			if (O->bScriptHeld && O->CanMove())
			{
				IdleUnit(*O);
			}
		}
		break;
	case EGenScriptAction::A_SET_TRAIN_HELD:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); O && O->Train)
		{
			O->Train->bHeld = Int(P, 1) != 0;
		}
		break;
	case EGenScriptAction::A_NAMED_SET_EVAC_LEFT_OR_RIGHT:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->EvacDisposition = uint8(FMath::Clamp(Int(P, 1), 0, 2));
		}
		break;
	case EGenScriptAction::A_EXIT_SPECIFIC_BUILDING:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); O && O->IsKindOf(TEXT("STRUCTURE")) && O->GetContain())
		{
			Logic->EvacuateContainer(*O);
		}
		break;
	case EGenScriptAction::A_NAMED_EXIT_ALL:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); O && O->GetContain())
		{
			Logic->EvacuateContainer(*O);
		}
		break;
	case EGenScriptAction::A_NAMED_ATTACK_NAMED:
	{
		// doNamedAttackNamed: both have to be there, and the one that attacks has to have an AI module; then
		// aiForceAttackObject(target, NO_MAX_SHOTS_LIMIT, CMD_FROM_SCRIPT) - an attack order that is not dropped
		// when the target dies, and that is not limited to a number of shots.
		FGenObject* Attacker = GetUnitNamed(Str(P, 0));
		FGenObject* Target = GetUnitNamed(Str(P, 1));
		if (IsLiving(Attacker) && IsLiving(Target) && Attacker->bHasAI)
		{
			Attacker->ClearAIMode();
			Attacker->CommandAttack(Target->GetID(), true);
		}
		break;
	}
	case EGenScriptAction::A_TEAM_ENTER_NAMED:
	{
		// doTeamEnterNamed (ScriptActions.cpp:1547): the transport has to be there, and every member of the team
		// that can move is put in it - AIGroup::groupEnter, which is aiEnter for each member.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FGenObject* Into = GetUnitNamed(Str(P, 1));
		if (!Team || !IsLiving(Into) || !Into->GetContain())
		{
			break;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members);
		for (FGenObject* O : Members)
		{
			if (O->CanMove())
			{
				O->ClearAIMode();
				O->CommandEnter(*Into);
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_ENTER_NAMED:
	{
		FGenObject* Unit = GetUnitNamed(Str(P, 0));
		FGenObject* Into = GetUnitNamed(Str(P, 1));
		if (IsLiving(Unit) && IsLiving(Into))
		{
			Unit->ClearAIMode();
			Unit->CommandEnter(*Into);
		}
		break;
	}
	case EGenScriptAction::A_NAMED_GARRISON_SPECIFIC_BUILDING:
	{
		FGenObject* Unit = GetUnitNamed(Str(P, 0));
		FGenObject* Building = GetUnitNamed(Str(P, 1));
		if (IsLiving(Unit) && IsLiving(Building) && Building->GetContain())
		{
			Unit->ClearAIMode();
			Unit->CommandEnter(*Building);
		}
		break;
	}
	case EGenScriptAction::A_TEAM_GARRISON_SPECIFIC_BUILDING:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FGenObject* Building = GetUnitNamed(Str(P, 1));
		if (Team && IsLiving(Building) && Building->GetContain() && Building->IsKindOf(TEXT("STRUCTURE")))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (!O->IsContained() && Logic->CanEnterObject(*O, *Building, true))
				{
					O->ClearAIMode();
					O->CommandEnter(*Building);
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_LOAD_TRANSPORTS:
		// doLoadAllTransports: the team's passengers into the team's transports (first fit, for the partition solver).
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members, Transports, Units;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->IsContained()) { continue; }
				if (O->IsKindOf(TEXT("TRANSPORT")) && O->GetContain()) { Transports.Add(O); }
				else { Units.Add(O); }
			}
			TArray<int32> Free;
			for (const FGenObject* T : Transports)
			{
				Free.Add(Logic->GetContainMax(*T) - Logic->GetContainCount(*T));
			}
			for (FGenObject* U : Units)
			{
				for (int32 t = 0; t < Transports.Num(); ++t)
				{
					const int32 Slots = U->GetTemplate()->TransportSlotCount;
					if (Slots > 0 && Slots <= Free[t] && Logic->CanEnterObject(*U, *Transports[t], false))
					{
						Free[t] -= Slots;
						U->ClearAIMode();
						U->CommandEnter(*Transports[t]);
						break;
					}
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_GUARD_IN_TUNNEL_NETWORK:
		// doTeamGuardInTunnelNetwork: each member goes into its player's nearest tunnel and guards from inside.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				FGenObject* Best = nullptr;
				float BestDistSq = FLT_MAX;
				for (FGenObject* B : Logic->GetObjectsOf(O->GetOwner()))
				{
					if (IsLiving(B) && B->GetTemplate()->Contain.Type == EGenContainType::Tunnel && Logic->CanEnterObject(*O, *B, true))
					{
						const float D = (B->GetPosition() - O->GetPosition()).SizeSquared();
						if (D < BestDistSq) { BestDistSq = D; Best = B; }
					}
				}
				if (Best)
				{
					O->ClearAIMode();
					O->CommandEnter(*Best);
				}
			}
		}
		break;
	case EGenScriptAction::A_NAMED_USE_COMMANDBUTTON_ABILITY:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			UseCommandButton(*O, Str(P, 1), nullptr, nullptr);
		}
		break;
	case EGenScriptAction::A_NAMED_USE_COMMANDBUTTON_ABILITY_ON_NAMED:
	{
		FGenObject* O = GetUnitNamed(Str(P, 0));
		const FGenObject* Target = GetUnitNamed(Str(P, 2));
		if (O && Target)
		{
			UseCommandButton(*O, Str(P, 1), Target, nullptr);
		}
		break;
	}
	case EGenScriptAction::A_NAMED_USE_COMMANDBUTTON_ABILITY_AT_WAYPOINT:
	{
		FGenObject* O = GetUnitNamed(Str(P, 0));
		FVector3f Pos;
		if (O && GetWaypoint(Str(P, 2), Pos))
		{
			UseCommandButton(*O, Str(P, 1), nullptr, &Pos);
		}
		break;
	}
	case EGenScriptAction::A_TEAM_USE_COMMANDBUTTON_ABILITY:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members) { UseCommandButton(*O, Str(P, 1), nullptr, nullptr); }
		}
		break;
	case EGenScriptAction::A_TEAM_USE_COMMANDBUTTON_ABILITY_ON_NAMED:
	{
		// doTeamUseCommandButtonAbilityOnNamed (ScriptActions.cpp:4426): the team and the thing it is used on both
		// have to be there and the button has to be in the control-bar table, then the button is used on every
		// member of the team. USA02: 'GLA_attackers' using its supply- or repair-button ability on a named building.
		// The order the original takes them in is team, ability, target.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenObject* Target = GetUnitNamed(Str(P, 2));
		if (!Team || !IsLiving(Target))
		{
			break;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members);
		for (FGenObject* O : Members) { UseCommandButton(*O, Str(P, 1), Target, nullptr); }
		break;
	}
	case EGenScriptAction::A_NAMED_FIRE_SPECIAL_POWER_AT_WAYPOINT:
	{
		FGenObject* O = GetUnitNamed(Str(P, 0));
		FVector3f Pos;
		if (O && GetWaypoint(Str(P, 2), Pos))
		{
			const int32 Module = Logic->FindPowerModule(*O, Str(P, 1));
			if (Module != INDEX_NONE)
			{
				Logic->FireSpecialPower(*O, Module, Pos, GenInvalidObjectID, true);
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_SET_SPECIAL_POWER_COUNTDOWN:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			const int32 Module = Logic->FindPowerModule(*O, Str(P, 1));
			if (O->PowerReady.IsValidIndex(Module))
			{
				O->PowerReady[Module] = Frame + uint32(FMath::Max(0, Int(P, 2)) * FPS);
			}
		}
		break;
	case EGenScriptAction::A_CREATE_NAMED_ON_TEAM_AT_WAYPOINT:
	{
		FVector3f Pos;
		if (GetWaypoint(Str(P, 3), Pos))
		{
			CreateObjectOnTeam(Str(P, 0), Str(P, 1), Str(P, 2), Pos, 0.f);
		}
		break;
	}
	case EGenScriptAction::A_CREATE_UNNAMED_ON_TEAM_AT_WAYPOINT:
	{
		FVector3f Pos;
		if (GetWaypoint(Str(P, 2), Pos))
		{
			CreateObjectOnTeam(FString(), Str(P, 0), Str(P, 1), Pos, 0.f);
		}
		break;
	}
	case EGenScriptAction::A_UNIT_SPAWN_NAMED_LOCATION_ORIENTATION:
		CreateObjectOnTeam(Str(P, 0), Str(P, 1), Str(P, 2), P.IsValidIndex(3) ? P[3].Coord : FVector3f::ZeroVector, Real(P, 4));
		break;
	case EGenScriptAction::A_CREATE_REINFORCEMENT_TEAM:
		CreateReinforcements(Str(P, 0), Str(P, 1));
		break;
	case EGenScriptAction::A_TEAM_MERGE_INTO_TEAM:
	{
		// doMergeTeamIntoTeam: everything of the one goes onto the other (and so to its player).
		FGenMapTeam* From = GetTeamNamed(Str(P, 0));
		FGenMapTeam* To = GetOrCreateTeamNamed(Str(P, 1));
		if (From && To && From != To)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*From, Members, true);
			for (FGenObject* O : Members)
			{
				SetObjectTeam(*O, *To);
			}
			if (!From->bPlayerDefault && Protos.IsValidIndex(From->Proto) && !Protos[From->Proto].bSingleton)
			{
				From->bActive = false;
			}
		}
		break;
	}
	case EGenScriptAction::A_UNIT_AFFECT_OBJECT_PANEL_FLAGS:
		// changeObjectPanelFlagForSingleObject, for the flags the ported maps set.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			const FString& Flag = Str(P, 1);
			const bool bOn = Int(P, 2) != 0;
			if (Flag.Equals(TEXT("Indestructible"), ESearchCase::IgnoreCase)) { O->bIndestructible = bOn; }
			else if (Flag.Equals(TEXT("Selectable"), ESearchCase::IgnoreCase)) { O->bUnselectable = !bOn; }
			else if (Flag.Equals(TEXT("Enabled"), ESearchCase::IgnoreCase) || Flag.Equals(TEXT("Powered"), ESearchCase::IgnoreCase)) { O->bScriptDisabled = !bOn; }
			else if (Flag.Equals(TEXT("Unsellable"), ESearchCase::IgnoreCase)) { O->bScriptUnsellable = bOn; }
			else if (Flag.EndsWith(TEXT("Targetable"), ESearchCase::IgnoreCase)) { O->bScriptTargetable = bOn; }
		else { UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] object panel flag %s isn't ported: nothing"), Frame, *Flag); }
		}
		break;
	case EGenScriptAction::A_NAMED_FLASH_WHITE:
		// doNamedFlash: so many seconds of flashes, one every DRAWABLE_FRAMES_PER_FLASH.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); O && Int(P, 1) > 0)
		{
			Flashing.Add(O->GetID(), FMath::Max(1, Int(P, 1) * FPS / 15));
		}
		break;
	case EGenScriptAction::A_OBJECT_CREATE_RADAR_EVENT:
		// doObjectRadarCreateEvent (RadarEventType: 1 construction, 2 upgrade, 3 under attack, 4 information, ...).
		if (const FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			Logic->CreateRadarEvent(O->GetPosition(), EGenRadarEvent(FMath::Clamp(Int(P, 1) - 1, 0, int32(EGenRadarEvent::Fake))));
		}
		break;
	case EGenScriptAction::A_RADAR_CREATE_EVENT:
		// doRadarCreateEvent: the same marker, at a place the script gives rather than at a unit. USA02 puts an
		// information event over the airfield so the player is sent to look at it.
		Logic->CreateRadarEvent(P.IsValidIndex(0) ? P[0].Coord : FVector3f::ZeroVector,
			EGenRadarEvent(FMath::Clamp(Int(P, 1) - 1, 0, int32(EGenRadarEvent::Fake))));
		break;
	case EGenScriptAction::A_RADAR_FORCE_ENABLE:
		// Radar::forceOn(true): the radar is shown whatever the player has or has not built (GenHUD checks this).
		bRadarForced = true;
		break;
	case EGenScriptAction::A_DISABLE_OBJECT_SOUND:
		// doEnableObjectSound(false): the object's ambient sound stops.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->bFromMap = true;
			O->MapAmbientEnabled = 0;
			++O->MapAmbientScriptCount;
		}
		break;
	case EGenScriptAction::A_SCRIPTING_OVERRIDE_HULK_LIFETIME:
		HulkLifetimeOverride = Real(P, 0) < 0.f ? -1 : int32(Real(P, 0) * float(FPS));
		break;
	case EGenScriptAction::A_OBJECT_ALLOW_BONUSES:
		// doEnableOrDisableObjectDifficultyBonuses: "loops over every object in the game", and what comes later goes by this.
		bObjectsReceiveBonus = Int(P, 0) != 0;
		for (const FGenObjectID Id : Logic->GetObjectIDs())
		{
			if (FGenObject* O = Logic->FindObject(Id))
			{
				O->SetReceivingDifficultyBonus(bObjectsReceiveBonus);
			}
		}
		break;

	// --- players ---
	case EGenScriptAction::A_PLAYER_RELATES_PLAYER:
	{
		const int32 From = GetPlayerNamed(Str(P, 0));
		const int32 To = GetPlayerNamed(Str(P, 1));
		if (From != INDEX_NONE && To != INDEX_NONE)
		{
			Logic->SetPlayerRelationship(From, To, RelationFromInt(Int(P, 2)));
		}
		break;
	}
	case EGenScriptAction::A_TEAM_SET_OVERRIDE_RELATION_TO_TEAM:
	{
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FGenMapTeam* Other = GetTeamNamed(Str(P, 1));
		if (Team && Other)
		{
			Team->OverrideRelations.Add(Other->Id, RelationFromInt(Int(P, 2)));
			Logic->NoteTeamRelationOverride();
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SCIENCE_AVAILABILITY:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE)
		{
			// (Player::setScienceAvailability)
			const FString Science = Str(P, 1).ToLower();
			TSet<FString>& Hidden = HiddenSciences.FindOrAdd(Player);
			TSet<FString>& Disabled = DisabledSciences.FindOrAdd(Player);
			Hidden.Remove(Science);
			Disabled.Remove(Science);
			if (Str(P, 2).Equals(TEXT("Hidden"), ESearchCase::IgnoreCase)) { Hidden.Add(Science); }
			else if (Str(P, 2).Equals(TEXT("Disabled"), ESearchCase::IgnoreCase)) { Disabled.Add(Science); }
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SET_RANKLEVELLIMIT:
		RankLevelLimit = Int(P, 0);
		Logic->SetRankLevelLimit(RankLevelLimit);
		break;
	case EGenScriptAction::A_PLAYER_SET_RANKLEVEL:
	{
		// doPlayerSetRankLevel -> Player::setRankLevel: inside the ranks the data has, and inside any limit the map
		// set; a rank going down takes that rank's things back.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->SetRankLevel(Player, Int(P, 1)); }
		break;
	}
	case EGenScriptAction::A_PLAYER_AFFECT_RECEIVING_EXPERIENCE:
	{
		// Player::setSkillPointsModifier: what future skill points are multiplied by, so only what is earned after
		// this changes. USA02 sets 0 for 'ThePlayer' and later 1.0 again.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->SetSkillPointsModifier(Player, Real(P, 1)); }
		break;
	}
	case EGenScriptAction::A_PLAYER_GRANT_SCIENCE:
	{
		// doPlayerGrantScience -> Player::grantScience: given outright, with no prerequisite check.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->GrantScience(Player, Str(P, 1)); }
		break;
	}
	case EGenScriptAction::A_PLAYER_ENABLE_BASE_CONSTRUCTION:
	{
		// Player::setCanBuildBase, on the player. A side the computer plays builds from there (GenSkirmishAI).
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->SetCanBuildBase(Player, true); }
		break;
	}
	case EGenScriptAction::A_PLAYER_ENABLE_UNIT_CONSTRUCTION:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->SetCanBuildUnits(Player, true); }
		break;
	}
	case EGenScriptAction::A_SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST:
	{
		// doSkirmishFireSpecialPowerAtMostCost (ScriptActions.cpp:4166): one of that side's ready special-power
		// modules fires at the best place in the ground it can see of its enemy. Choosing that place is the computer
		// player's, so a side the human plays fires nothing here.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (FGenSkirmishAI* AI = Player != INDEX_NONE ? Logic->GetAI(Player) : nullptr)
		{
			AI->FireSpecialPowerFromScript(Str(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_WAREHOUSE_SET_VALUE:
	{
		// doSetWarehouseValue: the thing has to be there and have a SupplyWarehouseDockUpdate; the cash becomes
		// supply boxes at the value per box. USA02 fills 'Starting_Supply_Dock' with 60000 so the computer side has
		// something to work from.
		FGenObject* O = GetUnitNamed(Str(P, 0));
		if (!O || !O->GetDock() || !O->GetTemplate()->bSupplyWarehouse)
		{
			break;
		}
		const int32 PerBox = FMath::Max(1, FGenGameData::Get().ValuePerSupplyBox);
		O->SetBoxes(FMath::CeilToInt32(float(Int(P, 1)) / float(PerBox)));
		break;
	}
	case EGenScriptAction::A_PLAYER_SET_MONEY:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE)
		{
			Logic->Withdraw(Player, Logic->GetMoney(Player), false);
			Logic->Deposit(Player, Int(P, 1), false);
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_GIVE_MONEY:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE) { Logic->AddMoney(Player, Int(P, 1)); }
		break;
	}
	case EGenScriptAction::A_PLAYER_EXCLUDE_FROM_SCORE_SCREEN:
		if (const int32 Player = GetPlayerNamed(Str(P, 0)); Player != INDEX_NONE)
		{
			ExcludedFromScore.Add(Player);
		}
		break;
	case EGenScriptAction::A_COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE:
		// doRemoveCommandBarButton: the slot of that button in that object type's command set is emptied.
		if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Str(P, 1)))
		{
			if (const FGenCommandSet* Set = FGenCommandData::Get().FindSet(T->CommandSet))
			{
				for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
				{
					if (Set->Slots[i].Equals(Str(P, 0), ESearchCase::IgnoreCase))
					{
						RemovedButtons.FindOrAdd(T->CommandSet.ToLower()).AddUnique(i);
						break;
					}
				}
			}
		}
		break;
	case EGenScriptAction::A_VICTORY:
	case EGenScriptAction::A_DEFEAT:
		// doVictory / doDefeat: the input goes, the game is decided (and the scripts stop: "we are just timing down").
		bInputDisabled = true;
		bEnded = true;
		Client(A);
		Logic->ScriptGameOver(A.Type == EGenScriptAction::A_VICTORY);
		break;
	case EGenScriptAction::A_DISABLE_SCORING:
	case EGenScriptAction::A_ENABLE_SCORING:
		Logic->SetScoringEnabled(A.Type == EGenScriptAction::A_ENABLE_SCORING);
		break;
	case EGenScriptAction::A_FREEZE_TIME:
		bTimeFrozen = true;
		break;
	case EGenScriptAction::A_UNFREEZE_TIME:
		bTimeFrozen = false;
		break;
	case EGenScriptAction::A_SET_FPS_LIMIT:
		FpsLimit = Int(P, 0);
		Client(A);
		break;

	// --- the map and the fog ---
	case EGenScriptAction::A_MAP_REVEAL_ALL_PERM:
	case EGenScriptAction::A_MAP_REVEAL_ALL_UNDO_PERM:
	case EGenScriptAction::A_MAP_SHROUD_ALL:
	{
		// doRevealMapEntirePermanently / doShroudMapEntire: that player, or (no name) every human player.
		const int32 Player = Str(P, 0).IsEmpty() ? INDEX_NONE : GetPlayerNamed(Str(P, 0));
		for (const FGenPlayer& Pl : Logic->GetPlayers())
		{
			if (Player != INDEX_NONE ? Pl.Index != Player : !Pl.bHuman)
			{
				continue;
			}
			if (A.Type == EGenScriptAction::A_MAP_SHROUD_ALL) { Logic->ScriptShroudMap(Pl.Index); }
			else { Logic->ScriptRevealMapPermanently(Pl.Index, A.Type == EGenScriptAction::A_MAP_REVEAL_ALL_PERM); }
		}
		break;
	}
	case EGenScriptAction::A_MAP_REVEAL_PERMANENTLY_AT_WAYPOINT:
	{
		FVector3f Pos;
		const int32 Player = GetPlayerNamed(Str(P, 2));
		if (GetWaypoint(Str(P, 0), Pos) && Player != INDEX_NONE)
		{
			Logic->ScriptRevealPermanently(Player, FVector2f(Pos.X, Pos.Y), Real(P, 1));
			// (createNamedMapReveal: "Will fail if there's already one in existence of the same name.")
			if (!Str(P, 3).IsEmpty() && !NamedReveals.Contains(Str(P, 3)))
			{
				NamedReveals.Add(Str(P, 3), { Player, FVector2f(Pos.X, Pos.Y), Real(P, 1) });
			}
		}
		break;
	}
	case EGenScriptAction::A_MAP_SWITCH_BORDER:
		// doBorderSwitch: TerrainLogic::setActiveBoundary.
		Logic->SetActiveBoundary(Int(P, 0));
		break;
	case EGenScriptAction::A_ENABLE_BORDER_SHROUD:
	case EGenScriptAction::A_DISABLE_BORDER_SHROUD:
	case EGenScriptAction::A_OVERSIZE_TERRAIN:
	case EGenScriptAction::A_RESIZE_VIEW_GUARDBAND:
	case EGenScriptAction::A_DRAW_SKYBOX_BEGIN:
	case EGenScriptAction::A_DRAW_SKYBOX_END:
	case EGenScriptAction::A_OPTIONS_SET_PARTICLE_CAP_MODE:
		Client(A);
		break;
	case EGenScriptAction::A_DISABLE_COUNTDOWN_TIMER_DISPLAY:
		bNamedTimersShown = false; // (InGameUI::showNamedTimerDisplay)
		break;
	case EGenScriptAction::A_OPTIONS_SET_OCCLUSION_MODE:
		// doSetOcclusionMode: GameLogic::setShowBehindBuildingMarkers.
		bShowBehindMarkers = Int(P, 0) != 0;
		break;
	case EGenScriptAction::A_OPTIONS_SET_DRAWICON_UI_MODE:
		bDrawIconUI = Int(P, 0) != 0;
		Client(A);
		break;

	// --- what is seen and heard ---
	case EGenScriptAction::A_DISABLE_INPUT:
		bInputDisabled = true;
		Client(A);
		break;
	case EGenScriptAction::A_ENABLE_INPUT:
		bInputDisabled = false;
		Client(A);
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			if (FGenObject* O = Logic->FindObject(Id); O && O->GetParking())
			{
				O->GetParking()->ParkAllPlanes();
			}
		}
		break;
	case EGenScriptAction::A_CAMERA_FADE_ADD:
	case EGenScriptAction::A_CAMERA_FADE_MULTIPLY:
		SetFade(A);
		break;
	case EGenScriptAction::A_MUSIC_SET_TRACK:
		MusicTrack = Str(P, 0);
		MusicStartFrame = Frame;
		Client(A);
		break;
	case EGenScriptAction::A_PLAY_SOUND_EFFECT_AT:
		Client(A, &Str(P, 1));
		break;
	case EGenScriptAction::A_SETUP_CAMERA:
		Client(A, &Str(P, 0), &Str(P, 3));
		NoteCameraBusyUntil(Frame + 1);
		break;
	case EGenScriptAction::A_MOVE_CAMERA_TO:
	case EGenScriptAction::A_RESET_CAMERA:
		Client(A, &Str(P, 0));
		NoteCameraBusyUntil(Frame + uint32(FMath::Max(0.f, Real(P, 1)) * float(FPS)) + 1);
		break;
	case EGenScriptAction::A_CAMERA_LOOK_TOWARD_WAYPOINT:
		Client(A, &Str(P, 0));
		NoteCameraBusyUntil(Frame + uint32(FMath::Max(0.f, Real(P, 1)) * float(FPS)) + 1);
		break;
	case EGenScriptAction::A_ZOOM_CAMERA:
	case EGenScriptAction::A_PITCH_CAMERA:
	case EGenScriptAction::A_ROTATE_CAMERA:
		Client(A);
		NoteCameraBusyUntil(Frame + uint32(FMath::Max(0.f, Real(P, 1)) * float(FPS)) + 1);
		break;
	case EGenScriptAction::A_SPEECH_PLAY:
	case EGenScriptAction::A_MUSIC_SET_VOLUME:
	case EGenScriptAction::A_SOUND_SET_VOLUME:
	case EGenScriptAction::A_PLAY_SOUND_EFFECT:
	case EGenScriptAction::A_SOUND_DISABLE_TYPE:
	case EGenScriptAction::A_SOUND_ENABLE_TYPE:
	case EGenScriptAction::A_SOUND_REMOVE_ALL_DISABLED:
	case EGenScriptAction::A_SOUND_ENABLE_ALL:
	case EGenScriptAction::A_AUDIO_OVERRIDE_VOLUME_TYPE:
	case EGenScriptAction::A_AUDIO_RESTORE_VOLUME_ALL_TYPE:
	case EGenScriptAction::A_EVA_SET_ENABLED_DISABLED:
	case EGenScriptAction::A_SHOW_MILITARY_CAPTION:
	case EGenScriptAction::A_SCREEN_SHAKE:
	case EGenScriptAction::A_CAMERA_MOD_SET_FINAL_ZOOM:
	case EGenScriptAction::A_CAMERA_MOD_SET_FINAL_PITCH:
	case EGenScriptAction::A_CAMERA_LETTERBOX_BEGIN:
	case EGenScriptAction::A_CAMERA_LETTERBOX_END:
	case EGenScriptAction::A_CAMERA_BW_MODE_BEGIN:
	case EGenScriptAction::A_CAMERA_BW_MODE_END:
	case EGenScriptAction::A_CAMERA_MOD_FREEZE_ANGLE:
	case EGenScriptAction::A_SOUND_REMOVE_TYPE:
	case EGenScriptAction::A_CAMERA_MOD_LOOK_TOWARD:
		// A camera mod turns toward a waypoint: the point is resolved here, the turn is the client's.
		Client(A, &Str(P, 0));
		break;

	// --- the GLA missions ---
	case EGenScriptAction::A_RESTART_TIMER:
	{
		// ScriptEngine::restartTimer
		FCounter& Timer = Counters.FindOrAdd(Str(P, 0));
		if (Timer.Value > 0)
		{
			Timer.bCountdown = true;
		}
		break;
	}
	case EGenScriptAction::A_SET_DEFAULT_ATTACK_PRIORITY:
	{
		// ScriptEngine::setPriorityDefault
		FGenAttackPriority& Set = Logic->FindOrAddAttackPriority(Str(P, 0));
		Set.DefaultPriority = Int(P, 1);
		Set.MaxPriority = FMath::Max(Set.MaxPriority, Int(P, 1));
		break;
	}
	case EGenScriptAction::A_TEAM_AVAILABLE_FOR_RECRUITMENT:
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			Team->Recruitable = Int(P, 1) != 0 ? 1 : 0;
		}
		break;
	case EGenScriptAction::A_TEAM_SET_ATTITUDE:
		// updateTeamSetAttitude: AIGroup::setAttitude. Moods only move what the computer plays.
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (Logic->GetPlayers().IsValidIndex(O->GetOwner()) && !Logic->GetPlayers()[O->GetOwner()].bHuman)
				{
					O->ScriptAttitude = int8(FMath::Clamp(Int(P, 1), -2, 2));
				}
			}
		}
		break;
	case EGenScriptAction::A_TEAM_ATTACK_TEAM:
	{
		// doAttack: AIGroup::groupAttackTeam. (Each goes for the one of them nearest it.)
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenMapTeam* Victims = GetTeamNamed(Str(P, 1));
		if (!Team || !Victims)
		{
			break;
		}
		TArray<FGenObject*> Members, Targets;
		GetTeamMembers(*Team, Members);
		GetTeamMembers(*Victims, Targets);
		for (FGenObject* O : Members)
		{
			const FGenObject* Best = nullptr;
			float BestDistSq = FLT_MAX;
			for (const FGenObject* Them : Targets)
			{
				const float DistSq = (Them->GetPosition() - O->GetPosition()).SizeSquared();
				if (DistSq < BestDistSq) { BestDistSq = DistSq; Best = Them; }
			}
			if (Best && O->bHasAI && O->HasWeapons())
			{
				O->ClearAIMode();
				O->CommandAttack(Best->GetID(), true);
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_WANDER:
		// doTeamWander: AIWanderState, each along the path from its nearest waypoint. (On its way, not wide of it.)
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				FollowWaypoints(*O, Str(P, 1), false);
			}
		}
		break;
	case EGenScriptAction::A_UNIT_MOVE_TOWARDS_NEAREST_OBJECT_TYPE:
	{
		// doMoveUnitTowardsNearest: to the nearest of that type inside the area.
		FGenObject* Unit = GetUnitNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 2));
		if (!IsLiving(Unit) || !Area || !Unit->bHasAI)
		{
			break;
		}
		const TArray<const FGenThingTemplate*>& Wanted = ResolveTypes(Str(P, 1));
		const FGenObject* Best = nullptr;
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (IsLiving(O) && Wanted.Contains(O->GetTemplate()->Canonical()) && Area->Contains(O->GetPosition().X, O->GetPosition().Y))
			{
				const float DistSq = (O->GetPosition() - Unit->GetPosition()).SizeSquared();
				if (DistSq < BestDistSq) { BestDistSq = DistSq; Best = O; }
			}
		}
		if (Best)
		{
			Unit->ClearAIMode();
			Unit->CommandMoveTo(Best->GetPosition());
		}
		break;
	}
	case EGenScriptAction::A_TEAM_CAPTURE_NEAREST_UNOWNED_FACTION_UNIT:
	{
		// doTeamCaptureNearestUnownedFactionUnit: the whole team goes for the vehicle with nobody in it nearest them.
		FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		FVector2f Center;
		if (!Team || !GetTeamCenter(*Team, Center))
		{
			break;
		}
		FGenObject* Best = nullptr;
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			FGenObject* O = Logic->FindObject(Id);
			if (IsLiving(O) && O->IsUnmanned() && O->GetOwner() != Team->Player)
			{
				const float DistSq = FVector2f::DistSquared(Center, FVector2f(O->GetPosition().X, O->GetPosition().Y));
				if (DistSq < BestDistSq) { BestDistSq = DistSq; Best = O; }
			}
		}
		if (Best)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (O->CanMove())
				{
					O->ClearAIMode();
					O->CommandEnter(*Best);
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_TEAM_PARTIAL_USE_COMMANDBUTTON:
		// doTeamPartialUseCommandButton: that share of the members the button is any use to, rounded down.
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 1)))
		{
			TArray<FGenObject*> Members, Able;
			GetTeamMembers(*Team, Members);
			for (FGenObject* O : Members)
			{
				if (ObjectHasButton(*O, Str(P, 2))) { Able.Add(O); }
			}
			const int32 Num = int32(Real(P, 0) / 100.f * float(Able.Num()));
			for (int32 i = 0; i < Num; ++i)
			{
				UseCommandButton(*Able[i], Str(P, 2), nullptr, nullptr);
			}
		}
		break;
	case EGenScriptAction::A_SKIRMISH_WAIT_FOR_COMMANDBUTTON_AVAILABLE_ALL:
		break; // ("should never be used outside of Sequential scripts")
	case EGenScriptAction::A_NAMED_STOP:
		if (FGenObject* O = GetUnitNamed(Str(P, 0)); IsLiving(O) && O->bHasAI)
		{
			IdleUnit(*O);
		}
		break;
	case EGenScriptAction::A_NAMED_FACE_WAYPOINT:
	case EGenScriptAction::A_TEAM_FACE_WAYPOINT:
	{
		// doNamedFaceWaypoint / doTeamFaceWaypoint: aiFacePosition. (They stop and are turned that way at once, as for
		// TEAM_FACE_NAMED; only a cinematic asks.)
		FVector3f Pos;
		if (!GetWaypoint(Str(P, 1), Pos))
		{
			break;
		}
		TArray<FGenObject*> Members;
		if (A.Type == EGenScriptAction::A_NAMED_FACE_WAYPOINT)
		{
			if (FGenObject* O = GetUnitNamed(Str(P, 0))) { Members.Add(O); }
		}
		else if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			GetTeamMembers(*Team, Members);
		}
		for (FGenObject* O : Members)
		{
			if (O->CanMove())
			{
				IdleUnit(*O);
				const FVector3f To = Pos - O->GetPosition();
				O->SetOrientation(FMath::Atan2(To.Y, To.X));
			}
		}
		break;
	}
	case EGenScriptAction::A_NAMED_USE_COMMANDBUTTON_ABILITY_USING_WAYPOINT_PATH:
		// doNamedUseCommandButtonAbilityUsingWaypointPath. (The only user is the particle cannon at the end of GLA
		// mission 3; here it fires at the path's nearest waypoint and stays there instead of sweeping along it.)
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			if (const FGenMapObject* Way = GetClosestWaypointOnPath(FVector2f(O->GetPosition().X, O->GetPosition().Y), Str(P, 2)))
			{
				const FVector3f Pos(Way->Location.X, Way->Location.Y, Logic->GetGroundHeight(Way->Location.X, Way->Location.Y));
				UseCommandButton(*O, Str(P, 1), nullptr, &Pos);
			}
		}
		break;
	case EGenScriptAction::A_NAMED_SET_UNMANNED_STATUS:
		// doNamedSetUnmanned: no driver, and nobody's.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->MakeUnmanned(GenInvalidObjectID);
			const int32* DefaultId = DefaultTeams.Find(Logic->GetNeutralPlayer());
			if (FGenMapTeam* NeutralTeam = DefaultId ? FindTeam(*DefaultId) : nullptr)
			{
				SetObjectTeam(*O, *NeutralTeam);
			}
		}
		break;
	case EGenScriptAction::A_DELETE_ALL_UNMANNED:
		// deleteAllUnmanned
		for (FGenObjectID Id : TArray<FGenObjectID>(Logic->GetObjectIDs()))
		{
			if (FGenObject* O = Logic->FindObject(Id); O && !O->IsDestroyed() && O->IsUnmanned())
			{
				Logic->RemoveObjectByScript(*O);
			}
		}
		break;
	case EGenScriptAction::A_NAMED_CUSTOM_COLOR:
		// doNamedCustomColor: Object::setCustomIndicatorColor.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			const uint32 ARGB = uint32(Int(P, 1));
			O->CustomIndicatorColor = FLinearColor(FColor(uint8((ARGB >> 16) & 0xff), uint8((ARGB >> 8) & 0xff), uint8(ARGB & 0xff)));
			O->bCustomIndicatorColor = true;
		}
		break;
	case EGenScriptAction::A_NAMED_SET_TOPPLE_DIRECTION:
		// doSetToppleDirection: kept by name, for when the thing falls.
		if (P.IsValidIndex(1))
		{
			ToppleDirections.Add(Str(P, 0), FVector2f(P[1].Coord.X, P[1].Coord.Y).GetSafeNormal());
		}
		break;
	case EGenScriptAction::A_NAMED_SET_REPULSOR:
		break; // (OBJECT_STATUS_REPULSOR scatters what wanders nearby. Nothing wanders that way here: only an intro asks.)
	case EGenScriptAction::A_TEAM_FLASH:
	case EGenScriptAction::A_TEAM_FLASH_WHITE:
		// doTeamFlash: white, or each in its own colour.
		if (const FGenMapTeam* Team = GetTeamNamed(Str(P, 0)); Team && Int(P, 1) > 0)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(*Team, Members);
			for (const FGenObject* O : Members)
			{
				Flashing.Add(O->GetID(), FMath::Max(1, Int(P, 1) * FPS / 15));
				if (A.Type == EGenScriptAction::A_TEAM_FLASH_WHITE) { FlashColors.Remove(O->GetID()); }
				else if (O->bCustomIndicatorColor) { FlashColors.Add(O->GetID(), O->CustomIndicatorColor); }
				else if (Logic->GetPlayers().IsValidIndex(O->GetOwner())) { FlashColors.Add(O->GetID(), Logic->GetPlayers()[O->GetOwner()].Color); }
			}
		}
		break;
	case EGenScriptAction::A_PLAYER_ADD_RANKLEVEL:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE)
		{
			Logic->SetRankLevel(Player, Logic->GetPlayers()[Player].RankLevel + Int(P, 1));
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_HUNT:
	{
		// Player::setUnitsShouldHunt: everything of the player's but the workers.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			break;
		}
		const TArray<FGenObject*> Things(Logic->GetObjectsOf(Player));
		for (FGenObject* O : Things)
		{
			if (IsLiving(O) && O->bHasAI && O->CanMove() && O->HasWeapons() && !O->IsKindOf(TEXT("DOZER")) && !O->IsKindOf(TEXT("HARVESTER"))
				&& !O->IsKindOf(TEXT("IGNORES_SELECT_ALL")))
			{
				O->CommandHunt();
			}
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SELL_EVERYTHING:
	{
		// Player::sellEverythingUnderTheSun
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			break;
		}
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			if (IsLiving(O) && O->IsKindOf(TEXT("STRUCTURE")) && !O->GetTemplate()->bTechBuilding && !O->IsBeingSold())
			{
				FGenCommand Sell(EGenCommandType::Sell);
				Sell.Player = Player;
				Sell.Units = { O->GetID() };
				Logic->QueueSimCommand(MoveTemp(Sell));
			}
		}
		break;
	}
	case EGenScriptAction::A_RESUME_SUPPLY_TRUCKING:
		// doResumeSupplyTruckingForIdleUnits: every human player's idle trucks go back to gathering.
		for (const FGenPlayer& Pl : Logic->GetPlayers())
		{
			if (Pl.bHuman)
			{
				for (FGenObject* O : Logic->GetObjectsOf(Pl.Index))
				{
					if (IsLiving(O)) { O->ResumeSupplyTrucking(); }
				}
			}
		}
		break;
	case EGenScriptAction::A_MAP_SHROUD_AT_WAYPOINT:
	{
		// doShroudMapAtWaypoint: that player, or (no name) every human player.
		FVector3f Pos;
		const int32 Player = Str(P, 2).IsEmpty() ? INDEX_NONE : GetPlayerNamed(Str(P, 2));
		if (GetWaypoint(Str(P, 0), Pos))
		{
			for (const FGenPlayer& Pl : Logic->GetPlayers())
			{
				if (Player != INDEX_NONE ? Pl.Index == Player : Pl.bHuman)
				{
					Logic->ScriptShroudOnce(Pl.Index, FVector2f(Pos.X, Pos.Y), Real(P, 1));
				}
			}
		}
		break;
	}
	case EGenScriptAction::A_WATER_CHANGE_HEIGHT_OVER_TIME:
	{
		// doWaterChangeHeightOverTime (with no damage asked for, in the one mission that does this).
		float From = 0.f;
		if (Logic->GetScriptWaterHeight(Str(P, 0), From))
		{
			const FString Name = Str(P, 0);
			WaterMoves.RemoveAll([&Name](const FWaterMove& M) { return M.Name == Name; });
			WaterMoves.Add({ Name, From, Real(P, 1), Frame, FMath::Max(1, int32(Real(P, 2) * float(FPS))) });
		}
		break;
	}
	case EGenScriptAction::A_DISPLAY_COUNTER:
		NamedTimers.Add(Str(P, 0), Str(P, 1));
		NamedCounters.Add(Str(P, 0));
		break;
	case EGenScriptAction::A_HIDE_COUNTER:
		NamedTimers.Remove(Str(P, 0));
		NamedCounters.Remove(Str(P, 0));
		break;
	case EGenScriptAction::A_DISABLE_SPECIAL_POWER_DISPLAY:
	case EGenScriptAction::A_ENABLE_SPECIAL_POWER_DISPLAY:
		bPowerDisplaysHidden = A.Type == EGenScriptAction::A_DISABLE_SPECIAL_POWER_DISPLAY;
		break;
	case EGenScriptAction::A_CAMEO_FLASH:
		// doCameoFlash: for that many seconds.
		CameoFlashes.Add(Str(P, 0), Frame + uint32(FMath::Max(0, Int(P, 1)) * FPS));
		break;
	case EGenScriptAction::A_RADAR_ENABLE: // (Radar::hide(false): nothing here ever hides it.)
	case EGenScriptAction::A_DEBUG_MESSAGE_BOX: // (Only in the original's debug build.)
		break;
	case EGenScriptAction::A_SOUND_PLAY_NAMED:
		// doSoundPlayFromNamed: where the unit is.
		if (const FGenObject* O = GetUnitNamed(Str(P, 1)))
		{
			Client(A);
			ClientEvents.Last().Pos = O->GetPosition();
			ClientEvents.Last().bHasPos = true;
		}
		break;
	case EGenScriptAction::A_CAMERA_ADD_SHAKER_AT:
	case EGenScriptAction::A_CAMERA_MOD_FINAL_LOOK_TOWARD:
		Client(A, &Str(P, 0));
		break;
	case EGenScriptAction::A_CAMERA_LOOK_TOWARD_OBJECT:
		// doRotateCameraTowardObject: it turns, then keeps looking for the seconds it holds.
		if (const FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			Client(A);
			ClientEvents.Last().Pos = O->GetPosition();
			ClientEvents.Last().bHasPos = true;
			NoteCameraBusyUntil(Frame + uint32(FMath::Max(0.f, Real(P, 1) + Real(P, 2)) * float(FPS)) + 1);
		}
		break;
	case EGenScriptAction::A_MOVE_CAMERA_ALONG_WAYPOINT_PATH:
		Client(A, &Str(P, 0));
		if (const FGenMapObject* Way = Map ? Map->FindWaypoint(Str(P, 0)) : nullptr)
		{
			GetWaypointPath(*Way, ClientEvents.Last().Path);
		}
		NoteCameraBusyUntil(Frame + uint32(FMath::Max(0.f, Real(P, 1)) * float(FPS)) + 1);
		break;

	default:
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] action %s isn't ported: nothing"), Frame, *A.TypeName);
		break;
	}
}
