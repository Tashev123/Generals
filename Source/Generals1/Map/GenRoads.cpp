#include "Map/GenRoads.h"
#include "Core/GenMeshUtil.h"
#include "Core/GenCoords.h"
#include "Core/GenIni.h"
#include "Map/GenMapData.h"
#include "Map/GenTerrain.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

// ---------------------------------------------------------------------------------------------
// Roads.ini
// ---------------------------------------------------------------------------------------------

namespace
{
	/** "R:192 G:192 B:192" */
	FColor RoadParseColor(const FString& Raw)
	{
		FColor Out(192, 192, 192);
		TArray<FString> Tokens;
		Raw.ParseIntoArrayWS(Tokens);
		for (const FString& T : Tokens)
		{
			if (T.Len() < 3 || T[1] != TEXT(':'))
			{
				continue;
			}
			const uint8 V = uint8(FMath::Clamp(FCString::Atoi(*T.Mid(2)), 0, 255));
			switch (FChar::ToUpper(T[0]))
			{
			case TEXT('R'): Out.R = V; break;
			case TEXT('G'): Out.G = V; break;
			case TEXT('B'): Out.B = V; break;
			default: break;
			}
		}
		return Out;
	}
}

const FGenTerrainRoads& FGenTerrainRoads::Get()
{
	static FGenTerrainRoads Instance;
	if (!Instance.bLoaded)
	{
		Instance.Load();
	}
	return Instance;
}

void FGenTerrainRoads::Load()
{
	bLoaded = true;
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Default\\Roads.ini"), Blocks);
	GenIni::ParseFile(TEXT("Data\\INI\\Roads.ini"), Blocks);
	int32 IdCounter = 1; // "MUST start this at 1"
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (B->Keyword.Equals(TEXT("Road"), ESearchCase::IgnoreCase))
		{
			FGenRoadType* R = Roads.FindByPredicate([&](const FGenRoadType& X) { return X.Name == B->Name(); });
			if (!R)
			{
				R = &Roads.AddDefaulted_GetRef();
				R->Name = B->Name();
				R->Id = IdCounter++;
			}
			if (B->Find(TEXT("Texture"))) { R->Texture = B->GetString(TEXT("Texture")); }
			if (B->Find(TEXT("RoadWidth"))) { R->Width = B->GetFloat(TEXT("RoadWidth")); }
			if (B->Find(TEXT("RoadWidthInTexture"))) { R->WidthInTexture = B->GetFloat(TEXT("RoadWidthInTexture")); }
		}
		else if (B->Keyword.Equals(TEXT("Bridge"), ESearchCase::IgnoreCase))
		{
			FGenBridgeType* R = Bridges.FindByPredicate([&](const FGenBridgeType& X) { return X.Name == B->Name(); });
			if (!R)
			{
				R = &Bridges.AddDefaulted_GetRef();
				R->Name = B->Name();
				R->Id = IdCounter++;
			}
			if (B->Find(TEXT("BridgeScale"))) { R->Scale = B->GetFloat(TEXT("BridgeScale"), 1.f); }
			if (const FGenIniEntry* E = B->Find(TEXT("RadarColor"))) { R->RadarColor = RoadParseColor(E->Raw); }
			if (B->Find(TEXT("BridgeModelName"))) { R->Model = B->GetString(TEXT("BridgeModelName")); }
			if (B->Find(TEXT("Texture"))) { R->Texture = B->GetString(TEXT("Texture")); }
			if (B->Find(TEXT("BridgeModelNameDamaged"))) { R->ModelDamaged = B->GetString(TEXT("BridgeModelNameDamaged")); }
			if (B->Find(TEXT("TextureDamaged"))) { R->TextureDamaged = B->GetString(TEXT("TextureDamaged")); }
			if (B->Find(TEXT("BridgeModelNameReallyDamaged"))) { R->ModelReallyDamaged = B->GetString(TEXT("BridgeModelNameReallyDamaged")); }
			if (B->Find(TEXT("TextureReallyDamaged"))) { R->TextureReallyDamaged = B->GetString(TEXT("TextureReallyDamaged")); }
			if (B->Find(TEXT("BridgeModelNameBroken"))) { R->ModelBroken = B->GetString(TEXT("BridgeModelNameBroken")); }
			if (B->Find(TEXT("TextureBroken"))) { R->TextureBroken = B->GetString(TEXT("TextureBroken")); }
			if (B->Find(TEXT("TowerObjectNameFromLeft"))) { R->TowerFromLeft = B->GetString(TEXT("TowerObjectNameFromLeft")); }
			if (B->Find(TEXT("TowerObjectNameFromRight"))) { R->TowerFromRight = B->GetString(TEXT("TowerObjectNameFromRight")); }
			if (B->Find(TEXT("TowerObjectNameToLeft"))) { R->TowerToLeft = B->GetString(TEXT("TowerObjectNameToLeft")); }
			if (B->Find(TEXT("TowerObjectNameToRight"))) { R->TowerToRight = B->GetString(TEXT("TowerObjectNameToRight")); }
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Roads.ini: %d road types, %d bridge types"), Roads.Num(), Bridges.Num());
}

const FGenRoadType* FGenTerrainRoads::FindRoad(const FString& Name) const
{
	return Roads.FindByPredicate([&](const FGenRoadType& X) { return X.Name == Name; });
}

const FGenBridgeType* FGenTerrainRoads::FindBridge(const FString& Name) const
{
	return Bridges.FindByPredicate([&](const FGenBridgeType& X) { return X.Name == Name; });
}

// ---------------------------------------------------------------------------------------------
// The layout: W3DRoadBuffer::loadRoads up to (not including) the vertex buffers
// ---------------------------------------------------------------------------------------------

namespace
{
	constexpr float RoadTeeWidthAdjustment = 1.03f;
	constexpr float RoadCornerRadius = 1.5f;        // "outer radius 2.0, inner radius 1.0"
	constexpr float RoadTightCornerRadius = 0.5f;   // "outer radius 1.0, inner radius 0.0"
	constexpr float RoadDefaultScale = 8.f;
	constexpr float RoadMinSegment = 0.25f;
	constexpr int32 RoadMaxSegments = 4000;         // GameData.ini MaxRoadSegments
	constexpr int32 RoadFlagAngled = 0x8;           // FLAG_ROAD_CORNER_ANGLED
	constexpr int32 RoadFlagTight = 0x40;           // FLAG_ROAD_CORNER_TIGHT
	constexpr int32 RoadFlagJoin = 0x80;            // FLAG_ROAD_JOIN

	/** TCorner: also the order the pieces of one road type are drawn in. */
	enum ERoadCorner : uint8 { RC_Segment, RC_Curve, RC_Tee, RC_FourWay, RC_ThreeWayY, RC_ThreeWayH, RC_ThreeWayHFlip, RC_AlphaJoin, RC_Num };
	enum { RoadBL, RoadBR, RoadTL, RoadTR };

	using FRoadVec = FVector2f;

	FRoadVec RoadNormalized(FRoadVec V)
	{
		const float L2 = V.X * V.X + V.Y * V.Y;
		if (L2 != 0.f)
		{
			const float Inv = 1.f / FMath::Sqrt(L2);
			V.X *= Inv;
			V.Y *= Inv;
		}
		return V;
	}

	float RoadLength(const FRoadVec& V) { return FMath::Sqrt(V.X * V.X + V.Y * V.Y); }
	float RoadDot(const FRoadVec& A, const FRoadVec& B) { return A.X * B.X + A.Y * B.Y; }
	float RoadCross(const FRoadVec& A, const FRoadVec& B) { return A.X * B.Y - A.Y * B.X; }

	/** Vector2::Rotate */
	FRoadVec RoadRotated(const FRoadVec& V, float Theta)
	{
		const float S = FMath::Sin(Theta), C = FMath::Cos(Theta);
		return FRoadVec(V.X * C - V.Y * S, V.X * S + V.Y * C);
	}

	/** "-1 means v2 is to the right of v1, 1 means v2 is to the left of v1, and 0 means v2 is parallel to v1" */
	int32 RoadXpSign(const FRoadVec& V1, const FRoadVec& V2)
	{
		const float X = RoadCross(V1, V2);
		return X < 0.f ? -1 : (X > 0.f ? 1 : 0);
	}

	/** LineSegClass, flat. */
	struct FRoadLine
	{
		FRoadVec P0, P1, Dir;
		float Length = 0.f;

		FRoadLine(const FRoadVec& A, const FRoadVec& B) { Set(A, B); }
		void Set(const FRoadVec& A, const FRoadVec& B)
		{
			P0 = A;
			P1 = B;
			Length = RoadLength(B - A);
			Dir = RoadNormalized(B - A);
		}
		/** Find_Intersection: where the two (endless) lines cross - the point on this one. False if parallel. */
		bool Intersect(const FRoadLine& Other, FRoadVec& Out) const
		{
			const float C = RoadCross(Dir, Other.Dir);
			if (C * C == 0.f)
			{
				return false;
			}
			Out = P0 + Dir * (RoadCross(Other.P0 - P0, Other.Dir) / C);
			return true;
		}
		/** Find_Point_Closest_To */
		FRoadVec ClosestPoint(const FRoadVec& Pos) const
		{
			const float D = RoadDot(Dir, Pos - P0);
			if (D <= 0.f) { return P0; }
			if (D >= Length) { return P1; }
			return P0 + Dir * D;
		}
	};

	/** TRoadPt: one end of a segment. Top and bottom are the corners of the road's width there. */
	struct FRoadPt
	{
		FRoadVec Loc = FRoadVec::ZeroVector;
		FRoadVec Top = FRoadVec::ZeroVector;
		FRoadVec Bottom = FRoadVec::ZeroVector;
		int32 Count = 0;       // how many other segments of the kind end here
		bool bAngled = false;
		bool bJoin = false;
	};

	/** RoadSegment */
	struct FRoadSeg
	{
		FRoadPt Pt1, Pt2;
		float CurveRadius = 0.f;
		uint8 Type = RC_Segment;
		float Scale = 1.f;
		float WidthInTexture = 1.f;
		int32 UniqueID = 0;
	};

	/** m_roadTypes[]: a road type with a place in the drawing order. */
	struct FRoadTypeSlot
	{
		const FGenRoadType* Def = nullptr;
		int32 UniqueID = -1;
		int32 Stacking = 0;
	};

	/** What loadFloat4PtSection is handed: a quad on the map and how the texture lies on it. */
	struct FRoadPiece
	{
		FRoadVec Loc;            // where u = UOffset and v = VOffset
		FRoadVec Normal, Vector; // unit vectors across and along
		FRoadVec Corners[4];
		float UOffset = 0.f, VOffset = 0.f, UScale = 1.f, VScale = 1.f;
		int32 TypeSlot = 0;
		uint8 Corner = RC_Segment;
		int32 Layer = 0;
		FBox2f Bounds = FBox2f(ForceInit);
	};

	class FRoadLayout
	{
	public:
		/** A fixed array, as in the original: the code keeps pointers into it while it appends. */
		TArray<FRoadSeg> Roads;
		int32 NumRoads = 0;
		TArray<FRoadTypeSlot> Types;

		void Load(const FGenMapData& Map)
		{
			const FGenTerrainRoads& Defs = FGenTerrainRoads::Get();
			Roads.SetNum(RoadMaxSegments + 1);
			// The road list is built by putting each new one in front: the last of the file comes first.
			for (int32 i = Defs.GetRoads().Num() - 1; i >= 0; --i)
			{
				FRoadTypeSlot& S = Types.AddDefaulted_GetRef();
				S.Def = &Defs.GetRoads()[i];
				S.UniqueID = S.Def->Id;
			}
			AddMapObjects(Map, Defs);
			UpdateCountsAndFlags();
			InsertTeeIntersections();
			InsertCurveSegments();
			InsertCrossTypeJoins();
		}

		/** The preload* / load* functions: the quad and the piece of texture for a segment. */
		bool MakePiece(const FRoadSeg& R, FRoadPiece& P) const
		{
			const FRoadVec Loc1 = R.Pt1.Loc, Loc2 = R.Pt2.Loc;
			const float Scale = R.Scale;
			FRoadVec RoadVector = Loc2 - Loc1;
			FRoadVec RoadNormal(-RoadVector.Y, RoadVector.X);
			P.Loc = Loc1;
			P.UScale = Scale;
			P.VScale = Scale;
			P.Corner = R.Type;
			switch (R.Type)
			{
			case RC_Segment: // preloadRoadSegment
				P.UOffset = 0.f;
				P.VOffset = 85.f / 512.f;
				P.Corners[RoadBL] = R.Pt1.Bottom;
				P.Corners[RoadTL] = R.Pt1.Top;
				P.Corners[RoadBR] = R.Pt2.Bottom;
				P.Corners[RoadTR] = R.Pt2.Top;
				break;

			case RC_Curve: // loadCurve
			{
				P.UOffset = 4.f / 512.f;
				P.VOffset = (R.CurveRadius == RoadTightCornerRadius) ? 425.f / 512.f : 255.f / 512.f;
				const float CurveHeight = R.WidthInTexture * Scale / 2.f;
				RoadVector = RoadNormalized(RoadVector) * Scale;
				RoadNormal = RoadNormalized(RoadNormal) * FMath::Abs(CurveHeight);
				FRoadVec* C = P.Corners;
				C[RoadBL] = Loc1 - RoadNormal;
				if (R.CurveRadius == RoadTightCornerRadius)
				{
					C[RoadBR] = C[RoadBL] + RoadVector * 0.5f;
					C[RoadTR] = C[RoadBR] + RoadNormal * 2.f;
					C[RoadTL] = C[RoadBL] + RoadNormal * 2.f;
					// "Tweak a little"
					C[RoadBR] += RoadVector * 0.1f;
					C[RoadBR] += RoadNormal * 0.2f;
					C[RoadBL] -= RoadNormal * 0.1f;
					C[RoadBL] -= RoadVector * 0.02f;
					C[RoadTL] -= RoadVector * 0.02f;
					C[RoadTR] -= RoadVector * 0.4f;
					C[RoadTR] += RoadNormal * 0.2f;
				}
				else
				{
					C[RoadBR] = C[RoadBL] + RoadVector;
					C[RoadTR] = C[RoadBR] + RoadNormal * 2.f;
					C[RoadTL] = C[RoadBL] + RoadNormal * 2.f;
					C[RoadBR] += RoadVector * 0.1f;
					C[RoadBR] += RoadNormal * 0.4f;
					C[RoadBL] -= RoadNormal * 0.2f;
					C[RoadBL] -= RoadVector * 0.02f;
					C[RoadTL] -= RoadVector * 0.02f;
					C[RoadTR] -= RoadVector * 0.4f;
					C[RoadTR] += RoadNormal * 0.4f;
				}
				break;
			}

			case RC_Tee:
			case RC_FourWay: // loadTee -> loadFloatSection
			{
				P.UOffset = 425.f / 512.f;
				P.VOffset = (R.Type == RC_FourWay) ? 425.f / 512.f : 255.f / 512.f;
				const float TeeFactor = Scale * RoadTeeWidthAdjustment / 2.f;
				const float Left = R.WidthInTexture * Scale / 2.f;
				RoadVector = RoadNormalized(RoadVector) * TeeFactor;
				RoadNormal = RoadNormalized(RoadNormal) * TeeFactor;
				const FRoadVec RoadLeft = RoadNormalized(RoadVector) * Left;
				RoadVector += RoadLeft;
				const FRoadVec LeftCenter = Loc1 - RoadLeft;
				P.Corners[RoadBL] = LeftCenter - RoadNormal;
				P.Corners[RoadBR] = P.Corners[RoadBL] + RoadVector;
				P.Corners[RoadTR] = P.Corners[RoadBR] + RoadNormal * 2.f;
				P.Corners[RoadTL] = P.Corners[RoadBL] + RoadNormal * 2.f;
				break;
			}

			case RC_ThreeWayY: // loadY
			{
				UnitVectors(RoadVector, RoadNormal);
				P.UOffset = 255.f / 512.f;
				P.VOffset = 226.f / 512.f;
				RoadVector *= Scale;
				RoadNormal *= Scale;
				RoadVector *= 1.59f;
				P.Corners[RoadTL] = Loc1 + RoadNormal * 0.29f - RoadVector * 0.5f;
				P.Corners[RoadBL] = P.Corners[RoadTL] - RoadNormal * 1.08f;
				P.Corners[RoadBR] = P.Corners[RoadBL] + RoadVector;
				P.Corners[RoadTR] = P.Corners[RoadTL] + RoadVector;
				break;
			}

			case RC_ThreeWayH:
			case RC_ThreeWayHFlip: // loadH
			{
				const bool bFlip = R.Type == RC_ThreeWayHFlip;
				UnitVectors(RoadVector, RoadNormal);
				P.UOffset = 202.f / 512.f;
				P.VOffset = 364.f / 512.f;
				RoadVector *= Scale;
				RoadNormal *= Scale;
				RoadNormal *= 1.35f;
				P.Corners[RoadBL] = Loc1 - RoadNormal * (bFlip ? 0.20f : 0.8f) - RoadVector * R.WidthInTexture / 2.f;
				const FRoadVec Width = RoadVector * R.WidthInTexture / 2.f + RoadVector * 1.2f;
				P.Corners[RoadBR] = P.Corners[RoadBL] + Width;
				P.Corners[RoadTR] = P.Corners[RoadBR] + RoadNormal;
				P.Corners[RoadTL] = P.Corners[RoadBL] + RoadNormal;
				if (bFlip)
				{
					RoadNormal = -RoadNormal;
				}
				break;
			}

			case RC_AlphaJoin: // loadAlphaJoin
			{
				UnitVectors(RoadVector, RoadNormal);
				P.UOffset = 106.f / 512.f;
				P.VOffset = 425.f / 512.f;
				const float UScale = R.WidthInTexture; // (for these it holds a width in world units)
				RoadVector *= Scale * 48.f / 128.f;               // "we just want 48 pixels"
				RoadNormal *= UScale * (1.f + 8.f / 128.f);       // "we want 8 extra pixels"
				P.Corners[RoadTL] = Loc1 + RoadNormal * 0.5f - RoadVector * 0.65f;
				P.Corners[RoadBL] = P.Corners[RoadTL] - RoadNormal;
				P.Corners[RoadBR] = P.Corners[RoadBL] + RoadVector;
				P.Corners[RoadTR] = P.Corners[RoadTL] + RoadVector;
				P.VScale = UScale;
				break;
			}

			default:
				return false;
			}
			P.Normal = RoadNormalized(RoadNormal);
			P.Vector = RoadNormalized(RoadVector);
			if (P.UScale == 0.f || P.VScale == 0.f)
			{
				return false;
			}
			P.Bounds = FBox2f(ForceInit);
			for (const FRoadVec& C : P.Corners)
			{
				P.Bounds += C;
			}
			return true;
		}

	private:
		static void UnitVectors(FRoadVec& RoadVector, FRoadVec& RoadNormal)
		{
			if (FMath::Abs(RoadVector.X) < RoadMinSegment && FMath::Abs(RoadVector.Y) < RoadMinSegment)
			{
				RoadVector = FRoadVec(1.f, 0.f);
				RoadNormal = FRoadVec(0.f, 1.f);
			}
			else
			{
				RoadVector = RoadNormalized(RoadVector);
				RoadNormal = RoadNormalized(RoadNormal);
			}
		}

		static void FlipTheRoad(FRoadSeg& R) { Swap(R.Pt1, R.Pt2); }

		void MoveRoadSegTo(int32 FromNdx, int32 ToNdx)
		{
			if (FromNdx < 0 || FromNdx >= NumRoads || ToNdx < 0 || ToNdx >= NumRoads || FromNdx == ToNdx)
			{
				return;
			}
			const FRoadSeg Cur = Roads[FromNdx];
			if (FromNdx < ToNdx)
			{
				for (int32 i = FromNdx; i < ToNdx; ++i) { Roads[i] = Roads[i + 1]; }
			}
			else
			{
				for (int32 i = FromNdx; i > ToNdx; --i) { Roads[i] = Roads[i - 1]; }
			}
			Roads[ToNdx] = Cur;
		}

		/** "Checks to see if any segments need to link before this one." */
		void CheckLinkBefore(int32 Ndx)
		{
			if (Roads[Ndx].Pt2.Count != 1)
			{
				return;
			}
			FRoadVec Loc2 = Roads[Ndx].Pt2.Loc;
			int32 EndOfCurSeg = Ndx + 1;
			while (EndOfCurSeg < NumRoads - 1)
			{
				if (Roads[EndOfCurSeg].Pt1.Loc != Roads[EndOfCurSeg + 1].Pt2.Loc)
				{
					break;
				}
				++EndOfCurSeg;
			}
			int32 CheckNdx = EndOfCurSeg + 1;
			while (CheckNdx < NumRoads)
			{
				if (Roads[CheckNdx].Pt1.Loc == Loc2)
				{
					MoveRoadSegTo(CheckNdx, Ndx);
					Loc2 = Roads[Ndx].Pt2.Loc;
					if (Roads[Ndx].Pt2.Count != 1) { return; }
					++EndOfCurSeg;
				}
				else if (Roads[CheckNdx].Pt2.Loc == Loc2)
				{
					FlipTheRoad(Roads[CheckNdx]);
					MoveRoadSegTo(CheckNdx, Ndx);
					Loc2 = Roads[Ndx].Pt2.Loc;
					if (Roads[Ndx].Pt2.Count != 1) { return; }
					++EndOfCurSeg;
				}
				else
				{
					++CheckNdx;
				}
				if (CheckNdx <= EndOfCurSeg)
				{
					CheckNdx = EndOfCurSeg + 1;
				}
			}
		}

		/** "Checks to see if any segments need to link after this one." */
		void CheckLinkAfter(int32 Ndx)
		{
			if (Roads[Ndx].Pt1.Count != 1 || Ndx >= NumRoads - 1)
			{
				return;
			}
			FRoadVec Loc1 = Roads[Ndx].Pt1.Loc;
			int32 CheckNdx = Ndx + 1;
			while (CheckNdx < NumRoads && Ndx < NumRoads - 1)
			{
				if (Roads[CheckNdx].Pt2.Loc == Loc1)
				{
					++Ndx;
					MoveRoadSegTo(CheckNdx, Ndx);
					Loc1 = Roads[Ndx].Pt1.Loc;
					if (Roads[Ndx].Pt1.Count != 1) { return; }
				}
				else if (Roads[CheckNdx].Pt1.Loc == Loc1)
				{
					FlipTheRoad(Roads[CheckNdx]);
					++Ndx;
					MoveRoadSegTo(CheckNdx, Ndx);
					Loc1 = Roads[Ndx].Pt1.Loc;
					if (Roads[Ndx].Pt1.Count != 1) { return; }
				}
				else
				{
					++CheckNdx;
				}
			}
		}

		/** updateCounts: only roads of one kind connect. */
		void UpdateCounts(FRoadSeg& Road)
		{
			Road.Pt1.Count = 0;
			Road.Pt2.Count = 0;
			const FRoadVec Loc1 = Road.Pt1.Loc, Loc2 = Road.Pt2.Loc;
			for (int32 i = 0; i < NumRoads; ++i)
			{
				FRoadSeg& O = Roads[i];
				if (O.UniqueID != Road.UniqueID)
				{
					continue;
				}
				if (O.Pt1.Loc == Loc1) { ++O.Pt1.Count; ++Road.Pt1.Count; }
				if (O.Pt1.Loc == Loc2) { ++O.Pt1.Count; ++Road.Pt2.Count; }
				if (O.Pt2.Loc == Loc1) { ++O.Pt2.Count; ++Road.Pt1.Count; }
				if (O.Pt2.Loc == Loc2) { ++O.Pt2.Count; ++Road.Pt2.Count; }
			}
		}

		/** addMapObject: puts the segment next to one it continues, so that chains end up in a row. */
		void AddMapObject(const FRoadSeg& Road, bool bUpdateTheCounts)
		{
			FRoadSeg Cur = Road;
			const FRoadVec Loc1 = Cur.Pt1.Loc, Loc2 = Cur.Pt2.Loc;
			const FRoadVec RoadNormal = RoadNormalized(FRoadVec(-(Loc2.Y - Loc1.Y), Loc2.X - Loc1.X)) * (Cur.Scale * Cur.WidthInTexture / 2.f);
			Cur.Pt1.Top = Loc1 + RoadNormal;
			Cur.Pt1.Bottom = Loc1 - RoadNormal;
			Cur.Pt2.Top = Loc2 + RoadNormal;
			Cur.Pt2.Bottom = Loc2 - RoadNormal;
			if (bUpdateTheCounts)
			{
				UpdateCounts(Cur);
			}
			if (NumRoads >= RoadMaxSegments)
			{
				return;
			}
			int32 i;
			bool bFlip = false, bAddBefore = false, bAddAfter = false, bBothMatch = false;
			for (i = 0; i < NumRoads; ++i)
			{
				bBothMatch = false;
				if ((Roads[i].Pt1.Loc == Loc1 && Roads[i].Pt2.Loc == Loc2) || (Roads[i].Pt1.Loc == Loc2 && Roads[i].Pt2.Loc == Loc1))
				{
					bBothMatch = true; // "identical segment, so discard"
					break;
				}
				if (Cur.Pt1.Count == 1)
				{
					if (Roads[i].Pt1.Loc == Loc1) { bFlip = true; bAddAfter = true; }
					if (Roads[i].Pt2.Loc == Loc1) { bFlip = false; bAddBefore = true; }
				}
				if (Cur.Pt2.Count == 1)
				{
					if (Roads[i].Pt1.Loc == Loc2) { bFlip = false; bAddAfter = true; }
					if (Roads[i].Pt2.Loc == Loc2) { bFlip = true; bAddBefore = true; }
				}
				if (bAddBefore || bAddAfter)
				{
					break;
				}
			}
			if (bBothMatch)
			{
				return;
			}
			int32 AddIndex = i;
			if (bAddAfter)
			{
				++AddIndex;
			}
			if (AddIndex < NumRoads)
			{
				for (i = NumRoads; i > AddIndex; --i)
				{
					Roads[i] = Roads[i - 1];
				}
			}
			Roads[AddIndex] = Cur;
			if (bFlip)
			{
				FlipTheRoad(Roads[AddIndex]);
			}
			++NumRoads;
			if (bAddBefore)
			{
				CheckLinkBefore(AddIndex);
			}
			else if (bAddAfter)
			{
				CheckLinkAfter(AddIndex);
			}
		}

		/** addMapObjects: a road is two map objects in a row, flagged as its first and second point. */
		void AddMapObjects(const FGenMapData& Map, const FGenTerrainRoads& Defs)
		{
			const TArray<FGenMapObject>& Objs = Map.Objects;
			for (int32 n = 0; n < Objs.Num(); ++n)
			{
				if (NumRoads >= RoadMaxSegments)
				{
					break;
				}
				const FGenMapObject& A = Objs[n];
				if (!(A.Flags & FGenMapObject::FlagRoadPoint1))
				{
					continue;
				}
				if (n + 1 >= Objs.Num())
				{
					break;
				}
				const FGenMapObject& B = Objs[n + 1];
				if (!(B.Flags & FGenMapObject::FlagRoadPoint2))
				{
					continue;
				}
				FRoadVec Loc1(A.Location.X, A.Location.Y), Loc2(B.Location.X, B.Location.Y);
				if (Loc1.X == Loc2.X && Loc1.Y == Loc2.Y)
				{
					Loc2.X += 0.25f;
				}
				FRoadSeg Cur;
				Cur.Scale = RoadDefaultScale;
				Cur.WidthInTexture = 1.f;
				Cur.UniqueID = 1;
				if (const FGenRoadType* Road = Defs.FindRoad(A.TemplateName))
				{
					Cur.WidthInTexture = Road->WidthInTexture;
					Cur.Scale = Road->Width;
					Cur.UniqueID = Road->Id;
				}
				Cur.Pt1.Loc = Loc1;
				Cur.Pt1.bAngled = (A.Flags & RoadFlagAngled) != 0;
				Cur.Pt1.bJoin = (A.Flags & RoadFlagJoin) != 0;
				Cur.Pt2.Loc = Loc2;
				Cur.Pt2.bAngled = (B.Flags & RoadFlagAngled) != 0;
				Cur.Pt2.bJoin = (B.Flags & RoadFlagJoin) != 0;
				Cur.Type = RC_Segment;
				Cur.CurveRadius = (A.Flags & RoadFlagTight) ? RoadTightCornerRadius : RoadCornerRadius;
				AddMapObject(Cur, true);
				++n; // (the second point is done with)
			}
			// Once more, now that every end knows how many others it meets.
			const int32 CurCount = NumRoads;
			NumRoads = 0;
			for (int32 i = 0; i < CurCount; ++i)
			{
				const FRoadSeg Cur = Roads[i];
				AddMapObject(Cur, false);
			}
		}

		void UpdateCountsAndFlags()
		{
			for (int32 i = 0; i < NumRoads; ++i)
			{
				Roads[i].Pt1.Count = 0;
				Roads[i].Pt2.Count = 0;
			}
			for (int32 j = NumRoads - 1; j > 0; --j)
			{
				const FRoadVec Loc1 = Roads[j].Pt1.Loc, Loc2 = Roads[j].Pt2.Loc;
				for (int32 i = 0; i < j; ++i)
				{
					if (Roads[i].UniqueID != Roads[j].UniqueID)
					{
						continue;
					}
					if (Roads[i].Pt1.Loc == Loc1) { ++Roads[i].Pt1.Count; ++Roads[j].Pt1.Count; }
					if (Roads[i].Pt1.Loc == Loc2) { ++Roads[i].Pt1.Count; ++Roads[j].Pt2.Count; }
					if (Roads[i].Pt2.Loc == Loc1) { ++Roads[i].Pt2.Count; ++Roads[j].Pt1.Count; }
					if (Roads[i].Pt2.Loc == Loc2) { ++Roads[i].Pt2.Count; ++Roads[j].Pt2.Count; }
				}
			}
		}

		/** A junction piece goes on the end of the array. */
		bool AppendJunction(const FRoadVec& Loc1, const FRoadVec& Loc2, int32 LikeIndex, float WidthInTexture, int32 Pt1Count, uint8 Type)
		{
			if (NumRoads >= RoadMaxSegments)
			{
				return false;
			}
			FRoadSeg& N = Roads[NumRoads];
			N.Pt1.Loc = Loc1;
			N.Pt2.Loc = Loc2;
			N.Scale = Roads[LikeIndex].Scale;
			N.WidthInTexture = WidthInTexture;
			N.Pt1.Count = Pt1Count;
			N.Type = Type;
			N.UniqueID = Roads[LikeIndex].UniqueID;
			++NumRoads;
			return true;
		}

		bool AppendCurve(const FRoadVec& Loc1, const FRoadVec& Loc2, int32 LikeIndex)
		{
			if (NumRoads >= RoadMaxSegments)
			{
				return false;
			}
			FRoadSeg& N = Roads[NumRoads];
			N.Pt1.Loc = Loc1;
			N.Pt2.Loc = Loc2;
			N.Scale = Roads[LikeIndex].Scale;
			N.WidthInTexture = Roads[LikeIndex].WidthInTexture;
			N.Type = RC_Curve;
			N.CurveRadius = Roads[LikeIndex].CurveRadius;
			N.UniqueID = Roads[LikeIndex].UniqueID;
			++NumRoads;
			return true;
		}

		/**
		 * The segments meeting at Loc: the one at Index1 and those after it. Pr are their far ends, Pc
		 * the ends at Loc. (Anything ending there is taken, whatever its kind.)
		 */
		bool FindMeeting(const FRoadVec& Loc, int32 Index1, FRoadPt* Pr[4], FRoadPt* Pc[4], int32 Wanted)
		{
			for (int32 k = 0; k < 4; ++k) { Pr[k] = nullptr; Pc[k] = nullptr; }
			if (Roads[Index1].Pt1.Loc == Loc)
			{
				Pr[0] = &Roads[Index1].Pt2;
				Pc[0] = &Roads[Index1].Pt1;
			}
			else
			{
				Pr[0] = &Roads[Index1].Pt1;
				Pc[0] = &Roads[Index1].Pt2;
			}
			auto Take = [&](FRoadPt* Far, FRoadPt* Near)
			{
				// The first free place; when all are taken the last one is overwritten.
				int32 Slot = Wanted - 1;
				for (int32 k = 1; k < Wanted; ++k)
				{
					if (!Pr[k]) { Slot = k; break; }
				}
				Pr[Slot] = Far;
				Pc[Slot] = Near;
			};
			for (int32 i = Index1 + 1; i < NumRoads; ++i)
			{
				if (Roads[i].Pt1.Loc == Loc)
				{
					Roads[i].Pt1.Count = -2;
					Take(&Roads[i].Pt2, &Roads[i].Pt1);
				}
				if (Roads[i].Pt2.Loc == Loc)
				{
					Roads[i].Pt2.Count = -2;
					Take(&Roads[i].Pt1, &Roads[i].Pt2);
				}
			}
			for (int32 k = 1; k < Wanted; ++k)
			{
				if (!Pr[k]) { return false; }
			}
			return true;
		}

		/** Top and bottom of an end, kept on the sides they were on, at Offset either side of the centre. */
		static void SetWidth(FRoadPt* Pc, const FRoadVec& Along, const FRoadVec& Offset, bool bPositiveIsTop)
		{
			const float X = RoadCross(Along, Pc->Top - Pc->Loc);
			if (bPositiveIsTop ? X > 0.f : X < 0.f)
			{
				Pc->Bottom = Pc->Loc - Offset;
				Pc->Top = Pc->Loc + Offset;
			}
			else
			{
				Pc->Bottom = Pc->Loc + Offset;
				Pc->Top = Pc->Loc - Offset;
			}
		}

		/** offset3Way: the three ends move out to the edges of the T piece. */
		static void Offset3Way(FRoadPt* Pc1, FRoadPt* Pc2, FRoadPt* Pc3, const FRoadVec& Loc, FRoadVec UpVector, const FRoadVec& TeeVector, float WidthInTexture)
		{
			Pc1->Loc = Loc - UpVector;
			Pc2->Loc = Loc + UpVector;
			Pc3->Loc = Loc + TeeVector;
			// "Make sure the t vector goes right with respect to the up vector."
			FRoadVec RightTee = TeeVector;
			if (RoadCross(UpVector, TeeVector) < 0.f)
			{
				RightTee = -TeeVector;
			}
			RightTee *= WidthInTexture;
			SetWidth(Pc1, UpVector, RightTee, true);
			SetWidth(Pc2, UpVector, RightTee, true);
			UpVector *= WidthInTexture;
			SetWidth(Pc3, RightTee, UpVector, false);
		}

		/** offsetH: the same for a T whose arm comes in at a slant. */
		static void OffsetH(FRoadPt* Pc1, FRoadPt* Pc2, FRoadPt* Pc3, const FRoadVec& Loc, const FRoadVec& UpVector, const FRoadVec& TeeVector, bool bFlip, bool bMirror, float WidthInTexture)
		{
			if (bFlip != bMirror)
			{
				Pc1->Loc = Loc - UpVector * 2.05f;
				Pc2->Loc = Loc + UpVector * 0.46f;
			}
			else
			{
				Pc1->Loc = Loc - UpVector * 0.46f;
				Pc2->Loc = Loc + UpVector * 2.05f;
			}
			FRoadVec RightTee = TeeVector;
			if (RoadCross(UpVector, TeeVector) < 0.f)
			{
				RightTee = -TeeVector;
			}
			RightTee *= WidthInTexture;
			SetWidth(Pc1, UpVector, RightTee, true);
			SetWidth(Pc2, UpVector, RightTee, true);

			const FRoadVec Arm = RoadRotated(TeeVector, bFlip ? PI / 4.f : -PI / 4.f);
			const FRoadVec ArmNormal = FRoadVec(-Arm.Y, Arm.X) * WidthInTexture;
			Pc3->Loc += Arm * 2.10f;
			if (RoadXpSign(Arm, Pc3->Top - Loc) == 1)
			{
				Pc3->Top = Pc3->Loc + ArmNormal;
				Pc3->Bottom = Pc3->Loc - ArmNormal;
			}
			else
			{
				Pc3->Top = Pc3->Loc - ArmNormal;
				Pc3->Bottom = Pc3->Loc + ArmNormal;
			}
		}

		/** offsetY: the ends move to the stumps of the Y piece. */
		static void OffsetY(FRoadPt* Pc1, FRoadPt* Pc2, FRoadPt* Pc3, const FRoadVec& Loc, const FRoadVec& UpVector, float WidthInTexture)
		{
			Pc3->Loc += UpVector * 0.55f;
			Pc3->Top += UpVector * 0.55f;
			Pc3->Bottom += UpVector * 0.55f;

			FRoadVec Arm = RoadRotated(UpVector, 3.f * PI / 4.f);
			FRoadVec ArmNormal = FRoadVec(-Arm.Y, Arm.X) * WidthInTexture;
			Pc2->Loc += Arm * 1.1f;
			if (RoadXpSign(Arm, Pc2->Top - Loc) == 1)
			{
				Pc2->Top = Pc2->Loc + ArmNormal;
				Pc2->Bottom = Pc2->Loc - ArmNormal;
			}
			else
			{
				Pc2->Top = Pc2->Loc - ArmNormal;
				Pc2->Bottom = Pc2->Loc + ArmNormal;
			}

			Arm = RoadRotated(UpVector, -3.f * PI / 4.f);
			ArmNormal = FRoadVec(-Arm.Y, Arm.X) * WidthInTexture;
			Pc1->Loc += Arm * 1.1f;
			if (RoadXpSign(Arm, Pc1->Top - Loc) == 1)
			{
				Pc1->Top = Pc1->Loc + ArmNormal;
				Pc1->Bottom = Pc1->Loc - ArmNormal;
			}
			else
			{
				Pc1->Top = Pc1->Loc - ArmNormal;
				Pc1->Bottom = Pc1->Loc + ArmNormal;
			}
		}

		/** offset4Way */
		static void Offset4Way(FRoadPt* Pc1, FRoadPt* Pc2, FRoadPt* Pc3, FRoadPt* Pr3, FRoadPt* Pc4, const FRoadVec& Loc, FRoadVec AlignVector, float WidthInTexture)
		{
			Pc1->Loc = Loc - AlignVector;
			Pc2->Loc = Loc + AlignVector;
			const FRoadVec V3 = Pr3->Loc - Loc;
			const FRoadVec TeeVector = RoadRotated(AlignVector, RoadCross(AlignVector, V3) < 0.f ? -PI / 2.f : PI / 2.f);
			Pc3->Loc = Loc + TeeVector;
			Pc4->Loc = Loc - TeeVector;

			const FRoadVec RealTee = RoadRotated(AlignVector, PI / 2.f) * WidthInTexture;
			SetWidth(Pc1, AlignVector, RealTee, true);
			SetWidth(Pc2, AlignVector, RealTee, true);
			const FRoadVec Align = AlignVector * WidthInTexture;
			SetWidth(Pc3, RealTee, Align, false);
			SetWidth(Pc4, RealTee, Align, false);
			Pc1->Count = 0;
			Pc2->Count = 0;
			Pc3->Count = 0;
			Pc4->Count = 0;
		}

		/** insertY: "Inserts a Y intersection if the corner meets 'Y' criteria." */
		bool InsertY(const FRoadVec Loc, int32 Index1, float Scale)
		{
			FRoadPt* Pr[4];
			FRoadPt* Pc[4];
			if (!FindMeeting(Loc, Index1, Pr, Pc, 3))
			{
				return false;
			}
			const FRoadVec V1 = RoadNormalized(Pr[0]->Loc - Loc), V2 = RoadNormalized(Pr[1]->Loc - Loc), V3 = RoadNormalized(Pr[2]->Loc - Loc);
			bool bDo12 = false, bDo13 = false, bDo32 = false;
			const float Dot12 = RoadDot(V1, V2), Dot13 = RoadDot(V1, V3), Dot32 = RoadDot(V3, V2);
			float Score12 = 2.f, Score13 = 2.f, Score32 = 2.f;
			const float Cos30 = 0.866f, Cos45 = 0.707f;
			// "Too close to a straight line, do a straight side tee."
			if (Dot12 < -Cos30 || Dot13 < -Cos30 || Dot32 < -Cos30)
			{
				return false;
			}

			int32 S1 = 0;
			int32 S2 = RoadXpSign(V1, V2);
			int32 S3 = RoadXpSign(V1, V3);
			if (S2 != S3 && (S2 + S3 == 0))
			{
				const FRoadVec V1_90(-V1.Y, V1.X);
				if (RoadXpSign(V1_90, V2) == 1 && RoadXpSign(V1_90, V3) == 1)
				{
					bDo32 = true;
					Score32 = FMath::Abs(Dot12 + Cos45) + FMath::Abs(Dot13 + Cos45);
				}
			}
			S1 = RoadXpSign(V3, V1);
			S2 = RoadXpSign(V3, V2);
			if (S2 != S1 && (S2 + S1 == 0))
			{
				const FRoadVec V3_90(-V3.Y, V3.X);
				if (RoadXpSign(V3_90, V2) == 1 && RoadXpSign(V3_90, V1) == 1)
				{
					bDo12 = true;
					Score12 = FMath::Abs(Dot13 + Cos45) + FMath::Abs(Dot32 + Cos45);
				}
			}
			S1 = RoadXpSign(V2, V1);
			S3 = RoadXpSign(V2, V3);
			if (S3 != S1 && (S3 + S1 == 0))
			{
				const FRoadVec V2_90(-V2.Y, V2.X);
				if (RoadXpSign(V2_90, V3) == 1 && RoadXpSign(V2_90, V1) == 1)
				{
					bDo13 = true;
					Score13 = FMath::Abs(Dot12 + Cos45) + FMath::Abs(Dot32 + Cos45);
				}
			}
			if (Score12 < Score13)
			{
				bDo13 = false;
				if (Score12 < Score32) { bDo32 = false; } else { bDo12 = false; }
			}
			else
			{
				bDo12 = false;
				if (Score13 < Score32) { bDo32 = false; } else { bDo13 = false; }
			}

			FRoadVec UpVector;
			if (bDo12) { UpVector = V3; }
			else if (bDo13) { UpVector = V2; }
			else if (bDo32) { UpVector = V1; }
			else { return false; }
			UpVector = RoadNormalized(UpVector) * (0.5f * Scale); // "we are offsetting one half road width"
			const FRoadVec TeeVector = RoadRotated(UpVector, -PI / 2.f);

			const float Width = Roads[Index1].WidthInTexture;
			if (bDo12)
			{
				if (RoadXpSign(V3, V1) == -1) { OffsetY(Pc[0], Pc[1], Pc[2], Loc, UpVector, Width); }
				else { OffsetY(Pc[1], Pc[0], Pc[2], Loc, UpVector, Width); }
			}
			if (bDo13)
			{
				if (RoadXpSign(V2, V1) == -1) { OffsetY(Pc[0], Pc[2], Pc[1], Loc, UpVector, Width); }
				else { OffsetY(Pc[2], Pc[0], Pc[1], Loc, UpVector, Width); }
			}
			if (bDo32)
			{
				if (RoadXpSign(V1, V3) == -1) { OffsetY(Pc[2], Pc[1], Pc[0], Loc, UpVector, Width); }
				else { OffsetY(Pc[1], Pc[2], Pc[0], Loc, UpVector, Width); }
			}
			Pc[0]->Count = 0;
			Pc[1]->Count = 0;
			Pc[2]->Count = 0;
			return AppendJunction(Loc, Loc + TeeVector, Index1, Width, -3, RC_ThreeWayY);
		}

		/** insertTee: where three of a kind meet. */
		void InsertTee(const FRoadVec Loc, int32 Index1, float Scale)
		{
			if (InsertY(Loc, Index1, Scale))
			{
				return;
			}
			FRoadPt* Pr[4];
			FRoadPt* Pc[4];
			if (!FindMeeting(Loc, Index1, Pr, Pc, 3))
			{
				return;
			}
			const FRoadVec V1 = RoadNormalized(Pr[0]->Loc - Loc), V2 = RoadNormalized(Pr[1]->Loc - Loc), V3 = RoadNormalized(Pr[2]->Loc - Loc);
			const float Dot12 = RoadDot(V1, V2), Dot13 = RoadDot(V1, V3), Dot32 = RoadDot(V3, V2);
			// "The greatest negative dot product is the pair that is heading most opposite each other."
			bool bDo12 = false, bDo13 = false, bDo32 = false;
			if (Dot12 < Dot13)
			{
				if (Dot12 < Dot32) { bDo12 = true; } else { bDo32 = true; }
			}
			else
			{
				if (Dot13 < Dot32) { bDo13 = true; } else { bDo32 = true; }
			}
			FRoadVec UpVector = FRoadVec::ZeroVector, Decider = FRoadVec::ZeroVector;
			if (bDo12) { UpVector = V2 - V1; Decider = V3; }
			if (bDo13) { UpVector = V3 - V1; Decider = V2; }
			if (bDo32) { UpVector = V2 - V3; Decider = V1; }
			UpVector = RoadNormalized(UpVector);

			const float Width = Roads[Index1].WidthInTexture;
			const float Cos60 = 0.5f;
			const bool bSlanted = FMath::Abs(RoadDot(UpVector, Decider)) > Cos60;
			float Angle = PI / 2.f;
			bool bMirror = false;
			if (RoadCross(UpVector, Decider) < 0.f)
			{
				Angle = -Angle;
				bMirror = true;
			}
			UpVector = RoadNormalized(UpVector) * (0.5f * Scale);
			const FRoadVec TeeVector = RoadRotated(UpVector, Angle);
			if (bSlanted)
			{
				bool bFlip = false;
				if (bDo12)
				{
					bFlip = RoadXpSign(TeeVector, V3) == 1;
					OffsetH(Pc[0], Pc[1], Pc[2], Loc, UpVector, TeeVector, bFlip, bMirror, Width);
				}
				if (bDo13)
				{
					bFlip = RoadXpSign(TeeVector, V2) == 1;
					OffsetH(Pc[0], Pc[2], Pc[1], Loc, UpVector, TeeVector, bFlip, bMirror, Width);
				}
				if (bDo32)
				{
					bFlip = RoadXpSign(TeeVector, V1) == 1;
					OffsetH(Pc[2], Pc[1], Pc[0], Loc, UpVector, TeeVector, bFlip, bMirror, Width);
				}
				Pc[0]->Count = 0;
				Pc[1]->Count = 0;
				Pc[2]->Count = 0;
				AppendJunction(Loc, Loc + TeeVector, Index1, Width, -3, bFlip ? RC_ThreeWayHFlip : RC_ThreeWayH);
			}
			else
			{
				if (bDo12) { Offset3Way(Pc[0], Pc[1], Pc[2], Loc, UpVector, TeeVector, Width); }
				if (bDo13) { Offset3Way(Pc[0], Pc[2], Pc[1], Loc, UpVector, TeeVector, Width); }
				if (bDo32) { Offset3Way(Pc[2], Pc[1], Pc[0], Loc, UpVector, TeeVector, Width); }
				Pc[0]->Count = 0;
				Pc[1]->Count = 0;
				Pc[2]->Count = 0;
				AppendJunction(Loc, Loc + TeeVector, Index1, Width, -3, RC_Tee);
			}
		}

		/** insert4Way: where four meet. */
		void Insert4Way(const FRoadVec Loc, int32 Index1, float Scale)
		{
			FRoadPt* Pr[4];
			FRoadPt* Pc[4];
			if (!FindMeeting(Loc, Index1, Pr, Pc, 4))
			{
				return;
			}
			FRoadVec V[4];
			for (int32 k = 0; k < 4; ++k)
			{
				V[k] = RoadNormalized(Pr[k]->Loc - Loc);
			}
			// "The most negative dot product is the pair that is heading most opposite each other."
			static const int32 PairA[6] = { 0, 0, 0, 1, 1, 2 };
			static const int32 PairB[6] = { 1, 2, 3, 2, 3, 3 };
			int32 CurPair = 0;
			float CurDot = RoadDot(V[0], V[1]);
			for (int32 p = 1; p < 6; ++p)
			{
				const float D = RoadDot(V[PairA[p]], V[PairB[p]]);
				if (D < CurDot)
				{
					CurPair = p;
					CurDot = D;
				}
			}
			FRoadVec AlignVector = RoadNormalized(V[PairB[CurPair]] - V[PairA[CurPair]]) * (0.5f * Scale);
			const float Width = Roads[Index1].WidthInTexture;
			switch (CurPair)
			{
			case 0: Offset4Way(Pc[0], Pc[1], Pc[2], Pr[2], Pc[3], Loc, AlignVector, Width); break; // 1-2
			case 1: Offset4Way(Pc[0], Pc[2], Pc[1], Pr[1], Pc[3], Loc, AlignVector, Width); break; // 1-3
			case 2: Offset4Way(Pc[0], Pc[3], Pc[2], Pr[2], Pc[1], Loc, AlignVector, Width); break; // 1-4
			case 3: Offset4Way(Pc[1], Pc[2], Pc[0], Pr[0], Pc[3], Loc, AlignVector, Width); break; // 2-3
			case 4: Offset4Way(Pc[1], Pc[3], Pc[0], Pr[0], Pc[2], Loc, AlignVector, Width); break; // 2-4
			default: Offset4Way(Pc[2], Pc[3], Pc[0], Pr[0], Pc[1], Loc, AlignVector, Width); break; // 3-4
			}
			if (AlignVector.X < 0.f)
			{
				// "Since the 4 way intersection is symmetrical, make it go right (pos x)"
				AlignVector = -AlignVector;
			}
			AppendJunction(Loc, Loc + AlignVector, Index1, RoadTeeWidthAdjustment, -4, RC_FourWay);
		}

		void InsertTeeIntersections()
		{
			const int32 NumRoadSegments = NumRoads;
			for (int32 i = 0; i < NumRoadSegments; ++i)
			{
				if (Roads[i].Type != RC_Segment)
				{
					continue;
				}
				if (Roads[i].Pt1.Count == 2) { InsertTee(Roads[i].Pt1.Loc, i, Roads[i].Scale); }
				if (Roads[i].Pt2.Count == 2) { InsertTee(Roads[i].Pt2.Loc, i, Roads[i].Scale); }
				if (Roads[i].Pt1.Count == 3) { Insert4Way(Roads[i].Pt1.Loc, i, Roads[i].Scale); }
				if (Roads[i].Pt2.Count == 3) { Insert4Way(Roads[i].Pt2.Loc, i, Roads[i].Scale); }
			}
		}

		/** miter: "Adjusts the end points to create a smooth miter join between road segments." */
		void Miter(int32 Ndx1, int32 Ndx2)
		{
			FRoadVec Hit;
			if (FRoadLine(Roads[Ndx1].Pt1.Top, Roads[Ndx1].Pt2.Top).Intersect(FRoadLine(Roads[Ndx2].Pt1.Top, Roads[Ndx2].Pt2.Top), Hit))
			{
				Roads[Ndx2].Pt2.Top = Hit;
				Roads[Ndx1].Pt1.Top = Hit;
			}
			if (FRoadLine(Roads[Ndx1].Pt1.Bottom, Roads[Ndx1].Pt2.Bottom).Intersect(FRoadLine(Roads[Ndx2].Pt1.Bottom, Roads[Ndx2].Pt2.Bottom), Hit))
			{
				Roads[Ndx2].Pt2.Bottom = Hit;
				Roads[Ndx1].Pt1.Bottom = Hit;
			}
		}

		/** rotateAbout */
		static void RotateAbout(FRoadVec& P, const FRoadVec& Center, float Angle)
		{
			const FRoadVec Offset = P - Center;
			P += RoadRotated(Offset, Angle) - Offset;
		}

		/** insertCurveSegmentAt: the corner between two segments in a row, rounded in steps of 30 degrees. */
		void InsertCurveSegmentAt(int32 Ndx1, int32 Ndx2)
		{
			const float DotLimit = 0.5f; // "If the dot product of the new line is less than this, abort."
			const float Radius = Roads[Ndx1].CurveRadius * Roads[Ndx1].Scale;
			const FRoadVec OriginalPt = Roads[Ndx1].Pt1.Loc;
			FRoadLine Line1(Roads[Ndx1].Pt1.Loc, Roads[Ndx1].Pt2.Loc);
			FRoadLine Line2(Roads[Ndx2].Pt1.Loc, Roads[Ndx2].Pt2.Loc);
			if (Roads[Ndx1].UniqueID != Roads[Ndx2].UniqueID)
			{
				return;
			}
			const float CurSin = RoadDot(Line1.Dir, Line2.Dir);
			FRoadVec *Pr1, *Pr2, *Pr3, *Pr4;
			if (RoadCross(Line1.Dir, Line2.Dir) > 0.f)
			{
				Pr1 = &Roads[Ndx1].Pt1.Loc;
				Pr2 = &Roads[Ndx1].Pt2.Loc;
				Pr3 = &Roads[Ndx2].Pt1.Loc;
				Pr4 = &Roads[Ndx2].Pt2.Loc;
			}
			else
			{
				Pr4 = &Roads[Ndx1].Pt1.Loc;
				Pr3 = &Roads[Ndx1].Pt2.Loc;
				Pr2 = &Roads[Ndx2].Pt1.Loc;
				Pr1 = &Roads[Ndx2].Pt2.Loc;
				Line1.Set(*Pr1, *Pr2);
				Line2.Set(*Pr3, *Pr4);
			}
			const float Count = FMath::Acos(CurSin) / (PI / 6.f); // "number of 30 degree steps"
			if (Count < 0.9f || Roads[Ndx1].Pt1.bAngled)
			{
				Miter(Ndx1, Ndx2);
				return;
			}

			const FRoadVec Offset1 = RoadRotated(Line1.Dir * Radius, -PI / 2.f);
			const FRoadVec Offset2 = RoadRotated(Line2.Dir * Radius, -PI / 2.f);
			FRoadVec Centre;
			if (!FRoadLine(*Pr1 + Offset1, *Pr2 + Offset1).Intersect(FRoadLine(*Pr3 + Offset2, *Pr4 + Offset2), Centre))
			{
				return;
			}
			// Where the arc leaves each of the two lines.
			FRoadVec Int1 = Centre, Int3 = Centre;
			FRoadLine(Centre, Centre - Offset2).Intersect(Line2, Int1);
			FRoadLine(Centre, Centre - Offset1).Intersect(Line1, Int3);
			// "Make sure the lines didn't clip out of existence."
			if (RoadDot(Line2.Dir, Int1 - *Pr3) < DotLimit || RoadDot(Line1.Dir, *Pr2 - Int3) < DotLimit)
			{
				*Pr1 = OriginalPt;
				*Pr4 = OriginalPt;
				Miter(Ndx1, Ndx2);
				return;
			}
			*Pr4 = Int1;
			const float Angle = -PI / 6.f;
			FRoadVec Pt2 = *Pr4;
			FRoadVec Pt1 = *Pr3;
			FRoadVec Direction = Pt1 - Pt2;
			const FRoadVec CenterOfCurve = RoadNormalized(FRoadVec(-Direction.Y, Direction.X)) * (Roads[Ndx1].CurveRadius * Roads[Ndx1].Scale) + Pt2;

			RotateAbout(Pt2, CenterOfCurve, Angle);
			Direction = RoadRotated(Direction, Angle);
			Pt1 = Pt2 + Direction;
			if (!AppendCurve(Pt2, Pt1, Ndx1))
			{
				return;
			}
			if (Count > 2.f)
			{
				for (int32 i = 2; float(i) < Count; ++i)
				{
					Direction = RoadRotated(Direction, Angle);
					RotateAbout(Pt2, CenterOfCurve, Angle);
					Pt1 = Pt2 + Direction;
					if (!AppendCurve(Pt2, Pt1, Ndx1))
					{
						return;
					}
				}
			}
			*Pr1 = Int3;
			if (Count > 1.f)
			{
				Pt2 = *Pr1;
				Pt1 = *Pr1 + *Pr1 - *Pr2;
				if (!AppendCurve(Pt2, Pt1, Ndx1))
				{
					return;
				}
			}

			// "Recalculate top & bottom."
			FRoadVec RoadVector = Roads[Ndx1].Pt2.Loc - Roads[Ndx1].Pt1.Loc;
			FRoadVec RoadNormal = RoadNormalized(FRoadVec(-RoadVector.Y, RoadVector.X)) * (Roads[Ndx1].Scale * Roads[Ndx1].WidthInTexture / 2.f);
			Roads[Ndx1].Pt1.Top = Roads[Ndx1].Pt1.Loc + RoadNormal;
			Roads[Ndx1].Pt1.Bottom = Roads[Ndx1].Pt1.Loc - RoadNormal;
			RoadVector = Roads[Ndx2].Pt2.Loc - Roads[Ndx2].Pt1.Loc;
			RoadNormal = RoadNormalized(FRoadVec(-RoadVector.Y, RoadVector.X)) * (Roads[Ndx2].Scale * Roads[Ndx2].WidthInTexture / 2.f);
			Roads[Ndx2].Pt2.Top = Roads[Ndx2].Pt2.Loc + RoadNormal;
			Roads[Ndx2].Pt2.Bottom = Roads[Ndx2].Pt2.Loc - RoadNormal;
		}

		void InsertCurveSegments()
		{
			const int32 NumRoadSegments = NumRoads;
			int32 SegmentStartIndex = -1;
			for (int32 i = 0; i < NumRoadSegments; ++i)
			{
				if (i < NumRoadSegments - 1 && Roads[i].Pt1.Loc == Roads[i + 1].Pt2.Loc)
				{
					if (Roads[i + 1].Pt2.Count == 1 && Roads[i].Pt1.Count == 1)
					{
						InsertCurveSegmentAt(i, i + 1);
						if (SegmentStartIndex < 0)
						{
							SegmentStartIndex = i;
						}
					}
				}
				else if (SegmentStartIndex >= 0)
				{
					// (a chain that comes back to where it started)
					if (Roads[i].Pt1.Loc == Roads[SegmentStartIndex].Pt2.Loc)
					{
						if (Roads[SegmentStartIndex].Pt2.Count == 1 && Roads[i].Pt1.Count == 1)
						{
							InsertCurveSegmentAt(i, SegmentStartIndex);
						}
					}
					SegmentStartIndex = -1;
				}
			}
		}

		/** findCrossTypeJoinVector: a straight segment of another kind that Loc lies on; the join turns square to it. */
		int32 FindCrossTypeJoinVector(const FRoadVec& Loc, FRoadVec& JoinVector, int32 UniqueID) const
		{
			for (int32 i = 0; i < NumRoads; ++i)
			{
				const FRoadSeg& R = Roads[i];
				if (R.UniqueID == UniqueID || R.Type != RC_Segment)
				{
					continue;
				}
				const FRoadVec Loc1 = R.Pt1.Loc, Loc2 = R.Pt2.Loc;
				const float Half = R.Scale / 2.f;
				if (Loc.X < FMath::Min(Loc1.X, Loc2.X) - Half || Loc.Y < FMath::Min(Loc1.Y, Loc2.Y) - Half
					|| Loc.X > FMath::Max(Loc1.X, Loc2.X) + Half || Loc.Y > FMath::Max(Loc1.Y, Loc2.Y) + Half)
				{
					continue;
				}
				const FRoadVec OnLine = FRoadLine(Loc1, Loc2).ClosestPoint(Loc);
				if (RoadLength(OnLine - Loc) < R.Scale * 0.55f)
				{
					const FRoadVec RoadVec = Loc2 - Loc1;
					JoinVector = RoadRotated(RoadVec, RoadXpSign(RoadVec, JoinVector) == 1 ? PI / 2.f : -PI / 2.f);
					return R.UniqueID;
				}
			}
			return 0;
		}

		/** adjustStacking: the joining road is drawn over the one it joins. */
		void AdjustStacking(int32 TopUniqueID, int32 BottomUniqueID)
		{
			const int32 Top = Types.IndexOfByPredicate([&](const FRoadTypeSlot& S) { return S.UniqueID == TopUniqueID; });
			const int32 Bottom = Types.IndexOfByPredicate([&](const FRoadTypeSlot& S) { return S.UniqueID == BottomUniqueID; });
			if (Top == INDEX_NONE || Bottom == INDEX_NONE || Types[Top].Stacking > Types[Bottom].Stacking)
			{
				return;
			}
			const int32 NewStacking = Types[Bottom].Stacking;
			for (FRoadTypeSlot& S : Types)
			{
				if (S.Stacking > NewStacking)
				{
					++S.Stacking;
				}
			}
			Types[Top].Stacking = NewStacking + 1;
		}

		/** insertCrossTypeJoins: "Inserts alpha blend type joins at open ends." */
		void InsertCrossTypeJoins()
		{
			const int32 NumRoadSegments = NumRoads;
			for (int32 i = 0; i < NumRoadSegments; ++i)
			{
				FRoadVec Loc1, Loc2;
				bool bIsPt1 = false;
				if (Roads[i].Pt2.Count == 0 && Roads[i].Pt2.bJoin)
				{
					Loc1 = Roads[i].Pt2.Loc;
					Loc2 = Roads[i].Pt1.Loc;
				}
				else if (Roads[i].Pt1.Count == 0 && Roads[i].Pt1.bJoin)
				{
					Loc1 = Roads[i].Pt1.Loc;
					Loc2 = Roads[i].Pt2.Loc;
					bIsPt1 = true;
				}
				else
				{
					continue;
				}
				const FRoadVec RoadVector = RoadNormalized(Loc2 - Loc1);
				FRoadVec JoinVector = RoadVector;
				const int32 OtherID = FindCrossTypeJoinVector(Loc1, JoinVector, Roads[i].UniqueID);
				if (!OtherID)
				{
					JoinVector *= 100.f;
				}
				const FRoadVec RoadNormal(-RoadVector.Y, RoadVector.X);
				const FRoadVec JoinNormal(-JoinVector.Y, JoinVector.X);
				const float Half = Roads[i].Scale * Roads[i].WidthInTexture / 2.f;
				const FRoadLine JoinLine(Loc1, Loc1 + JoinNormal);

				// The end of the road is cut along the join's line.
				FRoadVec Hit;
				FRoadVec Top = Roads[i].Pt1.Top;
				if (JoinLine.Intersect(FRoadLine(Loc1 + RoadNormal * Half, Loc2 + RoadNormal * Half), Hit))
				{
					if (bIsPt1) { Roads[i].Pt1.Top = Hit; } else { Roads[i].Pt2.Bottom = Hit; }
					Top = Hit;
				}
				FRoadVec Bottom = Roads[i].Pt1.Bottom;
				if (JoinLine.Intersect(FRoadLine(Loc1 - RoadNormal * Half, Loc2 - RoadNormal * Half), Hit))
				{
					if (bIsPt1) { Roads[i].Pt1.Bottom = Hit; } else { Roads[i].Pt2.Top = Hit; }
					Bottom = Hit;
				}
				const float ScaleAdjustment = RoadLength(Bottom - Top) / (Roads[i].Scale * Roads[i].WidthInTexture);
				if (OtherID)
				{
					AdjustStacking(Roads[i].UniqueID, OtherID);
				}
				if (!AppendJunction(Loc1, Loc1 + JoinVector, i, Roads[i].Scale * ScaleAdjustment, 0, RC_AlphaJoin))
				{
					return;
				}
			}
		}
	};

	// -----------------------------------------------------------------------------------------
	// Onto the ground
	// -----------------------------------------------------------------------------------------

	/** The original's FLOAT_AMOUNT: how far a road lies above what is under it. */
	constexpr float RoadFloatAmount = GenCoords::MapHeightScale / 8.f;
	/** Between a piece and one drawn over it. */
	constexpr float RoadLayerStep = 0.03f;
	constexpr int32 RoadChunkCells = 64;

	struct FRoadVertex
	{
		FVector3f Pos;      // Generals units
		FVector3f Normal;   // Unreal space
		FVector2f UV;
	};

	struct FRoadMeshBuffer
	{
		int32 TypeSlot = 0;
		TArray<FRoadVertex> Vertices;
		TArray<int32> Indices;
	};

	/** Sutherland-Hodgman: what of a convex polygon lies on the left of A -> B. */
	int32 RoadClipPolygon(const FRoadVec* In, int32 NumIn, const FRoadVec& A, const FRoadVec& B, FRoadVec* Out)
	{
		int32 N = 0;
		const FRoadVec Edge = B - A;
		for (int32 i = 0; i < NumIn; ++i)
		{
			const FRoadVec& P = In[i];
			const FRoadVec& Q = In[(i + 1) % NumIn];
			const float DP = RoadCross(Edge, P - A);
			const float DQ = RoadCross(Edge, Q - A);
			if (DP >= 0.f)
			{
				Out[N++] = P;
			}
			if ((DP >= 0.f) != (DQ >= 0.f))
			{
				Out[N++] = P + (Q - P) * (DP / (DP - DQ));
			}
		}
		return N;
	}

	/** Do two (convex) quads, corners in order around, overlap by more than a touch? */
	bool RoadQuadsOverlap(const FRoadVec A[4], const FRoadVec B[4], float Tolerance)
	{
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			const FRoadVec* P = Pass ? B : A;
			for (int32 e = 0; e < 4; ++e)
			{
				const FRoadVec Edge = P[(e + 1) & 3] - P[e];
				const FRoadVec Axis = RoadNormalized(FRoadVec(-Edge.Y, Edge.X));
				if (Axis.X == 0.f && Axis.Y == 0.f)
				{
					continue;
				}
				float MinA = FLT_MAX, MaxA = -FLT_MAX, MinB = FLT_MAX, MaxB = -FLT_MAX;
				for (int32 k = 0; k < 4; ++k)
				{
					const float DA = RoadDot(Axis, A[k]);
					const float DB = RoadDot(Axis, B[k]);
					MinA = FMath::Min(MinA, DA); MaxA = FMath::Max(MaxA, DA);
					MinB = FMath::Min(MinB, DB); MaxB = FMath::Max(MaxB, DB);
				}
				if (FMath::Min(MaxA, MaxB) - FMath::Max(MinA, MinB) < Tolerance)
				{
					return false;
				}
			}
		}
		return true;
	}

	/** Lays the pieces on the terrain's triangles. */
	class FRoadDraper
	{
	public:
		FRoadDraper(const FGenMapData& InMap, const UGenTerrainComponent* InTerrain) : Map(InMap), Terrain(InTerrain) {}

		int32 NumTriangles = 0;

		void Drape(const FRoadPiece& P, FRoadMeshBuffer& Out)
		{
			// In strips along the piece, so that a long road on the slant doesn't visit every cell of
			// the square around it. (The outline is the original's: its columns run the same way.)
			const float Len = FMath::Max(RoadLength(P.Corners[RoadBR] - P.Corners[RoadBL]), RoadLength(P.Corners[RoadTR] - P.Corners[RoadTL]));
			const int32 Strips = FMath::Clamp(FMath::CeilToInt32(Len / 40.f), 1, 256);
			for (int32 s = 0; s < Strips; ++s)
			{
				const float T0 = float(s) / float(Strips), T1 = float(s + 1) / float(Strips);
				const FRoadVec BL = FMath::Lerp(P.Corners[RoadBL], P.Corners[RoadBR], T0);
				const FRoadVec BR = FMath::Lerp(P.Corners[RoadBL], P.Corners[RoadBR], T1);
				const FRoadVec TL = FMath::Lerp(P.Corners[RoadTL], P.Corners[RoadTR], T0);
				const FRoadVec TR = FMath::Lerp(P.Corners[RoadTL], P.Corners[RoadTR], T1);
				DrapeTriangle(P, BL, BR, TR, Out);
				DrapeTriangle(P, BL, TR, TL, Out);
			}
		}

	private:
		const FGenMapData& Map;
		const UGenTerrainComponent* Terrain;

		FVector3f VertexNormal(int32 X, int32 Y) const
		{
			// (as UGenTerrainComponent::BuildChunk)
			const float DX = Map.GetHeightAt(X + 1, Y) - Map.GetHeightAt(X - 1, Y);
			const float DY = Map.GetHeightAt(X, Y + 1) - Map.GetHeightAt(X, Y - 1);
			return GenCoords::DirToUE(-DX, -DY, 2.f * GenCoords::MapXYFactor).GetSafeNormal();
		}

		void DrapeTriangle(const FRoadPiece& P, FRoadVec A, FRoadVec B, FRoadVec C, FRoadMeshBuffer& Out)
		{
			const float Area2 = RoadCross(B - A, C - A);
			if (FMath::Abs(Area2) < 1e-4f)
			{
				return;
			}
			if (Area2 < 0.f)
			{
				Swap(B, C); // counter-clockwise seen from above, like the terrain's
			}
			const float Inv = 1.f / GenCoords::MapXYFactor;
			const int32 X0 = FMath::Clamp(FMath::FloorToInt32(FMath::Min3(A.X, B.X, C.X) * Inv) + Map.Border, 0, Map.Width - 2);
			const int32 X1 = FMath::Clamp(FMath::FloorToInt32(FMath::Max3(A.X, B.X, C.X) * Inv) + Map.Border, 0, Map.Width - 2);
			const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(FMath::Min3(A.Y, B.Y, C.Y) * Inv) + Map.Border, 0, Map.Height - 2);
			const int32 Y1 = FMath::Clamp(FMath::FloorToInt32(FMath::Max3(A.Y, B.Y, C.Y) * Inv) + Map.Border, 0, Map.Height - 2);
			const float ZOffset = RoadFloatAmount + float(P.Layer) * RoadLayerStep;
			const FRoadVec Tri[3] = { A, B, C };
			static const int32 CornerDX[4] = { 0, 1, 1, 0 };
			static const int32 CornerDY[4] = { 0, 0, 1, 1 };
			// HeightMap.cpp's two triangles of a cell, and the two of one whose diagonal is flipped.
			static const int32 Plain[2][3] = { { 0, 2, 3 }, { 0, 1, 2 } };
			static const int32 Flipped[2][3] = { { 1, 3, 0 }, { 1, 2, 3 } };
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					FRoadVec CellPos[4];
					float CellZ[4];
					for (int32 c = 0; c < 4; ++c)
					{
						CellPos[c] = FRoadVec(float(X + CornerDX[c] - Map.Border) * GenCoords::MapXYFactor, float(Y + CornerDY[c] - Map.Border) * GenCoords::MapXYFactor);
						CellZ[c] = Map.GetHeightAt(X + CornerDX[c], Y + CornerDY[c]);
					}
					const bool bFlip = Terrain && Terrain->IsCellFlipped(Map, X, Y);
					bool bHaveNormals = false;
					FVector3f CellN[4];
					for (int32 t = 0; t < 2; ++t)
					{
						const int32* Ndx = bFlip ? Flipped[t] : Plain[t];
						const FRoadVec T0 = CellPos[Ndx[0]], T1 = CellPos[Ndx[1]], T2 = CellPos[Ndx[2]];
						FRoadVec Buf1[10], Buf2[10];
						int32 N = RoadClipPolygon(Tri, 3, T0, T1, Buf1);
						if (N < 3) { continue; }
						N = RoadClipPolygon(Buf1, N, T1, T2, Buf2);
						if (N < 3) { continue; }
						N = RoadClipPolygon(Buf2, N, T2, T0, Buf1);
						if (N < 3) { continue; }
						float PolyArea2 = 0.f;
						for (int32 k = 1; k + 1 < N; ++k)
						{
							PolyArea2 += RoadCross(Buf1[k] - Buf1[0], Buf1[k + 1] - Buf1[0]);
						}
						if (PolyArea2 < 1e-3f)
						{
							continue;
						}
						if (!bHaveNormals)
						{
							for (int32 c = 0; c < 4; ++c)
							{
								CellN[c] = VertexNormal(X + CornerDX[c], Y + CornerDY[c]);
							}
							bHaveNormals = true;
						}
						const float TriArea2 = RoadCross(T1 - T0, T2 - T0);
						const int32 Base = Out.Vertices.Num();
						for (int32 k = 0; k < N; ++k)
						{
							const FRoadVec& Q = Buf1[k];
							// Where on the terrain's triangle: its height and its shading there.
							const float W1 = RoadCross(Q - T0, T2 - T0) / TriArea2;
							const float W2 = RoadCross(T1 - T0, Q - T0) / TriArea2;
							const float W0 = 1.f - W1 - W2;
							FRoadVertex& V = Out.Vertices.AddDefaulted_GetRef();
							V.Pos = FVector3f(Q.X, Q.Y, W0 * CellZ[Ndx[0]] + W1 * CellZ[Ndx[1]] + W2 * CellZ[Ndx[2]] + ZOffset);
							V.Normal = (CellN[Ndx[0]] * W0 + CellN[Ndx[1]] * W1 + CellN[Ndx[2]] * W2).GetSafeNormal();
							// loadFloat4PtSection: "Road is 1/16 texture height."
							const FRoadVec Rel = Q - P.Loc;
							V.UV = FVector2f(P.UOffset + RoadDot(P.Vector, Rel) / (P.UScale * 4.f), P.VOffset - RoadDot(P.Normal, Rel) / (P.VScale * 4.f));
						}
						for (int32 k = 1; k + 1 < N; ++k)
						{
							Out.Indices.Append({ Base, Base + k, Base + k + 1 });
							++NumTriangles;
						}
					}
				}
			}
		}
	};

	UStaticMesh* BuildRoadMesh(UObject* Outer, const FRoadMeshBuffer& Buf, UMaterialInterface* Material)
	{
		FMeshDescription MD;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();
		TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
		VUV.SetNumChannels(1);
		const FPolygonGroupID Group = MD.CreatePolygonGroup();
		Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Road");
		MD.ReserveNewVertices(Buf.Vertices.Num());
		MD.ReserveNewVertexInstances(Buf.Vertices.Num());
		MD.ReserveNewTriangles(Buf.Indices.Num() / 3);

		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(Buf.Vertices.Num());
		for (const FRoadVertex& V : Buf.Vertices)
		{
			const FVertexID Id = MD.CreateVertex();
			VPos[Id] = GenCoords::ToUEf(V.Pos.X, V.Pos.Y, V.Pos.Z);
			const FVertexInstanceID I = MD.CreateVertexInstance(Id);
			VNrm[I] = V.Normal;
			VTan[I] = FVector3f::CrossProduct(FVector3f(0.f, 1.f, 0.f), V.Normal).GetSafeNormal();
			VSign[I] = 1.f;
			VUV.Set(I, 0, V.UV);
			Instances.Add(I);
		}
		for (int32 i = 0; i + 2 < Buf.Indices.Num(); i += 3)
		{
			const FVertexInstanceID Tri[3] = { Instances[Buf.Indices[i]], Instances[Buf.Indices[i + 1]], Instances[Buf.Indices[i + 2]] };
			MD.CreateTriangle(Group, Tri);
		}

		UStaticMesh* SM = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
		SM->GetStaticMaterials().Add(GenRuntimeMaterialSlot(Material, TEXT("Road")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		SM->BuildFromMeshDescriptions({ &MD }, Params);
		return SM;
	}
}

// ---------------------------------------------------------------------------------------------

void UGenRoadComponent::Clear()
{
	for (UStaticMeshComponent* C : Meshes)
	{
		if (C) { C->DestroyComponent(); }
	}
	Meshes.Reset();
	Materials.Reset();
}

void UGenRoadComponent::Build(const FGenMapData& Map, const UGenTerrainComponent* Terrain)
{
	Clear();
	const double Start = FPlatformTime::Seconds();
	FRoadLayout Layout;
	Layout.Load(Map);
	if (Layout.NumRoads == 0)
	{
		return;
	}

	// W3DRoadBuffer::drawRoads: by stacking order, then road type, then kind of piece.
	TArray<FRoadPiece> Pieces;
	int32 NumOfKind[RC_Num] = {};
	int32 MaxStacking = 0;
	for (const FRoadTypeSlot& S : Layout.Types)
	{
		MaxStacking = FMath::Max(MaxStacking, S.Stacking);
	}
	for (int32 Stacking = 0; Stacking <= MaxStacking; ++Stacking)
	{
		for (int32 t = 0; t < Layout.Types.Num(); ++t)
		{
			if (Layout.Types[t].Stacking != Stacking)
			{
				continue;
			}
			for (int32 Corner = 0; Corner < RC_Num; ++Corner)
			{
				for (int32 r = 0; r < Layout.NumRoads; ++r)
				{
					const FRoadSeg& R = Layout.Roads[r];
					if (R.Type != Corner || R.UniqueID != Layout.Types[t].UniqueID)
					{
						continue;
					}
					FRoadPiece P;
					if (Layout.MakePiece(R, P))
					{
						P.TypeSlot = t;
						Pieces.Add(P);
						++NumOfKind[Corner];
					}
				}
			}
		}
	}

	// A piece lies a step above whatever it is drawn over.
	TArray<TStaticArray<FRoadVec, 4>> Rings;
	Rings.SetNum(Pieces.Num());
	int32 MaxLayer = 0;
	for (int32 i = 0; i < Pieces.Num(); ++i)
	{
		FRoadPiece& P = Pieces[i];
		Rings[i][0] = P.Corners[RoadBL];
		Rings[i][1] = P.Corners[RoadBR];
		Rings[i][2] = P.Corners[RoadTR];
		Rings[i][3] = P.Corners[RoadTL];
		for (int32 j = 0; j < i; ++j)
		{
			const FRoadPiece& Q = Pieces[j];
			if (Q.Layer < P.Layer || !P.Bounds.Intersect(Q.Bounds))
			{
				continue;
			}
			if (RoadQuadsOverlap(Rings[i].GetData(), Rings[j].GetData(), 0.3f))
			{
				P.Layer = Q.Layer + 1;
			}
		}
		MaxLayer = FMath::Max(MaxLayer, P.Layer);
	}

	// One mesh for each road type in each block of the map.
	FRoadDraper Draper(Map, Terrain);
	TMap<uint64, FRoadMeshBuffer> Buffers;
	for (const FRoadPiece& P : Pieces)
	{
		const FRoadVec Centre = P.Bounds.GetCenter();
		const int32 CX = FMath::Clamp(FMath::FloorToInt32(Centre.X / GenCoords::MapXYFactor) + Map.Border, 0, Map.Width - 1) / RoadChunkCells;
		const int32 CY = FMath::Clamp(FMath::FloorToInt32(Centre.Y / GenCoords::MapXYFactor) + Map.Border, 0, Map.Height - 1) / RoadChunkCells;
		const uint64 Key = (uint64(P.TypeSlot) << 32) | (uint64(CY) << 16) | uint64(CX);
		FRoadMeshBuffer& Buf = Buffers.FindOrAdd(Key);
		Buf.TypeSlot = P.TypeSlot;
		Draper.Drape(P, Buf);
	}

	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	UMaterialInterface* Parent = Assets->GetNamedMaterial(TEXT("M_Road"));
	TMap<int32, UMaterialInstanceDynamic*> TypeMaterials;
	TArray<FString> Missing;
	for (const TPair<uint64, FRoadMeshBuffer>& It : Buffers)
	{
		const FRoadMeshBuffer& Buf = It.Value;
		if (Buf.Indices.Num() == 0)
		{
			continue;
		}
		UMaterialInstanceDynamic*& Material = TypeMaterials.FindOrAdd(Buf.TypeSlot);
		if (!Material)
		{
			const FGenRoadType* Def = Layout.Types[Buf.TypeSlot].Def;
			Material = UMaterialInstanceDynamic::Create(Parent, this);
			if (UTexture2D* Tex = Assets->GetTexture(Def->Texture))
			{
				Material->SetTextureParameterValue(TEXT("RoadTexture"), Tex);
			}
			else
			{
				Missing.AddUnique(Def->Texture);
			}
			UGenTerrainComponent::ApplyCloudAndLightMap(Material, Map); // (W3DRoadBuffer::drawRoads: the same cloud and noise stages)
			Materials.Add(Material);
		}
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetOwner() ? (UObject*)GetOwner() : (UObject*)this, NAME_None, RF_Transient);
		C->SetStaticMesh(BuildRoadMesh(this, Buf, Material));
		C->SetMaterial(0, Material);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCanEverAffectNavigation(false);
		C->SetCastShadow(false);
		C->SetupAttachment(this);
		C->RegisterComponent();
		Meshes.Add(C);
	}

	UE_LOG(LogGenerals, Log, TEXT("Roads: %d straight pieces, %d curve pieces, %d T, %d four-way, %d Y, %d slanted T, %d joins; %d road types (stacked up to %d), %d layers, %d triangles in %d meshes, %.0f ms%s"),
		NumOfKind[RC_Segment], NumOfKind[RC_Curve], NumOfKind[RC_Tee], NumOfKind[RC_FourWay], NumOfKind[RC_ThreeWayY], NumOfKind[RC_ThreeWayH] + NumOfKind[RC_ThreeWayHFlip],
		NumOfKind[RC_AlphaJoin], TypeMaterials.Num(), MaxStacking, MaxLayer + 1, Draper.NumTriangles, Meshes.Num(), (FPlatformTime::Seconds() - Start) * 1000.0,
		Missing.Num() > 0 ? *(TEXT("; MISSING textures: ") + FString::Join(Missing, TEXT(", "))) : TEXT(""));
}
