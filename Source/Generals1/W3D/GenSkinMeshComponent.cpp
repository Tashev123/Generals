#include "W3D/GenSkinMeshComponent.h"
#include "W3D/GenAssets.h"
#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "LocalVertexFactory.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "StaticMeshResources.h"

/** The render thread's side (after the engine's FProceduralMeshSceneProxy, with one set of buffers for all sections). */
class FGenSkinMeshProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FGenSkinMeshProxy(UGenSkinMeshComponent* Component, const FGenSkinData& Skin, const TArray<FVector3f>& Positions, const TArray<FPackedNormal>& Tangents)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel(), "FGenSkinMeshProxy")
		, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetShaderPlatform()))
		, NumVertices(Component->GetNumVertices())
	{
		const bool bPosed = Positions.Num() == NumVertices && Tangents.Num() == NumVertices * 2;
		TArray<FDynamicMeshVertex> Vertices;
		Vertices.SetNumUninitialized(NumVertices);
		int32 Base = 0;
		for (int32 s = 0; s < Skin.Sections.Num(); ++s)
		{
			const FGenSkinData::FSection& S = Skin.Sections[s];
			const int32 N = S.LocalPositions.Num();
			for (int32 v = 0; v < N; ++v)
			{
				FDynamicMeshVertex& Vert = Vertices[Base + v];
				Vert.Position = bPosed ? Positions[Base + v] : FVector3f::ZeroVector;
				Vert.TangentX = bPosed ? Tangents[(Base + v) * 2] : FPackedNormal(FVector3f(1.f, 0.f, 0.f));
				Vert.TangentZ = bPosed ? Tangents[(Base + v) * 2 + 1] : FPackedNormal(FVector3f(0.f, 0.f, 1.f));
				Vert.TangentZ.Vector.W = 127;
				Vert.Color = S.Colors.IsValidIndex(v) ? S.Colors[v].ToFColor(false) : FColor::White;
				const FVector2f UV = S.UVs.IsValidIndex(v) ? FVector2f(S.UVs[v]) : FVector2f::ZeroVector;
				Vert.TextureCoordinate[0] = UV;
				for (int32 t = 1; t < MAX_STATIC_TEXCOORDS; ++t)
				{
					Vert.TextureCoordinate[t] = FVector2f::ZeroVector;
				}
			}
			FSection& Out = Sections.AddDefaulted_GetRef();
			Out.FirstIndex = uint32(IndexBuffer.Indices.Num());
			Out.NumTriangles = uint32(S.Triangles.Num() / 3);
			Out.MinVertex = uint32(Base);
			Out.MaxVertex = uint32(Base + FMath::Max(N, 1) - 1);
			for (int32 Index : S.Triangles)
			{
				IndexBuffer.Indices.Add(uint32(Base + Index));
			}
			Out.Material = Component->GetMaterial(s);
			if (!Out.Material)
			{
				Out.Material = UMaterial::GetDefaultMaterial(MD_Surface);
			}
			Base += N;
		}

		// (Two sets of texture coordinates, the second all zero: what a material that asks for one got from the procedural mesh.)
		VertexBuffers.InitFromDynamicVertex(&VertexFactory, Vertices, 2);
		BeginInitResource(&VertexBuffers.PositionVertexBuffer);
		BeginInitResource(&VertexBuffers.StaticMeshVertexBuffer);
		BeginInitResource(&VertexBuffers.ColorVertexBuffer);
		BeginInitResource(&IndexBuffer);
		BeginInitResource(&VertexFactory);
	}

	virtual ~FGenSkinMeshProxy()
	{
		VertexBuffers.PositionVertexBuffer.ReleaseResource();
		VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
		VertexBuffers.ColorVertexBuffer.ReleaseResource();
		IndexBuffer.ReleaseResource();
		VertexFactory.ReleaseResource();
	}

	/** A pose arrives: over the positions and the tangents with it. */
	void SetPose_RenderThread(FRHICommandListBase& RHICmdList, const TArray<FVector3f>& Positions, const TArray<FPackedNormal>& Tangents)
	{
		FPositionVertexBuffer& PositionBuffer = VertexBuffers.PositionVertexBuffer;
		FStaticMeshVertexBuffer& MeshBuffer = VertexBuffers.StaticMeshVertexBuffer;
		const uint32 PositionBytes = uint32(Positions.Num()) * sizeof(FVector3f);
		const uint32 TangentBytes = uint32(Tangents.Num()) * sizeof(FPackedNormal);
		if (Positions.Num() != NumVertices || Tangents.Num() != NumVertices * 2 || NumVertices == 0
			|| !PositionBuffer.VertexBufferRHI || PositionBuffer.GetNumVertices() * PositionBuffer.GetStride() != PositionBytes
			|| !MeshBuffer.TangentsVertexBuffer.VertexBufferRHI || uint32(MeshBuffer.GetTangentSize()) != TangentBytes)
		{
			return;
		}
		void* Data = RHICmdList.LockBuffer(PositionBuffer.VertexBufferRHI, 0, PositionBytes, RLM_WriteOnly);
		FMemory::Memcpy(Data, Positions.GetData(), PositionBytes);
		RHICmdList.UnlockBuffer(PositionBuffer.VertexBufferRHI);

		Data = RHICmdList.LockBuffer(MeshBuffer.TangentsVertexBuffer.VertexBufferRHI, 0, TangentBytes, RLM_WriteOnly);
		FMemory::Memcpy(Data, Tangents.GetData(), TangentBytes);
		RHICmdList.UnlockBuffer(MeshBuffer.TangentsVertexBuffer.VertexBufferRHI);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (!(VisibilityMap & (1 << ViewIndex)))
			{
				continue;
			}
			FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
			FPrimitiveUniformShaderParametersBuilder Builder;
			BuildUniformShaderParameters(Builder);
			DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), Builder);

			for (int32 s = 0; s < Sections.Num(); ++s)
			{
				const FSection& Section = Sections[s];
				if (Section.NumTriangles == 0)
				{
					continue;
				}
				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.IndexBuffer = &IndexBuffer;
				Mesh.bWireframe = false;
				Mesh.VertexFactory = &VertexFactory;
				Mesh.MaterialRenderProxy = Section.Material->GetRenderProxy();
				BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
				BatchElement.FirstIndex = Section.FirstIndex;
				BatchElement.NumPrimitives = Section.NumTriangles;
				BatchElement.MinVertexIndex = Section.MinVertex;
				BatchElement.MaxVertexIndex = Section.MaxVertex;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = false;
				Mesh.MeshIdInPrimitive = uint16(s);
				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	virtual bool CanBeOccluded() const override
	{
		return !MaterialRelevance.bDisableDepthTest;
	}

	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + FPrimitiveSceneProxy::GetAllocatedSize(); }

private:
	struct FSection
	{
		UMaterialInterface* Material = nullptr;
		uint32 FirstIndex = 0, NumTriangles = 0, MinVertex = 0, MaxVertex = 0;
	};

	FStaticMeshVertexBuffers VertexBuffers;
	FDynamicMeshIndexBuffer32 IndexBuffer;
	FLocalVertexFactory VertexFactory;
	FMaterialRelevance MaterialRelevance;
	TArray<FSection> Sections;
	int32 NumVertices = 0;
};

UGenSkinMeshComponent::UGenSkinMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UGenSkinMeshComponent::SetSkin(const FGenSkinData* InSkin)
{
	Skin = InSkin;
	NumVertices = 0;
	if (Skin)
	{
		for (const FGenSkinData::FSection& S : Skin->Sections)
		{
			NumVertices += S.LocalPositions.Num();
		}
	}
	PosePositions.Reset();
	PoseTangents.Reset();
	LocalBox = FBox(ForceInit);
	MarkRenderStateDirty();
}

int32 UGenSkinMeshComponent::GetNumMaterials() const
{
	return Skin ? Skin->Sections.Num() : 0;
}

FPrimitiveSceneProxy* UGenSkinMeshComponent::CreateSceneProxy()
{
	return (Skin && NumVertices > 0) ? new FGenSkinMeshProxy(this, *Skin, PosePositions, PoseTangents) : nullptr;
}

FBoxSphereBounds UGenSkinMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBox.IsValid)
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
	}
	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

void UGenSkinMeshComponent::PoseChanged(const FBox3f& Box)
{
	if (PosePositions.Num() != NumVertices || PoseTangents.Num() != NumVertices * 2 || NumVertices == 0)
	{
		return;
	}
	// The bounds are the pose's and half as much again all round, and stay until a pose leaves them:
	// to give the engine new ones for every pose costs more than a soldier drawn a little too readily.
	const FBox PoseBox(FVector(Box.Min), FVector(Box.Max));
	if (Box.IsValid && !(LocalBox.IsValid && LocalBox.IsInside(PoseBox)))
	{
		LocalBox = PoseBox.ExpandBy(PoseBox.GetExtent() * 0.5 + FVector(10.0));
		UpdateBounds();
		MarkRenderTransformDirty();
	}
	if (!SceneProxy)
	{
		return; // (the one that gets made starts from this pose)
	}
	FGenSkinMeshProxy* Proxy = static_cast<FGenSkinMeshProxy*>(SceneProxy);
	ENQUEUE_RENDER_COMMAND(GenSkinPose)([Proxy, Positions = PosePositions, Tangents = PoseTangents](FRHICommandListImmediate& RHICmdList)
	{
		Proxy->SetPose_RenderThread(RHICmdList, Positions, Tangents);
	});
}
