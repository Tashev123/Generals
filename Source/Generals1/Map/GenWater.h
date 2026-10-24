#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GenWater.generated.h"

class FGenMapData;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
struct FGenPolygonTrigger;

/** A WaterSet of Water.ini: how water looks at one time of day. */
struct GENERALS1_API FGenWaterSet
{
	FString SkyTexture;
	FString WaterTexture;
	FLinearColor DiffuseColor = FLinearColor::White;   // 0..1, as written (gamma space)
};

/**
 * Water.ini (GameClient/Water.cpp): the four WaterSets and the WaterTransparency block. A map's own
 * map.ini may replace the latter (greener, murkier, shallower water), so the settings are read anew
 * for every map.
 */
struct GENERALS1_API FGenWaterSettings
{
	FGenWaterSet Sets[4];                               // MORNING, AFTERNOON, EVENING, NIGHT
	float MinOpacity = 1.f;                             // TransparentWaterMinOpacity: opacity at depth
	float TransparentDepth = 3.f;                       // TransparentWaterDepth: the depth at which that is reached
	FLinearColor StandingWaterColor = FLinearColor::White; // white: lit like the terrain; anything else: this colour, unlit
	FString StandingWaterTexture = TEXT("TWWater01.tga");
	bool bAdditive = false;
	FColor RadarWaterColor = FColor(140, 140, 255);

	static const FGenWaterSettings& Get();
	/** Water.ini (with its Default\ version), then the map's map.ini. */
	static void LoadForMap(const FString& MapFolder);
};

/**
 * The water of a map (W3DWater.cpp's WaterRenderObjClass::renderWater): every water area of the
 * map's polygon triggers, as standing water or as a river.
 *
 * Standing water is drawn on the terrain's own grid: a flat sheet at the area's height whose opacity
 * at each terrain vertex is the depth there over TransparentWaterDepth - what the original gets by
 * writing that value into the frame buffer's alpha with the shoreline tiles and blending the water
 * by it. So the shore shows through shallow water and deep water has the map's full opacity.
 * A river is a ribbon between its two banks, its texture running along it.
 */
UCLASS()
class GENERALS1_API UGenWaterComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	void Build(const FGenMapData& Map);
	void Clear();

	int32 GetNumWaterCells() const { return NumWaterCells; }

private:
	void BuildStanding(const FGenMapData& Map);
	void BuildRiver(const FGenMapData& Map, const FGenPolygonTrigger& River);
	UMaterialInstanceDynamic* MakeMaterial(const TCHAR* MaterialName, const FGenMapData& Map);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Meshes;

	int32 NumWaterCells = 0;
};
