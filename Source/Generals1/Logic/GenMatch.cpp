#include "Logic/GenMatch.h"
#include "Core/GenFileSystem.h"
#include "Logic/GenLogicTypes.h"
#include "Generals1.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	/** The console variables a match carries, by name, with the member that holds each. */
	struct FSimVariable
	{
		const TCHAR* Name;
		int32 FGenMatchSettings::* Member;
	};
	const FSimVariable SimVariables[] = {
		{ TEXT("gen.PathCellsPerStep"), &FGenMatchSettings::PathCellsPerStep },
		{ TEXT("gen.PathCliffMargin"), &FGenMatchSettings::PathCliffMargin },
		{ TEXT("gen.PathWeight"), &FGenMatchSettings::PathWeight },
		{ TEXT("gen.MissileFlight"), &FGenMatchSettings::MissileFlight },
		{ TEXT("gen.Steering"), &FGenMatchSettings::Steering },
	};

	void SerializeBool(FArchive& Ar, bool& B)
	{
		uint8 Raw = B ? 1 : 0;
		Ar << Raw;
		B = Raw != 0;
	}
}

// ---- slots and settings ---------------------------------------------------------------------------

void FGenSkirmishSlot::Serialize(FArchive& Ar)
{
	Ar << Faction;
	SerializeBool(Ar, bHuman);
	Ar << StartPosition << Team;
	Ar << Color;
	Ar << Name;
	Ar << Difficulty;
}

void FGenMatchSettings::Serialize(FArchive& Ar)
{
	Ar << Map << StartingCash << Seed;
	int32 NumSlots = Slots.Num();
	Ar << NumSlots;
	if (Ar.IsLoading())
	{
		Slots.SetNum(FMath::Clamp(NumSlots, 0, 16));
	}
	for (FGenSkirmishSlot& S : Slots)
	{
		S.Serialize(Ar);
	}
	SerializeBool(Ar, bNoAI);
	Ar << DefaultDifficulty << RevealedPlayer;
	SerializeBool(Ar, bPowersReady);
	Ar << LogicFPS << PathCellsPerStep << PathCliffMargin << PathWeight << MissileFlight << Steering;
}

FGenMatchSettings FGenMatchSettings::FromCommandLine()
{
	FGenMatchSettings S;
	const TCHAR* Cmd = FCommandLine::Get();
	// The simulation's random numbers: -GenSeed= makes a run repeatable.
	S.Seed = int32(FDateTime::Now().GetTicks() & 0x7fffffff);
	FParse::Value(Cmd, TEXT("GenSeed="), S.Seed);
	S.bNoAI = FParse::Param(Cmd, TEXT("GenNoAI"));
	FString DifficultyName;
	if (FParse::Value(Cmd, TEXT("GenDifficulty="), DifficultyName))
	{
		S.DefaultDifficulty = DifficultyName.Equals(TEXT("Easy"), ESearchCase::IgnoreCase) ? 0
			: (DifficultyName.Equals(TEXT("Hard"), ESearchCase::IgnoreCase) ? 2 : 1);
	}
	// (-GenNoShroud: whoever plays on this computer; StartSkirmish puts in who that is.)
	S.RevealedPlayer = FParse::Param(Cmd, TEXT("GenNoShroud")) ? -2 : INDEX_NONE;
	S.bPowersReady = FParse::Param(Cmd, TEXT("GenPowersReady"));
	S.LogicFPS = GenLogic::GetLogicFPS();
	for (const FSimVariable& V : SimVariables)
	{
		if (const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(V.Name))
		{
			S.*V.Member = Var->GetInt();
		}
	}
	return S;
}

bool FGenMatchSettings::ApplyToSimulation(FString* OutWhy) const
{
	if (LogicFPS != GenLogic::GetLogicFPS())
	{
		if (OutWhy)
		{
			*OutWhy = FString::Printf(TEXT("the game runs its logic at %d frames a second, this match at %d (start it with -dpcvars=gen.LogicFPS=%d)"),
				GenLogic::GetLogicFPS(), LogicFPS, LogicFPS);
		}
		return false;
	}
	for (const FSimVariable& V : SimVariables)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(V.Name))
		{
			Var->Set(this->*V.Member, ECVF_SetByCode);
		}
	}
	return true;
}

FString FGenMatchSettings::Describe() const
{
	FString Out = FString::Printf(TEXT("%s, $%d, seed %d, %d slots"), *Map, StartingCash, Seed, Slots.Num());
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		const FGenSkirmishSlot& S = Slots[i];
		Out += FString::Printf(TEXT("; p%d %s%s start %d team %d"), i + 1, *S.Faction, S.bHuman ? TEXT(" (human)") : TEXT(""), S.StartPosition, S.Team);
	}
	if (bNoAI) { Out += TEXT("; no AI"); }
	if (RevealedPlayer >= 0) { Out += FString::Printf(TEXT("; p%d sees all"), RevealedPlayer); }
	if (bPowersReady) { Out += TEXT("; powers ready"); }
	return Out;
}

void FGenSetupObject::Serialize(FArchive& Ar)
{
	Ar << Template << Owner;
	Ar << Pos.X << Pos.Y << Pos.Z;
	Ar << Angle;
}

// ---- the build and the data -----------------------------------------------------------------------

const FString& GenBuild::GetBuildId()
{
	static const FString Id = []
	{
		// The binary that holds the simulation: the game itself, or (in the editor's build) the game's module.
#if IS_MONOLITHIC
		const FString Path = FPlatformProcess::ExecutablePath();
#else
		const FString Path = FModuleManager::Get().GetModuleFilename(TEXT("Generals1"));
#endif
		TArray<uint8> Bytes;
		if (Path.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			UE_LOG(LogGenerals, Warning, TEXT("Build id: can't read '%s'"), *Path);
			return FString(TEXT("unknown"));
		}
		FSHA1 Sha;
		Sha.Update(Bytes.GetData(), Bytes.Num());
		Sha.Final();
		FSHAHash Hash;
		Sha.GetHash(Hash.Hash);
		const FString Result = Hash.ToString().Left(16);
		UE_LOG(LogGenerals, Log, TEXT("Build id %s (%s, %d bytes)"), *Result, *FPaths::GetCleanFilename(Path), Bytes.Num());
		return Result;
	}();
	return Id;
}

uint32 GenBuild::GetDataHash()
{
	static const uint32 Hash = []
	{
		FGenFileSystem& FS = FGenFileSystem::Get();
		FS.Initialize();
		TArray<FString> Files = FS.FindFiles(TEXT("Data\\INI\\"), TEXT(".ini"));
		for (FString& F : Files)
		{
			F = FGenFileSystem::NormalizeKey(F);
		}
		Files.Sort();
		uint32 Crc = 0;
		TArray<uint8> Bytes;
		for (const FString& F : Files)
		{
			Bytes.Reset();
			if (FS.ReadFile(F, Bytes))
			{
				const FTCHARToUTF8 Name(*F);
				Crc = FCrc::MemCrc32(Name.Get(), Name.Length(), Crc);
				Crc = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num(), Crc);
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Data hash %08x (%d INI files)"), Crc, Files.Num());
		return Crc;
	}();
	return Hash;
}

uint32 GenBuild::GetFileHash(const FString& VfsPath)
{
	TArray<uint8> Bytes;
	return FGenFileSystem::Get().ReadFile(VfsPath, Bytes) ? FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()) : 0;
}

// ---- replays --------------------------------------------------------------------------------------

void FGenReplay::Serialize(FArchive& Ar)
{
	Ar << BuildId << DataHash << Date << RecordedBy;
	Settings.Serialize(Ar);
	int32 Num = SetupObjects.Num();
	Ar << Num;
	if (Ar.IsLoading())
	{
		SetupObjects.SetNum(FMath::Max(0, Num));
	}
	for (FGenSetupObject& O : SetupObjects)
	{
		O.Serialize(Ar);
	}
	Num = Commands.Num();
	Ar << Num;
	if (Ar.IsLoading())
	{
		Commands.SetNum(FMath::Max(0, Num));
	}
	for (FGenCommand& C : Commands)
	{
		C.Serialize(Ar);
	}
	Ar << Checksums;
	SerializeBool(Ar, bCheated);
}

bool FGenReplay::Save(const FString& Path)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	uint32 M = Magic, V = Version;
	Writer << M << V;
	Serialize(Writer);
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool FGenReplay::Load(const FString& Path, FString* OutWhy)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		if (OutWhy) { *OutWhy = TEXT("can't read the file"); }
		return false;
	}
	FMemoryReader Reader(Bytes);
	uint32 M = 0, V = 0;
	Reader << M << V;
	if (M != Magic || V != Version)
	{
		if (OutWhy) { *OutWhy = M != Magic ? FString(TEXT("not a replay")) : FString::Printf(TEXT("a replay of version %u (this game reads %u)"), V, Version); }
		return false;
	}
	Serialize(Reader);
	if (Reader.IsError())
	{
		if (OutWhy) { *OutWhy = TEXT("the file is damaged"); }
		return false;
	}
	return true;
}

bool FGenReplay::SaveChecksumLog(const FString& Path) const
{
	FString Text;
	Text.Reserve(Checksums.Num() * 16);
	for (int32 i = 0; i < Checksums.Num(); ++i)
	{
		Text += FString::Printf(TEXT("%d %08x\n"), i + 1, Checksums[i]);
	}
	return FFileHelper::SaveStringToFile(Text, *Path);
}

FString FGenReplay::GetDefaultFolder()
{
	return FPaths::ProjectSavedDir() / TEXT("Replays");
}
