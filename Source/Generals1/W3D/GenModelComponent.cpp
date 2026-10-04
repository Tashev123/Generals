#include "W3D/GenModelComponent.h"
#include "Core/GenCoords.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "ProceduralMeshComponent.h"

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
}

UGenModelComponent::UGenModelComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bTickInEditor = true;
}

void UGenModelComponent::OnUnregister()
{
	Super::OnUnregister();
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
			C->DestroyComponent();
		}
	}
	MeshComponents.Reset();
	SubObjects.Reset();
	File.Reset();
	Hierarchy.Reset();
	Animation.Reset();
	IdlePool.Reset();
	PoseW3D.Reset();
	PivotVisible.Reset();
	BoneOverrides.Reset();
	BoneOffsets.Reset();
	ModelName.Reset();
	SetComponentTickEnabled(false);
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

	// Sub-objects: from the HLOD if present, otherwise every mesh in the file on the root bone.
	TArray<FGenW3DSubObject> Subs;
	if (File->HLod)
	{
		Hierarchy = Assets->FindHierarchy(File->HLod->HierarchyName, File.Get());
		Subs = File->HLod->SubObjects;
		Subs.Append(File->HLod->Aggregates);
	}
	else
	{
		Hierarchy = File->Hierarchy;
		for (const FGenW3DMesh& M : File->Meshes)
		{
			Subs.Add({ 0, M.FullName() });
		}
	}

	const int32 NumPivots = Hierarchy ? Hierarchy->Pivots.Num() : 1;
	PoseW3D.Init(FTransform3f::Identity, NumPivots);
	PivotVisible.Init(true, NumPivots);

	UObject* Outer = GetOwner() ? (UObject*)GetOwner() : (UObject*)this;
	for (const FGenW3DSubObject& SO : Subs)
	{
		const FGenW3DMesh* Mesh = File->FindMesh(SO.Name);
		if (!Mesh)
		{
			continue; // boxes (PICKBOX, collision) and missing meshes are not rendered
		}
		const bool bCastShadow = (Mesh->Attributes & GenW3D::MeshFlagCastShadow) != 0 || !Mesh->IsHidden();

		FSubObject Sub;
		Sub.Name = StripContainer(SO.Name);
		Sub.bHiddenInMesh = Mesh->IsHidden();
		Sub.bHiddenByDefault = Sub.bHiddenInMesh;

		const bool bCanSkin = Mesh->IsSkin() && Hierarchy && Mesh->Influences.Num() == Mesh->Vertices.Num();
		if (bCanSkin)
		{
			// Deforming skin: CPU-skinned procedural mesh in component space.
			const FGenSkinData* Skin = Assets->GetSkinData(InModelName, *Mesh);
			if (!Skin || Skin->Sections.Num() == 0)
			{
				continue;
			}
			UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(Outer, NAME_None, RF_Transient);
			ConfigureMeshComponent(C, bCastShadow);
			C->bUseAsyncCooking = true;
			C->SetupAttachment(this);
			C->RegisterComponent();
			for (int32 s = 0; s < Skin->Sections.Num(); ++s)
			{
				const FGenSkinData::FSection& S = Skin->Sections[s];
				TArray<FVector> Pos;
				TArray<FVector> Nrm;
				Pos.SetNumZeroed(S.LocalPositions.Num());
				Nrm.SetNumZeroed(S.LocalPositions.Num());
				C->CreateMeshSection_LinearColor(s, Pos, S.Triangles, Nrm, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false, false);
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
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Outer, NAME_None, RF_Transient);
			C->SetStaticMesh(Built->Mesh);
			ConfigureMeshComponent(C, bCastShadow);
			C->SetupAttachment(this);
			C->RegisterComponent();
			Sub.Component = C;
			Sub.Sections = Built->Sections;
			Sub.bModelSpace = Built->bSkinBakedToModelSpace;
			Sub.Bone = Sub.bModelSpace ? 0 : FMath::Clamp(SO.Bone, 0, NumPivots - 1);
		}

		if (Sub.Name.Contains(TEXT("HEADLIGHT"), ESearchCase::IgnoreCase))
		{
			Sub.bForcedHidden = true; // daytime default; see SetHeadlightsVisible
		}
		Sub.Component->SetVisibility(!Sub.bHiddenByDefault && !Sub.bForcedHidden);
		MeshComponents.Add(Sub.Component);
		FSubObject& Added = SubObjects.Add_GetRef(MoveTemp(Sub));
		ApplyMaterials(Added);
	}

	UpdatePose();
	return SubObjects.Num() > 0;
}

void UGenModelComponent::ApplyMaterials(FSubObject& Sub)
{
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	for (int32 s = 0; s < Sub.Sections.Num(); ++s)
	{
		Sub.Component->SetMaterial(s, Assets->GetSectionMaterial(Sub.Sections[s], HouseColor));
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
	for (FSubObject& Sub : SubObjects)
	{
		if (Sub.Name.Equals(SubObjectName, ESearchCase::IgnoreCase))
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
	SetComponentTickEnabled(Mode != EGenAnimMode::Manual);
	UpdatePose();
	return true;
}

void UGenModelComponent::StopAnimation()
{
	Animation.Reset();
	IdlePool.Reset();
	SetComponentTickEnabled(false);
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

bool UGenModelComponent::GetBoneTransform(const FString& BoneName, FTransform& OutTransform) const
{
	if (!Hierarchy)
	{
		return false;
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
	if (!Animation)
	{
		SetComponentTickEnabled(false);
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
	UpdatePose();
}

void UGenModelComponent::UpdatePose()
{
	const int32 NumPivots = PoseW3D.Num();
	int32 Frame0 = 0, Frame1 = 0;
	float Blend = 0.f;
	if (Animation)
	{
		const int32 N = Animation->NumFrames;
		Frame0 = FMath::Clamp(FMath::FloorToInt32(AnimFrame), 0, N - 1);
		if (CVarGenAnimInterpolation.GetValueOnGameThread() != 0)
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
	}
	UpdateSkins();
}

void UGenModelComponent::UpdateSkins()
{
	TArray<FVector> Pos;
	TArray<FVector> Nrm;
	for (FSubObject& Sub : SubObjects)
	{
		UProceduralMeshComponent* PMC = Cast<UProceduralMeshComponent>(Sub.Component);
		if (!Sub.Skin || !PMC || !PMC->IsVisible())
		{
			continue;
		}
		for (int32 s = 0; s < Sub.Skin->Sections.Num(); ++s)
		{
			const FGenSkinData::FSection& S = Sub.Skin->Sections[s];
			const int32 N = S.LocalPositions.Num();
			Pos.SetNumUninitialized(N);
			Nrm.SetNumUninitialized(N);
			for (int32 v = 0; v < N; ++v)
			{
				const FTransform3f& Bone = PoseW3D.IsValidIndex(S.Bones[v]) ? PoseW3D[S.Bones[v]] : PoseW3D[0];
				const FVector3f P = Bone.TransformPosition(S.LocalPositions[v]);
				const FVector3f Nm = Bone.TransformVectorNoScale(S.LocalNormals[v]);
				Pos[v] = FVector(GenCoords::ToUEf(P.X, P.Y, P.Z));
				Nrm[v] = FVector(GenCoords::DirToUE(Nm.X, Nm.Y, Nm.Z));
			}
			PMC->UpdateMeshSection_LinearColor(s, Pos, Nrm, TArray<FVector2D>(), TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
		}
	}
}
