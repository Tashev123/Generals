#include "W3D/GenAssets.h"
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
	if (!FGenFileSystem::Get().ReadFile(FString::Printf(TEXT("Art\\W3D\\%s.w3d"), *Name), Bytes) || !GenW3D::Parse(Bytes, *File))
	{
		UE_LOG(LogGenerals, Warning, TEXT("W3D '%s' not found or invalid"), *Name);
		MissingW3D.Add(Key);
		return nullptr;
	}
	W3DFiles.Add(Key, File);
	return File;
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

int32 UGenAssetSubsystem::GetPristineBones(const FString& ModelName, const FString& Prefix, int32 StartIndex, int32 MaxBones, TArray<FTransform3f>& OutBones)
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
	// Base pose in model space: each pivot's own transform on top of its parent's.
	auto ModelSpace = [&Hierarchy](int32 Pivot)
	{
		FTransform3f Xf = FTransform3f::Identity;
		for (int32 Guard = 0; Hierarchy->Pivots.IsValidIndex(Pivot) && Guard < 256; ++Guard)
		{
			const FGenW3DPivot& P = Hierarchy->Pivots[Pivot];
			Xf = Xf * FTransform3f(P.Rotation.GetNormalized(), P.Translation);
			Pivot = P.Parent;
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

UMaterialInterface* UGenAssetSubsystem::GetSectionMaterial(const FGenSectionMaterial& S, const FLinearColor& HouseColor)
{
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
	if (TObjectPtr<UMaterialInstanceDynamic>* Found = Materials.Find(Key))
	{
		return *Found;
	}

	UMaterialInterface* Base = GetBaseMaterial(S.Blend, S.bTwoSided);
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
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
		SM->GetStaticMaterials().Add(FStaticMaterial(GetBaseMaterial(Out.Sections[s].Blend, Out.Sections[s].bTwoSided), Slot, Slot));
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
