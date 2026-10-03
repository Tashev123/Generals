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
	void Clear();

	UTexture2D* GetAtlas() const { return Atlas; }

private:
	bool BuildAtlas(const FGenMapData& Map);
	void BuildChunk(const FGenMapData& Map, int32 X0, int32 Y0, int32 X1, int32 Y1);

	/** Atlas pixel origin of each source tile (index = global tile index). */
	TArray<FIntPoint> TileOrigins;
	FIntPoint AtlasSize = FIntPoint::ZeroValue;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Atlas;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> Material;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Chunks;
};
