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

	/** Control_Bone: a bone shifted and turned in its own frame (a wheel's spring, steering and roll), after animation. */
	void SetBoneControl(const FString& BoneName, const FVector3f& OffsetW3D, const FQuat4f& Rotation);

	/** W3DTankDraw's tread scrolling: how far the left, right and middle treads' textures have rolled (0..1). */
	void SetTreadOffsets(float Left, float Right, float Middle);
	/** After a batch of SetBoneControl: the model posed once for all of them (if anything changed). */
	void UpdatePoseIfDirty() { if (bControlsChanged) { bControlsChanged = false; UpdatePose(); } }

	/**
	 * The look of a stealthed object (Drawable::setEffectiveOpacity and the heat-vision pass): FadeOut 0..1
	 * thins the whole model out (0 = solid), Glow 0..1 tints it the colour of a detected unit.
	 */
	void SetStealthLook(float FadeOut, float Glow);

	/** Drawable::getTintColor / getSelectionColor: light added to the model as it is drawn (the flash of something just selected). */
	void SetTint(const FLinearColor& Tint);

	/**
	 * Swaying in the breeze (W3DTreeBuffer's vertex shader, SwayClientUpdate): Type 1..10 is which of the breeze's
	 * ten motions the model leans with (UGenFXManager::UpdateSway), 0 none. Every vertex is pushed along by its
	 * height above BaseZ - the foot of the tree, in the world (Unreal units).
	 */
	void SetSway(int32 Type, float BaseZ);
	int32 GetSwayType() const { return SwayType; }

	/**
	 * HLodClass::Cast_Ray, as W3DView::pickDrawable uses it: where a ray first meets what the model shows -
	 * the triangles of its meshes as they are posed, and its boxes (PICKBOX, BOUNDINGBOX). Hidden parts
	 * are not in the way, nor are those blended onto the picture (glass, lights, muzzle flashes). The ray
	 * is in the world; InOutT is how many times Dir along it the nearest thing so far lies, and a hit
	 * nearer than that makes it less.
	 */
	bool RaycastWorld(const FVector& Origin, const FVector& Dir, double& InOutT) const;

	/** True while a non-looping animation is still playing. */
	bool IsAnimationPlaying() const;

	/** Animatable3DObjClass::Is_Animation_Complete: a ONCE animation on its last frame, a ONCE_BACKWARDS one on
	 *  its first. (Nothing to play counts as complete: what waits for it then needn't wait.) */
	bool IsAnimationComplete() const;
	bool HasAnimation() const { return Animation.IsValid(); }
	int32 GetAnimNumFrames() const { return Animation ? Animation->NumFrames : 0; }
	float GetAnimFrame() const { return AnimFrame; }
	FString GetAnimationName() const { return Animation ? Animation->Name : FString(); }
	bool IsAnimationPaused() const { return bAnimPaused; }
	/** Puts the animation on that frame (START_FRAME_FIRST / LAST, RANDOMSTART, MAINTAIN_FRAME_ACROSS_STATES). */
	void SetAnimFrame(float Frame);
	/** W3DModelDraw::setCurAnimDurationInMsec: the animation as it stands now takes this long from end to end. */
	void SetAnimDuration(float Seconds);
	/** W3DModelDraw::setPauseAnimation: the animation stands still (a building without power, a tank hit by an EMP). */
	void SetAnimationPaused(bool bPaused) { bAnimPaused = bPaused; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnUnregister() override;
	virtual void BeginDestroy() override;

	/**
	 * Animated by TickManaged - one loop over all such models - instead of by a tick function of its
	 * own. For the game's models: there are hundreds, the engine's tick machinery costs more per model
	 * than the animating does, and their turn can then come at a time of the game's choosing (while the
	 * logic step has the floor: animating a model needs nothing of the logic).
	 */
	/** As it was when its model was first put on: no animation, no bone turned or shifted, every part
	 *  shown or hidden as the model says, nothing of a stealth look. For a model about to be used again. */
	void ResetForReuse();
	/** (-GenPerf) Parts of the model, those of them that show, and how many the showing rigid ones would be if all on one bone were one. */
	void CountParts(int32& OutParts, int32& OutShown, int32& OutGroups) const;
	/**
	 * The parts made anew if more has become known about parts of this model being moved or hidden since
	 * they were made, and that changes which of them are drawn as one mesh. True if they were.
	 */
	bool RefreshParts();
	void SetManagedTick(bool bManaged);
	static void TickManaged(float DeltaTime);
	static int32 GetNumManagedAnimating();
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

private:
	struct FSubObject
	{
		FString Name;            // mesh name without container
		int32 SubIndex = INDEX_NONE; // among the model's sub-objects (none: the one mesh of the merged ones)
		int32 Bone = 0;
		bool bHiddenInMesh = false;
		bool bHiddenByDefault = false;
		bool bForcedHidden = false;
		bool bModelSpace = false; // component at identity (skins)
		TObjectPtr<UMeshComponent> Component = nullptr;
		TArray<FGenSectionMaterial> Sections;
		const FGenSkinData* Skin = nullptr; // CPU-skinned W3D skin
		FBox3f SkinBox = FBox3f(ForceInit); // what the skin's vertices take up, as last worked out
		bool bSkinPosed = false;            // ... and not yet handed to its mesh
	};

	void ClearModel();
	/** What a ray is tested against (RaycastWorld): made when first asked for, anew when the parts are. */
	struct FPickPart
	{
		const FGenW3DMesh* Mesh = nullptr;
		const FGenW3DBox* Box = nullptr;
		int32 Bone = 0;
		int32 SubObject = INDEX_NONE;    // in SubObjects, for whether it shows (none: one of the merged, or a box)
		bool bSkin = false;              // its vertices each follow a bone of their own
		FVector3f Min = FVector3f::ZeroVector, Max = FVector3f::ZeroVector; // what it takes up on its bone
	};
	void BuildPickParts() const;
	mutable TArray<FPickPart> PickParts;
	mutable bool bPickPartsBuilt = false;
	mutable FVector3f PickCenter = FVector3f::ZeroVector; // the model's reach: a sphere in its own space (W3D frame)
	mutable float PickRadius = 0.f;
	/** The model's parts: one mesh for all those its plan says can be one (FGenModelPlan), one each for the rest. */
	void BuildParts();
	/** ... anew, by the plan as it is now. What was shown or hidden stays so; the model is posed. */
	void RebuildParts();
	TArray<FGenModelPlan::FPart> MergedMembers; // the sub-objects that are one mesh here
	TArray<int32> MergedBones;                  // the bones those are on, each once
	TArray<FTransform3f> MergedBonePose;        // ... and where the base pose has them, which is where the mesh has them
	uint32 PlanVersion = 0;
	bool bRebuildingParts = false;
	/** Poses the model where its animation stands: ComputePose, then ApplyPose. */
	void UpdatePose();
	/** The bones, and the skins' vertices after them. Touches nothing but this model's own data, so the
	 *  models that want posing in a frame are done side by side (TickManaged). */
	void ComputePose();
	/** What the pose means for the engine: parts moved and shown or hidden, the skins' new vertices sent. Game thread. */
	void ApplyPose();
	void ComputeSkins();
	bool bWantsPose = false;          // (Advance: the animation has moved on far enough to be worth posing)
	/** The animation is running (or not): ticked from now on, one way or the other. */
	void SetAnimating(bool bAnimating);
	/** A frame's worth of animation. */
	void Advance(float DeltaTime);
	bool bAnimPaused = false;
	bool bManagedTick = false;
	int32 ManagedIndex = INDEX_NONE;  // among the managed models that are animating
	/** Some part of the model was drawn (or cast a shadow) a moment ago. */
	bool IsBeingDrawn() const;
	/** The animation has moved on since the model was last posed (it wasn't in view). */
	mutable bool bPoseDirty = false;
	bool bControlsChanged = false;    // SetBoneControl moved a bone since the last UpdatePoseIfDirty
	float PosedAnimFrame = -1000.f;   // where the animation stood when the model was last posed
	int32 Serial = 0;                 // for taking turns
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
	float StealthFade = 0.f;
	float StealthGlow = 0.f;
	FLinearColor ObjectTint = FLinearColor(0.f, 0.f, 0.f, 0.f);
	int32 SwayType = 0;               // SetSway
	float SwayBaseZ = 0.f;
	bool bFadeable = false;           // the sections use the materials that can fade
};
