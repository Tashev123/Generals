#include "Logic/GenPathfinder.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Map/GenMapData.h"
#include "Generals1.h"
#include "Algo/Reverse.h"

static TAutoConsoleVariable<int32> CVarGenPathCliffMargin(
	TEXT("gen.PathCliffMargin"), 1,
	TEXT("1: ground units keep a cell away from cliffs and gaps a cell wide beside structures are shut, as in the original (read when a map loads). 0: off."));

static TAutoConsoleVariable<int32> CVarGenPathWeight(
	TEXT("gen.PathWeight"), 13,
	TEXT("How much more the distance still to go counts than the distance covered in a path search, in tenths (10 = shortest paths, slowest search)."));

namespace
{
	constexpr float CellSize = GenLogic::PathfindCellSize;
	/** LAYER_Z_CLOSE_ENOUGH_F: how near in height a bridge's deck and something else count as touching. */
	constexpr float LayerZCloseEnough = 10.f;
}

void FGenPathfinder::Build(const FGenMapData& Map, const TArray<FGenBridgeInfo>& InBridges)
{
	Width = Map.Width - 1;
	Height = Map.Height - 1;
	Border = Map.Border;
	Terrain.Init(ECell::Clear, Width * Height);
	Obstacles.Init(0, Width * Height);
	Fences.Init(0, Width * Height);
	ConnectLayer.Init(0, Width * Height);
	Layers.Reset();
	NumNodes = Width * Height;
	GoalRects.Reset();
	FixedRects.Reset();
	Regions.Reset();
	SeenStamp.Reset();
	ClosedStamp.Reset();
	++ObstacleVersion;

	const FBox2f Playable = Map.GetPlayableBounds();
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
			Terrain[Y * Width + X] = Type;
		}
	}

	// Pathfinder::classifyMap: "Expand all cliff cells one step", "Convert pinched to cliff", "Add a border
	// of pinched cells to cliffs." Ground units keep a cell away from a cliff's foot and from its edge, and
	// the cell after that is one they would rather not use.
	CellFlags.Init(0, Width * Height);
	if (CVarGenPathCliffMargin.GetValueOnAnyThread() != 0)
	{
		TArray<int32> Marked;
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			Marked.Reset();
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					if (Terrain[Y * Width + X] != ECell::Cliff)
					{
						continue;
					}
					for (int32 NY = FMath::Max(0, Y - 1); NY <= FMath::Min(Height - 1, Y + 1); ++NY)
					{
						for (int32 NX = FMath::Max(0, X - 1); NX <= FMath::Min(Width - 1, X + 1); ++NX)
						{
							if (Terrain[NY * Width + NX] == ECell::Clear)
							{
								Marked.Add(NY * Width + NX);
							}
						}
					}
				}
			}
			for (int32 I : Marked)
			{
				if (Pass == 0)
				{
					Terrain[I] = ECell::Cliff;
					CellFlags[I] |= FlagGrownCliff;
				}
				else
				{
					CellFlags[I] |= FlagCliffPinch;
				}
			}
		}
	}

	int32 Counts[int32(ECell::Closed) + 1] = {};
	int32 NumGrown = 0, NumPinched = 0; // (within the playable area, like the other counts)
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const FVector2f Lo((X - Border) * CellSize, (Y - Border) * CellSize);
			const FVector2f Hi = Lo + FVector2f(CellSize, CellSize);
			// Units may not leave the playable area.
			if (Lo.X < Playable.Min.X || Lo.Y < Playable.Min.Y || Hi.X > Playable.Max.X || Hi.Y > Playable.Max.Y)
			{
				Terrain[Y * Width + X] = ECell::Impassable;
			}
			Counts[int32(Terrain[Y * Width + X])]++;
			NumGrown += Terrain[Y * Width + X] == ECell::Cliff && (CellFlags[Y * Width + X] & FlagGrownCliff) != 0;
			NumPinched += Terrain[Y * Width + X] == ECell::Clear && (CellFlags[Y * Width + X] & FlagCliffPinch) != 0;
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Pathfinder: %dx%d cells (clear %d, water %d, cliff %d - %d of them the cell beside one, %d pinched, outside %d)"),
		Width, Height, Counts[int32(ECell::Clear)], Counts[int32(ECell::Water)], Counts[int32(ECell::Cliff)], NumGrown, NumPinched, Counts[int32(ECell::Impassable)]);

	for (const FGenBridgeInfo& B : InBridges)
	{
		BuildLayer(Map, B);
	}
	GoalUnit.Init(GenInvalidObjectID, NumNodes);
	FixedUnit.Init(GenInvalidObjectID, NumNodes);
}

void FGenPathfinder::BuildLayer(const FGenMapData& Map, const FGenBridgeInfo& Bridge)
{
	// PathfindLayer::allocateCells: the block of cells around the bridge, "pad with 1 extra".
	const int32 X0 = FMath::Max(0, FMath::FloorToInt32((Bridge.Bounds.Min.X - CellSize / 100.f) / CellSize) - 1 + Border);
	const int32 Y0 = FMath::Max(0, FMath::FloorToInt32((Bridge.Bounds.Min.Y - CellSize / 100.f) / CellSize) - 1 + Border);
	const int32 X1 = FMath::Min(Width, FMath::CeilToInt32((Bridge.Bounds.Max.X + CellSize / 100.f) / CellSize) + 1 + Border);
	const int32 Y1 = FMath::Min(Height, FMath::CeilToInt32((Bridge.Bounds.Max.Y + CellSize / 100.f) / CellSize) + 1 + Border);
	if (X1 <= X0 || Y1 <= Y0 || Layers.Num() >= 250)
	{
		return; // off the map
	}
	FLayer& L = Layers.AddDefaulted_GetRef();
	const int32 LayerId = Layers.Num();
	L.Bridge = Bridge;
	L.XOrigin = X0;
	L.YOrigin = Y0;
	L.W = X1 - X0;
	L.H = Y1 - Y0;
	L.NodeBase = NumNodes;
	NumNodes += L.W * L.H;
	L.Type.Init(0, L.W * L.H);
	L.Connect.Init(0, L.W * L.H);

	// PathfindLayer::classifyLayerMapCell
	int32 NumDeck = 0, NumEntry = 0, NumClosedBelow = 0;
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			const FVector2f Lo((X - Border) * CellSize, (Y - Border) * CellSize);
			const FVector2f Hi = Lo + FVector2f(CellSize, CellSize);
			const int32 OnBridge = int32(Bridge.IsPointOnBridge(Lo.X, Lo.Y)) + int32(Bridge.IsPointOnBridge(Lo.X, Hi.Y))
				+ int32(Bridge.IsPointOnBridge(Hi.X, Hi.Y)) + int32(Bridge.IsPointOnBridge(Hi.X, Lo.Y));
			uint8 Type = 0;
			bool bConnect = false;
			if (OnBridge == 4)
			{
				Type = 1;
			}
			else
			{
				if (OnBridge != 0)
				{
					Type = 2; // "it's off the bridge"
				}
				const FBox2f Cell(Lo, Hi);
				if (Bridge.IsCellOnSide(Cell))
				{
					Type = 2;
				}
				else
				{
					if (Bridge.IsCellOnEnd(Cell))
					{
						Type = 1;
					}
					if (Bridge.IsCellEntryPoint(Cell))
					{
						Type = 1;
						bConnect = true;
						ConnectLayer[Y * Width + X] = uint8(LayerId);
					}
				}
			}
			if (Type != 0 && !bConnect)
			{
				// "Check for bridge clearance. If the ground isn't 1 pathfind cells below, mark impassable."
				const FVector2f C = (Lo + Hi) * 0.5f;
				ECell& Ground = Terrain[Y * Width + X];
				if (Map.GetGroundHeight(C.X, C.Y) + LayerZCloseEnough > Bridge.GetHeight(C.X, C.Y) && Ground != ECell::Impassable)
				{
					Ground = ECell::BridgeImpassable;
					++NumClosedBelow;
				}
			}
			const int32 I = (Y - Y0) * L.W + (X - X0);
			L.Type[I] = Type;
			L.Connect[I] = bConnect ? 1 : 0;
			NumDeck += Type == 1;
			NumEntry += bConnect;
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Pathfinder: bridge layer %d: %d deck cells, %d of them steps to the ground, %d ground cells closed under it"),
		LayerId, NumDeck, NumEntry, NumClosedBelow);
}

int32 FGenPathfinder::NodeIndex(int32 Layer, int32 X, int32 Y) const
{
	if (Layer <= 0)
	{
		return (X < 0 || Y < 0 || X >= Width || Y >= Height) ? -1 : Y * Width + X;
	}
	if (!Layers.IsValidIndex(Layer - 1))
	{
		return -1;
	}
	const FLayer& L = Layers[Layer - 1];
	const int32 LX = X - L.XOrigin, LY = Y - L.YOrigin;
	if (LX < 0 || LY < 0 || LX >= L.W || LY >= L.H)
	{
		return -1;
	}
	const int32 I = LY * L.W + LX;
	return L.Type[I] != 0 ? L.NodeBase + I : -1;
}

void FGenPathfinder::NodeToCell(int32 Node, int32& OutLayer, int32& OutX, int32& OutY) const
{
	OutLayer = 0;
	if (Node >= Width * Height)
	{
		for (int32 i = 0; i < Layers.Num(); ++i)
		{
			const FLayer& L = Layers[i];
			if (Node >= L.NodeBase && Node < L.NodeBase + L.W * L.H)
			{
				const int32 I = Node - L.NodeBase;
				OutLayer = i + 1;
				OutX = L.XOrigin + I % L.W;
				OutY = L.YOrigin + I / L.W;
				return;
			}
		}
	}
	OutX = Node % Width;
	OutY = Node / Width;
}

const FGenBridgeInfo* FGenPathfinder::GetBridge(int32 Layer) const
{
	return Layers.IsValidIndex(Layer - 1) ? &Layers[Layer - 1].Bridge : nullptr;
}

int32 FGenPathfinder::GetLayerForDestination(const FVector3f& Pos, float GroundZ) const
{
	int32 Best = 0;
	float BestDistance = FMath::Abs(Pos.Z - GroundZ);
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FGenBridgeInfo& B = Layers[i].Bridge;
		if (B.IsPointOnBridge(Pos.X, Pos.Y))
		{
			const float Delta = FMath::Abs(Pos.Z - B.GetHeight(Pos.X, Pos.Y));
			if (Delta < BestDistance)
			{
				Best = i + 1;
				BestDistance = Delta;
			}
		}
	}
	return Best;
}

int32 FGenPathfinder::GetHighestLayerForDestination(const FVector3f& Pos, float GroundZ) const
{
	int32 Best = 0;
	float BestDistance = Pos.Z - GroundZ; // ("NOT fabs in this case.")
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FGenBridgeInfo& B = Layers[i].Bridge;
		if (B.IsPointOnBridge(Pos.X, Pos.Y))
		{
			const float Delta = Pos.Z - B.GetHeight(Pos.X, Pos.Y);
			if (Delta >= 0.f && FMath::Abs(Delta) < FMath::Abs(BestDistance))
			{
				Best = i + 1;
				BestDistance = Delta;
			}
		}
	}
	return Best;
}

float FGenPathfinder::GetLayerHeight(float X, float Y, int32 Layer, float GroundZ) const
{
	// W3DTerrainLogic::getLayerHeight: the deck where the spot is on the bridge (and the deck is above the ground).
	if (const FGenBridgeInfo* B = GetBridge(Layer))
	{
		if (B->IsPointOnBridge(X, Y))
		{
			return FMath::Max(GroundZ, B->GetHeight(X, Y));
		}
	}
	return GroundZ;
}

int32 FGenPathfinder::PickBridge(const FVector3f& Start, const FVector3f& Dir, FVector3f& OutHit) const
{
	int32 Best = 0;
	float BestDist = FLT_MAX;
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		FVector3f Hit;
		if (Layers[i].Bridge.RayHit(Start, Dir, Hit))
		{
			const float D = FVector3f::DistSquared(Start, Hit);
			if (D < BestDist)
			{
				BestDist = D;
				Best = i + 1;
				OutHit = Hit;
			}
		}
	}
	return Best;
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
	if (Obstacles[I] > 0)
	{
		return ECell::Obstacle;
	}
	if (Terrain[I] == ECell::Clear)
	{
		if (Fences[I] > 0)
		{
			return ECell::Fence;
		}
		if (CellFlags[I] & FlagClosed)
		{
			return ECell::Closed;
		}
	}
	return Terrain[I];
}

bool FGenPathfinder::IsPinched(int32 X, int32 Y) const
{
	if (X < 0 || Y < 0 || X >= Width || Y >= Height)
	{
		return false;
	}
	const int32 I = Y * Width + X;
	return (CellFlags[I] & (FlagCliffPinch | FlagObstaclePinch)) != 0 && Terrain[I] == ECell::Clear && Obstacles[I] == 0;
}

bool FGenPathfinder::CanPass(int32 X, int32 Y, uint32 Surfaces, int32 Layer) const
{
	if (Layer > 0)
	{
		// A bridge's deck is plain ground to whatever can drive or walk.
		const int32 Node = NodeIndex(Layer, X, Y);
		if (Node < 0 || Layers[Layer - 1].Type[Node - Layers[Layer - 1].NodeBase] != 1)
		{
			return false;
		}
		if (AvoidRadius >= 0)
		{
			for (int32 DY = -AvoidRadius; DY <= AvoidRadius; ++DY)
			{
				for (int32 DX = -AvoidRadius; DX <= AvoidRadius; ++DX)
				{
					const int32 Other = NodeIndex(Layer, X + DX, Y + DY);
					if (Other >= 0 && FixedUnit[Other] != GenInvalidObjectID && FixedUnit[Other] != AvoidSelf) { return false; }
				}
			}
		}
		return (Surfaces & (GenSurface::Ground | GenSurface::Air)) != 0;
	}
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
	// Pathfinder::validMovementPosition: "if (isCrusher && toCell->isObstacleFence()) return true"
	case ECell::Fence: return (Surfaces & GenSurface::Ground) != 0 && (Surfaces & GenSurface::Fence) != 0;
	default: return false;
	}
}

bool FGenPathfinder::IsPassable(const FVector2f& WorldPos, uint32 Surfaces, int32 Layer) const
{
	const FIntPoint C = WorldToCell(WorldPos);
	return CanPass(C.X, C.Y, Surfaces, Layer);
}

FIntRect FGenPathfinder::RasterizeFootprint(const FGenObject& Obj, int32 Delta)
{
	FIntRect Touched(FIntPoint(MAX_int32, MAX_int32), FIntPoint(MIN_int32, MIN_int32));
	const FGenThingTemplate* T = Obj.GetTemplate();
	if (!T || Width == 0)
	{
		return Touched;
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
				Touched.Min.X = FMath::Min(Touched.Min.X, X);
				Touched.Min.Y = FMath::Min(Touched.Min.Y, Y);
				Touched.Max.X = FMath::Max(Touched.Max.X, X);
				Touched.Max.Y = FMath::Max(Touched.Max.Y, Y);
			}
		}
	}
	return Touched;
}

void FGenPathfinder::ReclassifyAround(FIntRect B, bool bInsert)
{
	if (B.Max.X < B.Min.X || B.Max.Y < B.Min.Y || CVarGenPathCliffMargin.GetValueOnAnyThread() == 0)
	{
		return;
	}
	// Pathfinder::internal_classifyObjectFootprint, once the footprint itself is in (or out): the cells
	// two around it are looked over again.
	B.Min.X = FMath::Max(0, B.Min.X - 2);
	B.Min.Y = FMath::Max(0, B.Min.Y - 2);
	B.Max.X = FMath::Min(Width - 1, B.Max.X + 2);
	B.Max.Y = FMath::Min(Height - 1, B.Max.Y + 2);
	if (!bInsert)
	{
		for (int32 Y = B.Min.Y; Y <= B.Max.Y; ++Y)
		{
			for (int32 X = B.Min.X; X <= B.Max.X; ++X)
			{
				CellFlags[Y * Width + X] &= ~FlagClosed;
			}
		}
	}
	// "Check for pinched cells, and close them off": "If the total open cells are < 2 or total cells < 4,
	// we are pinched" - a gap a cell wide between two structures (or a structure and a cliff) is no way through.
	TArray<int32, TInlineAllocator<32>> Shut;
	for (int32 Y = B.Min.Y; Y <= B.Max.Y; ++Y)
	{
		for (int32 X = B.Min.X; X <= B.Max.X; ++X)
		{
			CellFlags[Y * Width + X] &= ~FlagObstaclePinch;
			if (GetCell(X, Y) != ECell::Clear)
			{
				continue;
			}
			int32 Total = 0, Orthogonal = 0;
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					if ((DX != 0 || DY != 0) && GetCell(X + DX, Y + DY) == ECell::Clear)
					{
						++Total;
						Orthogonal += (DX == 0 || DY == 0);
					}
				}
			}
			if (Orthogonal < 2 || Total < 4)
			{
				Shut.Add(Y * Width + X);
			}
		}
	}
	for (int32 I : Shut)
	{
		CellFlags[I] |= FlagClosed;
	}
	// "Expand building bounds 1 cell": open ground right beside a structure is pinched.
	for (int32 Y = B.Min.Y; Y <= B.Max.Y; ++Y)
	{
		for (int32 X = B.Min.X; X <= B.Max.X; ++X)
		{
			if (GetCell(X, Y) != ECell::Clear)
			{
				continue;
			}
			auto IsObstacle = [this](int32 OX, int32 OY)
			{
				const ECell C = GetCell(OX, OY);
				return C == ECell::Obstacle || C == ECell::Fence;
			};
			if (IsObstacle(X - 1, Y) || IsObstacle(X + 1, Y) || IsObstacle(X, Y - 1) || IsObstacle(X, Y + 1))
			{
				CellFlags[Y * Width + X] |= FlagObstaclePinch;
			}
		}
	}
}

void FGenPathfinder::RasterizeFence(const FGenObject& Obj, int32 Delta)
{
	// Pathfinder::classifyFence: a line of cells from where the fence stands (less FenceXOffset) along
	// its facing for FenceWidth, taken in half-cell steps.
	const FGenThingTemplate* T = Obj.GetTemplate();
	if (!T || Width == 0 || T->FenceWidth <= 0.f)
	{
		return;
	}
	const float Angle = Obj.GetOrientation();
	const FVector2f Dir(FMath::Cos(Angle), FMath::Sin(Angle));
	const FVector2f Start = FVector2f(Obj.GetPosition().X, Obj.GetPosition().Y) - Dir * T->FenceXOffset;
	const float StepSize = CellSize * 0.5f;
	const int32 NumSteps = FMath::CeilToInt32(T->FenceWidth / StepSize);
	int32 LastCell = INDEX_NONE;
	bool bChanged = false;
	for (int32 i = 0; i < NumSteps; ++i)
	{
		const FIntPoint C = WorldToCell(Start + Dir * (StepSize * float(i)));
		if (C.X < 0 || C.Y < 0 || C.X >= Width || C.Y >= Height)
		{
			continue;
		}
		const int32 Index = C.Y * Width + C.X;
		if (Index != LastCell)
		{
			LastCell = Index;
			Fences[Index] = uint8(FMath::Clamp(int32(Fences[Index]) + Delta, 0, 255));
			bChanged = true;
		}
	}
	if (bChanged)
	{
		++ObstacleVersion;
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
			if (!bInside)
			{
				continue;
			}
			if (X < 0 || Y < 0 || X >= Width || Y >= Height)
			{
				return false;
			}
			// What building goes by is the land itself and what stands on it (BuildAssistant), not the
			// pathfinder's margins: the cell kept free beside a cliff and a gap shut beside a structure are
			// no hindrance. (Nor is a fence: it is cleared away with the shrubs.)
			const int32 I = Y * Width + X;
			const bool bLand = Terrain[I] == ECell::Clear || (Terrain[I] == ECell::Cliff && (CellFlags[I] & FlagGrownCliff) != 0);
			if (Obstacles[I] > 0 || !bLand)
			{
				return false;
			}
		}
	}
	return true;
}

void FGenPathfinder::AddObstacle(const FGenObject& Obj)
{
	ReclassifyAround(RasterizeFootprint(Obj, +1), true);
	++ObstacleVersion;
}

void FGenPathfinder::RemoveObstacle(const FGenObject& Obj)
{
	ReclassifyAround(RasterizeFootprint(Obj, -1), false);
	++ObstacleVersion;
}

const TArray<int32>& FGenPathfinder::GetRegions(uint32 Surfaces) const
{
	FRegions& R = Regions.FindOrAdd(Surfaces);
	if (R.Version == ObstacleVersion && R.Label.Num() == NumNodes)
	{
		return R.Label;
	}
	GEN_PERF_SCOPE("Path.Regions");
	R.Version = ObstacleVersion;
	// (Units don't count: they come and go. A way that only units close is found, or given up on, by the search.)
	const int32 SavedAvoid = AvoidRadius;
	AvoidRadius = -1;
	const int32 Ground = Width * Height;
	R.Label.SetNumUninitialized(NumNodes);
	// -2: open, not yet labelled; -1: closed.
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			R.Label[Y * Width + X] = CanPass(X, Y, Surfaces, 0) ? -2 : -1;
		}
		if ((Y & 15) == 15 && YieldFn)
		{
			YieldFn(YieldContext);
		}
	}
	for (int32 l = 0; l < Layers.Num(); ++l)
	{
		const FLayer& L = Layers[l];
		const bool bOpen = (Surfaces & (GenSurface::Ground | GenSurface::Air)) != 0;
		for (int32 i = 0; i < L.W * L.H; ++i)
		{
			R.Label[L.NodeBase + i] = (bOpen && L.Type[i] == 1) ? -2 : -1;
		}
	}
	// Flood each open node's surroundings. Four neighbours are enough: a diagonal step is only ever
	// taken where both cells beside it are open too.
	int32 Next = 0;
	TArray<int32>& Queue = RegionQueue;
	for (int32 Seed = 0; Seed < NumNodes; ++Seed)
	{
		if (R.Label[Seed] != -2)
		{
			continue;
		}
		const int32 Id = Next++;
		Queue.Reset();
		Queue.Add(Seed);
		R.Label[Seed] = Id;
		for (int32 q = 0; q < Queue.Num(); ++q)
		{
			if ((q & 8191) == 8191 && YieldFn)
			{
				YieldFn(YieldContext);
			}
			const int32 Node = Queue[q];
			auto Visit = [&R, &Queue, Id](int32 Other)
			{
				if (Other >= 0 && R.Label[Other] == -2)
				{
					R.Label[Other] = Id;
					Queue.Add(Other);
				}
			};
			if (Node < Ground)
			{
				const int32 X = Node % Width, Y = Node / Width;
				if (X > 0) { Visit(Node - 1); }
				if (X < Width - 1) { Visit(Node + 1); }
				if (Y > 0) { Visit(Node - Width); }
				if (Y < Height - 1) { Visit(Node + Width); }
				if (const int32 Up = ConnectLayer[Node])
				{
					Visit(NodeIndex(Up, X, Y));
				}
			}
			else
			{
				int32 L, X, Y;
				NodeToCell(Node, L, X, Y);
				Visit(NodeIndex(L, X - 1, Y));
				Visit(NodeIndex(L, X + 1, Y));
				Visit(NodeIndex(L, X, Y - 1));
				Visit(NodeIndex(L, X, Y + 1));
				if (Layers[L - 1].Connect[Node - Layers[L - 1].NodeBase])
				{
					Visit(NodeIndex(0, X, Y));
				}
			}
		}
	}
	AvoidRadius = SavedAvoid;
	return R.Label;
}

bool FGenPathfinder::IsLineClear(const FVector2f& A, const FVector2f& B, uint32 Surfaces, int32 Layer) const
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
		if (!CanPass(C.X, C.Y, Surfaces, Layer))
		{
			return false;
		}
		// Moving diagonally between cells must not squeeze between two blocked cells.
		if (Last.X != INT32_MIN && Last.X != C.X && Last.Y != C.Y)
		{
			if (!CanPass(Last.X, C.Y, Surfaces, Layer) && !CanPass(C.X, Last.Y, Surfaces, Layer))
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

void FGenPathfinder::Stamp(TArray<FGenObjectID>& Cells, const FUnitCells& Where, FGenObjectID From, FGenObjectID To)
{
	// Only cells currently holding From change: claiming (invalid -> id) never takes a cell someone else
	// holds, releasing (id -> invalid) never frees someone else's.
	for (int32 Y = FMath::Max(0, Where.Rect.Min.Y); Y < FMath::Min(Height, Where.Rect.Max.Y); ++Y)
	{
		for (int32 X = FMath::Max(0, Where.Rect.Min.X); X < FMath::Min(Width, Where.Rect.Max.X); ++X)
		{
			const int32 Node = NodeIndex(Where.Layer, X, Y);
			if (Node >= 0 && Cells[Node] == From) { Cells[Node] = To; }
		}
	}
}

void FGenPathfinder::UpdateUnitPos(FGenObjectID Id, const FVector2f& Pos, float Radius, bool bMoving, int32 Layer)
{
	if (Width == 0)
	{
		return;
	}
	const FUnitCells* Old = FixedRects.Find(Id);
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
	const FUnitCells Rect{ UnitRect(Pos, Radius), Layer };
	if (Old && *Old != Rect) { Stamp(FixedUnit, *Old, Id, GenInvalidObjectID); }
	Stamp(FixedUnit, Rect, GenInvalidObjectID, Id);
	FixedRects.Add(Id, Rect);
}

void FGenPathfinder::UpdateUnitGoal(FGenObjectID Id, const FVector2f& GoalPos, float Radius, int32 Layer)
{
	if (Width == 0)
	{
		return;
	}
	const FUnitCells Rect{ UnitRect(GoalPos, Radius), Layer };
	if (const FUnitCells* Old = GoalRects.Find(Id))
	{
		if (*Old == Rect) { return; }
		Stamp(GoalUnit, *Old, Id, GenInvalidObjectID);
	}
	Stamp(GoalUnit, Rect, GenInvalidObjectID, Id);
	GoalRects.Add(Id, Rect);
}

void FGenPathfinder::RemoveUnitGoal(FGenObjectID Id)
{
	if (const FUnitCells* Old = GoalRects.Find(Id))
	{
		Stamp(GoalUnit, *Old, Id, GenInvalidObjectID);
		GoalRects.Remove(Id);
	}
}

void FGenPathfinder::RemoveUnit(FGenObjectID Id)
{
	if (const FUnitCells* Old = FixedRects.Find(Id)) { Stamp(FixedUnit, *Old, Id, GenInvalidObjectID); }
	if (const FUnitCells* Old = GoalRects.Find(Id)) { Stamp(GoalUnit, *Old, Id, GenInvalidObjectID); }
	FixedRects.Remove(Id);
	GoalRects.Remove(Id);
}

bool FGenPathfinder::IsDestinationFree(FGenObjectID Id, const FVector2f& Pos, float Radius, uint32 Surfaces, int32 Layer) const
{
	// Pathfinder::checkDestination
	const FIntRect Rect = UnitRect(Pos, Radius);
	for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
	{
		for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
		{
			const int32 Node = NodeIndex(Layer, X, Y);
			if (Node < 0 || !CanPass(X, Y, Surfaces, Layer))
			{
				return false;
			}
			const FGenObjectID Goal = GoalUnit[Node];
			const FGenObjectID Fixed = FixedUnit[Node];
			if ((Goal != GenInvalidObjectID && Goal != Id) || (Fixed != GenInvalidObjectID && Fixed != Id))
			{
				return false; // someone else is going there, or parked there
			}
		}
	}
	return true;
}

bool FGenPathfinder::AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos, int32 Layer) const
{
	return AdjustUnitDestination(Id, Radius, Surfaces, InOutPos, [](const FVector2f&) { return true; }, Layer);
}

bool FGenPathfinder::AdjustUnitDestination(FGenObjectID Id, float Radius, uint32 Surfaces, FVector2f& InOutPos, TFunctionRef<bool(const FVector2f&)> Accept, int32 Layer) const
{
	GEN_PERF_SCOPE("Path.AdjustDest");
	// Pathfinder::adjustDestination / adjustTargetDestination
	if (Width == 0 || (Accept(InOutPos) && IsDestinationFree(Id, InOutPos, Radius, Surfaces, Layer)))
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
		if (Accept(P) && IsDestinationFree(Id, P, Radius, Surfaces, Layer))
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
	TArray<FVector2f>& OutPath, int32 StartLayer, int32 GoalLayer, TArray<uint8>* OutLayers) const
{
	GEN_PERF_SCOPE("Path.AroundUnits");
	FDetachedSearch Detached(*this);
	AvoidSelf = Id;
	// A vehicle keeps a cell clear of the parked units, so that its body gets past them too; where that finds
	// no way (or none that gets near the goal), it tries again hard by them.
	AvoidRadius = Radius > CellSize ? 1 : 0;
	bool bFound = FindPath(Start, Goal, Surfaces, OutPath, StartLayer, GoalLayer, OutLayers);
	if (AvoidRadius > 0 && (!bFound || OutPath.Num() == 0 || FVector2f::DistSquared(OutPath.Last(), Goal) > FMath::Square(2.f * CellSize)))
	{
		AvoidRadius = 0;
		bFound = FindPath(Start, Goal, Surfaces, OutPath, StartLayer, GoalLayer, OutLayers);
	}
	AvoidSelf = GenInvalidObjectID;
	AvoidRadius = -1;
	return bFound;
}

bool FGenPathfinder::FindPath(const FVector2f& Start, const FVector2f& Goal, uint32 Surfaces, TArray<FVector2f>& OutPath,
	int32 StartLayer, int32 GoalLayer, TArray<uint8>* OutLayers) const
{
	GEN_PERF_SCOPE("Path.FindPath");
	FDetachedSearch Detached(*this);
	OutPath.Reset();
	if (OutLayers)
	{
		OutLayers->Reset();
	}
	if (Width == 0)
	{
		return false;
	}
	FIntPoint S = WorldToCell(Start);
	FIntPoint G = WorldToCell(Goal);
	S.X = FMath::Clamp(S.X, 0, Width - 1);
	S.Y = FMath::Clamp(S.Y, 0, Height - 1);
	if (StartLayer > 0 && NodeIndex(StartLayer, S.X, S.Y) < 0)
	{
		StartLayer = 0; // not on that bridge after all
	}
	if (GoalLayer > 0 && !CanPass(G.X, G.Y, Surfaces, GoalLayer))
	{
		GoalLayer = 0;  // no room on the bridge there: the ground below (or the nearest to it) it is
	}

	// Units standing on an invalid cell (e.g. spawned inside a footprint) path from the nearest valid one.
	// (Not when pathing around units: the start may touch a parked unit, and must not jump past it.)
	if (StartLayer == 0 && AvoidRadius < 0 && !CanPass(S.X, S.Y, Surfaces))
	{
		FVector2f Fixed;
		if (!AdjustDestination(Start, Surfaces, Fixed, 8))
		{
			return false;
		}
		S = WorldToCell(Fixed);
	}
	FVector2f GoalPos = Goal;
	if (GoalLayer == 0 && !CanPass(G.X, G.Y, Surfaces))
	{
		if (AdjustDestination(Goal, Surfaces, GoalPos))
		{
			G = WorldToCell(GoalPos);
		}
	}

	const int32 StartIdx = NodeIndex(StartLayer, S.X, S.Y);
	int32 GoalIdx = NodeIndex(GoalLayer, G.X, G.Y);
	if (StartIdx == GoalIdx || StartIdx < 0)
	{
		OutPath.Add(GoalPos);
		if (OutLayers)
		{
			OutLayers->Add(uint8(GoalLayer));
		}
		return true;
	}

	// Can the goal be reached at all? If not (it lies on ground cut off from where we are), make for the
	// place nearest to it that can be - instead of searching the whole map to find that there is no way.
	bool bReachable = false;
	{
		const TArray<int32>& Region = GetRegions(Surfaces);
		const int32 StartRegion = Region[StartIdx];
		if (StartRegion >= 0 && GoalIdx >= 0 && Region[GoalIdx] == StartRegion)
		{
			bReachable = true;
		}
		else if (StartRegion >= 0)
		{
			int32 BestCell = -1;
			int64 BestDistSq = MAX_int64;
			for (int32 Ring = 1; Ring <= 96 && BestCell < 0; ++Ring)
			{
				for (int32 DY = -Ring; DY <= Ring; ++DY)
				{
					const int32 Step = FMath::Abs(DY) == Ring ? 1 : 2 * Ring;
					for (int32 DX = -Ring; DX <= Ring; DX += Step)
					{
						const int32 Cell = NodeIndex(0, G.X + DX, G.Y + DY);
						const int64 DistSq = int64(DX) * DX + int64(DY) * DY;
						if (Cell >= 0 && Region[Cell] == StartRegion && DistSq < BestDistSq)
						{
							BestDistSq = DistSq;
							BestCell = Cell;
						}
					}
				}
			}
			if (BestCell >= 0)
			{
				G = FIntPoint(BestCell % Width, BestCell / Width);
				GoalLayer = 0;
				GoalIdx = BestCell;
				GoalPos = CellCenter(G.X, G.Y);
				bReachable = true;
				if (StartIdx == GoalIdx)
				{
					return true; // already as near as it gets
				}
			}
		}
	}

	// A* (8-connected, octile heuristic, no corner cutting) over the ground's cells and the bridges'.
	if (SeenStamp.Num() != NumNodes)
	{
		SeenStamp.Init(0, NumNodes);
		ClosedStamp.Init(0, NumNodes);
		GCost.SetNumUninitialized(NumNodes);
		Parent.SetNumUninitialized(NumNodes);
		SearchStamp = 0;
	}
	if (++SearchStamp == 0)
	{
		FMemory::Memzero(SeenStamp.GetData(), SeenStamp.Num() * sizeof(uint32));
		FMemory::Memzero(ClosedStamp.GetData(), ClosedStamp.Num() * sizeof(uint32));
		SearchStamp = 1;
	}
	const uint32 Stamp = SearchStamp;
	using FNode = FOpenNode;
	Open.Reset();
	auto Less = [](const FNode& A, const FNode& B) { return A.F < B.F; };
	auto Heuristic = [&](int32 X, int32 Y)
	{
		const int32 DX = FMath::Abs(X - G.X), DY = FMath::Abs(Y - G.Y);
		return 10 * FMath::Max(DX, DY) + 4 * FMath::Min(DX, DY);
	};
	// What is left to go counts for a little more than what has been covered (weighted A*): the search
	// then heads for the goal instead of spreading evenly, and looks at a fraction of the cells - for a
	// way that can be a few percent longer than the shortest, which the straightening afterwards mostly
	// takes out again.
	const int32 Weight = FMath::Clamp(CVarGenPathWeight.GetValueOnAnyThread(), 10, 30);
	int32 Expanded = 0;
	// A search that may not get there is not allowed to look at everything: around parked units there
	// may be no way at all, and off the labelled ground nobody knows.
	int32 Budget = AvoidRadius >= 0 ? 25000 : (bReachable ? MAX_int32 : 60000);

	SeenStamp[StartIdx] = Stamp;
	GCost[StartIdx] = 0;
	Parent[StartIdx] = -1;
	Open.HeapPush({ Heuristic(S.X, S.Y) * Weight / 10, StartIdx }, Less);

	int32 BestIdx = StartIdx;
	int32 BestH = Heuristic(S.X, S.Y);
	const int32 DX8[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	const int32 DY8[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

	while (Open.Num() > 0)
	{
		FNode Cur;
		Open.HeapPop(Cur, Less, EAllowShrinking::No);
		if (ClosedStamp[Cur.Index] == Stamp) { continue; }
		ClosedStamp[Cur.Index] = Stamp;
		if (Cur.Index == GoalIdx)
		{
			BestIdx = GoalIdx;
			break;
		}
		if (--Budget < 0)
		{
			GEN_PERF_COUNT("Path.GaveUp#", 1);
			break; // as near as we got will have to do
		}
		++Expanded;
		if ((Expanded & 511) == 0 && YieldFn)
		{
			YieldFn(YieldContext);
		}
		int32 CL, CX, CY;
		NodeToCell(Cur.Index, CL, CX, CY);
		const int32 H = Heuristic(CX, CY);
		// (The nearest cell to an unreachable goal is one on the goal's own layer, if any was reached.)
		if (H < BestH && (CL == GoalLayer || BestIdx == StartIdx)) { BestH = H; BestIdx = Cur.Index; }

		auto Relax = [&](int32 NI, int32 NX, int32 NY, int32 Cost)
		{
			if (NI < 0 || ClosedStamp[NI] == Stamp) { return; }
			const int32 NewG = GCost[Cur.Index] + Cost;
			if (SeenStamp[NI] != Stamp || NewG < GCost[NI])
			{
				SeenStamp[NI] = Stamp;
				GCost[NI] = NewG;
				Parent[NI] = Cur.Index;
				Open.HeapPush({ NewG + Heuristic(NX, NY) * Weight / 10, NI }, Less);
			}
		};
		for (int32 k = 0; k < 8; ++k)
		{
			const int32 NX = CX + DX8[k], NY = CY + DY8[k];
			if (!CanPass(NX, NY, Surfaces, CL)) { continue; }
			const bool bDiag = k >= 4;
			if (bDiag && (!CanPass(CX + DX8[k], CY, Surfaces, CL) || !CanPass(CX, CY + DY8[k], Surfaces, CL))) { continue; }
			// PathfindCell::costSoFar ("if (getPinched()) cost += COST_DIAGONAL") and examineNeighboringCells
			// ("else if (newCell->getPinched()) newCostSoFar += COST_ORTHOGONAL").
			Relax(NodeIndex(CL, NX, NY), NX, NY, (bDiag ? 14 : 10) + ((CL == 0 && IsPinched(NX, NY)) ? 24 : 0));
		}
		// Pathfinder::checkChangeLayers: at a bridge's ends the cell above (or below) is the same place.
		if (CL == 0)
		{
			if (const int32 Up = ConnectLayer[Cur.Index])
			{
				Relax(NodeIndex(Up, CX, CY), CX, CY, 0);
			}
		}
		else if (Layers[CL - 1].Connect[Cur.Index - Layers[CL - 1].NodeBase])
		{
			Relax(NodeIndex(0, CX, CY), CX, CY, 0);
		}
	}

	GEN_PERF_COUNT("Path.Cells#", Expanded);
	CellsSearched += Expanded;
	if (!bReachable)
	{
		GEN_PERF_COUNT("Path.Unreachable#", 1);
	}
	GEN_PERF_SCOPE("Path.Straighten");

	// Reconstruct (to the goal, or to the closest reachable cell).
	TArray<FVector2f> Cells;
	TArray<uint8> CellLayers;
	for (int32 I = BestIdx; I != -1 && I != StartIdx; I = Parent[I])
	{
		int32 L, X, Y;
		NodeToCell(I, L, X, Y);
		Cells.Add(CellCenter(X, Y));
		CellLayers.Add(uint8(L));
	}
	Algo::Reverse(Cells);
	Algo::Reverse(CellLayers);
	if (BestIdx == GoalIdx)
	{
		if (Cells.Num() > 0) { Cells.Last() = GoalPos; } else { Cells.Add(GoalPos); CellLayers.Add(uint8(GoalLayer)); }
	}
	if (Cells.Num() == 0)
	{
		return true; // already at the closest reachable point
	}

	// Smooth: from each anchor, jump to the farthest waypoint in line of sight - within a layer. Where the
	// path changes layer (the two cells are one place) the spot itself stays in the path.
	FVector2f Anchor = Start;
	int32 AnchorLayer = StartLayer;
	auto Emit = [&](const FVector2f& P, int32 Layer)
	{
		OutPath.Add(P);
		if (OutLayers)
		{
			OutLayers->Add(uint8(Layer));
		}
		Anchor = P;
	};
	int32 i = 0;
	while (i < Cells.Num())
	{
		if (CellLayers[i] != AnchorLayer)
		{
			AnchorLayer = CellLayers[i];
			if (!Cells[i].Equals(Anchor, 0.01f) || i == Cells.Num() - 1)
			{
				Emit(Cells[i], AnchorLayer);
			}
			++i;
			continue;
		}
		int32 RunEnd = i;
		while (RunEnd + 1 < Cells.Num() && CellLayers[RunEnd + 1] == AnchorLayer)
		{
			++RunEnd;
		}
		// (No further ahead than 48 cells, and half as far each time the view is blocked: looking from
		// every waypoint to every later one made long paths cost more to straighten than to find.)
		int32 Far = i;
		for (int32 Reach = FMath::Min(RunEnd - i, 48); Reach > 0; Reach /= 2)
		{
			if (IsLineClear(Anchor, Cells[i + Reach], Surfaces, AnchorLayer)) { Far = i + Reach; break; }
		}
		Emit(Cells[Far], AnchorLayer);
		i = Far + 1;
	}
	return true;
}

// ---- group paths --------------------------------------------------------------------------------

int32 FGenPathfinder::ClearCellForDiameter(bool bCrusher, int32 CellX, int32 CellY, int32 Layer, int32 PathDiameter, TFunctionRef<int32(FGenObjectID)> CrushableLevel) const
{
	for (;;)
	{
		const int32 Radius = PathDiameter / 2;
		const int32 NumCellsAbove = Radius == 0 ? 1 : Radius;
		// "We remove the outside corner cells from the check."
		const bool bCutCorners = Radius > 1;
		bool bClear = true;
		for (int32 X = CellX - Radius; X < CellX + NumCellsAbove && bClear; ++X)
		{
			const bool bXEdge = X == CellX - Radius || X == CellX + NumCellsAbove - 1;
			for (int32 Y = CellY - Radius; Y < CellY + NumCellsAbove && bClear; ++Y)
			{
				const bool bYEdge = Y == CellY - Radius || Y == CellY + NumCellsAbove - 1;
				if (bXEdge && bYEdge && bCutCorners)
				{
					continue;
				}
				if (X < 0 || Y < 0 || X >= Width || Y >= Height)
				{
					return 0; // "off the map"
				}
				// (Pathfinder::getCell: the bridge's own cell where it has one there, else the ground's.)
				int32 Node = Layer > 0 ? NodeIndex(Layer, X, Y) : -1;
				if (Node >= 0)
				{
					bClear = Layers[Layer - 1].Type[Node - Layers[Layer - 1].NodeBase] == 1;
				}
				else
				{
					Node = Y * Width + X;
					const ECell Type = GetCell(X, Y);
					// A fence is in nobody's way who can drive it down.
					bClear = Type == ECell::Clear || (Type == ECell::Fence && bCrusher);
				}
				// Somebody parked there: in the way unless he can be driven (or walked) over.
				if (bClear && PathDiameter >= 2 && FixedUnit[Node] != GenInvalidObjectID)
				{
					const int32 Level = CrushableLevel(FixedUnit[Node]);
					bClear = Level < 0 || Level <= (bCrusher ? 1 : 0);
				}
			}
		}
		if (bClear)
		{
			return Radius == 0 ? 1 : 2 * Radius;
		}
		if (PathDiameter < 2)
		{
			return 0;
		}
		PathDiameter -= 2;
	}
}

bool FGenPathfinder::IsGroundLineClear(const FVector2f& A, const FVector2f& B, int32 Layer, int32 PathDiameter, bool bCrusher, TFunctionRef<int32(FGenObjectID)> CrushableLevel) const
{
	// Pathfinder::isGroundPathPassable: the whole width has room at every cell along the line.
	const FVector2f D = B - A;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(D.Size() / (CellSize * 0.5f)));
	FIntPoint Last(MIN_int32, MIN_int32);
	for (int32 i = 0; i <= Steps; ++i)
	{
		const FIntPoint C = WorldToCell(A + D * (float(i) / float(Steps)));
		if (C == Last)
		{
			continue;
		}
		Last = C;
		if (ClearCellForDiameter(bCrusher, C.X, C.Y, Layer, PathDiameter, CrushableLevel) != PathDiameter)
		{
			return false;
		}
	}
	return true;
}

bool FGenPathfinder::FindGroundPath(const FVector2f& From, const FVector2f& To, int32 PathDiameter, bool bCrusher,
	TFunctionRef<int32(FGenObjectID)> CrushableLevel, TArray<FVector2f>& OutPath, int32 FromLayer, int32 ToLayer) const
{
	GEN_PERF_SCOPE("Path.GroundPath");
	FDetachedSearch Detached(*this);
	OutPath.Reset();
	if (Width == 0)
	{
		return false;
	}
	auto ClearAt = [&](int32 X, int32 Y, int32 L) { return ClearCellForDiameter(bCrusher, X, Y, L, PathDiameter, CrushableLevel); };

	// The goal: where the whole width has room, up to eight cells from where it was asked for.
	FIntPoint G = WorldToCell(To);
	if (ToLayer > 0 && NodeIndex(ToLayer, G.X, G.Y) < 0)
	{
		ToLayer = 0;
	}
	if (ClearAt(G.X, G.Y, ToLayer) != PathDiameter)
	{
		static const int32 SX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, SY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		bool bFound = false;
		for (int32 Offset = 1; Offset < 8 && !bFound; ++Offset)
		{
			for (int32 k = 0; k < 8 && !bFound; ++k)
			{
				const FIntPoint C(G.X + SX[k] * Offset, G.Y + SY[k] * Offset);
				if (ClearAt(C.X, C.Y, ToLayer) == PathDiameter)
				{
					G = C;
					bFound = true;
				}
			}
		}
		if (!bFound)
		{
			return false;
		}
	}
	int32 GoalIdx = NodeIndex(ToLayer, G.X, G.Y);
	if (GoalIdx < 0)
	{
		ToLayer = 0;
		GoalIdx = NodeIndex(0, G.X, G.Y);
	}
	FIntPoint S = WorldToCell(From);
	S.X = FMath::Clamp(S.X, 0, Width - 1);
	S.Y = FMath::Clamp(S.Y, 0, Height - 1);
	int32 StartIdx = NodeIndex(FromLayer, S.X, S.Y);
	if (StartIdx < 0)
	{
		FromLayer = 0;
		StartIdx = NodeIndex(0, S.X, S.Y);
	}
	if (GoalIdx < 0 || StartIdx < 0 || StartIdx == GoalIdx)
	{
		return false;
	}
	// "zone1 != zone2": no way there on the ground at all.
	{
		const TArray<int32>& Region = GetRegions(GenSurface::Ground);
		if (Region[StartIdx] < 0 || Region[StartIdx] != Region[GoalIdx])
		{
			return false;
		}
	}

	if (SeenStamp.Num() != NumNodes)
	{
		SeenStamp.Init(0, NumNodes);
		ClosedStamp.Init(0, NumNodes);
		GCost.SetNumUninitialized(NumNodes);
		Parent.SetNumUninitialized(NumNodes);
		SearchStamp = 0;
	}
	if (++SearchStamp == 0)
	{
		FMemory::Memzero(SeenStamp.GetData(), SeenStamp.Num() * sizeof(uint32));
		FMemory::Memzero(ClosedStamp.GetData(), ClosedStamp.Num() * sizeof(uint32));
		SearchStamp = 1;
	}
	const uint32 Stamp = SearchStamp;
	Open.Reset();
	auto Less = [](const FOpenNode& A, const FOpenNode& B) { return A.F < B.F; };
	auto Heuristic = [&](int32 X, int32 Y)
	{
		const int32 DX = FMath::Abs(X - G.X), DY = FMath::Abs(Y - G.Y);
		return 10 * FMath::Max(DX, DY) + 4 * FMath::Min(DX, DY);
	};
	const int32 Weight = FMath::Clamp(CVarGenPathWeight.GetValueOnAnyThread(), 10, 30);
	SeenStamp[StartIdx] = Stamp;
	GCost[StartIdx] = 0;
	Parent[StartIdx] = -1;
	Open.HeapPush({ Heuristic(S.X, S.Y) * Weight / 10, StartIdx }, Less);

	// (The original's order: the four sides, then the diagonals - each open only beside an open side.)
	static const int32 DX8[8] = { 1, 0, -1, 0, 1, -1, -1, 1 };
	static const int32 DY8[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	static const int32 Adjacent[5] = { 0, 1, 2, 3, 0 };
	bool bFound = false;
	int32 Expanded = 0;
	int32 Budget = 150000;
	while (Open.Num() > 0)
	{
		FOpenNode Cur;
		Open.HeapPop(Cur, Less, EAllowShrinking::No);
		if (ClosedStamp[Cur.Index] == Stamp)
		{
			continue;
		}
		ClosedStamp[Cur.Index] = Stamp;
		if (Cur.Index == GoalIdx)
		{
			bFound = true;
			break;
		}
		if (--Budget < 0)
		{
			break;
		}
		if ((++Expanded & 255) == 0 && YieldFn)
		{
			YieldFn(YieldContext);
		}
		int32 CL, CX, CY;
		NodeToCell(Cur.Index, CL, CX, CY);
		auto Relax = [&](int32 NI, int32 NX, int32 NY, int32 Cost)
		{
			if (NI < 0 || ClosedStamp[NI] == Stamp)
			{
				return;
			}
			const int32 NewG = GCost[Cur.Index] + Cost;
			if (SeenStamp[NI] != Stamp || NewG < GCost[NI])
			{
				SeenStamp[NI] = Stamp;
				GCost[NI] = NewG;
				Parent[NI] = Cur.Index;
				Open.HeapPush({ NewG + Heuristic(NX, NY) * Weight / 10, NI }, Less);
			}
		};
		bool bOpenSide[8] = {};
		for (int32 k = 0; k < 8; ++k)
		{
			const int32 NX = CX + DX8[k], NY = CY + DY8[k];
			const int32 NI = NodeIndex(CL, NX, NY);
			if (NI < 0)
			{
				continue;
			}
			int32 Cost = k >= 4 ? 14 : 10;
			if (NI != GoalIdx)
			{
				if (k >= 4 && !bOpenSide[Adjacent[k - 4]] && !bOpenSide[Adjacent[k - 3]])
				{
					continue; // "make sure one of the adjacent sides is open"
				}
				// Open ground only (a bridge's deck is), and none of it hard by a cliff or a structure.
				const bool bOpen = CL > 0 ? Layers[CL - 1].Type[NI - Layers[CL - 1].NodeBase] == 1 : (GetCell(NX, NY) == ECell::Clear && !IsPinched(NX, NY));
				if (!bOpen)
				{
					continue;
				}
				bOpenSide[k] = true;
				// "See how wide the cell is": narrower than the group costs extra.
				const int32 ClearDiameter = ClearAt(NX, NY, CL);
				if (ClearDiameter < PathDiameter)
				{
					Cost += 6 * (PathDiameter - ClearDiameter);
				}
			}
			Relax(NI, NX, NY, Cost);
		}
		if (CL == 0)
		{
			if (const int32 Up = ConnectLayer[Cur.Index])
			{
				Relax(NodeIndex(Up, CX, CY), CX, CY, 0);
			}
		}
		else if (Layers[CL - 1].Connect[Cur.Index - Layers[CL - 1].NodeBase])
		{
			Relax(NodeIndex(0, CX, CY), CX, CY, 0);
		}
	}
	GEN_PERF_COUNT("Path.Cells#", Expanded);
	CellsSearched += Expanded;
	if (!bFound)
	{
		return false;
	}

	// Pathfinder::prependCells with "center" off: a node lies on its cell's low corner - the middle of the
	// width a group of an even number of cells takes up.
	TArray<FVector2f> Nodes;
	TArray<uint8> NodeLayers;
	for (int32 I = GoalIdx; I != -1 && I != StartIdx; I = Parent[I])
	{
		int32 L, X, Y;
		NodeToCell(I, L, X, Y);
		Nodes.Add(CellCenter(X, Y) - FVector2f(CellSize * 0.5f, CellSize * 0.5f));
		NodeLayers.Add(uint8(L));
	}
	Nodes.Add(From);
	NodeLayers.Add(uint8(FromLayer));
	Algo::Reverse(Nodes);
	Algo::Reverse(NodeLayers);

	// Path::optimizeGroundPath: from each corner on to the farthest node the whole width can make for in a
	// straight line ("horizontal, diagonal, and vertical steps are passable" as they are). Where the way
	// changes layer the spot stays in.
	OutPath.Add(From);
	int32 Anchor = 0;
	while (Anchor < Nodes.Num() - 1)
	{
		int32 RunEnd = Anchor + 1;
		while (RunEnd + 1 < Nodes.Num() && NodeLayers[RunEnd] == NodeLayers[Anchor] && NodeLayers[RunEnd + 1] == NodeLayers[Anchor])
		{
			++RunEnd;
		}
		int32 Far = Anchor + 1;
		const FVector2f Step = Nodes[Anchor + 1] - Nodes[Anchor];
		while (Far + 1 <= RunEnd && (Nodes[Far + 1] - Nodes[Far]).Equals(Step, 0.01f))
		{
			++Far;
		}
		for (int32 Reach = FMath::Min(RunEnd - Anchor, 64); Anchor + Reach > Far; Reach /= 2)
		{
			if (IsGroundLineClear(Nodes[Anchor], Nodes[Anchor + Reach], NodeLayers[Anchor], PathDiameter, bCrusher, CrushableLevel))
			{
				Far = Anchor + Reach;
				break;
			}
		}
		OutPath.Add(Nodes[Far]);
		Anchor = Far;
	}
	return OutPath.Num() >= 2;
}
