#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "W3D/GenAssets.h"
#include "W3D/GenW3D.h"
#include "GenModelComponent.generated.h"

class UMeshComponent;

UENUM()
enum class EGenAnimMode : uint8
{
	Once,
	Loop,
	OnceBackwards,
	LoopBackwards,
	PingPong,
	Manual,
};

/**
 * Renders one W3D model (HLOD + hierarchy) the way W3DModelDraw does: one mesh per sub-object,
 * attached to its bone, posed from the base hierarchy plus an optional W3D animation.
 * Sub-objects can be shown/hidden by name and house color is applied per player.
 */
UCLASS(ClassGroup = (Generals), meta = (BlueprintSpawnableComponent))
class GENERALS1_API UGenModelComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UGenModelComponent();

	/** Model name as used in INI files (e.g. "AVLeopard"). Returns false if it can't be loaded. */
	UFUNCTION(BlueprintCallable, Category = "Generals")
	bool SetModel(const FString& ModelName);

	UFUNCTION(BlueprintCallable, Category = "Generals")
	FString GetModelName() const { return ModelName; }

	/** Plays "HIERARCHY.ANIM". Speed multiplies the animation's native frame rate. */
	bool PlayAnimation(const FString& QualifiedName, EGenAnimMode Mode = EGenAnimMode::Loop, float Speed = 1.f);
	void StopAnimation();

	/**
	 * Plays one animation picked at random from a condition state's list. With bIdle, a new one is
	 * picked each time the current one finishes (IdleAnimation behaviour).
	 */
	void PlayAnimationSet(const TArray<FString>& Animations, EGenAnimMode Mode, float Speed, bool bIdle, bool bStartAtEnd = false);

	/** Restores every sub-object's visibility to its mesh default (before a state's hide/show list). */
	void ResetSubObjectVisibility();

	UFUNCTION(BlueprintCallable, Category = "Generals")
	void SetHouseColor(const FLinearColor& Color);

	/** Shows or hides a sub-object by mesh name (without container), e.g. "MUZZLEFX01". */
	UFUNCTION(BlueprintCallable, Category = "Generals")
	void ShowSubObject(const FString& SubObjectName, bool bShow);

	/**
	 * Shows/hides the sub-objects attached to bones "<Prefix>01".."<Prefix>99" (or "<Prefix>" alone),
	 * the way W3DModelDraw resolves WeaponMuzzleFlash / recoil bones.
	 */
	void SetSubObjectsOnBonePrefixHidden(const FString& Prefix, bool bHidden);

	/** W3DModelDraw::hideAllHeadlights: sub-objects named *HEADLIGHT* show only in the NIGHT condition. */
	void SetHeadlightsVisible(bool bShow);

	/** Bone transform in component space (Unreal units). */
	bool GetBoneTransform(const FString& BoneName, FTransform& OutTransform) const;

	/**
	 * Current pose of bone "<Name>01" (or "<Name>") in model space, W3D / Generals units and axes.
	 * Used for weapon fire-FX and projectile launch points.
	 */
	bool GetBoneTransformW3D(const FString& BoneName, FTransform3f& OutTransform) const;

	/** Poses of bones "<Prefix>01", "<Prefix>02", ... (or just "<Prefix>" if there are none); returns how many. */
	int32 GetNumberedBoneTransformsW3D(const FString& Prefix, TArray<FTransform3f>& OutTransforms) const;

	/** Overrides a bone's local rotation (e.g. turret yaw), applied after animation. */
	void SetBoneRotationOverride(const FString& BoneName, const FQuat& RotationInW3DSpace);

	/**
	 * Shifts bones "<Prefix>01".."<Prefix>99" (or "<Prefix>") in their own local frame, W3D units
	 * (e.g. barrel recoil along -X). A zero offset removes the override.
	 */
	void SetBoneOffsetOnPrefix(const FString& Prefix, const FVector3f& OffsetW3D);

	/** True while a non-looping animation is still playing. */
	bool IsAnimationPlaying() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

private:
	struct FSubObject
	{
		FString Name;            // mesh name without container
		int32 Bone = 0;
		bool bHiddenInMesh = false;
		bool bHiddenByDefault = false;
		bool bForcedHidden = false;
		bool bModelSpace = false; // component at identity (skins)
		TObjectPtr<UMeshComponent> Component = nullptr;
		TArray<FGenSectionMaterial> Sections;
		const FGenSkinData* Skin = nullptr; // CPU-skinned W3D skin
	};

	void ClearModel();
	void UpdatePose();
	void UpdateSkins();
	void ApplyMaterials(FSubObject& Sub);

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	FString ModelName;

	TSharedPtr<const FGenW3DFile> File;
	TSharedPtr<const FGenW3DHierarchy> Hierarchy;
	TSharedPtr<const FGenW3DAnimation> Animation;
	TArray<FSubObject> SubObjects;
	TArray<FTransform3f> PoseW3D;     // per pivot, model space (W3D frame)
	TArray<bool> PivotVisible;
	TMap<int32, FQuat4f> BoneOverrides;
	TMap<int32, FVector3f> BoneOffsets;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshComponent>> MeshComponents;

	EGenAnimMode AnimMode = EGenAnimMode::Loop;
	float AnimSpeed = 1.f;
	TArray<FString> IdlePool;
	float AnimFrame = 0.f;
	int32 PingPongDir = 1;
	FLinearColor HouseColor = FLinearColor(0.2f, 0.4f, 1.f);
};
