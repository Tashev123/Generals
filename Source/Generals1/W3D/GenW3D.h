#pragma once

#include "CoreMinimal.h"

/**
 * In-memory form of a W3D file (Libraries/Source/WWVegas/WW3D2/w3d_file.h).
 * Only what rendering, animation and gameplay need is kept.
 */
namespace GenW3D
{
	enum : uint32
	{
		MeshFlagHidden = 0x00001000,
		MeshFlagTwoSided = 0x00002000,
		MeshFlagCastShadow = 0x00008000,
		MeshGeometryMask = 0x00FF0000,
		MeshGeometryNormal = 0x00000000,
		MeshGeometryCameraAligned = 0x00010000,
		MeshGeometrySkin = 0x00020000,
		MeshGeometryCameraOriented = 0x00060000,
	};
}

enum class EGenW3DBlend : uint8
{
	Opaque,
	AlphaTest,      // opaque + alpha test
	Alpha,          // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
	Additive,       // ONE, ONE  or SRC_ALPHA, ONE
	Multiply,       // ZERO, SRC_COLOR
};

struct FGenW3DShader
{
	uint8 DepthCompare = 3;
	uint8 DepthMask = 1;
	uint8 DestBlend = 0;
	uint8 PriGradient = 1;
	uint8 SecGradient = 0;
	uint8 SrcBlend = 1;
	uint8 Texturing = 0;
	uint8 DetailColorFunc = 0;
	uint8 DetailAlphaFunc = 0;
	uint8 AlphaTest = 0;

	EGenW3DBlend GetBlend() const;
};

struct FGenW3DVertexMaterial
{
	FString Name;
	uint32 Attributes = 0;
	FColor Ambient = FColor::White;
	FColor Diffuse = FColor::White;
	FColor Specular = FColor::Black;
	FColor Emissive = FColor::Black;
	float Shininess = 1.f;
	float Opacity = 1.f;
	float Translucency = 0.f;
	FString MapperArgs0;
};

struct FGenW3DTexture
{
	FString Name;
	uint16 Attributes = 0;
	uint16 AnimType = 0;
	uint32 FrameCount = 1;
	float FrameRate = 0.f;
};

struct FGenW3DTextureStage
{
	TArray<uint32> TextureIds;             // 1 entry, or one per triangle
	TArray<FVector2f> TexCoords;           // per vertex
};

struct FGenW3DMaterialPass
{
	TArray<uint32> VertexMaterialIds;      // 1 entry, or one per vertex
	TArray<uint32> ShaderIds;              // 1 entry, or one per triangle
	TArray<FColor> DiffuseColors;          // DCG, per vertex (may be empty)
	TArray<FGenW3DTextureStage> Stages;
};

struct FGenW3DMesh
{
	FString Name;                          // mesh name (no container)
	FString Container;
	uint32 Attributes = 0;
	int32 SortLevel = 0;
	TArray<FVector3f> Vertices;
	TArray<FVector3f> Normals;
	TArray<FIntVector3> Triangles;
	TArray<uint16> Influences;             // bone per vertex (skins)
	TArray<FGenW3DVertexMaterial> VertexMaterials;
	TArray<FGenW3DShader> Shaders;
	TArray<FGenW3DTexture> Textures;
	TArray<FGenW3DMaterialPass> Passes;
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;

	FString FullName() const { return Container.IsEmpty() ? Name : Container + TEXT(".") + Name; }
	uint32 GeometryType() const { return Attributes & GenW3D::MeshGeometryMask; }
	bool IsSkin() const { return GeometryType() == GenW3D::MeshGeometrySkin; }
	bool IsHidden() const { return (Attributes & GenW3D::MeshFlagHidden) != 0; }
	bool IsTwoSided() const { return (Attributes & GenW3D::MeshFlagTwoSided) != 0; }
};

struct FGenW3DPivot
{
	FString Name;
	int32 Parent = -1;
	FVector3f Translation = FVector3f::ZeroVector;
	FQuat4f Rotation = FQuat4f::Identity;  // W3D frame (x,y,z,w)
};

struct FGenW3DHierarchy
{
	FString Name;
	FVector3f Center = FVector3f::ZeroVector;
	TArray<FGenW3DPivot> Pivots;

	int32 FindPivot(const FString& PivotName) const;
};

struct FGenW3DSubObject
{
	int32 Bone = 0;
	FString Name;                          // "CONTAINER.MESH"
};

struct FGenW3DHLod
{
	FString Name;
	FString HierarchyName;
	TArray<FGenW3DSubObject> SubObjects;   // highest detail LOD only
	TArray<FGenW3DSubObject> Aggregates;
};

struct FGenW3DBox
{
	FString Name;
	uint32 Attributes = 0;
	FVector3f Center = FVector3f::ZeroVector;
	FVector3f Extent = FVector3f::ZeroVector;
};

/** One animation channel, fully decoded to per-frame values. */
struct FGenW3DChannel
{
	enum class EKind : uint8 { X, Y, Z, Q, Vis };
	int32 Pivot = 0;
	EKind Kind = EKind::X;
	int32 FirstFrame = 0;                  // samples cover [FirstFrame, FirstFrame + Num)
	TArray<float> Scalars;                 // X/Y/Z
	TArray<FQuat4f> Quats;                 // Q
	TArray<uint8> Bits;                    // Vis (one byte per frame)
	bool DefaultVis = true;
};

struct FGenW3DAnimation
{
	FString Name;
	FString HierarchyName;
	int32 NumFrames = 0;
	float FrameRate = 30.f;
	TArray<FGenW3DChannel> Channels;
	TArray<int32> PivotLookup;             // [pivot * 5 + kind] -> channel index or INDEX_NONE

	void BuildLookup();
	int32 NumPivots() const { return PivotLookup.Num() / 5; }

	/** Delta pose of one pivot at an integer frame: Local = Base * T(OutTranslation) * R(OutRotation). */
	void SamplePivot(int32 PivotIndex, int32 Frame, FVector3f& OutTranslation, FQuat4f& OutRotation, bool& OutVisible) const;
};

struct FGenW3DFile
{
	TArray<FGenW3DMesh> Meshes;
	TSharedPtr<FGenW3DHierarchy> Hierarchy;
	TArray<TSharedPtr<FGenW3DAnimation>> Animations;
	TSharedPtr<FGenW3DHLod> HLod;
	TArray<FGenW3DBox> Boxes;

	const FGenW3DMesh* FindMesh(const FString& FullName) const;
	const FGenW3DBox* FindBox(const FString& FullName) const;
};

namespace GenW3D
{
	GENERALS1_API bool Parse(TConstArrayView<uint8> Data, FGenW3DFile& Out);
}
