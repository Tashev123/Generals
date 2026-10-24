// The part of the script engine the skirmish computer player's scripts need.
// Follows ScriptEngine.cpp (evaluation order, flags / counters / timers, subroutines, sequential
// scripts), ScriptConditions.cpp and ScriptActions.cpp. Each computer player has its own copy of its
// faction's scripts and its own flags, counters and timers.

#include "Logic/GenSkirmishAI.h"
#include "Core/GenPerf.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	const TCHAR* const ThisTeamName = TEXT("<This Team>");
	const TCHAR* const ThisPlayerName = TEXT("<This Player>");
	const TCHAR* const ThisPlayerEnemyName = TEXT("<This Player's Enemy>");
	const TCHAR* const LocalPlayerName = TEXT("<Local Player>");

	/** Parameter::LESS_THAN .. NOT_EQUAL */
	template<typename T>
	bool Compare(T A, int32 Comparison, T B)
	{
		switch (Comparison)
		{
		case 0: return A < B;
		case 1: return A <= B;
		case 2: return A == B;
		case 3: return A >= B;
		case 4: return A > B;
		case 5: return A != B;
		default: return false;
		}
	}

	bool IsLiving(const FGenObject* O)
	{
		return O && !O->IsDead() && !O->IsDestroyed();
	}

	/** PolygonTrigger::getCenterPoint / getRadius: middle of the bounds, and half their diagonal. */
	void TriggerCenterAndRadius(const FGenPolygonTrigger& T, FVector2f& OutCenter, float& OutRadius)
	{
		FBox2f Bounds(ForceInit);
		for (const FIntVector& P : T.Points)
		{
			Bounds += FVector2f(float(P.X), float(P.Y));
		}
		OutCenter = Bounds.GetCenter();
		OutRadius = Bounds.GetSize().Size() * 0.5f;
	}

	bool IsTeamCondition(EGenScriptCondition Type)
	{
		return Type == EGenScriptCondition::C_TEAM_HAS_UNITS || Type == EGenScriptCondition::C_TEAM_INSIDE_AREA_PARTIALLY
			|| Type == EGenScriptCondition::C_TEAM_DESTROYED || Type == EGenScriptCondition::C_TEAM_ENTERED_AREA_ENTIRELY
			|| Type == EGenScriptCondition::C_TEAM_OUTSIDE_AREA_ENTIRELY || Type == EGenScriptCondition::C_TEAM_SOME_HAVE_OBJECT_STATUS;
	}

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
}

// ---- setup --------------------------------------------------------------------------------------

void FGenSkirmishAI::InitScripts()
{
	FGenScriptLibrary& Lib = FGenScriptLibrary::Get();
	if (!Lib.Initialize())
	{
		return;
	}
	const FString PlayerName = TEXT("Skirmish") + Side;
	if (const FGenScriptList* List = Lib.FindList(PlayerName))
	{
		Scripts = *List;
	}
	else
	{
		UE_LOG(LogGenerals, Warning, TEXT("AI player %d: no skirmish scripts for '%s'"), Player, *PlayerName);
	}
	Lib.GetTeams(PlayerName, Protos);
	Teams[0]->Proto = FindProto(TEXT("team") + PlayerName);

	const int32 FPS = GenLogic::GetLogicFPS();
	auto Index = [this, FPS](FGenScript& S)
	{
		ScriptsByName.Add(S.Name, &S);
		// ScriptEngine::checkConditionsForTeamNames: periodic scripts start at a random offset; a script
		// whose conditions name a team is evaluated once per instance of that team.
		S.FrameToEvaluate = S.DelaySeconds > 0 ? uint32(Logic->GetRandom().RandRange(0, 2 * FPS)) : 0;
		FString Singleton, Multi;
		for (const TArray<FGenScriptCondition>& And : S.Conditions)
		{
			for (const FGenScriptCondition& C : And)
			{
				const int32 ProtoIndex = IsTeamCondition(C.Type) ? FindProto(Str(C.Params, 0)) : INDEX_NONE;
				if (ProtoIndex == INDEX_NONE)
				{
					continue;
				}
				if (Protos[ProtoIndex].bSingleton || Protos[ProtoIndex].MaxInstances < 2) { Singleton = Protos[ProtoIndex].Name; }
				else if (Multi.IsEmpty()) { Multi = Protos[ProtoIndex].Name; }
			}
		}
		S.ConditionTeam = Multi.IsEmpty() ? Singleton : Multi;
	};
	for (FGenScript& S : Scripts.Scripts)
	{
		Index(S);
	}
	for (FGenScriptGroup& G : Scripts.Groups)
	{
		GroupsByName.Add(G.Name, &G);
		for (FGenScript& S : G.Scripts)
		{
			Index(S);
		}
	}
}

FGenScript* FGenSkirmishAI::FindScript(const FString& Name)
{
	FGenScript** Found = ScriptsByName.Find(Name);
	return Found ? *Found : nullptr;
}

FGenScriptGroup* FGenSkirmishAI::FindGroup(const FString& Name)
{
	FGenScriptGroup** Found = GroupsByName.Find(Name);
	return Found ? *Found : nullptr;
}

// ---- evaluation ---------------------------------------------------------------------------------

namespace
{
	// -GenPerf: what the conditions cost, by kind (all computer players together).
	double GConditionSeconds[256];
	int32 GConditionCalls[256];
	uint32 GConditionReportFrame = 0;
	int32 GScriptsEvaluated = 0, GScriptsSkipped = 0;

	const TCHAR* ConditionName(int32 Type)
	{
		static const TCHAR* Names[] = {
			TEXT("Unknown"),
#define GEN_X(Name) TEXT(#Name),
			GEN_SCRIPT_CONDITIONS(GEN_X)
#undef GEN_X
		};
		return Type >= 0 && Type < UE_ARRAY_COUNT(Names) ? Names[Type] : TEXT("?");
	}

	void ReportConditions(uint32 Frame)
	{
		if (Frame < GConditionReportFrame + 900)
		{
			return;
		}
		const double Steps = double(FMath::Max<uint32>(1, Frame - GConditionReportFrame));
		GConditionReportFrame = Frame;
		TArray<int32> Order;
		for (int32 i = 0; i < 256; ++i)
		{
			if (GConditionCalls[i] > 0) { Order.Add(i); }
		}
		Order.Sort([](int32 A, int32 B) { return GConditionSeconds[A] > GConditionSeconds[B]; });
		FString Line;
		for (int32 i = 0; i < FMath::Min(Order.Num(), 14); ++i)
		{
			const int32 T = Order[i];
			Line += FString::Printf(TEXT(" %s=%.3f(x%.0f)"), ConditionName(T), GConditionSeconds[T] * 1000.0 / Steps, double(GConditionCalls[T]) / Steps);
		}
		UE_LOG(LogGenerals, Log, TEXT("  [perf f%u] script conditions, ms per step (all players): scripts evaluated x%.0f, waiting x%.0f;%s"),
			Frame, double(GScriptsEvaluated) / Steps, double(GScriptsSkipped) / Steps, *Line);
		FMemory::Memzero(GConditionSeconds);
		FMemory::Memzero(GConditionCalls);
		GScriptsEvaluated = GScriptsSkipped = 0;
	}
}

void FGenSkirmishAI::UpdateScripts()
{
	GEN_PERF_SCOPE("AI.Scripts");
	if (GenPerf::bEnabled)
	{
		ReportConditions(Frame);
	}
	// ScriptEngine::update: timers count down (to -1), every active script that isn't a subroutine is
	// evaluated, then teams get their state scripts and sequential scripts advance.
	for (TPair<FString, FCounter>& It : Counters)
	{
		if (It.Value.bCountdown && It.Value.Value >= 0)
		{
			--It.Value.Value;
		}
	}
	ExecuteScripts(Scripts.Scripts);
	for (FGenScriptGroup& G : Scripts.Groups)
	{
		if (G.bActive && !G.bSubroutine)
		{
			ExecuteScripts(G.Scripts);
		}
	}
	UpdateTeamStates();
	UpdateSequentialScripts();
}

void FGenSkirmishAI::ExecuteScripts(TArray<FGenScript>& InScripts)
{
	for (FGenScript& S : InScripts)
	{
		if (!S.bSubroutine)
		{
			Logic->SetStretchDetail(S.Name);
			ExecuteScript(S);
			Logic->SimYield(TEXT("a computer player's script"));
		}
	}
}

void FGenSkirmishAI::ExecuteScript(FGenScript& Script)
{
	if (!Script.bActive)
	{
		return;
	}
	if ((Difficulty == EGenDifficulty::Easy && !Script.bEasy) || (Difficulty == EGenDifficulty::Normal && !Script.bNormal)
		|| (Difficulty == EGenDifficulty::Hard && !Script.bHard))
	{
		return;
	}
	if (Frame < Script.FrameToEvaluate)
	{
		++GScriptsSkipped;
		return;
	}
	++GScriptsEvaluated;
	TGuardValue<const FGenScript*> Running(RunningScript, &Script);
	if (Script.DelaySeconds > 0)
	{
		Script.FrameToEvaluate = Frame + uint32(Script.DelaySeconds * GenLogic::GetLogicFPS());
	}

	FGenTeam* SavedConditionTeam = ConditionTeam;
	TArray<int32, TInlineAllocator<8>> Instances;
	const int32 ProtoIndex = Script.ConditionTeam.IsEmpty() ? INDEX_NONE : FindProto(Script.ConditionTeam);
	if (ProtoIndex != INDEX_NONE)
	{
		for (const TUniquePtr<FGenTeam>& T : Teams)
		{
			if (T->Proto == ProtoIndex)
			{
				Instances.Add(T->Id);
			}
		}
	}
	if (Instances.Num() > 0)
	{
		// The conditions refer to a team: they (and the actions) apply to each instance of it.
		for (int32 TeamId : Instances)
		{
			ConditionTeam = FindTeam(TeamId);
			if (!ConditionTeam)
			{
				continue;
			}
			if (EvaluateConditions(Script, nullptr))
			{
				ExecuteActions(Script.Actions);
				if (Script.bOneShot)
				{
					Script.bActive = false;
				}
			}
			else if (Script.FalseActions.Num() > 0)
			{
				ExecuteActions(Script.FalseActions);
			}
		}
	}
	else
	{
		ConditionTeam = nullptr;
		if (EvaluateConditions(Script, nullptr))
		{
			ExecuteActions(Script.Actions);
			if (Script.bOneShot)
			{
				Script.bActive = false;
			}
		}
		else if (Script.FalseActions.Num() > 0)
		{
			ExecuteActions(Script.FalseActions);
			if (Script.bOneShot)
			{
				Script.bActive = false;
			}
		}
	}
	ConditionTeam = SavedConditionTeam;
}

bool FGenSkirmishAI::EvaluateConditions(const FGenScript& Script, FGenTeam* ThisTeam)
{
	// OR of ANDs. While conditions are evaluated <This Team> is the team given (if any), else the
	// team the script is being evaluated for.
	TGuardValue<FGenTeam*> Guard(CallingTeam, ThisTeam);
	for (const TArray<FGenScriptCondition>& And : Script.Conditions)
	{
		if (And.Num() == 0)
		{
			continue;
		}
		bool bAll = true;
		for (const FGenScriptCondition& C : And)
		{
			bool bTrue;
			if (GenPerf::bEnabled)
			{
				const double Start = FPlatformTime::Seconds();
				bTrue = EvaluateCondition(C);
				GConditionSeconds[uint8(C.Type)] += FPlatformTime::Seconds() - Start;
				++GConditionCalls[uint8(C.Type)];
			}
			else
			{
				bTrue = EvaluateCondition(C);
			}
			if (!bTrue)
			{
				bAll = false;
				break;
			}
		}
		if (bAll)
		{
			return true;
		}
	}
	return false;
}

bool FGenSkirmishAI::EvaluateProductionCondition(FGenTeamProto& Proto)
{
	// TeamPrototype::evaluateProductionCondition
	if (Proto.bConditionAlwaysFalse)
	{
		return false;
	}
	if (!Proto.ConditionScript)
	{
		const FGenScript* Script = Proto.ProductionCondition.IsEmpty() ? nullptr : FindScript(Proto.ProductionCondition);
		const bool bLevel = Script && ((Difficulty == EGenDifficulty::Easy && Script->bEasy) || (Difficulty == EGenDifficulty::Normal && Script->bNormal)
			|| (Difficulty == EGenDifficulty::Hard && Script->bHard));
		if (!bLevel)
		{
			Proto.bConditionAlwaysFalse = true;
			return false;
		}
		Proto.ConditionScript = Script;
	}
	if (Frame < Proto.ConditionFrameToEvaluate)
	{
		return false;
	}
	if (Proto.ConditionScript->DelaySeconds > 0)
	{
		Proto.ConditionFrameToEvaluate = Frame + uint32(Proto.ConditionScript->DelaySeconds * GenLogic::GetLogicFPS());
	}
	return EvaluateConditions(*Proto.ConditionScript, nullptr);
}

void FGenSkirmishAI::ExecuteActions(const TArray<FGenScriptAction>& Actions)
{
	for (int32 i = 0; i < Actions.Num(); ++i)
	{
		ExecuteAction(Actions[i]);
	}
}

void FGenSkirmishAI::ExecuteActionsForTeam(const TArray<FGenScriptAction>& Actions, FGenTeam* ThisTeam)
{
	// ScriptEngine::friend_executeAction
	TGuardValue<FGenTeam*> Guard(CallingTeam, ThisTeam);
	ExecuteActions(Actions);
}

void FGenSkirmishAI::CallSubroutine(const FString& ScriptName)
{
	if (SubroutineDepth > 16)
	{
		return;
	}
	TGuardValue<int32> Depth(SubroutineDepth, SubroutineDepth + 1);
	if (FGenScriptGroup* Group = FindGroup(ScriptName))
	{
		if (Group->bSubroutine && Group->bActive)
		{
			ExecuteScripts(Group->Scripts);
		}
	}
	else if (FGenScript* Script = FindScript(ScriptName))
	{
		if (Script->bSubroutine)
		{
			ExecuteScript(*Script);
		}
	}
}

void FGenSkirmishAI::RunScript(const FString& ScriptName, FGenTeam* ThisTeam)
{
	// ScriptEngine::runScript: a subroutine script (or group) run on behalf of a team.
	if (ScriptName.IsEmpty() || ScriptName == TEXT("<none>"))
	{
		return;
	}
	TGuardValue<FGenTeam*> Guard(CallingTeam, ThisTeam);
	ConditionTeam = nullptr;
	CallSubroutine(ScriptName);
}

// ---- names --------------------------------------------------------------------------------------

FGenTeam* FGenSkirmishAI::GetTeamNamed(const FString& Name)
{
	// ScriptEngine::getTeamNamed
	if (Name == ThisTeamName)
	{
		return CallingTeam ? CallingTeam : ConditionTeam;
	}
	auto IsNamed = [this, &Name](const FGenTeam* T) { return T && Protos.IsValidIndex(T->Proto) && Protos[T->Proto].Name == Name; };
	if (IsNamed(CallingTeam)) { return CallingTeam; }
	if (IsNamed(ConditionTeam)) { return ConditionTeam; }
	const int32 ProtoIndex = FindProto(Name);
	if (ProtoIndex == INDEX_NONE)
	{
		return nullptr;
	}
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		if (T->Proto == ProtoIndex)
		{
			return (Protos[ProtoIndex].bSingleton && !T->bActive) ? nullptr : T.Get();
		}
	}
	return nullptr;
}

int32 FGenSkirmishAI::GetPlayerNamed(const FString& Name)
{
	if (Name == ThisPlayerName) { return Player; }
	if (Name == ThisPlayerEnemyName) { return GetEnemy(); }
	if (Name == LocalPlayerName) { return Logic->GetLocalPlayer(); }
	if (Name == TEXT("Skirmish") + Side) { return Player; }
	return INDEX_NONE;
}

const FGenPolygonTrigger* FGenSkirmishAI::GetTriggerNamed(const FString& InName)
{
	// ScriptEngine::getQualifiedTriggerAreaByName: "my" and "enemy" perimeters go by start position.
	const FGenMapData* Map = Logic->GetMap();
	if (!Map)
	{
		return nullptr;
	}
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	FString Name = InName;
	const bool bMyInner = InName == TEXT("[Skirmish]MyInnerPerimeter"), bMyOuter = InName == TEXT("[Skirmish]MyOuterPerimeter");
	const bool bEnemyInner = InName == TEXT("[Skirmish]EnemyInnerPerimeter"), bEnemyOuter = InName == TEXT("[Skirmish]EnemyOuterPerimeter");
	if (bMyInner || bMyOuter)
	{
		Name = FString::Printf(TEXT("%s%d"), bMyInner ? TEXT("InnerPerimeter") : TEXT("OuterPerimeter"), Players[Player].StartPosition + 1);
	}
	else if (bEnemyInner || bEnemyOuter)
	{
		const int32 Enemy = GetEnemy();
		Name = FString::Printf(TEXT("%s%d"), bEnemyInner ? TEXT("InnerPerimeter") : TEXT("OuterPerimeter"),
			Players.IsValidIndex(Enemy) ? Players[Enemy].StartPosition + 1 : -1);
	}
	return Map->FindTrigger(Name);
}

const TArray<const FGenThingTemplate*>& FGenSkirmishAI::ResolveTypes(const FString& TypeOrList) const
{
	if (const TArray<const FGenThingTemplate*>* Found = ResolvedTypes.Find(TypeOrList))
	{
		return *Found;
	}
	TArray<const FGenThingTemplate*>& Out = ResolvedTypes.Add(TypeOrList);
	if (const TArray<FString>* List = ObjectTypeLists.Find(TypeOrList))
	{
		for (const FString& Name : *List)
		{
			if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Name))
			{
				Out.AddUnique(T);
			}
		}
	}
	else if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(TypeOrList))
	{
		Out.Add(T);
	}
	return Out;
}

bool FGenSkirmishAI::IsInTypeList(const FString& TypeOrList, const FGenThingTemplate& Thing) const
{
	return ResolveTypes(TypeOrList).ContainsByPredicate([&Thing](const FGenThingTemplate* T) { return T->Canonical() == Thing.Canonical(); });
}

int32 FGenSkirmishAI::CountObjects(int32 PlayerIndex, const FString& TypeOrList, const FGenPolygonTrigger* Area) const
{
	// Player::countObjectsByThingTemplate: finished, living objects of those types (inside the area, if one is given).
	const TArray<const FGenThingTemplate*>& Types = ResolveTypes(TypeOrList);
	if (Types.Num() == 0)
	{
		return 0;
	}
	if (Area)
	{
		int32 Count = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(PlayerIndex))
		{
			if (IsLiving(O) && Types.Contains(O->GetTemplate()) && Area->Contains(O->GetPosition().X, O->GetPosition().Y))
			{
				++Count;
			}
		}
		return Count;
	}
	// Whole-map counts are asked for many times per frame: one pass over the objects per player and frame.
	FCountCache& Cache = CountCaches.FindOrAdd(PlayerIndex);
	if (Cache.Frame != Frame)
	{
		Cache.Frame = Frame;
		Cache.Counts.Reset();
		for (const FGenObject* O : Logic->GetObjectsOf(PlayerIndex))
		{
			if (IsLiving(O) && !O->IsUnderConstruction() && !O->IsProjectile())
			{
				++Cache.Counts.FindOrAdd(O->GetTemplate()->Canonical());
			}
		}
	}
	int32 Count = 0;
	for (const FGenThingTemplate* T : Types)
	{
		if (const int32* N = Cache.Counts.Find(T->Canonical()))
		{
			Count += *N;
		}
	}
	return Count;
}

// ---- conditions ---------------------------------------------------------------------------------

bool FGenSkirmishAI::EvaluateCondition(const FGenScriptCondition& C)
{
	const TArray<FGenScriptParam>& P = C.Params;
	switch (C.Type)
	{
	case EGenScriptCondition::C_CONDITION_TRUE:
		return true;
	case EGenScriptCondition::C_COUNTER:
		return Compare(Counters.FindOrAdd(Str(P, 0)).Value, Int(P, 1), Int(P, 2));
	case EGenScriptCondition::C_FLAG:
		return Flags.FindOrAdd(Str(P, 0)) == (Int(P, 1) != 0);
	case EGenScriptCondition::C_TIMER_EXPIRED:
	{
		const FCounter& Timer = Counters.FindOrAdd(Str(P, 0));
		return Timer.bCountdown && Timer.Value < 1; // a timer that was never started hasn't expired
	}

	case EGenScriptCondition::C_PLAYER_HAS_OBJECT_COMPARISON:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		return Who != INDEX_NONE && Compare(CountObjects(Who, Str(P, 3), nullptr), Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_BUILT_BY_PLAYER:
	{
		// (Only a real object type counts here: given the name of an object list this is always false.)
		const int32 Who = GetPlayerNamed(Str(P, 1));
		return Who != INDEX_NONE && FGenGameData::Get().FindTemplate(Str(P, 0)) && CountObjects(Who, Str(P, 0), nullptr) != 0;
	}
	case EGenScriptCondition::C_PLAYER_HAS_CREDITS:
	{
		// The original compares the given amount against the money, in that order.
		const int32 Who = GetPlayerNamed(Str(P, 2));
		return Who != INDEX_NONE && Compare(Int(P, 0), Int(P, 1), Logic->GetMoney(Who));
	}
	case EGenScriptCondition::C_START_POSITION_IS:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		return Who != INDEX_NONE && Logic->GetPlayers()[Who].StartPosition == Int(P, 1) - 1;
	}
	case EGenScriptCondition::C_SKIRMISH_PLAYER_FACTION:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		const FGenPlayerTemplate* T = Who != INDEX_NONE ? Logic->GetPlayers()[Who].Template : nullptr;
		return T && T->Side == Str(P, 1);
	}
	case EGenScriptCondition::C_PLAYER_ALL_DESTROYED:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		if (Who == INDEX_NONE)
		{
			return true;
		}
		for (const FGenObject* O : Logic->GetObjectsOf(Who))
		{
			if (IsLiving(O) && !O->IsProjectile() && O->IsTargetable())
			{
				return false;
			}
		}
		return true;
	}

	case EGenScriptCondition::C_PLAYER_HAS_POWER:
	case EGenScriptCondition::C_PLAYER_HAS_NO_POWER:
	case EGenScriptCondition::C_PLAYER_POWER_COMPARE_PERCENT:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		if (Who == INDEX_NONE)
		{
			return false;
		}
		int32 Produced = 0, Consumed = 0;
		Logic->GetEnergy(Who, Produced, Consumed);
		if (C.Type == EGenScriptCondition::C_PLAYER_HAS_POWER) { return Produced >= Consumed; }
		if (C.Type == EGenScriptCondition::C_PLAYER_HAS_NO_POWER) { return Produced < Consumed; }
		// Energy::getEnergySupplyRatio
		const float Ratio = Consumed == 0 ? float(Produced) : float(Produced) / float(Consumed);
		return Compare(Ratio, Int(P, 1), float(Int(P, 2)) / 100.f);
	}

	case EGenScriptCondition::C_PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTriggerNamed(Str(P, 4));
		return Who != INDEX_NONE && Area && Compare(CountObjects(Who, Str(P, 3), Area), Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_SKIRMISH_VALUE_IN_AREA:
	{
		// What the player's (living) things inside the area cost, in total.
		const int32 Who = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTriggerNamed(Str(P, 3));
		if (Who == INDEX_NONE || !Area)
		{
			return false;
		}
		int32 Total = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(Who))
		{
			if (IsLiving(O) && !O->IsProjectile() && Area->Contains(O->GetPosition().X, O->GetPosition().Y) && !O->IsKindOf(TEXT("INERT")))
			{
				Total += int32(O->GetTemplate()->BuildCost);
			}
		}
		return Compare(Total, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_SUPPLY_SOURCE_SAFE:
	{
		// Checked every two seconds.
		if (Frame <= C.CustomFrame && C.CustomData != 0)
		{
			return C.CustomData == 1;
		}
		C.CustomFrame = Frame + uint32(2 * GenLogic::GetLogicFPS());
		const bool bSafe = GetPlayerNamed(Str(P, 0)) == Player && IsSupplySourceSafe(Int(P, 1));
		C.CustomData = bSafe ? 1 : -1;
		return bSafe;
	}
	case EGenScriptCondition::C_SKIRMISH_UNOWNED_FACTION_UNIT_EXISTS:
		return Compare(0, Int(P, 1), Int(P, 2)); // abandoned vehicles to capture: there are none yet

	case EGenScriptCondition::C_TEAM_HAS_UNITS:
	{
		const FString& Name = Str(P, 0);
		if (const FGenTeam* Team = GetTeamNamed(Name))
		{
			if (Name == ThisTeamName || (Protos.IsValidIndex(Team->Proto) && Protos[Team->Proto].Name == Name && (Team == CallingTeam || Team == ConditionTeam)))
			{
				return TeamHasUnits(*Team);
			}
		}
		// Any instance of a team of that name.
		const int32 ProtoIndex = FindProto(Name);
		for (const TUniquePtr<FGenTeam>& T : Teams)
		{
			if (ProtoIndex != INDEX_NONE && T->Proto == ProtoIndex && TeamHasUnits(*T))
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_TEAM_DESTROYED:
	{
		const FGenTeam* Team = GetTeamNamed(Str(P, 0));
		if (!Team)
		{
			return false; // a team that doesn't exist isn't destroyed
		}
		bool bAny = false;
		ForEachMember(*Team, [&bAny](FGenObject&) { bAny = true; });
		return !bAny;
	}
	case EGenScriptCondition::C_TEAM_INSIDE_AREA_PARTIALLY:
	case EGenScriptCondition::C_TEAM_ENTERED_AREA_ENTIRELY:
	case EGenScriptCondition::C_TEAM_OUTSIDE_AREA_ENTIRELY:
	{
		const FGenTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTriggerNamed(Str(P, 1));
		if (!Team || !Area)
		{
			return false;
		}
		int32 Inside = 0, Total = 0;
		ForEachMember(*Team, [&Inside, &Total, Area](FGenObject& O)
		{
			++Total;
			Inside += Area->Contains(O.GetPosition().X, O.GetPosition().Y) ? 1 : 0;
		});
		if (C.Type == EGenScriptCondition::C_TEAM_INSIDE_AREA_PARTIALLY) { return Inside > 0; }
		if (C.Type == EGenScriptCondition::C_TEAM_OUTSIDE_AREA_ENTIRELY) { return Inside == 0; }
		return Total > 0 && Inside == Total;
	}

	// --- upgrades, sciences, special powers ---
	case EGenScriptCondition::C_SKIRMISH_SPECIAL_POWER_READY:
	{
		// (Looked at every ten seconds at most while the power isn't there or is recharging.)
		if (C.CustomFrame > Frame)
		{
			return false;
		}
		const int32 Who = GetPlayerNamed(Str(P, 0));
		if (Who == INDEX_NONE || !FGenUpgradeData::Get().FindPower(Str(P, 1)))
		{
			return false;
		}
		int32 Module = INDEX_NONE;
		const FGenObject* Source = Logic->FindPowerSource(Who, Str(P, 1), false, &Module);
		if (Source && !Source->IsDisabled() && Logic->IsPowerReady(*Source, Module))
		{
			return true;
		}
		uint32 ReadyFrame = 0;
		if (Source)
		{
			Logic->IsPowerReady(*Source, Module, &ReadyFrame);
		}
		const uint32 Later = Frame + uint32(10 * GenLogic::GetLogicFPS());
		C.CustomFrame = (ReadyFrame > Frame && ReadyFrame < Later) ? ReadyFrame : Later;
		return false;
	}
	case EGenScriptCondition::C_PLAYER_BUILT_UPGRADE:
	{
		// (An event: true once for each upgrade that gets finished.)
		const int32 Who = GetPlayerNamed(Str(P, 0));
		return Who != INDEX_NONE && Logic->ConsumeScriptEvent(Who, Str(P, 1), false);
	}
	case EGenScriptCondition::C_PLAYER_ACQUIRED_SCIENCE:
	{
		const int32 Who = GetPlayerNamed(Str(P, 0));
		return Who != INDEX_NONE && Logic->ConsumeScriptEvent(Who, Str(P, 1), true);
	}

	case EGenScriptCondition::C_SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE:
	{
		// ScriptConditions::evaluateSkirmishPlayerTechBuildingWithinDistancePerimeter: a tech building that
		// isn't ours or an ally's within that distance of the area. (Worked out once: "if we have a cached
		// value, return it".)
		if (C.CustomData != 0)
		{
			return C.CustomData == 1;
		}
		const int32 Who = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTriggerNamed(Str(P, 2));
		if (Who == INDEX_NONE || !Area || Area->Points.Num() == 0)
		{
			return false;
		}
		// PolygonTrigger::getCenterPoint / getRadius: the middle of its bounds, and half their diagonal.
		FBox2f Bounds(ForceInit);
		for (const FIntVector& Pt : Area->Points)
		{
			Bounds += FVector2f(float(Pt.X), float(Pt.Y));
		}
		const FVector2f Center = Bounds.GetCenter();
		const float Radius = Bounds.GetExtent().Size() + Real(P, 1);
		bool bFound = false;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (IsLiving(O) && O->IsKindOf(TEXT("TECH_BUILDING")) && O->GetOwner() != Who
				&& Logic->GetRelationship(Who, O->GetOwner()) != EGenRelationship::Allies
				&& FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Center) <= Radius * Radius)
			{
				bFound = true;
				break;
			}
		}
		C.CustomData = bFound ? 1 : -1;
		return bFound;
	}

	// Not there yet: named map objects, music.
	case EGenScriptCondition::C_NAMED_NOT_DESTROYED:
	case EGenScriptCondition::C_MUSIC_TRACK_HAS_COMPLETED:
	case EGenScriptCondition::C_PLAYER_LOST_OBJECT_TYPE:
	case EGenScriptCondition::C_UNIT_HEALTH:
	case EGenScriptCondition::C_PLAYER_TRIGGERED_SPECIAL_POWER:
	case EGenScriptCondition::C_PLAYER_COMPLETED_SPECIAL_POWER:
	case EGenScriptCondition::C_TEAM_SOME_HAVE_OBJECT_STATUS:
	case EGenScriptCondition::C_CONDITION_FALSE:
	default:
		return false;
	}
}

// ---- team orders --------------------------------------------------------------------------------

void FGenSkirmishAI::TeamGuardArea(FGenTeam& Team, const FGenPolygonTrigger& Area)
{
	// AIGroup::groupGuardArea: the post is the middle of the area; what is hostile inside it gets attacked.
	FVector2f Center;
	float Radius = 0.f;
	TriggerCenterAndRadius(Area, Center, Radius);
	ForEachMember(Team, [&Center, Radius, &Area](FGenObject& O)
	{
		if (O.CanMove() && O.GetBuildTarget() == GenInvalidObjectID && !O.GetTemplate()->bIsSupplyTruck)
		{
			O.CommandGuard(Center, Radius, &Area);
		}
	});
}

void FGenSkirmishAI::TeamFollowApproachPath(FGenTeam& Team, const FString& Label, bool bMoveToStartOnly)
{
	// ScriptActions::doTeamMoveToSkirmishApproachPath / doTeamFollowSkirmishApproachPath: the approach
	// paths of a map are labelled by where they lead: "Center1" goes to the base at start position 1.
	const FGenMapData* Map = Logic->GetMap();
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Enemy = GetEnemy();
	FVector2f Center;
	if (!Map || !Players.IsValidIndex(Enemy) || !GetTeamCenter(Team, Center))
	{
		return;
	}
	const FGenMapObject* Way = Map->GetClosestWaypointOnPath(Center, FString::Printf(TEXT("%s%d"), *Label, Players[Enemy].StartPosition + 1));
	if (!Way)
	{
		return;
	}
	TArray<FVector2f> Path;
	if (bMoveToStartOnly)
	{
		Path.Add(FVector2f(Way->Location.X, Way->Location.Y));
	}
	else
	{
		Map->GetWaypointPath(*Way, Path);
	}
	ForEachMember(Team, [&Path](FGenObject& O)
	{
		if (O.CanMove())
		{
			O.CommandFollowPath(Path);
		}
	});
}

// ---- actions ------------------------------------------------------------------------------------

void FGenSkirmishAI::ExecuteAction(const FGenScriptAction& A)
{
	const TArray<FGenScriptParam>& P = A.Params;
	const int32 FPS = GenLogic::GetLogicFPS();
	auto IdleTeam = [this](FGenTeam& Team)
	{
		ForEachMember(Team, [](FGenObject& O)
		{
			if (O.CanMove() && O.GetBuildTarget() == GenInvalidObjectID && !O.GetTemplate()->bIsSupplyTruck)
			{
				O.ClearAIMode();
				O.ClearAttack();
				O.CommandIdle();
			}
		});
	};
	auto MergeInto = [this](FGenTeam& Source, int32 DestId)
	{
		for (FGenObjectID Id : TArray<FGenObjectID>(Source.Members))
		{
			SetObjectTeam(Id, DestId);
		}
	};

	switch (A.Type)
	{
	// --- flags, counters, timers, script control (ScriptEngine) ---
	case EGenScriptAction::A_SET_FLAG:
		Flags.FindOrAdd(Str(P, 0)) = Int(P, 1) != 0;
		break;
	case EGenScriptAction::A_SET_COUNTER:
		Counters.FindOrAdd(Str(P, 0)).Value = Int(P, 1);
		break;
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
		// (Despite the name the value is in seconds.)
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

	// --- the player ---
	case EGenScriptAction::A_PLAYER_GIVE_MONEY:
	{
		// Not the original's: "<Side> Give AI money to cover bug" hands every computer player $2200 as the game
		// begins; at the user's word (2026-10-23) they start with the cash chosen in the menu, like everybody else.
		// (The later bonuses of a hard army stay.)
		if (RunningScript && RunningScript->Name.Contains(TEXT("Give AI money to cover bug")))
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d: '%s' gives no money (the menu's starting cash stands)"), Frame, Player,
				*RunningScript->Name);
			break;
		}
		const int32 Who = GetPlayerNamed(Str(P, 0));
		if (Who != INDEX_NONE) { Logic->AddMoney(Who, Int(P, 1)); }
		break;
	}
	case EGenScriptAction::A_PLAYER_ENABLE_UNIT_CONSTRUCTION:
		if (GetPlayerNamed(Str(P, 0)) == Player) { bCanBuildUnits = true; }
		break;
	case EGenScriptAction::A_PLAYER_ENABLE_BASE_CONSTRUCTION:
		if (GetPlayerNamed(Str(P, 0)) == Player) { bCanBuildBase = true; }
		break;
	case EGenScriptAction::A_PLAYER_SELL_EVERYTHING:
		if (GetPlayerNamed(Str(P, 0)) == Player)
		{
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (IsLiving(O) && O->GetOwner() == Player && O->IsKindOf(TEXT("STRUCTURE")) && !O->IsBeingSold())
				{
					FGenCommand Sell(EGenCommandType::Sell);
					Sell.Player = Player;
					Sell.Units = { Id };
					Logic->QueueSimCommand(MoveTemp(Sell));
				}
			}
		}
		break;

	// --- base building (AIPlayer) ---
	case EGenScriptAction::A_SKIRMISH_BUILD_BUILDING:
		BuildSpecificBuilding(Str(P, 0));
		break;
	case EGenScriptAction::A_SKIRMISH_BUILD_STRUCTURE_FRONT:
		BuildBaseDefenseStructure(Str(P, 0), false);
		break;
	case EGenScriptAction::A_SKIRMISH_BUILD_STRUCTURE_FLANK:
		BuildBaseDefenseStructure(Str(P, 0), true);
		break;
	case EGenScriptAction::A_SKIRMISH_BUILD_BASE_DEFENSE_FRONT:
	case EGenScriptAction::A_SKIRMISH_BUILD_BASE_DEFENSE_FLANK:
		if (const FGenAISideInfo* SideInfo = FGenAIData::Get().Sides.Find(Side))
		{
			BuildBaseDefenseStructure(SideInfo->BaseDefenseStructure, A.Type == EGenScriptAction::A_SKIRMISH_BUILD_BASE_DEFENSE_FLANK);
		}
		break;
	case EGenScriptAction::A_AI_PLAYER_BUILD_SUPPLY_CENTER:
		if (GetPlayerNamed(Str(P, 0)) == Player) { BuildBySupplies(Int(P, 2), Str(P, 1)); }
		break;
	case EGenScriptAction::A_AI_PLAYER_BUILD_TYPE_NEAREST_TEAM:
		if (const FGenTeam* Team = GetTeamNamed(Str(P, 2))) { BuildSpecificBuildingNearestTeam(Str(P, 1), *Team); }
		break;

	// --- teams ---
	case EGenScriptAction::A_BUILD_TEAM:
		BuildSpecificTeam(FindProto(Str(P, 0)), true);
		break;
	case EGenScriptAction::A_RECRUIT_TEAM:
		RecruitSpecificTeam(FindProto(Str(P, 0)), Real(P, 1));
		break;
	case EGenScriptAction::A_TEAM_GUARD_AREA:
	case EGenScriptAction::A_TEAM_ATTACK_AREA:
	{
		FGenTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTriggerNamed(Str(P, 1));
		if (Team && Area) { TeamGuardArea(*Team, *Area); }
		break;
	}
	case EGenScriptAction::A_TEAM_HUNT:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			ForEachMember(*Team, [](FGenObject& O)
			{
				if (O.CanMove() && O.HasWeapons()) { O.CommandHunt(); }
			});
		}
		break;
	case EGenScriptAction::A_TEAM_HUNT_WITH_COMMAND_BUTTON:
		// ScriptActions::doTeamHuntWithCommandButton: each member uses that button's ability on whatever is
		// nearest, again and again (a unit's own ability; with any other kind of button the team simply hunts).
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			const FString Button = Str(P, 1);
			const FGenCommandButton* Btn = FGenCommandData::Get().FindButton(Button);
			const FGenSpecialPowerTemplate* Power = Btn ? FGenUpgradeData::Get().FindPower(Btn->SpecialPower) : nullptr;
			const bool bAbility = Btn && Btn->Command == EGenCommand::SpecialPower && Power && Power->bPorted && Power->AbilityKind != EGenAbilityKind::Other;
			ForEachMember(*Team, [&](FGenObject& O)
			{
				if (bAbility && O.GetTemplate()->bButtonHunt && O.FindAbility(Power->Name) != INDEX_NONE)
				{
					O.SetHuntButton(Button);
				}
				else if (O.CanMove() && O.HasWeapons())
				{
					O.CommandHunt();
				}
			});
		}
		break;
	case EGenScriptAction::A_TEAM_STOP:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0))) { IdleTeam(*Team); }
		break;
	case EGenScriptAction::A_TEAM_STOP_AND_DISBAND:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			IdleTeam(*Team);
			if (Team->Id != 0) { MergeInto(*Team, 0); }
		}
		break;
	case EGenScriptAction::A_TEAM_MERGE_INTO_TEAM:
	{
		FGenTeam* Source = GetTeamNamed(Str(P, 0));
		FGenTeam* Dest = GetTeamNamed(Str(P, 1));
		if (Source && Dest && Source != Dest && Source->Id != 0) { MergeInto(*Source, Dest->Id); }
		break;
	}
	case EGenScriptAction::A_TEAM_AVAILABLE_FOR_RECRUITMENT:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0))) { Team->Recruitable = Int(P, 1) != 0 ? 1 : 0; }
		break;
	case EGenScriptAction::A_TEAM_DECREASE_PRIORITY:
	case EGenScriptAction::A_TEAM_INCREASE_PRIORITY:
		if (const FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (Protos.IsValidIndex(Team->Proto))
			{
				FGenTeamProto& Proto = Protos[Team->Proto];
				Proto.ProductionPriority += A.Type == EGenScriptAction::A_TEAM_INCREASE_PRIORITY ? Proto.SuccessIncrease : -Proto.FailureDecrease;
			}
		}
		break;
	case EGenScriptAction::A_TEAM_EXECUTE_SEQUENTIAL_SCRIPT:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			if (const FGenScript* Script = FindScript(Str(P, 1)))
			{
				IdleTeam(*Team); // so the sequence starts at once
				AppendSequentialScript(Team->Id, Script);
			}
		}
		break;
	case EGenScriptAction::A_TEAM_STOP_SEQUENTIAL_SCRIPT:
		if (const FGenTeam* Team = GetTeamNamed(Str(P, 0))) { RemoveSequentialScripts(Team->Id); }
		break;
	case EGenScriptAction::A_SKIRMISH_MOVE_TO_APPROACH_PATH:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0))) { TeamFollowApproachPath(*Team, Str(P, 1), true); }
		break;
	case EGenScriptAction::A_SKIRMISH_FOLLOW_APPROACH_PATH:
		if (FGenTeam* Team = GetTeamNamed(Str(P, 0))) { TeamFollowApproachPath(*Team, Str(P, 1), false); }
		break;
	case EGenScriptAction::A_TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE:
	{
		// To the nearest object of that type inside the area.
		FGenTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTriggerNamed(Str(P, 2));
		FVector2f Center;
		if (!Team || !Area || !GetTeamCenter(*Team, Center))
		{
			break;
		}
		const FGenObject* Best = nullptr;
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (!IsLiving(O) || !IsInTypeList(Str(P, 1), *O->GetTemplate()) || !Area->Contains(O->GetPosition().X, O->GetPosition().Y))
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
			const FVector3f Goal = Best->GetPosition();
			ForEachMember(*Team, [&Goal](FGenObject& O)
			{
				if (O.CanMove())
				{
					O.ClearAIMode();
					O.CommandMoveTo(Goal);
				}
			});
		}
		break;
	}

	// --- upgrades and special powers ---
	case EGenScriptAction::A_AI_PLAYER_BUILD_UPGRADE:
		if (GetPlayerNamed(Str(P, 0)) == Player) { BuildUpgrade(Str(P, 1)); }
		break;
	case EGenScriptAction::A_SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST:
		if (GetPlayerNamed(Str(P, 0)) == Player) { FireSpecialPowerAtMostCost(Str(P, 1)); }
		break;
	case EGenScriptAction::A_TEAM_USE_COMMANDBUTTON_ABILITY:
	{
		const FGenTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenCommandButton* Button = FGenCommandData::Get().FindButton(Str(P, 1));
		if (Team && Button)
		{
			ForEachMember(*Team, [this, Button](FGenObject& O) { DoCommandButton(O, *Button); });
		}
		break;
	}
	case EGenScriptAction::A_TEAM_PARTIAL_USE_COMMANDBUTTON:
	{
		// That share of the team's members that have the button.
		const FGenTeam* Team = GetTeamNamed(Str(P, 1));
		const FGenCommandButton* Button = FGenCommandData::Get().FindButton(Str(P, 2));
		if (Team && Button)
		{
			TArray<FGenObject*> Able;
			ForEachMember(*Team, [this, Button, &Able](FGenObject& O) { if (HasCommandButton(O, *Button)) { Able.Add(&O); } });
			const int32 Num = int32(Real(P, 0) / 100.f * float(Able.Num()));
			for (int32 i = 0; i < Num; ++i)
			{
				DoCommandButton(*Able[i], *Button);
			}
		}
		break;
	}

	// --- garrisons and transports ---
	case EGenScriptAction::A_TEAM_LOAD_TRANSPORTS:
		// ScriptActions::doLoadAllTransports: the team's passengers get into the team's transports.
		if (const FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Transports, Units;
			ForEachMember(*Team, [&Transports, &Units](FGenObject& O)
			{
				if (O.IsContained()) { return; }
				if (O.IsKindOf(TEXT("TRANSPORT")) && O.GetContain()) { Transports.Add(&O); }
				else { Units.Add(&O); }
			});
			// (First fit, in place of the original's partition solver.)
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
	case EGenScriptAction::A_TEAM_GARRISON_NEAREST_BUILDING:
		// ScriptActions::doTeamGarrisonNearestBuilding: the infantry fill the buildings nearest the team's first member.
		if (const FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			TArray<FGenObject*> Infantry;
			ForEachMember(*Team, [&Infantry](FGenObject& O)
			{
				if (!O.IsContained() && O.IsKindOf(TEXT("INFANTRY")) && !O.IsKindOf(TEXT("NO_GARRISON"))) { Infantry.Add(&O); }
			});
			if (Infantry.Num() == 0)
			{
				break;
			}
			const FVector3f From = Infantry[0]->GetPosition();
			TArray<FGenObject*> Buildings;
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				FGenObject* B = Logic->FindObject(Id);
				if (IsLiving(B) && B->IsKindOf(TEXT("STRUCTURE")) && B->GetTemplate()->Contain.Type == EGenContainType::Garrison
					&& Logic->CanEnterObject(*Infantry[0], *B, true))
				{
					Buildings.Add(B);
				}
			}
			Buildings.Sort([&From](const FGenObject& A, const FGenObject& B)
			{
				return (A.GetPosition() - From).SizeSquared() < (B.GetPosition() - From).SizeSquared();
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
		// AIGroup::groupEvacuate: the team's transports and buildings empty, and members that are inside something get out.
		if (const FGenTeam* Team = GetTeamNamed(Str(P, 0)))
		{
			ForEachMember(*Team, [this](FGenObject& O)
			{
				if (O.IsContained()) { O.CommandExit(); }
				for (FGenObjectID Id : TArray<FGenObjectID>(Logic->GetRiders(O)))
				{
					if (FGenObject* R = Logic->FindObject(Id)) { R->CommandExit(); }
				}
			});
		}
		break;

	// Not there yet: attack priorities, unit abilities, entering named buildings, capturing, guarding from
	// the tunnels, music. Their scripts run; these steps do nothing.
	default:
		break;
	}
}

// ---- sequential scripts -------------------------------------------------------------------------

void FGenSkirmishAI::AppendSequentialScript(int32 TeamId, const FGenScript* Script)
{
	for (FGenSequentialScript& Seq : SequentialScripts)
	{
		if (Seq.Team == TeamId)
		{
			Seq.Scripts.Add(Script); // runs after the ones already queued
			return;
		}
	}
	FGenSequentialScript& Seq = SequentialScripts.AddDefaulted_GetRef();
	Seq.Team = TeamId;
	Seq.Scripts.Add(Script);
}

void FGenSkirmishAI::RemoveSequentialScripts(int32 TeamId)
{
	SequentialScripts.RemoveAll([TeamId](const FGenSequentialScript& Seq) { return Seq.Team == TeamId; });
}

void FGenSkirmishAI::UpdateSequentialScripts()
{
	GEN_PERF_SCOPE("AI.SeqScripts");
	// ScriptEngine::evaluateAndProgressAllSequentialScripts: whenever its team stands idle, a sequential
	// script performs its next action; several in one frame if they finish at once.
	for (int32 i = 0; i < SequentialScripts.Num();)
	{
		const int32 TeamId = SequentialScripts[i].Team;
		bool bRemoved = false;
		for (int32 Spin = 0; Spin < 20; ++Spin)
		{
			FGenTeam* Team = FindTeam(TeamId);
			if (!Team || !SequentialScripts.IsValidIndex(i) || SequentialScripts[i].Team != TeamId || SequentialScripts[i].Scripts.Num() == 0)
			{
				RemoveSequentialScripts(TeamId);
				bRemoved = true;
				break;
			}
			FGenSequentialScript& Seq = SequentialScripts[i];
			const bool bIdle = TeamIsIdle(*Team);
			if (!((bIdle && Seq.FramesToWait < 1) || Seq.FramesToWait == 0))
			{
				if (Seq.FramesToWait > 0)
				{
					--Seq.FramesToWait;
				}
				break;
			}
			++Seq.CurrentInstruction;
			const FGenScript* Script = Seq.Scripts[0];
			if (Script->Actions.IsValidIndex(Seq.CurrentInstruction))
			{
				Seq.FramesToWait = -1;
				const FGenScriptAction Action = Script->Actions[Seq.CurrentInstruction];
				// <This Team> is the team the sequence runs on.
				TGuardValue<FGenTeam*> GuardCalling(CallingTeam, nullptr);
				TGuardValue<FGenTeam*> GuardCondition(ConditionTeam, Team);
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d team '%s' (%d units): %s -> %s"), Frame, Player,
					Protos.IsValidIndex(Team->Proto) ? *Protos[Team->Proto].Name : TEXT("default"), Team->Members.Num(), *Script->Name, *Action.TypeName);
				ExecuteAction(Action);
			}
			else
			{
				// This script is done: on to the next one queued for the team, if any.
				Seq.Scripts.RemoveAt(0);
				Seq.CurrentInstruction = -1;
				Seq.FramesToWait = -1;
				if (Seq.Scripts.Num() == 0)
				{
					SequentialScripts.RemoveAt(i);
					bRemoved = true;
					break;
				}
			}
		}
		if (!bRemoved)
		{
			++i;
		}
	}
}
