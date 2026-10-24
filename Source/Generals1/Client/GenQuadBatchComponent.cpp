#include "Client/GenQuadBatchComponent.h"
#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "LocalVertexFactory.h"
#include "MaterialDomain.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "StaticMeshResources.h"

/** The render thread's side (after the engine's FCableSceneProxy: buffers that stay, written into every frame). */
class FGenQuadBatchProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	explicit FGenQuadBatchProxy(UGenQuadBatchComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel(), "FGenQuadBatchProxy")
		, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetShaderPlatform()))
	{
		// Materials join while the proxy lives (a texture's first particle): instances of the one it
		// was made with, so nothing about it changes - but the engine's check knows only the first.
		bVerifyUsedMaterials = false;
	}

	virtual ~FGenQuadBatchProxy()
	{
		ReleaseBuffers();
	}

	void ReleaseBuffers()
	{
		if (CapacityQuads > 0)
		{
			VertexBuffers.PositionVertexBuffer.ReleaseResource();
			VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
			VertexBuffers.ColorVertexBuffer.ReleaseResource();
			IndexBuffer.ReleaseResource();
			VertexFactory.ReleaseResource();
			CapacityQuads = 0;
		}
	}

	/** The frame's quads arrive: into the buffers with them (which grow, by doubling, when they must). */
	void SetFrame_RenderThread(FRHICommandListBase& RHICmdList, FGenQuadFrame& Frame)
	{
		const int32 NumVerts = Frame.Positions.Num();
		const int32 NumQuads = NumVerts / 4;
		Draws = MoveTemp(Frame.Draws);
		UsedQuads = NumQuads;
		if (NumQuads == 0)
		{
			return;
		}
		if (NumQuads > CapacityQuads)
		{
			ReleaseBuffers();
			CapacityQuads = int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(NumQuads, 256))));
			const int32 CapacityVerts = CapacityQuads * 4;
			VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
			VertexBuffers.InitWithDummyData(RHICmdList, &VertexFactory, uint32(CapacityVerts));
			IndexBuffer.Indices.SetNumUninitialized(CapacityQuads * 6);
			for (int32 q = 0; q < CapacityQuads; ++q)
			{
				const uint32 v = uint32(q * 4);
				uint32* I = &IndexBuffer.Indices[q * 6];
				I[0] = v; I[1] = v + 1; I[2] = v + 2;
				I[3] = v; I[4] = v + 2; I[5] = v + 3;
			}
			IndexBuffer.InitResource(RHICmdList);

			// The tangents never change (the quads are unlit; this is what the procedural mesh gave them).
			FStaticMeshVertexBuffer& Buffer = VertexBuffers.StaticMeshVertexBuffer;
			for (int32 v = 0; v < CapacityVerts; ++v)
			{
				Buffer.SetVertexTangents(uint32(v), FVector3f(1.f, 0.f, 0.f), FVector3f(0.f, 1.f, 0.f), FVector3f(0.f, 0.f, 1.f));
			}
			void* Data = RHICmdList.LockBuffer(Buffer.TangentsVertexBuffer.VertexBufferRHI, 0, Buffer.GetTangentSize(), RLM_WriteOnly);
			FMemory::Memcpy(Data, Buffer.GetTangentData(), Buffer.GetTangentSize());
			RHICmdList.UnlockBuffer(Buffer.TangentsVertexBuffer.VertexBufferRHI);
		}
		{
			FPositionVertexBuffer& Buffer = VertexBuffers.PositionVertexBuffer;
			const uint32 Bytes = uint32(NumVerts) * Buffer.GetStride();
			check(Buffer.GetStride() == sizeof(FVector3f));
			void* Data = RHICmdList.LockBuffer(Buffer.VertexBufferRHI, 0, Bytes, RLM_WriteOnly);
			FMemory::Memcpy(Data, Frame.Positions.GetData(), Bytes);
			RHICmdList.UnlockBuffer(Buffer.VertexBufferRHI);
		}
		{
			FColorVertexBuffer& Buffer = VertexBuffers.ColorVertexBuffer;
			const uint32 Bytes = uint32(NumVerts) * Buffer.GetStride();
			check(Buffer.GetStride() == sizeof(FColor));
			void* Data = RHICmdList.LockBuffer(Buffer.VertexBufferRHI, 0, Bytes, RLM_WriteOnly);
			FMemory::Memcpy(Data, Frame.Colors.GetData(), Bytes);
			RHICmdList.UnlockBuffer(Buffer.VertexBufferRHI);
		}
		{
			// (One texture coordinate per vertex, at full precision: a beam's texture is tiled along it many times.)
			FStaticMeshVertexBuffer& Buffer = VertexBuffers.StaticMeshVertexBuffer;
			const uint32 Bytes = uint32(NumVerts) * sizeof(FVector2f);
			void* Data = RHICmdList.LockBuffer(Buffer.TexCoordVertexBuffer.VertexBufferRHI, 0, Bytes, RLM_WriteOnly);
			FMemory::Memcpy(Data, Frame.UVs.GetData(), Bytes);
			RHICmdList.UnlockBuffer(Buffer.TexCoordVertexBuffer.VertexBufferRHI);
		}
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		if (UsedQuads == 0 || CapacityQuads == 0)
		{
			return;
		}
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

			for (int32 d = 0; d < Draws.Num(); ++d)
			{
				const FGenQuadDraw& Draw = Draws[d];
				if (!Draw.Material || Draw.NumQuads <= 0 || Draw.FirstQuad + Draw.NumQuads > UsedQuads)
				{
					continue;
				}
				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.IndexBuffer = &IndexBuffer;
				Mesh.bWireframe = false;
				Mesh.VertexFactory = &VertexFactory;
				Mesh.MaterialRenderProxy = Draw.Material;
				BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
				BatchElement.FirstIndex = uint32(Draw.FirstQuad * 6);
				BatchElement.NumPrimitives = uint32(Draw.NumQuads * 2);
				BatchElement.MinVertexIndex = uint32(Draw.FirstQuad * 4);
				BatchElement.MaxVertexIndex = uint32((Draw.FirstQuad + Draw.NumQuads) * 4 - 1);
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = false;
				// (What tells the draws of one primitive apart when the translucent ones are sorted: they keep their order.)
				Mesh.MeshIdInPrimitive = uint16(d);
				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = false;
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = false;
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + FPrimitiveSceneProxy::GetAllocatedSize(); }

private:
	FStaticMeshVertexBuffers VertexBuffers;
	FDynamicMeshIndexBuffer32 IndexBuffer;
	FLocalVertexFactory VertexFactory;
	FMaterialRelevance MaterialRelevance;
	TArray<FGenQuadDraw> Draws;
	int32 CapacityQuads = 0;
	int32 UsedQuads = 0;
};

UGenQuadBatchComponent::UGenQuadBatchComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bReceivesDecals = false;
	bUseAsOccluder = false;
}

FPrimitiveSceneProxy* UGenQuadBatchComponent::CreateSceneProxy()
{
	// (Without a material there is nothing to say how it is to be drawn: the proxy is made when the first one comes.)
	return UsedMaterials.Num() > 0 ? new FGenQuadBatchProxy(this) : nullptr;
}

FBoxSphereBounds UGenQuadBatchComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// The whole map: what is in it changes every frame, and none of it is worth culling as one piece.
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(1.e6), 1.e6);
}

void UGenQuadBatchComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	for (UMaterialInterface* M : UsedMaterials)
	{
		if (M)
		{
			OutMaterials.Add(M);
		}
	}
}

UMaterialInterface* UGenQuadBatchComponent::GetMaterial(int32 Index) const
{
	return UsedMaterials.IsValidIndex(Index) ? UsedMaterials[Index].Get() : nullptr;
}

void UGenQuadBatchComponent::AddMaterial(UMaterialInterface* Material)
{
	if (!Material || UsedMaterials.Contains(Material))
	{
		return;
	}
	UsedMaterials.Add(Material);
	if (UsedMaterials.Num() == 1)
	{
		MarkRenderStateDirty(); // (now there can be a proxy)
	}
}

void UGenQuadBatchComponent::SetFrame(FGenQuadFrame& Frame)
{
	const bool bHasQuads = Frame.Positions.Num() >= 4 && Frame.Draws.Num() > 0;
	if (!SceneProxy || (!bHasQuads && !bSentQuads))
	{
		// (No proxy: it is made at the end of the frame its first material came in, and draws from the next.)
		bSentQuads = bSentQuads && SceneProxy != nullptr;
		Frame.Reset();
		return;
	}
	bSentQuads = bHasQuads;
	FGenQuadBatchProxy* Proxy = static_cast<FGenQuadBatchProxy*>(SceneProxy);
	ENQUEUE_RENDER_COMMAND(GenQuadBatchFrame)([Proxy, Data = MoveTemp(Frame)](FRHICommandListImmediate& RHICmdList) mutable
	{
		Proxy->SetFrame_RenderThread(RHICmdList, Data);
	});
	Frame.Reset();
}
