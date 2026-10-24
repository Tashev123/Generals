#include "Map/GenBridges.h"
#include "Core/GenMeshUtil.h"
#include "Core/GenCoords.h"
#include "Map/GenMapData.h"
#include "Map/GenRoads.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

// ---------------------------------------------------------------------------------------------
// FGenBridgeInfo: the Bridge class of TerrainLogic.cpp
// ---------------------------------------------------------------------------------------------

namespace
{
	constexpr float BridgeFloatAmt = 0.25f;   // BRIDGE_FLOAT_AMT
	constexpr float BridgeCellSize = 10.f;    // PATHFIND_CELL_SIZE

	/** Point_In_Triangle_2D: inside or on the edge. */
	bool BridgePointInTriangle(const FVector3f& P0, const FVector3f& P1, const FVector3f& P2, float X, float Y)
	{
		auto Perp = [](float AX, float AY, float BX, float BY) { return AX * BY - AY * BX; };
		const float Whole = Perp(P1.X - P0.X, P1.Y - P0.Y, P2.X - P0.X, P2.Y - P0.Y);
		if (Whole == 0.f)
		{
			return false; // (a bridge of no size)
		}
		const float Side = Whole > 0.f ? 1.f : -1.f;
		if (Perp(P1.X - P0.X, P1.Y - P0.Y, X - P0.X, Y - P0.Y) * Side < 0.f) { return false; }
		if (Perp(P2.X - P1.X, P2.Y - P1.Y, X - P1.X, Y - P1.Y) * Side < 0.f) { return false; }
		if (Perp(P0.X - P2.X, P0.Y - P2.Y, X - P2.X, Y - P2.Y) * Side < 0.f) { return false; }
		return true;
	}

	/** LineInRegion: does the line from P1 to P2 touch the rectangle? (TerrainLogic.cpp's clipper, as written.) */
	bool BridgeLineInRegion(const FVector2f& P1, const FVector2f& P2, const FBox2f& Region)
	{
		enum { ClipLeft = 0x01, ClipRight = 0x02, ClipBottom = 0x04, ClipTop = 0x08 };
		const float Left = Region.Min.X, Right = Region.Max.X, Top = Region.Min.Y, Bottom = Region.Max.Y;
		float X1 = P1.X, Y1 = P1.Y, X2 = P2.X, Y2 = P2.Y;
		auto Code = [&](float X, float Y)
		{
			int32 C = 0;
			if (X < Left) { C = ClipLeft; } else if (X > Right) { C = ClipRight; }
			if (Y < Top) { C |= ClipTop; } else if (Y > Bottom) { C |= ClipBottom; }
			return C;
		};
		const int32 Code1 = Code(X1, Y1), Code2 = Code(X2, Y2);
		if ((Code1 | Code2) == 0)
		{
			return true;
		}
		if (Code1 & Code2)
		{
			return false;
		}
		float Diff;
		if (Code1)
		{
			if (Code1 & ClipTop)
			{
				if ((Diff = Y2 - Y1) == 0.f) { return false; }
				X1 += (X2 - X1) * (Top - Y1) / Diff;
				Y1 = Top;
			}
			else if (Code1 & ClipBottom)
			{
				if ((Diff = Y2 - Y1) == 0.f) { return false; }
				X1 += (X2 - X1) * (Bottom - Y1) / Diff;
				Y1 = Bottom;
			}
			if (X1 > Right)
			{
				if ((Diff = X2 - X1) == 0.f) { return false; }
				Y1 += (Y2 - Y1) * (Right - X1) / Diff;
				X1 = Right;
			}
			else if (X1 < Left)
			{
				if ((Diff = X2 - X1) == 0.f) { return false; }
				Y1 += (Y2 - Y1) * (Left - X1) / Diff;
				X1 = Left;
			}
		}
		if (Code2)
		{
			if (Code2 & ClipTop)
			{
				if ((Diff = Y2 - Y1) == 0.f) { return false; }
				X2 += (X2 - X1) * (Top - Y2) / Diff;
				Y2 = Top;
			}
			else if (Code2 & ClipBottom)
			{
				if ((Diff = Y2 - Y1) == 0.f) { return false; }
				X2 += (X2 - X1) * (Bottom - Y2) / Diff;
				Y2 = Bottom;
			}
			if (X2 > Right)
			{
				if ((Diff = X2 - X1) == 0.f) { return false; }
				Y2 += (Y2 - Y1) * (Right - X2) / Diff;
				X2 = Right;
			}
			else if (X2 < Left)
			{
				if ((Diff = X2 - X1) == 0.f) { return false; }
				Y2 += (Y2 - Y1) * (Left - X2) / Diff;
				X2 = Left;
			}
		}
		return X1 >= Left && X1 <= Right && Y1 >= Top && Y1 <= Bottom && X2 >= Left && X2 <= Right && Y2 >= Top && Y2 <= Bottom;
	}

	FVector2f BridgeXY(const FVector3f& V) { return FVector2f(V.X, V.Y); }
}

void FGenBridgeInfo::ComputeBounds()
{
	Bounds = FBox2f(ForceInit);
	Bounds += BridgeXY(FromLeft);
	Bounds += BridgeXY(FromRight);
	Bounds += BridgeXY(ToLeft);
	Bounds += BridgeXY(ToRight);
}

bool FGenBridgeInfo::IsPointOnBridge(float X, float Y) const
{
	if (X < Bounds.Min.X || X > Bounds.Max.X || Y < Bounds.Min.Y || Y > Bounds.Max.Y)
	{
		return false;
	}
	return BridgePointInTriangle(FromLeft, FromRight, ToLeft, X, Y) || BridgePointInTriangle(FromRight, ToLeft, ToRight, X, Y);
}

float FGenBridgeInfo::GetHeight(float X, float Y) const
{
	// The plane through three of the corners.
	const FVector3f N = FVector3f::CrossProduct(FromRight - FromLeft, ToLeft - FromLeft);
	if (FMath::IsNearlyZero(N.Z))
	{
		return FromLeft.Z;
	}
	return FromLeft.Z - (N.X * (X - FromLeft.X) + N.Y * (Y - FromLeft.Y)) / N.Z;
}

bool FGenBridgeInfo::IsCellOnEnd(const FBox2f& Cell) const
{
	// The end lines, shortened by one pathfind cell at either side.
	const FVector2f EndVector = BridgeXY((FromRight - FromLeft).GetSafeNormal()) * BridgeCellSize;
	return BridgeLineInRegion(BridgeXY(FromLeft) + EndVector, BridgeXY(FromRight) - EndVector, Cell)
		|| BridgeLineInRegion(BridgeXY(ToLeft) + EndVector, BridgeXY(ToRight) - EndVector, Cell);
}

bool FGenBridgeInfo::IsCellOnSide(const FBox2f& Cell) const
{
	const FVector2f EndVector = BridgeXY((FromRight - FromLeft).GetSafeNormal()) * (BridgeCellSize * 0.51f);
	FVector2f FL = BridgeXY(FromLeft) - EndVector, FR = BridgeXY(FromRight) + EndVector;
	FVector2f TL = BridgeXY(ToLeft) - EndVector, TR = BridgeXY(ToRight) + EndVector;
	if (BridgeLineInRegion(FL, TL, Cell) || BridgeLineInRegion(FR, TR, Cell))
	{
		return true;
	}
	FL -= EndVector;
	FR += EndVector;
	TL -= EndVector;
	TR += EndVector;
	return BridgeLineInRegion(FL, TL, Cell) || BridgeLineInRegion(FR, TR, Cell);
}

bool FGenBridgeInfo::IsCellEntryPoint(const FBox2f& Cell) const
{
	const FVector2f EndVector = BridgeXY((FromRight - FromLeft).GetSafeNormal()) * BridgeCellSize;
	const FVector2f BridgeVector = BridgeXY((To - From).GetSafeNormal()) * (BridgeCellSize / 2.f);
	return BridgeLineInRegion(BridgeXY(FromLeft) - BridgeVector + EndVector, BridgeXY(FromRight) - BridgeVector - EndVector, Cell)
		|| BridgeLineInRegion(BridgeXY(ToLeft) + BridgeVector + EndVector, BridgeXY(ToRight) + BridgeVector - EndVector, Cell);
}

bool FGenBridgeInfo::RayHit(const FVector3f& Start, const FVector3f& Dir, FVector3f& OutHit) const
{
	const FVector3f N = FVector3f::CrossProduct(FromRight - FromLeft, ToLeft - FromLeft);
	const float Denom = FVector3f::DotProduct(N, Dir);
	if (FMath::IsNearlyZero(Denom))
	{
		return false;
	}
	const float T = FVector3f::DotProduct(N, FromLeft - Start) / Denom;
	if (T < 0.f)
	{
		return false;
	}
	OutHit = Start + Dir * T;
	return IsPointOnBridge(OutHit.X, OutHit.Y);
}

// ---------------------------------------------------------------------------------------------
// The model: W3DBridge::load
// ---------------------------------------------------------------------------------------------

namespace
{
	/** A sub-object's place in the model (RenderObjClass::Get_Transform), W3D frame. */
	struct FBridgeXform
	{
		FQuat4f Rot = FQuat4f::Identity;
		FVector3f Trans = FVector3f::ZeroVector;

		FVector3f Apply(const FVector3f& V) const { return Rot.RotateVector(V) + Trans; }
	};

	FBridgeXform BridgePivotXform(const FGenW3DHierarchy* H, int32 Bone)
	{
		FBridgeXform X;
		int32 Guard = 0;
		for (int32 B = Bone; H && H->Pivots.IsValidIndex(B) && Guard++ < 64; B = H->Pivots[B].Parent)
		{
			const FGenW3DPivot& P = H->Pivots[B];
			X.Trans = P.Rotation.RotateVector(X.Trans) + P.Translation;
			X.Rot = P.Rotation * X.Rot;
		}
		return X;
	}

	struct FBridgeModel
	{
		TSharedPtr<const FGenW3DFile> File;
		const FGenW3DMesh* Left = nullptr;
		const FGenW3DMesh* Section = nullptr;
		const FGenW3DMesh* Right = nullptr;
		FBridgeXform LeftX, SectionX, RightX;
		float Scale = 1.f;
		float LeftMinX = 0.f, LeftMaxX = 0.f, SectionMinX = 0.f, SectionMaxX = 0.f, RightMinX = 0.f, RightMaxX = 0.f;
		float MinY = 0.f, MaxY = 0.f;
		float Length = 1.f;
		bool bSectional = false;   // SECTIONAL_BRIDGE (spans are repeated) or FIXED_BRIDGE
	};

	bool LoadBridgeModel(const FGenBridgeType& Type, FBridgeModel& M)
	{
		UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
		M.File = Assets->LoadW3D(Type.Model);
		if (!M.File)
		{
			return false;
		}
		const FGenW3DHierarchy* Hierarchy = M.File->Hierarchy.Get();
		TSharedPtr<const FGenW3DHierarchy> Other;
		if (!Hierarchy && M.File->HLod)
		{
			Other = Assets->FindHierarchy(M.File->HLod->HierarchyName, M.File.Get());
			Hierarchy = Other.Get();
		}
		// The sub-object whose name starts with "<model>.BRIDGE_LEFT" (and so on); the last such wins.
		auto Find = [&](const TCHAR* Part, const FGenW3DMesh*& OutMesh, FBridgeXform& OutX)
		{
			FString Name = Type.Model + TEXT(".") + Part;
			if (M.File->HLod)
			{
				const FString Prefix = Name;
				for (const FGenW3DSubObject& S : M.File->HLod->SubObjects)
				{
					if (S.Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
					{
						OutX = BridgePivotXform(Hierarchy, S.Bone);
						Name = S.Name;
					}
				}
			}
			Name.TrimStartAndEndInline();
			OutMesh = nullptr;
			for (const FGenW3DMesh& Mesh : M.File->Meshes)
			{
				if (Mesh.FullName().TrimStartAndEnd().Equals(Name, ESearchCase::IgnoreCase))
				{
					OutMesh = &Mesh;
					break;
				}
			}
		};
		Find(TEXT("BRIDGE_LEFT"), M.Left, M.LeftX);
		Find(TEXT("BRIDGE_SPAN"), M.Section, M.SectionX);
		Find(TEXT("BRIDGE_RIGHT"), M.Right, M.RightX);
		M.Scale = Type.Scale;
		if (!M.Left)
		{
			return false;
		}
		M.bSectional = M.Right && M.Section;

		auto ExtentX = [](const FGenW3DMesh& Mesh, const FBridgeXform& X, float& MinX, float& MaxX)
		{
			MinX = FLT_MAX;
			MaxX = -FLT_MAX;
			for (const FVector3f& V : Mesh.Vertices)
			{
				const float PX = X.Apply(V).X;
				MinX = FMath::Min(MinX, PX);
				MaxX = FMath::Max(MaxX, PX);
			}
		};
		ExtentX(*M.Left, M.LeftX, M.LeftMinX, M.LeftMaxX);
		// "we assume all sections are the same width, so we only do maxY for first section"
		M.MinY = FLT_MAX;
		M.MaxY = -FLT_MAX;
		for (const FVector3f& V : M.Left->Vertices)
		{
			const float PY = M.LeftX.Apply(V).Y;
			M.MinY = FMath::Min(M.MinY, PY);
			M.MaxY = FMath::Max(M.MaxY, PY);
		}
		if (M.bSectional)
		{
			ExtentX(*M.Section, M.SectionX, M.SectionMinX, M.SectionMaxX);
			ExtentX(*M.Right, M.RightX, M.RightMinX, M.RightMaxX);
		}
		else
		{
			M.SectionMinX = M.SectionMaxX = M.RightMinX = M.RightMaxX = M.LeftMaxX;
		}
		M.Length = FMath::Max(1.f, M.RightMaxX - M.LeftMinX);
		if (M.bSectional)
		{
			// "make sure the sections align"
			const float AllowableError = 0.05f * M.Length;
			if (M.LeftMaxX > M.SectionMinX + AllowableError || M.RightMinX < M.SectionMaxX - AllowableError)
			{
				M.bSectional = false;
			}
		}
		return true;
	}

	struct FBridgeGeometry
	{
		TArray<FVector3f> Positions;   // Generals world units
		TArray<FVector3f> Normals;     // Generals frame
		TArray<FVector2f> UVs;
		TArray<int32> Indices;
	};

	/** W3DBridge::getModelVertices / getModelIndices: one part of the model, laid along the bridge. */
	void AppendBridgePart(const FGenW3DMesh* Mesh, const FBridgeXform& X, float XOffset, const FVector3f& Vec, const FVector3f& VecNormal,
		const FVector3f& VecZ, const FVector3f& Start, FBridgeGeometry& Out)
	{
		if (!Mesh)
		{
			return;
		}
		const int32 Base = Out.Positions.Num();
		const TArray<FVector2f>* UVs = nullptr;
		if (Mesh->Passes.Num() > 0 && Mesh->Passes[0].Stages.Num() > 0 && Mesh->Passes[0].Stages[0].TexCoords.Num() == Mesh->Vertices.Num())
		{
			UVs = &Mesh->Passes[0].Stages[0].TexCoords;
		}
		for (int32 i = 0; i < Mesh->Vertices.Num(); ++i)
		{
			const FVector3f V = X.Apply(Mesh->Vertices[i]);
			Out.Positions.Add(Vec * (V.X + XOffset) + VecNormal * V.Y + VecZ * V.Z + Start);
			const FVector3f N = X.Rot.RotateVector(Mesh->Normals.IsValidIndex(i) ? Mesh->Normals[i] : FVector3f(0.f, 0.f, 1.f));
			Out.Normals.Add((Vec * N.X + VecNormal * N.Y + VecZ * N.Z).GetSafeNormal());
			Out.UVs.Add(UVs ? FVector2f((*UVs)[i].X, 1.f - (*UVs)[i].Y) : FVector2f::ZeroVector);
		}
		for (const FIntVector3& T : Mesh->Triangles)
		{
			Out.Indices.Append({ Base + T.X, Base + T.Y, Base + T.Z });
		}
	}

	/** W3DBridge::getIndicesNVertices */
	int32 BuildBridgeGeometry(const FBridgeModel& M, const FVector3f& Start, const FVector3f& End, FBridgeGeometry& Out)
	{
		FVector3f Vec = End - Start;
		const FVector3f VecNormal = FVector3f(-Vec.Y, Vec.X, 0.f).GetSafeNormal() * M.Scale;
		const float DesiredLength = FMath::Max(Vec.Size(), 0.001f);
		// "Rotate along the y axis to get the appropriate Z height adjustment."
		const float DeltaZ = (End.Z - Start.Z) / DesiredLength;
		const float DeltaX = FMath::Sqrt(FMath::Max(0.f, 1.f - DeltaZ * DeltaZ));
		const FVector3f VecZ = FVector3f(-DeltaZ, 0.f, DeltaX) * M.Scale;
		const float XOffset = -M.LeftMinX;
		if (!M.Section)
		{
			// A bridge in one piece: stretched from end to end.
			Vec /= M.Length;
			AppendBridgePart(M.Left, M.LeftX, XOffset, Vec, VecNormal, VecZ, Start, Out);
			return 0;
		}
		const float SpanLength = M.RightMinX - M.LeftMaxX;
		int32 NumSpans = 1;
		if (M.bSectional && SpanLength > 0.f)
		{
			const float Spannable = DesiredLength - (M.Length - SpanLength);
			NumSpans = FMath::Max(0, FMath::FloorToInt32((Spannable + SpanLength / 2.f) / SpanLength));
		}
		const float BridgeLength = M.Length + float(NumSpans - 1) * SpanLength;
		Vec /= FMath::Max(BridgeLength, 0.001f);
		AppendBridgePart(M.Left, M.LeftX, XOffset, Vec, VecNormal, VecZ, Start, Out);
		for (int32 i = 0; i < NumSpans; ++i)
		{
			AppendBridgePart(M.Section, M.SectionX, XOffset + float(i) * SpanLength, Vec, VecNormal, VecZ, Start, Out);
		}
		AppendBridgePart(M.Right, M.RightX, XOffset + float(NumSpans - 1) * SpanLength, Vec, VecNormal, VecZ, Start, Out);
		return NumSpans;
	}

	UStaticMesh* BuildBridgeMesh(UObject* Outer, const FBridgeGeometry& G, UMaterialInterface* Material)
	{
		FMeshDescription MD;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();
		TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
		VUV.SetNumChannels(1);
		const FPolygonGroupID Group = MD.CreatePolygonGroup();
		Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Bridge");
		MD.ReserveNewVertices(G.Positions.Num());
		MD.ReserveNewVertexInstances(G.Positions.Num());
		MD.ReserveNewTriangles(G.Indices.Num() / 3);

		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(G.Positions.Num());
		for (int32 i = 0; i < G.Positions.Num(); ++i)
		{
			const FVertexID Id = MD.CreateVertex();
			VPos[Id] = GenCoords::ToUEf(G.Positions[i].X, G.Positions[i].Y, G.Positions[i].Z);
			const FVertexInstanceID I = MD.CreateVertexInstance(Id);
			const FVector3f N = GenCoords::DirToUE(G.Normals[i].X, G.Normals[i].Y, G.Normals[i].Z).GetSafeNormal();
			VNrm[I] = N;
			FVector3f Tangent = FVector3f::CrossProduct(FVector3f(0.f, 1.f, 0.f), N);
			if (Tangent.SizeSquared() < 1e-6f)
			{
				Tangent = FVector3f::CrossProduct(FVector3f(1.f, 0.f, 0.f), N);
			}
			VTan[I] = Tangent.GetSafeNormal();
			VSign[I] = 1.f;
			VUV.Set(I, 0, G.UVs[i]);
			Instances.Add(I);
		}
		for (int32 i = 0; i + 2 < G.Indices.Num(); i += 3)
		{
			if (G.Indices[i] == G.Indices[i + 1] || G.Indices[i + 1] == G.Indices[i + 2] || G.Indices[i] == G.Indices[i + 2])
			{
				continue;
			}
			const FVertexInstanceID Tri[3] = { Instances[G.Indices[i]], Instances[G.Indices[i + 1]], Instances[G.Indices[i + 2]] };
			MD.CreateTriangle(Group, Tri);
		}

		UStaticMesh* SM = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
		SM->GetStaticMaterials().Add(GenRuntimeMaterialSlot(Material, TEXT("Bridge")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		SM->BuildFromMeshDescriptions({ &MD }, Params);
		return SM;
	}
}

// ---------------------------------------------------------------------------------------------

void UGenBridgeComponent::Clear()
{
	for (UStaticMeshComponent* C : Meshes)
	{
		if (C) { C->DestroyComponent(); }
	}
	Meshes.Reset();
	Materials.Reset();
	Infos.Reset();
}

void UGenBridgeComponent::Build(const FGenMapData& Map)
{
	Clear();
	const FGenTerrainRoads& Defs = FGenTerrainRoads::Get();
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	TMap<FString, UMaterialInstanceDynamic*> TextureMaterials;

	// W3DBridgeBuffer::loadBridges: a bridge is two map objects in a row, flagged as its two points.
	const TArray<FGenMapObject>& Objs = Map.Objects;
	for (int32 n = 0; n + 1 < Objs.Num(); ++n)
	{
		const FGenMapObject& A = Objs[n];
		const FGenMapObject& B = Objs[n + 1];
		if (!(A.Flags & FGenMapObject::FlagBridgePoint1) || !(B.Flags & FGenMapObject::FlagBridgePoint2))
		{
			continue;
		}
		++n;
		const FGenBridgeType* Type = Defs.FindBridge(A.TemplateName);
		FBridgeModel Model;
		if (!Type || !LoadBridgeModel(*Type, Model))
		{
			UE_LOG(LogGenerals, Warning, TEXT("Bridge '%s' at (%.0f, %.0f): %s"), *A.TemplateName, A.Location.X, A.Location.Y,
				Type ? *FString::Printf(TEXT("model '%s' has no BRIDGE_LEFT part"), *Type->Model) : TEXT("no such bridge type"));
			continue;
		}
		const FVector3f Start(A.Location.X, A.Location.Y, Map.GetGroundHeight(A.Location.X, A.Location.Y) + BridgeFloatAmt);
		const FVector3f End(B.Location.X, B.Location.Y, Map.GetGroundHeight(B.Location.X, B.Location.Y) + BridgeFloatAmt);

		// W3DBridge::getBridgeInfo: the deck is as wide as the model's first part.
		FGenBridgeInfo& Info = Infos.AddDefaulted_GetRef();
		Info.Type = Type;
		Info.From = Start;
		Info.To = End;
		Info.Width = (Model.MaxY - Model.MinY) * Model.Scale;
		const FVector3f VecNormal = FVector3f(-(End.Y - Start.Y), End.X - Start.X, 0.f).GetSafeNormal();
		Info.FromLeft = Start + VecNormal * (Model.MaxY * Model.Scale);
		Info.FromRight = Start + VecNormal * (Model.MinY * Model.Scale);
		Info.ToLeft = End + VecNormal * (Model.MaxY * Model.Scale);
		Info.ToRight = End + VecNormal * (Model.MinY * Model.Scale);
		Info.ComputeBounds();

		FBridgeGeometry Geometry;
		const int32 NumSpans = BuildBridgeGeometry(Model, Start, End, Geometry);
		if (Geometry.Indices.Num() == 0)
		{
			continue;
		}
		// Lit like the terrain (the original lights a bridge's vertices with the terrain's lights), the
		// texture's alpha cut out: the road material does just that.
		UMaterialInstanceDynamic*& Material = TextureMaterials.FindOrAdd(Type->Texture.ToLower());
		if (!Material)
		{
			Material = UMaterialInstanceDynamic::Create(Assets->GetNamedMaterial(TEXT("M_Road")), this);
			if (UTexture2D* Tex = Assets->GetTexture(Type->Texture))
			{
				Material->SetTextureParameterValue(TEXT("RoadTexture"), Tex);
			}
			else
			{
				UE_LOG(LogGenerals, Warning, TEXT("Bridge texture '%s' not found"), *Type->Texture);
			}
			Materials.Add(Material);
		}
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetOwner() ? (UObject*)GetOwner() : (UObject*)this, NAME_None, RF_Transient);
		C->SetStaticMesh(BuildBridgeMesh(this, Geometry, Material));
		C->SetMaterial(0, Material);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCanEverAffectNavigation(false);
		C->SetupAttachment(this);
		C->RegisterComponent();
		Meshes.Add(C);
		UE_LOG(LogGenerals, Log, TEXT("Bridge '%s' (%s, %s): (%.0f, %.0f, %.1f) to (%.0f, %.0f, %.1f), %.0f long, deck %.0f wide, %d spans, %d triangles"),
			*A.TemplateName, *Type->Model, Model.bSectional ? TEXT("in sections") : TEXT("one piece"), Start.X, Start.Y, Start.Z, End.X, End.Y, End.Z,
			FVector2f::Distance(BridgeXY(Start), BridgeXY(End)), Info.Width, NumSpans, Geometry.Indices.Num() / 3);
	}
}
