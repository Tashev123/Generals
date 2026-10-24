// Data of the skirmish computer player: Data\Scripts\SkirmishScripts.scb (scripts and team definitions,
// Scripts.cpp / SidesList.cpp) and AIData.ini (settings and per-side base layouts, AI.cpp).

#include "Logic/GenSkirmishAI.h"
#include "Core/GenCompression.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Map/GenDataChunk.h"
#include "Generals1.h"

namespace
{
	constexpr int32 ParamCoord3D = 16;   // Parameter::COORD3D: stored as three reals

	EGenScriptCondition ConditionFromName(const FString& Name)
	{
		static const TMap<FString, EGenScriptCondition> Map = [] {
			TMap<FString, EGenScriptCondition> M;
#define GEN_X(N) M.Add(TEXT(#N), EGenScriptCondition::C_##N);
			GEN_SCRIPT_CONDITIONS(GEN_X)
#undef GEN_X
			return M;
		}();
		const EGenScriptCondition* Found = Map.Find(Name);
		return Found ? *Found : EGenScriptCondition::Unknown;
	}

	EGenScriptAction ActionFromName(const FString& Name)
	{
		static const TMap<FString, EGenScriptAction> Map = [] {
			TMap<FString, EGenScriptAction> M;
#define GEN_X(N) M.Add(TEXT(#N), EGenScriptAction::A_##N);
			GEN_SCRIPT_ACTIONS(GEN_X)
#undef GEN_X
			return M;
		}();
		const EGenScriptAction* Found = Map.Find(Name);
		return Found ? *Found : EGenScriptAction::Unknown;
	}

	void ReadParams(FGenChunkReader& R, TArray<FGenScriptParam>& Out)
	{
		const int32 Num = R.I32();
		for (int32 i = 0; i < Num && i < 64; ++i)
		{
			FGenScriptParam& P = Out.AddDefaulted_GetRef();
			const int32 Type = R.I32();
			if (Type == ParamCoord3D)
			{
				R.Vec3();
			}
			else
			{
				P.Int = R.I32();
				P.Real = R.F32();
				P.String = R.AStr();
			}
		}
	}

	void ReadScript(FGenChunkReader& R, const FGenChunkReader::FChunk& C, FGenScript& S)
	{
		S.Name = R.AStr();
		R.AStr(); R.AStr(); R.AStr(); // comment, condition comment, action comment
		S.bActive = R.U8() != 0;
		S.bOneShot = R.U8() != 0;
		S.bEasy = R.U8() != 0;
		S.bNormal = R.U8() != 0;
		S.bHard = R.U8() != 0;
		S.bSubroutine = R.U8() != 0;
		S.DelaySeconds = C.Version >= 2 ? R.I32() : 0;

		FGenChunkReader::FChunk Sub;
		while (R.NextChunk(C.End, Sub))
		{
			if (Sub.Name == TEXT("OrCondition"))
			{
				TArray<FGenScriptCondition>& And = S.Conditions.AddDefaulted_GetRef();
				FGenChunkReader::FChunk CC;
				while (R.NextChunk(Sub.End, CC))
				{
					if (CC.Name == TEXT("Condition"))
					{
						FGenScriptCondition& Cond = And.AddDefaulted_GetRef();
						R.I32(); // numeric type: the name key is what identifies it
						Cond.TypeName = CC.Version >= 4 ? R.NameKey() : FString();
						Cond.Type = ConditionFromName(Cond.TypeName);
						ReadParams(R, Cond.Params);
					}
					R.Pos = CC.End;
				}
			}
			else if (Sub.Name == TEXT("ScriptAction") || Sub.Name == TEXT("ScriptActionFalse"))
			{
				FGenScriptAction& A = (Sub.Name == TEXT("ScriptAction") ? S.Actions : S.FalseActions).AddDefaulted_GetRef();
				R.I32();
				A.TypeName = Sub.Version >= 2 ? R.NameKey() : FString();
				A.Type = ActionFromName(A.TypeName);
				ReadParams(R, A.Params);
			}
			R.Pos = Sub.End;
		}
	}

	FString DictString(const FGenDict& D, const TCHAR* Key)
	{
		const FGenDictValue* V = D.Find(Key);
		return V ? V->String : FString();
	}

	int32 DictInt(const FGenDict& D, const TCHAR* Key, int32 Default = 0)
	{
		const FGenDictValue* V = D.Find(Key);
		return V ? V->Int : Default;
	}

	bool DictBool(const FGenDict& D, const TCHAR* Key)
	{
		const FGenDictValue* V = D.Find(Key);
		return V && V->Bool;
	}

	/** "X:501.22 Y:546.25" */
	FVector2f ParseCoord2D(const FString& Raw)
	{
		FVector2f Out = FVector2f::ZeroVector;
		TArray<FString> Tok;
		GenIni::Tokenize(Raw, Tok, TEXT(" \t=:"));
		for (int32 i = 0; i + 1 < Tok.Num(); i += 2)
		{
			if (Tok[i].Equals(TEXT("X"), ESearchCase::IgnoreCase)) { Out.X = FCString::Atof(*Tok[i + 1]); }
			else if (Tok[i].Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { Out.Y = FCString::Atof(*Tok[i + 1]); }
		}
		return Out;
	}
}

// ---- scripts ------------------------------------------------------------------------------------

FGenScriptLibrary& FGenScriptLibrary::Get()
{
	static FGenScriptLibrary Instance;
	return Instance;
}

bool FGenScriptLibrary::Initialize()
{
	if (bInitialized)
	{
		return bLoaded;
	}
	bInitialized = true;

	TArray<uint8> Raw, Data;
	if (!FGenFileSystem::Get().ReadFile(TEXT("Data\\Scripts\\SkirmishScripts.scb"), Raw))
	{
		UE_LOG(LogGenerals, Warning, TEXT("SkirmishScripts.scb not found: computer players will do nothing"));
		return false;
	}
	TMap<uint32, FString> Names;
	int32 Start = GenCompression::Decompress(Raw, Data) ? FGenChunkReader::ReadNameTable(Data, Names) : 0;
	if (Start == 0)
	{
		Data = Raw;
		Start = FGenChunkReader::ReadNameTable(Data, Names);
	}
	if (Start == 0)
	{
		UE_LOG(LogGenerals, Warning, TEXT("SkirmishScripts.scb: not a CkMp file"));
		return false;
	}

	FGenChunkReader R(Data, Names);
	R.Pos = Start;
	TArray<FGenScriptList> ReadLists;
	TArray<FString> PlayerNames;
	int32 NumScripts = 0;
	FGenChunkReader::FChunk C;
	while (R.NextChunk(Data.Num(), C))
	{
		if (C.Name == TEXT("PlayerScriptsList"))
		{
			FGenChunkReader::FChunk LC;
			while (R.NextChunk(C.End, LC))
			{
				if (LC.Name == TEXT("ScriptList"))
				{
					FGenScriptList& List = ReadLists.AddDefaulted_GetRef();
					FGenChunkReader::FChunk SC;
					while (R.NextChunk(LC.End, SC))
					{
						if (SC.Name == TEXT("Script"))
						{
							ReadScript(R, SC, List.Scripts.AddDefaulted_GetRef());
							++NumScripts;
						}
						else if (SC.Name == TEXT("ScriptGroup"))
						{
							FGenScriptGroup& G = List.Groups.AddDefaulted_GetRef();
							G.Name = R.AStr();
							G.bActive = R.U8() != 0;
							G.bSubroutine = SC.Version >= 2 ? R.U8() != 0 : false;
							FGenChunkReader::FChunk GC;
							while (R.NextChunk(SC.End, GC))
							{
								if (GC.Name == TEXT("Script"))
								{
									ReadScript(R, GC, G.Scripts.AddDefaulted_GetRef());
									++NumScripts;
								}
								R.Pos = GC.End;
							}
						}
						R.Pos = SC.End;
					}
				}
				R.Pos = LC.End;
			}
		}
		else if (C.Name == TEXT("ScriptsPlayers"))
		{
			const bool bReadDicts = C.Version >= 2 && R.I32() != 0;
			const int32 Num = R.I32();
			for (int32 i = 0; i < Num && R.Pos < C.End; ++i)
			{
				PlayerNames.Add(R.AStr());
				if (bReadDicts) { R.Dict(); }
			}
		}
		else if (C.Name == TEXT("ScriptTeams"))
		{
			while (R.Pos < C.End)
			{
				const FGenDict D = R.Dict();
				FGenTeamProto& T = Teams.AddDefaulted_GetRef();
				T.Name = DictString(D, TEXT("teamName"));
				T.Owner = DictString(D, TEXT("teamOwner"));
				T.bSingleton = DictBool(D, TEXT("teamIsSingleton"));
				for (int32 u = 1; u <= 7; ++u)
				{
					const FString Unit = DictString(D, *FString::Printf(TEXT("teamUnitType%d"), u));
					const int32 Max = DictInt(D, *FString::Printf(TEXT("teamUnitMaxCount%d"), u));
					if (Max > 0 && !Unit.IsEmpty())
					{
						T.Units.Add({ Unit, DictInt(D, *FString::Printf(TEXT("teamUnitMinCount%d"), u)), Max });
					}
				}
				T.ProductionPriority = DictInt(D, TEXT("teamProductionPriority"));
				T.SuccessIncrease = DictInt(D, TEXT("teamProductionPrioritySuccessIncrease"));
				T.FailureDecrease = DictInt(D, TEXT("teamProductionPriorityFailureDecrease"));
				T.MaxInstances = DictInt(D, TEXT("teamMaxInstances"));
				T.InitialIdleFrames = DictInt(D, TEXT("teamInitialIdleFrames"));
				T.bExecuteActions = DictBool(D, TEXT("teamExecutesActionsOnCreate"));
				T.bAutoReinforce = DictBool(D, TEXT("teamAutoReinforce"));
				T.bRecruitable = DictBool(D, TEXT("teamIsAIRecruitable"));
				if (const FGenDictValue* V = D.Find(TEXT("teamDestroyedThreshold"))) { T.DestroyedThreshold = V->Real; }
				T.ProductionCondition = DictString(D, TEXT("teamProductionCondition"));
				T.OnCreate = DictString(D, TEXT("teamOnCreateScript"));
				T.OnDestroyed = DictString(D, TEXT("teamOnDestroyedScript"));
				T.OnIdle = DictString(D, TEXT("teamOnIdleScript"));
				T.OnEnemySighted = DictString(D, TEXT("teamEnemySightedScript"));
				T.OnAllClear = DictString(D, TEXT("teamAllClearScript"));
				T.OnUnitDestroyed = DictString(D, TEXT("teamOnUnitDestroyedScript"));
				for (int32 g = 0; g < FGenTeamProto::MaxGenericScripts; ++g)
				{
					T.Generic[g] = DictString(D, *FString::Printf(TEXT("teamGenericScriptHook%d"), g));
				}
			}
		}
		R.Pos = C.End;
	}

	// The n-th script list belongs to the n-th player name.
	for (int32 i = 0; i < ReadLists.Num() && i < PlayerNames.Num(); ++i)
	{
		if (!PlayerNames[i].IsEmpty())
		{
			Lists.Add(PlayerNames[i], MoveTemp(ReadLists[i]));
		}
	}
	bLoaded = Lists.Num() > 0;
	UE_LOG(LogGenerals, Log, TEXT("Skirmish scripts: %d scripts for %d players, %d team definitions"), NumScripts, Lists.Num(), Teams.Num());
	return bLoaded;
}

void FGenScriptLibrary::GetTeams(const FString& Owner, TArray<FGenTeamProto>& Out) const
{
	for (const FGenTeamProto& T : Teams)
	{
		if (T.Owner.Equals(Owner, ESearchCase::IgnoreCase))
		{
			Out.Add(T);
		}
	}
}

// ---- AIData.ini ---------------------------------------------------------------------------------

FGenAIData& FGenAIData::Get()
{
	static FGenAIData Instance;
	return Instance;
}

void FGenAIData::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Default\\AIData.ini"), Blocks);
	GenIni::ParseFile(TEXT("Data\\INI\\AIData.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("AIData"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		StructureSeconds = B->GetFloat(TEXT("StructureSeconds"), StructureSeconds);
		TeamSeconds = B->GetFloat(TEXT("TeamSeconds"), TeamSeconds);
		Wealthy = B->GetInt(TEXT("Wealthy"), Wealthy);
		Poor = B->GetInt(TEXT("Poor"), Poor);
		StructuresWealthyRate = B->GetFloat(TEXT("StructuresWealthyRate"), StructuresWealthyRate);
		StructuresPoorRate = B->GetFloat(TEXT("StructuresPoorRate"), StructuresPoorRate);
		TeamsWealthyRate = B->GetFloat(TEXT("TeamsWealthyRate"), TeamsWealthyRate);
		TeamsPoorRate = B->GetFloat(TEXT("TeamsPoorRate"), TeamsPoorRate);
		TeamResourcesToStart = B->GetFloat(TEXT("TeamResourcesToStart"), TeamResourcesToStart);
		GuardInnerModifierAI = B->GetFloat(TEXT("GuardInnerModifierAI"), GuardInnerModifierAI);
		GuardOuterModifierAI = B->GetFloat(TEXT("GuardOuterModifierAI"), GuardOuterModifierAI);
		GuardInnerModifierHuman = B->GetFloat(TEXT("GuardInnerModifierHuman"), GuardInnerModifierHuman);
		GuardOuterModifierHuman = B->GetFloat(TEXT("GuardOuterModifierHuman"), GuardOuterModifierHuman);
		MaxRecruitRadius = B->GetFloat(TEXT("MaxRecruitRadius"), MaxRecruitRadius);
		SkirmishBaseDefenseExtraDistance = B->GetFloat(TEXT("SkirmishBaseDefenseExtraDistance"), SkirmishBaseDefenseExtraDistance);
		SupplyCenterSafeRadius = B->GetFloat(TEXT("SupplyCenterSafeRadius"), SupplyCenterSafeRadius);
		MinInfantryForGroup = B->GetInt(TEXT("MinInfantryForGroup"), MinInfantryForGroup);
		MinVehiclesForGroup = B->GetInt(TEXT("MinVehiclesForGroup"), MinVehiclesForGroup);
		MinDistanceForGroup = B->GetFloat(TEXT("MinDistanceForGroup"), MinDistanceForGroup);
		DistanceRequiresGroup = B->GetFloat(TEXT("DistanceRequiresGroup"), DistanceRequiresGroup);
		RebuildDelaySeconds = B->GetInt(TEXT("RebuildDelayTimeSeconds"), RebuildDelaySeconds);
		bRotateSkirmishBases = B->GetBool(TEXT("RotateSkirmishBases"), bRotateSkirmishBases);

		for (const FGenIniEntry& E : B->Entries)
		{
			if (!E.Block)
			{
				continue;
			}
			if (E.Key.Equals(TEXT("SideInfo"), ESearchCase::IgnoreCase))
			{
				FGenAISideInfo& Info = Sides.FindOrAdd(E.Value(0));
				Info.Gatherers[0] = E.Block->GetInt(TEXT("ResourceGatherersEasy"), 2);
				Info.Gatherers[1] = E.Block->GetInt(TEXT("ResourceGatherersNormal"), 2);
				Info.Gatherers[2] = E.Block->GetInt(TEXT("ResourceGatherersHard"), 2);
				Info.BaseDefenseStructure = E.Block->GetString(TEXT("BaseDefenseStructure1"));
				for (const FGenIniEntry& SE : E.Block->Entries)
				{
					const int32 SetIndex = (SE.Block && SE.Key.StartsWith(TEXT("SkillSet"), ESearchCase::IgnoreCase)) ? FCString::Atoi(*SE.Key.Mid(8)) - 1 : INDEX_NONE;
					if (SetIndex < 0 || SetIndex >= 5)
					{
						continue;
					}
					Info.SkillSets[SetIndex].Reset();
					for (const FGenIniEntry& Science : SE.Block->Entries)
					{
						if (Science.Key.Equals(TEXT("Science"), ESearchCase::IgnoreCase))
						{
							Info.SkillSets[SetIndex].Add(Science.Value(0));
						}
					}
				}
			}
			else if (E.Key.Equals(TEXT("SkirmishBuildList"), ESearchCase::IgnoreCase))
			{
				TArray<FGenAIBuildListEntry>& List = BuildLists.FindOrAdd(E.Value(0));
				List.Reset();
				for (const FGenIniEntry& SE : E.Block->Entries)
				{
					if (!SE.Block || !SE.Key.Equals(TEXT("Structure"), ESearchCase::IgnoreCase))
					{
						continue;
					}
					FGenAIBuildListEntry& S = List.AddDefaulted_GetRef();
					S.Template = SE.Value(0);
					if (const FGenIniEntry* Loc = SE.Block->Find(TEXT("Location"))) { S.Location = ParseCoord2D(Loc->Raw); }
					S.Angle = FMath::DegreesToRadians(SE.Block->GetFloat(TEXT("Angle"), 0.f));
					S.Rebuilds = SE.Block->GetInt(TEXT("Rebuilds"), 0);
					S.bInitiallyBuilt = SE.Block->GetBool(TEXT("InitiallyBuilt"), false);
					S.bAutomaticallyBuild = SE.Block->GetBool(TEXT("AutomaticallyBuild"), true);
				}
			}
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("AIData: build lists for %d sides"), BuildLists.Num());
}
