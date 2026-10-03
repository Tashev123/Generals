#pragma once

#include "CoreMinimal.h"

/**
 * Generic tree form of Generals INI data (GameEngine/Source/Common/INI/INI.cpp).
 *
 *  - ';' starts a comment, control characters become spaces, tokens split on " \t=".
 *  - Each top-level statement is "<BlockType> [args...]" closed by "End".
 *  - A field opens a nested block only when its original parser calls INI::initFromINI;
 *    that set is reproduced in GenIni.cpp. Typed interpretation happens later, per template.
 */
struct FGenIniBlock;

struct GENERALS1_API FGenIniEntry
{
	FString Key;
	TArray<FString> Values;        // tokens after the key
	FString Raw;                   // text after the key / '=' (comment stripped), for custom tokenizing
	int32 Line = 0;
	TSharedPtr<FGenIniBlock> Block; // set when the field opens a nested block

	const FString& Value(int32 Index = 0) const;
	float AsFloat(int32 Index = 0, float Default = 0.f) const;
	int32 AsInt(int32 Index = 0, int32 Default = 0) const;
	bool AsBool(int32 Index = 0, bool Default = false) const;
};

struct GENERALS1_API FGenIniBlock
{
	FString Keyword;               // "Object", "Draw", "ConditionState", ...
	TArray<FString> Args;          // e.g. ["W3DTankDraw", "ModuleTag_01"]
	FString File;
	int32 Line = 0;
	TArray<FGenIniEntry> Entries;

	const FString& Name() const;   // first arg
	const FGenIniEntry* Find(const TCHAR* Key) const;
	TArray<const FGenIniEntry*> FindAll(const TCHAR* Key) const;
	FString GetString(const TCHAR* Key, const FString& Default = FString()) const;
	float GetFloat(const TCHAR* Key, float Default = 0.f) const;
	int32 GetInt(const TCHAR* Key, int32 Default = 0) const;
	bool GetBool(const TCHAR* Key, bool Default = false) const;
};

namespace GenIni
{
	/** Parses INI text into top-level blocks. Errors are logged; returns false on structural errors. */
	GENERALS1_API bool Parse(const FString& Text, const FString& FileName, TArray<TSharedPtr<FGenIniBlock>>& OutBlocks);

	/** Splits on the default separators " \t=". */
	GENERALS1_API void Tokenize(const FString& Text, TArray<FString>& OutTokens, const TCHAR* Separators = TEXT(" \t="));

	/** Reads an INI file from the game VFS (Latin-1) and parses it. */
	GENERALS1_API bool ParseFile(const FString& VfsPath, TArray<TSharedPtr<FGenIniBlock>>& OutBlocks);
}
