// Picking a model with the mouse: W3DView::pickDrawable casts a ray through the scene, and each model
// (HLodClass::Cast_Ray) hands it on to its parts - a mesh tests its triangles (MeshClass::Cast_Ray), a
// box itself (OBBoxRenderObjClass::Cast_Ray: the PICKBOX and BOUNDINGBOX of small models). So a click
// selects what the cursor is on, not what it is near.

#include "W3D/GenModelComponent.h"
#include "Core/GenCoords.h"
#include "Core/GenPerf.h"

static TAutoConsoleVariable<int32> CVarGenPickBoxes(
	TEXT("gen.PickBoxes"), 1,
	TEXT("1: a model's own pick box (the PICKBOX of soldiers and other small things) is part of what a click lands on, as in the original. 0: only what is drawn of it."));

namespace
{
	/** A ray and a triangle, seen from either side; a hit nearer than InOutT makes it less. */
	bool RayTriangle(const FVector3f& O, const FVector3f& D, const FVector3f& A, const FVector3f& B, const FVector3f& C, float& InOutT)
	{
		const FVector3f E1 = B - A, E2 = C - A;
		const FVector3f P = FVector3f::CrossProduct(D, E2);
		const float Det = FVector3f::DotProduct(E1, P);
		if (FMath::Abs(Det) < 1.e-12f)
		{
			return false;
		}
		const float Inv = 1.f / Det;
		const FVector3f S = O - A;
		const float U = FVector3f::DotProduct(S, P) * Inv;
		if (U < 0.f || U > 1.f)
		{
			return false;
		}
		const FVector3f Q = FVector3f::CrossProduct(S, E1);
		const float V = FVector3f::DotProduct(D, Q) * Inv;
		if (V < 0.f || U + V > 1.f)
		{
			return false;
		}
		const float T = FVector3f::DotProduct(E2, Q) * Inv;
		if (T <= 0.f || T >= InOutT)
		{
			return false;
		}
		InOutT = T;
		return true;
	}

	/** A ray and a box: where the ray goes in (0 if it starts inside), if that is nearer than MaxT. */
	bool RayBox(const FVector3f& O, const FVector3f& D, const FVector3f& Min, const FVector3f& Max, float MaxT, float& OutT)
	{
		float T0 = 0.f, T1 = MaxT;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float Do = D[Axis], Oo = O[Axis];
			if (FMath::Abs(Do) < 1.e-9f)
			{
				if (Oo < Min[Axis] || Oo > Max[Axis])
				{
					return false;
				}
				continue;
			}
			float A = (Min[Axis] - Oo) / Do, B = (Max[Axis] - Oo) / Do;
			if (A > B)
			{
				Swap(A, B);
			}
			T0 = FMath::Max(T0, A);
			T1 = FMath::Min(T1, B);
			if (T0 > T1)
			{
				return false;
			}
		}
		OutT = T0;
		return true;
	}
}

void UGenModelComponent::BuildPickParts() const
{
	bPickPartsBuilt = true;
	PickParts.Reset();
	PickCenter = FVector3f::ZeroVector;
	PickRadius = 0.f;
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Assets || !File)
	{
		return;
	}
	TArray<FGenW3DSubObject> Subs;
	TSharedPtr<const FGenW3DHierarchy> SameHierarchy;
	Assets->GetModelSubObjects(*File, Subs, SameHierarchy);
	const int32 NumPivots = PoseW3D.Num();

	// (The model's reach is taken in its base pose.)
	TArray<FTransform3f, TInlineAllocator<64>> Base;
	Base.Init(FTransform3f::Identity, FMath::Max(1, NumPivots));
	for (int32 i = 0; i < NumPivots; ++i)
	{
		if (Hierarchy && Hierarchy->Pivots.IsValidIndex(i))
		{
			const FGenW3DPivot& P = Hierarchy->Pivots[i];
			const FTransform3f Local(P.Rotation.GetNormalized(), P.Translation);
			Base[i] = (P.Parent >= 0 && P.Parent < i) ? Local * Base[P.Parent] : Local;
		}
	}
	FBox3f Reach(ForceInit);
	auto AddBox = [&](const FVector3f& Min, const FVector3f& Max, const FTransform3f& Xf)
	{
		for (int32 c = 0; c < 8; ++c)
		{
			Reach += Xf.TransformPosition(FVector3f((c & 1) ? Max.X : Min.X, (c & 2) ? Max.Y : Min.Y, (c & 4) ? Max.Z : Min.Z));
		}
	};

	for (int32 SubIndex = 0; SubIndex < Subs.Num(); ++SubIndex)
	{
		const FGenW3DSubObject& SO = Subs[SubIndex];
		FPickPart Part;
		Part.Bone = FMath::Clamp(SO.Bone, 0, FMath::Max(0, NumPivots - 1));
		Part.Mesh = File->FindMesh(SO.Name);
		Part.Box = Part.Mesh ? nullptr : File->FindBox(SO.Name);
		if (Part.Mesh)
		{
			const FGenW3DMesh& M = *Part.Mesh;
			// MeshClass::Cast_Ray: "so we could select trees but filter out headlight beams, etc." - nothing
			// that is blended onto the picture is in the way. (Nor a mesh that turns to face the camera.)
			const EGenW3DBlend Blend = M.Shaders.Num() > 0 ? M.Shaders[0].GetBlend() : EGenW3DBlend::Opaque;
			if (M.Triangles.Num() == 0 || Blend == EGenW3DBlend::Alpha || Blend == EGenW3DBlend::Additive || Blend == EGenW3DBlend::Multiply
				|| M.GeometryType() == GenW3D::MeshGeometryCameraAligned || M.GeometryType() == GenW3D::MeshGeometryCameraOriented)
			{
				continue;
			}
			Part.SubObject = SubObjects.IndexOfByPredicate([SubIndex](const FSubObject& S) { return S.SubIndex == SubIndex; });
			const bool bMerged = MergedMembers.ContainsByPredicate([SubIndex](const FGenModelPlan::FPart& P) { return P.SubIndex == SubIndex; });
			if (Part.SubObject == INDEX_NONE && !bMerged)
			{
				continue; // (never made: nothing of it shows)
			}
			Part.bSkin = M.IsSkin() && Hierarchy && M.Influences.Num() == M.Vertices.Num();
			if (Part.bSkin)
			{
				for (int32 v = 0; v < M.Vertices.Num(); ++v)
				{
					Reach += Base[FMath::Clamp(int32(M.Influences[v]), 0, Base.Num() - 1)].TransformPosition(M.Vertices[v]);
				}
			}
			else
			{
				FBox3f Own(ForceInit);
				for (const FVector3f& V : M.Vertices)
				{
					Own += V;
				}
				Part.Min = Own.Min;
				Part.Max = Own.Max;
				AddBox(Own.Min, Own.Max, Base[Part.Bone]);
			}
		}
		else if (Part.Box)
		{
			Part.Min = Part.Box->Center - Part.Box->Extent;
			Part.Max = Part.Box->Center + Part.Box->Extent;
			AddBox(Part.Min, Part.Max, Base[Part.Bone]);
		}
		else
		{
			continue;
		}
		PickParts.Add(Part);
	}
	if (Reach.IsValid)
	{
		PickCenter = Reach.GetCenter();
		// (Half as much again, and a little: parts move - doors, cranes, a soldier's arms.)
		PickRadius = Reach.GetExtent().Size() * 1.5f + 5.f;
	}
}

bool UGenModelComponent::RaycastWorld(const FVector& Origin, const FVector& Dir, double& InOutT) const
{
	if (!File || PoseW3D.Num() == 0 || bHiddenInGame)
	{
		return false;
	}
	if (!bPickPartsBuilt)
	{
		BuildPickParts();
	}
	if (PickParts.Num() == 0)
	{
		return false;
	}
	// Into the model's own space, in the W3D frame (y the other way, units instead of cm). Both ends of
	// the ray go across, so that a distance along it means the same on either side.
	const FTransform& ToWorld = GetComponentTransform();
	const FVector LO = ToWorld.InverseTransformPosition(Origin);
	const FVector LE = ToWorld.InverseTransformPosition(Origin + Dir);
	const double S = 1.0 / GenCoords::UnitsToCm;
	const FVector3f O(float(LO.X * S), float(-LO.Y * S), float(LO.Z * S));
	const FVector3f D(float((LE.X - LO.X) * S), float(-(LE.Y - LO.Y) * S), float((LE.Z - LO.Z) * S));

	// RTS3DScene::castRay: "Do a quick ray-sphere test" first.
	const float DD = D.SizeSquared();
	if (DD <= 0.f)
	{
		return false;
	}
	const FVector3f ToCenter = PickCenter - O;
	const float Along = FVector3f::DotProduct(ToCenter, D) / DD;
	if ((ToCenter - D * Along).SizeSquared() > PickRadius * PickRadius)
	{
		return false;
	}

	GEN_PERF_SCOPE("R.PickModel");
	if (bPoseDirty)
	{
		const_cast<UGenModelComponent*>(this)->UpdatePose(); // out of view until now: the pose is behind the animation
	}
	float T = float(FMath::Min(InOutT, 1.e30));
	bool bHit = false;
	static TArray<FVector3f> Posed; // (game thread only)
	const bool bBoxes = CVarGenPickBoxes.GetValueOnGameThread() != 0;
	for (const FPickPart& Part : PickParts)
	{
		if (Part.Box && !bBoxes)
		{
			continue;
		}
		// What doesn't show isn't in the way: a part hidden by name or by its mesh, or on a bone the animation hides.
		if (SubObjects.IsValidIndex(Part.SubObject))
		{
			const FSubObject& Sub = SubObjects[Part.SubObject];
			if (Sub.bHiddenByDefault || Sub.bForcedHidden)
			{
				continue;
			}
		}
		if (!Part.bSkin && PivotVisible.IsValidIndex(Part.Bone) && !PivotVisible[Part.Bone])
		{
			continue;
		}
		if (Part.bSkin)
		{
			// A skin's vertices each follow a bone of their own: where the pose has them now.
			const FGenW3DMesh& M = *Part.Mesh;
			Posed.SetNumUninitialized(M.Vertices.Num(), EAllowShrinking::No);
			for (int32 v = 0; v < M.Vertices.Num(); ++v)
			{
				Posed[v] = PoseW3D[FMath::Clamp(int32(M.Influences[v]), 0, PoseW3D.Num() - 1)].TransformPosition(M.Vertices[v]);
			}
			for (const FIntVector3& Tri : M.Triangles)
			{
				if (Posed.IsValidIndex(Tri.X) && Posed.IsValidIndex(Tri.Y) && Posed.IsValidIndex(Tri.Z))
				{
					bHit |= RayTriangle(O, D, Posed[Tri.X], Posed[Tri.Y], Posed[Tri.Z], T);
				}
			}
			continue;
		}
		const FTransform3f& Bone = PoseW3D[FMath::Clamp(Part.Bone, 0, PoseW3D.Num() - 1)];
		const FVector3f BO = Bone.InverseTransformPosition(O);
		const FVector3f BD = Bone.InverseTransformVector(D);
		float Enter = 0.f;
		if (!RayBox(BO, BD, Part.Min, Part.Max, T, Enter))
		{
			continue;
		}
		if (Part.Box)
		{
			if (Enter > 0.f && Enter < T)
			{
				T = Enter;
				bHit = true;
			}
			continue;
		}
		const FGenW3DMesh& M = *Part.Mesh;
		for (const FIntVector3& Tri : M.Triangles)
		{
			if (M.Vertices.IsValidIndex(Tri.X) && M.Vertices.IsValidIndex(Tri.Y) && M.Vertices.IsValidIndex(Tri.Z))
			{
				bHit |= RayTriangle(BO, BD, M.Vertices[Tri.X], M.Vertices[Tri.Y], M.Vertices[Tri.Z], T);
			}
		}
	}
	if (bHit)
	{
		InOutT = double(T);
	}
	return bHit;
}
