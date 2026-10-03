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

	/** First-pass UVs (stage 0) and per-vertex diffuse colors, when present for every vertex. */
	void GetVertexStreams(const FGenW3DMesh& M, const TArray<FVector2f>*& OutUVs, const TArray<FColor>*& OutColors)
	{
		const int32 NumVerts = M.Vertices.Num();
		const FGenW3DMaterialPass* Pass = M.Passes.Num() > 0 ? &M.Passes[0] : nullptr;
		const FGenW3DTextureStage* Stage = Pass && Pass->Stages.Num() > 0 ? &Pass->Stages[0] : nullptr;
		OutUVs = Stage && Stage->TexCoords.Num() == NumVerts ? &Stage->TexCoords : nullptr;
		OutColors = Pass && Pass->DiffuseColors.Num() == NumVerts ? &Pass->DiffuseColors : nullptr;
	}

	/** Groups triangles into sections by (shader, texture, vertex material) and derives their materials. */
	void BuildSections(const FGenW3DMesh& M, TArray<FGenSectionMaterial>& OutSections, TArray<int32>& OutTriSection)
	{
		const FGenW3DMaterialPass* Pass = M.Passes.Num() > 0 ? &M.Passes[0] : nullptr;
		const FGenW3DTextureStage* Stage = Pass && Pass->Stages.Num() > 0 ? &Pass->Stages[0] : nullptr;
		auto PickId = [](const TArray<uint32>& Ids, int32 Index) -> int32
		{
			if (Ids.Num() == 0) { return INDEX_NONE; }
			return int32(Ids.Num() > 1 && Ids.IsValidIndex(Index) ? Ids[Index] : Ids[0]);
		};

		TMap<FIntVector, int32> SectionOf;
		OutTriSection.SetNumUninitialized(M.Triangles.Num());
		for (int32 t = 0; t < M.Triangles.Num(); ++t)
		{
			const int32 ShaderId = Pass ? PickId(Pass->ShaderIds, t) : INDEX_NONE;
			const int32 TexId = Stage ? PickId(Stage->TextureIds, t) : INDEX_NONE;
			const int32 VMatId = Pass ? PickId(Pass->VertexMaterialIds, M.Triangles[t].X) : INDEX_NONE;
			const FIntVector SKey(ShaderId, TexId, VMatId);
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
