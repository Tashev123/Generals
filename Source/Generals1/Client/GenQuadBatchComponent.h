#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "GenQuadBatchComponent.generated.h"

class FMaterialRenderProxy;
class UMaterialInterface;

/** A run of quads drawn with one material. */
struct FGenQuadDraw
{
	FMaterialRenderProxy* Material = nullptr;
	int32 FirstQuad = 0;
	int32 NumQuads = 0;
};

/** What is drawn this frame: four vertices per quad (world space), and which quads go with which material. */
struct FGenQuadFrame
{
	TArray<FVector3f> Positions;
	TArray<FVector2f> UVs;
	TArray<FColor> Colors;
	TArray<FGenQuadDraw> Draws;

	void Reset()
	{
		Positions.Reset();
		UVs.Reset();
		Colors.Reset();
		Draws.Reset();
	}
};

/**
 * All the particles of one kind of blending, as one thing to draw: a vertex buffer that is filled once
 * a frame and a draw per texture out of it, in the order they are given.
 *
 * (They used to be a procedural mesh per texture, each of which the engine re-sent on its own every
 * frame - a few dozen small uploads that cost more than everything they drew.)
 */
UCLASS()
class GENERALS1_API UGenQuadBatchComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UGenQuadBatchComponent();

	/**
	 * A material that draws will use. All of a component's materials must be instances of one and the
	 * same material: the first says how the whole of it is treated (which pass, which blending).
	 */
	void AddMaterial(UMaterialInterface* Material);

	/** The frame's quads (taken over: Frame is left empty). */
	void SetFrame(FGenQuadFrame& Frame);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	virtual int32 GetNumMaterials() const override { return UsedMaterials.Num(); }
	virtual UMaterialInterface* GetMaterial(int32 Index) const override;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> UsedMaterials;

	bool bSentQuads = false; // the render thread still has quads of ours to draw
};
