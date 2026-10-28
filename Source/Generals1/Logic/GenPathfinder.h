#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"
#include "Map/GenBridgeInfo.h"

class FGenMapData;
class FGenObject;

/**
 * Grid pathfinder (AIPathfind.cpp equivalent, simplified).
 *
 * One cell per height map cell (PATHFIND_CELL_SIZE == MAP_XY_FACTOR). Cells are classified like
 * Pathfinder::classifyMapCell (cliff flags, water polygons) plus structure footprints
 * (classifyObjectFootprint: immobile, non-small structures), and a footprint cell remembers which
 * object put it there (PathfindCellInfo's obstacle id). Paths are 8-connected A* without
 * corner cutting, then smoothed with line-of-sight checks.
 *
 * Bridges are layers, as in the original (PathfindLayer): every bridge has cells of its own over the
 * ground's, and a path steps from the ground onto a bridge and off it only at the cells beyond its two
 * ends. So units cross a bridge on its deck while others pass below it. Layer 0 is the ground, layer
 * n the n-th bridge; wherever a cell is named, the layer comes with it.
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
		Fence,              // an obstacle to all but crushers (PathfindCell::isObstacleFence)
		Impassable,
		BridgeImpassable,   // ground too close under a bridge's deck (CELL_BRIDGE_IMPASSABLE)
		Closed,             // a gap a cell wide beside a structure, shut (the original's CELL_IMPASSABLE: "impassable except for aircraft")
	};

	void Build(const FGenMapData& Map, const TArray<FGenBridgeInfo>& InBridges = TArray<FGenBridgeInfo>());
	/** Called every few hundred cells of a search and of the labelling of regions: the simulation's thread
	 *  lets the game thread have its turn there. (Nothing may search, or change what is passable, from in there.) */
	void SetYield(void (*InYield)(void*), void* InContext) { YieldFn = InYield; YieldContext = InContext; }
	/**
	 * A search touches only what is the pathfinder's own: Begin is called when one starts and, if it
	 * says yes, End when it is over - the simulation's thread steps out of the logic for that long
	 * (FGenSimThread::BeginDetached). Nothing may change what is passable, or search, meanwhile; the
	 * game thread never does either in the middle of a step.
	 */
	void SetDetach(bool (*InBegin)(void*), void (*InEnd)(void*), void* InContext) { BeginSearchFn = InBegin; EndSearchFn = InEnd; SearchContext = InContext; }
	/** Cells searched since the count was last reset (the logic resets it every step: Pathfinder's m_cumulativeCellsAllocated). */
	int32 GetCellsSearched() const { return CellsSearched; }
	void ResetCellsSearched() { CellsSearched = 0; }

	// --- bridges ---
	int32 GetNumLayers() const { return Layers.Num(); }
	/** The bridge of a layer (1..GetNumLayers()), or null. */
	const FGenBridgeInfo* GetBridge(int32 Layer) const;
	/** TerrainLogic::getLayerForDestination: the layer whose surface is nearest to that height at that spot. */
	int32 GetLayerForDestination(const FVector3f& Pos, float GroundZ) const;
	/** TerrainLogic::getHighestLayerForDestination: of the surfaces at or below the point, the nearest ("must be
	 *  ABOVE (or on) the bridge for this call") - so anything even slightly below a deck is on the ground. */
	int32 GetHighestLayerForDestination(const FVector3f& Pos, float GroundZ) const;
	/** TerrainLogic::getLayerHeight */
	float GetLayerHeight(float X, float Y, int32 Layer, float GroundZ) const;
	/** The first bridge deck a ray meets (its layer and the spot), or 0. */
	int32 PickBridge(const FVector3f& Start, const FVector3f& Dir, FVector3f& OutHit) const;

	/** Adds/removes a structure footprint (reference counted per cell, and the cell records which object
	 *  it belongs to). */
	void AddObstacle(const FGenObject& Obj);
	void RemoveObstacle(const FGenObject& Obj);
	/** Pathfinder::classifyFence: the cells under a fence, which only what can crush drives through. */
	void AddFence(const FGenObject& Obj) { RasterizeFence(Obj, +1); }
	void RemoveFence(const FGenObject& Obj) { RasterizeFence(Obj, -1); }

	/** Finds a path in world units. Returns false if the start is invalid; if the goal is unreachable the
	 *  path leads to the closest reachable cell. OutPath excludes the start, ends at the (adjusted) goal.
	 *  OutLayers gets the layer of every point of the path.
	 *  IgnoreObstacleID: the object whose footprint this search may walk onto - the one it is going into
	 *  (Pathfinder::validMovementPosition: "if (toCell->isObstaclePresent( m_ignoreObstacleID )) return true").
	 *  A unit pathing into a building, or into an aircraft parked at its airfield, gets a path across it. */
	bool FindPath(const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces, TArray<FVector2f>& OutPath,
		int32 StartLayer = 0, int32 GoalLayer = 0, TArray<uint8>* OutLayers = nullptr,
		FGenObjectID IgnoreObstacleID = GenInvalidObjectID) const;

	/** Nearest passable position to Pos for the given surfaces (spiral search). */
	bool AdjustDestination(const FVector2f& Pos, uint32 Surfaces, FVector2f& OutPos, int32 MaxRadiusCells = 30) const;

	bool IsPassable(const FVector2f& WorldPos, uint32 Surfaces, int32 Layer = 0) const;
	/** True if every cell under an oriented box (or circle) footprint is open ground: no cliff, water, building or map edge. */
	bool IsAreaClear(const FVector2f& Center, float Angle, float Major, float Minor, bool bBox) const;
	bool IsLineClear(const FVector2f& A, const FVector2f& B, uint32 Surfaces, int32 Layer = 0) const;

	/** PathfindCell::getPinched: open ground hard by a cliff or a structure - passable, at a price. */
	bool IsPinched(int32 X, int32 Y) const;
	/**
	 * Pathfinder::validLocomotorSurfacesForCellType for the cell under a spot: which of a unit's locomotors may
	 * move it there (chooseBestLocomotorForPosition). Beyond the original, the open ground at a cliff's foot and
	 * edge that classifyMap counts as cliff ("Expand all cliff cells one step") also takes a ground locomotor:
	 * a climber walks it rather than climbing flat ground.
	 */
	uint32 LocomotorSurfacesAt(const FVector2f& WorldPos, int32 Layer) const;
	/** Pathfinder::clearCellForDiameter: how wide a body of units (in cells, PathDiameter at most) has room around that cell.
	 *  CrushableLevel: that of a unit, for the ones parked in the way (< 0: no such unit). */
	int32 ClearCellForDiameter(bool bCrusher, int32 X, int32 Y, int32 Layer, int32 PathDiameter, TFunctionRef<int32(FGenObjectID)> CrushableLevel) const;
	/**
	 * Pathfinder::findGroundPath: a way over open ground for a group PathDiameter cells wide; narrower
	 * places cost extra, ground hard by cliffs and structures is not used at all. OutPath: its corners
	 * (Path::optimizeGroundPath), the start first. False: there is none.
	 */
	bool FindGroundPath(const FVector2f& From, const FVector2f& To, int32 PathDiameter, bool bCrusher,
		TFunctionRef<int32(FGenObjectID)> CrushableLevel, TArray<FVector2f>& OutPath, int32 FromLayer = 0, int32 ToLayer = 0) const;

	// --- units (Pathfinder::updatePos / updateGoal / checkDestination / adjustDestination) ---
	// Ground units reserve the cells of their destination, and mark the cells they stand on while not
	// moving, so that other units end their moves on free ground and can path around parked ones.

	/** Marks the cells a unit covers; only stationary units block (UNIT_PRESENT_FIXED). */
	void UpdateUnitPos(FGenObjectID Id, const FVector2f& Pos, float Radius, bool bMoving, int32 Layer = 0);
	/** Reserves the cells around a unit's destination. */
	void UpdateUnitGoal(FGenObjectID Id, const FVector2f& GoalPos, float Radius, int32 Layer = 0);
	/** Forgets a unit entirely (death / removal). */
	void RemoveUnit(FGenObjectID Id);
	/** Pathfinder::removeGoal: gives up the ground a unit had reserved to end its move on. */
	void RemoveUnitGoal(FGenObjectID Id);
	/** True if a unit of that size can end a move at Pos: passable ground, no other unit's goal, nobody parked there.
	 *  Cells that belong to IgnoreObstacleID, and the ones that unit holds, are free for us (checkDestination:
	 *  "if (cell->isObstaclePresent( ignoreId )) continue" / "if (goalUnitID == ignoreId) continue"). */
	bool IsDestinationFree(FGenObjectID Id, const FVector2f& Pos, float Radius, uint32 Surfaces, int32 Layer = 0,
		FGenObjectID IgnoreObstacleID = GenInvalidObjectID) const;
	/** Moves InOutPos to the nearest free destination (outward square spiral, 400 cells as in the original). */
	bool AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos, int32 Layer = 0,
		FGenObjectID IgnoreObstacleID = GenInvalidObjectID) const;
	/** Same, with an extra condition on the spot (adjustTargetDestination: e.g. "within weapon range of the target"). */
	bool AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos, TFunctionRef<bool(const FVector2f&)> Accept, int32 Layer = 0,
		FGenObjectID IgnoreObstacleID = GenInvalidObjectID) const;
	/** FindPath that treats cells held by other stationary units (widened by this unit's size) as blocked. */
	bool FindPathAroundUnits(FGenObjectID Id, float Radius, const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces, TArray<FVector2f>& OutPath,
		int32 StartLayer = 0, int32 GoalLayer = 0, TArray<uint8>* OutLayers = nullptr,
		FGenObjectID IgnoreObstacleID = GenInvalidObjectID) const;
	/** Pathfinder::getRadiusAndCenter: half-size in cells of a unit's footprint, and whether it is centered in a cell. */
	static void GetRadiusAndCenter(float BoundingCircleRadius, int32& OutRadius, bool& bOutCenter);

	/** Which object put the footprint cell at that spot there (-1: none). PathfindCellInfo's obstacle id, which
	 *  findPath compares with m_ignoreObstacleID. */
	FGenObjectID GetFootprintOwner(int32 X, int32 Y) const;
	/** ... and the one under a world position. */
	FGenObjectID GetFootprintOwnerAt(const FVector2f& WorldPos) const { return GetFootprintOwner(WorldToCell(WorldPos).X, WorldToCell(WorldPos).Y); }

	ECell GetCell(int32 X, int32 Y) const;
	int32 GetWidth() const { return Width; }
	int32 GetHeight() const { return Height; }
	FIntPoint WorldToCell(const FVector2f& P) const;
	FVector2f CellCenter(int32 X, int32 Y) const;

private:
	/** A bridge's own cells (PathfindLayer): a block around the bridge, indexed like the ground's. */
	struct FLayer
	{
		FGenBridgeInfo Bridge;
		int32 XOrigin = 0, YOrigin = 0, W = 0, H = 0;
		int32 NodeBase = 0;        // where its cells start among the nodes (the ground's come first)
		TArray<uint8> Type;        // 0: not part of the bridge, 1: deck, 2: on the bridge but closed (its sides)
		TArray<uint8> Connect;     // 1: the ground can be stepped onto from here, and this cell from the ground
	};
	/** A unit's block of cells on a layer. */
	struct FUnitCells
	{
		FIntRect Rect;             // cells [Min, Max)
		int32 Layer = 0;
		bool operator==(const FUnitCells& O) const { return Rect == O.Rect && Layer == O.Layer; }
		bool operator!=(const FUnitCells& O) const { return !(*this == O); }
	};

	bool CanPass(int32 X, int32 Y, uint32 Surfaces, int32 Layer = 0) const;
	/** The node of a cell: the ground's own index, or a place among a bridge's cells. -1: there is no such cell. */
	int32 NodeIndex(int32 Layer, int32 X, int32 Y) const;
	void NodeToCell(int32 Node, int32& OutLayer, int32& OutX, int32& OutY) const;
	void BuildLayer(const FGenMapData& Map, const FGenBridgeInfo& Bridge);
	/** Returns the block of cells it touched (Max below Min: none). */
	FIntRect RasterizeFootprint(const FGenObject& Obj, int32 Delta);
	/** The ground two cells around a footprint that went in or out: gaps shut or opened again, ground beside it pinched. */
	void ReclassifyAround(FIntRect Cells, bool bInsert);
	bool IsGroundLineClear(const FVector2f& A, const FVector2f& B, int32 Layer, int32 PathDiameter, bool bCrusher, TFunctionRef<int32(FGenObjectID)> CrushableLevel) const;
	void RasterizeFence(const FGenObject& Obj, int32 Delta);
	FIntRect UnitRect(const FVector2f& Pos, float Radius) const;   // cells [Min, Max)
	void Stamp(TArray<FGenObjectID>& Cells, const FUnitCells& Where, FGenObjectID From, FGenObjectID To);

	void (*YieldFn)(void*) = nullptr;
	void* YieldContext = nullptr;
	mutable int32 CellsSearched = 0;
	bool (*BeginSearchFn)(void*) = nullptr;
	void (*EndSearchFn)(void*) = nullptr;
	void* SearchContext = nullptr;
	struct FDetachedSearch
	{
		const FGenPathfinder& Owner;
		bool bLeft;
		explicit FDetachedSearch(const FGenPathfinder& In) : Owner(In), bLeft(In.BeginSearchFn ? In.BeginSearchFn(In.SearchContext) : false) {}
		~FDetachedSearch() { if (bLeft && Owner.EndSearchFn) { Owner.EndSearchFn(Owner.SearchContext); } }
	};
	/** Holds the object whose footprint one search or destination test may cross, and gives back what was
	 *  held before it. */
	struct FIgnoreObstacle
	{
		const FGenPathfinder& Owner;
		FGenObjectID Saved;
		FIgnoreObstacle(const FGenPathfinder& In, FGenObjectID Id) : Owner(In), Saved(In.ObstacleToIgnore) { In.ObstacleToIgnore = Id; }
		~FIgnoreObstacle() { Owner.ObstacleToIgnore = Saved; }
	};

	int32 Width = 0;
	int32 Height = 0;
	int32 Border = 0;
	TArray<ECell> Terrain;     // static classification
	TArray<float> CellZ;       // the ground's height at each cell's corner (what examineNeighboringCells compares on a cliff)
	TArray<uint16> Obstacles;  // footprint reference counts
	TArray<uint8> Fences;      // fences across the cell
	/** Which object put a footprint cell there (-1: none). PathfindCellInfo's obstacle id. */
	TArray<FGenObjectID> ObstacleOwner;
	enum : uint8
	{
		FlagGrownCliff = 1,    // open ground at a cliff's foot or edge, counted as cliff
		FlagCliffPinch = 2,    // the cell after that
		FlagObstaclePinch = 4, // open ground right beside a structure
		FlagClosed = 8,        // open ground with too little open ground around it, beside a structure
	};
	TArray<uint8> CellFlags;

	TArray<FLayer> Layers;           // [layer - 1]
	TArray<uint8> ConnectLayer;      // per ground cell: the layer that is stepped onto there (0: none)
	int32 NumNodes = 0;              // ground cells plus every layer's

	TArray<FGenObjectID> GoalUnit;   // per node: unit that reserved it as (part of) its destination
	TArray<FGenObjectID> FixedUnit;  // per node: unit standing still on it
	TMap<FGenObjectID, FUnitCells> GoalRects;
	TMap<FGenObjectID, FUnitCells> FixedRects;

	// Set only for the duration of FindPathAroundUnits.
	mutable FGenObjectID AvoidSelf = GenInvalidObjectID;
	mutable int32 AvoidRadius = -1;  // < 0: units are ignored

	// Set only for the duration of one search or destination test: Pathfinder's m_ignoreObstacleID, the
	// object whose footprint may be crossed - the one the unit is going into.
	mutable FGenObjectID ObstacleToIgnore = GenInvalidObjectID;

	// --- what a search works with; kept from one search to the next ---
	// Nothing is cleared between searches: a node's cost and parent count only if it carries the stamp
	// of the search under way. (On the largest maps clearing them cost more than most searches did.)
	struct FOpenNode { int32 F; int32 Index; };
	mutable TArray<uint32> SeenStamp;    // per node: the search that last gave it a cost
	mutable TArray<uint32> ClosedStamp;  // per node: the search that last closed it
	mutable TArray<int32> GCost;
	mutable TArray<int32> Parent;
	mutable TArray<FOpenNode> Open;
	mutable uint32 SearchStamp = 0;

	/**
	 * Which nodes can be reached from which (the original's PathfindZoneManager): two nodes with the
	 * same label are joined by a way on those surfaces, units aside. A goal on another label can't be
	 * reached, and is known not to be without searching the whole map for it.
	 */
	struct FRegions
	{
		uint32 Version = 0;
		TArray<int32> Label;             // per node: region (-1: closed)
	};
	const TArray<int32>& GetRegions(uint32 Surfaces) const;
	mutable TMap<uint32, FRegions> Regions;  // by surfaces
	mutable TArray<int32> RegionQueue;
	uint32 ObstacleVersion = 1;          // goes up whenever a building's footprint is added or taken away
};
