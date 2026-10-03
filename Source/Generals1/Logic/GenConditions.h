#pragma once

#include "CoreMinimal.h"

/**
 * ModelConditionFlags: named condition bits (MOVING, REALLYDAMAGED, NIGHT, ...).
 * Names are assigned indices on first use, so INI-defined names work without a hard-coded table.
 */
struct GENERALS1_API FGenConditionBits
{
	static constexpr int32 MaxBits = 256;
	uint64 Words[MaxBits / 64] = {};

	void Set(int32 Bit, bool bOn = true)
	{
		if (Bit < 0 || Bit >= MaxBits) { return; }
		const uint64 Mask = uint64(1) << (Bit & 63);
		if (bOn) { Words[Bit >> 6] |= Mask; } else { Words[Bit >> 6] &= ~Mask; }
	}
	bool Test(int32 Bit) const { return Bit >= 0 && Bit < MaxBits && (Words[Bit >> 6] & (uint64(1) << (Bit & 63))) != 0; }
	bool Any() const { for (uint64 W : Words) { if (W) { return true; } } return false; }

	/** popcount(this & Other) */
	int32 CountIntersection(const FGenConditionBits& Other) const
	{
		int32 N = 0;
		for (int32 i = 0; i < UE_ARRAY_COUNT(Words); ++i) { N += FMath::CountBits64(Words[i] & Other.Words[i]); }
		return N;
	}
	/** popcount(Other & ~this): bits Other requires that this doesn't have. */
	int32 CountMissing(const FGenConditionBits& Other) const
	{
		int32 N = 0;
		for (int32 i = 0; i < UE_ARRAY_COUNT(Words); ++i) { N += FMath::CountBits64(Other.Words[i] & ~Words[i]); }
		return N;
	}
	FGenConditionBits Without(const FGenConditionBits& Mask) const
	{
		FGenConditionBits R = *this;
		for (int32 i = 0; i < UE_ARRAY_COUNT(Words); ++i) { R.Words[i] &= ~Mask.Words[i]; }
		return R;
	}
	bool operator==(const FGenConditionBits& O) const { return FMemory::Memcmp(Words, O.Words, sizeof(Words)) == 0; }
	bool operator!=(const FGenConditionBits& O) const { return !(*this == O); }
};

namespace GenConditions
{
	/** Index for a condition name (case-insensitive), allocated on first use. */
	GENERALS1_API int32 Index(const FString& Name);
	GENERALS1_API FGenConditionBits Parse(const TArray<FString>& Names);

	// Frequently used conditions (resolved once).
	GENERALS1_API int32 Moving();
	GENERALS1_API int32 Damaged();
	GENERALS1_API int32 ReallyDamaged();
	GENERALS1_API int32 Rubble();
	GENERALS1_API int32 Night();
	GENERALS1_API int32 Snow();
}
