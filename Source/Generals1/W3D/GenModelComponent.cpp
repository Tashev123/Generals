#include "W3D/GenModelComponent.h"
#include "Core/GenPerf.h"
#include "Core/GenCoords.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "W3D/GenSkinMeshComponent.h"
#include "Async/ParallelFor.h"

static TAutoConsoleVariable<int32> CVarGenAnimInterpolation(
	TEXT("gen.AnimInterpolation"), 1,
	TEXT("Interpolate W3D animations between keyframes (the original snapped to integer frames)."),
	ECVF_Default);

namespace
{
	/** W3D model-space transform -> Unreal component-space transform. */
	FTransform ToUETransform(const FTransform3f& T)
	{
		const FVector3f P = T.GetTranslation();
		const FQuat4f Q = T.GetRotation();
		return FTransform(GenCoords::QuatToUE(Q.X, Q.Y, Q.Z, Q.W), GenCoords::ToUE(P.X, P.Y, P.Z), FVector(T.GetScale3D()));
	}

	FString StripContainer(const FString& FullName)
	{
		FString Left, Right;
		return FullName.Split(TEXT("."), &Left, &Right) ? Right : FullName;
	}

	void ConfigureMeshComponent(UMeshComponent* C, bool bCastShadow)
	{
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetGenerateOverlapEvents(false);
		C->SetCanEverAffectNavigation(false);
		C->bReceivesDecals = false;
		C->SetCastShadow(bCastShadow);
	}

	/**
	 * Part components nobody is using (the game thread's). A model that changes its looks - a building
	 * going up, then standing, then damaged - takes its old parts down and puts new ones up, twenty at a
	 * time. Destroyed, they are garbage the engine has to stop and collect: nine in ten of all the
	 * garbage a game made were these. Kept here, they are the next model's parts.
	 */
	TArray<TWeakObjectPtr<UStaticMeshComponent>> GFreeParts;
	constexpr int32 MaxFreeParts = 4000;

	void ReleasePart(UMeshComponent* C, const AActor* Owner)
	{
		UStaticMeshComponent* Part = C->GetClass() == UStaticMeshComponent::StaticClass() ? static_cast<UStaticMeshComponent*>(C) : nullptr;
		const UWorld* World = Part ? Part->GetWorld() : nullptr;
		if (Part && Owner && Part->GetOuter() == Owner && World && !World->bIsTearingDown && !Owner->IsActorBeingDestroyed() && GFreeParts.Num() < MaxFreeParts)
		{
			Part->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
			Part->UnregisterComponent();
			Part->EmptyOverrideMaterials();
			GFreeParts.Add(Part);
			return;
		}
		C->DestroyComponent();
	}

	/** A static mesh component for a part: one that was put away if there is one (of this map's), else a new one. Not registered. */
	UStaticMeshComponent* TakeStaticPart(UObject* Outer)
	{
		while (GFreeParts.Num() > 0)
		{
			UStaticMeshComponent* Part = GFreeParts.Pop(EAllowShrinking::No).Get();
			if (Part && Part->GetOuter() == Outer && !Part->IsRegistered())
			{
				Part->SetRelativeTransform(FTransform::Identity);
				return Part;
			}
		}
		return NewObject<UStaticMeshComponent>(Outer, NAME_None, RF_Transient);
	}
}

static TAutoConsoleVariable<int32> CVarGenPoseBudget(
	TEXT("gen.PoseBudget"), 32,
	TEXT("How many animated models in view are blended between animation frames per drawn frame (the rest take turns; all are posed at their animation's own frame rate)."));

UGenModelComponent::UGenModelComponent()
{
	static int32 NextSerial = 0;
	Serial = NextSerial++;
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bTickInEditor = true;
}

namespace
{
	/** The managed models that are animating (game thread's). */
	TArray<UGenModelComponent*> GManagedModels;
}

void UGenModelComponent::OnUnregister()
{
	if (ManagedIndex != INDEX_NONE)
	{
		SetAnimating(false); // (out of the world: nobody is to tick it any more)
	}
	Super::OnUnregister();
}

void UGenModelComponent::BeginDestroy()
{
	if (ManagedIndex != INDEX_NONE)
	{
		SetAnimating(false);
	}
	Super::BeginDestroy();
}

void UGenModelComponent::ResetForReuse()
{
	Animation.Reset();
	IdlePool.Reset();
	SetAnimating(false);
	AnimFrame = 0.f;
	PosedAnimFrame = -1000.f;
	BoneOverrides.Reset();
	BoneOffsets.Reset();
	bAnimPaused = false;
	SetStealthLook(0.f, 0.f);
	SetTint(FLinearColor(0.f, 0.f, 0.f, 0.f));
	RefreshParts();
	ResetSubObjectVisibility(); // (poses the model)
}

void UGenModelComponent::CountParts(int32& OutParts, int32& OutShown, int32& OutGroups) const
{
	TSet<int32> Groups;
	for (const FSubObject& Sub : SubObjects)
	{
		if (!Sub.Component)
		{
			continue;
		}
		++OutParts;
		if (Sub.Component->IsVisible())
		{
			++OutShown;
			if (Sub.Skin)
			{
				++OutGroups;
			}
			else
			{
				Groups.Add(Sub.bModelSpace ? -1 : Sub.Bone);
			}
		}
	}
	OutGroups += Groups.Num();
}

void UGenModelComponent::SetManagedTick(bool bManaged)
{
	if (bManaged == bManagedTick)
	{
		return;
	}
	const bool bAnimating = bManagedTick ? ManagedIndex != INDEX_NONE : IsComponentTickEnabled();
	SetAnimating(false);
	bManagedTick = bManaged;
	SetAnimating(bAnimating);
}

void UGenModelComponent::SetAnimating(bool bAnimating)
{
	if (!bManagedTick)
	{
		SetComponentTickEnabled(bAnimating);
		return;
	}
	if (bAnimating == (ManagedIndex != INDEX_NONE))
	{
		return;
	}
	if (bAnimating)
	{
		ManagedIndex = GManagedModels.Add(this);
	}
	else
	{
		GManagedModels.RemoveAtSwap(ManagedIndex, EAllowShrinking::No);
		if (GManagedModels.IsValidIndex(ManagedIndex))
		{
			GManagedModels[ManagedIndex]->ManagedIndex = ManagedIndex;
		}
		ManagedIndex = INDEX_NONE;
	}
}

void UGenModelComponent::TickManaged(float DeltaTime)
{
	static TArray<UGenModelComponent*> ToPose;
	ToPose.Reset();
	{
		GEN_PERF_SCOPE("R.ModelTick");
		// Backwards: a model that stops animating takes itself out, and the last one takes its place.
		for (int32 i = GManagedModels.Num() - 1; i >= 0; --i)
		{
			if (GManagedModels.IsValidIndex(i))
			{
				UGenModelComponent* Model = GManagedModels[i];
				Model->bWantsPose = false;
				Model->Advance(DeltaTime);
				if (Model->bWantsPose)
				{
					ToPose.Add(Model);
				}
			}
		}
	}
	GEN_PERF_SCOPE("R.Pose");
	GEN_PERF_COUNT("R.Poses#", ToPose.Num());
	// Posing is arithmetic on each model's own bones and vertices: many hands. What follows from it -
	// parts moved, new vertices sent to the renderer - is the game thread's, one model after the other.
	if (ToPose.Num() >= 6)
	{
		ParallelFor(TEXT("GenModelPose"), ToPose.Num(), 2, [](int32 Index) { ToPose[Index]->ComputePose(); });
	}
	else
	{
		for (UGenModelComponent* Model : ToPose)
		{
			Model->ComputePose();
		}
	}
	for (UGenModelComponent* Model : ToPose)
	{
		Model->ApplyPose();
	}
}

int32 UGenModelComponent::GetNumManagedAnimating()
{
	return GManagedModels.Num();
}

void UGenModelComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// The sub-object meshes are separate components: take them down with the model.
	ClearModel();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UGenModelComponent::ClearModel()
{
	for (UMeshComponent* C : MeshComponents)
	{
		if (C)
		{
			ReleasePart(C, GetOwner());
		}
	}
	MeshComponents.Reset();
	SubObjects.Reset();
	MergedMembers.Reset();
	MergedBones.Reset();
	MergedBonePose.Reset();
	PickParts.Reset();
	bPickPartsBuilt = false;
	PlanVersion = 0;
	File.Reset();
	Hierarchy.Reset();
	Animation.Reset();
	IdlePool.Reset();
	PoseW3D.Reset();
	PivotVisible.Reset();
	BoneOverrides.Reset();
	BoneOffsets.Reset();
	ModelName.Reset();
	SetAnimating(false);
}

bool UGenModelComponent::SetModel(const FString& InModelName)
{
	if (InModelName.Equals(ModelName, ESearchCase::IgnoreCase) && File)
	{
		return true;
	}
	ClearModel();
	if (InModelName.IsEmpty() || InModelName.Equals(TEXT("NONE"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Assets)
	{
		return false;
	}
	File = Assets->LoadW3D(InModelName);
	if (!File)
	{
		return false;
	}
	ModelName = InModelName;

	TArray<FGenW3DSubObject> Subs;
	Assets->GetModelSubObjects(*File, Subs, Hierarchy);
	const int32 NumPivots = Hierarchy ? FMath::Max(1, Hierarchy->Pivots.Num()) : 1;
	PoseW3D.Init(FTransform3f::Identity, NumPivots);
	PivotVisible.Init(true, NumPivots);

	BuildParts();
	UpdatePose();
	// (A model of bones alone is a model too: a mine field is eight bones with a light on each.)
	return SubObjects.Num() > 0 || (Hierarchy && Hierarchy->Pivots.Num() > 1);
}

void UGenModelComponent::BuildParts()
{
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	PickParts.Reset();
	bPickPartsBuilt = false;
	if (!Assets || !File)
	{
		return;
	}
	// Sub-objects: from the HLOD if present, otherwise every mesh in the file on the root bone.
	TArray<FGenW3DSubObject> Subs;
	TSharedPtr<const FGenW3DHierarchy> SameHierarchy;
	Assets->GetModelSubObjects(*File, Subs, SameHierarchy);
	const int32 NumPivots = PoseW3D.Num();
	const FString& InModelName = ModelName;
	// (The plan stays where it is while the parts are made: nothing below asks for another.)
	const FGenModelPlan& Plan = Assets->GetModelPlan(ModelName);
	PlanVersion = Plan.InfoVersion;

	UObject* Outer = GetOwner() ? (UObject*)GetOwner() : (UObject*)this;
	auto AddPart = [this](FSubObject& Sub)
	{
		if (Sub.Name.Contains(TEXT("HEADLIGHT"), ESearchCase::IgnoreCase))
		{
			Sub.bForcedHidden = true; // daytime default; see SetHeadlightsVisible
		}
		Sub.Component->SetVisibility(!Sub.bHiddenByDefault && !Sub.bForcedHidden);
		if (Sub.Component->bHiddenInGame != bHiddenInGame)
		{
			Sub.Component->SetHiddenInGame(bHiddenInGame); // (as the model is: in a transport, in the fog, in the pool)
		}
		// (A model put on while the object is stealthed starts out with the look the old one had.)
		Sub.Component->SetCustomPrimitiveDataFloat(0, StealthFade);
		Sub.Component->SetCustomPrimitiveDataFloat(1, StealthGlow);
		Sub.Component->SetCustomPrimitiveDataVector4(2, FVector4(ObjectTint.R, ObjectTint.G, ObjectTint.B, 0.f));
		MeshComponents.Add(Sub.Component);
		FSubObject& Added = SubObjects.Add_GetRef(MoveTemp(Sub));
		ApplyMaterials(Added);
	};

	if (Plan.Merged.Num() > 0 && Plan.MergedMesh.Mesh)
	{
		// The meshes nothing moves or hides, as one. It has each of them where the base pose has its bone:
		// those places are kept, to see should a pose ever have one of the bones elsewhere (ApplyPose).
		MergedMembers = Plan.Merged;
		TArray<FTransform3f, TInlineAllocator<64>> Base;
		Base.Init(FTransform3f::Identity, NumPivots);
		for (int32 i = 0; i < NumPivots; ++i)
		{
			if (Hierarchy && Hierarchy->Pivots.IsValidIndex(i))
			{
				const FGenW3DPivot& P = Hierarchy->Pivots[i];
				const FTransform3f Local(P.Rotation.GetNormalized(), P.Translation);
				Base[i] = (P.Parent >= 0 && P.Parent < i) ? Local * Base[P.Parent] : Local;
			}
		}
		for (const FGenModelPlan::FPart& Part : MergedMembers)
		{
			if (!MergedBones.Contains(Part.Bone))
			{
				MergedBones.Add(Part.Bone);
				MergedBonePose.Add(Base.IsValidIndex(Part.Bone) ? Base[Part.Bone] : FTransform3f::Identity);
			}
		}
		UStaticMeshComponent* C = TakeStaticPart(Outer);
		C->SetStaticMesh(Plan.MergedMesh.Mesh);
		ConfigureMeshComponent(C, true);
		C->SetupAttachment(this);
		C->RegisterComponent();
		FSubObject Sub;
		Sub.Name = TEXT("(merged)");
		Sub.Component = C;
		Sub.Sections = Plan.MergedMesh.Sections;
		Sub.bModelSpace = true;
		AddPart(Sub);
	}

	for (int32 SubIndex = 0; SubIndex < Subs.Num(); ++SubIndex)
	{
		if (Plan.bIsMerged.IsValidIndex(SubIndex) && Plan.bIsMerged[SubIndex])
		{
			continue;
		}
		const FGenW3DSubObject& SO = Subs[SubIndex];
		const FGenW3DMesh* Mesh = File->FindMesh(SO.Name);
		if (!Mesh)
		{
			continue; // boxes (PICKBOX, collision) and missing meshes are not rendered
		}
		const bool bCastShadow = (Mesh->Attributes & GenW3D::MeshFlagCastShadow) != 0 || !Mesh->IsHidden();

		FSubObject Sub;
		Sub.Name = StripContainer(SO.Name);
		Sub.SubIndex = SubIndex;
		Sub.bHiddenInMesh = Mesh->IsHidden();
		Sub.bHiddenByDefault = Sub.bHiddenInMesh;

		const bool bCanSkin = Mesh->IsSkin() && Hierarchy && Mesh->Influences.Num() == Mesh->Vertices.Num();
		if (bCanSkin)
		{
			// Deforming skin: skinned on the CPU, in component space.
			const FGenSkinData* Skin = Assets->GetSkinData(InModelName, *Mesh);
			if (!Skin || Skin->Sections.Num() == 0)
			{
				continue;
			}
			UGenSkinMeshComponent* C = NewObject<UGenSkinMeshComponent>(Outer, NAME_None, RF_Transient);
			ConfigureMeshComponent(C, bCastShadow);
			C->SetSkin(Skin);
			C->SetupAttachment(this);
			C->RegisterComponent();
			for (const FGenSkinData::FSection& S : Skin->Sections)
			{
				Sub.Sections.Add(S.Material);
			}
			Sub.Component = C;
			Sub.Skin = Skin;
			Sub.bModelSpace = true;
			Sub.Bone = 0;
		}
		else
		{
			const FGenBuiltMesh* Built = Assets->GetBuiltMesh(InModelName, *Mesh, Hierarchy.Get());
			if (!Built || !Built->Mesh)
			{
				continue;
			}
			UStaticMeshComponent* C = TakeStaticPart(Outer);
			C->SetStaticMesh(Built->Mesh);
			ConfigureMeshComponent(C, bCastShadow);
			C->SetupAttachment(this);
			C->RegisterComponent();
			Sub.Component = C;
			Sub.Sections = Built->Sections;
			Sub.bModelSpace = Built->bSkinBakedToModelSpace;
			Sub.Bone = Sub.bModelSpace ? 0 : FMath::Clamp(SO.Bone, 0, NumPivots - 1);
		}
		AddPart(Sub);
	}
}

void UGenModelComponent::RebuildParts()
{
	if (!File || bRebuildingParts)
	{
		return;
	}
	TGuardValue<bool> Guard(bRebuildingParts, true);
	GEN_PERF_SCOPE("R.ModelRebuild");
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	// What was shown or hidden stays so (and what is hidden is, by that, no part of the one mesh).
	TMap<FString, TPair<bool, bool>> Kept;
	for (const FSubObject& Sub : SubObjects)
	{
		Kept.Add(Sub.Name.ToLower(), TPair<bool, bool>(Sub.bHiddenByDefault, Sub.bForcedHidden));
		if (Assets && Sub.bForcedHidden && !Sub.bHiddenInMesh)
		{
			Assets->NoteToggledSubObject(ModelName, Sub.Name);
		}
	}
	for (UMeshComponent* C : MeshComponents)
	{
		if (C)
		{
			ReleasePart(C, GetOwner());
		}
	}
	MeshComponents.Reset();
	SubObjects.Reset();
	MergedMembers.Reset();
	MergedBones.Reset();
	MergedBonePose.Reset();
	BuildParts();
	for (FSubObject& Sub : SubObjects)
	{
		if (const TPair<bool, bool>* Flags = Kept.Find(Sub.Name.ToLower()))
		{
			Sub.bHiddenByDefault = Flags->Key;
			Sub.bForcedHidden = Flags->Value;
		}
	}
	UpdatePose();
}

bool UGenModelComponent::RefreshParts()
{
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	// (Parts come out of the one mesh as more becomes known, never into it: with none in it there is nothing to do.)
	if (!Assets || !File || bRebuildingParts || MergedMembers.Num() == 0 || Assets->GetMergeInfoVersion(ModelName) == PlanVersion)
	{
		return false;
	}
	const FGenModelPlan& Plan = Assets->GetModelPlan(ModelName);
	PlanVersion = Plan.InfoVersion;
	bool bSame = Plan.Merged.Num() == MergedMembers.Num();
	for (int32 i = 0; bSame && i < MergedMembers.Num(); ++i)
	{
		bSame = Plan.Merged[i].SubIndex == MergedMembers[i].SubIndex;
	}
	if (bSame)
	{
		return false;
	}
	RebuildParts();
	return true;
}

void UGenModelComponent::ApplyMaterials(FSubObject& Sub)
{
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	for (int32 s = 0; s < Sub.Sections.Num(); ++s)
	{
		Sub.Component->SetMaterial(s, Assets->GetSectionMaterial(Sub.Sections[s], HouseColor, bFadeable));
	}
}

void UGenModelComponent::SetStealthLook(float FadeOut, float Glow)
{
	FadeOut = FMath::Clamp(FadeOut, 0.f, 1.f);
	Glow = FMath::Clamp(Glow, 0.f, 1.f);
	const bool bWantFadeable = FadeOut > 0.001f;
	if (bWantFadeable != bFadeable)
	{
		bFadeable = bWantFadeable;
		for (FSubObject& Sub : SubObjects)
		{
			if (Sub.Component) { ApplyMaterials(Sub); }
		}
	}
	if (FadeOut == StealthFade && Glow == StealthGlow)
	{
		return;
	}
	StealthFade = FadeOut;
	StealthGlow = Glow;
	// Custom primitive data rather than material parameters: the material instances are shared between
	// all objects that look alike, and only this one is to change.
	for (UMeshComponent* C : MeshComponents)
	{
		if (C)
		{
			C->SetCustomPrimitiveDataFloat(0, StealthFade);
			C->SetCustomPrimitiveDataFloat(1, StealthGlow);
		}
	}
}

void UGenModelComponent::SetTint(const FLinearColor& Tint)
{
	if (Tint.Equals(ObjectTint, 1.e-3f))
	{
		return;
	}
	ObjectTint = Tint;
	for (UMeshComponent* C : MeshComponents)
	{
		if (C)
		{
			C->SetCustomPrimitiveDataVector4(2, FVector4(Tint.R, Tint.G, Tint.B, 0.f));
		}
	}
}

void UGenModelComponent::SetHouseColor(const FLinearColor& Color)
{
	HouseColor = Color;
	for (FSubObject& Sub : SubObjects)
	{
		const bool bHouseColored = Sub.Sections.ContainsByPredicate([](const FGenSectionMaterial& S) { return S.HouseColorMode != 0; });
		if (bHouseColored && Sub.Component)
		{
			ApplyMaterials(Sub);
		}
	}
}

void UGenModelComponent::ShowSubObject(const FString& SubObjectName, bool bShow)
{
	// W3DModelDraw::doHideShowSubObjs: the sub-object of that name, and with it "all other child objects
	// that use this bone as a parent" (doHideShowBoneSubObjs: whatever hangs on a bone further down the
	// tree - a launcher's spare missile under the turret that is put away).
	auto IsBelow = [this](int32 Bone, int32 Ancestor)
	{
		for (int32 Guard = 0; Hierarchy && Bone > 0 && Hierarchy->Pivots.IsValidIndex(Bone) && Guard < 256; ++Guard)
		{
			Bone = Hierarchy->Pivots[Bone].Parent;
			if (Bone == Ancestor)
			{
				return true;
			}
		}
		return false;
	};
	TArray<int32, TInlineAllocator<4>> NamedBones;
	for (const FGenModelPlan::FPart& Part : MergedMembers)
	{
		if (Part.Name.Equals(SubObjectName, ESearchCase::IgnoreCase)) { NamedBones.AddUnique(Part.Bone); }
	}
	for (const FSubObject& Sub : SubObjects)
	{
		if (Sub.Name.Equals(SubObjectName, ESearchCase::IgnoreCase)) { NamedBones.AddUnique(Sub.Bone); }
	}
	auto Affected = [&](const FString& Name, int32 Bone)
	{
		if (Name.Equals(SubObjectName, ESearchCase::IgnoreCase))
		{
			return true;
		}
		for (int32 Named : NamedBones)
		{
			if (Named > 0 && IsBelow(Bone, Named)) { return true; }
		}
		return false;
	};
	if (!bShow)
	{
		// Parts drawn as one mesh: parts of their own from now on, for every model of the kind.
		bool bRebuild = false;
		for (const FGenModelPlan::FPart& Part : MergedMembers)
		{
			if (Affected(Part.Name, Part.Bone))
			{
				if (UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get())
				{
					Assets->NoteToggledSubObject(ModelName, Part.Name);
				}
				bRebuild = true;
			}
		}
		if (bRebuild)
		{
			RebuildParts();
		}
	}
	for (FSubObject& Sub : SubObjects)
	{
		if (Affected(Sub.Name, Sub.Bone))
		{
			Sub.bForcedHidden = !bShow;
			Sub.bHiddenByDefault = false;
			if (Sub.Component)
			{
				Sub.Component->SetVisibility(bShow && PivotVisible.IsValidIndex(Sub.Bone) && PivotVisible[Sub.Bone]);
			}
		}
	}
}

void UGenModelComponent::SetHeadlightsVisible(bool bShow)
{
	for (FSubObject& Sub : SubObjects)
	{
		if (Sub.Name.Contains(TEXT("HEADLIGHT"), ESearchCase::IgnoreCase))
		{
			Sub.bForcedHidden = !bShow;
		}
	}
	UpdatePose();
}

void UGenModelComponent::SetSubObjectsOnBonePrefixHidden(const FString& Prefix, bool bHidden)
{
	if (!Hierarchy || Prefix.IsEmpty())
	{
		return;
	}
	// doSingleBoneName: a name resolves to a hierarchy bone, or failing that to a sub-object of that name.
	auto Resolve = [this](const FString& Name, TArray<int32>& OutBones) -> bool
	{
		const int32 Bone = Hierarchy->FindPivot(Name);
		if (Bone != INDEX_NONE)
		{
			OutBones.AddUnique(Bone);
			return true;
		}
		for (const FSubObject& Sub : SubObjects)
		{
			if (!Sub.bModelSpace && Sub.Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				OutBones.AddUnique(Sub.Bone);
				return true;
			}
		}
		for (const FGenModelPlan::FPart& Part : MergedMembers)
		{
			if (Part.Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				OutBones.AddUnique(Part.Bone);
				return true;
			}
		}
		return false;
	};
	TArray<int32> Bones;
	for (int32 i = 1; i <= 99; ++i)
	{
		if (!Resolve(FString::Printf(TEXT("%s%02d"), *Prefix, i), Bones)) { break; }
	}
	if (Bones.Num() == 0)
	{
		Resolve(Prefix, Bones);
	}
	if (bHidden && MergedBones.ContainsByPredicate([&Bones](int32 Bone) { return Bones.Contains(Bone); }))
	{
		// Parts drawn as one mesh are on those bones: parts of their own from now on, for every model of the kind.
		if (UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get())
		{
			Assets->NoteMovedBonePrefix(ModelName, Prefix);
		}
		RebuildParts();
	}
	bool bChanged = false;
	for (FSubObject& Sub : SubObjects)
	{
		if (!Sub.bModelSpace && Bones.Contains(Sub.Bone) && Sub.bForcedHidden != bHidden)
		{
			Sub.bForcedHidden = bHidden;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		UpdatePose();
	}
}

bool UGenModelComponent::PlayAnimation(const FString& QualifiedName, EGenAnimMode Mode, float Speed)
{
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	Animation = Assets ? Assets->FindAnimation(QualifiedName) : nullptr;
	if (!Animation || Animation->NumFrames <= 0)
	{
		Animation.Reset();
		return false;
	}
	AnimMode = Mode;
	AnimSpeed = Speed;
	PingPongDir = 1;
	AnimFrame = (Mode == EGenAnimMode::OnceBackwards || Mode == EGenAnimMode::LoopBackwards) ? float(Animation->NumFrames - 1) : 0.f;
	SetAnimating(Mode != EGenAnimMode::Manual);
	if (MergedMembers.Num() > 0)
	{
		// What it moves is no part of the one mesh (for the animations a draw module lists that was seen
		// to when the model's plan was made; this is for the others).
		Assets->NoteAnimation(ModelName, *Animation);
	}
	if (!RefreshParts())
	{
		UpdatePose();
	}
	return true;
}

void UGenModelComponent::StopAnimation()
{
	Animation.Reset();
	IdlePool.Reset();
	SetAnimating(false);
	UpdatePose();
}

void UGenModelComponent::PlayAnimationSet(const TArray<FString>& Animations, EGenAnimMode Mode, float Speed, bool bIdle, bool bStartAtEnd)
{
	IdlePool.Reset();
	if (Animations.Num() == 0)
	{
		StopAnimation();
		return;
	}
	if (PlayAnimation(Animations[FMath::RandRange(0, Animations.Num() - 1)], Mode, Speed) && bStartAtEnd && Mode == EGenAnimMode::Manual)
	{
		// START_FRAME_LAST: the pose at the end of the animation is held.
		AnimFrame = float(Animation->NumFrames - 1);
		UpdatePose();
	}
	if (bIdle)
	{
		IdlePool = Animations;
	}
}

bool UGenModelComponent::IsAnimationPlaying() const
{
	if (!Animation)
	{
		return false;
	}
	switch (AnimMode)
	{
	case EGenAnimMode::Once: return AnimFrame < float(Animation->NumFrames - 1);
	case EGenAnimMode::OnceBackwards: return AnimFrame > 0.f;
	case EGenAnimMode::Manual: return false;
	default: return true;
	}
}

bool UGenModelComponent::IsAnimationComplete() const
{
	if (!Animation)
	{
		return true;
	}
	switch (AnimMode)
	{
	case EGenAnimMode::Once: return AnimFrame >= float(Animation->NumFrames - 1);
	case EGenAnimMode::OnceBackwards: return AnimFrame <= 0.f;
	default: return false;
	}
}

void UGenModelComponent::SetAnimFrame(float Frame)
{
	if (Animation)
	{
		AnimFrame = FMath::Clamp(Frame, 0.f, float(FMath::Max(0, Animation->NumFrames - 1)));
		UpdatePose();
	}
}

void UGenModelComponent::SetAnimDuration(float Seconds)
{
	if (Animation && Seconds > 0.f && Animation->FrameRate > 0.f && Animation->NumFrames > 0)
	{
		// (a multiplier of the animation's own rate, as the original sets one)
		AnimSpeed = float(Animation->NumFrames) / Animation->FrameRate / Seconds;
	}
}

void UGenModelComponent::ResetSubObjectVisibility()
{
	for (FSubObject& Sub : SubObjects)
	{
		Sub.bHiddenByDefault = Sub.bHiddenInMesh;
		Sub.bForcedHidden = false;
	}
	UpdatePose();
}

void UGenModelComponent::SetBoneRotationOverride(const FString& BoneName, const FQuat& Rotation)
{
	if (!Hierarchy)
	{
		return;
	}
	const int32 Idx = Hierarchy->FindPivot(BoneName);
	if (Idx != INDEX_NONE)
	{
		BoneOverrides.Add(Idx, FQuat4f(Rotation));
		UpdatePose();
	}
}

bool UGenModelComponent::GetBoneTransformW3D(const FString& BoneName, FTransform3f& OutTransform) const
{
	if (!Hierarchy || BoneName.IsEmpty())
	{
		return false;
	}
	if (bPoseDirty)
	{
		const_cast<UGenModelComponent*>(this)->UpdatePose(); // out of view until now: the pose is behind the animation
	}
	int32 Idx = Hierarchy->FindPivot(BoneName + TEXT("01"));
	if (Idx == INDEX_NONE)
	{
		Idx = Hierarchy->FindPivot(BoneName);
	}
	if (!PoseW3D.IsValidIndex(Idx))
	{
		return false;
	}
	OutTransform = PoseW3D[Idx];
	return true;
}

int32 UGenModelComponent::GetNumberedBoneTransformsW3D(const FString& Prefix, TArray<FTransform3f>& OutTransforms) const
{
	OutTransforms.Reset();
	if (bPoseDirty && Hierarchy)
	{
		const_cast<UGenModelComponent*>(this)->UpdatePose();
	}
	if (!Hierarchy || Prefix.IsEmpty())
	{
		return 0;
	}
	for (int32 i = 1; i <= 99; ++i)
	{
		const int32 Idx = Hierarchy->FindPivot(FString::Printf(TEXT("%s%02d"), *Prefix, i));
		if (!PoseW3D.IsValidIndex(Idx))
		{
			break;
		}
		OutTransforms.Add(PoseW3D[Idx]);
	}
	if (OutTransforms.Num() == 0)
	{
		const int32 Idx = Hierarchy->FindPivot(Prefix);
		if (PoseW3D.IsValidIndex(Idx))
		{
			OutTransforms.Add(PoseW3D[Idx]);
		}
	}
	return OutTransforms.Num();
}

void UGenModelComponent::SetBoneOffsetOnPrefix(const FString& Prefix, const FVector3f& OffsetW3D)
{
	if (!Hierarchy || Prefix.IsEmpty())
	{
		return;
	}
	TArray<int32, TInlineAllocator<8>> Bones;
	for (int32 i = 1; i <= 99; ++i)
	{
		const int32 Bone = Hierarchy->FindPivot(FString::Printf(TEXT("%s%02d"), *Prefix, i));
		if (Bone == INDEX_NONE) { break; }
		Bones.Add(Bone);
	}
	if (Bones.Num() == 0)
	{
		const int32 Bone = Hierarchy->FindPivot(Prefix);
		if (Bone != INDEX_NONE) { Bones.Add(Bone); }
	}
	bool bChanged = false;
	for (int32 Bone : Bones)
	{
		if (OffsetW3D.IsNearlyZero())
		{
			bChanged |= BoneOffsets.Remove(Bone) > 0;
		}
		else
		{
			FVector3f& Cur = BoneOffsets.FindOrAdd(Bone, FVector3f::ZeroVector);
			bChanged |= !Cur.Equals(OffsetW3D, 1e-4f);
			Cur = OffsetW3D;
		}
	}
	if (bChanged)
	{
		UpdatePose();
	}
}

void UGenModelComponent::SetBoneControl(const FString& BoneName, const FVector3f& OffsetW3D, const FQuat4f& Rotation)
{
	if (!Hierarchy || BoneName.IsEmpty())
	{
		return;
	}
	const int32 Bone = Hierarchy->FindPivot(BoneName);
	if (Bone == INDEX_NONE)
	{
		return;
	}
	FVector3f& Offset = BoneOffsets.FindOrAdd(Bone, FVector3f::ZeroVector);
	FQuat4f& Rot = BoneOverrides.FindOrAdd(Bone, FQuat4f::Identity);
	if (Offset.Equals(OffsetW3D, 1e-4f) && Rot.Equals(Rotation, 1e-5f))
	{
		return;
	}
	Offset = OffsetW3D;
	Rot = Rotation;
	bPoseDirty = true;
	bControlsChanged = true; // (posed once for all of this frame's bones: UpdatePoseIfDirty)
}

void UGenModelComponent::SetTreadOffsets(float Left, float Right, float Middle)
{
	for (UMeshComponent* C : MeshComponents)
	{
		if (C)
		{
			// (Custom primitive data 5..7, read only by sections whose material scrolls as treads.)
			C->SetCustomPrimitiveDataFloat(5, Left);
			C->SetCustomPrimitiveDataFloat(6, Right);
			C->SetCustomPrimitiveDataFloat(7, Middle);
		}
	}
}

bool UGenModelComponent::GetBoneTransform(const FString& BoneName, FTransform& OutTransform) const
{
	if (!Hierarchy)
	{
		return false;
	}
	if (bPoseDirty)
	{
		const_cast<UGenModelComponent*>(this)->UpdatePose();
	}
	const int32 Idx = Hierarchy->FindPivot(BoneName);
	if (!PoseW3D.IsValidIndex(Idx))
	{
		return false;
	}
	OutTransform = ToUETransform(PoseW3D[Idx]);
	return true;
}

void UGenModelComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	bWantsPose = false;
	Advance(DeltaTime);
	if (bWantsPose)
	{
		UpdatePose();
	}
}

void UGenModelComponent::Advance(float DeltaTime)
{
	if (!Animation)
	{
		SetAnimating(false);
		return;
	}
	if (bAnimPaused)
	{
		return;
	}
	const float Last = float(Animation->NumFrames - 1);
	const float Step = DeltaTime * Animation->FrameRate * AnimSpeed;
	switch (AnimMode)
	{
	case EGenAnimMode::Loop:
		AnimFrame = FMath::Fmod(AnimFrame + Step, float(Animation->NumFrames));
		break;
	case EGenAnimMode::LoopBackwards:
		AnimFrame -= Step;
		if (AnimFrame < 0.f) { AnimFrame += float(Animation->NumFrames); }
		break;
	case EGenAnimMode::Once:
		AnimFrame = FMath::Min(AnimFrame + Step, Last);
		if (AnimFrame >= Last && IdlePool.Num() > 0)
		{
			// IdleAnimation: pick another idle once this one finishes.
			const TArray<FString> Pool = IdlePool;
			PlayAnimation(Pool[FMath::RandRange(0, Pool.Num() - 1)], EGenAnimMode::Once, AnimSpeed);
			IdlePool = Pool;
			return;
		}
		break;
	case EGenAnimMode::OnceBackwards:
		AnimFrame = FMath::Max(AnimFrame - Step, 0.f);
		break;
	case EGenAnimMode::PingPong:
		AnimFrame += Step * PingPongDir;
		if (AnimFrame >= Last) { AnimFrame = Last; PingPongDir = -1; }
		if (AnimFrame <= 0.f) { AnimFrame = 0.f; PingPongDir = 1; }
		break;
	default:
		break;
	}
	// Posing a model (and skinning it) costs more than everything else it does put together, and most
	// of the time most models are nowhere near the screen. Those keep their animation running, but are
	// posed again only when they come into view - or when somebody asks where one of their bones is.
	bPoseDirty = true;
	if (!IsBeingDrawn())
	{
		return;
	}
	// In view: posed whenever the animation has reached its next frame (the rate it was made at, 15 to
	// 30 a second) or its end. The blend towards the next frame, which is what makes it smooth at a
	// hundred frames a second, is given to as many models per drawn frame as the budget allows, in
	// turn - with a few soldiers on screen that is all of them every frame, with a thousand each gets
	// its turn less often, and the game stays fast.
	static uint64 CountFrame = 0;
	static int32 CountNow = 0, CountLast = 0;
	if (CountFrame != GFrameCounter)
	{
		CountFrame = GFrameCounter;
		CountLast = CountNow;
		CountNow = 0;
	}
	++CountNow;
	const bool bAtEnd = (AnimMode == EGenAnimMode::Once && AnimFrame >= Last) || (AnimMode == EGenAnimMode::OnceBackwards && AnimFrame <= 0.f);
	const bool bNextFrame = FMath::Abs(AnimFrame - PosedAnimFrame) >= 1.f || (bAtEnd && AnimFrame != PosedAnimFrame);
	const int32 Budget = FMath::Max(1, CVarGenPoseBudget.GetValueOnGameThread());
	const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(FMath::Max(CountLast, 1), Budget));
	if (bNextFrame || (uint64(Serial) + GFrameCounter) % uint64(Stride) == 0)
	{
		bWantsPose = true;
	}
}

bool UGenModelComponent::IsBeingDrawn() const
{
	for (const UMeshComponent* MC : MeshComponents)
	{
		if (MC && MC->WasRecentlyRendered(0.25f))
		{
			return true;
		}
	}
	return false;
}

void UGenModelComponent::UpdatePose()
{
	ComputePose();
	ApplyPose();
}

void UGenModelComponent::ComputePose()
{
	bPoseDirty = false;
	PosedAnimFrame = AnimFrame;
	const int32 NumPivots = PoseW3D.Num();
	int32 Frame0 = 0, Frame1 = 0;
	float Blend = 0.f;
	if (Animation)
	{
		const int32 N = Animation->NumFrames;
		Frame0 = FMath::Clamp(FMath::FloorToInt32(AnimFrame), 0, N - 1);
		if (CVarGenAnimInterpolation.GetValueOnAnyThread() != 0)
		{
			Blend = FMath::Clamp(AnimFrame - float(Frame0), 0.f, 1.f);
			const bool bLooping = AnimMode == EGenAnimMode::Loop || AnimMode == EGenAnimMode::LoopBackwards;
			Frame1 = bLooping ? (Frame0 + 1) % N : FMath::Min(Frame0 + 1, N - 1);
		}
		else
		{
			Frame1 = Frame0;
		}
	}

	for (int32 i = 0; i < NumPivots; ++i)
	{
		FTransform3f Local = FTransform3f::Identity;
		int32 Parent = -1;
		bool bPivotVis = true;
		if (Hierarchy && Hierarchy->Pivots.IsValidIndex(i))
		{
			const FGenW3DPivot& P = Hierarchy->Pivots[i];
			Parent = P.Parent;
			// HTreeClass::Anim_Update: Local = Base * T(anim) * R(anim); pivot 0 is the root (not animated).
			FQuat4f Rot = P.Rotation;
			FVector3f Trans = P.Translation;
			if (Animation && i > 0 && i < Animation->NumPivots())
			{
				FVector3f AT;
				FQuat4f AR;
				Animation->SamplePivot(i, Frame0, AT, AR, bPivotVis);
				if (Blend > 0.f && Frame1 != Frame0)
				{
					FVector3f AT1;
					FQuat4f AR1;
					bool bVis1;
					Animation->SamplePivot(i, Frame1, AT1, AR1, bVis1);
					AT = FMath::Lerp(AT, AT1, Blend);
					AR = FQuat4f::Slerp(AR, AR1, Blend);
				}
				Trans += Rot.RotateVector(AT);
				Rot = Rot * AR;
			}
			// Control_Bone: Local = Base * Control (translation then rotation in the bone's own frame).
			if (const FVector3f* Offset = BoneOffsets.Find(i))
			{
				Trans += Rot.RotateVector(*Offset);
			}
			if (const FQuat4f* Override = BoneOverrides.Find(i))
			{
				Rot = Rot * (*Override);
			}
			Local = FTransform3f(Rot.GetNormalized(), Trans);
		}
		PoseW3D[i] = (Parent >= 0 && Parent < i) ? Local * PoseW3D[Parent] : Local;
		PivotVisible[i] = bPivotVis && (Parent < 0 || Parent >= i || PivotVisible[Parent]);
	}
	ComputeSkins();
}

void UGenModelComponent::ApplyPose()
{
	if (MergedBones.Num() > 0 && !bRebuildingParts)
	{
		// The parts drawn as one mesh are where the base pose has their bones. Should this pose have one
		// of those elsewhere, or hidden - nothing said beforehand that it would - what is on it becomes
		// parts of their own again, for every model of the kind from now on.
		TArray<int32, TInlineAllocator<8>> Left;
		for (int32 i = 0; i < MergedBones.Num(); ++i)
		{
			const int32 Bone = MergedBones[i];
			if (PoseW3D.IsValidIndex(Bone) && (!PivotVisible[Bone] || !PoseW3D[Bone].Equals(MergedBonePose[i], 1.e-3f)))
			{
				Left.Add(Bone);
			}
		}
		if (Left.Num() > 0)
		{
			if (UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get())
			{
				Assets->NoteMovedPivots(ModelName, MakeArrayView(Left));
			}
			RebuildParts(); // (poses the model, with the parts it has now)
			return;
		}
	}
	for (FSubObject& Sub : SubObjects)
	{
		if (!Sub.Component)
		{
			continue;
		}
		if (!Sub.bModelSpace && PoseW3D.IsValidIndex(Sub.Bone))
		{
			Sub.Component->SetRelativeTransform(ToUETransform(PoseW3D[Sub.Bone]));
		}
		const bool bShouldShow = !Sub.bHiddenByDefault && !Sub.bForcedHidden && (Sub.bModelSpace || !PivotVisible.IsValidIndex(Sub.Bone) || PivotVisible[Sub.Bone]);
		if (Sub.Component->IsVisible() != bShouldShow)
		{
			Sub.Component->SetVisibility(bShouldShow);
		}
		if (Sub.bSkinPosed)
		{
			Sub.bSkinPosed = false;
			if (UGenSkinMeshComponent* SC = Cast<UGenSkinMeshComponent>(Sub.Component))
			{
				SC->PoseChanged(Sub.SkinBox);
			}
		}
	}
}

void UGenModelComponent::ComputeSkins()
{
	// A W3D skin's vertex follows one bone. The bones as what they do to a vertex, already in Unreal's
	// frame (mirrored on Y; positions in cm): made when the first skin wants them.
	struct FBoneAxes
	{
		FVector3f X, Y, Z, T;
	};
	TArray<FBoneAxes, TInlineAllocator<64>> Bones;
	const float Cm = float(GenCoords::UnitsToCm);
	const FPackedNormal TangentX(FVector3f(1.f, 0.f, 0.f));
	for (FSubObject& Sub : SubObjects)
	{
		UGenSkinMeshComponent* SC = Sub.Skin ? Cast<UGenSkinMeshComponent>(Sub.Component) : nullptr;
		// (Shown, as ApplyPose is about to see to: a skin hangs on no bone that could hide it.)
		if (!SC || Sub.bHiddenByDefault || Sub.bForcedHidden || PoseW3D.Num() == 0)
		{
			continue;
		}
		if (Bones.Num() == 0)
		{
			Bones.SetNumUninitialized(PoseW3D.Num());
			for (int32 b = 0; b < PoseW3D.Num(); ++b)
			{
				const FQuat4f Q = PoseW3D[b].GetRotation();
				const FVector3f AX = Q.GetAxisX(), AY = Q.GetAxisY(), AZ = Q.GetAxisZ(), T = PoseW3D[b].GetTranslation();
				Bones[b] = { GenCoords::DirToUE(AX.X, AX.Y, AX.Z), GenCoords::DirToUE(AY.X, AY.Y, AY.Z), GenCoords::DirToUE(AZ.X, AZ.Y, AZ.Z),
					GenCoords::ToUEf(T.X, T.Y, T.Z) };
			}
		}
		const int32 Total = SC->GetNumVertices();
		TArray<FVector3f>& Pos = SC->GetPosePositions();
		TArray<FPackedNormal>& Tan = SC->GetPoseTangents();
		Pos.SetNumUninitialized(Total, EAllowShrinking::No);
		Tan.SetNumUninitialized(Total * 2, EAllowShrinking::No);
		FVector3f Min(MAX_flt), Max(-MAX_flt);
		int32 Base = 0;
		for (const FGenSkinData::FSection& S : Sub.Skin->Sections)
		{
			const int32 N = S.LocalPositions.Num();
			if (Base + N > Total)
			{
				break;
			}
			for (int32 v = 0; v < N; ++v)
			{
				const FBoneAxes& Bone = Bones.IsValidIndex(S.Bones[v]) ? Bones[S.Bones[v]] : Bones[0];
				const FVector3f& L = S.LocalPositions[v];
				const FVector3f& LN = S.LocalNormals[v];
				const FVector3f P = (Bone.X * L.X + Bone.Y * L.Y + Bone.Z * L.Z) * Cm + Bone.T;
				Pos[Base + v] = P;
				Min = FVector3f::Min(Min, P);
				Max = FVector3f::Max(Max, P);
				Tan[(Base + v) * 2] = TangentX;
				Tan[(Base + v) * 2 + 1] = FPackedNormal(Bone.X * LN.X + Bone.Y * LN.Y + Bone.Z * LN.Z);
			}
			Base += N;
		}
		Sub.SkinBox = Base > 0 ? FBox3f(Min, Max) : FBox3f(ForceInit);
		Sub.bSkinPosed = true;
	}
}
