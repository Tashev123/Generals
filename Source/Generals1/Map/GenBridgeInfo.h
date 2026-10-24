#pragma once

#include "CoreMinimal.h"

struct FGenBridgeType;

/**
 * Where a bridge is, for the game logic (BridgeInfo and the Bridge class of TerrainLogic.cpp): the
 * rectangle of its deck, as wide as the bridge model, between the two bridge points of the map.
 * Generals world units.
 */
struct GENERALS1_API FGenBridgeInfo
{
	FVector3f From = FVector3f::ZeroVector;      // the ends of the centre line; z is the ground there plus a little
	FVector3f To = FVector3f::ZeroVector;
	FVector3f FromLeft = FVector3f::ZeroVector;  // the four corners of the deck
	FVector3f FromRight = FVector3f::ZeroVector;
	FVector3f ToLeft = FVector3f::ZeroVector;
	FVector3f ToRight = FVector3f::ZeroVector;
	float Width = 0.f;
	FBox2f Bounds = FBox2f(ForceInit);
	const FGenBridgeType* Type = nullptr;

	void ComputeBounds();

	/** Bridge::isPointOnBridge: inside the deck's rectangle, seen from above. */
	bool IsPointOnBridge(float X, float Y) const;
	/** Bridge::getBridgeHeight: the deck's plane at that spot. */
	float GetHeight(float X, float Y) const;
	/** Bridge::isCellOnEnd: the cell touches one of the two end lines (pulled in one cell from the sides). */
	bool IsCellOnEnd(const FBox2f& Cell) const;
	/** Bridge::isCellOnSide: the cell touches a side line, or the line one cell beyond it. */
	bool IsCellOnSide(const FBox2f& Cell) const;
	/** Bridge::isCellEntryPoint: "Is a pathfind cell a spot to move onto the bridge" - half a cell beyond an end. */
	bool IsCellEntryPoint(const FBox2f& Cell) const;
	/** Where a ray meets the deck (true if it does, within the rectangle). */
	bool RayHit(const FVector3f& Start, const FVector3f& Dir, FVector3f& OutHit) const;
};
