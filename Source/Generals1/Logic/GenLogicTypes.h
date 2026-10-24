#pragma once

#include "CoreMinimal.h"

/**
 * Shared simulation types and unit conversions.
 *
 * The simulation runs on a fixed timestep (default 30 Hz, like LOGICFRAMES_PER_SECOND) while
 * rendering runs at the display's frame rate and interpolates between logic frames. Rates read
 * from INI (per second) are converted to per-frame values at load time using the configured
 * logic rate, exactly like the original's INI::parseVelocityReal & co. - so the port can follow
 * the original per-frame code while the rate itself stays configurable (gen.LogicFPS).
 */
namespace GenLogic
{
	/** Logic frames per second (console variable gen.LogicFPS, read when game data loads). */
	GENERALS1_API int32 GetLogicFPS();
	FORCEINLINE float SecondsPerFrame() { return 1.f / float(GetLogicFPS()); }

	// INI conversions (INI.cpp)
	FORCEINLINE float VelocityPerFrame(float PerSecond) { return PerSecond / float(GetLogicFPS()); }
	FORCEINLINE float AccelPerFrame(float PerSecond2) { const float F = float(GetLogicFPS()); return PerSecond2 / (F * F); }
	FORCEINLINE float AngularVelocityPerFrame(float DegreesPerSecond) { return FMath::DegreesToRadians(DegreesPerSecond) / float(GetLogicFPS()); }
	FORCEINLINE float FrictionPerFrame(float PerSecond) { return PerSecond / float(GetLogicFPS()); }
	FORCEINLINE int32 MsecToFrames(float Msec) { return FMath::CeilToInt32(Msec * float(GetLogicFPS()) / 1000.f); }

	/** PATHFIND_CELL_SIZE_F (equals MAP_XY_FACTOR). */
	constexpr float PathfindCellSize = 10.f;

	/** Normalize an angle to [-PI, PI). */
	FORCEINLINE float NormalizeAngle(float A)
	{
		A = FMath::Fmod(A + PI, 2.f * PI);
		if (A < 0.f) { A += 2.f * PI; }
		return A - PI;
	}
	/** Smallest signed difference a - b. */
	FORCEINLINE float AngleDiff(float A, float B) { return NormalizeAngle(A - B); }
}

using FGenObjectID = uint32;
constexpr FGenObjectID GenInvalidObjectID = 0;

/** LocomotorSurfaceType bits (LocomotorSet.h). */
namespace GenSurface
{
	enum : uint32
	{
		Ground = 1 << 0,
		Water = 1 << 1,
		Cliff = 1 << 2,
		Air = 1 << 3,
		Rubble = 1 << 4,
		Fence = 1 << 5,   // not a locomotor's surface: a crusher's leave to drive through fences (Pathfinder's isCrusher)
	};
}

/** LocomotorAppearance (Locomotor.h). */
enum class EGenLocoAppearance : uint8
{
	TwoLegs,
	FourWheels,
	Treads,
	Hover,
	Thrust,
	Wings,
	Climber,
	Other,
	Motorcycle,
};

/** One beam to draw this frame (W3DLaserDraw + LaserUpdate): a laser pointer, a hacker's stream of numbers, a particle cannon's beam. */
struct FGenBeam
{
	const struct FGenLaserDraw* Draw = nullptr;
	FVector3f Start = FVector3f::ZeroVector;
	FVector3f End = FVector3f::ZeroVector;
	float WidthScale = 1.f;   // LaserUpdate's width scalar: a beam that is still widening, or narrowing away
};

/** Relationship between players. */
enum class EGenRelationship : uint8
{
	Enemies,
	Neutral,
	Allies,
};

struct FGenBattlePlanData;

/** BattlePlanBonuses: what the plans in force do to a player's army. */
struct GENERALS1_API FGenBattlePlanBonuses
{
	float ArmorScalar = 1.f;                // damage taken (hold the line: less)
	float SightScalar = 1.f;                // how far they see (search and destroy: further)
	int32 Bombardment = 0;                  // > 0: the BATTLEPLAN_... weapon bonus condition
	int32 HoldTheLine = 0;
	int32 SearchAndDestroy = 0;
	const FGenBattlePlanData* Kinds = nullptr; // whom it applies to (the plan's Valid/InvalidMemberKindOf)
};
