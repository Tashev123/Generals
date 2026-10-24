#include "Game/GenRadar.h"
#include "Logic/GenGameData.h"
#include "Map/GenBridgeInfo.h"
#include "Map/GenMapData.h"
#include "Map/GenRoads.h"
#include "Map/GenTerrain.h"
#include "Map/GenWater.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace
{
	/**
	 * W3DRadar::interpolateColorForHeight: lighter above MidZ (towards HiZ), darker below it (towards LoZ).
	 */
	void InterpolateColorForHeight(FLinearColor& Color, float Height, float HiZ, float MidZ, float LoZ)
	{
		constexpr float HowBright = 0.95f, HowDark = 0.60f;
		// "sanity on map height (flat maps bomb)"
		if (HiZ == MidZ) { HiZ = MidZ + 0.1f; }
		if (MidZ == LoZ) { LoZ = MidZ - 0.1f; }
		if (HiZ == LoZ) { HiZ = LoZ + 0.2f; }
		float T;
		FLinearColor Target;
		if (Height >= MidZ)
		{
			T = (Height - MidZ) / (HiZ - MidZ);
			Target = Color + (FLinearColor::White - Color) * HowBright;
		}
		else
		{
			T = (MidZ - Height) / (MidZ - LoZ);
			Target = Color + (FLinearColor::Black - Color) * HowDark;
		}
		Color = Color + (Target - Color) * T;
		Color.R = FMath::Clamp(Color.R, 0.f, 1.f);
		Color.G = FMath::Clamp(Color.G, 0.f, 1.f);
		Color.B = FMath::Clamp(Color.B, 0.f, 1.f);
		Color.A = 1.f;
	}
}

FBox2D GenRadar::FindDrawRect(const FBox2D& Window, const FVector2f& MapSize)
{
	const FVector2D Size = Window.GetSize();
	if (MapSize.X <= 0.f || MapSize.Y <= 0.f || Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return Window;
	}
	const double RatioW = MapSize.X / Size.X, RatioH = MapSize.Y / Size.Y;
	if (RatioW >= RatioH)
	{
		// A wide map: bars above and below.
		const double H = MapSize.Y / RatioW;
		const double Top = (Size.Y - H) * 0.5;
		return FBox2D(Window.Min + FVector2D(0.0, Top), Window.Min + FVector2D(Size.X, Size.Y - Top));
	}
	const double W = MapSize.X / RatioH;
	const double Left = (Size.X - W) * 0.5;
	return FBox2D(Window.Min + FVector2D(Left, 0.0), Window.Min + FVector2D(Size.X - Left, Size.Y));
}

FIntPoint GenRadar::WorldToRadar(const FVector2f& World, const FVector2f& MapSize)
{
	const float XSample = FMath::Max(MapSize.X / float(CellWidth), 0.001f), YSample = FMath::Max(MapSize.Y / float(CellHeight), 0.001f);
	// (The original converts with a plain cast; "keep it in bounds".)
	return FIntPoint(FMath::Clamp(int32(World.X / XSample), 0, CellWidth - 1), FMath::Clamp(int32(World.Y / YSample), 0, CellHeight - 1));
}

FVector2D GenRadar::RadarToPixel(const FVector2D& Radar, const FBox2D& DrawRect)
{
	const FVector2D Size = DrawRect.GetSize();
	// "Note the inverted y here to orient the way our world looks with +x = right and -y = down."
	return FVector2D(DrawRect.Min.X + Radar.X * Size.X / CellWidth, DrawRect.Min.Y + (double(CellHeight) - Radar.Y) * Size.Y / CellHeight);
}

bool GenRadar::PixelToWorld(const FVector2D& Pixel, const FBox2D& Window, const FVector2f& MapSize, FVector2f& OutWorld)
{
	const FBox2D Rect = FindDrawRect(Window, MapSize);
	const FVector2D Size = Rect.GetSize();
	if (Size.X <= 0.0 || Size.Y <= 0.0 || Pixel.X < Rect.Min.X || Pixel.X > Rect.Max.X || Pixel.Y < Rect.Min.Y || Pixel.Y > Rect.Max.Y)
	{
		return false;
	}
	OutWorld.X = float((Pixel.X - Rect.Min.X) / Size.X) * MapSize.X;
	OutWorld.Y = float(1.0 - (Pixel.Y - Rect.Min.Y) / Size.Y) * MapSize.Y;
	return true;
}

UTexture2D* GenRadar::BuildTerrainTexture(const FGenMapData& Map, const UGenTerrainComponent& Terrain, const TArray<FGenBridgeInfo>& Bridges, float& OutAverageZ)
{
	const FVector2f MapSize = Map.GetPlayableBounds().Max;
	const float XSample = MapSize.X / float(CellWidth), YSample = MapSize.Y / float(CellHeight);
	const FColor WaterByte = FGenWaterSettings::Get().RadarWaterColor; // (a map may have its own)
	const FLinearColor WaterColor(WaterByte.R / 255.f, WaterByte.G / 255.f, WaterByte.B / 255.f, 1.f);

	// TerrainLogic::getExtent: the lowest and the highest point of the map.
	float LoZ = TNumericLimits<float>::Max(), HiZ = -TNumericLimits<float>::Max();
	for (int32 Y = 0; Y < Map.Height; ++Y)
	{
		for (int32 X = 0; X < Map.Width; ++X)
		{
			const float Z = Map.GetHeightAt(X, Y);
			LoZ = FMath::Min(LoZ, Z);
			HiZ = FMath::Max(HiZ, Z);
		}
	}

	// Radar::newMap: the average height of the dry land ("since we're averaging let's skip every second sample").
	double SumZ = 0.0;
	int32 NumZ = 0;
	for (int32 Y = 0; Y < CellHeight; Y += 2)
	{
		for (int32 X = 0; X < CellWidth; X += 2)
		{
			const float WX = X * XSample, WY = Y * YSample;
			if (!Map.IsUnderwater(WX, WY))
			{
				SumZ += Map.GetGroundHeight(WX, WY);
				++NumZ;
			}
		}
	}
	OutAverageZ = NumZ > 0 ? float(SumZ / NumZ) : 0.f;

	// One sample per radar cell: its colour as land and, where it is under water, as water.
	TArray<FLinearColor> Land, Water;
	TArray<bool> bWet, bBridge;
	Land.SetNumUninitialized(CellWidth * CellHeight);
	Water.SetNumUninitialized(CellWidth * CellHeight);
	bWet.SetNumUninitialized(CellWidth * CellHeight);
	bBridge.Init(false, CellWidth * CellHeight);
	for (int32 Y = 0; Y < CellHeight; ++Y)
	{
		for (int32 X = 0; X < CellWidth; ++X)
		{
			const int32 i = Y * CellWidth + X;
			const float WX = X * XSample, WY = Y * YSample;
			const float Z = Map.GetGroundHeight(WX, WY);
			// "check to see if this point is part of a working bridge": it has the bridge type's colour,
			// shaded for the bridge's height, and none of the water under it.
			const FGenBridgeInfo* Bridge = Bridges.FindByPredicate([&](const FGenBridgeInfo& B) { return B.IsPointOnBridge(WX, WY); });
			if (Bridge && Bridge->Type)
			{
				const FColor RC = Bridge->Type->RadarColor;
				Land[i] = FLinearColor(RC.R / 255.f, RC.G / 255.f, RC.B / 255.f, 1.f);
				InterpolateColorForHeight(Land[i], (Bridge->FromLeft.Z + Bridge->FromRight.Z + Bridge->ToLeft.Z + Bridge->ToRight.Z) / 4.f, OutAverageZ, HiZ, LoZ);
				Water[i] = Land[i];
				bWet[i] = false;
				bBridge[i] = true;
				continue;
			}
			float WaterZ = 0.f;
			bWet[i] = Map.IsUnderwater(WX, WY, &WaterZ);
			// (The original passes these three heights in this order: the terrain average as "high" and the
			// map's highest point as "middle". So land only ever gets darker, the lower it lies.)
			Land[i] = Terrain.GetTerrainColorAt(Map, WX, WY);
			InterpolateColorForHeight(Land[i], Z, OutAverageZ, HiZ, LoZ);
			Water[i] = WaterColor;
			if (bWet[i])
			{
				InterpolateColorForHeight(Water[i], Z, WaterZ, WaterZ, LoZ);
			}
		}
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(CellWidth, CellHeight, PF_B8G8R8A8, TEXT("GenRadarTerrain"));
	if (!Texture)
	{
		return nullptr;
	}
	Texture->SRGB = true;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->NeverStream = true;
	FTexturePlatformData* Platform = Texture->GetPlatformData();
	uint8* Dst = Platform ? static_cast<uint8*>(Platform->Mips[0].BulkData.Lock(LOCK_READ_WRITE)) : nullptr;
	if (!Dst)
	{
		return nullptr;
	}
	for (int32 Y = 0; Y < CellHeight; ++Y)
	{
		for (int32 X = 0; X < CellWidth; ++X)
		{
			// "How many tiles from the center tile we will sample away": one, so each cell is the average of
			// itself and its neighbours (for water: of those that are water too).
			const bool bWater = bWet[Y * CellWidth + X];
			FLinearColor Sum(0.f, 0.f, 0.f, 0.f);
			int32 Samples = 0;
			for (int32 J = Y - 1; J <= Y + 1; ++J)
			{
				for (int32 I = X - 1; I <= X + 1; ++I)
				{
					if (I < 0 || J < 0 || I >= CellWidth || J >= CellHeight)
					{
						continue;
					}
					const int32 n = J * CellWidth + I;
					if (bWater && !bWet[n])
					{
						continue;
					}
					Sum += bWater ? Water[n] : Land[n];
					++Samples;
				}
			}
			const FLinearColor C = bBridge[Y * CellWidth + X] ? Land[Y * CellWidth + X] : Sum / float(FMath::Max(Samples, 1));
			// North is up on the radar: the top row of the picture is the map's far edge.
			uint8* P = Dst + ((CellHeight - 1 - Y) * CellWidth + X) * 4;
			P[0] = uint8(FMath::Clamp(C.B, 0.f, 1.f) * 255.f);
			P[1] = uint8(FMath::Clamp(C.G, 0.f, 1.f) * 255.f);
			P[2] = uint8(FMath::Clamp(C.R, 0.f, 1.f) * 255.f);
			P[3] = 255;
		}
	}
	Platform->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}

bool GenRadar::ClipLine(FVector2D& A, FVector2D& B, const FBox2D& Rect)
{
	// Liang-Barsky
	double T0 = 0.0, T1 = 1.0;
	const FVector2D D = B - A;
	const double P[4] = { -D.X, D.X, -D.Y, D.Y };
	const double Q[4] = { A.X - Rect.Min.X, Rect.Max.X - A.X, A.Y - Rect.Min.Y, Rect.Max.Y - A.Y };
	for (int32 i = 0; i < 4; ++i)
	{
		if (FMath::Abs(P[i]) < 1e-9)
		{
			if (Q[i] < 0.0) { return false; }
			continue;
		}
		const double R = Q[i] / P[i];
		if (P[i] < 0.0) { T0 = FMath::Max(T0, R); } else { T1 = FMath::Min(T1, R); }
		if (T0 > T1) { return false; }
	}
	const FVector2D A0 = A;
	A = A0 + D * T0;
	B = A0 + D * T1;
	return true;
}
