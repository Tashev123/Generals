#include "W3D/GenTextureLoader.h"
#include "Core/GenFileSystem.h"
#include "Generals1.h"
#include "Engine/Texture2D.h"
#include "Misc/Paths.h"
#include "TextureResource.h"

namespace
{
	uint32 LE32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }

	UTexture2D* NewTransientTexture(const FString& DebugName, int32 Width, int32 Height, EPixelFormat Format, bool bSRGB)
	{
		const FName Name = MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(*FString::Printf(TEXT("T_%s"), *DebugName)));
		UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage(), Name, RF_Transient);
		FTexturePlatformData* PD = new FTexturePlatformData();
		PD->SizeX = Width;
		PD->SizeY = Height;
		PD->SetNumSlices(1);
		PD->PixelFormat = Format;
		Tex->SetPlatformData(PD);
		Tex->bNotOfflineProcessed = true;
		Tex->SRGB = bSRGB;
		Tex->Filter = TF_Trilinear;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->NeverStream = true;
		return Tex;
	}

	void AddMip(UTexture2D* Tex, int32 W, int32 H, const void* Data, int64 Bytes)
	{
		FTexture2DMipMap* Mip = new FTexture2DMipMap(W, H, 1);
		Tex->GetPlatformData()->Mips.Add(Mip);
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Dest = Mip->BulkData.Realloc(Bytes);
		FMemory::Memcpy(Dest, Data, Bytes);
		Mip->BulkData.Unlock();
	}

	void Downsample(const FGenImage& Src, FGenImage& Dst)
	{
		Dst.Width = FMath::Max(1, Src.Width / 2);
		Dst.Height = FMath::Max(1, Src.Height / 2);
		Dst.Pixels.SetNumUninitialized(Dst.Width * Dst.Height);
		for (int32 y = 0; y < Dst.Height; ++y)
		{
			for (int32 x = 0; x < Dst.Width; ++x)
			{
				const int32 x0 = FMath::Min(x * 2, Src.Width - 1), x1 = FMath::Min(x * 2 + 1, Src.Width - 1);
				const int32 y0 = FMath::Min(y * 2, Src.Height - 1), y1 = FMath::Min(y * 2 + 1, Src.Height - 1);
				const FColor& A = Src.At(x0, y0); const FColor& B = Src.At(x1, y0);
				const FColor& C = Src.At(x0, y1); const FColor& D = Src.At(x1, y1);
				Dst.At(x, y) = FColor(
					uint8((A.R + B.R + C.R + D.R + 2) / 4), uint8((A.G + B.G + C.G + D.G + 2) / 4),
					uint8((A.B + B.B + C.B + D.B + 2) / 4), uint8((A.A + B.A + C.A + D.A + 2) / 4));
			}
		}
	}
}

bool GenTextureLoader::DecodeTGA(TConstArrayView<uint8> Data, FGenImage& Out, bool bFileRowOrder)
{
	if (Data.Num() < 18)
	{
		return false;
	}
	const uint8* H = Data.GetData();
	const int32 IdLen = H[0];
	const int32 ColorMapType = H[1];
	const int32 ImageType = H[2];
	const int32 CmapLen = H[5] | (H[6] << 8);
	const int32 CmapBits = H[7];
	const int32 Width = H[12] | (H[13] << 8);
	const int32 Height = H[14] | (H[15] << 8);
	const int32 Bpp = H[16];
	const bool bTopDown = (H[17] & 0x20) != 0;
	const bool bRLE = ImageType >= 9;
	const int32 BaseType = bRLE ? ImageType - 8 : ImageType;
	if (Width <= 0 || Height <= 0 || (BaseType != 2 && BaseType != 3) || ColorMapType != 0)
	{
		return false;
	}
	const int32 BytesPP = (Bpp + 7) / 8;
	if (BytesPP < 1 || BytesPP > 4)
	{
		return false;
	}

	const uint8* P = H + 18 + IdLen + (ColorMapType ? CmapLen * ((CmapBits + 7) / 8) : 0);
	const uint8* End = Data.GetData() + Data.Num();

	Out.Width = Width;
	Out.Height = Height;
	Out.Pixels.SetNumUninitialized(Width * Height);

	auto ReadPixel = [&](const uint8* S) -> FColor
	{
		switch (BytesPP)
		{
		case 1: return FColor(S[0], S[0], S[0], 255);
		case 2:
		{
			const uint16 V = uint16(S[0] | (S[1] << 8));
			return FColor(uint8(((V >> 10) & 31) * 255 / 31), uint8(((V >> 5) & 31) * 255 / 31), uint8((V & 31) * 255 / 31), (V & 0x8000) ? 255 : 0);
		}
		case 3: return FColor(S[2], S[1], S[0], 255);
		default: return FColor(S[2], S[1], S[0], S[3]);
		}
	};

	const int32 Total = Width * Height;
	int32 Count = 0;
	TArray<FColor> Linear;
	Linear.SetNumUninitialized(Total);
	while (Count < Total)
	{
		if (bRLE)
		{
			if (P >= End) { return false; }
			const uint8 Header = *P++;
			const int32 Run = (Header & 0x7F) + 1;
			if (Header & 0x80)
			{
				if (P + BytesPP > End) { return false; }
				const FColor C = ReadPixel(P);
				P += BytesPP;
				for (int32 i = 0; i < Run && Count < Total; ++i) { Linear[Count++] = C; }
			}
			else
			{
				for (int32 i = 0; i < Run && Count < Total; ++i)
				{
					if (P + BytesPP > End) { return false; }
					Linear[Count++] = ReadPixel(P);
					P += BytesPP;
				}
			}
		}
		else
		{
			if (P + BytesPP > End) { return false; }
			Linear[Count++] = ReadPixel(P);
			P += BytesPP;
		}
	}

	// Linear holds rows in file order. Convert to top-first unless file order was requested.
	const bool bFlip = !bFileRowOrder && !bTopDown;
	for (int32 Row = 0; Row < Height; ++Row)
	{
		const int32 DstRow = bFlip ? (Height - 1 - Row) : Row;
		FMemory::Memcpy(&Out.Pixels[DstRow * Width], &Linear[Row * Width], Width * sizeof(FColor));
	}
	return true;
}

UTexture2D* GenTextureLoader::CreateFromDDS(TConstArrayView<uint8> Data, const FString& DebugName, bool bSRGB)
{
	if (Data.Num() < 128 || LE32(Data.GetData()) != 0x20534444) // "DDS "
	{
		return nullptr;
	}
	const uint8* D = Data.GetData();
	const int32 Height = int32(LE32(D + 12));
	const int32 Width = int32(LE32(D + 16));
	int32 MipCount = FMath::Max(1, int32(LE32(D + 28)));
	const uint32 PfFlags = LE32(D + 80);
	const uint32 FourCC = LE32(D + 84);
	const uint32 RGBBits = LE32(D + 88);
	const uint32 RMask = LE32(D + 92), GMask = LE32(D + 96), BMask = LE32(D + 100), AMask = LE32(D + 104);

	const uint8* Pixels = D + 128;
	const uint8* End = D + Data.Num();

	if (PfFlags & 0x4) // DDPF_FOURCC
	{
		EPixelFormat Format;
		int32 BlockBytes;
		switch (FourCC)
		{
		case 0x31545844: Format = PF_DXT1; BlockBytes = 8; break;  // DXT1
		case 0x33545844: Format = PF_DXT3; BlockBytes = 16; break; // DXT3
		case 0x35545844: Format = PF_DXT5; BlockBytes = 16; break; // DXT5
		default:
			UE_LOG(LogGenerals, Warning, TEXT("Unsupported DDS FourCC in %s"), *DebugName);
			return nullptr;
		}
		UTexture2D* Tex = NewTransientTexture(DebugName, Width, Height, Format, bSRGB);
		int32 W = Width, H = Height;
		for (int32 m = 0; m < MipCount; ++m)
		{
			const int64 Bytes = int64(FMath::Max(1, (W + 3) / 4)) * FMath::Max(1, (H + 3) / 4) * BlockBytes;
			if (Pixels + Bytes > End)
			{
				break;
			}
			AddMip(Tex, W, H, Pixels, Bytes);
			Pixels += Bytes;
			W = FMath::Max(1, W / 2);
			H = FMath::Max(1, H / 2);
		}
		Tex->UpdateResource();
		return Tex;
	}

	// Uncompressed: convert the top mip to BGRA8 and rebuild mips.
	if (RGBBits != 32 && RGBBits != 24 && RGBBits != 16)
	{
		return nullptr;
	}
	const int32 BytesPP = RGBBits / 8;
	if (Pixels + int64(Width) * Height * BytesPP > End)
	{
		return nullptr;
	}
	auto Extract = [](uint32 V, uint32 Mask) -> uint8
	{
		if (!Mask) { return 255; }
		const int32 Shift = FMath::CountTrailingZeros(Mask);
		const uint32 Max = Mask >> Shift;
		return uint8(((V & Mask) >> Shift) * 255 / Max);
	};
	FGenImage Img;
	Img.Width = Width;
	Img.Height = Height;
	Img.Pixels.SetNumUninitialized(Width * Height);
	for (int32 i = 0; i < Width * Height; ++i)
	{
		uint32 V = 0;
		FMemory::Memcpy(&V, Pixels + i * BytesPP, BytesPP);
		Img.Pixels[i] = FColor(Extract(V, RMask), Extract(V, GMask), Extract(V, BMask), (PfFlags & 0x1) ? Extract(V, AMask) : 255);
	}
	return CreateFromImage(Img, DebugName, bSRGB, true);
}

UTexture2D* GenTextureLoader::CreateFromImage(const FGenImage& Image, const FString& DebugName, bool bSRGB, bool bMips)
{
	if (!Image.IsValid())
	{
		return nullptr;
	}
	UTexture2D* Tex = NewTransientTexture(DebugName, Image.Width, Image.Height, PF_B8G8R8A8, bSRGB);
	AddMip(Tex, Image.Width, Image.Height, Image.Pixels.GetData(), int64(Image.Pixels.Num()) * sizeof(FColor));
	if (bMips)
	{
		FGenImage Cur = Image;
		while (Cur.Width > 1 || Cur.Height > 1)
		{
			FGenImage Next;
			Downsample(Cur, Next);
			AddMip(Tex, Next.Width, Next.Height, Next.Pixels.GetData(), int64(Next.Pixels.Num()) * sizeof(FColor));
			Cur = MoveTemp(Next);
		}
	}
	Tex->UpdateResource();
	return Tex;
}

UTexture2D* GenTextureLoader::LoadGameTexture(const FString& TextureName, bool bSRGB)
{
	FGenFileSystem& FS = FGenFileSystem::Get();
	const FString Base = FPaths::GetBaseFilename(TextureName);
	const FString Dirs[] = { TEXT("Art\\Textures\\"), TEXT("Data\\English\\Art\\Textures\\"), TEXT("Art\\Terrain\\") };

	TArray<uint8> Bytes;
	for (const FString& Dir : Dirs)
	{
		if (FS.ReadFile(Dir + Base + TEXT(".dds"), Bytes))
		{
			if (UTexture2D* Tex = CreateFromDDS(Bytes, Base, bSRGB))
			{
				return Tex;
			}
		}
		if (FS.ReadFile(Dir + Base + TEXT(".tga"), Bytes))
		{
			FGenImage Img;
			if (DecodeTGA(Bytes, Img))
			{
				return CreateFromImage(Img, Base, bSRGB, true);
			}
		}
	}
	return nullptr;
}
