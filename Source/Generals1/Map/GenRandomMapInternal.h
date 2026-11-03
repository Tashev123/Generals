#pragma once

#include "CoreMinimal.h"
#include "Map/GenRandomMap.h"

class FGenMapData;

/**
 * The inside of the random map generator: what is common to its parts.
 *
 * Nothing here may depend on the computer it runs on: every computer of a network game makes the map itself. So there is
 * one rule: only integers, + - * / and square roots on floats, our own random numbers, our own sine, arrays in a fixed order.
 */
namespace GenRandomMapDetail
{
	/** PCG32: the same numbers everywhere, and a stream of their own for each stage, so that a change in one doesn't move the rest. */
	class FDetRandom
	{
	public:
		FDetRandom(uint32 Seed, uint32 Stream)
		{
			State = 0;
			Inc = (uint64(Stream) << 1) | 1u;
			Next();
			State += Seed;
			Next();
		}
		uint32 Next()
		{
			const uint64 Old = State;
			State = Old * 6364136223846793005ULL + Inc;
			const uint32 Xor = uint32(((Old >> 18u) ^ Old) >> 27u);
			const uint32 Rot = uint32(Old >> 59u);
			return (Xor >> Rot) | (Xor << ((0u - Rot) & 31u));
		}
		int32 Range(int32 Min, int32 Max) { return Max <= Min ? Min : Min + int32(Next() % uint32(Max - Min + 1)); }
		float Frac() { return float(Next() >> 8) * (1.f / 16777216.f); }
		float Between(float A, float B) { return A + (B - A) * Frac(); }
		bool Chance(float P) { return Frac() < P; }
	private:
		uint64 State;
		uint64 Inc;
	};

	/** Turns (1 = a full circle): a polynomial, not the C library's. */
	void DetSinCos(float Turns, float& OutSin, float& OutCos);
	/** Radians for the map's objects from turns. */
	inline float TurnsToRadians(float Turns) { return Turns * 6.2831853f; }

	/** Gradient noise, about -1..1. */
	float GradientNoise(float X, float Y, uint32 Seed);
	float FractalNoise(float X, float Y, uint32 Seed, int32 Octaves);

	inline float Smooth(float A, float B, float X)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	/** A name with how often it is taken. */
	struct FWeighted
	{
		const TCHAR* Name = nullptr;
		int32 Weight = 1;
	};
	const TCHAR* PickWeighted(const TArray<FWeighted>& List, FDetRandom& Random);

	/** An environment: all of it names out of the game's own data, taken from official maps of that kind of place. */
	struct FEnvironmentDef
	{
		const TCHAR* Key = nullptr;
		const TCHAR* Label = nullptr;
		const TCHAR* MapIniDonor = nullptr;         // the official map whose map.ini it uses (water look, weather)
		const TCHAR* LightDonors[4] = {};           // for each time of day, the official map whose lights it uses
		bool bSnowy = false;                        // WorldInfo's weather: what stands on the map wears its snowy looks
		float Hilliness = 1.f;                      // how much the ground rolls
		float Forest = 1.f;                         // how many trees (1: about as many as an official green map)
		// Terrain classes (Terrain.ini). Lowest on the list of FMapBuilder::PaintClasses is painted over by the higher.
		TArray<const TCHAR*> Ground;                // the main mix
		const TCHAR* Patch = nullptr;               // patches in it
		const TCHAR* Dirt = nullptr;                // slopes, road sides
		const TCHAR* Rock = nullptr;                // steep slopes
		const TCHAR* Cliff = nullptr;               // what can't be climbed
		const TCHAR* Shore = nullptr;               // beside the water
		const TCHAR* Bed = nullptr;                 // under the water
		const TCHAR* Base = nullptr;                // a player's base site
		const TCHAR* Town = nullptr;                // inside a town
		const TCHAR* Crops = nullptr;               // fields, if the place has them
		// What stands on it
		TArray<FWeighted> Trees, Bushes, Rocks, BigRocks;
		TArray<FWeighted> Houses, Shops, Large, Towers, FarmBuildings;
		TArray<FWeighted> StreetProps, YardProps, FarmProps, Cars;
		TArray<const TCHAR*> Landmarks;             // one-offs for the middle of a town
		const TCHAR* StreetLamp = nullptr;
		// Roads.ini
		const TCHAR* RoadMain = nullptr;
		const TCHAR* RoadTown = nullptr;
		const TCHAR* RoadAvenue = nullptr;
		const TCHAR* Bridge = nullptr;
		// Sounds of the place
		TArray<const TCHAR*> AmbientDay, AmbientNight, AmbientWater;
	};
	const TArray<FEnvironmentDef>& GetEnvironmentDefs();

	enum class ELayout : int32 { Plains, Highlands, Rivers, Lake, Canyons, Coast, City, Count };
	/** How the map's name spells it. */
	const TCHAR* GetLayoutKey(ELayout Layout);

	/** A building or a resource the plan has decided on. */
	struct FPlanSite
	{
		enum EKind : uint8 { Dock, Pile, Derrick, Refinery, Artillery, Reinforcement, Hospital, RepairBay, Bunker };
		EKind Kind = Dock;
		FVector2f Pos = FVector2f::ZeroVector;
		float Turns = 0.f;
	};

	/** The way between two places that the terrain keeps open. */
	struct FPlanCorridor
	{
		TArray<FVector2f> Points;
		bool bRoad = false;
	};

	/** Where a corridor crosses water. */
	struct FPlanCrossing
	{
		FVector2f From = FVector2f::ZeroVector, To = FVector2f::ZeroVector;
		bool bFord = false;                         // a sand bar instead of a bridge
	};

	struct FPlanTown
	{
		FVector2f Center = FVector2f::ZeroVector;
		float Radius = 0.f;
		bool bCity = false;
		bool bFarm = false;
		bool bVertical = false;                     // its main street runs along y
	};

	/** What is decided before any terrain exists: quick, and all the menus need. */
	struct FPlan
	{
		FGenRandomMapParams Params;                 // as asked for
		int32 Environment = 0, TimeOfDay = 1;       // as decided
		ELayout Layout = ELayout::Plains;
		int32 Players = 2;
		uint32 Seed = 1;
		int32 CellsX = 0, CellsY = 0;               // the playable area (map cells, 10 world units each)
		float SizeX = 0.f, SizeY = 0.f;             // ... in world units
		FVector2f Middle = FVector2f::ZeroVector;   // the middle of the land
		TArray<FVector2f> Starts;
		// Water
		bool bLake = false;
		FVector2f LakeCenter = FVector2f::ZeroVector;
		float LakeRadius = 0.f;
		TArray<TArray<FVector2f>> Rivers;
		float RiverHalfWidth = 0.f;
		bool bCoast = false;
		FVector2f CoastPoint = FVector2f::ZeroVector, CoastInland = FVector2f::ZeroVector;
		// The rest
		TArray<FPlanCorridor> Corridors;
		TArray<FPlanCrossing> Crossings;
		TArray<FPlanSite> Sites;
		TArray<FPlanTown> Towns;
	};

	bool MakePlan(const FGenRandomMapParams& Params, FPlan& Out);
	/** The whole map out of the plan. OutProblems (optional) says what checks found, empty is good. */
	bool BuildMap(const FPlan& Plan, FGenMapData& Out, TArray<FString>* OutProblems = nullptr);
	/** World units: the ground (and its water) these parts work with. */
	constexpr float WaterZ = 18.f;
}
