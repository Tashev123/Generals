#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

class FGenObject;

/**
 * Objects by where they are (PartitionManager): the map in square cells, every object entered in each
 * cell its bounding circle touches. "What is near here" then looks at the cells around a spot instead
 * of at every object in the game - the difference between a unit count the game can take and one it
 * can't, since most of what units do is ask that question.
 *
 * Rebuilt once per logic step, after the dead have been removed. Objects made later in the step are
 * found from the next step on; objects that move during the step are still found, because every
 * question is asked a little wider than its range (Slack).
 */
class GENERALS1_API FGenPartition
{
public:
	void Init(const FVector2f& InMin, const FVector2f& InMax, float InCellSize = 100.f);
	void Rebuild(const TArray<FGenObject*>& Objects);
	/** The object is gone before the step is over. */
	void Remove(FGenObject* Object);
	bool IsValid() const { return W > 0; }

	/**
	 * Every object whose bounding circle may come within Range of Center - each once, nearest cells in
	 * no particular order. The caller checks the exact distance (positions may have changed since the
	 * rebuild). Fn(FGenObject&).
	 */
	template <typename F>
	void ForEachInRange(const FVector2f& Center, float Range, F&& Fn) const
	{
		if (W <= 0)
		{
			return;
		}
		TArray<FGenObject*, TInlineAllocator<128>> Found;
		Collect(Center, Range + Slack, Found);
		for (FGenObject* O : Found)
		{
			Fn(*O);
		}
	}

	/**
	 * Outward from Center, ring of cells by ring: Fn(FGenObject&) for every object, each once; after a
	 * ring Done(Radius) says whether to stop - everything nearer than Radius has been seen by then.
	 * MaxRange < 0: as far as the map goes.
	 */
	template <typename F, typename D>
	void ForEachByRings(const FVector2f& Center, float MaxRange, F&& Fn, D&& Done) const
	{
		if (W <= 0)
		{
			return;
		}
		const int32 CX = CellX(Center.X), CY = CellY(Center.Y);
		const int32 MaxRing = MaxRange < 0.f ? FMath::Max(W, H) : FMath::Min(FMath::Max(W, H), FMath::CeilToInt32((MaxRange + Slack) / CellSize) + 1);
		const uint32 Stamp = NextStamp();
		TArray<FGenObject*, TInlineAllocator<128>> Found;
		for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
		{
			Found.Reset();
			for (int32 Y = CY - Ring; Y <= CY + Ring; ++Y)
			{
				if (Y < 0 || Y >= H)
				{
					continue;
				}
				const int32 Step = (Y == CY - Ring || Y == CY + Ring) ? 1 : FMath::Max(1, 2 * Ring);
				for (int32 X = CX - Ring; X <= CX + Ring; X += Step)
				{
					if (X >= 0 && X < W)
					{
						CollectCell(Y * W + X, Stamp, Found);
					}
				}
			}
			for (FGenObject* O : Found)
			{
				Fn(*O);
			}
			// What hasn't been seen yet lies beyond this ring: at least Ring cells away, less the slack.
			if (Done(FMath::Max(0.f, float(Ring) * CellSize - Slack)))
			{
				return;
			}
		}
	}

private:
	void Collect(const FVector2f& Center, float Range, TArray<FGenObject*, TInlineAllocator<128>>& Out) const;
	void CollectCell(int32 Cell, uint32 Stamp, TArray<FGenObject*, TInlineAllocator<128>>& Out) const;
	uint32 NextStamp() const;
	int32 CellX(float X) const { return FMath::Clamp(FMath::FloorToInt32((X - Min.X) / CellSize), 0, W - 1); }
	int32 CellY(float Y) const { return FMath::Clamp(FMath::FloorToInt32((Y - Min.Y) / CellSize), 0, H - 1); }

	/** How far something can have moved since the rebuild (the fastest aircraft, one step) and then some. */
	static constexpr float Slack = 24.f;

	FVector2f Min = FVector2f::ZeroVector;
	float CellSize = 100.f;
	int32 W = 0, H = 0;
	TArray<int32> CellStart;             // per cell: where its entries begin (W * H + 1 of them)
	TArray<FGenObject*> Entries;
	TArray<FGenObject*> Indexed;         // the objects, by their slot
	mutable TArray<uint32> SeenStamp;    // per slot: the question that last saw it
	mutable uint32 QueryStamp = 0;
	TArray<int32> Scratch;
};
