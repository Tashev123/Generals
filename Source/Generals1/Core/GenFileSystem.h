#pragma once

#include "CoreMinimal.h"

/**
 * Read-only view of the game's data, mirroring Win32BIGFileSystem + LocalFileSystem:
 *   1. loose files under <ZeroHour>/Data
 *   2. *.big in the Zero Hour install dir (first archive providing a path wins)
 *   3. *.big in the base Generals install dir
 * GenLauncher mod archives ("NNN_*.big") are skipped so we always read vanilla data.
 *
 * Paths are case-insensitive and use backslashes, e.g. "Art\\W3D\\AVLeopard.w3d".
 */
class GENERALS1_API FGenFileSystem
{
public:
	static FGenFileSystem& Get();

	/** Indexes archives on first use. Safe to call repeatedly. */
	bool Initialize();
	bool IsInitialized() const { return bInitialized; }

	bool Exists(const FString& Path) const;
	bool ReadFile(const FString& Path, TArray<uint8>& OutData) const;

	/** All indexed paths (original case) starting with Prefix and ending with Suffix (case-insensitive). */
	TArray<FString> FindFiles(const FString& Prefix, const FString& Suffix) const;

	const FString& GetZeroHourDir() const { return ZeroHourDir; }
	const FString& GetGeneralsDir() const { return GeneralsDir; }

	static FString NormalizeKey(const FString& Path);

private:
	struct FEntry
	{
		FString Path;          // original case
		int32 SourceIndex = -1; // index into Sources
		int64 Offset = -1;      // -1 for loose files
		int64 Size = 0;
	};

	void IndexLooseFiles(const FString& Root);
	void IndexArchive(const FString& ArchivePath);
	static FString FindInstallDir(const TCHAR* RegistrySubKey, const TCHAR* Fallback);

	bool bInitialized = false;
	FString ZeroHourDir;
	FString GeneralsDir;
	TArray<FString> Sources;         // archive or loose file paths
	TMap<FString, FEntry> Files;     // key: normalized path
	mutable FCriticalSection ReadLock;
};
