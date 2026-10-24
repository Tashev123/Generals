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

	// Two-pass "W3D Reflectivity" meshes: Texture is alpha-blended over an opaque first pass, which
	// therefore shows wherever the texture's alpha is below 1 (windows, glass).
	bool bUnderlay = false;
	FString UnderTexture;            // environment map of that first pass ("" = plain color)
	FLinearColor UnderTint = FLinearColor::White;

	// LinearOffsetTextureMapperClass on stage 0: 1 the texture slides along with time (UVPerSec, from UVStart);
	// 2..4 a tank's left / right / middle treads, which its draw module rolls instead (custom primitive data
	// 5..7, UGenModelComponent::SetTreadOffsets). Either way the coordinates are scaled by UVScale first.
	uint8 ScrollMode = 0;
	FVector2f UVPerSec = FVector2f::ZeroVector;
	FVector2f UVStart = FVector2f::ZeroVector;
	FVector2f UVScale = FVector2f(1.f, 1.f);
};

/**
 * What is known so far about a model's parts being moved or hidden - by a draw module's turret and
 * recoil bones and its lists of things to show and hide, by an animation's channels, or by a pose
 * seen to have left the base pose. Parts none of this touches can be drawn as one (FGenModelPlan).
 */
struct FGenModelMergeInfo
{
	TSet<FString> ToggledNames;         // sub-objects shown or hidden by name (lower case)
	TSet<FString> BonePrefixes;         // bones (or sub-objects) turned, shifted or hidden by name or numbered prefix (lower case)
	TSet<int32> MovingPivots;           // pivots an animation has channels for, or that were seen away from the base pose
	TSet<FString> SeenAnimations;
	TArray<FString> PossibleAnimations; // "HIER.ANIM": what a draw module may play on the model ...
	int32 PossibleLooked = 0;           // ... and how many of those have been read for the bones they move
	uint32 Version = 1;                 // goes up whenever something is added
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
 * How a model is put together for drawing. A W3D model is a dozen or two meshes, each on a bone of its
 * own; drawn as they come that is a dozen or two things for the engine to cull, light, shadow and - when
 * the model moves - carry to its render thread, for every building and vehicle on the map. Those of
 * them that never move against the model and are never hidden are one mesh here, in model space.
 */
struct FGenModelPlan
{
	struct FPart
	{
		int32 SubIndex = 0;         // among the model's sub-objects
		int32 Bone = 0;
		FString Name;               // without the container
	};
	uint32 InfoVersion = 0;         // the FGenModelMergeInfo it was made from
	TArray<FPart> Merged;           // the meshes that are one (none, or two and more)
	TArray<bool> bIsMerged;         // per sub-object of the model
	FGenBuiltMesh MergedMesh;
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
	 * Drawable::getPristineBonePositions: bones of a model in its base pose (model space, W3D frame),
	 * untouched by animation. StartIndex 0 looks up the bone called exactly Prefix; otherwise
	 * "<Prefix>NN" from NN = StartIndex until one is missing (or MaxBones). Returns how many were found.
	 */
	int32 GetPristineBones(const FString& ModelName, const FString& Prefix, int32 StartIndex, int32 MaxBones, TArray<FTransform3f>& OutBones,
		const FString& Animation = FString(), bool bLastFrame = false);
	/**
	 * One bone of a model in its base pose (model space, W3D frame), with one bone on the way to it - a
	 * turret - turned about its own up axis by Angle. BoneName as the draw modules give it: "Muzzle"
	 * finds "Muzzle01" first. (Safe to call from the simulation's thread, like GetPristineBones.)
	 */
	bool GetPristineBoneTurned(const FString& ModelName, const FString& BoneName, const FString& TurnedBone, float Angle, FTransform3f& OutBone,
		const FString& Animation = FString(), bool bLastFrame = false);
	// (Animation: ModelConditionInfo::validateCachedBones - "if we have any animations in this state, always
	// choose the first" and "make sure we're in frame zero", or its last frame with
	// PRISTINE_BONE_POS_IN_FINAL_FRAME: a launcher's arms are where that pose has them, not where the model's own has.)

	/**
	 * Builds (or returns cached) render mesh for one W3D mesh. Skins are baked into model space
	 * using the hierarchy's base pose, since they cannot bend as static meshes.
	 */
	const FGenBuiltMesh* GetBuiltMesh(const FString& FileName, const FGenW3DMesh& Mesh, const FGenW3DHierarchy* Hierarchy);

	/** The model's sub-objects as it is drawn (the HLOD's, or every mesh of the file on the root bone), and its hierarchy. */
	void GetModelSubObjects(const FGenW3DFile& File, TArray<FGenW3DSubObject>& OutSubs, TSharedPtr<const FGenW3DHierarchy>& OutHierarchy);
	/** Which of the model's rigid meshes are drawn as one, by what is known now (made anew when more is known). */
	const FGenModelPlan& GetModelPlan(const FString& ModelName);
	uint32 GetMergeInfoVersion(const FString& ModelName) const;
	// What gets moved or hidden (each makes the plan out of date if it is news):
	void NoteToggledSubObject(const FString& ModelName, const FString& SubObjectName);
	void NoteMovedBonePrefix(const FString& ModelName, const FString& Prefix);
	void NoteMovedPivots(const FString& ModelName, TConstArrayView<int32> Pivots);
	void NoteAnimation(const FString& ModelName, const FGenW3DAnimation& Animation);
	/** An animation that may be played on the model: read for the bones it moves when the model's plan is made. */
	void NotePossibleAnimation(const FString& ModelName, const FString& QualifiedName);

	/** Skinning data for a W3D skin mesh (cached). */
	const FGenSkinData* GetSkinData(const FString& FileName, const FGenW3DMesh& Mesh);

	UTexture2D* GetTexture(const FString& TextureName);

	/** Material instance for a section. House-colored sections get a per-color instance. */
	/** bFadeable: a variant whose opaque sections go through the masked material, so the model can fade (stealth). */
	UMaterialInterface* GetSectionMaterial(const FGenSectionMaterial& Section, const FLinearColor& HouseColor, bool bFadeable = false);

	/**
	 * Preloading (the original's KINDOF_PRELOAD): a model's file, meshes and textures, or an animation,
	 * made ready before anything shows them - so that the first tank of its kind rolling out of a factory
	 * doesn't hold up a frame while it is read and built. Queued here, and worked off a little every
	 * frame by TickPreload.
	 */
	void QueuePreloadModel(const FString& ModelName);
	void QueuePreloadAnimation(const FString& QualifiedName);
	/** Works at the queue for about that long (a piece once begun is finished). Returns how many pieces are left. */
	int32 TickPreload(double BudgetSeconds);
	int32 GetPreloadPending() const { return PreloadQueue.Num() - PreloadHead; }
	/** (gen.AssetMemory) What the assets read so far take up, by kind. */
	FString DescribeMemory() const;

	/** Base materials authored in the editor (created by Tools/ue_setup.py). */
	UMaterialInterface* GetBaseMaterial(EGenW3DBlend Blend, bool bTwoSided);
	UMaterialInterface* GetTerrainMaterial();
	/** One of the materials Tools/ue_setup.py makes, by name ("M_Water"). */
	UMaterialInterface* GetNamedMaterial(const TCHAR* Name);

private:
	TMap<FString, TSharedPtr<FGenW3DFile>> W3DFiles;     // key: lowercase name
	TSet<FString> MissingW3D;
	TMap<FString, FGenBuiltMesh> BuiltMeshes;            // key: file|mesh
	TMap<FString, TSharedPtr<FGenSkinData>> SkinData;    // key: file|mesh
	TMap<FString, FGenModelMergeInfo> MergeInfos;        // key: lower-case model name
	TMap<FString, TUniquePtr<FGenModelPlan>> ModelPlans;
	/** The rigid meshes nothing known moves or hides. bFinal: without those drawn blended, too (which takes reading their materials). */
	void FindMergeParts(const FGenW3DFile& File, const FGenW3DHierarchy* Hierarchy, const TArray<FGenW3DSubObject>& Subs, const FGenModelMergeInfo& Info,
		bool bFinal, TArray<FGenModelPlan::FPart>& OutParts) const;
	bool GetMergedMesh(const FString& FileName, const FGenW3DFile& File, const FGenW3DHierarchy* Hierarchy, const TArray<FGenW3DSubObject>& Subs,
		const TArray<FGenModelPlan::FPart>& Parts, FGenBuiltMesh& Out);
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UTexture2D>> Textures;
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> Materials;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMesh>> MeshRefs;
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UMaterialInterface>> BaseMaterials;
	TSet<FString> MissingTextures;
	/** The W3D files read so far are asked for by the game thread (models, animations) and by the
	 *  simulation (bones of base poses) at once. */
	FCriticalSection W3DLock;
	struct FPreloadStep
	{
		uint8 Kind = 0;             // 0: a model's file, 1: one of its sub-objects (mesh and textures), 2: an animation, 3: a model's merged mesh
		FString Name;
		int32 Index = 0;
	};
	TArray<FPreloadStep> PreloadQueue;
	int32 PreloadHead = 0;
	TSet<FString> PreloadQueued;    // what has been asked for before (this session)
};
