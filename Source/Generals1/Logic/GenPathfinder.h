#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

class FGenMapData;
class FGenObject;

/**
 * Grid pathfinder (AIPathfind.cpp equivalent, simplified).
 *
 * One cell per height map cell (PATHFIND_CELL_SIZE == MAP_XY_FACTOR). Cells are classified like
 * Pathfinder::classifyMapCell (cliff flags, water polygons) plus structure footprints
 * (classifyObjectFootprint: immobile, non-small structures). Paths are 8-connected A* without
 * corner cutting, then smoothed with line-of-sight checks.
 */
class GENERALS1_API FGenPathfinder
{
public:
	enum class ECell : uint8
	{
		Clear,
		Water,
		Cliff,
		Rubble,
		Obstacle,
		Impassable,
	};

	void Build(const FGenMapData& Map);

	/** Adds/removes a structure footprint (reference counted per cell). */
	void AddObstacle(const FGenObject& Obj);
	void RemoveObstacle(const FGenObject& Obj);

	/** Finds a path in world units. Returns false if the start is invalid; if the goal is unreachable the
	 *  path leads to the closest reachable cell. OutPath excludes the start, ends at the (adjusted) goal. */
	bool FindPath(const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces, TArray<FVector2f>& OutPath) const;

	/** Nearest passable position to Pos for the given surfaces (spiral search). */
	bool AdjustDestination(const FVector2f& Pos, uint32 Surfaces, FVector2f& OutPos, int32 MaxRadiusCells = 30) const;

	bool IsPassable(const FVector2f& WorldPos, uint32 Surfaces) const;
	bool IsLineClear(const FVector2f& A, const FVector2f& B, uint32 Surfaces) const;

	// --- units (Pathfinder::updatePos / updateGoal / checkDestination / adjustDestination) ---
	// Ground units reserve the cells of their destination, and mark the cells they stand on while not
	// moving, so that other units end their moves on free ground and can path around parked ones.

	/** Marks the cells a unit covers; only stationary units block (UNIT_PRESENT_FIXED). */
	void UpdateUnitPos(FGenObjectID Id, const FVector2f& Pos, float Radius, bool bMoving);
	/** Reserves the cells around a unit's destination. */
	void UpdateUnitGoal(FGenObjectID Id, const FVector2f& GoalPos, float Radius);
	/** Forgets a unit entirely (death / removal). */
	void RemoveUnit(FGenObjectID Id);
	/** True if a unit of that size can end a move at Pos: passable ground, no other unit's goal, nobody parked there. */
	bool IsDestinationFree(FGenObjectID Id, const FVector2f& Pos, float Radius, uint32 Surfaces) const;
	/** Moves InOutPos to the nearest free destination (outward square spiral, 400 cells as in the original). */
	bool AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos) const;
	/** Same, with an extra condition on the spot (adjustTargetDestination: e.g. "within weapon range of the target"). */
	bool AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos, TFunctionRef<bool(const FVector2f&)> Accept) const;
	/** FindPath that treats cells held by other stationary units (widened by this unit's size) as blocked. */
	bool FindPathAroundUnits(FGenObjectID Id, float Radius, const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces, TArray<FVector2f>& OutPath) const;
	/** Pathfinder::getRadiusAndCenter: half-size in cells of a unit's footprint, and whether it is centered in a cell. */
	static void GetRadiusAndCenter(float BoundingCircleRadius, int32& OutRadius, bool& bOutCenter);

	ECell GetCell(int32 X, int32 Y) const;
	int32 GetWidth() const { return Width; }
	int32 GetHeight() const { return Height; }
	FIntPoint WorldToCell(const FVector2f& P) const;
	FVector2f CellCenter(int32 X, int32 Y) const;

private:
	bool CanPass(int32 X, int32 Y, uint32 Surfaces) const;
	void RasterizeFootprint(const FGenObject& Obj, int32 Delta);
	FIntRect UnitRect(const FVector2f& Pos, float Radius) const;   // cells [Min, Max)
	void Stamp(TArray<FGenObjectID>& Layer, const FIntRect& Rect, FGenObjectID From, FGenObjectID To);

	int32 Width = 0;
	int32 Height = 0;
	int32 Border = 0;
	TArray<ECell> Terrain;     // static classification
	TArray<uint16> Obstacles;  // footprint reference counts

	TArray<FGenObjectID> GoalUnit;   // per cell: unit that reserved it as (part of) its destination
	TArray<FGenObjectID> FixedUnit;  // per cell: unit standing still on it
	TMap<FGenObjectID, FIntRect> GoalRects;
	TMap<FGenObjectID, FIntRect> FixedRects;

	// Set only for the duration of FindPathAroundUnits.
	mutable FGenObjectID AvoidSelf = GenInvalidObjectID;
	mutable int32 AvoidRadius = -1;  // < 0: units are ignored
};
