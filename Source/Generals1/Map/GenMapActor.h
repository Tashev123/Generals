#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GenMapActor.generated.h"

class FGenMapData;
class UGenTerrainComponent;
class UGenModelComponent;
class UDirectionalLightComponent;
class UPostProcessComponent;
class UExponentialHeightFogComponent;

/**
 * Loads a Generals map from the original archives: terrain, map lighting and the
 * map's placed objects (rendered from their INI draw modules). Usable in the editor
 * (Load Map button) and at runtime.
 */
UCLASS()
class GENERALS1_API AGenMapActor : public AActor
{
	GENERATED_BODY()

public:
	AGenMapActor();

	/** Map folder name under Maps\ (e.g. "Tournament Desert"). */
	UPROPERTY(EditAnywhere, Category = "Generals")
	FString MapName = TEXT("Tournament Desert");

	UPROPERTY(EditAnywhere, Category = "Generals")
	bool bSpawnMapObjects = true;

	UFUNCTION(CallInEditor, Category = "Generals")
	void LoadMap();

	UFUNCTION(CallInEditor, Category = "Generals")
	void ClearMap();

	bool LoadMapByName(const FString& Name);

	const FGenMapData* GetMapData() const { return Map.Get(); }

	/** Waypoint position in Unreal space (on the ground). */
	bool GetWaypointLocation(const FString& WaypointName, FVector& OutLocation) const;

	/** Ground height (Unreal Z) at an Unreal XY location. */
	float GetGroundZ(const FVector& UELocation) const;

	/** Playable area in Unreal space. */
	FBox GetPlayableBoundsUE() const;

	/** Ray vs. height map (Unreal space). */
	bool RaycastTerrain(const FVector& Start, const FVector& Dir, FVector& OutHit) const;

	/**
	 * Creates the visual for an object template at a Generals world position (z relative to ground),
	 * using its primary draw module's default condition state. Returns null if it has no model.
	 */
	UGenModelComponent* SpawnObjectVisual(const struct FGenThingTemplate& Template, const FVector3f& GenLocation, float Angle);

	/** Creates an unplaced visual (attached to this actor) for a template in its default state. */
	UGenModelComponent* CreateObjectVisual(const struct FGenThingTemplate& Template, const FLinearColor& HouseColor);

	/** Creates an empty model attached to another object visual (an object's additional draw modules). */
	UGenModelComponent* CreateAttachedVisual(UGenModelComponent* Parent, const FLinearColor& HouseColor);

	/** Creates an unplaced visual showing a bare W3D model (debris chunks). */
	UGenModelComponent* CreateModelVisual(const FString& Model, const FLinearColor& HouseColor);

protected:
	virtual void BeginDestroy() override;

private:
	void SetupLighting();
	void SpawnMapObjects();

	TSharedPtr<FGenMapData> Map;

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UGenTerrainComponent> Terrain;

	/** Map global light 0 (casts shadows) and the two optional fill lights. */
	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UDirectionalLightComponent> Sun;

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UDirectionalLightComponent> Fill1;

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UDirectionalLightComponent> Fill2;

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UExponentialHeightFogComponent> Fog;

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UPostProcessComponent> PostProcess;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UGenModelComponent>> ObjectModels;
};
