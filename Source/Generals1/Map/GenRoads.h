#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GenRoads.generated.h"

class FGenMapData;
class UGenTerrainComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** A "Road" of Roads.ini (TerrainRoadType). */
struct GENERALS1_API FGenRoadType
{
	FString Name;
	int32 Id = 0;                  // TerrainRoadCollection's counter: roads and bridges share it, from 1
	FString Texture;
	float Width = 0.f;             // RoadWidth: world units
	float WidthInTexture = 0.f;    // RoadWidthInTexture: how much of that the paved part takes
};

/** A "Bridge" of Roads.ini. */
struct GENERALS1_API FGenBridgeType
{
	FString Name;
	int32 Id = 0;
	float Scale = 1.f;             // BridgeScale
	FColor RadarColor = FColor(192, 192, 192);
	FString Model;                 // BridgeModelName
	FString Texture;
	FString ModelDamaged, TextureDamaged;
	FString ModelReallyDamaged, TextureReallyDamaged;
	FString ModelBroken, TextureBroken;
	FString TowerFromLeft, TowerFromRight, TowerToLeft, TowerToRight;
};

/** Roads.ini (GameClient/Terrain/TerrainRoads.cpp). */
class GENERALS1_API FGenTerrainRoads
{
public:
	static const FGenTerrainRoads& Get();

	/** In the order of the file. */
	const TArray<FGenRoadType>& GetRoads() const { return Roads; }
	const TArray<FGenBridgeType>& GetBridges() const { return Bridges; }
	const FGenRoadType* FindRoad(const FString& Name) const;
	const FGenBridgeType* FindBridge(const FString& Name) const;

private:
	void Load();

	TArray<FGenRoadType> Roads;
	TArray<FGenBridgeType> Bridges;
	bool bLoaded = false;
};

/**
 * The roads of a map (W3DRoadBuffer.cpp).
 *
 * A map holds roads as pairs of points. The original chains them up, rounds or mitres the corners,
 * puts T-, Y- and four-way pieces where three or four of a kind meet and a fading end piece where a road
 * is marked as joining another kind; every piece is a quad with a part of the road type's texture on it.
 * All of that is done here as there, to the letter - it decides what the roads look like.
 *
 * What differs is how a piece gets onto the ground. The original hangs it a little above the highest
 * terrain under it and draws the pieces in order, blending, without writing depth. Here each piece is
 * cut along the terrain's own triangles and laid on them, and pieces that lie over one another are
 * stacked a hair apart in the order the original draws them - so the roads are lit and shadowed with
 * the terrain, and nothing floats or flickers.
 */
UCLASS()
class GENERALS1_API UGenRoadComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	void Build(const FGenMapData& Map, const UGenTerrainComponent* Terrain);
	void Clear();

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Meshes;
};
