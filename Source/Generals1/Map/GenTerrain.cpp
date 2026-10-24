#include "Map/GenTerrain.h"
#include "Core/GenMeshUtil.h"
#include "Core/GenCoords.h"
#include "Core/GenFileSystem.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapData.h"
#include "W3D/GenAssets.h"
#include "W3D/GenTextureLoader.h"
#include "Generals1.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace
{
	constexpr int32 TilePixels = 64;      // TILE_PIXEL_EXTENT
	constexpr int32 AtlasPadding = 16;    // wrap-around border around each texture class block
	constexpr int32 ChunkCells = 32;
	constexpr uint8 InvertedMask = 0x1;
	constexpr uint8 FlippedMask = 0x2;

	/** WorldHeightMap::getAlphaUVData: per-corner alpha (0:SW,1:SE,2:NE,3:NW) and diagonal flip. */
	void BlendAlpha(const FGenBlendedTile& B, uint8 Alpha[4], bool& bFlip)
	{
		Alpha[0] = Alpha[1] = Alpha[2] = Alpha[3] = 0;
		bFlip = false;
		const bool bInv = (B.Inverted & InvertedMask) != 0;
		if (B.Horiz)
		{
			bFlip = (B.Inverted & FlippedMask) != 0;
			if (bInv) { Alpha[0] = Alpha[3] = 255; } else { Alpha[1] = Alpha[2] = 255; }
		}
		if (B.Vert)
		{
			bFlip = (B.Inverted & FlippedMask) != 0;
			if (bInv) { Alpha[0] = Alpha[1] = 255; } else { Alpha[2] = Alpha[3] = 255; }
		}
		if (B.RightDiagonal)
		{
			if (bInv)
			{
				Alpha[1] = 255;
				if (B.LongDiagonal) { Alpha[0] = Alpha[2] = 255; }
			}
			else
			{
				bFlip = true;
				Alpha[2] = 255;
				if (B.LongDiagonal) { Alpha[1] = Alpha[3] = 255; }
			}
		}
		if (B.LeftDiagonal)
		{
			if (bInv)
			{
				bFlip = true;
				Alpha[0] = 255;
				if (B.LongDiagonal) { Alpha[1] = Alpha[3] = 255; }
			}
			else
			{
				Alpha[3] = 255;
				if (B.LongDiagonal) { Alpha[0] = Alpha[2] = 255; }
			}
		}
		if (B.CustomBlendEdgeClass >= 0)
		{
			Alpha[0] = Alpha[1] = Alpha[2] = Alpha[3] = 0;
			bFlip = false;
		}
	}
}

void UGenTerrainComponent::Clear()
{
	for (UStaticMeshComponent* C : Chunks)
	{
		if (C) { C->DestroyComponent(); }
	}
	Chunks.Reset();
	ChunkRects.Reset();
	DirtyChunks.Reset();
	TileOrigins.Reset();
	TileColors.Reset();
	Atlas = nullptr;
	Material = nullptr;
}

bool UGenTerrainComponent::BuildAtlas(const FGenMapData& Map)
{
	struct FClassImage
	{
		const FGenTextureClass* Class = nullptr;
		FGenImage Image; // file row order (what WorldHeightMap::readTiles iterates)
		int32 BlockSize = 0;
		FIntPoint Pos;
	};
	TArray<FClassImage> Images;
	const FGenGameData& GD = FGenGameData::Get();
	for (const FGenTextureClass& TC : Map.TextureClasses)
	{
		const FGenTerrainType* Type = GD.FindTerrain(TC.Name);
		if (!Type)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Terrain type '%s' not found"), *TC.Name);
			continue;
		}
		TArray<uint8> Bytes;
		FClassImage CI;
		if (!FGenFileSystem::Get().ReadFile(TEXT("Art\\Terrain\\") + Type->Texture, Bytes) || !GenTextureLoader::DecodeTGA(Bytes, CI.Image, true))
		{
			UE_LOG(LogGenerals, Warning, TEXT("Terrain texture '%s' missing"), *Type->Texture);
			continue;
		}
		CI.Class = &TC;
		CI.BlockSize = TC.Width * TilePixels + 2 * AtlasPadding;
		Images.Add(MoveTemp(CI));
	}
	if (Images.Num() == 0)
	{
		return false;
	}

	// Shelf-pack class blocks, tallest first.
	Images.Sort([](const FClassImage& A, const FClassImage& B) { return A.BlockSize > B.BlockSize; });
	int32 AtlasWidth = 2048;
	for (const FClassImage& CI : Images) { AtlasWidth = FMath::Max(AtlasWidth, int32(FMath::RoundUpToPowerOfTwo(CI.BlockSize))); }
	int32 X = 0, Y = 0, ShelfH = 0;
	for (FClassImage& CI : Images)
	{
		if (X + CI.BlockSize > AtlasWidth)
		{
			X = 0;
			Y += ShelfH;
			ShelfH = 0;
		}
		CI.Pos = FIntPoint(X, Y);
		X += CI.BlockSize;
		ShelfH = FMath::Max(ShelfH, CI.BlockSize);
	}
	const int32 AtlasHeight = int32(FMath::RoundUpToPowerOfTwo(Y + ShelfH));
	AtlasSize = FIntPoint(AtlasWidth, AtlasHeight);

	FGenImage Img;
	Img.Width = AtlasWidth;
	Img.Height = AtlasHeight;
	Img.Pixels.Init(FColor(128, 128, 128, 255), AtlasWidth * AtlasHeight);

	int32 MaxTile = 0;
	for (const FGenTextureClass& TC : Map.TextureClasses) { MaxTile = FMath::Max(MaxTile, TC.FirstTile + TC.NumTiles); }
	TileOrigins.Init(FIntPoint(-1, -1), MaxTile + 1);
	TileColors.Init(FColor::Black, MaxTile + 1);

	for (const FClassImage& CI : Images)
	{
		const int32 W = CI.Class->Width;
		const int32 Span = W * TilePixels;
		// Copy the class image (file row r -> atlas row r) with wrap-around padding.
		for (int32 py = -AtlasPadding; py < Span + AtlasPadding; ++py)
		{
			const int32 SrcY = ((py % Span) + Span) % Span;
			for (int32 px = -AtlasPadding; px < Span + AtlasPadding; ++px)
			{
				const int32 SrcX = ((px % Span) + Span) % Span;
				const FColor C = (SrcX < CI.Image.Width && SrcY < CI.Image.Height) ? CI.Image.At(SrcX, SrcY) : FColor::Black;
				Img.At(CI.Pos.X + AtlasPadding + px, CI.Pos.Y + AtlasPadding + py) = FColor(C.R, C.G, C.B, 255);
			}
		}
		// readTiles: tileNdx = column/64 + width * (row/64), row in file order.
		for (int32 ty = 0; ty < W; ++ty)
		{
			for (int32 tx = 0; tx < W; ++tx)
			{
				const int32 Tile = CI.Class->FirstTile + tx + W * ty;
				if (TileOrigins.IsValidIndex(Tile))
				{
					TileOrigins[Tile] = FIntPoint(CI.Pos.X + AtlasPadding + tx * TilePixels, CI.Pos.Y + AtlasPadding + ty * TilePixels);
					// TileData::getRGBDataForWidth(1): the whole tile averaged into one pixel (the radar's terrain colour).
					uint32 SumR = 0, SumG = 0, SumB = 0;
					for (int32 py = 0; py < TilePixels; ++py)
					{
						for (int32 px = 0; px < TilePixels; ++px)
						{
							const int32 SX = tx * TilePixels + px, SY = ty * TilePixels + py;
							const FColor C = (SX < CI.Image.Width && SY < CI.Image.Height) ? CI.Image.At(SX, SY) : FColor::Black;
							SumR += C.R; SumG += C.G; SumB += C.B;
						}
					}
					const uint32 N = TilePixels * TilePixels;
					TileColors[Tile] = FColor(uint8(SumR / N), uint8(SumG / N), uint8(SumB / N), 255);
				}
			}
		}
	}

	Atlas = GenTextureLoader::CreateFromImage(Img, TEXT("TerrainAtlas"), true, true);
	return Atlas != nullptr;
}

FLinearColor UGenTerrainComponent::GetTerrainColorAt(const FGenMapData& Map, float WorldX, float WorldY) const
{
	const int32 X = FMath::Clamp(FMath::FloorToInt32(WorldX / GenCoords::MapXYFactor) + Map.Border, 0, Map.Width - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt32(WorldY / GenCoords::MapXYFactor) + Map.Border, 0, Map.Height - 1);
	const int32 Ndx = Y * Map.Width + X;
	const int32 Tile = Map.TileNdx.IsValidIndex(Ndx) ? (Map.TileNdx[Ndx] >> 2) : -1; // "we pack 4 grids into a tile"
	if (!TileColors.IsValidIndex(Tile))
	{
		return FLinearColor::Black;
	}
	const FColor C = TileColors[Tile];
	return FLinearColor(C.R / 255.f, C.G / 255.f, C.B / 255.f, 1.f); // (gamma-space values, as the original works with)
}

void UGenTerrainComponent::ApplyCloudAndLightMap(UMaterialInstanceDynamic* Material, const FGenMapData& Map)
{
	// FlatHeightMapRenderObjClass::Render: "if (TheGlobalData->m_timeOfDay == TIME_OF_DAY_NIGHT) doCloud = false".
	const FGenGameData& GD = FGenGameData::Get();
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Material || !Assets)
	{
		return;
	}
	constexpr int32 TimeOfDayNight = 4;
	const bool bClouds = GD.bUseCloudMap && Map.TimeOfDay != TimeOfDayNight;
	UTexture2D* Cloud = bClouds ? Assets->GetTexture(TEXT("TSCloudMed.tga")) : nullptr;
	UTexture2D* Noise = GD.bUseLightMap ? Assets->GetTexture(TEXT("TSNoiseUrb.tga")) : nullptr;
	if (Cloud) { Material->SetTextureParameterValue(TEXT("CloudTexture"), Cloud); }
	if (Noise) { Material->SetTextureParameterValue(TEXT("LightMapTexture"), Noise); }
	Material->SetScalarParameterValue(TEXT("CloudAmount"), Cloud ? 1.f : 0.f);
	Material->SetScalarParameterValue(TEXT("LightMapAmount"), Noise ? 1.f : 0.f);
}

bool UGenTerrainComponent::Build(const FGenMapData& Map)
{
	Clear();
	if (!BuildAtlas(Map))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Terrain atlas could not be built"));
	}

	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	Material = UMaterialInstanceDynamic::Create(Assets->GetTerrainMaterial(), this);
	if (Atlas)
	{
		Material->SetTextureParameterValue(TEXT("Atlas"), Atlas);
	}
	ApplyCloudAndLightMap(Material, Map);

	const int32 CellsX = Map.Width - 1;
	const int32 CellsY = Map.Height - 1;
	for (int32 Y0 = 0; Y0 < CellsY; Y0 += ChunkCells)
	{
		for (int32 X0 = 0; X0 < CellsX; X0 += ChunkCells)
		{
			BuildChunk(Map, X0, Y0, FMath::Min(X0 + ChunkCells, CellsX), FMath::Min(Y0 + ChunkCells, CellsY));
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Terrain: %dx%d cells, %d chunks, atlas %dx%d"), CellsX, CellsY, Chunks.Num(), AtlasSize.X, AtlasSize.Y);
	return true;
}

bool UGenTerrainComponent::IsCellFlipped(const FGenMapData& Map, int32 X, int32 Y) const
{
	// (as BuildChunk decides it: by the cell's first blend)
	const int32 Ndx = Y * Map.Width + X;
	const int32 B1 = Map.BlendNdx.IsValidIndex(Ndx) ? Map.BlendNdx[Ndx] : 0;
	if (B1 <= 0 || !Map.BlendedTiles.IsValidIndex(B1))
	{
		return false;
	}
	const FGenBlendedTile& BT = Map.BlendedTiles[B1];
	const int32 Base = BT.BlendNdx >> 2;
	if (!TileOrigins.IsValidIndex(Base) || TileOrigins[Base].X < 0)
	{
		return false;
	}
	uint8 Alpha[4];
	bool bFlip = false;
	BlendAlpha(BT, Alpha, bFlip);
	return bFlip;
}

void UGenTerrainComponent::MarkCellsDirty(const FGenMapData& Map, int32 X0, int32 Y0, int32 X1, int32 Y1)
{
	for (int32 i = 0; i < ChunkRects.Num(); ++i)
	{
		const FIntRect& R = ChunkRects[i];
		if (X0 < R.Max.X && X1 >= R.Min.X && Y0 < R.Max.Y && Y1 >= R.Min.Y)
		{
			DirtyChunks.AddUnique(i);
		}
	}
}

bool UGenTerrainComponent::RebuildDirtyChunk(const FGenMapData& Map)
{
	if (DirtyChunks.Num() == 0)
	{
		return false;
	}
	const int32 Index = DirtyChunks.Pop(EAllowShrinking::No);
	if (ChunkRects.IsValidIndex(Index))
	{
		const FIntRect R = ChunkRects[Index];
		BuildChunk(Map, R.Min.X, R.Min.Y, R.Max.X, R.Max.Y, Index);
	}
	return true;
}

void UGenTerrainComponent::BuildChunk(const FGenMapData& Map, int32 X0, int32 Y0, int32 X1, int32 Y1, int32 ReplaceIndex)
{
	const float InvW = AtlasSize.X > 0 ? 1.f / float(AtlasSize.X) : 0.f;
	const float InvH = AtlasSize.Y > 0 ? 1.f / float(AtlasSize.Y) : 0.f;

	// WorldHeightMap::getUVForNdx -> corner UVs (0:SW,1:SE,2:NE,3:NW).
	auto TileUVs = [&](int32 TileNdx, FVector2f UV[4]) -> bool
	{
		const int32 Base = TileNdx >> 2;
		if (!TileOrigins.IsValidIndex(Base) || TileOrigins[Base].X < 0)
		{
			for (int32 i = 0; i < 4; ++i) { UV[i] = FVector2f::ZeroVector; }
			return false;
		}
		const FIntPoint O = TileOrigins[Base];
		float MinU = float(O.X), MaxU = float(O.X + TilePixels);
		float MinV = float(O.Y), MaxV = float(O.Y + TilePixels);
		const float MidU = (MinU + MaxU) * 0.5f, MidV = (MinV + MaxV) * 0.5f;
		if (TileNdx & 2) { MaxV = MidV; } else { MinV = MidV; }
		if (TileNdx & 1) { MinU = MidU; } else { MaxU = MidU; }
		MinU *= InvW; MaxU *= InvW; MinV *= InvH; MaxV *= InvH;
		UV[0] = FVector2f(MinU, MaxV);
		UV[1] = FVector2f(MaxU, MaxV);
		UV[2] = FVector2f(MaxU, MinV);
		UV[3] = FVector2f(MinU, MinV);
		return true;
	};

	auto VertexNormal = [&](int32 X, int32 Y) -> FVector3f
	{
		const float DX = Map.GetHeightAt(X + 1, Y) - Map.GetHeightAt(X - 1, Y);
		const float DY = Map.GetHeightAt(X, Y + 1) - Map.GetHeightAt(X, Y - 1);
		const FVector3f N(-DX, -DY, 2.f * GenCoords::MapXYFactor);
		return GenCoords::DirToUE(N.X, N.Y, N.Z).GetSafeNormal();
	};

	FMeshDescription MD;
	FStaticMeshAttributes Attr(MD);
	Attr.Register();
	TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
	VUV.SetNumChannels(3);
	const FPolygonGroupID Group = MD.CreatePolygonGroup();
	Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Terrain");

	const int32 NumCells = (X1 - X0) * (Y1 - Y0);
	MD.ReserveNewVertices(NumCells * 4);
	MD.ReserveNewVertexInstances(NumCells * 4);
	MD.ReserveNewTriangles(NumCells * 2);

	const int32 CornerDX[4] = { 0, 1, 1, 0 };
	const int32 CornerDY[4] = { 0, 0, 1, 1 };

	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			const int32 Ndx = Y * Map.Width + X;
			FVector2f Base[4], Blend[4], Extra[4];
			uint8 A1[4] = { 0, 0, 0, 0 }, A2[4] = { 0, 0, 0, 0 };
			bool bFlip = false;

			TileUVs(Map.TileNdx.IsValidIndex(Ndx) ? Map.TileNdx[Ndx] : 0, Base);
			for (int32 i = 0; i < 4; ++i) { Blend[i] = Base[i]; Extra[i] = Base[i]; }

			const int32 B1 = Map.BlendNdx.IsValidIndex(Ndx) ? Map.BlendNdx[Ndx] : 0;
			if (B1 > 0 && Map.BlendedTiles.IsValidIndex(B1))
			{
				const FGenBlendedTile& BT = Map.BlendedTiles[B1];
				if (TileUVs(BT.BlendNdx, Blend))
				{
					BlendAlpha(BT, A1, bFlip);
				}
			}
			const int32 B2 = Map.ExtraBlendNdx.IsValidIndex(Ndx) ? Map.ExtraBlendNdx[Ndx] : 0;
			if (B2 > 0 && Map.BlendedTiles.IsValidIndex(B2))
			{
				const FGenBlendedTile& BT = Map.BlendedTiles[B2];
				bool bFlip2 = false;
				if (TileUVs(BT.BlendNdx, Extra))
				{
					BlendAlpha(BT, A2, bFlip2);
				}
			}

			FVertexInstanceID Inst[4];
			for (int32 c = 0; c < 4; ++c)
			{
				const int32 VX = X + CornerDX[c];
				const int32 VY = Y + CornerDY[c];
				const float WX = float(VX - Map.Border) * GenCoords::MapXYFactor;
				const float WY = float(VY - Map.Border) * GenCoords::MapXYFactor;
				const FVertexID V = MD.CreateVertex();
				VPos[V] = GenCoords::ToUEf(WX, WY, Map.GetHeightAt(VX, VY));
				const FVertexInstanceID I = MD.CreateVertexInstance(V);
				const FVector3f N = VertexNormal(VX, VY);
				VNrm[I] = N;
				VTan[I] = FVector3f::CrossProduct(FVector3f(0.f, 1.f, 0.f), N).GetSafeNormal();
				VSign[I] = 1.f;
				VUV.Set(I, 0, Base[c]);
				VUV.Set(I, 1, Blend[c]);
				VUV.Set(I, 2, Extra[c]);
				VCol[I] = FVector4f(float(A1[c]) / 255.f, float(A2[c]) / 255.f, 0.f, 1.f);
				Inst[c] = I;
			}
			// HeightMap.cpp index order: (0,2,3),(0,1,2); flipped cells rotate the corners by one.
			if (!bFlip)
			{
				const FVertexInstanceID T0[3] = { Inst[0], Inst[2], Inst[3] };
				const FVertexInstanceID T1[3] = { Inst[0], Inst[1], Inst[2] };
				MD.CreateTriangle(Group, T0);
				MD.CreateTriangle(Group, T1);
			}
			else
			{
				const FVertexInstanceID T0[3] = { Inst[1], Inst[3], Inst[0] };
				const FVertexInstanceID T1[3] = { Inst[1], Inst[2], Inst[3] };
				MD.CreateTriangle(Group, T0);
				MD.CreateTriangle(Group, T1);
			}
		}
	}

	UStaticMesh* SM = NewObject<UStaticMesh>(this, NAME_None, RF_Transient);
	SM->GetStaticMaterials().Add(GenRuntimeMaterialSlot(Material, TEXT("Terrain")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	SM->BuildFromMeshDescriptions({ &MD }, Params);

	if (Chunks.IsValidIndex(ReplaceIndex) && Chunks[ReplaceIndex])
	{
		// (The same component, a new mesh: nothing of the old one is left on screen for a frame.)
		Chunks[ReplaceIndex]->SetStaticMesh(SM);
		Chunks[ReplaceIndex]->SetMaterial(0, Material);
		return;
	}
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetOwner() ? (UObject*)GetOwner() : (UObject*)this, NAME_None, RF_Transient);
	C->SetStaticMesh(SM);
	C->SetMaterial(0, Material);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCanEverAffectNavigation(false);
	C->SetupAttachment(this);
	C->RegisterComponent();
	Chunks.Add(C);
	ChunkRects.Add(FIntRect(X0, Y0, X1, Y1));
}
