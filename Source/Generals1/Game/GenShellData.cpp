#include "Game/GenShellData.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Generals1.h"

namespace
{
	/** "X:1362.71 Y:1966.35 Z:0.00" */
	FVector2f ParseCoord(const FString& Raw)
	{
		FVector2f Out = FVector2f::ZeroVector;
		TArray<FString> Tokens;
		Raw.ParseIntoArrayWS(Tokens);
		for (const FString& T : Tokens)
		{
			if (T.StartsWith(TEXT("X:"), ESearchCase::IgnoreCase)) { Out.X = FCString::Atof(*T.Mid(2)); }
			else if (T.StartsWith(TEXT("Y:"), ESearchCase::IgnoreCase)) { Out.Y = FCString::Atof(*T.Mid(2)); }
		}
		return Out;
	}

	/** "R:221 G:226 B:13" */
	FLinearColor ParseColor(const FString& Raw)
	{
		int32 R = 255, G = 255, B = 255;
		TArray<FString> Tokens;
		Raw.ParseIntoArrayWS(Tokens);
		for (const FString& T : Tokens)
		{
			if (T.StartsWith(TEXT("R:"), ESearchCase::IgnoreCase)) { R = FCString::Atoi(*T.Mid(2)); }
			else if (T.StartsWith(TEXT("G:"), ESearchCase::IgnoreCase)) { G = FCString::Atoi(*T.Mid(2)); }
			else if (T.StartsWith(TEXT("B:"), ESearchCase::IgnoreCase)) { B = FCString::Atoi(*T.Mid(2)); }
		}
		return FLinearColor(FColor(uint8(FMath::Clamp(R, 0, 255)), uint8(FMath::Clamp(G, 0, 255)), uint8(FMath::Clamp(B, 0, 255))));
	}

	/** A text file of the game (8-bit), line by line; comments (';') cut off. */
	bool ReadLines(const TCHAR* Path, TArray<FString>& OutLines)
	{
		TArray<uint8> Bytes;
		if (!FGenFileSystem::Get().ReadFile(Path, Bytes))
		{
			return false;
		}
		FString Text;
		Text.Reserve(Bytes.Num());
		for (uint8 B : Bytes) { Text.AppendChar(TCHAR(B)); }
		Text.ParseIntoArrayLines(OutLines);
		for (FString& Line : OutLines)
		{
			int32 Comment = INDEX_NONE;
			if (Line.FindChar(TEXT(';'), Comment)) { Line.LeftInline(Comment); }
			Line.TrimStartAndEndInline();
		}
		return true;
	}

	/** The map cache writes file names with everything but letters and digits as _XX (hex). */
	FString DecodeCacheName(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			if (In[i] == TEXT('_') && i + 2 < In.Len() && FChar::IsHexDigit(In[i + 1]) && FChar::IsHexDigit(In[i + 2]))
			{
				Out.AppendChar(TCHAR(FParse::HexDigit(In[i + 1]) * 16 + FParse::HexDigit(In[i + 2])));
				i += 2;
			}
			else
			{
				Out.AppendChar(In[i]);
			}
		}
		return Out;
	}
}

FGenShellData& FGenShellData::Get()
{
	static FGenShellData Instance;
	return Instance;
}

FString FGenShellData::Text(const TCHAR* Label, const TCHAR* Fallback)
{
	const FString Found = FGenCommandData::Get().GetText(Label);
	return (Found.IsEmpty() || Found.Equals(Label, ESearchCase::IgnoreCase)) ? FString(Fallback) : Found;
}

void FGenShellData::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;
	FGenGameData::Get().Initialize();
	FGenCommandData::Get().Initialize();
	LoadMapCache();
	LoadMultiplayerSettings();
	LoadSides();
	UE_LOG(LogGenerals, Log, TEXT("Shell data: %d multiplayer maps, %d colours, %d cash choices, %d sides"), Maps.Num(), Colors.Num(), MoneyChoices.Num(), Sides.Num());
}

const FGenMapInfo* FGenShellData::FindMap(const FString& Name) const
{
	return Maps.FindByPredicate([&Name](const FGenMapInfo& M) { return M.Name.Equals(Name, ESearchCase::IgnoreCase); });
}

int32 FGenShellData::FindSide(const FString& Template) const
{
	return Sides.IndexOfByPredicate([&Template](const FGenSideInfo& S) { return S.Template.Equals(Template, ESearchCase::IgnoreCase); });
}

void FGenShellData::LoadMapCache()
{
	// MapUtil.cpp: "MapCache <file name>" ... "END", one per map the game knows.
	TArray<FString> Lines;
	if (!ReadLines(TEXT("Maps\\MapCache.ini"), Lines))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Maps\\MapCache.ini not found"));
		return;
	}

	FGenMapInfo Map;
	bool bIn = false, bMultiplayer = false;
	FString LookupTag;
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("MapCache "), ESearchCase::IgnoreCase))
		{
			Map = FGenMapInfo();
			bIn = true;
			bMultiplayer = false;
			LookupTag.Reset();
			// "maps\tournament desert\tournament desert.map": the folder is the map's name.
			const FString Path = DecodeCacheName(Line.Mid(9).TrimStartAndEnd());
			TArray<FString> Parts;
			Path.ParseIntoArray(Parts, TEXT("\\"));
			Map.Name = Parts.Num() >= 2 ? Parts[Parts.Num() - 2] : FPaths::GetBaseFilename(Path);
			continue;
		}
		if (!bIn)
		{
			continue;
		}
		if (Line.Equals(TEXT("END"), ESearchCase::IgnoreCase))
		{
			bIn = false;
			const FString MapFile = FString::Printf(TEXT("Maps\\%s\\%s.map"), *Map.Name, *Map.Name);
			if (bMultiplayer && Map.NumPlayers >= 2 && FGenFileSystem::Get().Exists(MapFile))
			{
				Map.DisplayName = FGenCommandData::Get().GetText(LookupTag);
				if (Map.DisplayName.IsEmpty() || Map.DisplayName.Equals(LookupTag, ESearchCase::IgnoreCase))
				{
					Map.DisplayName = Map.Name;
				}
				Maps.Add(MoveTemp(Map));
			}
			continue;
		}
		FString Key, Value;
		if (!Line.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		Key.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();
		if (Key.Equals(TEXT("isMultiplayer"), ESearchCase::IgnoreCase)) { bMultiplayer = Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase); }
		else if (Key.Equals(TEXT("numPlayers"), ESearchCase::IgnoreCase)) { Map.NumPlayers = FCString::Atoi(*Value); }
		else if (Key.Equals(TEXT("extentMin"), ESearchCase::IgnoreCase)) { Map.ExtentMin = ParseCoord(Value); }
		else if (Key.Equals(TEXT("extentMax"), ESearchCase::IgnoreCase)) { Map.ExtentMax = ParseCoord(Value); }
		else if (Key.Equals(TEXT("nameLookupTag"), ESearchCase::IgnoreCase)) { LookupTag = Value; }
		else if (Key.Equals(TEXT("techPosition"), ESearchCase::IgnoreCase)) { Map.TechPositions.Add(ParseCoord(Value)); }
		else if (Key.Equals(TEXT("supplyPosition"), ESearchCase::IgnoreCase)) { Map.SupplyPositions.Add(ParseCoord(Value)); }
		else if (Key.StartsWith(TEXT("Player_"), ESearchCase::IgnoreCase) && Key.EndsWith(TEXT("_Start"), ESearchCase::IgnoreCase))
		{
			const int32 N = FCString::Atoi(*Key.Mid(7));
			if (N >= 1 && N <= 8)
			{
				if (Map.Starts.Num() < N) { Map.Starts.SetNumZeroed(N); }
				Map.Starts[N - 1] = ParseCoord(Value);
			}
		}
	}
	Maps.Sort([](const FGenMapInfo& A, const FGenMapInfo& B) { return A.DisplayName.Compare(B.DisplayName, ESearchCase::IgnoreCase) < 0; });
}

void FGenShellData::LoadMultiplayerSettings()
{
	// MultiplayerSettings.cpp: the colours a player may wear and the starting cash to choose from.
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Multiplayer.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (B->Keyword.Equals(TEXT("MultiplayerColor"), ESearchCase::IgnoreCase))
		{
			FGenPlayerColorDef& C = Colors.AddDefaulted_GetRef();
			if (const FGenIniEntry* E = B->Find(TEXT("RGBColor"))) { C.Color = ParseColor(E->Raw); }
			const FString Tip = B->GetString(TEXT("TooltipName"));
			C.Label = FGenCommandData::Get().GetText(Tip);
			if (C.Label.IsEmpty() || C.Label.Equals(Tip, ESearchCase::IgnoreCase))
			{
				C.Label = B->Name().Replace(TEXT("Color"), TEXT(""));
			}
		}
		else if (B->Keyword.Equals(TEXT("MultiplayerStartingMoneyChoice"), ESearchCase::IgnoreCase))
		{
			const int32 Value = B->GetInt(TEXT("Value"));
			if (Value > 0)
			{
				MoneyChoices.Add(Value);
				if (B->GetBool(TEXT("Default"))) { DefaultMoney = Value; }
			}
		}
	}
	if (MoneyChoices.Num() == 0)
	{
		MoneyChoices.Add(DefaultMoney);
	}
}

void FGenShellData::LoadSides()
{
	// GUIUtil.cpp PopulatePlayerTemplateComboBox: every template with a starting building, one per Side,
	// without the generals that start locked (ChallengeMode.ini: the boss).
	TSet<FString> Locked;
	TArray<FString> Lines;
	ReadLines(TEXT("Data\\INI\\ChallengeMode.ini"), Lines);
	FString PersonaTemplate;
	for (const FString& Line : Lines)
	{
		FString Key, Value;
		if (Line.StartsWith(TEXT("GeneralPersona"), ESearchCase::IgnoreCase))
		{
			PersonaTemplate.Reset();
		}
		else if (Line.Split(TEXT("="), &Key, &Value))
		{
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Key.Equals(TEXT("PlayerTemplate"), ESearchCase::IgnoreCase))
			{
				PersonaTemplate = Value;
			}
			else if (Key.Equals(TEXT("StartsEnabled"), ESearchCase::IgnoreCase) && !Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase) && !PersonaTemplate.IsEmpty())
			{
				Locked.Add(PersonaTemplate.ToLower());
			}
		}
	}
	TSet<FString> SeenSides;
	for (const FGenPlayerTemplate& T : FGenGameData::Get().GetPlayerTemplates())
	{
		if (T.StartingBuilding.IsEmpty() || T.StartingBuilding.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			|| Locked.Contains(T.Name.ToLower()) || SeenSides.Contains(T.Side.ToLower()))
		{
			continue;
		}
		SeenSides.Add(T.Side.ToLower());
		FGenSideInfo& S = Sides.AddDefaulted_GetRef();
		S.Template = T.Name;
		const FString Label = TEXT("SIDE:") + T.Side;
		S.Label = FGenCommandData::Get().GetText(Label);
		if (S.Label.IsEmpty() || S.Label.Equals(Label, ESearchCase::IgnoreCase)) { S.Label = T.Side; }
		S.Icon = T.SideIcon;
		S.ScoreImage = T.ScoreScreenImage;
		S.ScoreMusic = T.ScoreScreenMusic;
	}
}
