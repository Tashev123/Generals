// The script engine of a map: ScriptEngine.cpp (evaluation, flags / counters / timers, subroutines, fades, the
// named objects and teams of the map) and ScriptConditions.cpp. The actions are in GenMapScriptActions.cpp.

#include "Logic/GenMapScripts.h"
#include "Audio/GenAudioData.h"
#include "Core/GenPerf.h"
#include "Logic/GenCommandData.h"
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

	/** Team.cpp locoSetMatches: a script's surface number: 1 ground, 2 air. */
	uint32 SurfacesFromScript(int32 Value)
	{
		return ((Value & 1) != 0 ? uint32(GenSurface::Ground) : 0u) | ((Value & 2) != 0 ? uint32(GenSurface::Air) : 0u);
	}

	bool IsTeamCondition(EGenScriptCondition Type)
	{
		return Type == EGenScriptCondition::C_TEAM_HAS_UNITS || Type == EGenScriptCondition::C_TEAM_INSIDE_AREA_PARTIALLY
			|| Type == EGenScriptCondition::C_TEAM_DESTROYED || Type == EGenScriptCondition::C_TEAM_ENTERED_AREA_ENTIRELY
			|| Type == EGenScriptCondition::C_TEAM_OUTSIDE_AREA_ENTIRELY || Type == EGenScriptCondition::C_TEAM_SOME_HAVE_OBJECT_STATUS
			|| Type == EGenScriptCondition::C_TEAM_DISCOVERED || Type == EGenScriptCondition::C_TEAM_ATTACKED_BY_PLAYER
			|| Type == EGenScriptCondition::C_TEAM_ATTACKED_BY_OBJECTTYPE || Type == EGenScriptCondition::C_TEAM_ENTERED_AREA_PARTIALLY
			|| Type == EGenScriptCondition::C_TEAM_CREATED || Type == EGenScriptCondition::C_TEAM_EXITED_AREA_PARTIALLY
			|| Type == EGenScriptCondition::C_TEAM_INSIDE_AREA_ENTIRELY || Type == EGenScriptCondition::C_TEAM_OWNED_BY_PLAYER
			|| Type == EGenScriptCondition::C_TEAM_REACHED_WAYPOINTS_END || Type == EGenScriptCondition::C_TEAM_STATE_IS
			|| Type == EGenScriptCondition::C_TEAM_ALL_HAS_OBJECT_STATUS;
	}

	/** ThingTemplate::isBuildFacility, which the original works out from what other things need: here, a command
	 *  center or a structure with something to build. */
	bool IsBuildFacility(const FGenObject& O)
	{
		if (!O.IsKindOf(TEXT("STRUCTURE")))
		{
			return false;
		}
		if (O.IsKindOf(TEXT("COMMANDCENTER")))
		{
			return true;
		}
		const FGenCommandData& CD = FGenCommandData::Get();
		const FGenCommandSet* Set = CD.FindSet(O.GetCommandSet());
		if (Set)
		{
			for (const FString& Slot : Set->Slots)
			{
				const FGenCommandButton* Button = CD.FindButton(Slot);
				if (Button && Button->Command == EGenCommand::UnitBuild)
				{
					return true;
				}
			}
		}
		return false;
	}
}

// ---- client events ------------------------------------------------------------------------------

const FString& FGenScriptClientEvent::Str(int32 Index) const { return ::Str(Params, Index); }
int32 FGenScriptClientEvent::Int(int32 Index) const { return ::Int(Params, Index); }
float FGenScriptClientEvent::Real(int32 Index) const { return Params.IsValidIndex(Index) ? Params[Index].Real : 0.f; }

// ---- setup --------------------------------------------------------------------------------------

FGenMapScripts::FGenMapScripts(UGenGameLogic* InLogic)
	: Logic(InLogic), TF(InLogic->GetTeamFactory())
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
		// checkConditionsForTeamNames: "Offset by a random number of frames". GLA mission 4 leans on it: its toxin count
		// goes down when the player has money, which a script takes away on the first frame. (Never on those first
		// frames here: where the original could draw 0, that mission would be won at once.)
		S.FrameToEvaluate = S.DelaySeconds > 0 ? uint32(Logic->GetRandom().RandRange(2, 2 * GenLogic::GetLogicFPS())) : 0;
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
	bThePlayerIsLocal = Logic->IsChallenge(); // (the teams above have the owners the map gives them)
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
		// (A map's teams are taken as active from the start: a map's teams are filled at load, so a team that is
		// empty has no members to wait for. What a computer player builds is inactive until it is all there.)
		Team.bActive = true;
		// "team<PlayerName>" is the player's default team (the neutral player's is "team"). Player::setDefaultTeam.
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
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectAggressiveness")))
	{
		Obj.ScriptAttitude = int8(FMath::Clamp(V->Int, -2, 2)); // (ai->setAttitude: mission 3's captured Paladins sleep till they are found)
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectSelectable")); V && !V->Bool)
	{
		Obj.bUnselectable = true;
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectEnabled")); V && !V->Bool)
	{
		Obj.bScriptDisabled = true;
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectPowered")); V && !V->Bool)
	{
		Obj.bScriptDisabled = true;
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectUnsellable")))
	{
		Obj.bScriptUnsellable = V->Bool;
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectTargetable")))
	{
		Obj.bScriptTargetable = V->Bool; // (mission 4's radio station: a civilian's building that a player may attack without force)
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectVisualRange")))
	{
		Obj.MapVisionRange = float(FMath::Max(V->Int, 0));
	}
	if (const FGenDictValue* V = MapObj.Properties.Find(TEXT("objectShroudClearingDistance")))
	{
		Obj.ShroudClearingRange = float(FMath::Max(V->Int, 0));
	}
}

// ---- evaluation ---------------------------------------------------------------------------------

void FGenMapScripts::Update(uint32 InFrame)
{
	GEN_PERF_SCOPE("MapScripts");
	Frame = InFrame;
	// ScriptEngine::update: the count of objects made or destroyed since this is set back to nothing, so a cached
	// condition knows when a change it cannot see on its own could have moved the answer.
	FrameObjectCountChanged = 0;
	if (Fade.Type != FGenScriptFade::None)
	{
		UpdateFade();
	}
	if (bEnded)
	{
		return; // ScriptEngine::update: "we are just timing down" once the game is won or lost
	}
	UpdateCompletedRoutes();
	UpdateWaterMoves();
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
	// ScriptEngine::update calls PlayerList::updateTeamStates after the scripts: every player's teams run their
	// own scripts (on-create once, a member destroyed, idle, an enemy sighted), and the enter/exit flags the
	// previous frame's objects set are cleared for the next frame's. A mission's computer sides are part of this
	// too - their AI only builds.
	TF.UpdateTeamStates([this](int32 TeamId, const FString& Name) { RunTeamScript(TeamId, Name); });
	UpdateSequentialScripts();
	for (int32 i = PausedPowers.Num() - 1; i >= 0; --i)
	{
		FGenObject* O = Logic->FindObject(PausedPowers[i].Object);
		if (O && O->PowerReady.IsValidIndex(PausedPowers[i].Module))
		{
			O->PowerReady[PausedPowers[i].Module] = Frame + PausedPowers[i].Left;
		}
		else
		{
			PausedPowers.RemoveAt(i);
		}
	}

	// Drawable::update's flashing: every half second, as many times as the script asked for.
	if (Frame % FramesPerFlash == 0)
	{
		for (auto It = Flashing.CreateIterator(); It; ++It)
		{
			if (FGenObject* O = Logic->FindObject(It.Key()); IsLiving(O) && It.Value() > 0)
			{
				const FLinearColor* Color = FlashColors.Find(It.Key());
				O->FlashAs(Color ? *Color : FLinearColor::White);
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

void FGenMapScripts::UpdateCompletedRoutes()
{
	// (The original's state machine says so as it finishes, for one frame. Here: the unit has come to the end of the
	// path, or has stopped near it where the end was taken.)
	CompletedRoutes.Reset();
	for (int32 i = RouteWatches.Num() - 1; i >= 0; --i)
	{
		const FRouteWatch& Watch = RouteWatches[i];
		const FGenObject* O = Logic->FindObject(Watch.Object);
		if (!IsLiving(O))
		{
			RouteWatches.RemoveAt(i);
			continue;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Watch.End);
		if (DistSq <= FMath::Square(30.f) || (O->IsAIIdle() && DistSq <= FMath::Square(120.f)))
		{
			CompletedRoutes.Add(Watch.Object, Watch.Label);
			RouteWatches.RemoveAt(i);
		}
	}
}

void FGenMapScripts::UpdateWaterMoves()
{
	// TerrainLogic::update: a step of each; the drawn water follows a few times a second.
	for (int32 i = WaterMoves.Num() - 1; i >= 0; --i)
	{
		const FWaterMove& Move = WaterMoves[i];
		const float T = FMath::Clamp(float(Frame - Move.Start) / float(Move.Frames), 0.f, 1.f);
		const bool bDone = T >= 1.f;
		Logic->SetScriptWaterHeight(Move.Name, FMath::Lerp(Move.From, Move.To, T), bDone);
		if (bDone || (Frame - Move.Start) % FramesPerFlash == 0)
		{
			FGenScriptClientEvent& E = ClientEvents.AddDefaulted_GetRef();
			E.Action = EGenScriptAction::A_WATER_CHANGE_HEIGHT_OVER_TIME;
			E.Frame = Frame;
		}
		if (bDone)
		{
			WaterMoves.RemoveAt(i);
		}
	}
}

bool FGenMapScripts::GetToppleDirection(const FGenObject& Obj, FVector2f& Out) const
{
	const FVector2f* Dir = Obj.ScriptName.IsEmpty() ? nullptr : ToppleDirections.Find(Obj.ScriptName);
	if (Dir)
	{
		Out = *Dir;
	}
	return Dir != nullptr;
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
	if (Instances.Num() > 0)
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
		if (And.Num() == 0)
		{
			continue; // (evaluateConditions: an OR block with nothing in it counts for nothing.)
		}
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

void FGenMapScripts::RunTeamScript(int32 TeamId, const FString& Name)
{
	// ScriptEngine::runScript: a team's own script, run as that team, with its player as <This Player>.
	FGenMapTeam* Team = FindTeam(TeamId);
	if (!Team || Name.IsEmpty() || Name == TEXT("<none>"))
	{
		return;
	}
	TGuardValue<int32> PlayerGuard(CurrentPlayer, Team->Player);
	TGuardValue<FGenMapTeam*> TeamGuard(CallingTeam, Team);
	CallSubroutine(Name);
}

void FGenMapScripts::NotifyTeamOfObjectDeath(FGenObject& Obj)
{
	TF.NotifyTeamOfObjectDeath(Obj, [this](int32 TeamId, const FString& Name) { RunTeamScript(TeamId, Name); });
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
			for (FGenScript& Sub : G->Scripts)
			{
				if (Sub.bActive)
				{
					ExecuteScript(Sub);
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

void FGenMapScripts::AppendSequential(int32 TeamId, FGenObjectID Object, const FGenScript* Script, int32 TimesToLoop)
{
	// ScriptEngine::appendSequentialScript goes behind what is already waiting for the same team or the same unit.
	// The port keeps them as separate entries in the same order, which comes to the same thing: an entry runs its
	// script to the end and is then gone, and the next one for that team or unit is behind it.
	// (Only the first entry of a team or a unit runs: UpdateSequentialScripts.)
	FSequential& New = SequentialScripts.AddDefaulted_GetRef();
	New.Team = TeamId;
	New.Object = Object;
	New.Script = Script;
	New.TimesToLoop = TimesToLoop;
	New.Uid = NextSequentialId++;
}

void FGenMapScripts::SetSequentialTimer(int32 TeamId, int32 Frames)
{
	for (FSequential& Seq : SequentialScripts)
	{
		if (Seq.Team == TeamId && Seq.Object == GenInvalidObjectID)
		{
			Seq.FramesToWait = Frames;
			return;
		}
	}
}

bool FGenMapScripts::TeamAnyContained(const FGenMapTeam& Team) const
{
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members);
	for (const FGenObject* O : Members)
	{
		if (O->IsContained())
		{
			return true;
		}
	}
	return false;
}

void FGenMapScripts::KillTeam(FGenMapTeam& Team)
{
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members);
	for (FGenObject* O : Members)
	{
		if (O->GetContain())
		{
			Logic->EvacuateContainer(*O);
		}
	}
	const int32 Neutral = Logic->GetNeutralPlayer();
	for (FGenObject* O : Members)
	{
		if (!IsLiving(O))
		{
			continue;
		}
		if (O->GetTemplate()->bTechBuilding && Neutral != INDEX_NONE)
		{
			Logic->ChangeOwner(*O, Neutral, false);
			if (const int32* Id = DefaultTeams.Find(Neutral))
			{
				if (FGenMapTeam* NeutralTeam = FindTeam(*Id)) { SetObjectTeam(*O, *NeutralTeam); }
			}
		}
		else if (!O->bIndestructible)
		{
			O->Kill();
		}
	}
}

void FGenMapScripts::UpdateSequentialScripts()
{
	// ScriptEngine::evaluateAndProgressAllSequentialScripts: an action runs when the unit it names, or the team it
	// runs on, stands idle - several in one frame while they keep finishing at once. The idle gate is what makes a
	// sequence mean anything: 'LCAC01 Deploy Sequence' is follow waypoints, get out, follow waypoints, and each
	// step has to be done before the next one is asked for.
	for (int32 i = 0; i < SequentialScripts.Num(); ++i)
	{
		// (The ones behind it on the same team or the same unit: m_nextScriptInSequence.)
		bool bBehindAnother = false;
		for (int32 j = 0; j < i && !bBehindAnother; ++j)
		{
			bBehindAnother = SequentialScripts[j].Object == SequentialScripts[i].Object
				&& (SequentialScripts[i].Object != GenInvalidObjectID || SequentialScripts[j].Team == SequentialScripts[i].Team);
		}
		if (bBehindAnother)
		{
			continue;
		}
		for (int32 Spin = 0; Spin < 20; ++Spin)
		{
			if (!SequentialScripts.IsValidIndex(i))
			{
				break;
			}
			FSequential& Seq = SequentialScripts[i];
			FGenMapTeam* Team = Seq.Team != 0 ? FindTeam(Seq.Team) : nullptr;
			FGenObject* Obj = Seq.Object != GenInvalidObjectID ? Logic->FindObject(Seq.Object) : nullptr;
			if ((Seq.Object != GenInvalidObjectID ? !Obj : !Team) || !Seq.Script)
			{
				SequentialScripts.RemoveAt(i);
				--i;
				break;
			}
			// AIGroup::isIdle for a team, AIUpdateInterface::isIdle for the unit the sequence runs on.
			const bool bIdle = Obj ? Obj->IsAIIdle() : TeamIsIdle(*Team);
			if (!((bIdle && Seq.FramesToWait < 1) || Seq.FramesToWait == 0))
			{
				if (Seq.FramesToWait > 0)
				{
					--Seq.FramesToWait;
				}
				break;
			}
			++Seq.Instruction;
			if (!Seq.Script->Actions.IsValidIndex(Seq.Instruction))
			{
				// This script is done. If the action asked for it more than once, it starts again behind the others.
				if (Seq.TimesToLoop != 0)
				{
					if (Seq.TimesToLoop != -1)
					{
						--Seq.TimesToLoop;
					}
					const int32 TeamId = Seq.Team;
					const FGenObjectID ObjectId = Seq.Object;
					const FGenScript* Again = Seq.Script;
					const int32 Times = Seq.TimesToLoop;
					AppendSequential(TeamId, ObjectId, Again, Times);
				}
				SequentialScripts.RemoveAt(i);
				--i;
				break;
			}
			const FGenScriptAction& Action = Seq.Script->Actions[Seq.Instruction];
			if (Action.Type == EGenScriptAction::A_TEAM_WAIT_FOR_NOT_CONTAINED_ALL)
			{
				// (Only for sequential scripts: this instruction again until nothing is inside anything.)
				const FGenMapTeam* Waiting = Str(Action.Params, 0) == ThisTeamName ? Team : GetTeamNamed(Str(Action.Params, 0));
				if (Waiting && TeamAnyContained(*Waiting))
				{
					--Seq.Instruction;
					break;
				}
				continue;
			}
			if (Action.Type == EGenScriptAction::A_SKIRMISH_WAIT_FOR_COMMANDBUTTON_AVAILABLE_ALL)
			{
				// (Only for sequential scripts, too: this instruction again until the button can be used.)
				const FGenMapTeam* Waiting = Str(Action.Params, 1) == ThisTeamName ? Team : GetTeamNamed(Str(Action.Params, 1));
				if (!Waiting || !TeamCommandButtonReady(*Waiting, Str(Action.Params, 2)))
				{
					--Seq.Instruction;
					break;
				}
				continue;
			}
			Seq.FramesToWait = -1;
			const int32 Uid = Seq.Uid;
			const int32 TeamId = Seq.Team;
			{
				// (m_conditionTeam, m_conditionObject, m_currentPlayer: <This Team>, <This Object>, <This Player>.)
				TGuardValue<FGenMapTeam*> TeamGuard(CallingTeam, Team);
				TGuardValue<FGenObjectID> ObjectGuard(ThisObject, Obj ? Obj->GetID() : GenInvalidObjectID);
				TGuardValue<int32> PlayerGuard(CurrentPlayer, Obj ? Obj->GetOwner() : (Team ? Team->Player : CurrentPlayer));
				ExecuteAction(Action);
			}
			i = SequentialScripts.IndexOfByPredicate([Uid](const FSequential& S) { return S.Uid == Uid; });
			if (i == INDEX_NONE)
			{
				// (It stopped itself. Everything has moved: the rest are looked at the next frame.)
				i = SequentialScripts.Num();
				break;
			}
			// "check to make sure they aren't dead" - an action can take the unit away, or disband the team it
			// runs on, so both are looked up again before going on.
			Obj = SequentialScripts[i].Object != GenInvalidObjectID ? Logic->FindObject(SequentialScripts[i].Object) : nullptr;
			if (SequentialScripts[i].Object != GenInvalidObjectID && !IsLiving(Obj))
			{
				SequentialScripts.RemoveAt(i);
				--i;
				break;
			}
			Team = TeamId != 0 ? FindTeam(TeamId) : nullptr;
			if (SequentialScripts[i].Object == GenInvalidObjectID)
			{
				TArray<FGenObject*> Left;
				if (Team) { GetTeamMembers(*Team, Left); }
				if (Left.Num() == 0)
				{
					SequentialScripts.RemoveAt(i); // AIGroup::isGroupAiDead
					--i;
					break;
				}
			}
			// The engine stays on this entry while it is still idle, so actions that finish at once run in the same
			// frame; one that started a move leaves the unit busy and the entry waits for the next frame.
			if (SequentialScripts[i].FramesToWait > 0 || !(Obj ? Obj->IsAIIdle() : (Team && TeamIsIdle(*Team))))
			{
				break;
			}
		}
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
	if (Name == LocalPlayerName || (bThePlayerIsLocal && Name == TEXT("ThePlayer")))
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

FGenMapTeam* FGenMapScripts::GetTeamNamed(const FString& Name)
{
	// ScriptEngine::getTeamNamed: <This Team>, the team the script runs for, or the definition's (latest) instance.
	if (bThePlayerIsLocal && Name == TEXT("teamThePlayer"))
	{
		return &TF.EnsureDefaultTeam(Logic->GetLocalPlayer());
	}
	if (Name == ThisTeamName)
	{
		return CallingTeam;
	}
	if (CallingTeam && Protos.IsValidIndex(CallingTeam->Proto) && Protos[CallingTeam->Proto].Name.Equals(Name, ESearchCase::CaseSensitive))
	{
		return CallingTeam;
	}
	return TF.FindTeamNamed(Name);
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

void FGenMapScripts::NameObject(FGenObject& Obj, const FString& Name)
{
	Obj.ScriptName = Name;
	Named.Add(Name.ToLower(), Obj.GetID());
	NamesThatExisted.Add(Name.ToLower());
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script] #%u %s is named %s"), Obj.GetID(), *Obj.GetTemplate()->Name, *Name);
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
	if (Name == TEXT("<This Object>"))
	{
		return Logic->FindObject(ThisObject);
	}
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

int32 FGenMapScripts::GetWaypointPath(const FGenMapObject& Start, TArray<FVector2f>& Out) const
{
	Out.Reset();
	return Map ? Map->GetWaypointPath(Start, Out) : -1;
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

int32 FGenMapScripts::GetObjectTeam(const FGenObject& Obj) const
{
	return TF.GetObjectTeam(Obj.GetID());
}

bool FGenMapScripts::IsButtonRemoved(const FString& CommandSet, int32 Slot) const
{
	const TArray<int32>* Slots = RemovedButtons.Find(CommandSet.ToLower());
	return Slots && Slots->Contains(Slot);
}

const FString* FGenMapScripts::GetButtonAdded(const FString& CommandSet, int32 Slot) const
{
	const TMap<int32, FString>* Slots = AddedButtons.Find(CommandSet.ToLower());
	return Slots ? Slots->Find(Slot) : nullptr;
}

bool FGenMapScripts::IsScienceHidden(int32 Player, const FString& Science) const
{
	const TSet<FString>* Set = HiddenSciences.Find(Player);
	return Set && Set->Contains(Science.ToLower());
}

bool FGenMapScripts::IsScienceDisabled(int32 Player, const FString& Science) const
{
	const TSet<FString>* Set = DisabledSciences.Find(Player);
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

int32 FGenMapScripts::CountOfTypes(int32 Player, const FString& Types) const
{
	// Player::countObjectsByThingTemplate (living things of those types).
	const TArray<const FGenThingTemplate*>& Wanted = ResolveTypes(Types);
	int32 N = 0;
	for (const FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (IsLiving(O) && !O->IsUnderConstruction() && Wanted.Contains(O->GetTemplate()->Canonical()))
		{
			++N;
		}
	}
	return N;
}

bool FGenMapScripts::PlayerUnitComparison(const FGenScriptCondition& C, int32 Player, int32 Comparison, int32 Count, const FString& Types) const
{
	// evaluatePlayerUnitCondition
	return Compare(CountOfTypes(Player, Types), Comparison, Count);
}

bool FGenMapScripts::TeamMemberExited(const FGenScriptCondition& C, const FGenMapTeam& Team, const FGenPolygonTrigger& Area, uint32 Surfaces)
{
	// (Object::didExit is worked out here from what was inside when this was last asked: the scripts that ask are
	// on all the time. What is kept is only ever looked up, never walked.)
	TSet<FGenObjectID>& Was = AreaMembers.FindOrAdd(TPair<const FGenScriptCondition*, int32>(&C, Team.Id));
	TSet<FGenObjectID> Now;
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members);
	bool bExited = false;
	for (const FGenObject* O : Members)
	{
		const uint32 Theirs = O->CanMove() ? O->GetPathSurfaces() : uint32(GenSurface::Ground);
		if ((Surfaces != 0 && (Theirs & Surfaces) == 0) || O->IsKindOf(TEXT("INERT")))
		{
			continue;
		}
		if (Area.Contains(O->GetPosition().X, O->GetPosition().Y))
		{
			Now.Add(O->GetID());
		}
		else if (Was.Contains(O->GetID()))
		{
			bExited = true;
		}
	}
	Was = MoveTemp(Now);
	return bExited;
}

bool FGenMapScripts::ObjectHasButton(const FGenObject& Obj, const FString& ButtonName) const
{
	const FGenCommandSet* Set = FGenCommandData::Get().FindSet(Obj.GetCommandSet());
	if (Set)
	{
		for (const FString& Slot : Set->Slots)
		{
			if (Slot.Equals(ButtonName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
	}
	return false;
}

bool FGenMapScripts::TeamCommandButtonReady(const FGenMapTeam& Team, const FString& ButtonName) const
{
	const FGenCommandButton* Button = FGenCommandData::Get().FindButton(ButtonName);
	if (!Button)
	{
		return false;
	}
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members);
	for (const FGenObject* O : Members)
	{
		// CommandButton::isReady. (An upgrade's button, in the original, is ready for what doesn't have it yet.)
		const int32 Module = Button->SpecialPower.IsEmpty() ? INDEX_NONE : Logic->FindPowerModule(*O, Button->SpecialPower);
		if (Module != INDEX_NONE && !Logic->IsPowerReady(*O, Module))
		{
			return false;
		}
	}
	return true;
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

bool FGenMapScripts::PlayerUnitsInArea(const FGenScriptCondition& C, int32 Player, const FGenPolygonTrigger& Area) const
{
	// evaluateSkirmishPlayerHasUnitsInArea (ScriptConditions.cpp:2496). It counts the members of the player's team
	// instances; every object is on one of its teams (its default one at worst), so counting what the player has
	// in the area is the same thing. The answer is kept on the condition and worked out again only when something
	// could have changed - a team that came in or out, or an object made or destroyed since the last time. That
	// cache cannot change an answer; it is there because these are asked every frame by scripts that stay on.
	bool bChanged = C.CustomData == 0;
	if (!bChanged)
	{
		for (const TUniquePtr<FGenTeam>& Ptr : Teams)
		{
			const FGenTeam* Team = Ptr.Get();
			if (Team && Team->Player == Player && Team->bEnteredOrExited)
			{
				bChanged = true;
				break;
			}
		}
	}
	if (!bChanged && FrameObjectCountChanged > C.CustomFrame)
	{
		bChanged = true; // "Objects were added/deleted last frame, so count could have changed."
	}
	if (!bChanged)
	{
		return C.CustomData == 1;
	}

	const int32 Count = CountInArea(Player, Area, [](const FGenObject&) { return true; });
	C.CustomData = Count > 0 ? 1 : -1;
	C.CustomFrame = FrameObjectCountChanged;
	return Count > 0;
}

bool FGenMapScripts::EvaluateProductionCondition(int32 ProtoIndex)
{
	// TeamPrototype::evaluateProductionCondition (Common/RTS/Team.cpp:1128), evaluated as the definition's own
	// player rather than as the script that asks. A definition with no condition script is always false; the
	// definitions of a mission's sides have none, so their teams come from the script action BUILD_TEAM.
	if (!Protos.IsValidIndex(ProtoIndex))
	{
		return false;
	}
	FGenTeamProto& Proto = Protos[ProtoIndex];
	if (Proto.bConditionAlwaysFalse)
	{
		return false;
	}
	if (!Proto.ConditionScript)
	{
		const FGenScript* Script = Proto.ProductionCondition.IsEmpty() ? nullptr : FindScript(Proto.ProductionCondition);
		const bool bLevel = Script && ((Difficulty == EGenDifficulty::Easy && Script->bEasy)
			|| (Difficulty == EGenDifficulty::Normal && Script->bNormal) || (Difficulty == EGenDifficulty::Hard && Script->bHard));
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
	TGuardValue<int32> GuardPlayer(CurrentPlayer, ProtoPlayers.IsValidIndex(ProtoIndex) ? ProtoPlayers[ProtoIndex] : CurrentPlayer);
	return EvaluateConditions(*Proto.ConditionScript);
}

bool FGenMapScripts::TeamIsIdle(const FGenMapTeam& Team) const
{
	// AIGroup::isIdle: every member that can move is standing idle.
	bool bIdle = true;
	TF.ForEachMember(Team, [](const FGenObject&) { return true; }, [&bIdle](FGenObject& O) { bIdle &= !O.CanMove() || O.IsAIIdle(); });
	return bIdle;
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
	if (Status.Equals(TEXT("IS_FIRING_WEAPON"), ESearchCase::IgnoreCase)) { return Obj.IsFiringWeaponForScripts(); }
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
	case EGenScriptCondition::C_SKIRMISH_PLAYER_HAS_UNITS_IN_AREA:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		if (Player == INDEX_NONE || !Area)
		{
			return false;
		}
		return PlayerUnitsInArea(C, Player, *Area);
	}
	case EGenScriptCondition::C_SKIRMISH_PLAYER_HAS_BEEN_ATTACKED_BY_PLAYER:
	{
		// Player::getAttackedBy: which sides have hurt something of this one. It is set by damage that actually
		// landed (GenObjectCombat.cpp), and once set it stays; a script's own NAMED_DAMAGE does not set it.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const int32 Attacker = GetPlayerNamed(Str(P, 1));
		return Player != INDEX_NONE && Attacker != INDEX_NONE && Logic->WasAttackedBy(Player, Attacker);
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
		return Team && Area && TeamInside(*Team, *Area, SurfacesFromScript(Int(P, 2)), false);
	}
	case EGenScriptCondition::C_TEAM_ENTERED_AREA_ENTIRELY:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return Team && Area && TeamInside(*Team, *Area, SurfacesFromScript(Int(P, 2)), true);
	}
	case EGenScriptCondition::C_TEAM_HAS_UNITS:
	{
		// evaluateHasUnits: <This Team> and the team of that name the script runs for, else any instance of the definition
		// (Team::hasAnyUnits: "If it's a structure, it's not a unit", nor a projectile, nor a mine).
		auto HasUnits = [this](const FGenMapTeam& Team)
		{
			TArray<FGenObject*> Members;
			GetTeamMembers(Team, Members);
			return Members.ContainsByPredicate([](const FGenObject* O)
			{
				return !O->IsKindOf(TEXT("STRUCTURE")) && !O->IsKindOf(TEXT("PROJECTILE")) && !O->IsKindOf(TEXT("MINE"));
			});
		};
		const FString& Name = Str(P, 0);
		const FGenMapTeam* This = GetTeamNamed(ThisTeamName);
		if (Name == ThisTeamName)
		{
			return This && HasUnits(*This);
		}
		if (This && Protos.IsValidIndex(This->Proto) && Protos[This->Proto].Name.Equals(Name, ESearchCase::CaseSensitive))
		{
			return HasUnits(*This);
		}
		const int32 Proto = FindProto(Name);
		for (const TUniquePtr<FGenTeam>& Ptr : Teams)
		{
			if (Proto != INDEX_NONE && Ptr->Proto == Proto && HasUnits(*Ptr))
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_TEAM_DESTROYED:
	{
		// evaluateIsDestroyed: "Non existent team is not destroyed." (Team::hasAnyObjects: shells, inert things and mines don't count.)
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		if (!Team)
		{
			return false;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members);
		return !Members.ContainsByPredicate([](const FGenObject* O) { return !O->IsKindOf(TEXT("PROJECTILE")) && !O->IsKindOf(TEXT("INERT")) && !O->IsKindOf(TEXT("MINE")); });
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
	case EGenScriptCondition::C_TEAM_OUTSIDE_AREA_ENTIRELY:
	{
		// evaluateTeamOutsideAreaEntirely: Team::allOutside, and a team that isn't there is outside.
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return !Team || (Area && !TeamInside(*Team, *Area, SurfacesFromScript(Int(P, 2)), false));
	}
	case EGenScriptCondition::C_MULTIPLAYER_ALLIED_DEFEAT:
		// evaluateMultiplayerAlliedDefeat -> VictoryConditions, which does nothing unless the game is multiplayer: no
		// mission gets a yes from this (the Challenge maps' taunt for it is never heard).
		return false;
	case EGenScriptCondition::C_PLAYER_EXCESS_POWER_COMPARE_VALUE:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return false;
		}
		int32 Produced = 0, Consumed = 0;
		Logic->GetEnergy(Player, Produced, Consumed);
		return Compare(Produced - Consumed, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_PLAYER_HAS_N_OR_FEWER_FACTION_BUILDINGS:
	{
		// evaluatePlayerHasNOrFewerFactionBuildings: the structures that count for victory.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return false;
		}
		int32 Buildings = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			Buildings += IsLiving(O) && O->IsKindOf(TEXT("STRUCTURE")) && O->IsKindOf(TEXT("MP_COUNT_FOR_VICTORY")) ? 1 : 0;
		}
		return Int(P, 1) >= Buildings;
	}
	case EGenScriptCondition::C_SKIRMISH_PLAYER_FACTION:
	{
		// evaluateSkirmishPlayerIsFaction
		const int32 Who = GetPlayerNamed(Str(P, 0));
		const FGenPlayerTemplate* T = Who != INDEX_NONE ? Logic->GetPlayers()[Who].Template : nullptr;
		return T && T->Side == Str(P, 1);
	}
	case EGenScriptCondition::C_SKIRMISH_PLAYER_HAS_COMPARISON_GARRISONED:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return false;
		}
		int32 Garrisoned = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			Garrisoned += IsLiving(O) && O->GetContain() && O->GetTemplate()->Contain.Type == EGenContainType::Garrison && Logic->GetContainCount(*O) > 0 ? 1 : 0;
		}
		return Compare(Garrisoned, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_SUPPLY_SOURCE_SAFE:
	{
		// evaluateSkirmishSupplySourceSafe: looked at every 2 seconds. It's a computer player's question.
		if (Frame <= C.CustomFrame && C.CustomData != 0)
		{
			return C.CustomData == 1;
		}
		C.CustomFrame = Frame + uint32(2 * GenLogic::GetLogicFPS());
		const int32 Player = GetPlayerNamed(Str(P, 0));
		FGenSkirmishAI* AI = Player != INDEX_NONE ? Logic->GetAI(Player) : nullptr;
		const bool bSafe = AI && AI->ScriptSupplySourceSafe(Int(P, 1));
		C.CustomData = bSafe ? 1 : -1;
		return bSafe;
	}
	case EGenScriptCondition::C_TEAM_ALL_HAS_OBJECT_STATUS:
	{
		// evaluateTeamHasObjectStatus, entireTeam: a team with nobody in it passes.
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		if (!Team)
		{
			return false;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members);
		for (const FGenObject* O : Members)
		{
			if (!HasObjectStatus(*O, Str(P, 1)))
			{
				return false;
			}
		}
		return true;
	}
	case EGenScriptCondition::C_TEAM_SOME_HAVE_OBJECT_STATUS:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		TArray<FGenObject*> Members;
		if (Team) { GetTeamMembers(*Team, Members); }
		for (const FGenObject* O : Members)
		{
			if (HasObjectStatus(*O, Str(P, 1)))
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_NAMED_DISCOVERED:
	{
		// evaluateNamedDiscovered
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		const int32 Player = GetPlayerNamed(Str(P, 1));
		if (!O || Player == INDEX_NONE || O->bScriptHeld || (O->IsStealthed() && !O->IsDetected() && !O->IsDisguised()))
		{
			return false;
		}
		bool bEverSeen = false;
		const EGenObjectShroud Shroud = Logic->ComputeObjectShroud(*O, Player, bEverSeen);
		return Shroud == EGenObjectShroud::Clear || Shroud == EGenObjectShroud::PartialClear;
	}
	case EGenScriptCondition::C_NAMED_INSIDE_AREA:
	case EGenScriptCondition::C_NAMED_OUTSIDE_AREA:
	{
		// evaluateNamedOutsideArea: "This is actually NamedUnitInside(...)" - nothing there isn't inside.
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		const bool bInside = O && Area && Area->Contains(O->GetPosition().X, O->GetPosition().Y);
		return C.Type == EGenScriptCondition::C_NAMED_INSIDE_AREA ? bInside : !bInside;
	}
	case EGenScriptCondition::C_ENEMY_SIGHTED:
	case EGenScriptCondition::C_TYPE_SIGHTED:
	{
		// evaluateEnemySighted / evaluateTypeSighted: something of that player's within the unit's sight, and not
		// hidden by stealth, that is of that relation to the unit (or of that type).
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		const bool bEnemy = C.Type == EGenScriptCondition::C_ENEMY_SIGHTED;
		const int32 Player = GetPlayerNamed(Str(P, 2));
		if (!O || Player == INDEX_NONE)
		{
			return false;
		}
		const TArray<const FGenThingTemplate*>* Wanted = bEnemy ? nullptr : &ResolveTypes(Str(P, 1));
		const EGenRelationship Rel = Int(P, 1) == 0 ? EGenRelationship::Enemies : (Int(P, 1) == 2 ? EGenRelationship::Allies : EGenRelationship::Neutral);
		bool bFound = false;
		Logic->GetPartition().ForEachInRange(FVector2f(O->GetPosition().X, O->GetPosition().Y), O->GetVisionRange(), [&](FGenObject& Them)
		{
			if (bFound || !IsLiving(&Them) || Them.GetOwner() != Player || (Them.IsStealthed() && !Them.IsDetected() && !Them.IsDisguised()))
			{
				return;
			}
			bFound = bEnemy ? Logic->GetRelationship(*O, Them) == Rel : Wanted->Contains(Them.GetTemplate()->Canonical());
		});
		return bFound;
	}
	case EGenScriptCondition::C_NAMED_SELECTED:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && SelectedUnits.Contains(O->GetID());
	}
	case EGenScriptCondition::C_SKIRMISH_PLAYER_IS_OUTSIDE_AREA:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return Player != INDEX_NONE && Area && !PlayerUnitsInArea(C, Player, *Area);
	}
	case EGenScriptCondition::C_NAMED_TOTALLY_DEAD:
		// evaluateNamedUnitTotallyDead: it existed once, and nothing is left of it.
		return !GetUnitNamed(Str(P, 0)) && NamesThatExisted.Contains(Str(P, 0).ToLower());
	case EGenScriptCondition::C_TEAM_ENTERED_AREA_PARTIALLY:
	{
		// Team::didPartialEnter: a member came in just now. (Kept per condition: more of the team inside than when
		// this was last asked.)
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		if (!Team || !Area)
		{
			return false;
		}
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members);
		const uint32 Surfaces = SurfacesFromScript(Int(P, 2));
		int32 Inside = 0;
		for (const FGenObject* O : Members)
		{
			const uint32 Theirs = O->CanMove() ? O->GetPathSurfaces() : uint32(GenSurface::Ground);
			if ((Surfaces == 0 || (Theirs & Surfaces) != 0) && !O->IsKindOf(TEXT("INERT")) && Area->Contains(O->GetPosition().X, O->GetPosition().Y))
			{
				++Inside;
			}
		}
		const bool bCameIn = Inside > C.CustomData;
		C.CustomData = Inside;
		return bCameIn;
	}
	case EGenScriptCondition::C_UNIT_HEALTH:
	{
		// evaluateUnitHealth: the percentage, rounded, of what it starts with.
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		if (!O || O->GetMaxHealth() <= 0.f)
		{
			return false;
		}
		const int32 Percent = int32((O->GetHealth() * 100.f + O->GetMaxHealth() * 0.5f) / O->GetMaxHealth());
		return Compare(Percent, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_BUILT_BY_PLAYER:
		// evaluateBuiltByPlayer: the player has one.
		return PlayerUnitComparison(C, GetPlayerNamed(Str(P, 1)), 4, 0, Str(P, 0));
	case EGenScriptCondition::C_PLAYER_HAS_CREDITS:
	{
		// (The original compares the given amount against the money, in that order.)
		const int32 Player = GetPlayerNamed(Str(P, 2));
		return Player != INDEX_NONE && Compare(Int(P, 0), Int(P, 1), Logic->GetMoney(Player));
	}
	case EGenScriptCondition::C_SKIRMISH_SPECIAL_POWER_READY:
	{
		// evaluateSkirmishSpecialPowerIsReady: (looked at every ten seconds at most while the power isn't there or is
		// recharging.)
		if (C.CustomFrame > Frame)
		{
			return false;
		}
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return false;
		}
		int32 Module = INDEX_NONE;
		const FGenObject* Source = Logic->FindPowerSource(Player, Str(P, 1), false, &Module);
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
	case EGenScriptCondition::C_PLAYER_TRIGGERED_SPECIAL_POWER:
	{
		// ScriptEngine::isSpecialPowerTriggered, removeFromList: each firing is seen once.
		TArray<FString>* Fired = TriggeredPowers.Find(GetPlayerNamed(Str(P, 0)));
		const int32 Index = Fired ? Fired->IndexOfByKey(Str(P, 1)) : INDEX_NONE;
		if (Index == INDEX_NONE)
		{
			return false;
		}
		Fired->RemoveAt(Index);
		return true;
	}
	case EGenScriptCondition::C_PLAYER_COMPLETED_SPECIAL_POWER:
	{
		// ScriptEngine::isSpecialPowerComplete, removeFromList: each finish is seen once.
		TArray<FString>* Done = CompletedPowers.Find(GetPlayerNamed(Str(P, 0)));
		const int32 Index = Done ? Done->IndexOfByKey(Str(P, 1)) : INDEX_NONE;
		if (Index == INDEX_NONE)
		{
			return false;
		}
		Done->RemoveAt(Index);
		return true;
	}
	case EGenScriptCondition::C_PLAYER_ACQUIRED_SCIENCE:
	{
		// evaluateScienceAcquired -> ScriptEngine::isScienceAcquired, removeFromList.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		return Player != INDEX_NONE && Logic->ConsumeScriptEvent(Player, Str(P, 1), true);
	}
	case EGenScriptCondition::C_NAMED_BUILDING_IS_EMPTY:
	{
		// evaluateIsBuildingEmpty: false for what can't hold anybody.
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && O->GetContain() && Logic->GetContainCount(*O) == 0;
	}
	case EGenScriptCondition::C_UNIT_EMPTIED:
	{
		// evaluateUnitHasEmptied: it had people in it last frame and has none now. The state stays as it was on the
		// frame that says yes, "because we want to make sure that multiple calls to this in the same frame actually work".
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		if (!O)
		{
			return false;
		}
		const int32 Count = O->GetContain() ? Logic->GetContainCount(*O) : 0;
		FTransportStatus* Status = TransportStatuses.Find(O->GetID());
		if (!Status)
		{
			TransportStatuses.Add(O->GetID(), FTransportStatus{ Frame, Count });
			return false;
		}
		if (Status->Frame == Frame - 1 && Status->Count > 0 && Count == 0)
		{
			return true;
		}
		Status->Frame = Frame;
		Status->Count = Count;
		return false;
	}
	case EGenScriptCondition::C_BUILDING_ENTERED_BY_PLAYER:
	{
		// evaluateBuildingEntered: the last player to come into it (OpenContain::getPlayerWhoEntered; it stays so).
		const FGenObject* O = GetUnitNamed(Str(P, 1));
		const int32 Player = GetPlayerNamed(Str(P, 0));
		return O && O->GetContain() && Player != INDEX_NONE && O->PlayerWhoEntered == Player;
	}
	case EGenScriptCondition::C_NAMED_ATTACKED_BY_OBJECTTYPE:
	{
		// evaluateNamedAttackedByType: "we don't care if the attacker is alive or dead".
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && O->LastDamageFrame != 0 && O->LastDamageSourceTemplate && ResolveTypes(Str(P, 1)).Contains(O->LastDamageSourceTemplate->Canonical());
	}
	case EGenScriptCondition::C_NAMED_CREATED:
		// evaluateNamedCreated: "This is actually evaluateNamedExists(...)".
		return GetUnitNamed(Str(P, 0)) != nullptr;
	case EGenScriptCondition::C_NAMED_DYING:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		return O && O->IsDead();
	}
	case EGenScriptCondition::C_NAMED_EXITED_AREA:
	{
		// Object::didExit, as NamedEnteredArea does its other half: inside when this was last asked, and not now.
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		if (!O || !Area)
		{
			return false;
		}
		const bool bInside = !O->IsDead() && Area->Contains(O->GetPosition().X, O->GetPosition().Y);
		const bool bWasInside = C.CustomData == 1;
		C.CustomData = bInside ? 1 : -1;
		return bWasInside && !bInside;
	}
	case EGenScriptCondition::C_NAMED_REACHED_WAYPOINTS_END:
	{
		const FGenObject* O = GetUnitNamed(Str(P, 0));
		const FString* Route = O ? CompletedRoutes.Find(O->GetID()) : nullptr;
		return Route && Route->Equals(Str(P, 1), ESearchCase::IgnoreCase);
	}
	case EGenScriptCondition::C_TEAM_REACHED_WAYPOINTS_END:
	{
		// evaluateTeamReachedWaypointsEnd: "true if any of the team completed the path".
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		TArray<FGenObject*> Members;
		if (Team) { GetTeamMembers(*Team, Members); }
		for (const FGenObject* O : Members)
		{
			const FString* Route = CompletedRoutes.Find(O->GetID());
			if (Route && Route->Equals(Str(P, 1), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
	case EGenScriptCondition::C_PLAYER_ALL_BUILDFACILITIES_DESTROYED:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return true; // "Non existent player is all destroyed."
		}
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			if (IsLiving(O) && IsBuildFacility(*O))
			{
				return false;
			}
		}
		return true;
	}
	case EGenScriptCondition::C_PLAYER_BUILT_UPGRADE:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		return Player != INDEX_NONE && Logic->ConsumeScriptEvent(Player, Str(P, 1), false);
	}
	case EGenScriptCondition::C_PLAYER_DESTROYED_N_BUILDINGS_PLAYER:
		return false; // ("@todo CLH implement me!" The original always says no.)
	case EGenScriptCondition::C_PLAYER_HAS_N_OR_FEWER_BUILDINGS:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return false;
		}
		int32 Buildings = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			Buildings += IsLiving(O) && O->IsKindOf(TEXT("STRUCTURE")) ? 1 : 0;
		}
		return Int(P, 1) >= Buildings;
	}
	case EGenScriptCondition::C_PLAYER_HAS_POWER:
	case EGenScriptCondition::C_PLAYER_HAS_NO_POWER:
	{
		// evaluatePlayerHasPower, and its opposite: nobody has no power, in that case.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		bool bPower = false;
		if (Player != INDEX_NONE)
		{
			int32 Produced = 0, Consumed = 0;
			Logic->GetEnergy(Player, Produced, Consumed);
			bPower = Produced >= Consumed;
		}
		return C.Type == EGenScriptCondition::C_PLAYER_HAS_POWER ? bPower : !bPower;
	}
	case EGenScriptCondition::C_PLAYER_POWER_COMPARE_PERCENT:
	{
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE)
		{
			return false;
		}
		int32 Produced = 0, Consumed = 0;
		Logic->GetEnergy(Player, Produced, Consumed);
		// Energy::getEnergySupplyRatio
		const float Ratio = Consumed == 0 ? float(Produced) : float(Produced) / float(Consumed);
		return Compare(Ratio, Int(P, 1), float(Int(P, 2)) / 100.f);
	}
	case EGenScriptCondition::C_PLAYER_LOST_OBJECT_TYPE:
	{
		// evaluatePlayerLostObjectType: fewer than when this was last asked.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		if (Player == INDEX_NONE || ResolveTypes(Str(P, 1)).Num() == 0)
		{
			return false;
		}
		const int32 Count = CountOfTypes(Player, Str(P, 1));
		int32& Known = LostObjectCounts.FindOrAdd(FString::FromInt(Player) + TEXT("|") + Str(P, 1));
		const bool bLost = Count < Known;
		Known = Count;
		return bLost;
	}
	case EGenScriptCondition::C_SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE:
	{
		// evaluateSkirmishPlayerTechBuildingWithinDistancePerimeter: a tech building that isn't the player's or an
		// ally's near the area. "If we have a cached value, return it."
		if (C.CustomData != 0)
		{
			return C.CustomData == 1;
		}
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 2));
		if (Player == INDEX_NONE || !Area || Area->Points.Num() == 0)
		{
			return false;
		}
		FBox2f Bounds(ForceInit);
		for (const FIntVector& Pt : Area->Points) { Bounds += FVector2f(float(Pt.X), float(Pt.Y)); }
		const FVector2f Center = Bounds.GetCenter();
		const float Radius = Bounds.GetExtent().Size() + (P.IsValidIndex(1) ? P[1].Real : 0.f);
		bool bFound = false;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (IsLiving(O) && O->IsKindOf(TEXT("TECH_BUILDING")) && O->GetOwner() != Player
				&& Logic->GetRelationship(Player, O->GetOwner()) != EGenRelationship::Allies
				&& FVector2f::Distance(Center, FVector2f(O->GetPosition().X, O->GetPosition().Y)) <= Radius)
			{
				bFound = true;
				break;
			}
		}
		C.CustomData = bFound ? 1 : -1;
		return bFound;
	}
	case EGenScriptCondition::C_SKIRMISH_UNOWNED_FACTION_UNIT_EXISTS:
	{
		// evaluateSkirmishUnownedFactionUnitComparison: the neutral player's vehicles with nobody in them.
		int32 Unmanned = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(Logic->GetNeutralPlayer()))
		{
			Unmanned += IsLiving(O) && O->IsUnmanned() ? 1 : 0;
		}
		return Compare(Unmanned, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_SKIRMISH_VALUE_IN_AREA:
	{
		// evaluateSkirmishValueInArea: what the player's things inside the area cost, in total.
		const int32 Player = GetPlayerNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 3));
		if (Player == INDEX_NONE || !Area)
		{
			return false;
		}
		int32 Total = 0;
		for (const FGenObject* O : Logic->GetObjectsOf(Player))
		{
			if (IsLiving(O) && !O->IsProjectile() && !O->IsKindOf(TEXT("INERT")) && Area->Contains(O->GetPosition().X, O->GetPosition().Y))
			{
				Total += int32(O->GetTemplate()->BuildCost);
			}
		}
		return Compare(Total, Int(P, 1), Int(P, 2));
	}
	case EGenScriptCondition::C_TEAM_CREATED:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		return Team && Team->bCreated;
	}
	case EGenScriptCondition::C_TEAM_EXITED_AREA_PARTIALLY:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return Team && Area && TeamMemberExited(C, *Team, *Area, SurfacesFromScript(Int(P, 2)));
	}
	case EGenScriptCondition::C_TEAM_INSIDE_AREA_ENTIRELY:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		const FGenPolygonTrigger* Area = GetTrigger(Str(P, 1));
		return Team && Area && TeamInside(*Team, *Area, SurfacesFromScript(Int(P, 2)), true);
	}
	case EGenScriptCondition::C_TEAM_OWNED_BY_PLAYER:
	{
		const FGenMapTeam* Team = GetTeamNamed(Str(P, 0));
		return Team && Team->Player == GetPlayerNamed(Str(P, 1));
	}
	case EGenScriptCondition::C_TEAM_STATE_IS:
		// Team::getState: only TEAM_SET_STATE gives a team a state, and no mission that asks uses it.
		return GetTeamNamed(Str(P, 0)) && Str(P, 1).IsEmpty();
	default:
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] condition %s isn't ported: false"), Frame, *C.TypeName);
		return false;
	}
}
