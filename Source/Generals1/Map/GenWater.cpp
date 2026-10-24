#include "Map/GenWater.h"
#include "Core/GenMeshUtil.h"
#include "Core/GenCoords.h"
#include "Core/GenIni.h"
#include "Map/GenMapData.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace
{
	FGenWaterSettings GWaterSettings;

	/** "R:221 G:226 B:13 A:255" -> 0..1 */
	FLinearColor ParseColor01(const FString& Raw)
	{
		FLinearColor Out(1.f, 1.f, 1.f, 1.f);
		TArray<FString> Tokens;
		Raw.ParseIntoArrayWS(Tokens);
		for (const FString& T : Tokens)
		{
			if (T.Len() < 3 || T[1] != TEXT(':'))
			{
				continue;
			}
			const float V = FCString::Atof(*T.Mid(2)) / 255.f;
			switch (FChar::ToUpper(T[0]))
			{
			case TEXT('R'): Out.R = V; break;
			case TEXT('G'): Out.G = V; break;
			case TEXT('B'): Out.B = V; break;
			case TEXT('A'): Out.A = V; break;
			default: break;
			}
		}
		return Out;
	}

	void ApplyBlocks(const TArray<TSharedPtr<FGenIniBlock>>& Blocks, FGenWaterSettings& S)
	{
		static const TCHAR* TimeNames[4] = { TEXT("MORNING"), TEXT("AFTERNOON"), TEXT("EVENING"), TEXT("NIGHT") };
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (B->Keyword.Equals(TEXT("WaterSet"), ESearchCase::IgnoreCase))
			{
				for (int32 i = 0; i < 4; ++i)
				{
					if (B->Name().Equals(TimeNames[i], ESearchCase::IgnoreCase))
					{
						FGenWaterSet& Set = S.Sets[i];
						if (B->Find(TEXT("SkyTexture"))) { Set.SkyTexture = B->GetString(TEXT("SkyTexture")); }
						if (B->Find(TEXT("WaterTexture"))) { Set.WaterTexture = B->GetString(TEXT("WaterTexture")); }
						if (const FGenIniEntry* E = B->Find(TEXT("DiffuseColor"))) { Set.DiffuseColor = ParseColor01(E->Raw); }
					}
				}
			}
			else if (B->Keyword.Equals(TEXT("WaterTransparency"), ESearchCase::IgnoreCase))
			{
				if (B->Find(TEXT("TransparentWaterMinOpacity"))) { S.MinOpacity = B->GetFloat(TEXT("TransparentWaterMinOpacity"), S.MinOpacity); }
				if (B->Find(TEXT("TransparentWaterDepth"))) { S.TransparentDepth = B->GetFloat(TEXT("TransparentWaterDepth"), S.TransparentDepth); }
				if (const FGenIniEntry* E = B->Find(TEXT("StandingWaterColor"))) { S.StandingWaterColor = ParseColor01(E->Raw); }
				if (B->Find(TEXT("StandingWaterTexture"))) { S.StandingWaterTexture = B->GetString(TEXT("StandingWaterTexture")); }
				if (B->Find(TEXT("AdditiveBlending"))) { S.bAdditive = B->GetBool(TEXT("AdditiveBlending")); }
				if (const FGenIniEntry* E = B->Find(TEXT("RadarWaterColor"))) { S.RadarWaterColor = ParseColor01(E->Raw).ToFColor(false); }
			}
		}
	}

	/** The original lit in gamma space; convert so that linear-space shading lands on the same output. */
	FLinearColor WaterGammaToLinear(const FLinearColor& C)
	{
		auto F = [](float V) { return FMath::Pow(FMath::Clamp(V, 0.f, 4.f), 2.2f); };
		return FLinearColor(F(C.R), F(C.G), F(C.B), 1.f);
	}

	/** Manual exposure at bias 0 scales the scene by 1/1.2 (see AGenMapActor::SetupLighting). */
	constexpr float WaterExposure = 1.2f;

	/** One quad of a sheet of water or a river: four corners with their opacity (and river texture coordinates). */
	struct FWaterVertex
	{
		FVector3f Pos;      // Generals units
		float Alpha = 1.f;
		FVector2f UV0 = FVector2f::ZeroVector;
		FVector2f UV1 = FVector2f::ZeroVector;
	};

	UStaticMesh* BuildWaterMesh(UObject* Outer, const TArray<FWaterVertex>& Vertices, const TArray<int32>& Indices, UMaterialInterface* Material)
	{
		FMeshDescription MD;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();
		TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
		VUV.SetNumChannels(2);
		const FPolygonGroupID Group = MD.CreatePolygonGroup();
		Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Water");
		MD.ReserveNewVertices(Vertices.Num());
		MD.ReserveNewVertexInstances(Vertices.Num());
		MD.ReserveNewTriangles(Indices.Num() / 3);

		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(Vertices.Num());
		for (const FWaterVertex& V : Vertices)
		{
			const FVertexID Id = MD.CreateVertex();
			VPos[Id] = GenCoords::ToUEf(V.Pos.X, V.Pos.Y, V.Pos.Z);
			const FVertexInstanceID I = MD.CreateVertexInstance(Id);
			VNrm[I] = FVector3f(0.f, 0.f, 1.f);
			VTan[I] = FVector3f(1.f, 0.f, 0.f);
			VSign[I] = 1.f;
			VUV.Set(I, 0, V.UV0);
			VUV.Set(I, 1, V.UV1);
			VCol[I] = FVector4f(1.f, 1.f, 1.f, V.Alpha);
			Instances.Add(I);
		}
		for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
		{
			const FVertexInstanceID Tri[3] = { Instances[Indices[i]], Instances[Indices[i + 1]], Instances[Indices[i + 2]] };
			MD.CreateTriangle(Group, Tri);
		}

		UStaticMesh* SM = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
		SM->GetStaticMaterials().Add(GenRuntimeMaterialSlot(Material, TEXT("Water")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		SM->BuildFromMeshDescriptions({ &MD }, Params);
		return SM;
	}
}

const FGenWaterSettings& FGenWaterSettings::Get()
{
	return GWaterSettings;
}

void FGenWaterSettings::LoadForMap(const FString& MapFolder)
{
	GWaterSettings = FGenWaterSettings();
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Default\\Water.ini"), Blocks);
	GenIni::ParseFile(TEXT("Data\\INI\\Water.ini"), Blocks);
	ApplyBlocks(Blocks, GWaterSettings);
	if (!MapFolder.IsEmpty())
	{
		Blocks.Reset();
		GenIni::ParseFile(MapFolder / TEXT("map.ini"), Blocks);
		ApplyBlocks(Blocks, GWaterSettings);
	}
}

void UGenWaterComponent::Clear()
{
	for (UStaticMeshComponent* C : Meshes)
	{
		if (C) { C->DestroyComponent(); }
	}
	Meshes.Reset();
	Materials.Reset();
	NumWaterCells = 0;
}

UMaterialInstanceDynamic* UGenWaterComponent::MakeMaterial(const TCHAR* MaterialName, const FGenMapData& Map)
{
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	const FGenWaterSettings& S = FGenWaterSettings::Get();
	const FGenWaterSet& Set = S.Sets[FMath::Clamp(Map.TimeOfDay - 1, 0, 3)];
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Assets->GetNamedMaterial(MaterialName), this);
	if (UTexture2D* Water = Assets->GetTexture(S.StandingWaterTexture)) { M->SetTextureParameterValue(TEXT("WaterTexture"), Water); }
	if (UTexture2D* Sparkles = Assets->GetTexture(TEXT("WaterSurfaceBubbles.tga"))) { M->SetTextureParameterValue(TEXT("SparkleTexture"), Sparkles); }
	if (UTexture2D* Noise = Assets->GetTexture(TEXT("Noise0000.tga"))) { M->SetTextureParameterValue(TEXT("NoiseTexture"), Noise); }
	if (UTexture2D* Edge = Assets->GetTexture(TEXT("TWAlphaEdge.tga"))) { M->SetTextureParameterValue(TEXT("EdgeTexture"), Edge); }

	// WaterRenderObjClass::drawTrapezoidWater: with the standing water colour left white the water is
	// lit like the terrain and tinted by the WaterSet's diffuse colour ("legacy lighting code"); any
	// other colour is used as it is, unlit (and black means white: "special case where we disable lighting").
	const FLinearColor C = S.StandingWaterColor;
	const bool bLit = FMath::IsNearlyEqual(C.R, 1.f, 0.002f) && FMath::IsNearlyEqual(C.G, 1.f, 0.002f) && FMath::IsNearlyEqual(C.B, 1.f, 0.002f);
	FLinearColor Tint = bLit ? Set.DiffuseColor : C;
	if (!bLit && C.R <= 0.f && C.G <= 0.f && C.B <= 0.f)
	{
		Tint = FLinearColor::White;
	}
	// The light on a flat surface, worked out as the original does and in its (gamma) space: the terrain's
	// ambient term plus what each of the terrain's lights gives straight down, times the tint. The result
	// is the colour the texture is multiplied by; only then is it taken to linear space, so that
	// "texture x shade" comes out on screen as it did. (Summing lights in linear space instead - what
	// Unreal's lighting does - gives a much darker figure for the same numbers.)
	FLinearColor Shade = Tint;
	if (bLit)
	{
		const FGenMapLighting* L = Map.GetCurrentLighting();
		FLinearColor Light = (L && L->Terrain.Num() > 0) ? L->Terrain[0].Ambient : FLinearColor(0.3f, 0.3f, 0.3f);
		for (int32 i = 0; L && i < L->Terrain.Num() && i < 3; ++i)
		{
			const float Down = -L->Terrain[i].Direction.Z; // "-m_terrainLightPos[lightIndex].z"
			if (Down > 0.f)
			{
				Light.R += L->Terrain[i].Diffuse.R * Down;
				Light.G += L->Terrain[i].Diffuse.G * Down;
				Light.B += L->Terrain[i].Diffuse.B * Down;
			}
		}
		Shade = FLinearColor(Light.R * Tint.R, Light.G * Tint.G, Light.B * Tint.B);
	}
	Shade = FLinearColor(FMath::Clamp(Shade.R, 0.f, 1.f), FMath::Clamp(Shade.G, 0.f, 1.f), FMath::Clamp(Shade.B, 0.f, 1.f));
	M->SetVectorParameterValue(TEXT("WaterTint"), WaterGammaToLinear(Shade) * WaterExposure);
	M->SetScalarParameterValue(TEXT("SparkleScale"), WaterExposure);
	UE_LOG(LogGenerals, Log, TEXT("Water material %s: texture '%s'%s, shade (%.2f, %.2f, %.2f)%s"), MaterialName, *S.StandingWaterTexture,
		Assets->GetTexture(S.StandingWaterTexture) ? TEXT("") : TEXT(" MISSING"), Shade.R, Shade.G, Shade.B, bLit ? TEXT(" from the map's lights") : TEXT(" (the map's own water colour)"));
	Materials.Add(M);
	return M;
}

void UGenWaterComponent::Build(const FGenMapData& Map)
{
	Clear();
	int32 NumRivers = 0;
	BuildStanding(Map);
	for (const FGenPolygonTrigger& T : Map.PolygonTriggers)
	{
		if (T.bWater && T.bRiver && T.Points.Num() > 2)
		{
			BuildRiver(Map, T);
			++NumRivers;
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Water: %d cells of standing water, %d rivers (opaque at depth %.1f, at most %.0f%%)"), NumWaterCells, NumRivers,
		FGenWaterSettings::Get().TransparentDepth * FGenWaterSettings::Get().MinOpacity, FGenWaterSettings::Get().MinOpacity * 100.f);
}

void UGenWaterComponent::BuildStanding(const FGenMapData& Map)
{
	const FGenWaterSettings& S = FGenWaterSettings::Get();
	// BaseHeightMapRenderObjClass::updateShorelineTile: "maximum depth of water before we reach the min opacity".
	const float TransparentDepth = S.TransparentDepth * S.MinOpacity;
	const bool bSoftEdge = S.TransparentDepth != 0.f;

	// WaterRenderObjClass::getWaterHeight at a height map vertex: the highest water area it lies in (0: none).
	TArray<const FGenPolygonTrigger*> Areas;
	for (const FGenPolygonTrigger& T : Map.PolygonTriggers)
	{
		if (T.bWater && !T.bRiver && T.Points.Num() > 2)
		{
			Areas.Add(&T);
		}
	}
	if (Areas.Num() == 0)
	{
		return;
	}
	TArray<float> WaterZ;
	WaterZ.Init(0.f, Map.Width * Map.Height);
	for (const FGenPolygonTrigger* T : Areas)
	{
		FBox2f AreaBounds(ForceInit);
		for (const FIntVector& P : T->Points)
		{
			AreaBounds += FVector2f(float(P.X), float(P.Y));
		}
		const int32 X0 = FMath::Clamp(FMath::FloorToInt32(AreaBounds.Min.X / GenCoords::MapXYFactor) + Map.Border, 0, Map.Width - 1);
		const int32 X1 = FMath::Clamp(FMath::CeilToInt32(AreaBounds.Max.X / GenCoords::MapXYFactor) + Map.Border, 0, Map.Width - 1);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(AreaBounds.Min.Y / GenCoords::MapXYFactor) + Map.Border, 0, Map.Height - 1);
		const int32 Y1 = FMath::Clamp(FMath::CeilToInt32(AreaBounds.Max.Y / GenCoords::MapXYFactor) + Map.Border, 0, Map.Height - 1);
		const float Z = T->GetWaterHeight();
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				float& Cell = WaterZ[Y * Map.Width + X];
				if (Z >= Cell && Z > 0.f && T->Contains(float(X - Map.Border) * GenCoords::MapXYFactor, float(Y - Map.Border) * GenCoords::MapXYFactor))
				{
					Cell = Z;
				}
			}
		}
	}

	// One quad per height map cell that has water over any of its corners.
	UMaterialInstanceDynamic* Material = MakeMaterial(TEXT("M_Water"), Map);
	constexpr int32 ChunkCells = 64;
	const int32 CornerDX[4] = { 0, 1, 1, 0 };
	const int32 CornerDY[4] = { 0, 0, 1, 1 };
	for (int32 CY = 0; CY < Map.Height - 1; CY += ChunkCells)
	{
		for (int32 CX = 0; CX < Map.Width - 1; CX += ChunkCells)
		{
			TArray<FWaterVertex> Vertices;
			TArray<int32> Indices;
			for (int32 Y = CY; Y < FMath::Min(CY + ChunkCells, Map.Height - 1); ++Y)
			{
				for (int32 X = CX; X < FMath::Min(CX + ChunkCells, Map.Width - 1); ++X)
				{
					float Z[4], Ground[4];
					float Level = 0.f;
					bool bAnyWet = false, bAllInWater = true;
					for (int32 c = 0; c < 4; ++c)
					{
						const int32 VX = X + CornerDX[c], VY = Y + CornerDY[c];
						Z[c] = WaterZ[VY * Map.Width + VX];
						Ground[c] = Map.GetHeightAt(VX, VY);
						Level = FMath::Max(Level, Z[c]);
						bAllInWater &= Z[c] > 0.f;
						bAnyWet |= Z[c] > Ground[c];
					}
					if (!bAnyWet || Level <= 0.f)
					{
						continue; // the ground is above the water everywhere here (or there is none)
					}
					const int32 Base = Vertices.Num();
					for (int32 c = 0; c < 4; ++c)
					{
						FWaterVertex& V = Vertices.AddDefaulted_GetRef();
						const int32 VX = X + CornerDX[c], VY = Y + CornerDY[c];
						V.Pos = FVector3f(float(VX - Map.Border) * GenCoords::MapXYFactor, float(VY - Map.Border) * GenCoords::MapXYFactor, Level);
						// A cell on the rim of the water area isn't a shoreline tile in the original: it keeps
						// the opacity the frame buffer was cleared to. Everywhere else: depth over the depth
						// at which the water is as opaque as it gets.
						float Alpha = S.MinOpacity;
						if (bSoftEdge && bAllInWater && TransparentDepth > 0.f)
						{
							Alpha = FMath::Min(FMath::Clamp((Z[c] - Ground[c]) / TransparentDepth, 0.f, 1.f), S.MinOpacity);
						}
						V.Alpha = Alpha;
					}
					Indices.Append({ Base, Base + 2, Base + 3, Base, Base + 1, Base + 2 });
					++NumWaterCells;
				}
			}
			if (Indices.Num() == 0)
			{
				continue;
			}
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetOwner() ? (UObject*)GetOwner() : (UObject*)this, NAME_None, RF_Transient);
			C->SetStaticMesh(BuildWaterMesh(this, Vertices, Indices, Material));
			C->SetMaterial(0, Material);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCanEverAffectNavigation(false);
			C->SetCastShadow(false);
			C->SetupAttachment(this);
			C->RegisterComponent();
			Meshes.Add(C);
		}
	}
}

void UGenWaterComponent::BuildRiver(const FGenMapData& Map, const FGenPolygonTrigger& River)
{
	// WaterRenderObjClass::drawRiverWater: the polygon's points run down one bank and back up the other;
	// the edge at RiverStart is the river's end, and from there pairs of points - one on each bank - make
	// the ribbon. The texture repeats along it once per river width.
	const int32 N = River.Points.Num();
	const int32 RectangleCount = N / 2 - 1;
	int32 InnerNdx = River.RiverStart;
	if (RectangleCount < 1 || InnerNdx < 0 || InnerNdx >= N - 1)
	{
		return;
	}
	int32 OuterNdx = InnerNdx + 1;
	float EndLen = 0.f, TotalLen = 0.f;
	for (int32 i = 0; i < N - 1; ++i)
	{
		const float Len = FVector2f::Distance(FVector2f(float(River.Points[i].X), float(River.Points[i].Y)), FVector2f(float(River.Points[i + 1].X), float(River.Points[i + 1].Y)));
		TotalLen += Len;
		if (i == InnerNdx) { EndLen = Len; }
	}
	if (EndLen <= 0.f)
	{
		return;
	}
	const float LengthOfRiver = TotalLen / 2.f - EndLen;
	const float VScale = (LengthOfRiver / EndLen) / float(RectangleCount);

	TArray<FWaterVertex> Vertices;
	TArray<int32> Indices;
	for (int32 i = 0; i < N / 2; ++i)
	{
		const FIntVector InnerPt = River.Points[OuterNdx];
		const FIntVector OuterPt = River.Points[InnerNdx];
		++OuterNdx;
		--InnerNdx;
		if (InnerNdx < 0) { InnerNdx = N - 1; }
		if (OuterNdx >= N) { OuterNdx = 0; }
		const float V = VScale * float(i);
		FWaterVertex& A = Vertices.AddDefaulted_GetRef();
		A.Pos = FVector3f(float(InnerPt.X), float(InnerPt.Y), float(InnerPt.Z));
		A.UV0 = FVector2f(0.5f, V);
		A.UV1 = FVector2f(1.f, V);
		FWaterVertex& B = Vertices.AddDefaulted_GetRef();
		B.Pos = FVector3f(float(OuterPt.X), float(OuterPt.Y), float(OuterPt.Z));
		B.UV0 = FVector2f(0.f, V);
		B.UV1 = FVector2f(0.f, V);
	}
	for (int32 i = 0; i < RectangleCount; ++i)
	{
		Indices.Append({ i * 2, i * 2 + 1, i * 2 + 3, i * 2, i * 2 + 3, i * 2 + 2 });
	}

	UMaterialInstanceDynamic* Material = MakeMaterial(TEXT("M_River"), Map);
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetOwner() ? (UObject*)GetOwner() : (UObject*)this, NAME_None, RF_Transient);
	C->SetStaticMesh(BuildWaterMesh(this, Vertices, Indices, Material));
	C->SetMaterial(0, Material);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCanEverAffectNavigation(false);
	C->SetCastShadow(false);
	C->SetupAttachment(this);
	C->RegisterComponent();
	Meshes.Add(C);
}
