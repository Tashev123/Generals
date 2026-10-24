#pragma once

#include "CoreMinimal.h"

/** One multiplayer map of Maps\MapCache.ini (MapUtil.cpp's MapMetaData). */
struct GENERALS1_API FGenMapInfo
{
	FString Name;                       // the map's folder and file name, as the cache spells it ("tournament desert")
	FString DisplayName;                // the name the game shows for it
	int32 NumPlayers = 0;
	FVector2f ExtentMin = FVector2f::ZeroVector;
	FVector2f ExtentMax = FVector2f::ZeroVector;
	TArray<FVector2f> Starts;           // Player_N_Start: index N - 1
	TArray<FVector2f> TechPositions;
	TArray<FVector2f> SupplyPositions;
};

/** A MultiplayerColor of Multiplayer.ini. */
struct GENERALS1_API FGenPlayerColorDef
{
	FString Label;                      // "Gold"
	FLinearColor Color = FLinearColor::White;
};

/** A side a player can choose: one player template per Side, as PopulatePlayerTemplateComboBox lists them. */
struct GENERALS1_API FGenSideInfo
{
	FString Template;                   // "FactionAmerica"
	FString Label;                      // "USA"
	FString Icon;                       // mapped image
	FString ScoreImage;                 // the score screen's backdrop for this side
	FString ScoreMusic;
};

/**
 * What the skirmish menu chooses from, read from the original's data: the multiplayer maps
 * (Maps\MapCache.ini), the player colours and starting cash choices (Multiplayer.ini), the sides
 * (PlayerTemplate.ini, without the generals ChallengeMode.ini keeps locked).
 */
class GENERALS1_API FGenShellData
{
public:
	static FGenShellData& Get();

	void Initialize();

	const TArray<FGenMapInfo>& GetMaps() const { return Maps; }
	const FGenMapInfo* FindMap(const FString& Name) const;
	const TArray<FGenPlayerColorDef>& GetColors() const { return Colors; }
	const TArray<int32>& GetMoneyChoices() const { return MoneyChoices; }
	int32 GetDefaultMoney() const { return DefaultMoney; }
	const TArray<FGenSideInfo>& GetSides() const { return Sides; }
	int32 FindSide(const FString& Template) const;

	/** A string of the game's string file, or the fallback if it isn't there. */
	static FString Text(const TCHAR* Label, const TCHAR* Fallback);

private:
	void LoadMapCache();
	void LoadMultiplayerSettings();
	void LoadSides();

	bool bInitialized = false;
	TArray<FGenMapInfo> Maps;
	TArray<FGenPlayerColorDef> Colors;
	TArray<int32> MoneyChoices;
	int32 DefaultMoney = 10000;
	TArray<FGenSideInfo> Sides;
};
