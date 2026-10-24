#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GenTerrain.generated.h"

class FGenMapData;
class UStaticMeshComponent;
class UTexture2D;

/**
 * Terrain renderer for a Generals height map (W3D HeightMapRenderObjClass equivalent).
 *
 * Each height map cell is a quad textured with a quarter of a 64x64 source tile; up to two
 * blend layers (BlendTileData) are drawn over it with per-corner vertex alpha. All tiles of the
 * map's texture classes are packed into one atlas; the material does
 *   lerp(lerp(Atlas(UV0), Atlas(UV1), VertexColor.R), Atlas(UV2), VertexColor.G).
 */
UCLASS()
class GENERALS1_API UGenTerrainComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	bool Build(const FGenMapData& Map);
	/** The terrain shader's cloud and light map stages (TSCloudMed sliding over it, the light map TSNoiseUrb),
	 *  as UseCloudMap / UseLightMap say - no clouds at night. For the terrain's and the roads' materials. */
	static void ApplyCloudAndLightMap(class UMaterialInstanceDynamic* Material, const FGenMapData& Map);
	void Clear();

	/** WorldHeightMap::getTerrainColorAt: the terrain tile at that spot, mipped down to one pixel. */
	FLinearColor GetTerrainColorAt(const FGenMapData& Map, float WorldX, float WorldY) const;

	UTexture2D* GetAtlas() const { return Atlas; }

	/** Whether the cell's two triangles are split along the other diagonal (cell indices, border included). */
	bool IsCellFlipped(const FGenMapData& Map, int32 X, int32 Y) const;

	/** The heights of those cells have changed (a building site was levelled): the chunks they are in are to be made anew. */
	void MarkCellsDirty(const FGenMapData& Map, int32 X0, int32 Y0, int32 X1, int32 Y1);
	/** Makes one of them anew (a millisecond or two each: one a frame). True if there was one. */
	bool RebuildDirtyChunk(const FGenMapData& Map);

private:
	bool BuildAtlas(const FGenMapData& Map);
	/** ReplaceIndex: the chunk it takes the place of (its mesh is made anew), or none. */
	void BuildChunk(const FGenMapData& Map, int32 X0, int32 Y0, int32 X1, int32 Y1, int32 ReplaceIndex = INDEX_NONE);
	TArray<FIntRect> ChunkRects;      // the cells of each chunk (parallel to Chunks)
	TArray<int32> DirtyChunks;

	/** Atlas pixel origin of each source tile (index = global tile index). */
	TArray<FIntPoint> TileOrigins;
	/** Average colour of each source tile. */
	TArray<FColor> TileColors;
	FIntPoint AtlasSize = FIntPoint::ZeroValue;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Atlas;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> Material;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Chunks;
};
