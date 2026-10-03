#pragma once

#include "CoreMinimal.h"

class UTexture2D;

/** Decoded image in BGRA8, top row first. */
struct FGenImage
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<FColor> Pixels;

	bool IsValid() const { return Width > 0 && Height > 0 && Pixels.Num() == Width * Height; }
	FColor& At(int32 X, int32 Y) { return Pixels[Y * Width + X]; }
	const FColor& At(int32 X, int32 Y) const { return Pixels[Y * Width + X]; }
};

namespace GenTextureLoader
{
	/** Decodes a TGA (8/16/24/32 bit, RLE or raw). If bFileRowOrder, rows are returned in the order stored in the file. */
	GENERALS1_API bool DecodeTGA(TConstArrayView<uint8> Data, FGenImage& Out, bool bFileRowOrder = false);

	/** Creates a transient texture from a DDS (DXT1/3/5 kept compressed; uncompressed formats converted). */
	GENERALS1_API UTexture2D* CreateFromDDS(TConstArrayView<uint8> Data, const FString& DebugName, bool bSRGB = true);

	/** Creates a transient BGRA8 texture with a full box-filtered mip chain. */
	GENERALS1_API UTexture2D* CreateFromImage(const FGenImage& Image, const FString& DebugName, bool bSRGB = true, bool bMips = true);

	/**
	 * Resolves a W3D/INI texture reference the way the original asset manager does:
	 * "foo.tga" is looked up as Art\Textures\foo.dds first, then Art\Textures\foo.tga.
	 */
	GENERALS1_API UTexture2D* LoadGameTexture(const FString& TextureName, bool bSRGB = true);
}
