// The script engine of a map: ScriptEngine.cpp (evaluation, flags / counters / timers, subroutines, fades, the
// named objects and teams of the map) and ScriptConditions.cpp. The actions are in GenMapScriptActions.cpp.

#include "Logic/GenMapScripts.h"
#include "Audio/GenAudioData.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Logic/GenSkirmishAI.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	const TCHAR* const ThisTeamName = TEXT("<This Team>");
	const TCHAR* const ThisPlayerName = TEXT("<This Player>");
	const TCHAR* const LocalPlayerName = TEXT("<Local Player>");

	/** ScriptEngine.cpp: FRAMES_TO_FADE_IN_AT_START */
	constexpr int32 FramesToFadeInAtStart = 33;
	/** Drawable.h: DRAWABLE_FRAMES_PER_FLASH (LOGICFRAMES_PER_SECOND / 2) */
	constexpr int32 FramesPerFlash = 15;

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

	const FString& Str(const TArray<FGenScriptParam>& P, int32 Index)
	{
		static const FString Empty;
		return P.IsValidIndex(Index) ? P[Index].String : Empty;
	}

	int32 Int(const TArray<FGenScriptParam>& P, int32 Index)
	{
		return P.IsValidIndex(Index) ? P[Index].Int : 0;
	}

	bool IsLiving(const FGenObject* O)
	{
		return O && !O->IsDead() && !O->IsDestroyed();
	}

	bool IsTeamCondition(EGenScriptCondition Type)
	{
		return Type == EGenScriptCondition::C_TEAM_HAS_UNITS || Type == EGenScriptCondition::C_TEAM_INSIDE_AREA_PARTIALLY
			|| Type == EGenScriptCondition::C_TEAM_DESTROYED || Type == EGenScriptCondition::C_TEAM_ENTERED_AREA_ENTIRELY
			|| Type == EGenScriptCondition::C_TEAM_OUTSIDE_AREA_ENTIRELY || Type == EGenScriptCondition::C_TEAM_SOME_HAVE_OBJECT_STATUS
			|| Type == EGenScriptCondition::C_TEAM_DISCOVERED || Type == EGenScriptCondition::C_TEAM_ATTACKED_BY_PLAYER
			|| Type == EGenScriptCondition::C_TEAM_ATTACKED_BY_OBJECTTYPE;
	}
}

// ---- client events ------------------------------------------------------------------------------

const FString& FGenScriptClientEvent::Str(int32 Index) const { return ::Str(Params, Index); }
int32 FGenScriptClientEvent::Int(int32 Index) const { return ::Int(Params, Index); }
float FGenScriptClientEvent::Real(int32 Index) const { return Params.IsValidIndex(Index) ? Params[Index].Real : 0.f; }

// ---- setup --------------------------------------------------------------------------------------

FGenMapScripts::FGenMapScripts(UGenGameLogic* InLogic)
	: Logic(InLogic)
{
}

void FGenMapScripts::NewMap(const FGenMapData& InMap, EGenDifficulty InDifficulty)
{
	Map = &InMap;
	Difficulty = InDifficulty;
	Lists = InMap.ScriptLists;
	int32 NumScripts = 0;
	auto Index = [this, &NumScripts](FGenScript& S)
	{
		ScriptsByName.Add(S.Name, &S);
		++NumScripts;
	};
	for (FGenScriptList& L : Lists)
	{
		for (FGenScript& S : L.Scripts) { Index(S); }
		for (FGenScriptGroup& G : L.Groups)
		{
			GroupsByName.Add(G.Name, &G);
			for (FGenScript& S : G.Scripts) { Index(S); }
		}
	}

	// TeamFactory::initFromSides: every team of the map; the singletons have their one team from the start.
	for (const FGenDict& D : InMap.Teams)
	{
		FGenTeamProto& Proto = Protos.AddDefaulted_GetRef();
		GenScriptIO::ReadTeamProto(D, Proto);
		const int32 Owner = Proto.Owner.IsEmpty() ? 0 : GetPlayerNamed(Proto.Owner);
		ProtoPlayers.Add(Owner == INDEX_NONE ? 0 : Owner);
	}
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	for (int32 i = 0; i < Protos.Num(); ++i)
	{
		const FGenTeamProto& Proto = Protos[i];
		if (!Proto.bSingleton)
		{
			continue;
		}
		const int32 Owner = ProtoPlayers[i];
		FGenMapTeam& Team = CreateTeam(i, Owner);
		// "team<PlayerName>" is the player's default team (the neutral player's is "team").
		const FString DefaultName = Players.IsValidIndex(Owner) ? TEXT("team") + Players[Owner].ScriptName : FString();
		if (Proto.Name.Equals(DefaultName, ESearchCase::IgnoreCase) && !DefaultTeams.Contains(Owner))
		{
			Team.bPlayerDefault = true;
			DefaultTeams.Add(Owner, Team.Id);
		}
	}
	for (const FGenPlayer& P : Players)
	{
		if (!DefaultTeams.Contains(P.Index))
		{
			// (A player the map gave no default team: one is made, so that whatever it gets has a team.)
			FGenMapTeam& Team = CreateTeam(INDEX_NONE, P.Index);
			Team.bPlayerDefault = true;
			DefaultTeams.Add(P.Index, Team.Id);
		}
	}

	// ScriptEngine::checkConditionsForTeamNames: a script whose conditions name a team of several instances is
	// evaluated once for each of them.
	for (TPair<FString, FGenScript*>& It : ScriptsByName)
	{
		FGenScript& S = *It.Value;
		for (const TArray<FGenScriptCondition>& And : S.Conditions)
		{
			for (const FGenScriptCondition& C : And)
			{
				const int32 P = IsTeamCondition(C.Type) ? FindProto(Str(C.Params, 0)) : INDEX_NONE;
				if (P != INDEX_NONE && !Protos[P].bSingleton && S.ConditionTeam.IsEmpty())
				{
					S.ConditionTeam = Protos[P].Name;
				}
			}
		}
	}

	// ScriptEngine::newMap: "default to a fade in from black".
	Fade.Type = FGenScriptFade::Multiply;
	FadeFrame = 0;
	FadeMin = 1.f;
	FadeMax = 0.f;
	FadeIncrease = 0;
	FadeHold = 0;
	FadeDecrease = FramesToFadeInAtStart;
	Fade.Value = 0.f;
	UE_LOG(LogGenerals, Log, TEXT("Map scripts: %d scripts in %d lists, %d team definitions, %d teams"), NumScripts, Lists.Num(), Protos.Num(), Teams.Num());
}

int32 FGenMapScripts::GetMapObjectOwner(const FGenMapObject& MapObj) const
{
	const int32 P = FindProto(MapObj.GetString(TEXT("originalOwner")));
	return P != INDEX_NONE ? ProtoPlayers[P] : 0;
}

void FGenMapScripts::OnMapObjectCreated(FGenObject& Obj, const FGenMapObject& MapObj)
{
	// The team named by originalOwner (TeamFactory::findTeam: its instance, made by the first object that asks).
	const FString TeamName = MapObj.GetString(TEXT("originalOwner"));
	if (FGenMapTeam* Team = GetOrCreateTeamNamed(TeamName))
	{
		SetObjectTeam(Obj, *Team);
	}
	// Object::updateObjValuesFromMapProperties
	const FString Name = MapObj.GetString(TEXT("objectName"));
	if (!Name.IsEmpty())
	{
		NameObject(Obj, Name);
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectMaxHPs")); V && V->Int >= 0)
	{
		Obj.MaxHealth = Obj.Health = float(V->Int);
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectInitialHealth")))
	{
		// ActiveBody::setInitialHealth: a percentage of the maximum.
		Obj.Health = FMath::Max(1.f, Obj.MaxHealth * float(V->Int) / 100.f);
		Obj.PreviousHealth = Obj.Health;
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectIndestructible")))
	{
		Obj.bIndestructible = V->Bool;
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectVeterancy")); V && V->Int > 0)
	{
		Obj.SetVeterancyLevel(V->Int, false);
	}
}

// ---- evaluation ---------------------------------------------------------------------------------

void FGenMapScripts::Update(uint32 InFrame)
{
	GEN_PERF_SCOPE("MapScripts");
	Frame = InFrame;
	if (Fade.Type != FGenScriptFade::None)
	{
		UpdateFade();
	}
	if (bEnded)
	{
		return; // ScriptEngine::update: "we are just timing down" once the game is won or lost
	}
	// Countdown timers go down to -1 and stop.
	for (TPair<FString, FCounter>& It : Counters)
	{
		if (It.Value.bCountdown && It.Value.Value >= 0)
		{
			--It.Value.Value;
		}
	}
	// Every side's scripts, each with its own player as <This Player>.
	for (int32 Side = 0; Side < Lists.Num(); ++Side)
	{
		CurrentPlayer = Side;
		ExecuteScripts(Side, Lists[Side].Scripts);
		for (FGenScriptGroup& G : Lists[Side].Groups)
		{
			if (G.bActive && !G.bSubroutine)
			{
				ExecuteScripts(Side, G.Scripts);
			}
		}
		CurrentPlayer = INDEX_NONE;
	}
	for (TUniquePtr<FGenMapTeam>& T : Teams)
	{
		T->bEnteredOrExited = false;
	}
	UpdateSequentialScripts();

	// Drawable::update's flashing: every half second, as many times as the script asked for.
	if (Frame % FramesPerFlash == 0)
	{
		for (auto It = Flashing.CreateIterator(); It; ++It)
		{
			if (FGenObject* O = Logic->FindObject(It.Key()); IsLiving(O) && It.Value() > 0)
			{
				O->FlashAs(FLinearColor::White);
				if (--It.Value() <= 0)
				{
					It.RemoveCurrent();
				}
			}
			else
			{
				It.RemoveCurrent();
			}
		}
	}
}

void FGenMapScripts::ExecuteScripts(int32 Side, TArray<FGenScript>& Scripts)
{
	for (FGenScript& S : Scripts)
	{
		if (!S.bSubroutine)
		{
			Logic->SetStretchDetail(S.Name);
			ExecuteScript(S);
			Logic->SimYield(TEXT("a map script"));
		}
	}
}

bool FGenMapScripts::IsForDifficulty(const FGenScript& S) const
{
	return (Difficulty == EGenDifficulty::Easy && S.bEasy) || (Difficulty == EGenDifficulty::Normal && S.bNormal)
		|| (Difficulty == EGenDifficulty::Hard && S.bHard);
}

void FGenMapScripts::ExecuteScript(FGenScript& Script)
{
	// ScriptEngine::executeScript
	if (!Script.bActive || !IsForDifficulty(Script))
	{
		return;
	}
	if (Frame < Script.FrameToEvaluate)
	{
		return;
	}
	if (Script.DelaySeconds > 0)
	{
		Script.FrameToEvaluate = Frame + uint32(Script.DelaySeconds * GenLogic::GetLogicFPS());
	}
	TGuardValue<const FGenScript*> Running(RunningScript, &Script);
	TArray<int32, TInlineAllocator<8>> Instances;
	if (!Script.ConditionTeam.IsEmpty())
	{
		const int32 P = FindProto(Script.ConditionTeam);
		for (const TUniquePtr<FGenMapTeam>& T : Teams)
		{
			if (T->Proto == P && P != INDEX_NONE)
			{
				Instances.Add(T->Id);
			}
		}
	}
	auto RunOnce = [this, &Script]()
	{
		if (EvaluateConditions(Script))
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] '%s' (p%d)"), Frame, *Script.Name, CurrentPlayer);
			if (Script.bOneShot)
			{
				Script.bActive = false; // (before its actions: a one-shot that enables itself again stays enabled)
			}
			ExecuteActions(Script.Actions);
		}
		else if (Script.FalseActions.Num() > 0)
		{
			if (Script.bOneShot)
			{
				Script.bActive = false;
			}
			ExecuteActions(Script.FalseActions);
		}
	};
	if (Instances.Num() > 1)
	{
		for (int32 TeamId : Instances)
		{
			TGuardValue<FGenMapTeam*> Guard(CallingTeam, FindTeam(TeamId));
			if (CallingTeam && Script.bActive)
			{
				RunOnce();
			}
		}
	}
	else
	{
		RunOnce();
	}
}

bool FGenMapScripts::EvaluateConditions(const FGenScript& Script)
{
	// OR of ANDs; an empty script is always true.
	if (Script.Conditions.Num() == 0)
	{
		return true;
	}
	for (const TArray<FGenScriptCondition>& And : Script.Conditions)
	{
		bool bAll = true;
		for (const FGenScriptCondition& C : And)
		{
			if (!EvaluateCondition(C))
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

void FGenMapScripts::ExecuteActions(const TArray<FGenScriptAction>& Actions)
{
	for (const FGenScriptAction& A : Actions)
	{
		ExecuteAction(A);
	}
}

FGenScript* FGenMapScripts::FindScript(const FString& Name)
{
	FGenScript** Found = ScriptsByName.Find(Name);
	return Found ? *Found : nullptr;
}

FGenScriptGroup* FGenMapScripts::FindGroup(const FString& Name)
{
	FGenScriptGroup** Found = GroupsByName.Find(Name);
	return Found ? *Found : nullptr;
}

void FGenMapScripts::CallSubroutine(const FString& Name)
{
	// ScriptEngine::callSubroutine: a script, or every script of a group, run now (if active).
	if (SubroutineDepth > 16)
	{
		return;
	}
	++SubroutineDepth;
	if (FGenScript* S = FindScript(Name))
	{
		if (S->bActive)
		{
			ExecuteScript(*S);
		}
	}
	else if (FGenScriptGroup* G = FindGroup(Name))
	{
		if (G->bActive)
		{
			for (FGenScript& S : G->Scripts)
			{
				if (S.bActive)
				{
					ExecuteScript(S);
				}
			}
		}
	}
	--SubroutineDepth;
}

void FGenMapScripts::SetFade(const FGenScriptAction& A)
{
	// ScriptEngine::setFade
	const TArray<FGenScriptParam>& P = A.Params;
	Fade.Type = A.Type == EGenScriptAction::A_CAMERA_FADE_ADD ? FGenScriptFade::Add : FGenScriptFade::Multiply;
	FadeFrame = 0;
	FadeMin = P.IsValidIndex(0) ? P[0].Real : 0.f;
	FadeMax = P.IsValidIndex(1) ? P[1].Real : 0.f;
	FadeIncrease = Int(P, 2);
	FadeHold = Int(P, 3);
	FadeDecrease = Int(P, 4);
	Fade.Value = FadeMin;
	if (FadeIncrease == 0)
	{
		UpdateFade();
	}
}

void FGenMapScripts::UpdateFade()
{
	// ScriptEngine::updateFades: up from the minimum, held at the maximum, back down.
	++FadeFrame;
	int32 F = FadeFrame;
	if (F <= FadeIncrease)
	{
		Fade.Value = FadeMin + (float(FadeFrame) / float(FadeIncrease)) * (FadeMax - FadeMin);
		return;
	}
	F -= FadeIncrease;
	if (F <= FadeHold)
	{
		Fade.Value = FadeMax;
		return;
	}
	F -= FadeHold;
	if (F <= FadeDecrease)
	{
		const int32 Divisor = FMath::Max(1, FadeDecrease + 1);
		Fade.Value = FadeMax + (float(F) / float(Divisor)) * (FadeMin - FadeMax);
		return;
	}
	Fade.Type = FGenScriptFade::None;
}

void FGenMapScripts::UpdateSequentialScripts()
{
	// ScriptEngine::evaluateAndProgressAllSequentialScripts, as far as the maps ported so far need it: each action
	// once its team is idle (or at once when it has nothing to wait for).
	for (int32 i = 0; i < SequentialScripts.Num();)
	{
		FSequential& Seq = SequentialScripts[i];
		FGenMapTeam* Team = FindTeam(Seq.Team);
		if (!Team || !Seq.Script || !Seq.Script->Actions.IsValidIndex(Seq.Instruction))
		{
			SequentialScripts.RemoveAt(i);
			continue;
		}
		TGuardValue<FGenMapTeam*> Guard(CallingTeam, Team);
		ExecuteAction(Seq.Script->Actions[Seq.Instruction++]);
		++i;
	}
}

void FGenMapScripts::Client(const FGenScriptAction& A, const FString* Waypoint, const FString* Waypoint2)
{
	FGenScriptClientEvent& E = ClientEvents.AddDefaulted_GetRef();
	E.Action = A.Type;
	E.Params = A.Params;
	E.Frame = Frame;
	if (Waypoint)
	{
		E.bHasPos = GetWaypoint(*Waypoint, E.Pos);
	}
	if (Waypoint2)
	{
		E.bHasPos2 = GetWaypoint(*Waypoint2, E.Pos2);
	}
}

FString FGenMapScripts::Describe() const
{
	int32 Active = 0;
	for (const TPair<FString, FGenScript*>& It : ScriptsByName)
	{
		Active += It.Value->bActive ? 1 : 0;
	}
	return FString::Printf(TEXT("%d scripts active, %d flags, %d counters, %d teams%s%s"), Active, Flags.Num(), Counters.Num(), Teams.Num(),
		bTimeFrozen ? TEXT(", time frozen") : TEXT(""), bInputDisabled ? TEXT(", input off") : TEXT(""));
}

// ---- names --------------------------------------------------------------------------------------

int32 FGenMapScripts::GetPlayerNamed(const FString& Name) const
{
	// ScriptEngine::getPlayerFromAsciiString
	if (Name == LocalPlayerName)
	{
		return Logic->GetLocalPlayer();
	}
	if (Name == ThisPlayerName)
	{
		return CurrentPlayer;
	}
	for (const FGenPlayer& P : Logic->GetPlayers())
	{
		if (!P.ScriptName.IsEmpty() && P.ScriptName.Equals(Name, ESearchCase::CaseSensitive))
		{
			return P.Index;
		}
	}
	return INDEX_NONE;
}

int32 FGenMapScripts::FindProto(const FString& Name) const
{
	return Protos.IndexOfByPredicate([&Name](const FGenTeamProto& P) { return P.Name.Equals(Name, ESearchCase::CaseSensitive); });
}

FGenMapTeam* FGenMapScripts::FindTeam(int32 Id)
{
	for (TUniquePtr<FGenMapTeam>& T : Teams)
	{
		if (T->Id == Id)
		{
			return T.Get();
		}
	}
	return nullptr;
}

FGenMapTeam& FGenMapScripts::CreateTeam(int32 ProtoIndex, int32 Player)
{
	TUniquePtr<FGenMapTeam>& T = Teams.Add_GetRef(MakeUnique<FGenMapTeam>());
	T->Id = NextTeamId++;
	T->Proto = ProtoIndex;
	T->Player = Player;
	return *T;
}

FGenMapTeam* FGenMapScripts::GetTeamNamed(const FString& Name)
{
	// ScriptEngine::getTeamNamed: <This Team>, the team the script runs for, or the definition's (latest) instance.
	if (Name == ThisTeamName)
	{
		return CallingTeam;
	}
	if (CallingTeam && Protos.IsValidIndex(CallingTeam->Proto) && Protos[CallingTeam->Proto].Name.Equals(Name, ESearchCase::CaseSensitive))
	{
		return CallingTeam;
	}
	const int32 P = FindProto(Name);
	if (P == INDEX_NONE)
	{
		return nullptr;
	}
	for (int32 i = Teams.Num() - 1; i >= 0; --i)
	{
		if (Teams[i]->Proto == P)
		{
			return Teams[i]->bActive ? Teams[i].Get() : nullptr;
		}
	}
	return nullptr;
}

FGenMapTeam* FGenMapScripts::GetOrCreateTeamNamed(const FString& Name)
{
	// TeamFactory::findTeam: the definition's instance, made if there is none yet.
	if (FGenMapTeam* T = GetTeamNamed(Name))
	{
		return T;
	}
	const int32 P = FindProto(Name);
	return P != INDEX_NONE ? &CreateTeam(P, ProtoPlayers[P]) : nullptr;
}

int32 FGenMapScripts::GetObjectTeam(const FGenObject& Obj) const
{
	if (const int32* Id = ObjectTeams.Find(Obj.GetID()))
	{
		for (const TUniquePtr<FGenMapTeam>& T : Teams)
		{
			if (T->Id == *Id)
			{
				// (Object::defect: a captured object is on its new owner's default team.)
				if (T->Player == Obj.GetOwner())
				{
					return *Id;
				}
				break;
			}
		}
	}
	const int32* Default = DefaultTeams.Find(Obj.GetOwner());
	return Default ? *Default : 0;
}

void FGenMapScripts::SetObjectTeam(FGenObject& Obj, FGenMapTeam& Team)
{
	// Object::setTeam: on another player's team, the object is that player's.
	if (Obj.GetOwner() != Team.Player)
	{
		Logic->ChangeOwner(Obj, Team.Player);
	}
	ObjectTeams.Remove(Obj.GetID());
	for (TUniquePtr<FGenMapTeam>& T : Teams)
	{
		T->Members.Remove(Obj.GetID());
	}
	if (!Team.bPlayerDefault)
	{
		ObjectTeams.Add(Obj.GetID(), Team.Id);
		Team.Members.AddUnique(Obj.GetID());
	}
	Team.bActive = true;
}

void FGenMapScripts::GetTeamMembers(const FGenMapTeam& Team, TArray<FGenObject*>& Out, bool bIncludeDead) const
{
	Out.Reset();
	auto Wanted = [bIncludeDead](const FGenObject* O) { return O && !O->IsDestroyed() && !O->IsProjectile() && (bIncludeDead || !O->IsDead()); };
	if (Team.bPlayerDefault)
	{
		for (FGenObject* O : Logic->GetObjectsOf(Team.Player))
		{
			if (Wanted(O) && GetObjectTeam(*O) == Team.Id)
			{
				Out.Add(O);
			}
		}
		return;
	}
	for (FGenObjectID Id : Team.Members)
	{
		FGenObject* O = Logic->FindObject(Id);
		if (Wanted(O) && O->GetOwner() == Team.Player)
		{
			Out.Add(O);
		}
	}
}

bool FGenMapScripts::GetTeamCenter(const FGenMapTeam& Team, FVector2f& Out) const
{
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members, true);
	if (Members.Num() == 0)
	{
		return false;
	}
	FVector2f Sum = FVector2f::ZeroVector;
	for (const FGenObject* O : Members)
	{
		Sum += FVector2f(O->GetPosition().X, O->GetPosition().Y);
	}
	Out = Sum / float(Members.Num());
	return true;
}

void FGenMapScripts::NameObject(FGenObject& Obj, const FString& Name)
{
	Obj.ScriptName = Name;
	Named.Add(Name.ToLower(), Obj.GetID());
	NamesThatExisted.Add(Name.ToLower());
}

void FGenMapScripts::TransferName(const FGenObject& From, FGenObject& To)
{
	// ScriptEngine::transferObjectName: what a script called the terrorist, it now calls the car bomb.
	if (From.ScriptName.IsEmpty())
	{
		return;
	}
	NameObject(To, From.ScriptName);
}

FGenObject* FGenMapScripts::GetUnitNamed(const FString& Name) const
{
	const FGenObjectID* Id = Named.Find(Name.ToLower());
	FGenObject* O = Id ? Logic->FindObject(*Id) : nullptr;
	return (O && !O->IsDestroyed()) ? O : nullptr;
}

const FGenPolygonTrigger* FGenMapScripts::GetTrigger(const FString& Name) const
{
	return Map ? Map->FindTrigger(Name) : nullptr;
}

bool FGenMapScripts::GetWaypoint(const FString& Name, FVector3f& Out) const
{
	const FGenMapObject* W = Map ? Map->FindWaypoint(Name) : nullptr;
	if (!W)
	{
		return false;
	}
	Out = FVector3f(W->Location.X, W->Location.Y, Map->GetGroundHeight(W->Location.X, W->Location.Y));
	return true;
}

const FGenMapObject* FGenMapScripts::GetClosestWaypointOnPath(const FVector2f& From, const FString& Label) const
{
	return Map ? Map->GetClosestWaypointOnPath(From, Label) : nullptr;
}

void FGenMapScripts::GetWaypointPath(const FGenMapObject& Start, TArray<FVector2f>& Out) const
{
	Out.Reset();
	if (Map)
	{
		Map->GetWaypointPath(Start, Out);
	}
}

const TArray<const FGenThingTemplate*>& FGenMapScripts::ResolveTypes(const FString& TypeOrList) const
{
	if (const TArray<const FGenThingTemplate*>* Found = ResolvedTypes.Find(TypeOrList))
	{
		return *Found;
	}
	TArray<const FGenThingTemplate*>& Out = ResolvedTypes.Add(TypeOrList);
	const FGenGameData& GD = FGenGameData::Get();
	if (const TArray<FString>* List = ObjectTypeLists.Find(TypeOrList))
	{
		for (const FString& T : *List)
		{
			if (const FGenThingTemplate* Tm = GD.FindTemplate(T)) { Out.AddUnique(Tm->Canonical()); }
		}
	}
	else if (const FGenThingTemplate* Tm = GD.FindTemplate(TypeOrList))
	{
		Out.Add(Tm->Canonical());
	}
	return Out;
}

bool FGenMapScripts::GetTeamRelationOverride(const FGenObject& From, const FGenObject& To, EGenRelationship& OutRelation) const
{
	const int32 FromTeam = GetObjectTeam(From);
	for (const TUniquePtr<FGenMapTeam>& T : Teams)
	{
		if (T->Id == FromTeam)
		{
			if (const EGenRelationship* R = T->OverrideRelations.Find(GetObjectTeam(To)))
			{
				OutRelation = *R;
				return true;
			}
			return false;
		}
	}
	return false;
}

bool FGenMapScripts::IsButtonRemoved(const FString& CommandSet, int32 Slot) const
{
	const TArray<int32>* Slots = RemovedButtons.Find(CommandSet.ToLower());
	return Slots && Slots->Contains(Slot);
}

bool FGenMapScripts::IsScienceHidden(int32 Player, const FString& Science) const
{
	const TSet<FString>* Set = HiddenSciences.Find(Player);
	return Set && Set->Contains(Science.ToLower());
}

// ---- conditions ---------------------------------------------------------------------------------

int32 FGenMapScripts::CountInArea(int32 Player, const FGenPolygonTrigger& Area, TFunctionRef<bool(const FGenObject&)> Wanted) const
{
	// evaluatePlayerHasUnitTypeInArea / ...KindInArea: the player's living things inside the area (INERT things aside).
	int32 Count = 0;
	for (const FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (IsLiving(O) && !O->IsProjectile() && !O->IsKindOf(TEXT("INERT")) && Wanted(*O) && Area.Contains(O->GetPosition().X, O->GetPosition().Y))
		{
			++Count;
		}
	}
	return Count;
}

bool FGenMapScripts::PlayerUnitComparison(const FGenScriptCondition& C, int32 Player, int32 Comparison, int32 Count, const FString& Types) const
{
	// evaluatePlayerUnitCondition: Player::countObjectsByThingTemplate (living things of those types).
	const TArray<const FGenThingTemplate*>& Wanted = ResolveTypes(Types);
	int32 N = 0;
	for (const FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (IsLiving(O) && !O->IsUnderConstruction() && Wanted.Contains(O->GetTemplate()->Canonical()))
		{
			++N;
		}
	}
	return Compare(N, Comparison, Count);
}

bool FGenMapScripts::NamedEnteredArea(const FGenScriptCondition& C, const FString& Unit, const FString& AreaName) const
{
	// Object::didEnter: inside now, and not when this was last asked. (Kept per condition: the scripts that ask
	// are active from the start, so it comes to the same.)
	const FGenObject* O = GetUnitNamed(Unit);
	const FGenPolygonTrigger* Area = GetTrigger(AreaName);
	if (!O || !Area || O->IsKindOf(TEXT("INERT")))
	{
		return false;
	}
	const bool bInside = !O->IsDead() && Area->Contains(O->GetPosition().X, O->GetPosition().Y);
	const bool bWasInside = C.CustomData == 1;
	C.CustomData = bInside ? 1 : -1;
	return bInside && !bWasInside;
}

bool FGenMapScripts::AttackedByPlayer(const FGenObject& Victim, int32 Player) const
{
	// evaluateNamedAttackedByPlayer: the last damage it took came from that player.
	if (Victim.LastDamageFrame == 0 || Player == INDEX_NONE)
	{
		return false;
	}
	if (Victim.LastDamageSourcePlayer == Player)
	{
		return true;
	}
	const FGenObject* Attacker = Logic->FindObject(Victim.LastDamageSource);
	return Attacker && Attacker->GetOwner() == Player;
}

bool FGenMapScripts::IsSoundComplete(const FString& Event, TMap<FString, uint32>& Pending) const
{
	// ScriptEngine::isSpeechComplete: the length of the sound, from when it is first asked about.
	uint32* Done = Pending.Find(Event);
	if (!Done)
	{
		const float Seconds = FGenAudioData::Get().GetEventLengthSeconds(Event);
		Done = &Pending.Add(Event, Frame + uint32(FMath::Max(0.f, Seconds) * float(GenLogic::GetLogicFPS())));
	}
	if (Frame >= *Done)
	{
		Pending.Remove(Event);
		return true;
	}
	return false;
}

bool FGenMapScripts::TeamInside(const FGenMapTeam& Team, const FGenPolygonTrigger& Area, uint32 Surfaces, bool bAll) const
{
	// Team::allInside / someInsideSomeOutside: the members on those surfaces (things without an AI count as ground units).
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members);
	bool bAny = false, bAnyInside = false, bAnyOutside = false;
	for (const FGenObject* O : Members)
	{
		const uint32 Theirs = O->CanMove() ? O->GetPathSurfaces() : uint32(GenSurface::Ground);
		if (Surfaces != 0 && (Theirs & Surfaces) == 0)
		{
			continue;
		}
		if (O->IsKindOf(TEXT("INERT")))
		{
			continue;
		}
		bAny = true;
		if (Area.Contains(O->GetPosition().X, O->GetPosition().Y)) { bAnyInside = true; }
		else { bAnyOutside = true; }
	}
	return bAll ? (bAny && !bAnyOutside) : (bAny && bAnyInside);
}

bool FGenMapScripts::HasObjectStatus(const FGenObject& Obj, const FString& Status) const
{
	// The object statuses the ported maps ask about.
	if (Status.Equals(TEXT("IS_CARBOMB"), ESearchCase::IgnoreCase)) { return Obj.IsCarBomb(); }
	if (Status.Equals(TEXT("STEALTHED"), ESearchCase::IgnoreCase)) { return Obj.IsStealthed(); }
	if (Status.Equals(TEXT("DETECTED"), ESearchCase::IgnoreCase)) { return Obj.IsDetected(); }
	if (Status.Equals(TEXT("UNDER_CONSTRUCTION"), ESearchCase::IgnoreCase)) { return Obj.IsUnderConstruction(); }
	if (Status.Equals(TEXT("SOLD"), ESearchCase::IgnoreCase)) { return Obj.IsBeingSold(); }
	if (Status.Equals(TEXT("HIJACKED"), ESearchCase::IgnoreCase)) { return Obj.IsHijacked(); }
	if (Status.Equals(TEXT("DESTROYED"), ESearchCase::IgnoreCase)) { return Obj.IsDead(); }
	return false;
}

bool FGenMapScripts::EvaluateCondition(const FGenScriptCondition& C)
{
	const TArray<FGenScriptParam>& P = C.Params;
	switch (C.Type)
	{
	case EGenScriptCondition::C_CONDITION_TRUE:
		return true;
	case EGenScriptCondition::C_CONDITION_FALSE:
		return false;
	case EGenScriptCondition::C_COUNTER:
	{
		const FCounter* Counter = Counters.Find(Str(P, 0));
		return Compare(Counter ? Counter->Value : 0, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_FLAG:
	{
		const bool* Flag = Flags.Find(Str(P, 0));
		return (Flag ? *Flag : false) == (Int(P, 1) != 0);
	}
	case EGenScriptCondition::C_TIMER_EXPIRED:
	{
		// (A timer that was never set has expired: it stands at 0, not counting down... "value < 1".)
		const FCounter* Timer = Counters.Find(Str(P, 0));
		return Timer && Timer->bCountdown && Timer->Value < 1;
	}
	case EGenScriptCondition::C_PLAYER_HAS_OBJECT_COMPARISON:
		return PlayerUnitComparison(C, GetPlayerNamed(Str(P, 0)), Int(P, 1), Int(P, 2), Str(P, 3));
	case EGenScriptCondition::C_PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 4));
		if (Player == INDEX_NONE || !Area)
		{
			return false;
		}
		const TArray<const FGenThingTemplate*>& Wanted = ResolveTypes(Str(P, 3));
		const int32 N = CountInArea(Player, *Area, [&Wanted](const FGenObject& O) { return Wanted.Contains(O.GetTemplate()->Canonical()); });
		return Compare(N, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_PLAYER_HAS_COMPARISON_UNIT_KIND_IN_TRIGGER_AREA:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 4));
		if (Player == INDEX_NONE || !Area)
		{
			return false;
		}
		const FString Kind = Str(P, 3).ToUpper();
		const int32 N = CountInArea(Player, *Area, [&Kind](const FGenObject& O) { return O.IsKindOf(*Kind); });
		return Compare(N, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_PLAYER_ALL_DESTROYED:
	{
		// Player::hasAnyObjects: anything of the player's still standing (INERT things aside).
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return true;
		}
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			if (IsLiving(O) && !O->IsProjectile() && !O->IsKindOf(TEXT("INERT")))
			{
				return false;
			}
		}
		return true;
	}
	case EGenScriptCondition::C_NAMED_NOT_DESTROYED:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && !O->IsDead();
	}
	case EGenScriptCondition::C_NAMED_DESTROYED:
	{
		// evaluateNamedUnitDestroyed: dead, or it existed once and is gone.
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O ? O->IsDead() : NamesThatExisted.Contains(Str(P, 0).ToLower());
	}
	case EGenScriptCondition::C_NAMED_ENTERED_AREA:
		return NamedEnteredArea(C, Str(P, 0), Str(P, 1));
	case EGenScriptCondition::C_NAMED_ATTACKED_BY_PLAYER:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && AttackedByPlayer(*O, GetPlayerNamed(Str(P, 1)));
	}
	case EGenScriptCondition::C_NAMED_OWNED_BY_PLAYER:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		const int32 Player = GetPlayerNamed(Str(P, 1));
		return O && Player != INDEX_NONE && O->GetOwner() == Player;
	}
	case EGenScriptCondition::C_NAMED_HAS_FREE_CONTAINER_SLOTS:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && O->GetContain() && Logic->GetContainCount(*O) < Logic->GetContainMax(*O);
	}
	case EGenScriptCondition::C_UNIT_HAS_OBJECT_STATUS:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && HasObjectStatus(*O, Str(P, 1));
	}
	case EGenScriptCondition::C_TEAM_INSIDE_AREA_PARTIALLY:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return Team && Area && TeamInside(*Team, *Area, uint32(Int(P, 2)), false);
	}
	case EGenScriptCondition::C_TEAM_ENTERED_AREA_ENTIRELY:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return Team && Area && TeamInside(*Team, *Area, uint32(Int(P, 2)), true);
	}
	case EGenScriptCondition::C_TEAM_HAS_UNITS:
	case EGenScriptCondition::C_TEAM_DESTROYED:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		TArray<FGenObject*> Members;
		if (Team)
		{
			GetTeamMembers(*Team, Members);
		}
		const bool bHas = Members.Num() > 0;
		return C.Type == EGenScriptCondition::C_TEAM_HAS_UNITS ? bHas : !bHas;
	}
	case EGenScriptCondition::C_TEAM_DISCOVERED:
	{
		// evaluateTeamDiscovered: a member the player can see (not held, not hidden by stealth).
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const int32 Player = GetPlayerNamed(Str(P, 1));
		if (!Team || Player == INDEX_NONE)
		{
			return false;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members, true);
		for (const FGenObject* O : Members)
		{
			if (O->bScriptHeld || (O->IsStealthed() && !O->IsDetected() && !O->IsDisguised()))
			{
				continue;
			}
			bool bEverSeen = false;
			const EGenObjectShroud Shroud = Logic->ComputeObjectShroud(*O, Player, bEverSeen);
			if (Shroud == EGenObjectShroud::Clear || Shroud == EGenObjectShroud::PartialClear)
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_TEAM_ATTACKED_BY_PLAYER:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const int32 Player = GetPlayerNamed(Str(P, 1));
		if (!Team || Player == INDEX_NONE)
		{
			return false;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members, true);
		for (const FGenObject* O : Members)
		{
			// (evaluateTeamAttackedByPlayer looks at the attacker that is still there.)
			const FGenObject* Attacker = O->LastDamageFrame != 0 ? Logic->FindObject(O->LastDamageSource) : nullptr;
			if (Attacker && Attacker->GetOwner() == Player)
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_TEAM_ATTACKED_BY_OBJECTTYPE:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		if (!Team)
		{
			return false;
		}
		const TArray<const FGenThingTemplate*>& Wanted = ResolveTypes(Str(P, 1));
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members, true);
		for (const FGenObject* O : Members)
		{
			if (O->LastDamageFrame != 0 && O->LastDamageSourceTemplate && Wanted.Contains(O->LastDamageSourceTemplate->Canonical()))
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_HAS_FINISHED_SPEECH:
		return IsSoundComplete(Str(P, 0), TestingSpeech);
	case EGenScriptCondition::C_HAS_FINISHED_AUDIO:
		return IsSoundComplete(Str(P, 0), TestingAudio);
	case EGenScriptCondition::C_MUSIC_TRACK_HAS_COMPLETED:
	{
		// AudioManager::hasMusicTrackCompleted: it is the track playing, and it has come to its end that many times.
		if (!MusicTrack.Equals(Str(P, 0), ESearchCase::IgnoreCase))
		{
			return false;
		}
		const float Seconds = FGenAudioData::Get().GetEventLengthSeconds(MusicTrack);
		if (Seconds <= 0.f)
		{
			return false;
		}
		const int32 Times = int32(float(Frame - MusicStartFrame) / (Seconds * float(GenLogic::GetLogicFPS())));
		return Times >= FMath::Max(1, Int(P, 1));
	}
	case EGenScriptCondition::C_CAMERA_MOVEMENT_FINISHED:
		return Frame >= CameraBusyUntil;
	default:
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] condition %s isn't ported: false"), Frame, *C.TypeName);
		return false;
	}
}
