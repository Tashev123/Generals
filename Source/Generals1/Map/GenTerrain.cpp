#include "Map/GenTerrain.h"
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
	TileOrigins.Reset();
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
				}
			}
		}
	}

	Atlas = GenTextureLoader::CreateFromImage(Img, TEXT("TerrainAtlas"), true, true);
	return Atlas != nullptr;
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

void UGenTerrainComponent::BuildChunk(const FGenMapData& Map, int32 X0, int32 Y0, int32 X1, int32 Y1)
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
	SM->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Terrain"), TEXT("Terrain")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	SM->BuildFromMeshDescriptions({ &MD }, Params);

	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetOwner() ? (UObject*)GetOwner() : (UObject*)this, NAME_None, RF_Transient);
	C->SetStaticMesh(SM);
	C->SetMaterial(0, Material);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCanEverAffectNavigation(false);
	C->SetupAttachment(this);
	C->RegisterComponent();
	Chunks.Add(C);
}
