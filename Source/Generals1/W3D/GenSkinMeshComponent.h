#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "PackedNormal.h"
#include "GenSkinMeshComponent.generated.h"

struct FGenSkinData;

/**
 * A W3D skin (an infantryman's body), skinned on the CPU: the triangles, texture coordinates and
 * colours go to the renderer once, and a pose is only new positions and normals written into the
 * buffers that are there.
 *
 * (It was a procedural mesh, which takes every vertex apart and puts it together again on both
 * threads for each update, and has the engine work out its bounds anew: with a crowd on the screen
 * that was most of what the render thread did.)
 */
UCLASS()
class GENERALS1_API UGenSkinMeshComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UGenSkinMeshComponent();

	/** The skin: its sections become the mesh's, their vertices one after the other. Before the component is registered. */
	void SetSkin(const FGenSkinData* InSkin);

	/** Where a pose is written: a position per vertex, and two packed tangents (X, then Z: the normal). */
	TArray<FVector3f>& GetPosePositions() { return PosePositions; }
	TArray<FPackedNormal>& GetPoseTangents() { return PoseTangents; }
	/** What was written goes to the renderer. Box: what the positions take up. */
	void PoseChanged(const FBox3f& Box);

	int32 GetNumVertices() const { return NumVertices; }
	const FGenSkinData* GetSkin() const { return Skin; }

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override;

private:
	const FGenSkinData* Skin = nullptr; // (the asset subsystem's, kept for as long as the program runs)
	int32 NumVertices = 0;
	TArray<FVector3f> PosePositions;    // the last pose: a proxy made later starts from it
	TArray<FPackedNormal> PoseTangents;
	FBox LocalBox = FBox(ForceInit);    // roomy, so that it seldom has to change
};
