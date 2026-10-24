#pragma once

#include "CoreMinimal.h"

/** Dict value from the map file (Common/Dict.h). */
struct GENERALS1_API FGenDictValue
{
	enum class EType : uint8 { Bool, Int, Real, AsciiString, UnicodeString };
	EType Type = EType::Int;
	bool Bool = false;
	int32 Int = 0;
	float Real = 0.f;
	FString String;
};

using FGenDict = TMap<FString, FGenDictValue>;

struct GENERALS1_API FGenMapObject
{
	FVector3f Location = FVector3f::ZeroVector; // Generals world units; z is relative to ground
	float Angle = 0.f;                            // radians
	int32 Flags = 0;
	FString TemplateName;
	FGenDict Properties;

	enum : int32
	{
		FlagRoadPoint1 = 0x2, FlagRoadPoint2 = 0x4, FlagRoadFlags = 0x6,
		FlagBridgePoint1 = 0x10, FlagBridgePoint2 = 0x20, FlagBridgeFlags = 0x30,
		FlagDontRender = 0x100,
	};

	FString GetString(const TCHAR* Key) const;
	int32 GetInt(const TCHAR* Key, int32 Default = 0) const;
	bool Has(const TCHAR* Key) const { return Properties.Contains(Key); }
	bool IsWaypoint() const { return Properties.Contains(TEXT("waypointID")); }
};

struct GENERALS1_API FGenTextureClass
{
	int32 FirstTile = 0;
	int32 NumTiles = 0;
	int32 Width = 0;
	FString Name;
};

struct GENERALS1_API FGenBlendedTile
{
	int32 BlendNdx = 0;
	uint8 Horiz = 0, Vert = 0, RightDiagonal = 0, LeftDiagonal = 0, Inverted = 0, LongDiagonal = 0;
	int32 CustomBlendEdgeClass = -1;
};

struct GENERALS1_API FGenMapLight
{
	FLinearColor Ambient = FLinearColor::Black;
	FLinearColor Diffuse = FLinearColor::White;
	FVector3f Direction = FVector3f(0, 0, -1);
};

struct GENERALS1_API FGenMapLighting
{
	TArray<FGenMapLight> Terrain;   // up to 3
	TArray<FGenMapLight> Objects;   // up to 3
};

struct GENERALS1_API FGenMapSide
{
	FGenDict Properties;
	struct FBuild
	{
		FString BuildingName;
		FString TemplateName;
		FVector3f Location;
		float Angle = 0.f;
		bool bInitiallyBuilt = false;
		int32 NumRebuilds = 0;
	};
	TArray<FBuild> BuildList;
};

/** PolygonTriggers chunk entry (water areas, script areas). */
struct GENERALS1_API FGenPolygonTrigger
{
	FString Name;
	int32 ID = 0;
	bool bWater = false;
	bool bRiver = false;
	int32 RiverStart = 0;        // the point after which the river's far end lies (PolygonTrigger::getRiverStart)
	TArray<FIntVector> Points;   // world units

	bool Contains(float X, float Y) const;
	float GetWaterHeight() const { return Points.Num() > 0 ? float(Points[0].Z) : 0.f; }
};

/** Parsed .map file (CkMp data chunks). */
class GENERALS1_API FGenMapData
{
public:
	bool LoadFromVfs(const FString& MapPath);
	bool LoadFromMemory(TConstArrayView<uint8> Data);

	// HeightMapData
	int32 Width = 0;
	int32 Height = 0;
	int32 Border = 0;
	TArray<FIntPoint> Boundaries;
	TArray<uint8> Heights;

	// BlendTileData
	TArray<int16> TileNdx;
	TArray<int16> BlendNdx;
	TArray<int16> ExtraBlendNdx;
	TArray<int16> CliffNdx;
	TArray<FGenTextureClass> TextureClasses;
	TArray<FGenTextureClass> EdgeTextureClasses;
	TArray<FGenBlendedTile> BlendedTiles;   // [0] unused
	TArray<uint8> CliffState;               // 1 bit per cell, row stride CliffStride
	int32 CliffStride = 0;

	// Other chunks
	TArray<FGenPolygonTrigger> PolygonTriggers;
	TArray<FGenMapObject> Objects;
	FGenDict WorldInfo;
	TArray<FGenMapSide> Sides;
	TArray<FGenDict> Teams;
	int32 TimeOfDay = 1;
	TArray<FGenMapLighting> Lighting;       // [0..3] = MORNING, AFTERNOON, EVENING, NIGHT

	FString Path;

	/** Raw height sample in world units at a height map vertex (clamped). */
	float GetHeightAt(int32 X, int32 Y) const;
	/** TerrainVisual::setRawMapHeight: a height map vertex (array indices, border included) is lowered to that raw
	 *  height if it stands higher - never raised. True if it was. */
	bool LowerRawHeight(int32 X, int32 Y, int32 RawHeight);

	/** Ground height at a Generals world position (bilinear over the height map). */
	float GetGroundHeight(float WorldX, float WorldY) const;
	/** The ground's (smoothed) up direction there, as getHeightMapHeight gives it. */
	FVector3f GetGroundNormal(float WorldX, float WorldY) const;

	/** Playable area in world units (inside the border). */
	FBox2f GetPlayableBounds() const;

	const FGenMapObject* FindWaypoint(const FString& Name) const;
	const FGenMapLighting* GetCurrentLighting() const;

	/** WaypointsList: directed links (from waypoint ID, to waypoint ID) that chain waypoints into paths. */
	TArray<FIntPoint> WaypointLinks;

	const FGenPolygonTrigger* FindTrigger(const FString& Name) const;
	const FGenMapObject* FindWaypointByID(int32 Id) const;
	/** TerrainLogic::getClosestWaypointOnPath: nearest waypoint carrying that path label (case-insensitive), or null. */
	const FGenMapObject* GetClosestWaypointOnPath(const FVector2f& Pos, const FString& Label) const;
	/** The positions of a waypoint and of those its links lead to, in order (ground height not applied). */
	void GetWaypointPath(const FGenMapObject& Start, TArray<FVector2f>& OutPath) const;

	/** Cliff flag of a height map cell (array indices, border included). */
	bool IsCliffCellIndex(int32 X, int32 Y) const;

	/** TerrainLogic::isUnderwater at a world position. */
	bool IsUnderwater(float WorldX, float WorldY, float* OutWaterZ = nullptr) const;

private:
	void InitCliffFlagsFromHeights();
};
