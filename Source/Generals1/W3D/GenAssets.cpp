#include "W3D/GenAssets.h"
#include "Core/GenMeshUtil.h"
#include "Core/GenCoords.h"
#include "Core/GenFileSystem.h"
#include "W3D/GenTextureLoader.h"
#include "Generals1.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Core/GenPerf.h"
#include "StaticMeshAttributes.h"

namespace
{
	const TCHAR* MaterialRoot = TEXT("/Game/Generals/Materials/");

	FString BaseMaterialName(EGenW3DBlend Blend, bool bTwoSided)
	{
		const TCHAR* Name = TEXT("Opaque");
		switch (Blend)
		{
		case EGenW3DBlend::Opaque: Name = TEXT("Opaque"); break;
		case EGenW3DBlend::AlphaTest: Name = TEXT("Masked"); break;
		case EGenW3DBlend::Alpha: Name = TEXT("Alpha"); break;
		case EGenW3DBlend::Additive: Name = TEXT("Additive"); break;
		case EGenW3DBlend::Multiply: Name = TEXT("Multiply"); break;
		}
		return FString::Printf(TEXT("MI_W3D_%s%s"), Name, bTwoSided ? TEXT("_2S") : TEXT(""));
	}

	/** Base pose of every pivot in W3D model space: World = ParentWorld * T(t) * R(q). */
	void ComputeBasePose(const FGenW3DHierarchy& H, TArray<FTransform3f>& OutWorld)
	{
		OutWorld.SetNum(H.Pivots.Num());
		for (int32 i = 0; i < H.Pivots.Num(); ++i)
		{
			const FGenW3DPivot& P = H.Pivots[i];
			const FTransform3f Local(P.Rotation, P.Translation);
			OutWorld[i] = P.Parent >= 0 && P.Parent < i ? Local * OutWorld[P.Parent] : Local;
		}
	}

	FVector3f AnyPerpendicular(const FVector3f& N)
	{
		const FVector3f Axis = FMath::Abs(N.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
		return FVector3f::CrossProduct(Axis, N).GetSafeNormal();
	}

	FLinearColor ToLinear(const FColor& C) { return FLinearColor::FromSRGBColor(C); }

	/** True when every triangle of the pass is drawn with the given blend mode. */
	bool PassBlendIs(const FGenW3DMesh& M, const FGenW3DMaterialPass& Pass, EGenW3DBlend Blend)
	{
		if (Pass.ShaderIds.Num() == 0)
		{
			return FGenW3DShader().GetBlend() == Blend;
		}
		for (const uint32 Id : Pass.ShaderIds)
		{
			if ((M.Shaders.IsValidIndex(Id) ? M.Shaders[Id] : FGenW3DShader()).GetBlend() != Blend)
			{
				return false;
			}
		}
		return true;
	}

	bool PassIsTextured(const FGenW3DMesh& M, const FGenW3DMaterialPass& Pass)
	{
		if (Pass.Stages.Num() == 0 || Pass.Stages[0].TextureIds.Num() == 0)
		{
			return false;
		}
		for (const uint32 Id : Pass.ShaderIds)
		{
			if (M.Shaders.IsValidIndex(Id) && M.Shaders[Id].Texturing != 0)
			{
				return true;
			}
		}
		return false;
	}

	bool PassIsEnvironmentMapped(const FGenW3DMesh& M, const FGenW3DMaterialPass& Pass)
	{
		for (const uint32 Id : Pass.VertexMaterialIds)
		{
			if (!M.VertexMaterials.IsValidIndex(Id) || !M.VertexMaterials[Id].IsEnvironmentMapped())
			{
				return false;
			}
		}
		return Pass.VertexMaterialIds.Num() > 0;
	}

	/** Which material pass carries the mesh's own texture, and the opaque pass it is blended over (if any). */
	struct FGenPassLayout
	{
		int32 Base = 0;
		int32 Under = INDEX_NONE;
	};

	/**
	 * W3D draws a mesh once per material pass, in order, so what ends up on screen is:
	 *  - an opaque pass replaces everything drawn before it (CBEuroCnd2: plain color, then the texture);
	 *  - "W3D Reflectivity" materials (CBIndWhs01 and ~2400 other meshes) draw an opaque environment map
	 *    (LAKEDUSK, EnvironmentMapperClass) or a plain color first and alpha-blend the real texture over it,
	 *    so the first pass only shows where the texture's alpha is below 1.
	 * Any other combination (additive night lights, detail passes, ...) keeps showing pass 0 alone.
	 */
	FGenPassLayout GetPassLayout(const FGenW3DMesh& M)
	{
		FGenPassLayout L;
		for (int32 p = 1; p < M.Passes.Num(); ++p)
		{
			const FGenW3DMaterialPass& Pass = M.Passes[p];
			if (PassBlendIs(M, Pass, EGenW3DBlend::Opaque))
			{
				L.Base = p;
				L.Under = INDEX_NONE;
			}
			else if (L.Under == INDEX_NONE && p == L.Base + 1 && PassBlendIs(M, Pass, EGenW3DBlend::Alpha) && PassIsTextured(M, Pass))
			{
				const FGenW3DMaterialPass& Below = M.Passes[L.Base];
				if (PassBlendIs(M, Below, EGenW3DBlend::Opaque) && (PassIsEnvironmentMapped(M, Below) || !PassIsTextured(M, Below)))
				{
					L.Under = L.Base;
					L.Base = p;
				}
			}
		}
		return L;
	}

	/** Base-pass UVs (stage 0) and per-vertex diffuse colors, when present for every vertex. */
	void GetVertexStreams(const FGenW3DMesh& M, const TArray<FVector2f>*& OutUVs, const TArray<FColor>*& OutColors)
	{
		const int32 NumVerts = M.Vertices.Num();
		const FGenW3DMaterialPass* Pass = M.Passes.Num() > 0 ? &M.Passes[GetPassLayout(M).Base] : nullptr;
		const FGenW3DTextureStage* Stage = Pass && Pass->Stages.Num() > 0 ? &Pass->Stages[0] : nullptr;
		OutUVs = Stage && Stage->TexCoords.Num() == NumVerts ? &Stage->TexCoords : nullptr;
		OutColors = Pass && Pass->DiffuseColors.Num() == NumVerts ? &Pass->DiffuseColors : nullptr;
	}

	/** The "Key=Value" settings of a vertex material's mapper (W3D's INI-like mapper arguments). */
	TMap<FString, float> ParseMapperArgs(const FString& Args)
	{
		FString Text = Args.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\t"), TEXT(" "));
		while (Text.Contains(TEXT(" ="))) { Text = Text.Replace(TEXT(" ="), TEXT("=")); }
		while (Text.Contains(TEXT("= "))) { Text = Text.Replace(TEXT("= "), TEXT("=")); }
		TArray<FString> Tokens;
		Text.ParseIntoArrayWS(Tokens);
		TMap<FString, float> Out;
		for (const FString& Token : Tokens)
		{
			FString Key, Value;
			if (Token.Split(TEXT("="), &Key, &Value) && !Key.IsEmpty())
			{
				Out.Add(Key.ToLower(), FCString::Atof(*Value));
			}
		}
		return Out;
	}

	/** Groups triangles into sections by (shader, texture, vertex material) and derives their materials. */
	void BuildSections(const FGenW3DMesh& M, TArray<FGenSectionMaterial>& OutSections, TArray<int32>& OutTriSection)
	{
		const FGenPassLayout Layout = GetPassLayout(M);
		const FGenW3DMaterialPass* Pass = M.Passes.Num() > 0 ? &M.Passes[Layout.Base] : nullptr;
		const FGenW3DTextureStage* Stage = Pass && Pass->Stages.Num() > 0 ? &Pass->Stages[0] : nullptr;
		const FGenW3DMaterialPass* Under = M.Passes.IsValidIndex(Layout.Under) ? &M.Passes[Layout.Under] : nullptr;
		const FGenW3DTextureStage* UnderStage = Under && Under->Stages.Num() > 0 ? &Under->Stages[0] : nullptr;
		auto PickId = [](const TArray<uint32>& Ids, int32 Index) -> int32
		{
			if (Ids.Num() == 0) { return INDEX_NONE; }
			return int32(Ids.Num() > 1 && Ids.IsValidIndex(Index) ? Ids[Index] : Ids[0]);
		};

		TMap<TPair<FIntVector, FIntVector>, int32> SectionOf;
		OutTriSection.SetNumUninitialized(M.Triangles.Num());
		for (int32 t = 0; t < M.Triangles.Num(); ++t)
		{
			const int32 ShaderId = Pass ? PickId(Pass->ShaderIds, t) : INDEX_NONE;
			const int32 TexId = Stage ? PickId(Stage->TextureIds, t) : INDEX_NONE;
			const int32 VMatId = Pass ? PickId(Pass->VertexMaterialIds, M.Triangles[t].X) : INDEX_NONE;
			const int32 UnderShaderId = Under ? PickId(Under->ShaderIds, t) : INDEX_NONE;
			const int32 UnderTexId = UnderStage ? PickId(UnderStage->TextureIds, t) : INDEX_NONE;
			const int32 UnderVMatId = Under ? PickId(Under->VertexMaterialIds, M.Triangles[t].X) : INDEX_NONE;
			const TPair<FIntVector, FIntVector> SKey(FIntVector(ShaderId, TexId, VMatId), FIntVector(UnderShaderId, UnderTexId, UnderVMatId));
			int32* Existing = SectionOf.Find(SKey);
			if (!Existing)
			{
				const int32 NewIndex = OutSections.Num();
				FGenSectionMaterial& S = OutSections.AddDefaulted_GetRef();
				const FGenW3DShader Shader = M.Shaders.IsValidIndex(ShaderId) ? M.Shaders[ShaderId] : FGenW3DShader();
				S.Blend = Shader.GetBlend();
				if (M.Textures.IsValidIndex(TexId) && Shader.Texturing != 0)
				{
					S.Texture = M.Textures[TexId].Name;
				}
				if (M.VertexMaterials.IsValidIndex(VMatId))
				{
					const FGenW3DVertexMaterial& VM = M.VertexMaterials[VMatId];
					S.Diffuse = ToLinear(VM.Diffuse);
					S.Emissive = ToLinear(VM.Emissive);
					S.Opacity = VM.Opacity;
					if (VM.Stage0Mapping() == GenW3D::MappingLinearOffset)
					{
						const TMap<FString, float> Args = ParseMapperArgs(VM.MapperArgs0);
						auto Arg = [&Args](const TCHAR* Key, float Default) { const float* V = Args.Find(Key); return V ? *V : Default; };
						S.UVScale = FVector2f(Arg(TEXT("uscale"), 1.f), Arg(TEXT("vscale"), 1.f));
						if (M.Name.StartsWith(TEXT("TREADS"), ESearchCase::IgnoreCase))
						{
							// W3DTankDraw::updateTreadObjects: "check next character after 'TREADS'".
							const TCHAR Side = M.Name.Len() > 6 ? FChar::ToUpper(M.Name[6]) : TEXT(' ');
							S.ScrollMode = Side == TEXT('L') ? 2 : Side == TEXT('R') ? 3 : 4;
						}
						else
						{
							S.ScrollMode = 1;
							S.UVPerSec = FVector2f(Arg(TEXT("upersec"), 0.f), Arg(TEXT("vpersec"), 0.f));
							S.UVStart = FVector2f(Arg(TEXT("uoffset"), 0.f), Arg(TEXT("voffset"), 0.f));
						}
					}
				}
				if (Under)
				{
					// The two passes become one opaque material that blends them by the texture's alpha.
					S.bUnderlay = true;
					S.Blend = EGenW3DBlend::Opaque;
					if (M.Textures.IsValidIndex(UnderTexId) && M.Shaders.IsValidIndex(UnderShaderId) && M.Shaders[UnderShaderId].Texturing != 0)
					{
						S.UnderTexture = M.Textures[UnderTexId].Name;
					}
					if (M.VertexMaterials.IsValidIndex(UnderVMatId))
					{
						S.UnderTint = ToLinear(M.VertexMaterials[UnderVMatId].Diffuse);
					}
				}
				S.bTwoSided = M.IsTwoSided();
				if (M.Name.StartsWith(TEXT("HOUSECOLOR"), ESearchCase::IgnoreCase))
				{
					S.HouseColorMode = 1;
				}
				else if (S.Texture.StartsWith(TEXT("ZHC"), ESearchCase::IgnoreCase))
				{
					S.HouseColorMode = 2;
				}
				Existing = &SectionOf.Add(SKey, NewIndex);
			}
			OutTriSection[t] = *Existing;
		}
	}
}

UGenAssetSubsystem* UGenAssetSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UGenAssetSubsystem>() : nullptr;
}

void UGenAssetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FGenFileSystem::Get().Initialize();
}

TSharedPtr<const FGenW3DFile> UGenAssetSubsystem::LoadW3D(const FString& Name)
{
	const FString Key = Name.ToLower();
	FScopeLock Lock(&W3DLock);
	if (const TSharedPtr<FGenW3DFile>* Found = W3DFiles.Find(Key))
	{
		return *Found;
	}
	if (MissingW3D.Contains(Key))
	{
		return nullptr;
	}
	TArray<uint8> Bytes;
	TSharedPtr<FGenW3DFile> File = MakeShared<FGenW3DFile>();
	// (W3DFileSystem: the language's folder first, then the common one.)
	const bool bRead = FGenFileSystem::Get().ReadFile(FString::Printf(TEXT("Data\\English\\Art\\W3D\\%s.w3d"), *Name), Bytes)
		|| FGenFileSystem::Get().ReadFile(FString::Printf(TEXT("Art\\W3D\\%s.w3d"), *Name), Bytes);
	if (!bRead || !GenW3D::Parse(Bytes, *File))
	{
		UE_LOG(LogGenerals, Warning, TEXT("W3D '%s' not found or invalid"), *Name);
		MissingW3D.Add(Key);
		return nullptr;
	}
	W3DFiles.Add(Key, File);
	return File;
}

void UGenAssetSubsystem::QueuePreloadModel(const FString& ModelName)
{
	if (ModelName.IsEmpty() || ModelName.Equals(TEXT("NONE"), ESearchCase::IgnoreCase))
	{
		return;
	}
	bool bAlready = false;
	PreloadQueued.Add(TEXT("m|") + ModelName.ToLower(), &bAlready);
	if (!bAlready)
	{
		PreloadQueue.Add({ 0, ModelName, 0 });
	}
}

void UGenAssetSubsystem::QueuePreloadAnimation(const FString& QualifiedName)
{
	if (QualifiedName.IsEmpty())
	{
		return;
	}
	bool bAlready = false;
	PreloadQueued.Add(TEXT("a|") + QualifiedName.ToLower(), &bAlready);
	if (!bAlready)
	{
		PreloadQueue.Add({ 2, QualifiedName, 0 });
	}
}

int32 UGenAssetSubsystem::TickPreload(double BudgetSeconds)
{
	const double Start = FPlatformTime::Seconds();
	while (PreloadHead < PreloadQueue.Num() && FPlatformTime::Seconds() - Start < BudgetSeconds)
	{
		const FPreloadStep Step = PreloadQueue[PreloadHead++]; // (a copy: the queue grows below)
		if (Step.Kind == 2)
		{
			FindAnimation(Step.Name);
			continue;
		}
		TSharedPtr<const FGenW3DFile> File = LoadW3D(Step.Name);
		if (!File)
		{
			continue;
		}
		// The sub-objects, as UGenModelComponent::SetModel takes them: the HLOD's, or every mesh of the file.
		const int32 NumLod = File->HLod ? File->HLod->SubObjects.Num() : 0;
		const int32 NumSubs = File->HLod ? NumLod + File->HLod->Aggregates.Num() : File->Meshes.Num();
		TSharedPtr<const FGenW3DHierarchy> Hierarchy = File->Hierarchy;
		if (File->HLod)
		{
			Hierarchy = FindHierarchy(File->HLod->HierarchyName, File.Get());
		}
		if (Step.Kind == 0)
		{
			PreloadQueue.Add({ 3, Step.Name, 0 });
			for (int32 i = 0; i < NumSubs; ++i)
			{
				PreloadQueue.Add({ 1, Step.Name, i });
			}
			continue;
		}
		if (Step.Kind == 3)
		{
			// The meshes of the model that are drawn as one, built, and what they are drawn with.
			const TArray<FGenSectionMaterial> MergedSections = GetModelPlan(Step.Name).MergedMesh.Sections;
			for (const FGenSectionMaterial& S : MergedSections)
			{
				if (S.HouseColorMode == 0)
				{
					GetSectionMaterial(S, FLinearColor::White);
				}
				else
				{
					if (!S.Texture.IsEmpty()) { GetTexture(S.Texture); }
					if (S.bUnderlay && !S.UnderTexture.IsEmpty()) { GetTexture(S.UnderTexture); }
				}
			}
			continue;
		}
		if (Step.Index >= NumSubs)
		{
			continue;
		}
		{
			// (Part of the merged mesh: not wanted by itself.)
			const FGenModelPlan& Plan = GetModelPlan(Step.Name);
			if (Plan.bIsMerged.IsValidIndex(Step.Index) && Plan.bIsMerged[Step.Index])
			{
				continue;
			}
		}
		const FString SubName = File->HLod ? (Step.Index < NumLod ? File->HLod->SubObjects[Step.Index].Name : File->HLod->Aggregates[Step.Index - NumLod].Name)
			: File->Meshes[Step.Index].FullName();
		const FGenW3DMesh* Mesh = File->FindMesh(SubName);
		if (!Mesh)
		{
			continue;
		}
		TArray<FGenSectionMaterial> Sections;
		if (Mesh->IsSkin() && Hierarchy && Mesh->Influences.Num() == Mesh->Vertices.Num())
		{
			if (const FGenSkinData* Skin = GetSkinData(Step.Name, *Mesh))
			{
				for (const FGenSkinData::FSection& S : Skin->Sections)
				{
					Sections.Add(S.Material);
				}
			}
		}
		else if (const FGenBuiltMesh* Built = GetBuiltMesh(Step.Name, *Mesh, Hierarchy.Get()))
		{
			Sections = Built->Sections;
		}
		for (const FGenSectionMaterial& S : Sections)
		{
			if (S.HouseColorMode == 0)
			{
				GetSectionMaterial(S, FLinearColor::White); // (the textures come with it)
			}
			else
			{
				// A material per player colour is made when a player's unit first wears it; the textures can be had now.
				if (!S.Texture.IsEmpty()) { GetTexture(S.Texture); }
				if (S.bUnderlay && !S.UnderTexture.IsEmpty()) { GetTexture(S.UnderTexture); }
			}
		}
	}
	if (PreloadHead >= PreloadQueue.Num())
	{
		PreloadQueue.Reset();
		PreloadHead = 0;
	}
	return PreloadQueue.Num() - PreloadHead;
}

TSharedPtr<const FGenW3DAnimation> UGenAssetSubsystem::FindAnimation(const FString& QualifiedName)
{
	FString Hier, AnimName;
	if (!QualifiedName.Split(TEXT("."), &Hier, &AnimName))
	{
		AnimName = QualifiedName;
	}
	TSharedPtr<const FGenW3DFile> File = LoadW3D(AnimName);
	if (!File)
	{
		return nullptr;
	}
	for (const TSharedPtr<FGenW3DAnimation>& A : File->Animations)
	{
		if (A->Name.Equals(AnimName, ESearchCase::IgnoreCase))
		{
			return A;
		}
	}
	return File->Animations.Num() > 0 ? File->Animations[0] : nullptr;
}

TSharedPtr<const FGenW3DHierarchy> UGenAssetSubsystem::FindHierarchy(const FString& HierarchyName, const FGenW3DFile* SameFile)
{
	if (HierarchyName.IsEmpty())
	{
		return nullptr;
	}
	if (SameFile && SameFile->Hierarchy && SameFile->Hierarchy->Name.Equals(HierarchyName, ESearchCase::IgnoreCase))
	{
		return SameFile->Hierarchy;
	}
	TSharedPtr<const FGenW3DFile> File = LoadW3D(HierarchyName);
	return File ? File->Hierarchy : nullptr;
}

namespace
{
	/** One pivot's own transform, with an animation's frame on it (HTreeClass::Anim_Update: Local = Base * T(anim) * R(anim)). */
	FTransform3f PristineLocal(const FGenW3DHierarchy& Hierarchy, int32 Pivot, const FGenW3DAnimation* Anim, int32 Frame, const FQuat4f* ExtraRotation = nullptr)
	{
		const FGenW3DPivot& P = Hierarchy.Pivots[Pivot];
		FQuat4f Rot = P.Rotation;
		FVector3f Trans = P.Translation;
		if (Anim && Pivot > 0 && Pivot < Anim->NumPivots())
		{
			FVector3f AT;
			FQuat4f AR;
			bool bVisible;
			Anim->SamplePivot(Pivot, Frame, AT, AR, bVisible);
			Trans += Rot.RotateVector(AT);
			Rot = Rot * AR;
		}
		if (ExtraRotation)
		{
			Rot = Rot * (*ExtraRotation);
		}
		return FTransform3f(Rot.GetNormalized(), Trans);
	}
}

bool UGenAssetSubsystem::GetPristineBoneTurned(const FString& ModelName, const FString& BoneName, const FString& TurnedBone, float Angle, FTransform3f& OutBone,
	const FString& Animation, bool bLastFrame)
{
	TSharedPtr<const FGenW3DFile> File = LoadW3D(ModelName);
	if (!File || BoneName.IsEmpty())
	{
		return false;
	}
	TSharedPtr<const FGenW3DHierarchy> Hierarchy = File->HLod ? FindHierarchy(File->HLod->HierarchyName, File.Get()) : File->Hierarchy;
	if (!Hierarchy)
	{
		return false;
	}
	int32 Pivot = Hierarchy->FindPivot(BoneName + TEXT("01"));
	if (Pivot == INDEX_NONE)
	{
		Pivot = Hierarchy->FindPivot(BoneName);
	}
	if (Pivot == INDEX_NONE)
	{
		return false;
	}
	const int32 Turned = TurnedBone.IsEmpty() ? INDEX_NONE : Hierarchy->FindPivot(TurnedBone);
	TSharedPtr<const FGenW3DAnimation> Anim = Animation.IsEmpty() ? nullptr : FindAnimation(Animation);
	const int32 Frame = (Anim && bLastFrame) ? FMath::Max(0, Anim->NumFrames - 1) : 0;
	// As UGenModelComponent poses it: each pivot's own transform (the turned one's with the turn on top
	// of its rotation) on its parent's.
	const FQuat4f Turn(FVector3f::UpVector, Angle);
	FTransform3f Xf = FTransform3f::Identity;
	for (int32 Guard = 0; Hierarchy->Pivots.IsValidIndex(Pivot) && Guard < 256; ++Guard)
	{
		Xf = Xf * PristineLocal(*Hierarchy, Pivot, Anim.Get(), Frame, Pivot == Turned ? &Turn : nullptr);
		Pivot = Hierarchy->Pivots[Pivot].Parent;
	}
	OutBone = Xf;
	return true;
}

int32 UGenAssetSubsystem::GetPristineBones(const FString& ModelName, const FString& Prefix, int32 StartIndex, int32 MaxBones, TArray<FTransform3f>& OutBones,
	const FString& Animation, bool bLastFrame)
{
	OutBones.Reset();
	TSharedPtr<const FGenW3DFile> File = LoadW3D(ModelName);
	if (!File || Prefix.IsEmpty())
	{
		return 0;
	}
	TSharedPtr<const FGenW3DHierarchy> Hierarchy = File->HLod ? FindHierarchy(File->HLod->HierarchyName, File.Get()) : File->Hierarchy;
	if (!Hierarchy)
	{
		return 0;
	}
	TSharedPtr<const FGenW3DAnimation> Anim = Animation.IsEmpty() ? nullptr : FindAnimation(Animation);
	const int32 Frame = (Anim && bLastFrame) ? FMath::Max(0, Anim->NumFrames - 1) : 0;
	// Base pose in model space (or that frame of the animation): each pivot's own transform on top of its parent's.
	auto ModelSpace = [&Hierarchy, &Anim, Frame](int32 Pivot)
	{
		FTransform3f Xf = FTransform3f::Identity;
		for (int32 Guard = 0; Hierarchy->Pivots.IsValidIndex(Pivot) && Guard < 256; ++Guard)
		{
			Xf = Xf * PristineLocal(*Hierarchy, Pivot, Anim.Get(), Frame);
			Pivot = Hierarchy->Pivots[Pivot].Parent;
		}
		return Xf;
	};
	if (StartIndex == 0)
	{
		const int32 Pivot = Hierarchy->FindPivot(Prefix);
		if (Pivot != INDEX_NONE)
		{
			OutBones.Add(ModelSpace(Pivot));
		}
		return OutBones.Num();
	}
	for (int32 i = StartIndex; OutBones.Num() < MaxBones; ++i)
	{
		const int32 Pivot = Hierarchy->FindPivot(FString::Printf(TEXT("%s%02d"), *Prefix, i));
		if (Pivot == INDEX_NONE)
		{
			break;
		}
		OutBones.Add(ModelSpace(Pivot));
	}
	return OutBones.Num();
}

UTexture2D* UGenAssetSubsystem::GetTexture(const FString& TextureName)
{
	const FString Key = FPaths::GetBaseFilename(TextureName).ToLower();
	if (Key.IsEmpty())
	{
		return nullptr;
	}
	if (TObjectPtr<UTexture2D>* Found = Textures.Find(Key))
	{
		return *Found;
	}
	if (MissingTextures.Contains(Key))
	{
		return nullptr;
	}
	UTexture2D* Tex = GenTextureLoader::LoadGameTexture(TextureName);
	if (!Tex)
	{
		UE_LOG(LogGenerals, Verbose, TEXT("Texture '%s' not found"), *TextureName);
		MissingTextures.Add(Key);
		return nullptr;
	}
	Textures.Add(Key, Tex);
	return Tex;
}

UMaterialInterface* UGenAssetSubsystem::GetBaseMaterial(EGenW3DBlend Blend, bool bTwoSided)
{
	const FString Name = BaseMaterialName(Blend, bTwoSided);
	if (TObjectPtr<UMaterialInterface>* Found = BaseMaterials.Find(Name))
	{
		return *Found;
	}
	UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *(FString(MaterialRoot) + Name + TEXT(".") + Name));
	if (!M)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Missing base material %s (run Tools/ue_setup.py)"), *Name);
		M = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	BaseMaterials.Add(Name, M);
	return M;
}

UMaterialInterface* UGenAssetSubsystem::GetNamedMaterial(const TCHAR* MaterialName)
{
	const FString Name = MaterialName;
	if (TObjectPtr<UMaterialInterface>* Found = BaseMaterials.Find(Name))
	{
		return *Found;
	}
	UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *(FString(MaterialRoot) + Name + TEXT(".") + Name));
	if (!M)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Missing material %s (run Tools/ue_setup.py)"), *Name);
		M = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	BaseMaterials.Add(Name, M);
	return M;
}

UMaterialInterface* UGenAssetSubsystem::GetTerrainMaterial()
{
	const FString Name = TEXT("M_Terrain");
	if (TObjectPtr<UMaterialInterface>* Found = BaseMaterials.Find(Name))
	{
		return *Found;
	}
	UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *(FString(MaterialRoot) + Name + TEXT(".") + Name));
	if (!M)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Missing terrain material (run Tools/ue_setup.py)"));
		M = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	BaseMaterials.Add(Name, M);
	return M;
}

UMaterialInterface* UGenAssetSubsystem::GetSectionMaterial(const FGenSectionMaterial& S, const FLinearColor& HouseColor, bool bFadeable)
{
	// An opaque material can't drop pixels: a section that has to fade is drawn masked, its alpha ignored.
	const bool bMaskedForFade = bFadeable && S.Blend == EGenW3DBlend::Opaque;
	FString Key = FString::Printf(TEXT("%s|%d|%d|%s|%s|%.3f|%d"), *S.Texture.ToLower(), int32(S.Blend), S.bTwoSided ? 1 : 0,
		*S.Diffuse.ToString(), *S.Emissive.ToString(), S.Opacity, S.HouseColorMode);
	if (S.HouseColorMode != 0)
	{
		Key += TEXT("|") + HouseColor.ToString();
	}
	if (S.bUnderlay)
	{
		Key += FString::Printf(TEXT("|under:%s|%s"), *S.UnderTexture.ToLower(), *S.UnderTint.ToString());
	}
	if (bMaskedForFade)
	{
		Key += TEXT("|fade");
	}
	if (S.ScrollMode != 0)
	{
		Key += FString::Printf(TEXT("|scroll:%d|%s|%s|%s"), S.ScrollMode, *S.UVPerSec.ToString(), *S.UVStart.ToString(), *S.UVScale.ToString());
	}
	if (TObjectPtr<UMaterialInstanceDynamic>* Found = Materials.Find(Key))
	{
		return *Found;
	}

	UMaterialInterface* Base = GetBaseMaterial(bMaskedForFade ? EGenW3DBlend::AlphaTest : S.Blend, S.bTwoSided);
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	MID->SetScalarParameterValue(TEXT("IgnoreAlpha"), bMaskedForFade ? 1.f : 0.f);
	UTexture2D* Tex = S.Texture.IsEmpty() ? nullptr : GetTexture(S.Texture);
	if (Tex)
	{
		MID->SetTextureParameterValue(TEXT("BaseTexture"), Tex);
	}
	MID->SetScalarParameterValue(TEXT("UseTexture"), Tex ? 1.f : 0.f);
	FLinearColor Tint = S.Diffuse;
	Tint.A = S.Opacity;
	MID->SetVectorParameterValue(TEXT("Tint"), Tint);
	MID->SetVectorParameterValue(TEXT("Emissive"), S.Emissive);
	MID->SetVectorParameterValue(TEXT("HouseColor"), HouseColor);
	MID->SetScalarParameterValue(TEXT("HouseColorMode"), float(S.HouseColorMode));
	if (S.ScrollMode != 0)
	{
		MID->SetScalarParameterValue(TEXT("ScrollMode"), float(S.ScrollMode));
		MID->SetVectorParameterValue(TEXT("UVScroll"), FLinearColor(S.UVPerSec.X, S.UVPerSec.Y, S.UVStart.X, S.UVStart.Y));
		MID->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(S.UVScale.X, S.UVScale.Y, 0.f, 0.f));
	}
	if (S.bUnderlay)
	{
		// Without an environment map the first pass is a plain color: EnvTexture stays white.
		if (UTexture2D* Env = S.UnderTexture.IsEmpty() ? nullptr : GetTexture(S.UnderTexture))
		{
			MID->SetTextureParameterValue(TEXT("EnvTexture"), Env);
		}
		MID->SetVectorParameterValue(TEXT("UnderTint"), S.UnderTint);
		MID->SetScalarParameterValue(TEXT("UnderlayMode"), 1.f);
	}
	Materials.Add(Key, MID);
	return MID;
}

const FGenSkinData* UGenAssetSubsystem::GetSkinData(const FString& FileName, const FGenW3DMesh& M)
{
	const FString Key = FileName.ToLower() + TEXT("|") + M.FullName().ToLower();
	if (const TSharedPtr<FGenSkinData>* Found = SkinData.Find(Key))
	{
		return Found->Get();
	}
	TSharedPtr<FGenSkinData> Data = MakeShared<FGenSkinData>();
	SkinData.Add(Key, Data);

	const int32 NumVerts = M.Vertices.Num();
	if (NumVerts == 0 || M.Triangles.Num() == 0 || M.Influences.Num() != NumVerts)
	{
		return Data.Get();
	}
	const TArray<FVector2f>* UVs = nullptr;
	const TArray<FColor>* Colors = nullptr;
	GetVertexStreams(M, UVs, Colors);

	TArray<FGenSectionMaterial> SectionMats;
	TArray<int32> TriSection;
	BuildSections(M, SectionMats, TriSection);

	// Split vertices per section (a vertex shared by two sections is duplicated).
	Data->Sections.SetNum(SectionMats.Num());
	TArray<TMap<int32, int32>> Remap;
	Remap.SetNum(SectionMats.Num());
	for (int32 s = 0; s < SectionMats.Num(); ++s)
	{
		Data->Sections[s].Material = SectionMats[s];
	}
	for (int32 t = 0; t < M.Triangles.Num(); ++t)
	{
		const FIntVector3& T = M.Triangles[t];
		if (T.X < 0 || T.Y < 0 || T.Z < 0 || T.X >= NumVerts || T.Y >= NumVerts || T.Z >= NumVerts)
		{
			continue;
		}
		FGenSkinData::FSection& S = Data->Sections[TriSection[t]];
		TMap<int32, int32>& Map = Remap[TriSection[t]];
		for (const int32 V : { T.X, T.Y, T.Z })
		{
			int32* Local = Map.Find(V);
			if (!Local)
			{
				const int32 NewIdx = S.LocalPositions.Num();
				S.LocalPositions.Add(M.Vertices[V]);
				S.LocalNormals.Add(M.Normals.IsValidIndex(V) ? M.Normals[V] : FVector3f::UpVector);
				S.Bones.Add(M.Influences[V]);
				S.UVs.Add(UVs ? FVector2D((*UVs)[V].X, 1.f - (*UVs)[V].Y) : FVector2D::ZeroVector);
				S.Colors.Add(Colors ? ToLinear((*Colors)[V]) : FLinearColor::White);
				Local = &Map.Add(V, NewIdx);
			}
			S.Triangles.Add(*Local);
		}
	}
	return Data.Get();
}

FString UGenAssetSubsystem::DescribeMemory() const
{
	const double MB = 1024.0 * 1024.0;
	int64 MeshBytes = 0, AnimBytes = 0, AnimConstantBytes = 0, AnimIdleBytes = 0, SkinBytes = 0, TextureBytes = 0, StaticMeshBytes = 0;
	int32 NumMeshes = 0, NumAnims = 0, NumChannels = 0, NumConstant = 0, NumStatic = 0, NumMerged = 0;
	for (const TPair<FString, TSharedPtr<FGenW3DFile>>& It : W3DFiles)
	{
		if (!It.Value)
		{
			continue;
		}
		for (const FGenW3DMesh& M : It.Value->Meshes)
		{
			++NumMeshes;
			MeshBytes += M.Vertices.GetAllocatedSize() + M.Normals.GetAllocatedSize() + M.Triangles.GetAllocatedSize() + M.Influences.GetAllocatedSize();
			for (const FGenW3DMaterialPass& Pass : M.Passes)
			{
				MeshBytes += Pass.VertexMaterialIds.GetAllocatedSize() + Pass.ShaderIds.GetAllocatedSize() + Pass.DiffuseColors.GetAllocatedSize();
				for (const FGenW3DTextureStage& Stage : Pass.Stages)
				{
					MeshBytes += Stage.TextureIds.GetAllocatedSize() + Stage.TexCoords.GetAllocatedSize();
				}
			}
		}
		for (const TSharedPtr<FGenW3DAnimation>& A : It.Value->Animations)
		{
			if (!A)
			{
				continue;
			}
			++NumAnims;
			AnimBytes += A->Channels.GetAllocatedSize() + A->PivotLookup.GetAllocatedSize();
			for (const FGenW3DChannel& Ch : A->Channels)
			{
				++NumChannels;
				const int64 Bytes = Ch.Scalars.GetAllocatedSize() + Ch.Quats.GetAllocatedSize() + Ch.Bits.GetAllocatedSize();
				AnimBytes += Bytes;
				bool bConstant = true, bIdle = true;
				for (int32 i = 0; i < Ch.Scalars.Num(); ++i) { bConstant &= Ch.Scalars[i] == Ch.Scalars[0]; bIdle &= FMath::Abs(Ch.Scalars[i]) <= 1.e-4f; }
				for (int32 i = 0; i < Ch.Quats.Num(); ++i)
				{
					bConstant &= Ch.Quats[i].Equals(Ch.Quats[0], 1.e-6f);
					bIdle &= FMath::Abs(Ch.Quats[i].X) <= 1.e-4f && FMath::Abs(Ch.Quats[i].Y) <= 1.e-4f && FMath::Abs(Ch.Quats[i].Z) <= 1.e-4f;
				}
				for (int32 i = 0; i < Ch.Bits.Num(); ++i) { bConstant &= Ch.Bits[i] == Ch.Bits[0]; bIdle &= Ch.Bits[i] != 0; }
				NumConstant += bConstant ? 1 : 0;
				AnimConstantBytes += bConstant ? Bytes : 0;
				AnimIdleBytes += bIdle ? Bytes : 0;
			}
		}
	}
	for (const TPair<FString, TSharedPtr<FGenSkinData>>& It : SkinData)
	{
		if (!It.Value)
		{
			continue;
		}
		for (const FGenSkinData::FSection& S : It.Value->Sections)
		{
			SkinBytes += S.LocalPositions.GetAllocatedSize() + S.LocalNormals.GetAllocatedSize() + S.Bones.GetAllocatedSize() + S.Triangles.GetAllocatedSize()
				+ S.UVs.GetAllocatedSize() + S.Colors.GetAllocatedSize();
		}
	}
	for (const TPair<FString, FGenBuiltMesh>& It : BuiltMeshes)
	{
		if (It.Value.Mesh)
		{
			++NumStatic;
			NumMerged += It.Key.Contains(TEXT("|merged")) ? 1 : 0;
			StaticMeshBytes += It.Value.Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
		}
	}
	for (const TPair<FString, TObjectPtr<UTexture2D>>& It : Textures)
	{
		if (It.Value)
		{
			TextureBytes += It.Value->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
		}
	}
	return FString::Printf(TEXT("%d model files: %d meshes %.1f MB as read; %d animations %.1f MB in %d channels, of them %d constant (%.1f MB), %.1f MB holding their bone where it is; ")
		TEXT("skins %.1f MB; %d built meshes (%d merged) %.1f MB; %d textures %.1f MB; %d materials; %d plans"),
		W3DFiles.Num(), NumMeshes, double(MeshBytes) / MB, NumAnims, double(AnimBytes) / MB, NumChannels, NumConstant, double(AnimConstantBytes) / MB, double(AnimIdleBytes) / MB,
		double(SkinBytes) / MB, NumStatic, NumMerged, double(StaticMeshBytes) / MB, Textures.Num(), double(TextureBytes) / MB, Materials.Num(), ModelPlans.Num());
}

static FAutoConsoleCommand CmdGenAssetMemory(
	TEXT("gen.AssetMemory"),
	TEXT("Logs what the models, animations and textures read from the game data so far take up."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (const UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get())
		{
			UE_LOG(LogGenerals, Log, TEXT("Asset memory: %s"), *Assets->DescribeMemory());
		}
	}));

static TAutoConsoleVariable<int32> CVarGenMergeParts(
	TEXT("gen.MergeParts"), 1,
	TEXT("1: the meshes of a model that never move against it and are never hidden are drawn as one. 0: each by itself (for models made from then on)."));

namespace
{
	FString StripMeshContainer(const FString& FullName)
	{
		FString Left, Right;
		return FullName.Split(TEXT("."), &Left, &Right) ? Right : FullName;
	}

	/** Something of the mesh is drawn blended over what is behind it (glass, lights, smoke). */
	bool IsBlended(const FGenW3DMesh& M)
	{
		TArray<FGenSectionMaterial> Sections;
		TArray<int32> TriSection;
		BuildSections(M, Sections, TriSection);
		for (const FGenSectionMaterial& S : Sections)
		{
			if (S.Blend != EGenW3DBlend::Opaque && S.Blend != EGenW3DBlend::AlphaTest)
			{
				return true;
			}
		}
		return false;
	}

	/** Two sections with the same key are drawn with the same material, whatever the house colour. */
	FString SectionKey(const FGenSectionMaterial& S)
	{
		return FString::Printf(TEXT("%s|%d|%d|%s|%s|%.3f|%d|%d|%s|%s"), *S.Texture.ToLower(), int32(S.Blend), S.bTwoSided ? 1 : 0, *S.Diffuse.ToString(),
			*S.Emissive.ToString(), S.Opacity, int32(S.HouseColorMode), S.bUnderlay ? 1 : 0, *S.UnderTexture.ToLower(), *S.UnderTint.ToString());
	}
}

void UGenAssetSubsystem::GetModelSubObjects(const FGenW3DFile& File, TArray<FGenW3DSubObject>& OutSubs, TSharedPtr<const FGenW3DHierarchy>& OutHierarchy)
{
	OutSubs.Reset();
	if (File.HLod)
	{
		OutHierarchy = FindHierarchy(File.HLod->HierarchyName, &File);
		OutSubs = File.HLod->SubObjects;
		OutSubs.Append(File.HLod->Aggregates);
	}
	else
	{
		OutHierarchy = File.Hierarchy;
		for (const FGenW3DMesh& M : File.Meshes)
		{
			OutSubs.Add({ 0, M.FullName() });
		}
	}
}

uint32 UGenAssetSubsystem::GetMergeInfoVersion(const FString& ModelName) const
{
	const FGenModelMergeInfo* Info = MergeInfos.Find(ModelName.ToLower());
	return Info ? Info->Version : 1;
}

void UGenAssetSubsystem::NoteToggledSubObject(const FString& ModelName, const FString& SubObjectName)
{
	if (ModelName.IsEmpty() || SubObjectName.IsEmpty())
	{
		return;
	}
	FGenModelMergeInfo& Info = MergeInfos.FindOrAdd(ModelName.ToLower());
	bool bKnown = false;
	Info.ToggledNames.Add(SubObjectName.ToLower(), &bKnown);
	Info.Version += bKnown ? 0 : 1;
}

void UGenAssetSubsystem::NoteMovedBonePrefix(const FString& ModelName, const FString& Prefix)
{
	if (ModelName.IsEmpty() || Prefix.IsEmpty())
	{
		return;
	}
	FGenModelMergeInfo& Info = MergeInfos.FindOrAdd(ModelName.ToLower());
	bool bKnown = false;
	Info.BonePrefixes.Add(Prefix.ToLower(), &bKnown);
	Info.Version += bKnown ? 0 : 1;
}

void UGenAssetSubsystem::NoteMovedPivots(const FString& ModelName, TConstArrayView<int32> Pivots)
{
	if (ModelName.IsEmpty())
	{
		return;
	}
	FGenModelMergeInfo& Info = MergeInfos.FindOrAdd(ModelName.ToLower());
	bool bNews = false;
	for (int32 Pivot : Pivots)
	{
		bool bKnown = false;
		Info.MovingPivots.Add(Pivot, &bKnown);
		bNews |= !bKnown;
	}
	Info.Version += bNews ? 1 : 0;
}

void UGenAssetSubsystem::NoteAnimation(const FString& ModelName, const FGenW3DAnimation& Animation)
{
	if (ModelName.IsEmpty())
	{
		return;
	}
	FGenModelMergeInfo& Info = MergeInfos.FindOrAdd(ModelName.ToLower());
	bool bSeen = false;
	Info.SeenAnimations.Add(Animation.HierarchyName.ToLower() + TEXT(".") + Animation.Name.ToLower(), &bSeen);
	if (bSeen)
	{
		return;
	}
	// Every pivot the animation moves, turns or hides (the root isn't animated). Having a channel isn't
	// enough: the exporter wrote one for every bone it knew of, and most of them hold the bone where it is.
	auto ChannelMoves = [&Animation](int32 Index)
	{
		if (!Animation.Channels.IsValidIndex(Index))
		{
			return false;
		}
		const FGenW3DChannel& Ch = Animation.Channels[Index];
		for (const float Value : Ch.Scalars)
		{
			if (FMath::Abs(Value) > 1.e-4f) { return true; }
		}
		for (const FQuat4f& Q : Ch.Quats)
		{
			if (FMath::Abs(Q.X) > 1.e-4f || FMath::Abs(Q.Y) > 1.e-4f || FMath::Abs(Q.Z) > 1.e-4f) { return true; }
		}
		if (Ch.Kind == FGenW3DChannel::EKind::Vis)
		{
			for (const uint8 Bit : Ch.Bits)
			{
				if (Bit == 0) { return true; }
			}
			if (!Ch.DefaultVis && (Ch.FirstFrame > 0 || Ch.FirstFrame + Ch.Bits.Num() < Animation.NumFrames)) { return true; }
		}
		return false;
	};
	bool bNews = false;
	for (int32 Pivot = 1; Pivot < Animation.NumPivots(); ++Pivot)
	{
		bool bHasChannel = false;
		for (int32 Kind = 0; Kind < 5 && !bHasChannel; ++Kind)
		{
			bHasChannel = ChannelMoves(Animation.PivotLookup[Pivot * 5 + Kind]);
		}
		if (bHasChannel)
		{
			bool bKnown = false;
			Info.MovingPivots.Add(Pivot, &bKnown);
			bNews |= !bKnown;
		}
	}
	Info.Version += bNews ? 1 : 0;
}

void UGenAssetSubsystem::NotePossibleAnimation(const FString& ModelName, const FString& QualifiedName)
{
	if (ModelName.IsEmpty() || QualifiedName.IsEmpty())
	{
		return;
	}
	FGenModelMergeInfo& Info = MergeInfos.FindOrAdd(ModelName.ToLower());
	if (!Info.PossibleAnimations.ContainsByPredicate([&QualifiedName](const FString& Known) { return Known.Equals(QualifiedName, ESearchCase::IgnoreCase); }))
	{
		Info.PossibleAnimations.Add(QualifiedName);
		++Info.Version;
	}
}

void UGenAssetSubsystem::FindMergeParts(const FGenW3DFile& File, const FGenW3DHierarchy* Hierarchy, const TArray<FGenW3DSubObject>& Subs, const FGenModelMergeInfo& Info,
	bool bFinal, TArray<FGenModelPlan::FPart>& OutParts) const
{
	OutParts.Reset();
	const int32 NumPivots = Hierarchy ? FMath::Max(1, Hierarchy->Pivots.Num()) : 1;
	// The pivots that move: seen to, or named by a draw module - by their own name or as "<prefix>01,
	// 02, ..." - and everything that hangs on them.
	TArray<bool, TInlineAllocator<128>> bMoves;
	bMoves.Init(false, NumPivots);
	for (int32 Pivot : Info.MovingPivots)
	{
		if (bMoves.IsValidIndex(Pivot)) { bMoves[Pivot] = true; }
	}
	auto MarkNamed = [&](const FString& Name)
	{
		bool bFound = false;
		const int32 Pivot = Hierarchy ? Hierarchy->FindPivot(Name) : INDEX_NONE;
		if (bMoves.IsValidIndex(Pivot))
		{
			bMoves[Pivot] = true;
			bFound = true;
		}
		for (const FGenW3DSubObject& SO : Subs)
		{
			if (StripMeshContainer(SO.Name).Equals(Name, ESearchCase::IgnoreCase))
			{
				bMoves[FMath::Clamp(SO.Bone, 0, NumPivots - 1)] = true;
				bFound = true;
			}
		}
		return bFound;
	};
	for (const FString& Prefix : Info.BonePrefixes)
	{
		for (int32 i = 1; i <= 99; ++i)
		{
			if (!MarkNamed(FString::Printf(TEXT("%s%02d"), *Prefix, i))) { break; }
		}
		MarkNamed(Prefix);
	}
	if (Hierarchy)
	{
		for (int32 i = 1; i < Hierarchy->Pivots.Num(); ++i)
		{
			const int32 Parent = Hierarchy->Pivots[i].Parent;
			if (Parent >= 0 && Parent < i && bMoves[Parent]) { bMoves[i] = true; }
		}
	}
	for (int32 s = 0; s < Subs.Num(); ++s)
	{
		const FGenW3DMesh* Mesh = File.FindMesh(Subs[s].Name);
		if (!Mesh || Mesh->IsHidden() || Mesh->Vertices.Num() == 0 || Mesh->Triangles.Num() == 0)
		{
			continue;
		}
		if (Mesh->IsSkin() && Hierarchy && Mesh->Influences.Num() == Mesh->Vertices.Num())
		{
			continue; // a skin: its vertices follow the bones
		}
		const FString Name = StripMeshContainer(Subs[s].Name);
		const int32 Bone = FMath::Clamp(Subs[s].Bone, 0, NumPivots - 1);
		if (bMoves[Bone] || Name.Contains(TEXT("HEADLIGHT"), ESearchCase::IgnoreCase) || Info.ToggledNames.Contains(Name.ToLower()))
		{
			continue;
		}
		if (bFinal && IsBlended(*Mesh))
		{
			continue; // see-through things are drawn back to front, each by where it is: it stays a thing of its own
		}
		OutParts.Add({ s, Bone, Name });
	}
}

const FGenModelPlan& UGenAssetSubsystem::GetModelPlan(const FString& ModelName)
{
	const FString Key = ModelName.ToLower();
	FGenModelMergeInfo& Info = MergeInfos.FindOrAdd(Key); // (nothing below adds a model to that map)
	if (const TUniquePtr<FGenModelPlan>* Slot = ModelPlans.Find(Key))
	{
		if (*Slot && (*Slot)->InfoVersion == Info.Version)
		{
			return **Slot;
		}
	}
	GEN_PERF_SCOPE("R.ModelPlan");
	TUniquePtr<FGenModelPlan> NewPlan = MakeUnique<FGenModelPlan>();
	FGenModelPlan& Plan = *NewPlan;
	static const bool bNoMerge = FParse::Param(FCommandLine::Get(), TEXT("GenNoMerge"));
	TSharedPtr<const FGenW3DFile> File = LoadW3D(ModelName);
	if (File)
	{
		TArray<FGenW3DSubObject> Subs;
		TSharedPtr<const FGenW3DHierarchy> Hierarchy;
		GetModelSubObjects(*File, Subs, Hierarchy);
		Plan.bIsMerged.Init(false, Subs.Num());
		if (!bNoMerge && CVarGenMergeParts.GetValueOnAnyThread() != 0)
		{
			// The animations a draw module may play on the model are read for the bones they move - while
			// there is still something left that could be one mesh (a soldier's first animation moves every
			// bone he has: the other thirty needn't be read for this).
			while (Info.PossibleLooked < Info.PossibleAnimations.Num())
			{
				FindMergeParts(*File, Hierarchy.Get(), Subs, Info, false, Plan.Merged);
				if (Plan.Merged.Num() < 2)
				{
					break;
				}
				const FString AnimName = Info.PossibleAnimations[Info.PossibleLooked++];
				if (TSharedPtr<const FGenW3DAnimation> Anim = FindAnimation(AnimName))
				{
					NoteAnimation(ModelName, *Anim);
				}
			}
			FindMergeParts(*File, Hierarchy.Get(), Subs, Info, true, Plan.Merged);
			if (Plan.Merged.Num() >= 2 && GetMergedMesh(ModelName, *File, Hierarchy.Get(), Subs, Plan.Merged, Plan.MergedMesh))
			{
				for (const FGenModelPlan::FPart& Part : Plan.Merged)
				{
					Plan.bIsMerged[Part.SubIndex] = true;
				}
			}
			else
			{
				Plan.Merged.Reset();
				Plan.MergedMesh = FGenBuiltMesh();
			}
		}
	}
	Plan.InfoVersion = Info.Version;
	static const bool bTellPlans = FParse::Param(FCommandLine::Get(), TEXT("GenPlans"));
	if (bTellPlans && File)
	{
		TArray<FGenW3DSubObject> Subs;
		TSharedPtr<const FGenW3DHierarchy> Hierarchy;
		GetModelSubObjects(*File, Subs, Hierarchy);
		TArray<FGenModelPlan::FPart> Rigid;
		FindMergeParts(*File, Hierarchy.Get(), Subs, FGenModelMergeInfo(), false, Rigid);
		FString Out;
		for (const FGenModelPlan::FPart& Part : Rigid)
		{
			if (!Plan.bIsMerged[Part.SubIndex])
			{
				const FGenW3DMesh* Mesh = File->FindMesh(Subs[Part.SubIndex].Name);
				Out += FString::Printf(TEXT(" %s(bone %d %s%s)"), *Part.Name, Part.Bone,
					Hierarchy && Hierarchy->Pivots.IsValidIndex(Part.Bone) ? *Hierarchy->Pivots[Part.Bone].Name : TEXT("?"),
					Info.ToggledNames.Contains(Part.Name.ToLower()) ? TEXT(", shown/hidden by name") : (Mesh && IsBlended(*Mesh) ? TEXT(", blended") : TEXT(", moves")));
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Plan %s v%u: %d of %d rigid visible meshes are one; anims read %d of %d; left out:%s"), *ModelName, Info.Version, Plan.Merged.Num(), Rigid.Num(),
			Info.PossibleLooked, Info.PossibleAnimations.Num(), *Out);
	}
	TUniquePtr<FGenModelPlan>& Slot = ModelPlans.FindOrAdd(Key);
	Slot = MoveTemp(NewPlan);
	return *Slot;
}

bool UGenAssetSubsystem::GetMergedMesh(const FString& FileName, const FGenW3DFile& File, const FGenW3DHierarchy* Hierarchy, const TArray<FGenW3DSubObject>& Subs,
	const TArray<FGenModelPlan::FPart>& Parts, FGenBuiltMesh& Out)
{
	FString Key = FileName.ToLower() + TEXT("|merged");
	for (const FGenModelPlan::FPart& Part : Parts)
	{
		Key += FString::Printf(TEXT("|%d"), Part.SubIndex);
	}
	if (const FGenBuiltMesh* Found = BuiltMeshes.Find(Key))
	{
		Out = *Found;
		return Out.Mesh != nullptr;
	}

	// Each bone where the model's base pose has it (as UGenModelComponent poses a model nothing is moving).
	TArray<FTransform3f> BoneWorld;
	if (Hierarchy)
	{
		BoneWorld.SetNum(Hierarchy->Pivots.Num());
		for (int32 i = 0; i < Hierarchy->Pivots.Num(); ++i)
		{
			const FGenW3DPivot& P = Hierarchy->Pivots[i];
			const FTransform3f Local(P.Rotation.GetNormalized(), P.Translation);
			BoneWorld[i] = P.Parent >= 0 && P.Parent < i ? Local * BoneWorld[P.Parent] : Local;
		}
	}

	FGenBuiltMesh Built;
	Built.bSkinBakedToModelSpace = true;
	FMeshDescription MD;
	FStaticMeshAttributes Attr(MD);
	Attr.Register();
	TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> GroupNames = Attr.GetPolygonGroupMaterialSlotNames();
	VUV.SetNumChannels(1);

	TMap<FString, int32> SectionOfKey;
	TArray<FPolygonGroupID> Groups;
	TArray<FVertexInstanceID> Instances;
	for (const FGenModelPlan::FPart& Part : Parts)
	{
		const FGenW3DMesh* MeshPtr = Subs.IsValidIndex(Part.SubIndex) ? File.FindMesh(Subs[Part.SubIndex].Name) : nullptr;
		if (!MeshPtr)
		{
			continue;
		}
		const FGenW3DMesh& M = *MeshPtr;
		const int32 NumVerts = M.Vertices.Num();
		const FTransform3f Xf = BoneWorld.IsValidIndex(Part.Bone) ? BoneWorld[Part.Bone] : FTransform3f::Identity;
		const TArray<FVector2f>* UVs = nullptr;
		const TArray<FColor>* Colors = nullptr;
		GetVertexStreams(M, UVs, Colors);
		TArray<FGenSectionMaterial> Sections;
		TArray<int32> TriSection;
		BuildSections(M, Sections, TriSection);
		// The mesh's sections among the merged mesh's: one for each material, whichever mesh it comes from.
		TArray<int32> MergedSection;
		for (const FGenSectionMaterial& S : Sections)
		{
			const FString SKey = SectionKey(S);
			int32 Index = INDEX_NONE;
			if (const int32* Found = SectionOfKey.Find(SKey))
			{
				Index = *Found;
			}
			else
			{
				Index = Built.Sections.Add(S);
				SectionOfKey.Add(SKey, Index);
				const FPolygonGroupID G = MD.CreatePolygonGroup();
				GroupNames[G] = FName(*FString::Printf(TEXT("S%d"), Index));
				Groups.Add(G);
			}
			MergedSection.Add(Index);
		}
		Instances.SetNumUninitialized(NumVerts, EAllowShrinking::No);
		for (int32 v = 0; v < NumVerts; ++v)
		{
			const FVertexID VID = MD.CreateVertex();
			const FVector3f P = Xf.TransformPosition(M.Vertices[v]);
			VPos[VID] = GenCoords::ToUEf(P.X, P.Y, P.Z);
			const FVertexInstanceID IID = MD.CreateVertexInstance(VID);
			const FVector3f Nw = Xf.TransformVectorNoScale(M.Normals.IsValidIndex(v) ? M.Normals[v] : FVector3f::ZeroVector);
			const FVector3f N = GenCoords::DirToUE(Nw.X, Nw.Y, Nw.Z).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
			VNrm[IID] = N;
			VTan[IID] = AnyPerpendicular(N);
			VSign[IID] = 1.f;
			VUV.Set(IID, 0, UVs ? FVector2f((*UVs)[v].X, 1.f - (*UVs)[v].Y) : FVector2f::ZeroVector);
			VCol[IID] = Colors ? FVector4f(ToLinear((*Colors)[v])) : FVector4f(1.f, 1.f, 1.f, 1.f);
			Instances[v] = IID;
		}
		for (int32 t = 0; t < M.Triangles.Num(); ++t)
		{
			const FIntVector3& T = M.Triangles[t];
			if (T.X < 0 || T.Y < 0 || T.Z < 0 || T.X >= NumVerts || T.Y >= NumVerts || T.Z >= NumVerts || T.X == T.Y || T.Y == T.Z || T.X == T.Z
				|| !MergedSection.IsValidIndex(TriSection[t]))
			{
				continue;
			}
			const FVertexInstanceID Tri[3] = { Instances[T.X], Instances[T.Y], Instances[T.Z] };
			MD.CreateTriangle(Groups[MergedSection[TriSection[t]]], Tri);
		}
	}
	if (MD.Triangles().Num() > 0)
	{
		FString MeshName = FString::Printf(TEXT("SM_%s_merged"), *FPaths::GetBaseFilename(FileName));
		for (TCHAR& C : MeshName)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('_')) { C = TEXT('_'); }
		}
		UStaticMesh* SM = NewObject<UStaticMesh>(this, MakeUniqueObjectName(this, UStaticMesh::StaticClass(), FName(*MeshName)), RF_Transient);
		for (int32 s = 0; s < Built.Sections.Num(); ++s)
		{
			const FName SlotName(*FString::Printf(TEXT("S%d"), s));
			SM->GetStaticMaterials().Add(GenRuntimeMaterialSlot(GetBaseMaterial(Built.Sections[s].Blend, Built.Sections[s].bTwoSided), SlotName));
		}
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = false;
		SM->BuildFromMeshDescriptions({ &MD }, Params);
		Built.Mesh = SM;
		MeshRefs.Add(SM);
	}
	BuiltMeshes.Add(Key, Built);
	Out = Built;
	return Out.Mesh != nullptr;
}

const FGenBuiltMesh* UGenAssetSubsystem::GetBuiltMesh(const FString& FileName, const FGenW3DMesh& M, const FGenW3DHierarchy* Hierarchy)
{
	const FString Key = FileName.ToLower() + TEXT("|") + M.FullName().ToLower();
	if (const FGenBuiltMesh* Found = BuiltMeshes.Find(Key))
	{
		return Found;
	}
	FGenBuiltMesh& Out = BuiltMeshes.Add(Key);

	const int32 NumVerts = M.Vertices.Num();
	if (NumVerts == 0 || M.Triangles.Num() == 0)
	{
		return &Out;
	}

	// Positions/normals: bone-local, except skins which are baked with the base pose.
	TArray<FVector3f> Positions = M.Vertices;
	TArray<FVector3f> Normals = M.Normals;
	Normals.SetNumZeroed(NumVerts);
	if (M.IsSkin() && Hierarchy && M.Influences.Num() == NumVerts)
	{
		TArray<FTransform3f> BoneWorld;
		ComputeBasePose(*Hierarchy, BoneWorld);
		for (int32 v = 0; v < NumVerts; ++v)
		{
			const int32 Bone = M.Influences[v];
			if (BoneWorld.IsValidIndex(Bone))
			{
				Positions[v] = BoneWorld[Bone].TransformPosition(Positions[v]);
				Normals[v] = BoneWorld[Bone].TransformVectorNoScale(Normals[v]);
			}
		}
		Out.bSkinBakedToModelSpace = true;
	}

	const TArray<FVector2f>* UVs = nullptr;
	const TArray<FColor>* Colors = nullptr;
	GetVertexStreams(M, UVs, Colors);

	TArray<int32> TriSection;
	BuildSections(M, Out.Sections, TriSection);

	// Mesh description: one vertex instance per W3D vertex.
	FMeshDescription MD;
	FStaticMeshAttributes Attr(MD);
	Attr.Register();
	TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> GroupNames = Attr.GetPolygonGroupMaterialSlotNames();
	VUV.SetNumChannels(1);

	MD.ReserveNewVertices(NumVerts);
	MD.ReserveNewVertexInstances(NumVerts);
	MD.ReserveNewTriangles(M.Triangles.Num());
	TArray<FVertexInstanceID> Instances;
	Instances.SetNumUninitialized(NumVerts);
	for (int32 v = 0; v < NumVerts; ++v)
	{
		const FVertexID VID = MD.CreateVertex();
		const FVector3f& P = Positions[v];
		VPos[VID] = GenCoords::ToUEf(P.X, P.Y, P.Z);
		const FVertexInstanceID IID = MD.CreateVertexInstance(VID);
		const FVector3f N = GenCoords::DirToUE(Normals[v].X, Normals[v].Y, Normals[v].Z).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
		VNrm[IID] = N;
		VTan[IID] = AnyPerpendicular(N);
		VSign[IID] = 1.f;
		// The original loader flips V (meshmdlio.cpp: 1 - v); Unreal uses the same top-left origin as D3D.
		VUV.Set(IID, 0, UVs ? FVector2f((*UVs)[v].X, 1.f - (*UVs)[v].Y) : FVector2f::ZeroVector);
		VCol[IID] = Colors ? FVector4f(ToLinear((*Colors)[v])) : FVector4f(1.f, 1.f, 1.f, 1.f);
		Instances[v] = IID;
	}

	TArray<FPolygonGroupID> Groups;
	for (int32 s = 0; s < Out.Sections.Num(); ++s)
	{
		const FPolygonGroupID G = MD.CreatePolygonGroup();
		GroupNames[G] = FName(*FString::Printf(TEXT("S%d"), s));
		Groups.Add(G);
	}
	for (int32 t = 0; t < M.Triangles.Num(); ++t)
	{
		const FIntVector3& T = M.Triangles[t];
		if (T.X < 0 || T.Y < 0 || T.Z < 0 || T.X >= NumVerts || T.Y >= NumVerts || T.Z >= NumVerts || T.X == T.Y || T.Y == T.Z || T.X == T.Z)
		{
			continue;
		}
		const FVertexInstanceID Tri[3] = { Instances[T.X], Instances[T.Y], Instances[T.Z] };
		MD.CreateTriangle(Groups[TriSection[t]], Tri);
	}

	FString MeshName = FString::Printf(TEXT("SM_%s_%s"), *FPaths::GetBaseFilename(FileName), *M.Name);
	for (TCHAR& C : MeshName)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_')) { C = TEXT('_'); }
	}
	UStaticMesh* SM = NewObject<UStaticMesh>(this, MakeUniqueObjectName(this, UStaticMesh::StaticClass(), FName(*MeshName)), RF_Transient);
	for (int32 s = 0; s < Out.Sections.Num(); ++s)
	{
		const FName Slot(*FString::Printf(TEXT("S%d"), s));
		SM->GetStaticMaterials().Add(GenRuntimeMaterialSlot(GetBaseMaterial(Out.Sections[s].Blend, Out.Sections[s].bTwoSided), Slot));
	}
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = false;
	SM->BuildFromMeshDescriptions({ &MD }, Params);

	Out.Mesh = SM;
	MeshRefs.Add(SM);
	return &Out;
}
