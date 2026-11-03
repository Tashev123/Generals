#pragma once

#include "CoreMinimal.h"

class FGenMapData;
struct FGenMapInfo;
struct FGenImage;

/**
 * What a random map is made from (the original has no such thing: it is what Age of Empires II's lobby offers,
 * with this game's environments, civilian buildings and times of day). Everything that decides the map is in here and
 * goes into the map's name, so that the name alone makes the same map again, to the last tree: on every computer of a
 * network game, in a replay and in a saved game - nothing about the map has to be sent or stored but its name.
 */
struct GENERALS1_API FGenRandomMapParams
{
	static constexpr int32 Any = -1;    // (Environment, Layout, TimeOfDay) the seed decides

	int32 Environment = Any;            // index of GenRandomMap::GetEnvironments()
	int32 Layout = Any;                 // index of GenRandomMap::GetLayouts()
	int32 Size = 1;                     // 0 small, 1 medium, 2 large, 3 huge
	int32 TimeOfDay = Any;              // 0 morning, 1 afternoon, 2 evening, 3 night
	int32 Resources = 1;                // 0 scarce, 1 normal, 2 rich
	int32 Players = 4;                  // how many start positions (2..8)
	int32 Seed = 1;

	/** "random:desert:rivers:medium:evening:normal:4:81237" */
	FString ToName() const;
	static bool FromName(const FString& Name, FGenRandomMapParams& Out);
};

namespace GenRandomMap
{
	/** Does this "map name" stand for a generated map? */
	GENERALS1_API bool IsRandomName(const FString& Name);

	/** What the menu chooses from (labels; the first of each list is "left to the seed" where the seed may decide). */
	GENERALS1_API const TArray<FString>& GetEnvironments();
	GENERALS1_API const TArray<FString>& GetLayouts();
	/** One line saying what a layout is. */
	GENERALS1_API FString GetLayoutDescription(int32 Layout);

	/** What the menus show of the map: its name, start positions, supplies. Quick: nothing is built for this. False: not a valid name. */
	GENERALS1_API bool MakeInfo(const FString& Name, FGenMapInfo& Out);

	/**
	 * The map itself, as if it had been read from a file. Worked out from the name alone, with nothing but
	 * integer arithmetic, + - * / and square roots (no sin, pow... whose last bits differ between processors), the same on
	 * every computer. The game's data must be loaded.
	 */
	GENERALS1_API bool Generate(const FString& Name, FGenMapData& Out);

	/** Where its map.ini is (water, weather): an official map of that environment. */
	GENERALS1_API FString GetMapIniFolder(const FString& Name);

	/** A picture of it for the menus, seen from above: north is the top row, the playable area only. */
	GENERALS1_API bool MakePreview(const FString& Name, FGenImage& Out);

	/** -GenRandomMapTest=N: makes N maps of all kinds, checks them, logs a line for each and writes their pictures to Saved/RandomMaps. */
	GENERALS1_API void RunSelfTest(int32 Count);
}
