#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Map/GenBridgeInfo.h"
#include "GenBridges.generated.h"

class FGenMapData;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/**
 * The bridges of a map (W3DBridgeBuffer.cpp).
 *
 * A map holds a bridge as two points; its kind (Roads.ini) names a model of three parts - a left end,
 * a span and a right end. The left end goes on the first point, the right end on the second, and as
 * many spans as fit come in between, the whole stretched a little so that it ends exactly on the points.
 * The model's width also gives the deck's rectangle that the game logic uses (GetInfos).
 *
 * Bridges are drawn in their pristine state only: they cannot be damaged yet.
 */
UCLASS()
class GENERALS1_API UGenBridgeComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	void Build(const FGenMapData& Map);
	void Clear();

	const TArray<FGenBridgeInfo>& GetInfos() const { return Infos; }

private:
	TArray<FGenBridgeInfo> Infos;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Meshes;
};
