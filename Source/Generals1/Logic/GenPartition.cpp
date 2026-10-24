#include "Logic/GenPartition.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"

void FGenPartition::Init(const FVector2f& InMin, const FVector2f& InMax, float InCellSize)
{
	Min = InMin;
	CellSize = FMath::Max(10.f, InCellSize);
	W = FMath::Max(1, FMath::CeilToInt32((InMax.X - InMin.X) / CellSize));
	H = FMath::Max(1, FMath::CeilToInt32((InMax.Y - InMin.Y) / CellSize));
	CellStart.Init(0, W * H + 1);
	Entries.Reset();
	Indexed.Reset();
	SeenStamp.Reset();
	QueryStamp = 0;
}

void FGenPartition::Rebuild(const TArray<FGenObject*>& Objects)
{
	GEN_PERF_SCOPE("Partition");
	if (W <= 0)
	{
		return;
	}
	// Two passes (a counting sort): how many entries each cell gets, then the entries themselves.
	Indexed.Reset(Objects.Num());
	for (int32& N : CellStart)
	{
		N = 0;
	}
	struct FSpan { int32 X0, Y0, X1, Y1; };
	TArray<FSpan, TInlineAllocator<1024>> Spans;
	Spans.Reserve(Objects.Num());
	for (FGenObject* O : Objects)
	{
		if (!O || O->IsDestroyed())
		{
			continue;
		}
		const float R = O->GetTemplate()->GetBoundingCircleRadius();
		const FVector3f& P = O->GetPosition();
		const FSpan S{ CellX(P.X - R), CellY(P.Y - R), CellX(P.X + R), CellY(P.Y + R) };
		Indexed.Add(O);
		Spans.Add(S);
		for (int32 Y = S.Y0; Y <= S.Y1; ++Y)
		{
			for (int32 X = S.X0; X <= S.X1; ++X)
			{
				++CellStart[Y * W + X + 1];
			}
		}
	}
	for (int32 i = 1; i < CellStart.Num(); ++i)
	{
		CellStart[i] += CellStart[i - 1];
	}
	Entries.SetNumUninitialized(CellStart.Last());
	Scratch.SetNumUninitialized(W * H);
	FMemory::Memcpy(Scratch.GetData(), CellStart.GetData(), W * H * sizeof(int32));
	for (int32 i = 0; i < Indexed.Num(); ++i)
	{
		Indexed[i]->PartitionSlot = i;
		const FSpan& S = Spans[i];
		for (int32 Y = S.Y0; Y <= S.Y1; ++Y)
		{
			for (int32 X = S.X0; X <= S.X1; ++X)
			{
				Entries[Scratch[Y * W + X]++] = Indexed[i];
			}
		}
	}
	if (SeenStamp.Num() < Indexed.Num())
	{
		SeenStamp.SetNumZeroed(Indexed.Num() + 256);
	}
}

void FGenPartition::Remove(FGenObject* Object)
{
	if (!Object || !Indexed.IsValidIndex(Object->PartitionSlot) || Indexed[Object->PartitionSlot] != Object)
	{
		return;
	}
	for (FGenObject*& E : Entries)
	{
		if (E == Object)
		{
			E = nullptr;
		}
	}
	Indexed[Object->PartitionSlot] = nullptr;
	Object->PartitionSlot = INDEX_NONE;
}

uint32 FGenPartition::NextStamp() const
{
	if (++QueryStamp == 0)
	{
		FMemory::Memzero(SeenStamp.GetData(), SeenStamp.Num() * sizeof(uint32));
		QueryStamp = 1;
	}
	return QueryStamp;
}

void FGenPartition::CollectCell(int32 Cell, uint32 Stamp, TArray<FGenObject*, TInlineAllocator<128>>& Out) const
{
	for (int32 e = CellStart[Cell], End = CellStart[Cell + 1]; e < End; ++e)
	{
		FGenObject* O = Entries[e];
		if (!O)
		{
			continue;
		}
		const int32 Slot = O->PartitionSlot;
		if (SeenStamp[Slot] != Stamp)
		{
			SeenStamp[Slot] = Stamp;
			Out.Add(O);
		}
	}
}

void FGenPartition::Collect(const FVector2f& Center, float Range, TArray<FGenObject*, TInlineAllocator<128>>& Out) const
{
	const uint32 Stamp = NextStamp();
	const int32 X0 = CellX(Center.X - Range), X1 = CellX(Center.X + Range);
	const int32 Y0 = CellY(Center.Y - Range), Y1 = CellY(Center.Y + Range);
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			CollectCell(Y * W + X, Stamp, Out);
		}
	}
}
