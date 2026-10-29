#pragma once

#include "CoreMinimal.h"
#include "Logic/GenCommand.h"

/** One skirmish slot as chosen in the setup screen. */
struct GENERALS1_API FGenSkirmishSlot
{
	FString Faction = TEXT("FactionAmerica");   // PlayerTemplate name
	bool bHuman = false;
	int32 StartPosition = -1;                    // -1 = next free
	int32 Team = -1;
	FLinearColor Color = FLinearColor::Transparent; // transparent = faction preferred color
	FString Name;                                // empty: "Player N"
	int32 Difficulty = -1;                       // a computer player's: 0 easy, 1 medium, 2 hard (-1: the match's default)

	void Serialize(FArchive& Ar);
};

/**
 * Everything a game starts from that the simulation depends on. The same settings, the same orders at the
 * same frames and the same program make the same game, frame for frame: that is what a replay is, and what
 * keeps the computers of a network game together.
 *
 * Players are numbered as the simulation numbers them: 0 is the neutral player, slot i is player i + 1.
 */
struct GENERALS1_API FGenMatchSettings
{
	FString Map;                    // its name in the map cache ("Tournament Desert"); a campaign's: its folder ("MD_USA01")
	// A campaign mission (Campaign.ini): the players, their teams and what happens come from the map's own sides and
	// scripts, not from slots. DefaultDifficulty is the campaign's difficulty.
	FString Campaign;               // "USA"; empty: a skirmish
	FString Mission;                // "Mission01"
	int32 StartingCash = 10000;
	int32 Seed = 0;                 // the simulation's random numbers
	TArray<FGenSkirmishSlot> Slots;
	bool bNoAI = false;             // (tests) computer players stand still
	int32 DefaultDifficulty = 1;    // for computer players whose slot doesn't say (0 easy, 1 medium, 2 hard)
	int32 RevealedPlayer = INDEX_NONE; // (tests, -GenNoShroud) that player sees the whole map, all the time
	bool bPowersReady = false;      // (tests, -GenPowersReady) special powers don't wait for their timers
	// The console variables that change what the simulation works out (see ApplyToSimulation).
	int32 LogicFPS = 30;
	int32 PathCellsPerStep = 5000;
	int32 PathCliffMargin = 1;
	int32 PathWeight = 13;
	int32 MissileFlight = 1;
	int32 Steering = 1;

	void Serialize(FArchive& Ar);
	/** What a game started on this computer gets: the command line's test switches and the console variables as they are. */
	static FGenMatchSettings FromCommandLine();
	/** Sets the console variables to the match's. False (and Why) when that can't be done: the logic rate is read
	 *  with the game's data, so a match at another rate needs the program started at it. */
	bool ApplyToSimulation(FString* OutWhy = nullptr) const;
	FString Describe() const;
};

/** An object the game's start put on the map besides the map's own and the players' starting ones (test switches). */
struct GENERALS1_API FGenSetupObject
{
	FString Template;
	int32 Owner = 0;
	FVector3f Pos = FVector3f::ZeroVector;
	float Angle = 0.f;

	void Serialize(FArchive& Ar);
};

/** What identifies this program and the game data it has read: two computers must agree on both to play together. */
namespace GenBuild
{
	/** A hash of the game's own code (the binary holding the simulation), worked out once. */
	GENERALS1_API const FString& GetBuildId();
	/** A hash of the game data the simulation is made from: every INI file under Data\INI, in path order. */
	GENERALS1_API uint32 GetDataHash();
	/** A hash of one file of the game data (a map); 0 if there is no such file. */
	GENERALS1_API uint32 GetFileHash(const FString& VfsPath);
}

/**
 * A game as it was played: its settings, what the start put on the map, every order with the frame it was
 * applied at, and a checksum of the simulation after every frame (UGenGameLogic::ComputeChecksum). Played
 * back, the orders make the game again, and the checksums say at which frame it stopped being the same game.
 */
struct GENERALS1_API FGenReplay
{
	static constexpr uint32 Magic = 0x52504547; // "GEPR"
	static constexpr uint32 Version = 3;

	FString BuildId;
	uint32 DataHash = 0;
	FString Date;
	int32 RecordedBy = INDEX_NONE;  // the player whose computer recorded it
	FGenMatchSettings Settings;
	TArray<FGenSetupObject> SetupObjects;
	TArray<FGenCommand> Commands;   // in the order applied
	TArray<uint32> Checksums;       // [n]: after the step that made frame n + 1
	bool bCheated = false;          // the test switches changed the game by hand outside the orders (not repeatable)

	void Serialize(FArchive& Ar);
	bool Save(const FString& Path);
	bool Load(const FString& Path, FString* OutWhy = nullptr);
	/** One line per frame: "frame checksum" (for comparing two runs outside the game). */
	bool SaveChecksumLog(const FString& Path) const;
	static FString GetDefaultFolder();
};
