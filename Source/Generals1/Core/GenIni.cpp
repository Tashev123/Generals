#include "Core/GenIni.h"
#include "Core/GenFileSystem.h"
#include "Generals1.h"

namespace
{
	const FString EmptyString;

	// Fields whose parser reads a nested block (calls INI::initFromINI), in any context.
	const TSet<FString>& BlockFields()
	{
		static const TSet<FString> Set = {
			// ThingTemplate
			TEXT("draw"), TEXT("body"), TEXT("behavior"), TEXT("clientupdate"), TEXT("weaponset"), TEXT("armorset"),
			TEXT("prerequisites"), TEXT("unitspecificsounds"), TEXT("unitspecificfx"), TEXT("addmodule"),
			TEXT("replacemodule"), TEXT("inheritablemodule"), TEXT("overrideablebylikekind"),
			// W3DModelDraw (AliasConditionState does not open a block)
			TEXT("defaultconditionstate"), TEXT("conditionstate"), TEXT("transitionstate"),
			// RadiusDecalTemplate users inside modules / nuggets
			TEXT("attackareadecal"), TEXT("deliverydecal"), TEXT("griddecaltemplate"), TEXT("targetingreticledecal"),
		};
		return Set;
	}

	// Fields that open a block only under a specific parent block.
	const TMap<FString, TSet<FString>>& ContextBlockFields()
	{
		static const TMap<FString, TSet<FString>> Map = [] {
			TMap<FString, TSet<FString>> M;
			M.Add(TEXT("fxlist"), { TEXT("sound"), TEXT("rayeffect"), TEXT("tracer"), TEXT("lightpulse"), TEXT("viewshake"),
				TEXT("terrainscorch"), TEXT("particlesystem"), TEXT("fxlistatbonepos") });
			M.Add(TEXT("objectcreationlist"), { TEXT("createobject"), TEXT("createdebris"), TEXT("applyrandomforce"),
				TEXT("deliverpayload"), TEXT("fireweapon"), TEXT("attack") });
			M.Add(TEXT("aidata"), { TEXT("sideinfo"), TEXT("skirmishbuildlist") });
			M.Add(TEXT("sideinfo"), { TEXT("skillset1"), TEXT("skillset2"), TEXT("skillset3"), TEXT("skillset4"), TEXT("skillset5") });
			M.Add(TEXT("skirmishbuildlist"), { TEXT("structure") });
			M.Add(TEXT("evaevent"), { TEXT("sidesounds") });
			M.Add(TEXT("campaign"), { TEXT("mission") });
			M.Add(TEXT("controlbarscheme"), { TEXT("imagepart"), TEXT("animatingpart") });
			M.Add(TEXT("animatingpart"), { TEXT("imagepart") });
			M.Add(TEXT("shellmenuscheme"), { TEXT("imagepart"), TEXT("linepart") });
			M.Add(TEXT("windowtransition"), { TEXT("window") });
			TSet<FString> Personas;
			for (int32 i = 0; i < 12; ++i) { Personas.Add(FString::Printf(TEXT("generalpersona%d"), i)); }
			M.Add(TEXT("challengegenerals"), Personas);
			return M;
		}();
		return Map;
	}

	bool OpensBlock(const FString& ParentKeyword, const FString& Key, bool bHasValues)
	{
		const FString K = Key.ToLower();
		if (BlockFields().Contains(K))
		{
			return true;
		}
		// AIUpdate's "Turret" block vs. W3DModelDraw's "Turret = Bone" field.
		if (!bHasValues && (K == TEXT("turret") || K == TEXT("altturret") || K.EndsWith(TEXT("radiuscursor"))))
		{
			return true;
		}
		const TSet<FString>* Ctx = ContextBlockFields().Find(ParentKeyword.ToLower());
		return Ctx && Ctx->Contains(K);
	}

	bool IsSingleLineTopLevel(const FString& Key)
	{
		return Key.Equals(TEXT("ReallyLowMHz"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("BenchProfile"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("LODPreset"), ESearchCase::IgnoreCase);
	}

	FORCEINLINE bool IsSep(TCHAR C, const TCHAR* Seps)
	{
		for (const TCHAR* S = Seps; *S; ++S)
		{
			if (*S == C) { return true; }
		}
		return false;
	}
}

// ---- entries / blocks --------------------------------------------------------------------------

const FString& FGenIniEntry::Value(int32 Index) const
{
	return Values.IsValidIndex(Index) ? Values[Index] : EmptyString;
}

float FGenIniEntry::AsFloat(int32 Index, float Default) const
{
	if (!Values.IsValidIndex(Index)) { return Default; }
	FString V = Values[Index];
	V.RemoveFromEnd(TEXT("%"));
	return FCString::Atof(*V);
}

int32 FGenIniEntry::AsInt(int32 Index, int32 Default) const
{
	return Values.IsValidIndex(Index) ? FCString::Atoi(*Values[Index]) : Default;
}

bool FGenIniEntry::AsBool(int32 Index, bool Default) const
{
	if (!Values.IsValidIndex(Index)) { return Default; }
	const FString& V = Values[Index];
	return V.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || V.Equals(TEXT("true"), ESearchCase::IgnoreCase) || V == TEXT("1");
}

const FString& FGenIniBlock::Name() const
{
	return Args.Num() > 0 ? Args[0] : EmptyString;
}

const FGenIniEntry* FGenIniBlock::Find(const TCHAR* Key) const
{
	for (const FGenIniEntry& E : Entries)
	{
		if (E.Key.Equals(Key, ESearchCase::IgnoreCase))
		{
			return &E;
		}
	}
	return nullptr;
}

TArray<const FGenIniEntry*> FGenIniBlock::FindAll(const TCHAR* Key) const
{
	TArray<const FGenIniEntry*> Out;
	for (const FGenIniEntry& E : Entries)
	{
		if (E.Key.Equals(Key, ESearchCase::IgnoreCase))
		{
			Out.Add(&E);
		}
	}
	return Out;
}

FString FGenIniBlock::GetString(const TCHAR* Key, const FString& Default) const
{
	const FGenIniEntry* E = Find(Key);
	return E && E->Values.Num() > 0 ? E->Values[0] : Default;
}

float FGenIniBlock::GetFloat(const TCHAR* Key, float Default) const
{
	const FGenIniEntry* E = Find(Key);
	return E ? E->AsFloat(0, Default) : Default;
}

int32 FGenIniBlock::GetInt(const TCHAR* Key, int32 Default) const
{
	const FGenIniEntry* E = Find(Key);
	return E ? E->AsInt(0, Default) : Default;
}

bool FGenIniBlock::GetBool(const TCHAR* Key, bool Default) const
{
	const FGenIniEntry* E = Find(Key);
	return E ? E->AsBool(0, Default) : Default;
}

// ---- parsing ----------------------------------------------------------------------------------

void GenIni::Tokenize(const FString& Text, TArray<FString>& OutTokens, const TCHAR* Separators)
{
	OutTokens.Reset();
	const int32 Len = Text.Len();
	int32 i = 0;
	while (i < Len)
	{
		while (i < Len && IsSep(Text[i], Separators)) { ++i; }
		const int32 Start = i;
		while (i < Len && !IsSep(Text[i], Separators)) { ++i; }
		if (i > Start)
		{
			OutTokens.Add(Text.Mid(Start, i - Start));
		}
	}
}

bool GenIni::Parse(const FString& Text, const FString& FileName, TArray<TSharedPtr<FGenIniBlock>>& OutBlocks)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);

	TArray<FGenIniBlock*> Stack;
	bool bOk = true;

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		FString Line = Lines[LineIdx];
		int32 Semi;
		if (Line.FindChar(TEXT(';'), Semi))
		{
			Line.LeftInline(Semi);
		}
		for (TCHAR& C : Line)
		{
			if (C > 0 && C < 32) { C = TEXT(' '); }
		}

		// key = first token; rest = text after key and optional '='
		int32 i = 0;
		const int32 Len = Line.Len();
		while (i < Len && IsSep(Line[i], TEXT(" \t="))) { ++i; }
		const int32 KeyStart = i;
		while (i < Len && !IsSep(Line[i], TEXT(" \t="))) { ++i; }
		if (i == KeyStart)
		{
			continue;
		}
		const FString Key = Line.Mid(KeyStart, i - KeyStart);
		while (i < Len && (Line[i] == TEXT(' ') || Line[i] == TEXT('\t'))) { ++i; }
		if (i < Len && Line[i] == TEXT('=')) { ++i; }
		FString Rest = Line.Mid(i).TrimStartAndEnd();

		if (Key.Equals(TEXT("End"), ESearchCase::IgnoreCase))
		{
			if (Stack.Num() == 0)
			{
				UE_LOG(LogGenerals, Warning, TEXT("%s:%d: unmatched End"), *FileName, LineIdx + 1);
				bOk = false;
				continue;
			}
			Stack.Pop();
			continue;
		}

		TArray<FString> Values;
		Tokenize(Rest, Values);

		if (Stack.Num() == 0)
		{
			TSharedPtr<FGenIniBlock> Block = MakeShared<FGenIniBlock>();
			Block->Keyword = Key;
			Block->Args = MoveTemp(Values);
			Block->File = FileName;
			Block->Line = LineIdx + 1;
			OutBlocks.Add(Block);
			if (!IsSingleLineTopLevel(Key))
			{
				Stack.Push(Block.Get());
			}
			continue;
		}

		FGenIniBlock* Parent = Stack.Last();
		FGenIniEntry& Entry = Parent->Entries.AddDefaulted_GetRef();
		Entry.Key = Key;
		Entry.Values = MoveTemp(Values);
		Entry.Raw = MoveTemp(Rest);
		Entry.Line = LineIdx + 1;
		if (OpensBlock(Parent->Keyword, Key, Entry.Values.Num() > 0))
		{
			Entry.Block = MakeShared<FGenIniBlock>();
			Entry.Block->Keyword = Key;
			Entry.Block->Args = Entry.Values;
			Entry.Block->File = FileName;
			Entry.Block->Line = LineIdx + 1;
			Stack.Push(Entry.Block.Get());
		}
	}

	if (Stack.Num() > 0)
	{
		UE_LOG(LogGenerals, Warning, TEXT("%s: unterminated block '%s' from line %d"), *FileName, *Stack.Last()->Keyword, Stack.Last()->Line);
		bOk = false;
	}
	return bOk;
}

bool GenIni::ParseFile(const FString& VfsPath, TArray<TSharedPtr<FGenIniBlock>>& OutBlocks)
{
	TArray<uint8> Bytes;
	if (!FGenFileSystem::Get().ReadFile(VfsPath, Bytes))
	{
		return false;
	}
	// INI files are 8-bit (Latin-1); widen byte by byte.
	FString Text;
	Text.Reserve(Bytes.Num());
	for (uint8 B : Bytes)
	{
		Text.AppendChar(TCHAR(B));
	}
	return Parse(Text, VfsPath, OutBlocks);
}
