#include "Core/GenFileSystem.h"
#include "Generals1.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	uint32 ReadBE32(const uint8* P) { return (uint32(P[0]) << 24) | (uint32(P[1]) << 16) | (uint32(P[2]) << 8) | uint32(P[3]); }

	bool IsModArchive(const FString& FileName)
	{
		// GenLauncher prefixes mod archives with a number and underscore, e.g. "340_ControlBarPro.big".
		int32 i = 0;
		while (i < FileName.Len() && FChar::IsDigit(FileName[i])) { ++i; }
		return i > 0 && i < FileName.Len() && FileName[i] == TEXT('_');
	}
}

FGenFileSystem& FGenFileSystem::Get()
{
	static FGenFileSystem Instance;
	return Instance;
}

FString FGenFileSystem::NormalizeKey(const FString& Path)
{
	FString Key = Path.Replace(TEXT("/"), TEXT("\\"));
	Key.ToLowerInline();
	return Key;
}

FString FGenFileSystem::FindInstallDir(const TCHAR* RegistrySubKey, const TCHAR* Fallback)
{
	FString Dir;
#if PLATFORM_WINDOWS
	const FString Keys[] = {
		FString(TEXT("SOFTWARE\\WOW6432Node\\Electronic Arts\\EA Games\\")) + RegistrySubKey,
		FString(TEXT("SOFTWARE\\Electronic Arts\\EA Games\\")) + RegistrySubKey,
	};
	for (const FString& Key : Keys)
	{
		if (FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, *Key, TEXT("InstallPath"), Dir) && !Dir.IsEmpty())
		{
			break;
		}
	}
#endif
	if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
	{
		Dir = Fallback;
	}
	FPaths::NormalizeDirectoryName(Dir);
	return Dir;
}

bool FGenFileSystem::Initialize()
{
	if (bInitialized)
	{
		return true;
	}

	// GamePaths.txt beside the game, for installs the registry doesn't name (a Steam or EA app copy):
	// "ZeroHour=<folder>" and "Generals=<folder>", a line each.
	FString PathsText;
	if (FFileHelper::LoadFileToString(PathsText, *(FPaths::RootDir() / TEXT("GamePaths.txt"))))
	{
		TArray<FString> Lines;
		PathsText.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value) && !Value.TrimStartAndEnd().IsEmpty() && !Line.StartsWith(TEXT("#")))
			{
				Value = Value.TrimStartAndEnd().TrimQuotes();
				if (Key.TrimStartAndEnd().Equals(TEXT("ZeroHour"), ESearchCase::IgnoreCase)) { ZeroHourDir = Value; }
				else if (Key.TrimStartAndEnd().Equals(TEXT("Generals"), ESearchCase::IgnoreCase)) { GeneralsDir = Value; }
			}
		}
	}
	// Command line overrides: -GenZHDir="..." -GenDir="..."
	if (FParse::Value(FCommandLine::Get(), TEXT("GenZHDir="), ZeroHourDir) || !ZeroHourDir.IsEmpty())
	{
		FPaths::NormalizeDirectoryName(ZeroHourDir);
	}
	else
	{
		ZeroHourDir = FindInstallDir(TEXT("Command and Conquer Generals Zero Hour"),
			TEXT("D:/Games/EA Games/Command & Conquer Generals Zero Hour"));
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("GenDir="), GeneralsDir) || !GeneralsDir.IsEmpty())
	{
		FPaths::NormalizeDirectoryName(GeneralsDir);
	}
	else
	{
		GeneralsDir = FindInstallDir(TEXT("Generals"), TEXT("D:/Games/EA Games/Command and Conquer Generals"));
	}

	if (!IFileManager::Get().DirectoryExists(*ZeroHourDir))
	{
		UE_LOG(LogGenerals, Error, TEXT("Zero Hour install not found (looked in '%s'). Pass -GenZHDir=<path>."), *ZeroHourDir);
		return false;
	}

	IndexLooseFiles(ZeroHourDir);

	for (const FString& Dir : { ZeroHourDir, GeneralsDir })
	{
		if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
		{
			continue;
		}
		TArray<FString> Archives;
		IFileManager::Get().FindFiles(Archives, *(Dir / TEXT("*.big")), true, false);
		Archives.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
		for (const FString& Name : Archives)
		{
			if (!IsModArchive(Name))
			{
				IndexArchive(Dir / Name);
			}
		}
	}

	bInitialized = true;
	UE_LOG(LogGenerals, Log, TEXT("Generals VFS: %d files from %d sources (ZH='%s', Generals='%s')"),
		Files.Num(), Sources.Num(), *ZeroHourDir, *GeneralsDir);
	return true;
}

void FGenFileSystem::IndexLooseFiles(const FString& Root)
{
	const FString DataDir = Root / TEXT("Data");
	TArray<FString> Found;
	IFileManager::Get().FindFilesRecursive(Found, *DataDir, TEXT("*"), true, false);
	for (const FString& Full : Found)
	{
		FString Rel = Full;
		FPaths::MakePathRelativeTo(Rel, *(Root + TEXT("/")));
		const FString Key = NormalizeKey(Rel);
		if (!Files.Contains(Key))
		{
			FEntry E;
			E.Path = Rel.Replace(TEXT("/"), TEXT("\\"));
			E.SourceIndex = Sources.Add(Full);
			E.Offset = -1;
			E.Size = IFileManager::Get().FileSize(*Full);
			Files.Add(Key, MoveTemp(E));
		}
	}
}

void FGenFileSystem::IndexArchive(const FString& ArchivePath)
{
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*ArchivePath));
	if (!Handle)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Cannot open archive %s"), *ArchivePath);
		return;
	}

	uint8 Header[16];
	if (!Handle->Read(Header, 16) || (FMemory::Memcmp(Header, "BIGF", 4) != 0 && FMemory::Memcmp(Header, "BIG4", 4) != 0))
	{
		UE_LOG(LogGenerals, Warning, TEXT("%s is not a BIG archive"), *ArchivePath);
		return;
	}
	const uint32 Count = ReadBE32(Header + 8);
	const uint32 HeaderSize = ReadBE32(Header + 12);
	if (HeaderSize < 16 || HeaderSize > Handle->Size())
	{
		return;
	}

	TArray<uint8> Table;
	Table.SetNumUninitialized(HeaderSize - 16);
	if (!Handle->Read(Table.GetData(), Table.Num()))
	{
		return;
	}

	const int32 SourceIndex = Sources.Add(ArchivePath);
	int32 Pos = 0;
	for (uint32 i = 0; i < Count && Pos + 8 < Table.Num(); ++i)
	{
		const uint32 Offset = ReadBE32(&Table[Pos]);
		const uint32 Size = ReadBE32(&Table[Pos + 4]);
		Pos += 8;
		const int32 NameStart = Pos;
		while (Pos < Table.Num() && Table[Pos] != 0) { ++Pos; }
		FString Name;
		Name.Reserve(Pos - NameStart);
		for (int32 c = NameStart; c < Pos; ++c)
		{
			Name.AppendChar(TCHAR(Table[c])); // Latin-1
		}
		++Pos; // skip NUL

		const FString Key = NormalizeKey(Name);
		if (!Files.Contains(Key))
		{
			FEntry E;
			E.Path = Name;
			E.SourceIndex = SourceIndex;
			E.Offset = Offset;
			E.Size = Size;
			Files.Add(Key, MoveTemp(E));
		}
	}
}

bool FGenFileSystem::Exists(const FString& Path) const
{
	return Files.Contains(NormalizeKey(Path));
}

bool FGenFileSystem::ReadFile(const FString& Path, TArray<uint8>& OutData) const
{
	const FEntry* E = Files.Find(NormalizeKey(Path));
	if (!E)
	{
		return false;
	}
	FScopeLock Lock(&ReadLock);
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Sources[E->SourceIndex]));
	if (!Handle)
	{
		return false;
	}
	if (E->Offset >= 0 && !Handle->Seek(E->Offset))
	{
		return false;
	}
	OutData.SetNumUninitialized(E->Size);
	return Handle->Read(OutData.GetData(), E->Size);
}

TArray<FString> FGenFileSystem::FindFiles(const FString& Prefix, const FString& Suffix) const
{
	const FString P = NormalizeKey(Prefix);
	const FString S = NormalizeKey(Suffix);
	TArray<FString> Out;
	for (const TPair<FString, FEntry>& It : Files)
	{
		if (It.Key.StartsWith(P, ESearchCase::CaseSensitive) && It.Key.EndsWith(S, ESearchCase::CaseSensitive))
		{
			Out.Add(It.Value.Path);
		}
	}
	Out.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	return Out;
}
