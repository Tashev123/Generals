#pragma once

#include "CoreMinimal.h"

/** CompressionManager equivalents for data stored in the game archives (maps). */
namespace GenCompression
{
	/** True if Data starts with a known compression tag ("EAR\0" RefPack, "ZLn\0" zlib). */
	GENERALS1_API bool IsCompressed(TConstArrayView<uint8> Data);

	/** Decompresses tagged data; returns the input unchanged if it is not compressed. */
	GENERALS1_API bool Decompress(TConstArrayView<uint8> Data, TArray<uint8>& Out);

	/** EA RefPack decoder (Libraries/Source/Compression/EAC/refdecode.cpp). Src excludes the 8-byte tag. */
	GENERALS1_API bool RefPackDecode(TConstArrayView<uint8> Src, TArray<uint8>& Out);
}
