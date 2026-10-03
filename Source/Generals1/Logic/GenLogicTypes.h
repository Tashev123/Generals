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

/** Relationship between players. */
enum class EGenRelationship : uint8
{
	Enemies,
	Neutral,
	Allies,
};
