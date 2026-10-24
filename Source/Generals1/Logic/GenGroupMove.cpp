// Moving a group of units: AIGroup.cpp (groupMoveToPosition with friend_computeGroundPath,
// friend_moveInfantryToPos, friend_moveVehicleToPos, friend_moveFormationToPos, groupTightenToPosition,
// computeIndividualDestination, groupCreateFormation).
//
// A handful of units ordered somewhere near go each to the place they hold in the group, moved over to
// where the order points. Three infantry or more (or three vehicles) with some way to go travel as a
// column instead: one way wide enough for all of them is found over open ground (FindGroundPath), and
// every unit gets its own lane beside it and its own place at the end. A formation (Ctrl+F) keeps its
// exact shape and the pace of its slowest member. A click in the middle of the group gathers it in.

#include "Logic/GenGameLogic.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenPathfinder.h"
#include "Logic/GenSkirmishAI.h"
#include "Map/GenMapData.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	constexpr float Cell = GenLogic::PathfindCellSize;
	/** PATH_DIAMETER_IN_CELLS: how wide the way of a group is. */
	constexpr int32 PathDiameterInCells = 6;
	constexpr float FarEnoughSq = (PathDiameterInCells * Cell) * (PathDiameterInCells * Cell);

	/** The nodes of a group's way its lanes are laid along: the first far enough from its start, the last far enough from its end. */
	bool FindEndNodes(const TArray<FVector2f>& P, int32& OutStart, int32& OutEnd)
	{
		OutStart = OutEnd = INDEX_NONE;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			if (FVector2f::DistSquared(P[i], P[0]) > FarEnoughSq)
			{
				OutStart = i;
				break;
			}
		}
		for (int32 i = 0; i < P.Num(); ++i)
		{
			if (FVector2f::DistSquared(P[i], P.Last()) > FarEnoughSq)
			{
				OutEnd = i;
			}
		}
		return OutStart != INDEX_NONE && OutEnd != INDEX_NONE;
	}

	/** The column whose count is lowest, nearest to the one wanted (the first of them on a tie). */
	int32 PickColumn(int32* Columns, int32 Num, int32 Step, int32 Wanted)
	{
		int32 Lowest = 10000;
		for (int32 i = 0; i < Num; i += Step)
		{
			Lowest = FMath::Min(Lowest, Columns[i]);
		}
		int32 Best = INDEX_NONE, BestDelta = 10000;
		for (int32 i = 0; i < Num; i += Step)
		{
			if (Columns[i] == Lowest && FMath::Abs(Wanted - i) < BestDelta)
			{
				BestDelta = FMath::Abs(Wanted - i);
				Best = i;
			}
		}
		if (Best >= 0)
		{
			Columns[Best]++;
		}
		return Best;
	}

	/** getHelicopterOffset: helicopters reserve no ground, so each is sent to a place of its own on rings round the spot. */
	void HelicopterOffset(FVector3f& Pos, int32 Index)
	{
		if (Index == 0)
		{
			return;
		}
		const float Diameter = 70.f;
		float Radius = Diameter;
		float Angle = 0.f;
		float Between = Diameter / Radius;
		for (int32 h = 1; h < Index; ++h)
		{
			Angle += Between;
			if (Angle > 2.f * PI)
			{
				Radius += Diameter;
				Between = Diameter / Radius;
				Angle -= 2.f * PI;
			}
		}
		Pos.X += FMath::Sin(Angle) * Radius;
		Pos.Y += FMath::Cos(Angle) * Radius;
	}
}

void UGenGameLogic::GroupOrderMove(FGenObject& Unit, const FVector3f& Dest)
{
	Unit.ClearAttack();         // a move order overrides the current attack
	Unit.StopConstructing();    // ... and a dozer's building job
	Unit.OnPlayerCommand(false); // ... and a gatherer's supply run
	Unit.CommandMoveTo(Dest);
	SimYield(TEXT("a group's order"));
}

void UGenGameLogic::GroupOrderWaypoint(FGenObject& Unit, const FVector2f& Dest)
{
	// AIUpdateInterface::privateFollowPathAppend: "We're adding a dynamic waypoint!"
	if (Unit.AppendToRoute(Dest))
	{
		return; // "We already have a path, so simply add the point to the end of it!"
	}
	// "Our unit is moving to a point already so simply add our waypoint after that point and convert it to a
	// waypoint command!" - or, idle, it sets off for the point.
	TArray<FVector2f> Points;
	if ((Unit.bMoving || Unit.IsWaitingForPath()) && !Unit.bMovingOutOfWay)
	{
		Points.Add(FVector2f(Unit.Goal.X, Unit.Goal.Y));
	}
	Points.Add(Dest);
	Unit.ClearAttack();
	Unit.StopConstructing();
	Unit.OnPlayerCommand(false);
	Unit.CommandFollowPath(Points);
	SimYield(TEXT("a group's order"));
}

void UGenGameLogic::GroupOrderPath(FGenObject& Unit, const TArray<FVector2f>& Points, int32 EndLayer)
{
	Unit.ClearAttack();
	Unit.StopConstructing();
	Unit.OnPlayerCommand(false);
	Unit.CommandFollowPath(Points);
	// (Pathfinder::updateGoal: the end of the way is its to end the move on, not where it stands now.)
	if (!Unit.bFlies && Points.Num() > 0)
	{
		Pathfinder.UpdateUnitGoal(Unit.ID, Points.Last(), Unit.Template->GetBoundingCircleRadius(), EndLayer);
	}
	SimYield(TEXT("a group's order"));
}

bool UGenGameLogic::GroupGroundPath(const TArray<FGenObject*>& Members, const FVector2f& GroupMin, const FVector2f& GroupMax,
	const FVector2f& GroupCenter, const FVector3f& Goal, bool bForColumns, TArray<FVector2f>& OutPath)
{
	// AIGroup::friend_computeGroundPath
	OutPath.Reset();
	const FGenAIData& AI = FGenAIData::Get();
	const FVector2f Pos(Goal.X, Goal.Y);
	float DistSq = 4.f * FMath::Square(AI.DistanceRequiresGroup);
	int32 NumInfantry = 0, NumVehicles = 0;
	const FGenObject* CenterUnit = nullptr;
	float CenterDistSq = 0.f;
	for (FGenObject* O : Members)
	{
		Pathfinder.RemoveUnitGoal(O->ID);
		if (O->IsKindOf(TEXT("INFANTRY")))
		{
			++NumInfantry;
		}
		else if (O->IsKindOf(TEXT("VEHICLE")))
		{
			if (O->IsKindOf(TEXT("AIRCRAFT")))
			{
				continue;
			}
			++NumVehicles;
		}
		else
		{
			continue;
		}
		// "we are getting the closest of ANY type of unit for later testing intentionally"
		const FVector2f UnitPos(O->Position.X, O->Position.Y);
		DistSq = FMath::Min(DistSq, FVector2f::DistSquared(UnitPos, Pos));
		const float D = FVector2f::DistSquared(UnitPos, GroupCenter);
		if (!CenterUnit || D < CenterDistSq)
		{
			CenterUnit = O;
			CenterDistSq = D;
		}
	}
	if (!CenterUnit)
	{
		return false;
	}
	// (Columns take three of a kind: with fewer the way would be found and thrown away again.)
	if (bForColumns && NumInfantry < AI.MinInfantryForGroup && NumVehicles < AI.MinVehiclesForGroup)
	{
		return false;
	}
	const FVector2f Center(CenterUnit->Position.X, CenterUnit->Position.Y);
	const float SpreadSq = (GroupMax - GroupMin).SizeSquared();
	if (SpreadSq > FMath::Square(AI.DistanceRequiresGroup))
	{
		DistSq = SpreadSq;
	}
	if (DistSq < FMath::Square(AI.MinDistanceForGroup))
	{
		return false; // "Have to be moving at least this far for group movement."
	}
	bool bGroup = DistSq > FMath::Square(AI.DistanceRequiresGroup) || NumInfantry > 6 || NumVehicles > 4;
	if (!bGroup)
	{
		// "see if all units have an unobstructed path to the center. If so, then they are close enough."
		bool bPassable = true;
		for (const FGenObject* O : Members)
		{
			if (O->IsKindOf(TEXT("INFANTRY")) && !Pathfinder.IsLineClear(FVector2f(O->Position.X, O->Position.Y), Center, O->GetPathSurfaces(), O->GetLayer()))
			{
				bPassable = false;
			}
		}
		bGroup = bPassable;
	}
	if (!bGroup)
	{
		return false;
	}
	auto CrushableLevel = [this](FGenObjectID Id)
	{
		const FGenObject* O = FindObject(Id);
		return O ? O->GetTemplate()->CrushableLevel : -1;
	};
	const int32 GoalLayer = Pathfinder.GetLayerForDestination(Goal, GetGroundHeight(Goal.X, Goal.Y));
	const bool bFound = Pathfinder.FindGroundPath(Center, Pos, PathDiameterInCells, false, CrushableLevel, OutPath, CenterUnit->GetLayer(), GoalLayer);
	UE_CLOG(IsTracing(), LogGenerals, Log, TEXT("  [group f%u] way for %d infantry and %d vehicles from (%.0f, %.0f) to (%.0f, %.0f): %s, %d corners%s"),
		Frame, NumInfantry, NumVehicles, Center.X, Center.Y, Pos.X, Pos.Y, bFound ? TEXT("found") : TEXT("none"), OutPath.Num(),
		bFound ? *FString::Printf(TEXT(", ends at (%.0f, %.0f)"), OutPath.Last().X, OutPath.Last().Y) : TEXT(""));
	return bFound;
}

bool UGenGameLogic::GroupMoveColumns(const TArray<FGenObject*>& Members, const FVector2f& GroupCenter, const TArray<FVector2f>& P,
	const FVector3f& Goal, bool bInfantry)
{
	// AIGroup::friend_moveInfantryToPos (three columns, five at the end) and friend_moveVehicleToPos (two, three at the end).
	int32 StartNode, EndNode;
	if (P.Num() < 2 || !FindEndNodes(P, StartNode, EndNode) || (!bInfantry && EndNode == 0))
	{
		return false;
	}
	FVector2f StartVector = (P[StartNode] - P[0]).GetSafeNormal();
	const FVector2f EndVector = (P.Last() - P[EndNode]).GetSafeNormal();
	FVector2f StartNormal(-StartVector.Y, StartVector.X);
	const FVector2f EndNormal(-EndVector.Y, EndVector.X);

	struct FUnit
	{
		FGenObject* O;
		float Key;
		int32 Column;  // which lane on the way (the original's)
		int32 Wide;    // which file at the end
		int32 Lane;    // which lane on the way, 0 = the +normal side (ours: see LaneSpread)
	};
	TArray<FUnit> Units;
	bool bUseEndVector = false;
	for (FGenObject* O : Members)
	{
		if (bInfantry)
		{
			if (!O->IsKindOf(TEXT("INFANTRY")))
			{
				continue;
			}
			if (O->IsKindOf(TEXT("MOB_NEXUS")))
			{
				return false; // "so the nexus will have a far-away goal position for the mobsters to aim at"
			}
		}
		else if (!O->IsKindOf(TEXT("VEHICLE")) || O->bFlies)
		{
			continue;
		}
		Pathfinder.RemoveUnitGoal(O->ID);
		Units.Add({ O, 0.f, 0, 0, 0 });
		// "If units are closer to the end vector than the start vector, use the end vector."
		const FVector2f UnitPos(O->Position.X, O->Position.Y);
		if (FVector2f::DistSquared(UnitPos, P[0]) > FVector2f::DistSquared(UnitPos, P.Last()))
		{
			bUseEndVector = true;
		}
	}
	const FGenAIData& AI = FGenAIData::Get();
	const int32 N = Units.Num();
	if (N < (bInfantry ? AI.MinInfantryForGroup : AI.MinVehiclesForGroup))
	{
		return false;
	}
	if (bUseEndVector)
	{
		StartVector = EndVector;
		StartNormal = EndNormal;
	}

	// (Not the original: it has vehicles keep to two lanes and infantry to three, a fixed 15 or 21 units either
	// side of the way - on a bridge they queued up on one side of the deck. The user asked for movement "like in
	// modern games": as many lanes as the group needs, spread across the room there is where they pass.)
	const int32 NumLanes = bInfantry ? FMath::Clamp(FMath::RoundToInt32(FMath::Sqrt(float(N))), 3, 5)
		: (N <= 3 ? 2 : FMath::Clamp(FMath::RoundToInt32(FMath::Sqrt(float(N))), 2, 4));

	// Left to right across the way: who takes which lane. Then front to back along it.
	for (FUnit& U : Units)
	{
		U.Key = FVector2f::DotProduct(FVector2f(U.O->Position.X, U.O->Position.Y) - GroupCenter, StartNormal);
	}
	Units.StableSort([](const FUnit& A, const FUnit& B) { return A.Key > B.Key; });
	for (int32 i = 0; i < N; ++i)
	{
		FUnit& U = Units[i];
		U.Lane = FMath::Min(NumLanes - 1, i * NumLanes / FMath::Max(N, 1));
		float Adjust = 0.f;
		if (bInfantry)
		{
			U.Column = FMath::Max(1 - i / FMath::Max(1, (N + 1) / 3), -1);   // 1, 0, -1 from left to right
			U.Wide = FMath::Max(2 - i / FMath::Max(1, (N + 3) / 5), -2);     // 2, 1, 0, -1, -2
			if (N < 16)
			{
				U.Wide = U.Column;
			}
			// (Infantry are sorted first by where their kind moves in a group.)
			const int32 Priority = U.O->Locomotor ? U.O->Locomotor->MovePriority : 2;
			Adjust = Priority == 1 ? -100.f * Cell : Priority == 0 ? -200.f * Cell : 0.f;
		}
		else
		{
			U.Column = 1 - i / FMath::Max(1, (N + 1) / 2);
			if (U.Column == 0)
			{
				U.Column = -1;
			}
			U.Wide = FMath::Max(1 - i / FMath::Max(1, (N + 1) / 3), -1);     // 1, 0, -1
			if (N < 5)
			{
				U.Wide = U.Column;
			}
		}
		U.Key = Adjust + FVector2f::DotProduct(FVector2f(U.O->Position.X, U.O->Position.Y) - GroupCenter, StartVector);
	}
	Units.StableSort([](const FUnit& A, const FUnit& B) { return A.Key > B.Key; });

	// "Even out columns by priority."
	int32 Narrow[3] = {}, Wider[5] = {};
	for (int32 Group = 2; Group >= 0; --Group)
	{
		for (FUnit& U : Units)
		{
			if (bInfantry)
			{
				if ((U.O->Locomotor ? U.O->Locomotor->MovePriority : 1) != Group)
				{
					continue;
				}
				int32 Best = PickColumn(Narrow, 3, 1, 1 + U.Column);
				if (Best >= 0) { U.Column = Best - 1; }
				Best = PickColumn(Wider, 5, 1, 2 + U.Wide);
				if (Best >= 0) { U.Wide = Best - 2; }
				if (N < 16)
				{
					U.Wide = U.Column;
				}
			}
			else
			{
				// (Vehicles are not told apart by priority: all of them are gone over each time.)
				int32 Best = PickColumn(Narrow, 3, 2, 1 + U.Column);
				if (Best >= 0) { U.Column = Best - 1; }
				Best = PickColumn(Wider, 3, 1, 1 + U.Wide);
				if (Best >= 0) { U.Wide = Best - 1; }
				if (N < 5)
				{
					U.Wide = U.Column;
				}
			}
		}
	}

	// clampToMap: "Human player has to stay within the visible map" (and nobody leaves the playable ground here).
	FBox2f Extent(FVector2f(-1e9f, -1e9f), FVector2f(1e9f, 1e9f));
	if (const FGenMapData* Map = GetMap())
	{
		Extent = Map->GetPlayableBounds();
		Extent.Min += FVector2f(Cell, Cell);
		Extent.Max -= FVector2f(Cell, Cell);
	}
	auto ClampToMap = [&Extent](FVector2f& Dest)
	{
		Dest.X = FMath::Clamp(Dest.X, Extent.Min.X, Extent.Max.X);
		Dest.Y = FMath::Clamp(Dest.Y, Extent.Min.Y, Extent.Max.Y);
	};

	const int32 GoalLayer = Pathfinder.GetLayerForDestination(Goal, GetGroundHeight(Goal.X, Goal.Y));

	// How the lanes lie at a corner of the way: centred in the room there is across it (on whichever surface -
	// the ground or a bridge deck - gives the most), a body's width off the edges, as far apart as they should be
	// or as the room allows.
	const uint32 Surfaces = Units[0].O->GetPathSurfaces();
	float Radius = 0.f;
	for (const FUnit& U : Units)
	{
		Radius = FMath::Max(Radius, U.O->Template->GetBoundingCircleRadius());
	}
	const float WantSpacing = bInfantry ? 2.1f * Cell : FMath::Max(3.2f * Cell, 2.4f * Radius);
	const float MinSpacing = bInfantry ? 1.0f * Cell : FMath::Max(1.6f * Cell, 1.2f * Radius);
	TMap<int32, TPair<float, float>> SpreadAt; // (by node: centre, spacing)
	auto LaneSpread = [&](int32 NodeIndex, const FVector2f& At, const FVector2f& Normal, float& OutCenter, float& OutSpacing)
	{
		if (const TPair<float, float>* Known = SpreadAt.Find(NodeIndex))
		{
			OutCenter = Known->Key;
			OutSpacing = Known->Value;
			return;
		}
		const float Step = 0.5f * Cell, MaxProbe = 9.f * Cell;
		float BestLeft = 0.f, BestRight = 0.f;
		bool bAny = false;
		for (int32 Layer = 0; Layer <= Pathfinder.GetNumLayers(); ++Layer)
		{
			if (!Pathfinder.IsPassable(At, Surfaces, Layer))
			{
				continue;
			}
			float Left = 0.f, Right = 0.f;
			while (Left < MaxProbe && Pathfinder.IsPassable(At + Normal * (Left + Step), Surfaces, Layer)) { Left += Step; }
			while (Right < MaxProbe && Pathfinder.IsPassable(At - Normal * (Right + Step), Surfaces, Layer)) { Right += Step; }
			if (!bAny || Left + Right > BestLeft + BestRight)
			{
				BestLeft = Left;
				BestRight = Right;
				bAny = true;
			}
		}
		OutCenter = 0.f;
		OutSpacing = WantSpacing;
		if (bAny)
		{
			const float Lo = -BestRight + Radius, Hi = BestLeft - Radius;
			OutCenter = Hi > Lo ? 0.5f * (Lo + Hi) : 0.5f * (BestLeft - BestRight);
			OutSpacing = NumLanes > 1 ? FMath::Clamp((Hi - Lo) / float(NumLanes - 1), MinSpacing, WantSpacing) : 0.f;
		}
		SpreadAt.Add(NodeIndex, TPair<float, float>(OutCenter, OutSpacing));
	};

	int32 ColumnFactor[7] = {};
	for (FUnit& U : Units)
	{
		const int32 Factor = ColumnFactor[U.Wide + 3]++;
		TArray<FVector2f> Path;
		int32 Node = StartNode, Previous = 0;
		FVector2f PrevPos(U.O->Position.X, U.O->Position.Y);
		while (P.IsValidIndex(Node))
		{
			FVector2f Dest = P[Node];
			int32 Next = INDEX_NONE;
			for (int32 T = Node + 1; T < P.Num(); ++T)
			{
				if (FVector2f::DistSquared(P[T], Dest) > FarEnoughSq)
				{
					Next = T;
					break;
				}
			}
			if (Next == INDEX_NONE)
			{
				break;
			}
			// The lane runs beside the way: off to the side of the line from the node before to the node after.
			const FVector2f Corner = P[Next] - P[Previous];
			const FVector2f CornerNormal = FVector2f(-Corner.Y, Corner.X).GetSafeNormal();
			float Center = 0.f, Spacing = WantSpacing;
			LaneSpread(Node, P[Node], CornerNormal, Center, Spacing);
			Dest += CornerNormal * (Center + (0.5f * float(NumLanes - 1) - float(U.Lane)) * Spacing);
			Dest += CornerNormal * ((Factor & 1) ? 0.25f * Cell : -0.25f * Cell);
			const FVector2f Cur = Dest - PrevPos;
			ClampToMap(Dest);
			// "Make sure that this dest is going in the same direction as the vector."
			if (FVector2f::DotProduct(Corner, Cur) > 0.f)
			{
				Path.Add(Dest);
				PrevPos = Dest;
			}
			++Node;
			for (int32 T = Previous + 1; T < P.Num() && T != Node; ++T)
			{
				if (FVector2f::DistSquared(P[T], P[Node]) > FarEnoughSq)
				{
					Previous = T;
				}
			}
		}

		// Its place at the end: files abreast across the way, rank behind rank.
		FVector2f Dest(Goal.X, Goal.Y);
		float Offset;
		if (bInfantry)
		{
			Offset = Cell * 2.2f;
			Dest += EndNormal * (Offset * float(FMath::Clamp(U.Wide, -2, 2)));
			if (Factor & 1)
			{
				Dest += EndNormal * Cell;
			}
			const int32 Priority = U.O->Locomotor ? U.O->Locomotor->MovePriority : 1;
			Dest += EndVector * (float(Priority - 2) * Cell);
		}
		else
		{
			Offset = N < 5 ? Cell * 1.5f : Cell * 3.2f;
			Dest += EndNormal * (Offset * float(FMath::Clamp(U.Wide, -3, 3)));
			if (Factor & 1)
			{
				Dest += EndNormal * Cell;
			}
		}
		Dest -= EndVector * (float(Factor) * Offset);
		while (Path.Num() > 0 && FVector2f::DotProduct(EndVector, Dest - Path.Last()) <= 0.f)
		{
			Path.Pop(EAllowShrinking::No);
		}
		ClampToMap(Dest);
		Pathfinder.AdjustUnitDestination(U.O->ID, U.O->Template->GetBoundingCircleRadius(), U.O->GetPathSurfaces(), Dest, GoalLayer);
		Path.Add(Dest);
		UE_CLOG(IsTracing(), LogGenerals, Log, TEXT("  [group f%u] #%u %s: lane %d of %d, file %d, rank %d - %d points to (%.0f, %.0f)"),
			Frame, U.O->ID, *U.O->Template->Name, U.Lane, NumLanes, U.Wide, Factor, Path.Num(), Dest.X, Dest.Y);
		GroupOrderPath(*U.O, Path, GoalLayer);
	}
	return true;
}

void UGenGameLogic::GroupMoveFormation(const TArray<FGenObject*>& Members, const TArray<FVector2f>& P, const FVector3f& Goal)
{
	// AIGroup::friend_moveFormationToPos: every member the same way, shifted by its place in the formation.
	int32 StartNode = INDEX_NONE, EndNode = INDEX_NONE;
	FVector2f EndPoint(Goal.X, Goal.Y);
	if (P.Num() >= 2)
	{
		EndPoint = P.Last();
		if (!FindEndNodes(P, StartNode, EndNode) || StartNode >= EndNode)
		{
			StartNode = EndNode = INDEX_NONE;
		}
	}
	// AIGroup::getSpeed: "speed is slowest speed ... we won't consider slow speeds from objects that are
	// being penalized by their damage state" (those fall behind instead).
	float Speed = 1e10f;
	for (const FGenObject* O : Members)
	{
		if (O->Locomotor && O->GetDamageState() < 2)
		{
			Speed = FMath::Min(Speed, O->Locomotor->Speed);
		}
	}
	const int32 GoalLayer = Pathfinder.GetLayerForDestination(Goal, GetGroundHeight(Goal.X, Goal.Y));
	for (FGenObject* O : Members)
	{
		const FVector2f Offset = O->FormationOffset;
		O->FormationSpeed = Speed;
		FVector2f Dest = EndPoint + Offset;
		if (StartNode != INDEX_NONE && !O->bFlies)
		{
			TArray<FVector2f> Path;
			for (int32 Node = StartNode; Node <= EndNode; ++Node)
			{
				Path.Add(P[Node] + Offset);
			}
			Pathfinder.AdjustUnitDestination(O->ID, O->Template->GetBoundingCircleRadius(), O->GetPathSurfaces(), Dest, GoalLayer);
			Path.Add(Dest);
			GroupOrderPath(*O, Path, GoalLayer);
		}
		else
		{
			GroupOrderMove(*O, FVector3f(Dest.X, Dest.Y, Pathfinder.GetLayerHeight(Dest.X, Dest.Y, GoalLayer, GetGroundHeight(Dest.X, Dest.Y))));
		}
		UE_CLOG(IsTracing(), LogGenerals, Log, TEXT("  [group f%u] #%u %s: formation %u, place (%.0f, %.0f), to (%.0f, %.0f) at speed %.2f"),
			Frame, O->ID, *O->Template->Name, O->FormationID, Offset.X, Offset.Y, Dest.X, Dest.Y, Speed);
	}
}

void UGenGameLogic::GroupTighten(const TArray<FGenObject*>& Members, const FVector3f& Goal)
{
	// AIGroup::groupTightenToPosition: all of them make for the spot itself, the nearest first ("works
	// better if you let the near units get the first paths") - each ends on the free ground nearest to it.
	TArray<FGenObject*> Sorted = Members;
	const FVector2f Pos(Goal.X, Goal.Y);
	Sorted.StableSort([&Pos](const FGenObject& A, const FGenObject& B)
	{
		return FVector2f::DistSquared(FVector2f(A.GetPosition().X, A.GetPosition().Y), Pos) < FVector2f::DistSquared(FVector2f(B.GetPosition().X, B.GetPosition().Y), Pos);
	});
	int32 HelicopterIndex = 0;
	for (FGenObject* O : Sorted)
	{
		Pathfinder.RemoveUnitGoal(O->ID);
	}
	for (FGenObject* O : Sorted)
	{
		FVector3f Dest = Goal;
		if (O->IsKindOf(TEXT("PRODUCED_AT_HELIPAD")))
		{
			HelicopterOffset(Dest, HelicopterIndex++);
		}
		GroupOrderMove(*O, Dest);
	}
	UE_CLOG(IsTracing(), LogGenerals, Log, TEXT("  [group f%u] %d units gather at (%.0f, %.0f)"), Frame, Sorted.Num(), Goal.X, Goal.Y);
}

void UGenGameLogic::GroupMoveToPosition(const TArray<FGenObjectID>& Units, const FVector3f& InGoal, bool bFromPlayer, bool bAddWaypoint)
{
	GEN_PERF_SCOPE("Logic.GroupMove");
	// Who counts: alive, out in the world (riders are not told to move) and able to move at all.
	TArray<FGenObject*> Members;
	for (FGenObjectID Id : Units)
	{
		FGenObject* O = FindObject(Id);
		if (O && O->CanMove() && !O->IsDead() && !O->IsContained())
		{
			Members.Add(O);
		}
	}
	if (Members.Num() == 0)
	{
		return;
	}
	FVector3f Goal = InGoal;

	// AIGroup::getMinMaxAndCenter. (As the original has it, the group is a formation if its first member is in one.)
	FVector2f Min(1e10f, 1e10f), Max(-1e10f, -1e10f), Center(0.f, 0.f);
	for (const FGenObject* O : Members)
	{
		const FVector2f P(O->Position.X, O->Position.Y);
		Center += P;
		Min = FVector2f::Min(Min, P);
		Max = FVector2f::Max(Max, P);
	}
	Center /= float(Members.Num());
	// ("if (addWaypoint) isFormation = false": a waypoint is no formation move, nor a march in columns.)
	bool bFormation = !bAddWaypoint && Members.Num() >= 2 && Members[0]->FormationID != 0;

	TArray<FVector2f> GroundPath;
	bool bDidInfantry = false, bDidVehicles = false;
	if (!bFormation && !bAddWaypoint && GroupGroundPath(Members, Min, Max, Center, Goal, true, GroundPath))
	{
		bDidInfantry = GroupMoveColumns(Members, Center, GroundPath, Goal, true);
		bDidVehicles = GroupMoveColumns(Members, Center, GroundPath, Goal, false);
	}

	// A click inside the group's own area (GroupMoveClickToGatherAreaFactor of it) gathers the group in.
	bool bTighten = false;
	const float GatherFactor = FGenGameData::Get().GroupMoveClickToGatherFactor;
	if (!bFormation && bFromPlayer && GatherFactor > 0.f)
	{
		const FVector2f Grow = (Max - Min) * ((GatherFactor - 1.f) * 0.5f); // ScaleRect2D
		const FVector2f Lo = Min - Grow, Hi = Max + Grow;
		bTighten = Goal.X >= Lo.X && Goal.X <= Hi.X && Goal.Y >= Lo.Y && Goal.Y <= Hi.Y;
	}
	float ExtraMargin = 0.f;
	for (const FGenObject* O : Members)
	{
		if (O->IsKindOf(TEXT("PRODUCED_AT_HELIPAD")))
		{
			bFormation = false;
			ExtraMargin = FMath::Max(ExtraMargin, O->Template->GeometryMajorRadius);
		}
		else if (O->IsKindOf(TEXT("AIRCRAFT")))
		{
			if (O->bFlies)
			{
				bTighten = false;   // "Don't tighten aircraft. It is a bad idea."
				bFormation = false; // "then keep spread formation after move"
			}
			ExtraMargin = FMath::Max(ExtraMargin, 10.f * Cell); // STD_AIRCRAFT_EXTRA_MARGIN
		}
	}
	// clampWaypointPosition: not right at the map's edge (STD_WAYPOINT_CLAMP_MARGIN).
	if (const FGenMapData* Map = GetMap())
	{
		const FBox2f Extent = Map->GetPlayableBounds();
		const float Margin = float(int32(4.f * Cell + ExtraMargin));
		const float X = FMath::Clamp(Goal.X, Extent.Min.X + Margin, FMath::Max(Extent.Min.X + Margin, Extent.Max.X - Margin));
		const float Y = FMath::Clamp(Goal.Y, Extent.Min.Y + Margin, FMath::Max(Extent.Min.Y + Margin, Extent.Max.Y - Margin));
		if (X != Goal.X || Y != Goal.Y)
		{
			Goal = FVector3f(X, Y, GetGroundHeight(X, Y));
		}
	}

	if (bTighten)
	{
		bFormation = false;
		// ("Int dy = (max.x-min.x)/PATHFIND_CELL_SIZE_F": both are the group's width, as written.)
		const int32 DX = int32((Max.X - Min.X) / Cell);
		if (!bAddWaypoint && DX * DX < 2000)
		{
			GroupTighten(Members, Goal);
			return;
		}
	}
	if (bFormation)
	{
		GroupGroundPath(Members, Min, Max, Center, Goal, false, GroundPath);
		GroupMoveFormation(Members, GroundPath, Goal);
		return;
	}

	// Everybody else: to the place he holds in the group, moved over to where the order points.
	TArray<FGenObject*> Rest;
	for (FGenObject* O : Members)
	{
		if (O->IsKindOf(TEXT("INFANTRY")) && bDidInfantry)
		{
			continue;
		}
		if (O->IsKindOf(TEXT("VEHICLE")) && bDidVehicles && !O->bFlies && !O->IsKindOf(TEXT("CLIFF_JUMPER")))
		{
			continue;
		}
		if (!bAddWaypoint)
		{
			Pathfinder.RemoveUnitGoal(O->ID);
		}
		Rest.Add(O);
	}
	// "Works better if you let the near units get the first paths... Move the ones nearest the goal first.
	// Reduces collision problems later."
	const FVector2f Pos(Goal.X, Goal.Y);
	Rest.StableSort([&Pos](const FGenObject& A, const FGenObject& B)
	{
		return FVector2f::DistSquared(FVector2f(A.GetPosition().X, A.GetPosition().Y), Pos) < FVector2f::DistSquared(FVector2f(B.GetPosition().X, B.GetPosition().Y), Pos);
	});
	const int32 GoalLayer = Pathfinder.GetLayerForDestination(Goal, GetGroundHeight(Goal.X, Goal.Y));
	FVector2f GroupCenter = Center;
	for (int32 i = 0; i < Rest.Num(); ++i)
	{
		FGenObject* O = Rest[i];
		O->FormationID = 0;
		O->FormationSpeed = 0.f;
		const FVector2f UnitPos(O->Position.X, O->Position.Y);
		if (i == 0)
		{
			GroupCenter = UnitPos; // the nearest one goes to the very spot; the others keep their places relative to it
		}
		// AIGroup::computeIndividualDestination: "compute vector from 'group center' to self" - no further
		// out than six times its own size - "move to same offset at destination".
		FVector2f V = UnitPos - GroupCenter;
		const float Length = FMath::Min(V.Size(), 6.f * O->Template->GetBoundingCircleRadius());
		V = V.GetSafeNormal() * Length;
		FVector2f Dest = Pos + V;
		if (bAddWaypoint)
		{
			// (The spot is only claimed when the unit sets off for it: the way it is going now keeps its own.)
			if (!O->bFlies)
			{
				Pathfinder.AdjustUnitDestination(O->ID, O->Template->GetBoundingCircleRadius(), O->GetPathSurfaces(), Dest, GoalLayer);
			}
			GroupOrderWaypoint(*O, Dest);
			continue;
		}
		if (!O->bFlies)
		{
			Pathfinder.AdjustUnitDestination(O->ID, O->Template->GetBoundingCircleRadius(), O->GetPathSurfaces(), Dest, GoalLayer);
			Pathfinder.UpdateUnitGoal(O->ID, Dest, O->Template->GetBoundingCircleRadius(), GoalLayer);
		}
		GroupOrderMove(*O, FVector3f(Dest.X, Dest.Y, Pathfinder.GetLayerHeight(Dest.X, Dest.Y, GoalLayer, GetGroundHeight(Dest.X, Dest.Y))));
	}
}

void UGenGameLogic::IssueCreateFormation(const TArray<FGenObjectID>& Units)
{
	FGenCommand C(EGenCommandType::CreateFormation);
	C.Units = Units;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecCreateFormation(const FGenCommand& C)
{
	// AIGroup::groupCreateFormation: every member keeps the place it holds now, relative to the middle
	// of the group - or, given to a group that is a formation already, no longer.
	TArray<FGenObject*> Members;
	FVector2f Center(0.f, 0.f);
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* O = FindObject(Id);
		if (O && O->CanMove() && !O->IsDead() && !O->IsContained())
		{
			Members.Add(O);
			Center += FVector2f(O->Position.X, O->Position.Y);
		}
	}
	if (Members.Num() == 0)
	{
		return;
	}
	Center /= float(Members.Num());
	const bool bIsFormation = (Members.Num() >= 2 && Members[0]->FormationID != 0) || (Members.Num() == 1 && Members[0]->FormationID != 0);
	const uint32 NewId = NextFormationID++;
	for (FGenObject* O : Members)
	{
		O->FormationID = bIsFormation ? 0 : NewId;
		O->FormationOffset = FVector2f(O->Position.X, O->Position.Y) - Center;
		O->FormationSpeed = 0.f;
	}
	UE_CLOG(IsTracing(), LogGenerals, Log, TEXT("  [group f%u] %d units %s"), Frame, Members.Num(),
		bIsFormation ? TEXT("are a formation no longer") : *FString::Printf(TEXT("are formation %u"), NewId));
}
