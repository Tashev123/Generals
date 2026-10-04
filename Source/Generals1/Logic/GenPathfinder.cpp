#include "Logic/GenPathfinder.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Map/GenMapData.h"
#include "Generals1.h"
#include "Algo/Reverse.h"

namespace
{
	constexpr float CellSize = GenLogic::PathfindCellSize;
}

void FGenPathfinder::Build(const FGenMapData& Map)
{
	Width = Map.Width - 1;
	Height = Map.Height - 1;
	Border = Map.Border;
	Terrain.Init(ECell::Clear, Width * Height);
	Obstacles.Init(0, Width * Height);
	GoalUnit.Init(GenInvalidObjectID, Width * Height);
	FixedUnit.Init(GenInvalidObjectID, Width * Height);
	GoalRects.Reset();
	FixedRects.Reset();

	const FBox2f Playable = Map.GetPlayableBounds();
	int32 Counts[6] = {};
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const FVector2f Lo((X - Border) * CellSize, (Y - Border) * CellSize);
			const FVector2f Hi = Lo + FVector2f(CellSize, CellSize);
			ECell Type = ECell::Clear;
			if (Map.IsCliffCellIndex(X, Y))
			{
				Type = ECell::Cliff;
			}
			// classifyMapCell: any underwater corner makes it a water cell.
			if (Map.IsUnderwater(Lo.X, Lo.Y) || Map.IsUnderwater(Lo.X, Hi.Y) || Map.IsUnderwater(Hi.X, Hi.Y) || Map.IsUnderwater(Hi.X, Lo.Y))
			{
				Type = ECell::Water;
			}
			// Units may not leave the playable area.
			if (Lo.X < Playable.Min.X || Lo.Y < Playable.Min.Y || Hi.X > Playable.Max.X || Hi.Y > Playable.Max.Y)
			{
				Type = ECell::Impassable;
			}
			Terrain[Y * Width + X] = Type;
			Counts[int32(Type)]++;
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Pathfinder: %dx%d cells (clear %d, water %d, cliff %d, outside %d)"),
		Width, Height, Counts[0], Counts[1], Counts[2], Counts[5]);
}

FIntPoint FGenPathfinder::WorldToCell(const FVector2f& P) const
{
	return FIntPoint(FMath::FloorToInt32(P.X / CellSize) + Border, FMath::FloorToInt32(P.Y / CellSize) + Border);
}

FVector2f FGenPathfinder::CellCenter(int32 X, int32 Y) const
{
	return FVector2f((X - Border + 0.5f) * CellSize, (Y - Border + 0.5f) * CellSize);
}

FGenPathfinder::ECell FGenPathfinder::GetCell(int32 X, int32 Y) const
{
	if (X < 0 || Y < 0 || X >= Width || Y >= Height)
	{
		return ECell::Impassable;
	}
	const int32 I = Y * Width + X;
	return Obstacles[I] > 0 ? ECell::Obstacle : Terrain[I];
}

bool FGenPathfinder::CanPass(int32 X, int32 Y, uint32 Surfaces) const
{
	const ECell C = GetCell(X, Y);
	if (C == ECell::Impassable)
	{
		return false;
	}
	if (AvoidRadius >= 0)
	{
		// Pathing around units: cells held by another stationary unit are closed.
		for (int32 DY = -AvoidRadius; DY <= AvoidRadius; ++DY)
		{
			for (int32 DX = -AvoidRadius; DX <= AvoidRadius; ++DX)
			{
				const int32 UX = X + DX, UY = Y + DY;
				if (UX < 0 || UY < 0 || UX >= Width || UY >= Height) { continue; }
				const FGenObjectID U = FixedUnit[UY * Width + UX];
				if (U != GenInvalidObjectID && U != AvoidSelf) { return false; }
			}
		}
	}
	if (Surfaces & GenSurface::Air)
	{
		return true;
	}
	switch (C)
	{
	case ECell::Clear: return (Surfaces & GenSurface::Ground) != 0;
	case ECell::Water: return (Surfaces & GenSurface::Water) != 0;
	case ECell::Cliff: return (Surfaces & GenSurface::Cliff) != 0;
	case ECell::Rubble: return (Surfaces & GenSurface::Rubble) != 0;
	default: return false;
	}
}

bool FGenPathfinder::IsPassable(const FVector2f& WorldPos, uint32 Surfaces) const
{
	const FIntPoint C = WorldToCell(WorldPos);
	return CanPass(C.X, C.Y, Surfaces);
}

void FGenPathfinder::RasterizeFootprint(const FGenObject& Obj, int32 Delta)
{
	const FGenThingTemplate* T = Obj.GetTemplate();
	if (!T || Width == 0)
	{
		return;
	}
	const FVector2f C(Obj.GetPosition().X, Obj.GetPosition().Y);
	const float Angle = Obj.GetOrientation();
	const float CosA = FMath::Cos(Angle), SinA = FMath::Sin(Angle);
	const float Major = T->GeometryMajorRadius;
	const float Minor = T->IsBox() ? T->GeometryMinorRadius : T->GeometryMajorRadius;
	const float R = T->GetBoundingCircleRadius();
	const FIntPoint Lo = WorldToCell(C - FVector2f(R, R));
	const FIntPoint Hi = WorldToCell(C + FVector2f(R, R));
	for (int32 Y = FMath::Max(0, Lo.Y); Y <= FMath::Min(Height - 1, Hi.Y); ++Y)
	{
		for (int32 X = FMath::Max(0, Lo.X); X <= FMath::Min(Width - 1, Hi.X); ++X)
		{
			const FVector2f D = CellCenter(X, Y) - C;
			bool bInside;
			if (T->IsBox())
			{
				// Rotate into the object's frame (x forward = major axis).
				const float LX = D.X * CosA + D.Y * SinA;
				const float LY = -D.X * SinA + D.Y * CosA;
				bInside = FMath::Abs(LX) <= Major && FMath::Abs(LY) <= Minor;
			}
			else
			{
				bInside = D.SizeSquared() <= Major * Major;
			}
			if (bInside)
			{
				uint16& Count = Obstacles[Y * Width + X];
				Count = uint16(FMath::Max(0, int32(Count) + Delta));
			}
		}
	}
}

bool FGenPathfinder::IsAreaClear(const FVector2f& Center, float Angle, float Major, float Minor, bool bBox) const
{
	const float CosA = FMath::Cos(Angle), SinA = FMath::Sin(Angle);
	const float R = bBox ? FMath::Sqrt(Major * Major + Minor * Minor) : Major;
	const FIntPoint Lo = WorldToCell(Center - FVector2f(R, R));
	const FIntPoint Hi = WorldToCell(Center + FVector2f(R, R));
	for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
	{
		for (int32 X = Lo.X; X <= Hi.X; ++X)
		{
			const FVector2f D = CellCenter(X, Y) - Center;
			const bool bInside = bBox
				? FMath::Abs(D.X * CosA + D.Y * SinA) <= Major && FMath::Abs(-D.X * SinA + D.Y * CosA) <= Minor
				: D.SizeSquared() <= Major * Major;
			if (bInside && GetCell(X, Y) != ECell::Clear)
			{
				return false;
			}
		}
	}
	return true;
}

void FGenPathfinder::AddObstacle(const FGenObject& Obj)
{
	RasterizeFootprint(Obj, +1);
}

void FGenPathfinder::RemoveObstacle(const FGenObject& Obj)
{
	RasterizeFootprint(Obj, -1);
}

bool FGenPathfinder::IsLineClear(const FVector2f& A, const FVector2f& B, uint32 Surfaces) const
{
	// Sample along the segment at quarter-cell steps (conservative supercover approximation).
	const FVector2f D = B - A;
	const float Len = D.Size();
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Len / (CellSize * 0.25f)));
	FIntPoint Last(INT32_MIN, INT32_MIN);
	for (int32 i = 0; i <= Steps; ++i)
	{
		const FIntPoint C = WorldToCell(A + D * (float(i) / float(Steps)));
		if (C == Last)
		{
			continue;
		}
		if (!CanPass(C.X, C.Y, Surfaces))
		{
			return false;
		}
		// Moving diagonally between cells must not squeeze between two blocked cells.
		if (Last.X != INT32_MIN && Last.X != C.X && Last.Y != C.Y)
		{
			if (!CanPass(Last.X, C.Y, Surfaces) && !CanPass(C.X, Last.Y, Surfaces))
			{
				return false;
			}
		}
		Last = C;
	}
	return true;
}

bool FGenPathfinder::AdjustDestination(const FVector2f& Pos, uint32 Surfaces, FVector2f& OutPos, int32 MaxRadiusCells) const
{
	const FIntPoint C = WorldToCell(Pos);
	if (CanPass(C.X, C.Y, Surfaces))
	{
		OutPos = Pos;
		return true;
	}
	for (int32 R = 1; R <= MaxRadiusCells; ++R)
	{
		float BestD = FLT_MAX;
		FIntPoint Best(-1, -1);
		for (int32 DY = -R; DY <= R; ++DY)
		{
			for (int32 DX = -R; DX <= R; ++DX)
			{
				if (FMath::Abs(DX) != R && FMath::Abs(DY) != R) { continue; }
				const int32 X = C.X + DX, Y = C.Y + DY;
				if (!CanPass(X, Y, Surfaces)) { continue; }
				const float D = FVector2f::DistSquared(CellCenter(X, Y), Pos);
				if (D < BestD) { BestD = D; Best = FIntPoint(X, Y); }
			}
		}
		if (Best.X >= 0)
		{
			OutPos = CellCenter(Best.X, Best.Y);
			return true;
		}
	}
	return false;
}

// ---- units --------------------------------------------------------------------------------------

void FGenPathfinder::GetRadiusAndCenter(float BoundingCircleRadius, int32& OutRadius, bool& bOutCenter)
{
	float Diameter = 2.f * BoundingCircleRadius;
	if (Diameter > CellSize && Diameter < 2.f * CellSize)
	{
		Diameter = 2.f * CellSize;
	}
	int32 R = FMath::FloorToInt32(Diameter / CellSize + 0.3f);
	bOutCenter = false;
	if (R == 0) { R = 1; }
	if (R & 1) { bOutCenter = true; }
	R /= 2;
	if (R > 2) // MAX_RADIUS
	{
		R = 2;
		bOutCenter = true;
	}
	OutRadius = R;
}

FIntRect FGenPathfinder::UnitRect(const FVector2f& Pos, float Radius) const
{
	int32 R;
	bool bCenter;
	GetRadiusAndCenter(Radius, R, bCenter);
	// Odd-sized footprints are centered on a cell, even-sized ones on a cell corner.
	const float Shift = bCenter ? 0.f : 0.5f;
	const FIntPoint C(FMath::FloorToInt32(Shift + Pos.X / CellSize) + Border, FMath::FloorToInt32(Shift + Pos.Y / CellSize) + Border);
	return FIntRect(C.X - R, C.Y - R, C.X + R + (bCenter ? 1 : 0), C.Y + R + (bCenter ? 1 : 0));
}

void FGenPathfinder::Stamp(TArray<FGenObjectID>& Layer, const FIntRect& Rect, FGenObjectID From, FGenObjectID To)
{
	// Only cells currently holding From change: claiming (invalid -> id) never takes a cell someone else
	// holds, releasing (id -> invalid) never frees someone else's.
	for (int32 Y = FMath::Max(0, Rect.Min.Y); Y < FMath::Min(Height, Rect.Max.Y); ++Y)
	{
		for (int32 X = FMath::Max(0, Rect.Min.X); X < FMath::Min(Width, Rect.Max.X); ++X)
		{
			FGenObjectID& Cell = Layer[Y * Width + X];
			if (Cell == From) { Cell = To; }
		}
	}
}

void FGenPathfinder::UpdateUnitPos(FGenObjectID Id, const FVector2f& Pos, float Radius, bool bMoving)
{
	if (Width == 0)
	{
		return;
	}
	const FIntRect* Old = FixedRects.Find(Id);
	if (bMoving)
	{
		if (Old)
		{
			Stamp(FixedUnit, *Old, Id, GenInvalidObjectID);
			FixedRects.Remove(Id);
		}
		return;
	}
	// Re-claimed every frame: cells another parked unit held when we arrived become ours once it leaves.
	const FIntRect Rect = UnitRect(Pos, Radius);
	if (Old && *Old != Rect) { Stamp(FixedUnit, *Old, Id, GenInvalidObjectID); }
	Stamp(FixedUnit, Rect, GenInvalidObjectID, Id);
	FixedRects.Add(Id, Rect);
}

void FGenPathfinder::UpdateUnitGoal(FGenObjectID Id, const FVector2f& GoalPos, float Radius)
{
	if (Width == 0)
	{
		return;
	}
	const FIntRect Rect = UnitRect(GoalPos, Radius);
	if (const FIntRect* Old = GoalRects.Find(Id))
	{
		if (*Old == Rect) { return; }
		Stamp(GoalUnit, *Old, Id, GenInvalidObjectID);
	}
	Stamp(GoalUnit, Rect, GenInvalidObjectID, Id);
	GoalRects.Add(Id, Rect);
}

void FGenPathfinder::RemoveUnit(FGenObjectID Id)
{
	if (const FIntRect* Old = FixedRects.Find(Id)) { Stamp(FixedUnit, *Old, Id, GenInvalidObjectID); }
	if (const FIntRect* Old = GoalRects.Find(Id)) { Stamp(GoalUnit, *Old, Id, GenInvalidObjectID); }
	FixedRects.Remove(Id);
	GoalRects.Remove(Id);
}

bool FGenPathfinder::IsDestinationFree(FGenObjectID Id, const FVector2f& Pos, float Radius, uint32 Surfaces) const
{
	// Pathfinder::checkDestination
	const FIntRect Rect = UnitRect(Pos, Radius);
	for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
	{
		for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
		{
			if (X < 0 || Y < 0 || X >= Width || Y >= Height || !CanPass(X, Y, Surfaces))
			{
				return false;
			}
			const FGenObjectID Goal = GoalUnit[Y * Width + X];
			const FGenObjectID Fixed = FixedUnit[Y * Width + X];
			if ((Goal != GenInvalidObjectID && Goal != Id) || (Fixed != GenInvalidObjectID && Fixed != Id))
			{
				return false; // someone else is going there, or parked there
			}
		}
	}
	return true;
}

bool FGenPathfinder::AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos) const
{
	return AdjustUnitDestination(Id, Radius, Surfaces, InOutPos, [](const FVector2f&) { return true; });
}

bool FGenPathfinder::AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos, TFunctionRef<bool(const FVector2f&)> Accept) const
{
	// Pathfinder::adjustDestination / adjustTargetDestination
	if (Width == 0 || (Accept(InOutPos) && IsDestinationFree(Id, InOutPos, Radius, Surfaces)))
	{
		return Width != 0;
	}
	int32 R;
	bool bCenter;
	GetRadiusAndCenter(Radius, R, bCenter);
	const float Shift = bCenter ? 0.f : 0.5f;
	int32 I = FMath::FloorToInt32(Shift + InOutPos.X / CellSize);
	int32 J = FMath::FloorToInt32(Shift + InOutPos.Y / CellSize);
	auto Try = [&](int32 CX, int32 CY) -> bool
	{
		const float Offset = bCenter ? 0.5f : 0.f;
		const FVector2f P((float(CX) + Offset) * CellSize, (float(CY) + Offset) * CellSize);
		if (Accept(P) && IsDestinationFree(Id, P, Radius, Surfaces))
		{
			InOutPos = P;
			return true;
		}
		return false;
	};
	if (Try(I, J))
	{
		return true;
	}
	// Outward square spiral: right, up, then left, down with a longer leg each time.
	int32 Limit = 400; // MAX_CELLS_TO_TRY
	int32 Delta = 1;
	while (Limit > 0)
	{
		for (int32 n = Delta; n > 0; --n) { ++I; --Limit; if (Try(I, J)) { return true; } }
		for (int32 n = Delta; n > 0; --n) { ++J; --Limit; if (Try(I, J)) { return true; } }
		++Delta;
		for (int32 n = Delta; n > 0; --n) { --I; --Limit; if (Try(I, J)) { return true; } }
		for (int32 n = Delta; n > 0; --n) { --J; --Limit; if (Try(I, J)) { return true; } }
		++Delta;
	}
	return false;
}

bool FGenPathfinder::FindPathAroundUnits(FGenObjectID Id, float Radius, const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces,
	TArray<FVector2f>& OutPath) const
{
	AvoidSelf = Id;
	AvoidRadius = 0;
	const bool bFound = FindPath(Start, Goal, Surfaces, OutPath);
	AvoidSelf = GenInvalidObjectID;
	AvoidRadius = -1;
	return bFound;
}

bool FGenPathfinder::FindPath(const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces, TArray<FVector2f>& OutPath) const
{
	OutPath.Reset();
	if (Width == 0)
	{
		return false;
	}
	FIntPoint S = WorldToCell(Start);
	FIntPoint G = WorldToCell(Goal);
	S.X = FMath::Clamp(S.X, 0, Width - 1);
	S.Y = FMath::Clamp(S.Y, 0, Height - 1);

	// Units standing on an invalid cell (e.g. spawned inside a footprint) path from the nearest valid one.
	// (Not when pathing around units: the start may touch a parked unit, and must not jump past it.)
	if (AvoidRadius < 0 && !CanPass(S.X, S.Y, Surfaces))
	{
		FVector2f Fixed;
		if (!AdjustDestination(Start, Surfaces, Fixed, 8))
		{
			return false;
		}
		S = WorldToCell(Fixed);
	}
	FVector2f GoalPos = Goal;
	if (!CanPass(G.X, G.Y, Surfaces))
	{
		if (AdjustDestination(Goal, Surfaces, GoalPos))
		{
			G = WorldToCell(GoalPos);
		}
	}

	if (S == G)
	{
		OutPath.Add(GoalPos);
		return true;
	}

	// A* (8-connected, octile heuristic, no corner cutting).
	const int32 N = Width * Height;
	TArray<int32> GCost;
	GCost.Init(INT32_MAX, N);
	TArray<int32> Parent;
	Parent.Init(-1, N);
	TBitArray<> Closed(false, N);

	struct FNode { int32 F; int32 Index; };
	TArray<FNode> Open;
	auto Less = [](const FNode& A, const FNode& B) { return A.F < B.F; };
	auto Heuristic = [&](int32 X, int32 Y)
	{
		const int32 DX = FMath::Abs(X - G.X), DY = FMath::Abs(Y - G.Y);
		return 10 * FMath::Max(DX, DY) + 4 * FMath::Min(DX, DY);
	};

	const int32 StartIdx = S.Y * Width + S.X;
	const int32 GoalIdx = G.Y * Width + G.X;
	GCost[StartIdx] = 0;
	Open.HeapPush({ Heuristic(S.X, S.Y), StartIdx }, Less);

	int32 BestIdx = StartIdx;
	int32 BestH = Heuristic(S.X, S.Y);
	const int32 DX8[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	const int32 DY8[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	int32 Expanded = 0;

	while (Open.Num() > 0)
	{
		FNode Cur;
		Open.HeapPop(Cur, Less, EAllowShrinking::No);
		if (Closed[Cur.Index]) { continue; }
		Closed[Cur.Index] = true;
		++Expanded;
		if (Cur.Index == GoalIdx)
		{
			BestIdx = GoalIdx;
			break;
		}
		const int32 CX = Cur.Index % Width, CY = Cur.Index / Width;
		const int32 H = Heuristic(CX, CY);
		if (H < BestH) { BestH = H; BestIdx = Cur.Index; }

		for (int32 k = 0; k < 8; ++k)
		{
			const int32 NX = CX + DX8[k], NY = CY + DY8[k];
			if (!CanPass(NX, NY, Surfaces)) { continue; }
			const bool bDiag = k >= 4;
			if (bDiag && (!CanPass(CX + DX8[k], CY, Surfaces) || !CanPass(CX, CY + DY8[k], Surfaces))) { continue; }
			const int32 NI = NY * Width + NX;
			if (Closed[NI]) { continue; }
			const int32 NewG = GCost[Cur.Index] + (bDiag ? 14 : 10);
			if (NewG < GCost[NI])
			{
				GCost[NI] = NewG;
				Parent[NI] = Cur.Index;
				Open.HeapPush({ NewG + Heuristic(NX, NY), NI }, Less);
			}
		}
	}

	// Reconstruct (to the goal, or to the closest reachable cell).
	TArray<FVector2f> Cells;
	for (int32 I = BestIdx; I != -1 && I != StartIdx; I = Parent[I])
	{
		Cells.Add(CellCenter(I % Width, I / Width));
	}
	Algo::Reverse(Cells);
	if (BestIdx == GoalIdx)
	{
		if (Cells.Num() > 0) { Cells.Last() = GoalPos; } else { Cells.Add(GoalPos); }
	}
	if (Cells.Num() == 0)
	{
		return true; // already at the closest reachable point
	}

	// Smooth: from each anchor, jump to the farthest waypoint in line of sight.
	FVector2f Anchor = Start;
	int32 i = 0;
	while (i < Cells.Num())
	{
		int32 Far = i;
		for (int32 j = Cells.Num() - 1; j > i; --j)
		{
			if (IsLineClear(Anchor, Cells[j], Surfaces)) { Far = j; break; }
		}
		OutPath.Add(Cells[Far]);
		Anchor = Cells[Far];
		i = Far + 1;
	}
	return true;
}
