// The actions of a map's scripts (ScriptActions.cpp). What is only seen or heard goes to the game thread as a
// client event (FGenMapScripts::Client); everything that changes the game is done here, inside the step.

#include "Logic/GenMapScripts.h"
#include "Logic/GenCommandData.h"
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
	GetWaypointPath(*Way, Path);
	if (Path.Num() == 0)
	{
		return;
	}
	Unit.ClearAIMode();
	if (bExact)
	{
		Unit.CommandFollowPathExact(Path);
	}
	else
	{
		Unit.CommandFollowPath(Path);
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
		return;
	}
	bool bInSet = false;
	for (const FString& Slot : Set->Slots)
	{
		bInSet |= Slot.Equals(ButtonName, ESearchCase::IgnoreCase);
	}
	if (!bInSet)
	{
		return;
	}
	switch (Button->Command)
	{
	case EGenCommand::SpecialPower:
	{
		const int32 Module = Logic->FindPowerModule(Obj, Button->SpecialPower);
		if (Module != INDEX_NONE)
		{
			const FVector3f At = Pos ? *Pos : (Target ? Target->GetPosition() : FVector3f(FLT_MAX, 0.f, 0.f));
			Logic->FireSpecialPower(Obj, Module, At, Target ? Target->GetID() : GenInvalidObjectID, true);
		}
		break;
	}
	case EGenCommand::FireWeapon:
		if (Target && Button->WeaponSlot >= 0 && Button->WeaponSlot < 3 && Obj.GetWeapon(Button->WeaponSlot).Template)
		{
			Obj.ClearAIMode();
			Obj.CommandAttackWithSlot(Target->GetID(), Button->WeaponSlot);
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
	FGenObject* O = Logic->CreateObject(T, Team->Player, FVector3f(Pos.X, Pos.Y, Logic->GetGroundHeight(Pos.X, Pos.Y)), Angle);
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
	// doCreateReinforcements: a new team of the definition's units at its origin (or the waypoint), in a row; with a
	// transport they ride in it and it carries them to the waypoint; without one they move there.
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
	const FGenGameData& GD = FGenGameData::Get();
	FGenObject* Transport = nullptr;
	if (const FGenThingTemplate* TT = GD.FindTemplate(Proto.TransportUnitType))
	{
		Transport = Logic->CreateObject(TT, Team.Player, Origin, 0.f);
		if (Transport)
		{
			SetObjectTeam(*Transport, Team);
		}
	}
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
	TArray<FGenObjectID> Movers;
	for (FGenObject* U : Units)
	{
		if (Transport && Transport->GetContain() && Logic->CanEnterObject(*U, *Transport, true))
		{
			Logic->AddToContain(*Transport, *U);
		}
		else
		{
			Movers.Add(U->GetID());
		}
	}
	if (Transport)
	{
		Transport->ClearAIMode();
		Transport->CommandMoveTo(Destination);
	}
	if (Movers.Num() > 0 && (bMove || Transport))
	{
		Logic->GroupMoveToPosition(Movers, Destination, false);
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] reinforcements %s: %d units%s at (%.0f, %.0f) for (%.0f, %.0f)"), Frame, *TeamName, Units.Num(),
		Transport ? TEXT(" in a transport") : TEXT(""), Origin.X, Origin.Y, Destination.X, Destination.Y);
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
		if (FGenMapTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (const FGenScript* S = FindScript(Str(P, 1)))
			{
				FSequential& Seq = SequentialScripts.AddDefaulted_GetRef();
				Seq.Team = Team->Id;
				Seq.Script = S;
			}
		}
		break;

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
					Ids.Add(O->GetID());
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
	case EGenScriptAction::A_DISABLE_OBJECT_SOUND:
		// doEnableObjectSound(false): the object's ambient sound stops.
		if (FGenObject* O = GetUnitNamed(Str(P, 0)))
		{
			O->bFromMap = true;
			O->MapAmbientEnabled = 0;
		}
		break;
	case EGenScriptAction::A_SCRIPTING_OVERRIDE_HULK_LIFETIME:
		HulkLifetimeOverride = Real(P, 0) < 0.f ? -1 : int32(Real(P, 0) * float(FPS));
		break;
	case EGenScriptAction::A_OBJECT_ALLOW_BONUSES:
		break; // (the difficulty bonuses of the computer's units aren't ported)

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
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SCIENCE_AVAILABILITY:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player != INDEX_NONE)
		{
			TSet<FString>& Set = HiddenSciences.FindOrAdd(Player);
			if (Str(P, 2).Equals(TEXT("Available"), ESearchCase::IgnoreCase)) { Set.Remove(Str(P, 1).ToLower()); }
			else { Set.Add(Str(P, 1).ToLower()); }
		}
		break;
	}
	case EGenScriptAction::A_PLAYER_SET_RANKLEVELLIMIT:
		RankLevelLimit = Int(P, 0);
		Logic->SetRankLevelLimit(RankLevelLimit);
		break;
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
	case EGenScriptAction::A_DISABLE_COUNTDOWN_TIMER_DISPLAY:
	case EGenScriptAction::A_OPTIONS_SET_OCCLUSION_MODE:
	case EGenScriptAction::A_OPTIONS_SET_PARTICLE_CAP_MODE:
		Client(A);
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
		Client(A);
		break;

	default:
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] action %s isn't ported: nothing"), Frame, *A.TypeName);
		break;
	}
}
