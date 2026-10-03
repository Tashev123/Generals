#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "W3D/GenW3D.h"
#include "GenAssets.generated.h"

class UStaticMesh;
class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** How a W3D mesh section is shaded (one per distinct shader/texture/vertex material combination). */
struct FGenSectionMaterial
{
	FString Texture;                 // W3D texture name ("" = untextured)
	EGenW3DBlend Blend = EGenW3DBlend::Opaque;
	bool bTwoSided = false;
	FLinearColor Diffuse = FLinearColor::White;
	FLinearColor Emissive = FLinearColor::Black;
	float Opacity = 1.f;
	uint8 HouseColorMode = 0;        // 0 none, 1 HOUSECOLOR mesh (tint), 2 ZHC texture (hue shift)
};

/** A W3D mesh converted to a runtime UStaticMesh, positioned in its bone's local space (Unreal units). */
struct FGenBuiltMesh
{
	TObjectPtr<UStaticMesh> Mesh = nullptr;
	TArray<FGenSectionMaterial> Sections;
	bool bSkinBakedToModelSpace = false;
};

/**
 * CPU-skinning source data for a W3D skin mesh. Each vertex is rigidly bound to one bone and stored
 * in that bone's space (W3D convention), so skinning is one transform per vertex.
 */
struct FGenSkinData
{
	struct FSection
	{
		FGenSectionMaterial Material;
		TArray<FVector3f> LocalPositions;   // W3D bone space
		TArray<FVector3f> LocalNormals;
		TArray<uint16> Bones;
		TArray<int32> Triangles;            // section-local indices
		TArray<FVector2D> UVs;
		TArray<FLinearColor> Colors;
	};
	TArray<FSection> Sections;
};

/**
 * Engine-wide cache of assets created from the original game data at runtime:
 * W3D files, built meshes, textures and material instances. Works in the editor too.
 */
UCLASS()
class GENERALS1_API UGenAssetSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	static UGenAssetSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Loads Art\W3D\<Name>.w3d (cached). Name is case-insensitive, without extension. */
	TSharedPtr<const FGenW3DFile> LoadW3D(const FString& Name);

	/** Resolves "HIERARCHY.ANIM" (the anim lives in ANIM.w3d, or in the model file of the same name). */
	TSharedPtr<const FGenW3DAnimation> FindAnimation(const FString& QualifiedName);

	/** Hierarchy for an HLOD: in the same file, or in <HierarchyName>.w3d. */
	TSharedPtr<const FGenW3DHierarchy> FindHierarchy(const FString& HierarchyName, const FGenW3DFile* SameFile = nullptr);

	/**
	 * Builds (or returns cached) render mesh for one W3D mesh. Skins are baked into model space
	 * using the hierarchy's base pose, since they cannot bend as static meshes.
	 */
	const FGenBuiltMesh* GetBuiltMesh(const FString& FileName, const FGenW3DMesh& Mesh, const FGenW3DHierarchy* Hierarchy);

	/** Skinning data for a W3D skin mesh (cached). */
	const FGenSkinData* GetSkinData(const FString& FileName, const FGenW3DMesh& Mesh);

	UTexture2D* GetTexture(const FString& TextureName);

	/** Material instance for a section. House-colored sections get a per-color instance. */
	UMaterialInterface* GetSectionMaterial(const FGenSectionMaterial& Section, const FLinearColor& HouseColor);

	/** Base materials authored in the editor (created by Tools/ue_setup.py). */
	UMaterialInterface* GetBaseMaterial(EGenW3DBlend Blend, bool bTwoSided);
	UMaterialInterface* GetTerrainMaterial();

private:
	TMap<FString, TSharedPtr<FGenW3DFile>> W3DFiles;     // key: lowercase name
	TSet<FString> MissingW3D;
	TMap<FString, FGenBuiltMesh> BuiltMeshes;            // key: file|mesh
	TMap<FString, TSharedPtr<FGenSkinData>> SkinData;    // key: file|mesh
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UTexture2D>> Textures;
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> Materials;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMesh>> MeshRefs;
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UMaterialInterface>> BaseMaterials;
	TSet<FString> MissingTextures;
};
