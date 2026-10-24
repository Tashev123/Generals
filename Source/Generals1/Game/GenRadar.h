#pragma once

#include "CoreMinimal.h"

class FGenMapData;
class UGenTerrainComponent;
class UTexture2D;
struct FGenBridgeInfo;

/**
 * The radar (Radar.cpp / W3DRadar.cpp): where things are on the minimap, and its picture of the terrain.
 * The radar works on a 128 x 128 grid stretched over the playable map, whatever its size on screen.
 */
namespace GenRadar
{
	constexpr int32 CellWidth = 128;    // RADAR_CELL_WIDTH
	constexpr int32 CellHeight = 128;   // RADAR_CELL_HEIGHT

	/** Radar::findDrawPositions: the part of the radar window the map fills, keeping the map's proportions. */
	FBox2D FindDrawRect(const FBox2D& Window, const FVector2f& MapSize);

	/** Radar::worldToRadar: the radar cell of a world position (kept on the radar). */
	FIntPoint WorldToRadar(const FVector2f& World, const FVector2f& MapSize);

	/** W3DRadar::radarToPixel: the screen position of (the south-west corner of) a radar cell; north is up. */
	FVector2D RadarToPixel(const FVector2D& Radar, const FBox2D& DrawRect);

	/** Radar::localPixelToRadar + radarToWorld2D. False outside the map's part of the window. */
	bool PixelToWorld(const FVector2D& Pixel, const FBox2D& Window, const FVector2f& MapSize, FVector2f& OutWorld);

	/**
	 * W3DRadar::buildTerrainTexture: the terrain's own colours, darker where it lies low, and water in the
	 * radar's water colour. OutAverageZ is Radar::getTerrainAverageZ, the height the view box is measured at.
	 */
	UTexture2D* BuildTerrainTexture(const FGenMapData& Map, const UGenTerrainComponent& Terrain, const TArray<FGenBridgeInfo>& Bridges, float& OutAverageZ);

	/** Clips a line to a rectangle (ClipLine2D). False if nothing of it is inside. */
	bool ClipLine(FVector2D& A, FVector2D& B, const FBox2D& Rect);
}
