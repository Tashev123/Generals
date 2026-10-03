#pragma once

#include "CoreMinimal.h"

/**
 * Coordinate conventions.
 *
 * The simulation keeps the original Generals units and frames so every INI value
 * (speeds, ranges, radii) is used unchanged:
 *   Generals world : right-handed, Z up, X east, Y north, 1 unit ~ 0.1 m
 *   W3D model space: right-handed, Z up, X forward, Y left (3ds Max)
 *   Unreal         : left-handed,  Z up, X forward, Y right, centimeters
 *
 * Generals/W3D -> Unreal is a mirror on Y plus a uniform scale. Mirroring flips
 * handedness, which also turns the source's CCW front faces into Unreal's CW
 * front faces, so triangle index order is kept as-is.
 */
namespace GenCoords
{
	/** Unreal centimeters per Generals world unit. */
	constexpr double UnitsToCm = 10.0;

	/** Logic frames per second (LOGICFRAMES_PER_SECOND). */
	constexpr int32 LogicFps = 30;

	/** Height map cell size and height scale (MapObject.h). */
	constexpr float MapXYFactor = 10.0f;
	constexpr float MapHeightScale = MapXYFactor / 16.0f;

	FORCEINLINE FVector ToUE(const FVector& Gen) { return FVector(Gen.X, -Gen.Y, Gen.Z) * UnitsToCm; }
	FORCEINLINE FVector ToUE(float X, float Y, float Z) { return FVector(X, -Y, Z) * UnitsToCm; }
	FORCEINLINE FVector3f ToUEf(float X, float Y, float Z) { return FVector3f(X, -Y, Z) * float(UnitsToCm); }
	FORCEINLINE FVector FromUE(const FVector& UE) { return FVector(UE.X, -UE.Y, UE.Z) / UnitsToCm; }

	/** Direction vectors (normals): mirror only, no scale. */
	FORCEINLINE FVector3f DirToUE(float X, float Y, float Z) { return FVector3f(X, -Y, Z); }

	/** Rotation quaternion (x,y,z,w) in a right-handed Z-up frame -> Unreal frame (mirror on Y). */
	FORCEINLINE FQuat QuatToUE(float X, float Y, float Z, float W) { return FQuat(-X, Y, -Z, W); }

	/** Generals heading (radians, CCW about +Z) -> Unreal yaw (degrees). */
	FORCEINLINE double AngleToYaw(float Radians) { return -FMath::RadiansToDegrees(double(Radians)); }
}
