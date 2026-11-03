#include "Map/GenRandomMapInternal.h"
#include "Core/GenCoords.h"
#include "Core/GenFileSystem.h"
#include "Game/GenShellData.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapData.h"
#include "Map/GenRoads.h"
#include "W3D/GenTextureLoader.h"
#include "Generals1.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// A random map's terrain and what stands on it, out of its plan (GenRandomMap.cpp). What comes out is an FGenMapData like the
// one read from a .map file: heights, tiles and blends the way the World Builder paints them (WHeightMapEdit.cpp), waypoints and
// areas under the names the skirmish scripts look for, roads, bridges and objects as map objects. The rest of the game doesn't
// know the difference. The rule of the whole generator holds here: see GenRandomMapInternal.h.

namespace GenRandomMapDetail
{
	namespace
	{
		constexpr float GroundZ = 44.f;             // the level the land lies around (raw height 70; the official maps: 16 to 100)
		constexpr float BedZ = 5.f;                 // the bottom of the water
		constexpr float StepZ = 36.f;               // a plateau's height: more than the 9.8 a cell that is still passable may rise
		constexpr int32 BorderCells = 40;           // what is shown beyond the playable area
		constexpr float BaseFlat = 360.f;           // around a start position: flat...
		constexpr float BaseFade = 620.f;           // ... and how far out that fades into the land
		constexpr float CorridorHalf = 130.f;       // the ways between places: this far to either side the ground is smoothed...
		constexpr float CorridorFade = 130.f;       // ... and that fades out over this.
		constexpr float CellSize = GenCoords::MapXYFactor;

		float Dist2D(const FVector2f& A, const FVector2f& B)
		{
			const float DX = A.X - B.X, DY = A.Y - B.Y;
			return FMath::Sqrt(DX * DX + DY * DY);
		}

		float DistToSeg(const FVector2f& P, const FVector2f& A, const FVector2f& B, float* OutT = nullptr)
		{
			const FVector2f AB = B - A;
			const float Len2 = AB.X * AB.X + AB.Y * AB.Y;
			const float T = Len2 > 0.001f ? FMath::Clamp(((P.X - A.X) * AB.X + (P.Y - A.Y) * AB.Y) / Len2, 0.f, 1.f) : 0.f;
			if (OutT) { *OutT = T; }
			return Dist2D(P, FVector2f(A.X + AB.X * T, A.Y + AB.Y * T));
		}

		/** A box blur, along the rows and then the columns (the sums in doubles: the same on every computer, and they don't drift). */
		void BoxBlur(TArray<float>& Grid, int32 W, int32 H, int32 Radius)
		{
			TArray<float> Temp;
			Temp.SetNumUninitialized(W * H);
			const double Norm = 1.0 / double(2 * Radius + 1);
			for (int32 Y = 0; Y < H; ++Y)
			{
				double Sum = 0.0;
				for (int32 K = -Radius; K <= Radius; ++K)
				{
					Sum += Grid[Y * W + FMath::Clamp(K, 0, W - 1)];
				}
				for (int32 X = 0; X < W; ++X)
				{
					Temp[Y * W + X] = float(Sum * Norm);
					Sum += Grid[Y * W + FMath::Min(X + Radius + 1, W - 1)] - Grid[Y * W + FMath::Max(X - Radius, 0)];
				}
			}
			for (int32 X = 0; X < W; ++X)
			{
				double Sum = 0.0;
				for (int32 K = -Radius; K <= Radius; ++K)
				{
					Sum += Temp[FMath::Clamp(K, 0, H - 1) * W + X];
				}
				for (int32 Y = 0; Y < H; ++Y)
				{
					Grid[Y * W + X] = float(Sum * Norm);
					Sum += Temp[FMath::Min(Y + Radius + 1, H - 1) * W + X] - Temp[FMath::Max(Y - Radius, 0) * W + X];
				}
			}
		}

		/** How far every cell is from the cells at 0 (10 to a cell: world units, near enough). */
		void ChamferDistance(TArray<int32>& D, int32 W, int32 H)
		{
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					int32 V = D[Y * W + X];
					if (X > 0) { V = FMath::Min(V, D[Y * W + X - 1] + 10); }
					if (Y > 0)
					{
						V = FMath::Min(V, D[(Y - 1) * W + X] + 10);
						if (X > 0) { V = FMath::Min(V, D[(Y - 1) * W + X - 1] + 14); }
						if (X + 1 < W) { V = FMath::Min(V, D[(Y - 1) * W + X + 1] + 14); }
					}
					D[Y * W + X] = V;
				}
			}
			for (int32 Y = H - 1; Y >= 0; --Y)
			{
				for (int32 X = W - 1; X >= 0; --X)
				{
					int32 V = D[Y * W + X];
					if (X + 1 < W) { V = FMath::Min(V, D[Y * W + X + 1] + 10); }
					if (Y + 1 < H)
					{
						V = FMath::Min(V, D[(Y + 1) * W + X] + 10);
						if (X + 1 < W) { V = FMath::Min(V, D[(Y + 1) * W + X + 1] + 14); }
						if (X > 0) { V = FMath::Min(V, D[(Y + 1) * W + X - 1] + 14); }
					}
					D[Y * W + X] = V;
				}
			}
		}

		/** The width in tiles of a terrain class: its picture, 64 pixels to the tile. */
		int32 GetTerrainWidth(const FString& Name)
		{
			static TMap<FString, int32> Known;
			if (const int32* Found = Known.Find(Name))
			{
				return *Found;
			}
			int32 Width = 0;
			if (const FGenTerrainType* Type = FGenGameData::Get().FindTerrain(Name))
			{
				TArray<uint8> Bytes;
				if (FGenFileSystem::Get().ReadFile(TEXT("Art\\Terrain\\") + Type->Texture, Bytes) && Bytes.Num() > 18)
				{
					const int32 PixelsX = int32(Bytes[12]) | (int32(Bytes[13]) << 8);
					const int32 PixelsY = int32(Bytes[14]) | (int32(Bytes[15]) << 8);
					Width = FMath::Min(PixelsX, PixelsY) / 64;
				}
			}
			Known.Add(Name, Width);
			return Width;
		}

		/** The ground plate that goes with a building, where there is one. */
		const TCHAR* FindBib(const FString& Building)
		{
			static const TCHAR* Pairs[][2] = {
				{ TEXT("GermanTownHouse1"), TEXT("GermanTownHouseBib1") }, { TEXT("GermanTownHouse2"), TEXT("GermanTownHouseBib2") },
				{ TEXT("GermanPub02"), TEXT("GermanPubBib2") }, { TEXT("GermanCotage01"), TEXT("GermanCottageBib1") },
				{ TEXT("MogadishuHouse02"), TEXT("MogadishuHouse02Bib") }, { TEXT("MogadishuHouse03"), TEXT("MogadishuHouse03Bib") }, { TEXT("MogadishuHouse04"), TEXT("MogadishuHouse04Bib") },
				{ TEXT("MogadishuTower05"), TEXT("MogadishuTower05Bib") }, { TEXT("MogadishuTower04"), TEXT("MogadishuTower04Bib") },
				{ TEXT("StanWatchTowerTall"), TEXT("StanWatchTowerTallBib") }, { TEXT("TechHospital"), TEXT("TechHospitalBib") },
				{ TEXT("IndustrialBuilding02"), TEXT("IndustrialBuilding02Bib") }, { TEXT("AsianBank"), TEXT("AsianBankBib") },
			};
			for (const auto& Pair : Pairs)
			{
				if (Building.Equals(Pair[0], ESearchCase::IgnoreCase))
				{
					return Pair[1];
				}
			}
			return nullptr;
		}

		const TCHAR* SiteTemplate(FPlanSite::EKind Kind)
		{
			switch (Kind)
			{
			case FPlanSite::Dock: return TEXT("SupplyDock");
			case FPlanSite::Pile: return TEXT("SupplyPileSmall");
			case FPlanSite::Derrick: return TEXT("TechOilDerrick");
			case FPlanSite::Refinery: return TEXT("TechOilRefinery");
			case FPlanSite::Artillery: return TEXT("TechArtilleryPlatform");
			case FPlanSite::Reinforcement: return TEXT("TechReinforcementPad");
			case FPlanSite::Hospital: return TEXT("TechHospital");
			case FPlanSite::RepairBay: return TEXT("TechRepairbay");
			default: return TEXT("CivilianBunker01");
			}
		}

		float SitePadRadius(FPlanSite::EKind Kind)
		{
			switch (Kind)
			{
			case FPlanSite::Dock: return 95.f;
			case FPlanSite::Pile: return 30.f;
			case FPlanSite::Derrick: return 45.f;
			case FPlanSite::Bunker: return 32.f;
			default: return 80.f;
			}
		}

		struct FRoadPiece
		{
			const TCHAR* Type = nullptr;
			FVector2f A = FVector2f::ZeroVector, B = FVector2f::ZeroVector;
			bool bJoinA = false, bJoinB = false;
		};

		struct FTownStreets
		{
			TArray<FRoadPiece> Pieces;
			TArray<FVector2f> Vertices;
		};

		class FMapBuilder
		{
		public:
			FMapBuilder(const FPlan& InPlan, FGenMapData& InMap, TArray<FString>* InProblems)
				: Plan(InPlan), Env(GetEnvironmentDefs()[InPlan.Environment]), Map(InMap), Problems(InProblems)
			{
				B = BorderCells;
				W = Plan.CellsX + 2 * B;
				H = Plan.CellsY + 2 * B;
				bWater = Plan.bLake || Plan.bCoast || Plan.Rivers.Num() > 0;
			}

			void Run()
			{
				LayoutRoads();
				ShapeTerrain();
				SetupMapBasics();
				CheckConnections();
				PaintGround();
				PlaceWaypointsAndAreas();
				PlaceRoadsAndBridges();
				MarkTaken();
				PlaceSites();
				PlaceTowns();
				PlaceNature();
				PlaceAmbience();
				SetupWorld();
			}

			int32 NumTrees = 0, NumBuildings = 0, NumCarved = 0;

		private:
			const FPlan& Plan;
			const FEnvironmentDef& Env;
			FGenMapData& Map;
			TArray<FString>* Problems;
			int32 W = 0, H = 0, B = 0;
			bool bWater = false;
			TArray<float> Z;                        // the ground at each height map vertex (world units)
			TArray<float> WaterSdf;                 // how far from the water each vertex is (negative: in it)
			TArray<int32> CorridorDist;             // ... from the ways between places
			TArray<uint8> Taken;                    // cells something stands on, or that must stay free
			TArray<uint8> NoNature;                 // cells no tree or rock should stand on
			TArray<FRoadPiece> RoadPieces;
			TArray<FTownStreets> Towns;
			TArray<TArray<FVector2f>> ExtraPaths;   // ways the terrain must keep open besides the plan's
			TArray<uint8> ClassGrid;
			int32 NextWaypointId = 1;
			int32 NextTriggerId = 1;
			float BaseZ = GroundZ;
			// The terrain classes of the map, in the order they paint over one another.
			int32 ClsBed = 0, ClsShore = 0, ClsGround[3] = { 0, 0, 0 }, ClsPatch = 0, ClsCrops = -1, ClsDirt = 0, ClsBase = 0, ClsTown = 0, ClsRock = 0, ClsCliff = 0;
			TMap<uint64, int32> BlendIndex;

			void Problem(const FString& What)
			{
				if (Problems)
				{
					Problems->Add(What);
				}
			}

			int32 Index(int32 X, int32 Y) const { return Y * W + X; }
			float WorldX(int32 X) const { return float(X - B) * CellSize; }
			float WorldY(int32 Y) const { return float(Y - B) * CellSize; }
			int32 CellX(float WX) const { return FMath::Clamp(FMath::FloorToInt32(WX / CellSize) + B, 0, W - 1); }
			int32 CellY(float WY) const { return FMath::Clamp(FMath::FloorToInt32(WY / CellSize) + B, 0, H - 1); }

			float SampleZ(const FVector2f& P) const { return Z[Index(CellX(P.X), CellY(P.Y))]; }

			// ------------------------------------------------------------------------------- roads

			const TCHAR* ValidRoad(const TCHAR* Name) const
			{
				return (Name && FGenTerrainRoads::Get().FindRoad(Name)) ? Name : Env.RoadMain;
			}

			void AddRoad(const TCHAR* Type, const FVector2f& A, const FVector2f& PointB, bool bJoinA = false, bool bJoinB = false)
			{
				if (Dist2D(A, PointB) < 25.f)
				{
					return;
				}
				FRoadPiece& Piece = RoadPieces.AddDefaulted_GetRef();
				Piece.Type = Type;
				Piece.A = A;
				Piece.B = PointB;
				Piece.bJoinA = bJoinA;
				Piece.bJoinB = bJoinB;
			}

			void LayoutStreets()
			{
				const TCHAR* TownRoad = ValidRoad(Env.RoadTown);
				const TCHAR* Avenue = ValidRoad(Env.RoadAvenue);
				Towns.SetNum(Plan.Towns.Num());
				for (int32 t = 0; t < Plan.Towns.Num(); ++t)
				{
					const FPlanTown& T = Plan.Towns[t];
					FTownStreets& S = Towns[t];
					if (T.bFarm)
					{
						continue;
					}
					auto AddStreet = [&S](const TCHAR* Type, const FVector2f& A, const FVector2f& PointB)
					{
						FRoadPiece& Piece = S.Pieces.AddDefaulted_GetRef();
						Piece.Type = Type;
						Piece.A = A;
						Piece.B = PointB;
						S.Vertices.AddUnique(A);
						S.Vertices.AddUnique(PointB);
					};
					if (T.bCity)
					{
						// A grid, the avenues through the middle.
						constexpr float Grid = 210.f;
						const int32 N = FMath::CeilToInt32(T.Radius / Grid);
						auto Inside = [&](int32 I, int32 J)
						{
							const float DX = float(I) * Grid, DY = float(J) * Grid;
							return DX * DX + DY * DY <= T.Radius * T.Radius;
						};
						for (int32 J = -N; J <= N; ++J)
						{
							for (int32 I = -N; I <= N; ++I)
							{
								if (!Inside(I, J))
								{
									continue;
								}
								const FVector2f Here = T.Center + FVector2f(float(I) * Grid, float(J) * Grid);
								if (Inside(I + 1, J))
								{
									AddStreet(J == 0 ? Avenue : TownRoad, Here, T.Center + FVector2f(float(I + 1) * Grid, float(J) * Grid));
								}
								if (Inside(I, J + 1))
								{
									AddStreet(I == 0 ? Avenue : TownRoad, Here, T.Center + FVector2f(float(I) * Grid, float(J + 1) * Grid));
								}
							}
						}
					}
					else
					{
						// A main street through the middle and, in a bigger village, one across it.
						const FVector2f Axis = T.bVertical ? FVector2f(0.f, 1.f) : FVector2f(1.f, 0.f);
						const FVector2f Across = T.bVertical ? FVector2f(1.f, 0.f) : FVector2f(0.f, 1.f);
						const int32 Steps = FMath::Max(1, FMath::RoundToInt32(T.Radius / 95.f));
						for (int32 i = -Steps; i < Steps; ++i)
						{
							AddStreet(TownRoad, T.Center + Axis * (T.Radius * float(i) / float(Steps)), T.Center + Axis * (T.Radius * float(i + 1) / float(Steps)));
						}
						if (T.Radius >= 160.f)
						{
							const float CrossHalf = T.Radius * 0.72f;
							const int32 CrossSteps = FMath::Max(1, FMath::RoundToInt32(CrossHalf / 95.f));
							for (int32 i = -CrossSteps; i < CrossSteps; ++i)
							{
								AddStreet(TownRoad, T.Center + Across * (CrossHalf * float(i) / float(CrossSteps)), T.Center + Across * (CrossHalf * float(i + 1) / float(CrossSteps)));
							}
						}
					}
				}
			}

			int32 TownAt(const FVector2f& P) const
			{
				for (int32 t = 0; t < Plan.Towns.Num(); ++t)
				{
					if (!Plan.Towns[t].bFarm && Dist2D(P, Plan.Towns[t].Center) < Plan.Towns[t].Radius + 8.f)
					{
						return t;
					}
				}
				return INDEX_NONE;
			}

			FVector2f NearestVertex(int32 Town, const FVector2f& P) const
			{
				FVector2f Best = Plan.Towns[Town].Center;
				float BestDist = FLT_MAX;
				for (const FVector2f& V : Towns[Town].Vertices)
				{
					const float D = Dist2D(V, P);
					if (D < BestDist)
					{
						BestDist = D;
						Best = V;
					}
				}
				return Best;
			}

			bool NearBase(const FVector2f& P, float Radius) const
			{
				for (const FVector2f& S : Plan.Starts)
				{
					if (Dist2D(S, P) < Radius)
					{
						return true;
					}
				}
				return false;
			}

			void LayoutRoads()
			{
				LayoutStreets();
				const TCHAR* MainRoad = ValidRoad(Env.RoadMain);
				TArray<bool> HasRoad;
				for (const FPlanCorridor& C : Plan.Corridors)
				{
					HasRoad.Add(C.bRoad);
				}
				// A town no road leads to gets one, from the nearest way.
				TArray<TPair<int32, FVector2f>> Links;
				for (int32 t = 0; t < Plan.Towns.Num(); ++t)
				{
					const FPlanTown& T = Plan.Towns[t];
					if (T.bFarm)
					{
						continue;
					}
					bool bReached = false;
					for (int32 c = 0; c < Plan.Corridors.Num() && !bReached; ++c)
					{
						if (HasRoad[c])
						{
							for (const FVector2f& P : Plan.Corridors[c].Points)
							{
								bReached |= Dist2D(P, T.Center) < T.Radius + 8.f;
							}
						}
					}
					if (bReached)
					{
						continue;
					}
					float BestDist = FLT_MAX;
					int32 BestCorridor = INDEX_NONE;
					FVector2f BestPoint = FVector2f::ZeroVector;
					for (int32 c = 0; c < Plan.Corridors.Num(); ++c)
					{
						for (const FVector2f& P : Plan.Corridors[c].Points)
						{
							const float D = Dist2D(P, T.Center);
							if (D < BestDist && !NearBase(P, 320.f) && TownAt(P) == INDEX_NONE)
							{
								BestDist = D;
								BestCorridor = c;
								BestPoint = P;
							}
						}
					}
					if (BestCorridor != INDEX_NONE)
					{
						HasRoad[BestCorridor] = true;
						Links.Emplace(t, BestPoint);
					}
				}
				for (int32 c = 0; c < Plan.Corridors.Num(); ++c)
				{
					if (!HasRoad[c])
					{
						continue;
					}
					const TArray<FVector2f>& Pts = Plan.Corridors[c].Points;
					for (int32 i = 0; i + 1 < Pts.Num(); ++i)
					{
						const FVector2f& P = Pts[i];
						const FVector2f& Q = Pts[i + 1];
						if (NearBase(P, 300.f) || NearBase(Q, 300.f))
						{
							continue;
						}
						const int32 TP = TownAt(P), TQ = TownAt(Q);
						if (TP == INDEX_NONE && TQ == INDEX_NONE)
						{
							AddRoad(MainRoad, P, Q);
						}
						else if (TP == INDEX_NONE)
						{
							AddRoad(MainRoad, P, NearestVertex(TQ, P), false, true);
						}
						else if (TQ == INDEX_NONE)
						{
							AddRoad(MainRoad, NearestVertex(TP, Q), Q, true, false);
						}
					}
				}
				for (const TPair<int32, FVector2f>& Link : Links)
				{
					const FVector2f Start = NearestVertex(Link.Key, Link.Value);
					TArray<FVector2f>& Path = ExtraPaths.AddDefaulted_GetRef();
					const int32 Pieces = FMath::Max(1, FMath::RoundToInt32(Dist2D(Start, Link.Value) / 220.f));
					for (int32 i = 0; i <= Pieces; ++i)
					{
						Path.Add(Start + (Link.Value - Start) * (float(i) / float(Pieces)));
					}
					for (int32 i = 0; i + 1 < Path.Num(); ++i)
					{
						AddRoad(MainRoad, Path[i], Path[i + 1], i == 0, false);
					}
				}
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					if (C.bFord)
					{
						AddRoad(MainRoad, C.From, C.To);
					}
				}
				for (const FTownStreets& S : Towns)
				{
					RoadPieces.Append(S.Pieces);
				}
			}

			// ------------------------------------------------------------------------------- the land

			void MarkPath(TArray<int32>& Field, const TArray<FVector2f>& Path) const
			{
				for (int32 i = 0; i + 1 < Path.Num(); ++i)
				{
					const float Len = Dist2D(Path[i], Path[i + 1]);
					const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Len / 5.f));
					for (int32 k = 0; k <= Steps; ++k)
					{
						const FVector2f P = Path[i] + (Path[i + 1] - Path[i]) * (float(k) / float(Steps));
						Field[Index(CellX(P.X), CellY(P.Y))] = 0;
					}
				}
			}

			void ApplyPad(const FVector2f& Center, float Inner, float Outer, float TargetZ)
			{
				const int32 X0 = CellX(Center.X - Outer), X1 = CellX(Center.X + Outer);
				const int32 Y0 = CellY(Center.Y - Outer), Y1 = CellY(Center.Y + Outer);
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						const float D = Dist2D(FVector2f(WorldX(X), WorldY(Y)), Center);
						const float Mask = 1.f - Smooth(Inner, Outer, D);
						if (Mask > 0.f)
						{
							float& Cell = Z[Index(X, Y)];
							Cell += (TargetZ - Cell) * Mask;
						}
					}
				}
			}

			void MakeBaseHeights()
			{
				const uint32 Seed = Plan.Seed * 2654435761u;
				float AmpBig = 12.f, AmpMid = 4.5f;
				switch (Plan.Layout)
				{
				case ELayout::Highlands: AmpBig = 8.f; AmpMid = 4.f; break;
				case ELayout::Canyons: AmpBig = 7.f; AmpMid = 3.f; break;
				case ELayout::City: AmpBig = 6.f; AmpMid = 2.5f; break;
				default: break;
				}
				AmpBig *= Env.Hilliness;
				AmpMid *= Env.Hilliness;
				const bool bDunes = Env.bSnowy || FCString::Stricmp(Env.Key, TEXT("desert")) == 0;
				Z.SetNumUninitialized(W * H);
				ParallelHeights(Seed, AmpBig, AmpMid, bDunes);
				if (Plan.Layout == ELayout::Highlands)
				{
					// (The edge of a plateau over about three cells: too steep to climb, not a wall of one cell.)
					BoxBlur(Z, W, H, 1);
					BaseZ = GroundZ + StepZ;
				}
				else
				{
					BaseZ = GroundZ + 4.f;
				}
			}

			void ParallelHeights(uint32 Seed, float AmpBig, float AmpMid, bool bDunes)
			{
				for (int32 Y = 0; Y < H; ++Y)
				{
					const float WY = WorldY(Y);
					for (int32 X = 0; X < W; ++X)
					{
						const float WX = WorldX(X);
						const float Big = FractalNoise(WX / 1150.f, WY / 1150.f, Seed ^ 0x1111u, 3);
						const float Mid = FractalNoise(WX / 330.f, WY / 330.f, Seed ^ 0x2222u, 3);
						float Height = GroundZ + Big * AmpBig + Mid * AmpMid;
						if (bDunes)
						{
							// Long low waves of sand or drift.
							Height += FractalNoise(WX / 150.f, WY / 420.f, Seed ^ 0x3333u, 2) * 2.2f;
						}
						switch (Plan.Layout)
						{
						case ELayout::Highlands:
						{
							const float T = FractalNoise(WX / 780.f, WY / 780.f, Seed ^ 0x4444u, 3) + 0.22f * FractalNoise(WX / 170.f, WY / 170.f, Seed ^ 0x5555u, 2);
							Height += (T > 0.1f ? StepZ : 0.f) + (T > 0.34f ? StepZ : 0.f);
							break;
						}
						case ELayout::Canyons:
						{
							// A wall of rock around each base, the gates are the ways out of it (they are smoothed afterwards).
							float Wall = 0.f;
							const float Wander = FractalNoise(WX / 260.f, WY / 260.f, Seed ^ 0x6666u, 2) * 65.f;
							for (const FVector2f& S : Plan.Starts)
							{
								const float D = Dist2D(FVector2f(WX, WY), S);
								Wall = FMath::Max(Wall, 1.f - Smooth(48.f, 74.f, FMath::Abs(D - 680.f - Wander)));
							}
							Height += Wall * (46.f + 10.f * Mid);
							break;
						}
						case ELayout::Plains:
						case ELayout::Lake:
						case ELayout::Rivers:
						case ELayout::Coast:
						{
							// Now and then a rock standing out of the land.
							const float T = FractalNoise(WX / 460.f, WY / 460.f, Seed ^ 0x7777u, 2);
							Height += Smooth(0.4f, 0.43f, T) * 30.f;
							break;
						}
						default:
							break;
						}
						if (Plan.bCoast)
						{
							const float Inland = (WX - Plan.CoastPoint.X) * Plan.CoastInland.X + (WY - Plan.CoastPoint.Y) * Plan.CoastInland.Y;
							Height += 14.f * Smooth(100.f, 1600.f, Inland);
						}
						Z[Index(X, Y)] = Height;
					}
				}
			}

			void SmoothCorridors()
			{
				// The ways between places: whatever cliffs lie across them become ramps.
				CorridorDist.Init(1000000, W * H);
				for (const FPlanCorridor& C : Plan.Corridors)
				{
					MarkPath(CorridorDist, C.Points);
				}
				for (const TArray<FVector2f>& Path : ExtraPaths)
				{
					MarkPath(CorridorDist, Path);
				}
				ChamferDistance(CorridorDist, W, H);
				TArray<float> Blurred = Z;
				BoxBlur(Blurred, W, H, 12);
				BoxBlur(Blurred, W, H, 12);
				for (int32 i = 0; i < Z.Num(); ++i)
				{
					const float Mask = 1.f - Smooth(CorridorHalf, CorridorHalf + CorridorFade, float(CorridorDist[i]));
					Z[i] += (Blurred[i] - Z[i]) * Mask;
				}
			}

			void CarveWater()
			{
				WaterSdf.Init(1.e6f, W * H);
				if (!bWater)
				{
					return;
				}
				const uint32 Seed = Plan.Seed * 40503u;
				TArray<int32> RiverDist;
				if (Plan.Rivers.Num() > 0)
				{
					RiverDist.Init(1000000, W * H);
					for (const TArray<FVector2f>& River : Plan.Rivers)
					{
						MarkPath(RiverDist, River);
					}
					ChamferDistance(RiverDist, W, H);
				}
				float ShoreSlope = 0.24f;
				if (Plan.bLake) { ShoreSlope = 0.2f; }
				if (Plan.bCoast) { ShoreSlope = 0.16f; }
				for (int32 Y = 0; Y < H; ++Y)
				{
					const float WY = WorldY(Y);
					for (int32 X = 0; X < W; ++X)
					{
						const float WX = WorldX(X);
						float D = 1.e6f;
						if (Plan.bLake)
						{
							const FVector2f P(WX, WY);
							D = FMath::Min(D, Dist2D(P, Plan.LakeCenter) - Plan.LakeRadius * (1.f + 0.26f * FractalNoise(WX / 420.f, WY / 420.f, Seed ^ 0x8888u, 2)));
						}
						if (RiverDist.Num() > 0)
						{
							const float Wide = Plan.RiverHalfWidth * (1.f + 0.25f * FractalNoise(WX / 180.f, WY / 180.f, Seed ^ 0x9999u, 2));
							D = FMath::Min(D, float(RiverDist[Index(X, Y)]) - Wide);
						}
						if (Plan.bCoast)
						{
							const float Inland = (WX - Plan.CoastPoint.X) * Plan.CoastInland.X + (WY - Plan.CoastPoint.Y) * Plan.CoastInland.Y;
							D = FMath::Min(D, Inland + 75.f * FractalNoise(WX / 520.f, WY / 520.f, Seed ^ 0xAAAAu, 2));
						}
						WaterSdf[Index(X, Y)] = D;
						float& Cell = Z[Index(X, Y)];
						if (D <= 0.f)
						{
							Cell = FMath::Max(BedZ, WaterZ - 1.5f + D * 0.12f);
						}
						else
						{
							Cell = FMath::Min(Cell, WaterZ + 1.2f + D * ShoreSlope);
						}
					}
				}
			}

			void ShapeTerrain()
			{
				MakeBaseHeights();
				SmoothCorridors();
				CarveWater();
				for (const FVector2f& S : Plan.Starts)
				{
					ApplyPad(S, BaseFlat, BaseFade, BaseZ);
				}
				for (const FPlanTown& T : Plan.Towns)
				{
					const float Level = FMath::Max(SampleZ(T.Center), bWater ? WaterZ + 9.f : 0.f);
					ApplyPad(T.Center, T.Radius + 30.f, T.Radius + 140.f, Level);
				}
				for (const FPlanSite& S : Plan.Sites)
				{
					const float Level = FMath::Max(SampleZ(S.Pos), bWater ? WaterZ + 8.f : 0.f);
					const float Pad = SitePadRadius(S.Kind);
					ApplyPad(S.Pos, Pad, Pad + 70.f, Level);
				}
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					if (C.bFord)
					{
						// A sand bar across the water.
						const int32 X0 = CellX(FMath::Min(C.From.X, C.To.X) - 120.f), X1 = CellX(FMath::Max(C.From.X, C.To.X) + 120.f);
						const int32 Y0 = CellY(FMath::Min(C.From.Y, C.To.Y) - 120.f), Y1 = CellY(FMath::Max(C.From.Y, C.To.Y) + 120.f);
						for (int32 Y = Y0; Y <= Y1; ++Y)
						{
							for (int32 X = X0; X <= X1; ++X)
							{
								const float D = DistToSeg(FVector2f(WorldX(X), WorldY(Y)), C.From, C.To);
								const float Mask = 1.f - Smooth(60.f, 105.f, D);
								float& Cell = Z[Index(X, Y)];
								if (Mask > 0.f && Cell < WaterZ + 2.f)
								{
									Cell += (WaterZ + 2.f - Cell) * Mask;
								}
							}
						}
					}
					else
					{
						// Both banks level, that is what the bridge's ends stand on.
						const float Level = FMath::Max((SampleZ(C.From) + SampleZ(C.To)) * 0.5f, WaterZ + 13.f);
						ApplyPad(C.From, 60.f, 150.f, Level);
						ApplyPad(C.To, 60.f, 150.f, Level);
					}
				}
				// Beyond the playable area the land rises a little.
				const uint32 Seed = Plan.Seed * 69069u;
				for (int32 Y = 0; Y < H; ++Y)
				{
					for (int32 X = 0; X < W; ++X)
					{
						const int32 OutX = FMath::Max(B - X, X - (W - B - 1));
						const int32 OutY = FMath::Max(B - Y, Y - (H - B - 1));
						const int32 Out = FMath::Max(OutX, OutY);
						if (Out > 0 && WaterSdf[Index(X, Y)] > 80.f)
						{
							Z[Index(X, Y)] += Smooth(0.f, float(B), float(Out)) * (26.f + 14.f * FractalNoise(WorldX(X) / 240.f, WorldY(Y) / 240.f, Seed, 2));
						}
					}
				}
				Quantize(0, 0, W - 1, H - 1);
			}

			/** The heights as the map stores them: 8 bits, 16 to ten world units. What everything after this works with. */
			void Quantize(int32 X0, int32 Y0, int32 X1, int32 Y1)
			{
				Map.Heights.SetNumZeroed(W * H);
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						float& Cell = Z[Index(X, Y)];
						const int32 Raw = FMath::Clamp(FMath::RoundToInt32(Cell / GenCoords::MapHeightScale), 0, 255);
						Map.Heights[Index(X, Y)] = uint8(Raw);
						Cell = float(Raw) * GenCoords::MapHeightScale;
					}
				}
			}

			void SetupMapBasics()
			{
				Map.Path = Plan.Params.ToName();
				Map.Width = W;
				Map.Height = H;
				Map.Border = B;
				Map.Boundaries.Reset();
				Map.Boundaries.Add(FIntPoint(Plan.CellsX, Plan.CellsY));
				Map.ActiveBoundary = 0;
				Map.CliffStride = (W + 7) / 8;
				Map.CliffState.SetNumZeroed(Map.CliffStride * H);
				Map.InitCliffFlagsFromHeights();
			}

			// ------------------------------------------------------------------------------- can everybody get everywhere?

			bool IsDry(int32 X, int32 Y) const
			{
				if (!bWater)
				{
					return true;
				}
				const int32 X1 = FMath::Min(X + 1, W - 1), Y1 = FMath::Min(Y + 1, H - 1);
				return Z[Index(X, Y)] >= WaterZ && Z[Index(X1, Y)] >= WaterZ && Z[Index(X, Y1)] >= WaterZ && Z[Index(X1, Y1)] >= WaterZ;
			}

			bool IsWalkable(int32 X, int32 Y) const
			{
				return !Map.IsCliffCellIndex(X, Y) && IsDry(X, Y);
			}

			void FloodFrom(const FVector2f& Start, TArray<uint8>& Reached) const
			{
				// As the pathfinder sees the ground: cliffs and water stop it, a bridge's deck is a way across.
				Reached.Init(0, W * H);
				TArray<uint8> Deck;
				Deck.Init(0, W * H);
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					if (C.bFord)
					{
						continue;
					}
					const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Dist2D(C.From, C.To) / 5.f));
					for (int32 k = 0; k <= Steps; ++k)
					{
						const FVector2f P = C.From + (C.To - C.From) * (float(k) / float(Steps));
						Deck[Index(CellX(P.X), CellY(P.Y))] = 1;
					}
				}
				TArray<int32> Queue;
				const int32 First = Index(CellX(Start.X), CellY(Start.Y));
				Queue.Add(First);
				Reached[First] = 1;
				const int32 MinX = B, MaxX = W - B - 1, MinY = B, MaxY = H - B - 1;
				for (int32 Head = 0; Head < Queue.Num(); ++Head)
				{
					const int32 Cell = Queue[Head];
					const int32 CX = Cell % W, CY = Cell / W;
					const int32 NX[4] = { CX - 1, CX + 1, CX, CX };
					const int32 NY[4] = { CY, CY, CY - 1, CY + 1 };
					for (int32 k = 0; k < 4; ++k)
					{
						if (NX[k] < MinX || NX[k] > MaxX || NY[k] < MinY || NY[k] > MaxY)
						{
							continue;
						}
						const int32 Next = Index(NX[k], NY[k]);
						if (!Reached[Next] && (Deck[Next] || IsWalkable(NX[k], NY[k])))
						{
							Reached[Next] = 1;
							Queue.Add(Next);
						}
					}
				}
			}

			/** A straight way from A to B: level, dry, nothing in its way. The last resort; a plan shouldn't need it. */
			void CarveWay(const FVector2f& A, const FVector2f& PointB)
			{
				constexpr float Half = 90.f, Fade = 150.f;
				const int32 X0 = CellX(FMath::Min(A.X, PointB.X) - Fade), X1 = CellX(FMath::Max(A.X, PointB.X) + Fade);
				const int32 Y0 = CellY(FMath::Min(A.Y, PointB.Y) - Fade), Y1 = CellY(FMath::Max(A.Y, PointB.Y) + Fade);
				const float ZA = FMath::Max(SampleZ(A), bWater ? WaterZ + 3.f : 0.f);
				const float ZB = FMath::Max(SampleZ(PointB), bWater ? WaterZ + 3.f : 0.f);
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						float T = 0.f;
						const float D = DistToSeg(FVector2f(WorldX(X), WorldY(Y)), A, PointB, &T);
						const float Mask = 1.f - Smooth(Half, Fade, D);
						if (Mask > 0.f)
						{
							float& Cell = Z[Index(X, Y)];
							Cell += (ZA + (ZB - ZA) * T - Cell) * Mask;
						}
					}
				}
				Quantize(X0, Y0, X1, Y1);
				Map.CliffState.Init(0, Map.CliffStride * H);
				Map.InitCliffFlagsFromHeights();
				++NumCarved;
			}

			void CheckConnections()
			{
				// Every player must be able to reach every other, and every supply dock.
				for (int32 Round = 0; Round < 8; ++Round)
				{
					TArray<uint8> Reached;
					FloodFrom(Plan.Starts[0], Reached);
					auto IsReached = [&](const FVector2f& P)
					{
						const int32 CX = CellX(P.X), CY = CellY(P.Y);
						// (A site's own cell may be a little off a slope: anything right next to it will do.)
						for (int32 DY = -3; DY <= 3; ++DY)
						{
							for (int32 DX = -3; DX <= 3; ++DX)
							{
								if (Reached[Index(FMath::Clamp(CX + DX, 0, W - 1), FMath::Clamp(CY + DY, 0, H - 1))])
								{
									return true;
								}
							}
						}
						return false;
					};
					bool bAll = true;
					FVector2f Cut = FVector2f::ZeroVector;
					for (const FVector2f& S : Plan.Starts)
					{
						if (!IsReached(S))
						{
							Cut = S;
							bAll = false;
							break;
						}
					}
					for (int32 i = 0; i < Plan.Sites.Num() && bAll; ++i)
					{
						const FPlanSite& S = Plan.Sites[i];
						if (S.Kind != FPlanSite::Pile && S.Kind != FPlanSite::Bunker && !IsReached(S.Pos))
						{
							Cut = S.Pos;
							bAll = false;
						}
					}
					if (bAll)
					{
						return;
					}
					// From the cut-off place to the nearest place that is reached, straight through.
					FVector2f Nearest = Plan.Starts[0];
					float BestDist = Dist2D(Cut, Nearest);
					for (int32 Y = B; Y < H - B; Y += 4)
					{
						for (int32 X = B; X < W - B; X += 4)
						{
							if (Reached[Index(X, Y)])
							{
								const FVector2f P(WorldX(X), WorldY(Y));
								const float D = Dist2D(P, Cut);
								if (D < BestDist)
								{
									BestDist = D;
									Nearest = P;
								}
							}
						}
					}
					CarveWay(Cut, Nearest);
				}
				Problem(TEXT("some places can't be reached after carving ways to them"));
			}

			// ------------------------------------------------------------------------------- the ground's colours (WHeightMapEdit.cpp)

			int32 AddClass(const TCHAR* Name, int32 Fallback)
			{
				if (!Name)
				{
					return Fallback;
				}
				const int32 Known = Map.TextureClasses.IndexOfByPredicate([Name](const FGenTextureClass& C) { return C.Name.Equals(Name, ESearchCase::IgnoreCase); });
				if (Known != INDEX_NONE)
				{
					return Known;
				}
				const int32 Width = GetTerrainWidth(Name);
				if (Width <= 0)
				{
					Problem(FString::Printf(TEXT("no terrain class '%s'"), Name));
					return Fallback;
				}
				int32 First = 0;
				if (Map.TextureClasses.Num() > 0)
				{
					First = Map.TextureClasses.Last().FirstTile + Map.TextureClasses.Last().NumTiles;
				}
				FGenTextureClass& C = Map.TextureClasses.AddDefaulted_GetRef();
				C.FirstTile = First;
				C.NumTiles = Width * Width;
				C.Width = Width;
				C.Name = Name;
				return Map.TextureClasses.Num() - 1;
			}

			/** WorldHeightMapEdit::getTileNdxForClass: a cell is a quarter of a tile. */
			int32 TileFor(int32 X, int32 Y, int32 Class) const
			{
				const FGenTextureClass& C = Map.TextureClasses[Class];
				int32 Tile = C.FirstTile + ((X / 2) % C.Width) + C.Width * ((Y / 2) % C.Width);
				Tile = Tile << 2;
				Tile += 2 * (Y & 1);
				Tile += X & 1;
				return Tile;
			}

			int32 FindOrCreateBlend(const FGenBlendedTile& Info)
			{
				const uint64 Key = uint64(uint32(Info.BlendNdx)) | (uint64(Info.Horiz) << 32) | (uint64(Info.Vert) << 33) | (uint64(Info.RightDiagonal) << 34)
					| (uint64(Info.LeftDiagonal) << 35) | (uint64(Info.Inverted) << 36) | (uint64(Info.LongDiagonal) << 40);
				if (const int32* Found = BlendIndex.Find(Key))
				{
					return *Found;
				}
				const int32 New = Map.BlendedTiles.Add(Info);
				BlendIndex.Add(Key, New);
				return New;
			}

			/** WorldHeightMapEdit::blendSpecificTiles: the tile blended over this cell, from the side of (SrcX, SrcY). */
			void BlendSpecific(int32 X, int32 Y, int32 SrcX, int32 SrcY, int32 BlendTile, bool bLongDiagonal)
			{
				constexpr uint8 FlippedMask = 0x2;
				FGenBlendedTile Info;
				Info.BlendNdx = BlendTile;
				Info.LongDiagonal = bLongDiagonal ? 1 : 0;
				Info.CustomBlendEdgeClass = -1;
				const int32 Ndx = Index(X, Y);
				// "Check if there is already a blend tile at the destination and record its flip state."
				bool bBaseNeedsFlip = false, bBaseIsDiagonal = false, bHasBase = false;
				FGenBlendedTile Base;
				if (Map.BlendNdx[Ndx] != 0)
				{
					Base = Map.BlendedTiles[Map.BlendNdx[Ndx]];
					bHasBase = true;
					bBaseIsDiagonal = Base.RightDiagonal || Base.LeftDiagonal;
					bBaseNeedsFlip = (Base.RightDiagonal && !Base.Inverted) || (Base.LeftDiagonal && Base.Inverted);
				}
				bool bFlipped = false;
				if (SrcY == Y)
				{
					Info.Horiz = 1;
					Info.Inverted = SrcX < X ? 1 : 0;
					if (bHasBase && bBaseNeedsFlip) { Info.Inverted |= FlippedMask; }
				}
				else if (SrcX == X)
				{
					Info.Vert = 1;
					Info.Inverted = SrcY < Y ? 1 : 0;
					if (bHasBase && bBaseNeedsFlip) { Info.Inverted |= FlippedMask; }
				}
				else
				{
					if (SrcX > X) { Info.RightDiagonal = 1; } else { Info.LeftDiagonal = 1; }
					Info.Inverted = SrcY < Y ? 1 : 0;
					if (bLongDiagonal)
					{
						Info.Inverted = Info.Inverted ? 0 : 1;
						Info.RightDiagonal = Info.RightDiagonal ? 0 : 1;
						Info.LeftDiagonal = Info.LeftDiagonal ? 0 : 1;
					}
					bFlipped = (Info.RightDiagonal && !Info.Inverted) || (Info.LeftDiagonal && Info.Inverted);
					if (bHasBase && bBaseIsDiagonal && bBaseNeedsFlip != bFlipped)
					{
						return; // "the base requires a certain flip state which we can't alter so can't apply extra blend here"
					}
				}
				if (bHasBase && Base.BlendNdx == BlendTile)
				{
					return;
				}
				const int32 New = FindOrCreateBlend(Info);
				if (bHasBase)
				{
					Map.ExtraBlendNdx[Ndx] = int16(New);
					if (bFlipped && !bBaseIsDiagonal)
					{
						FGenBlendedTile Flipped = Base;
						Flipped.Inverted |= FlippedMask;
						Map.BlendNdx[Ndx] = int16(FindOrCreateBlend(Flipped));
					}
				}
				else
				{
					Map.BlendNdx[Ndx] = int16(New);
				}
			}

			/** WorldHeightMapEdit::blendToThisClass: this cell is edged into by the cells of that class around it. */
			void BlendToClass(int32 X, int32 Y, int32 Class)
			{
				int32 Sides = 0;
				bool bTop = false, bBottom = false, bLeft = false, bRight = false;
				int32 CornerX = INDEX_NONE, CornerY = INDEX_NONE, SideX = INDEX_NONE, SideY = INDEX_NONE;
				for (int32 I = X - 1; I <= X + 1; ++I)
				{
					for (int32 J = Y - 1; J <= Y + 1; ++J)
					{
						if ((I == X && J == Y) || I < 0 || J < 0 || I >= W || J >= H || ClassGrid[Index(I, J)] != uint8(Class))
						{
							continue;
						}
						if (I == X || J == Y)
						{
							++Sides;
							if (SideX == INDEX_NONE) { SideX = I; SideY = J; }
							if (I == X) { if (J > Y) { bTop = true; } else { bBottom = true; } }
							else { if (I < X) { bLeft = true; } else { bRight = true; } }
						}
						else if (CornerX == INDEX_NONE)
						{
							CornerX = I;
							CornerY = J;
						}
					}
				}
				const int32 Tile = TileFor(X, Y, Class);
				if (Sides == 0 && CornerX != INDEX_NONE)
				{
					BlendSpecific(X, Y, CornerX, CornerY, Tile, false);
				}
				else if (Sides == 1)
				{
					BlendSpecific(X, Y, SideX, SideY, Tile, false);
				}
				else if (Sides == 2)
				{
					// "Note - we are doing a long diagonal blend on purpose."
					int32 SrcX = X, SrcY = Y;
					if (bTop) { --SrcY; }
					if (bBottom) { ++SrcY; }
					if (bLeft) { ++SrcX; }
					if (bRight) { --SrcX; }
					if (SrcX != X && SrcY != Y)
					{
						BlendSpecific(X, Y, SrcX, SrcY, Tile, true);
					}
				}
			}

			void PaintGround()
			{
				// In the order they paint over one another: a class edges into those before it on this list.
				ClsBed = AddClass(Env.Bed, 0);
				ClsShore = AddClass(Env.Shore, ClsBed);
				ClsGround[0] = AddClass(Env.Ground[0], ClsShore);
				ClsGround[1] = Env.Ground.Num() > 1 ? AddClass(Env.Ground[1], ClsGround[0]) : ClsGround[0];
				ClsGround[2] = Env.Ground.Num() > 2 ? AddClass(Env.Ground[2], ClsGround[1]) : ClsGround[1];
				ClsPatch = AddClass(Env.Patch, ClsGround[0]);
				ClsCrops = Env.Crops ? AddClass(Env.Crops, ClsGround[0]) : -1;
				ClsDirt = AddClass(Env.Dirt, ClsGround[0]);
				ClsBase = AddClass(Env.Base, ClsDirt);
				ClsTown = AddClass(Env.Town, ClsDirt);
				ClsRock = AddClass(Env.Rock, ClsDirt);
				ClsCliff = AddClass(Env.Cliff, ClsRock);
				if (Map.TextureClasses.Num() == 0)
				{
					Problem(TEXT("no terrain class at all"));
					return;
				}

				TArray<int32> RoadDist;
				RoadDist.Init(1000000, W * H);
				for (const FRoadPiece& Piece : RoadPieces)
				{
					TArray<FVector2f> Line;
					Line.Add(Piece.A);
					Line.Add(Piece.B);
					MarkPath(RoadDist, Line);
				}
				ChamferDistance(RoadDist, W, H);

				const uint32 Seed = Plan.Seed * 22695477u;
				ClassGrid.SetNumZeroed(W * H);
				for (int32 Y = 0; Y < H; ++Y)
				{
					const float WY = WorldY(Y) + 5.f;
					for (int32 X = 0; X < W; ++X)
					{
						const float WX = WorldX(X) + 5.f;
						const int32 X1 = FMath::Min(X + 1, W - 1), Y1 = FMath::Min(Y + 1, H - 1);
						const float H0 = Z[Index(X, Y)], H1 = Z[Index(X1, Y)], H2 = Z[Index(X, Y1)], H3 = Z[Index(X1, Y1)];
						const float Lo = FMath::Min(FMath::Min(H0, H1), FMath::Min(H2, H3));
						const float Hi = FMath::Max(FMath::Max(H0, H1), FMath::Max(H2, H3));
						const float Range = Hi - Lo;
						const float Fine = FractalNoise(WX / 70.f, WY / 70.f, Seed ^ 0x1234u, 2);
						int32 Class;
						if (bWater && Hi < WaterZ - 0.4f)
						{
							Class = ClsBed;
						}
						else if (bWater && Lo < WaterZ + 2.2f + 2.f * Fine)
						{
							Class = ClsShore;
						}
						else if (bWater && WaterSdf[Index(X, Y)] < 60.f + 30.f * Fine && Hi < WaterZ + 14.f)
						{
							Class = ClsShore;
						}
						else if (Range >= 7.f)
						{
							Class = ClsCliff;
						}
						else if (Range >= 3.4f + 0.8f * Fine)
						{
							Class = ClsRock;
						}
						else
						{
							Class = INDEX_NONE;
							const FVector2f P(WX, WY);
							// A town's paving, a farm's fields.
							for (const FPlanTown& T : Plan.Towns)
							{
								const float D = Dist2D(P, T.Center);
								if (T.bFarm)
								{
									if (ClsCrops >= 0)
									{
										const float Along = T.bVertical ? P.Y - T.Center.Y : P.X - T.Center.X;
										const float Side = T.bVertical ? P.X - T.Center.X : P.Y - T.Center.Y;
										if (FMath::Abs(Along) < T.Radius && Side > T.Radius * 0.1f && Side < T.Radius)
										{
											Class = ClsCrops;
										}
									}
								}
								else if (D < T.Radius * (T.bCity ? 1.02f : 0.75f) + 26.f * Fine)
								{
									Class = ClsTown;
								}
							}
							if (Class == INDEX_NONE)
							{
								const float Rough = FractalNoise(WX / 150.f, WY / 150.f, Seed ^ 0x2345u, 2);
								for (const FVector2f& S : Plan.Starts)
								{
									if (Dist2D(P, S) < 300.f + 110.f * Rough && Rough > -0.25f)
									{
										Class = ClsBase;
									}
								}
							}
							if (Class == INDEX_NONE && RoadDist[Index(X, Y)] < 22 + int32(14.f * Fine))
							{
								Class = ClsDirt;
							}
							if (Class == INDEX_NONE && Range >= 2.2f + 0.5f * Fine)
							{
								Class = ClsDirt;
							}
							if (Class == INDEX_NONE)
							{
								const float Mix = FractalNoise(WX / 520.f, WY / 520.f, Seed ^ 0x3456u, 3);
								const float Spot = FractalNoise(WX / 130.f, WY / 130.f, Seed ^ 0x4567u, 2);
								if (Spot > 0.3f)
								{
									Class = ClsPatch;
								}
								else if (Mix > 0.16f)
								{
									Class = ClsGround[1];
								}
								else if (Mix < -0.22f)
								{
									Class = ClsGround[2];
								}
								else
								{
									Class = ClsGround[0];
								}
							}
						}
						ClassGrid[Index(X, Y)] = uint8(Class);
					}
				}

				// autoBlendOut: "If it is mostly surrounded by us, convert it to us, cause if we blend in from 3 sides, it basically
				// obliterates it." The same goes for a strip of a cell between two parts of a class.
				for (int32 Pass = 0; Pass < 6; ++Pass)
				{
					bool bChanged = false;
					for (int32 Y = 0; Y < H; ++Y)
					{
						for (int32 X = 0; X < W; ++X)
						{
							const int32 Own = ClassGrid[Index(X, Y)];
							int32 Best = Own;
							for (int32 N = 0; N < 4; ++N)
							{
								static const int32 DX[4] = { -1, 1, 0, 0 };
								static const int32 DY[4] = { 0, 0, -1, 1 };
								const int32 NX = X + DX[N], NY = Y + DY[N];
								if (NX < 0 || NY < 0 || NX >= W || NY >= H)
								{
									continue;
								}
								const int32 Other = ClassGrid[Index(NX, NY)];
								if (Other <= Own || Other <= Best)
								{
									continue;
								}
								int32 Sides = 0, Total = 0;
								bool bLeft = false, bRight = false, bDown = false, bUp = false;
								for (int32 I = X - 1; I <= X + 1; ++I)
								{
									for (int32 J = Y - 1; J <= Y + 1; ++J)
									{
										if ((I == X && J == Y) || ClassGrid[Index(FMath::Clamp(I, 0, W - 1), FMath::Clamp(J, 0, H - 1))] != uint8(Other))
										{
											continue;
										}
										++Total;
										if (I == X || J == Y)
										{
											++Sides;
											bLeft |= I < X;
											bRight |= I > X;
											bDown |= J < Y;
											bUp |= J > Y;
										}
									}
								}
								if (Sides > 2 || Total > 5 || (bLeft && bRight) || (bDown && bUp))
								{
									Best = Other;
								}
							}
							if (Best != Own)
							{
								ClassGrid[Index(X, Y)] = uint8(Best);
								bChanged = true;
							}
						}
					}
					if (!bChanged)
					{
						break;
					}
				}

				Map.TileNdx.SetNumUninitialized(W * H);
				Map.BlendNdx.SetNumZeroed(W * H);
				Map.ExtraBlendNdx.SetNumZeroed(W * H);
				Map.CliffNdx.SetNumZeroed(W * H);
				Map.BlendedTiles.SetNum(1);
				for (int32 Y = 0; Y < H; ++Y)
				{
					for (int32 X = 0; X < W; ++X)
					{
						Map.TileNdx[Index(X, Y)] = int16(TileFor(X, Y, ClassGrid[Index(X, Y)]));
					}
				}
				// A cell takes the blends of the classes above it that touch it: only one way across an edge, or a seam shows.
				for (int32 Y = 0; Y < H; ++Y)
				{
					for (int32 X = 0; X < W; ++X)
					{
						const int32 Own = ClassGrid[Index(X, Y)];
						uint32 Mask = 0;
						for (int32 I = FMath::Max(X - 1, 0); I <= FMath::Min(X + 1, W - 1); ++I)
						{
							for (int32 J = FMath::Max(Y - 1, 0); J <= FMath::Min(Y + 1, H - 1); ++J)
							{
								const int32 Other = ClassGrid[Index(I, J)];
								if (Other > Own)
								{
									Mask |= 1u << Other;
								}
							}
						}
						for (int32 Class = Own + 1; Mask != 0 && Class < 32; ++Class)
						{
							if (Mask & (1u << Class))
							{
								BlendToClass(X, Y, Class);
								Mask &= ~(1u << Class);
							}
						}
					}
				}
			}

			// ------------------------------------------------------------------------------- map objects

			FGenMapObject& AddObject(const FString& Template, const FVector2f& Pos, float Radians, int32 Flags = 0)
			{
				FGenMapObject& O = Map.Objects.AddDefaulted_GetRef();
				O.Location = FVector3f(Pos.X, Pos.Y, 0.f);
				O.Angle = Radians;
				O.Flags = Flags;
				O.TemplateName = Template;
				return O;
			}

			static void SetString(FGenMapObject& O, const TCHAR* Key, const FString& Value)
			{
				FGenDictValue V;
				V.Type = FGenDictValue::EType::AsciiString;
				V.String = Value;
				O.Properties.Add(Key, V);
			}

			static void SetInt(FGenMapObject& O, const TCHAR* Key, int32 Value)
			{
				FGenDictValue V;
				V.Type = FGenDictValue::EType::Int;
				V.Int = Value;
				O.Properties.Add(Key, V);
			}

			/** Somewhere near that a unit can stand. */
			FVector2f FindDrySpot(const FVector2f& Pos) const
			{
				const FVector2f Clamped(FMath::Clamp(Pos.X, 30.f, Plan.SizeX - 30.f), FMath::Clamp(Pos.Y, 30.f, Plan.SizeY - 30.f));
				for (int32 Ring = 0; Ring < 24; ++Ring)
				{
					const float Radius = float(Ring) * 25.f;
					const int32 Steps = Ring == 0 ? 1 : 16;
					for (int32 k = 0; k < Steps; ++k)
					{
						float S, C;
						DetSinCos(float(k) / float(Steps), S, C);
						const FVector2f P(FMath::Clamp(Clamped.X + C * Radius, 30.f, Plan.SizeX - 30.f), FMath::Clamp(Clamped.Y + S * Radius, 30.f, Plan.SizeY - 30.f));
						if (IsWalkable(CellX(P.X), CellY(P.Y)))
						{
							return P;
						}
					}
				}
				return Clamped;
			}

			int32 AddWaypoint(const FString& Name, const FVector2f& Pos, const FString& PathLabel)
			{
				const int32 Id = NextWaypointId++;
				FGenMapObject& O = AddObject(TEXT("*Waypoints/Waypoint"), Pos, 0.f);
				SetInt(O, TEXT("waypointID"), Id);
				SetString(O, TEXT("waypointName"), Name);
				if (!PathLabel.IsEmpty())
				{
					SetString(O, TEXT("waypointPathLabel1"), PathLabel);
				}
				return Id;
			}

			void AddPath(const FString& Label, const TArray<FVector2f>& Points)
			{
				int32 Previous = INDEX_NONE;
				for (int32 i = 0; i < Points.Num(); ++i)
				{
					const int32 Id = AddWaypoint(FString::Printf(TEXT("%s_%d"), *Label, i), FindDrySpot(Points[i]), Label);
					if (Previous != INDEX_NONE)
					{
						Map.WaypointLinks.Add(FIntPoint(Previous, Id));
					}
					Previous = Id;
				}
			}

			void AddArea(const FString& Name, const FVector2f& Center, float Radius)
			{
				// An octagon, kept to the map.
				FGenPolygonTrigger T;
				T.Name = Name;
				T.ID = NextTriggerId++;
				for (int32 k = 0; k < 8; ++k)
				{
					float S, C;
					DetSinCos((float(k) + 0.5f) / 8.f, S, C);
					const float X = FMath::Clamp(Center.X + C * Radius, -20.f, Plan.SizeX + 20.f);
					const float Y = FMath::Clamp(Center.Y + S * Radius, -20.f, Plan.SizeY + 20.f);
					T.Points.Add(FIntVector(FMath::RoundToInt32(X), FMath::RoundToInt32(Y), 0));
				}
				Map.PolygonTriggers.Add(MoveTemp(T));
			}

			void PlaceWaypointsAndAreas()
			{
				// The names ScriptEngine and AISkirmishPlayer look for: where a base is, the ways into it (Center, Flank,
				// Backdoor: a computer player's attacks take them and its defences face them), its inner and outer perimeter.
				const float MinSize = FMath::Min(Plan.SizeX, Plan.SizeY);
				for (int32 i = 0; i < Plan.Starts.Num(); ++i)
				{
					const FVector2f Start = Plan.Starts[i];
					const int32 N = i + 1;
					AddWaypoint(FString::Printf(TEXT("Player_%d_Start"), N), Start, FString());
					FVector2f Inward = Plan.Middle - Start;
					const float Len = FMath::Sqrt(Inward.X * Inward.X + Inward.Y * Inward.Y);
					Inward = Len > 1.f ? Inward * (1.f / Len) : FVector2f(1.f, 0.f);
					const FVector2f Side(-Inward.Y, Inward.X);
					AddPath(FString::Printf(TEXT("Center%d"), N), { Plan.Middle, Start + Inward * 860.f, Start + Inward * 430.f });
					AddPath(FString::Printf(TEXT("Flank%d"), N), { Plan.Middle + Side * (MinSize * 0.34f), Start + (Inward * 0.5f + Side * 0.87f) * 900.f, Start + Side * 430.f });
					AddPath(FString::Printf(TEXT("Backdoor%d"), N), { Plan.Middle - Side * (MinSize * 0.34f), Start + (Inward * 0.5f - Side * 0.87f) * 900.f, Start - Side * 430.f });
					AddArea(FString::Printf(TEXT("InnerPerimeter%d"), N), Start, 640.f);
					AddArea(FString::Printf(TEXT("OuterPerimeter%d"), N), Start, 1080.f);
				}
				AddArea(TEXT("CombatZone"), Plan.Middle, MinSize * 0.33f);
				FGenPolygonTrigger World;
				World.Name = TEXT("SkirmishWorld");
				World.ID = NextTriggerId++;
				World.Points = { FIntVector(-20, -20, 0), FIntVector(int32(Plan.SizeX) + 20, -20, 0), FIntVector(int32(Plan.SizeX) + 20, int32(Plan.SizeY) + 20, 0), FIntVector(-20, int32(Plan.SizeY) + 20, 0) };
				Map.PolygonTriggers.Add(MoveTemp(World));
				if (bWater)
				{
					// As most official maps do: one area of water over all of the map, seen where the ground goes under it.
					FGenPolygonTrigger Water;
					Water.Name = TEXT("Water");
					Water.ID = NextTriggerId++;
					Water.bWater = true;
					const int32 Lo = -(B + 2) * int32(CellSize);
					const int32 HiX = (W - B + 2) * int32(CellSize), HiY = (H - B + 2) * int32(CellSize);
					const int32 Level = FMath::RoundToInt32(WaterZ);
					Water.Points = { FIntVector(Lo, Lo, Level), FIntVector(HiX, Lo, Level), FIntVector(HiX, HiY, Level), FIntVector(Lo, HiY, Level) };
					Map.PolygonTriggers.Add(MoveTemp(Water));
				}
			}

			void PlaceRoadsAndBridges()
			{
				constexpr int32 FlagJoin = 0x80;    // FLAG_ROAD_JOIN
				for (const FRoadPiece& Piece : RoadPieces)
				{
					AddObject(Piece.Type, Piece.A, 0.f, FGenMapObject::FlagRoadPoint1 | (Piece.bJoinA ? FlagJoin : 0));
					AddObject(Piece.Type, Piece.B, 0.f, FGenMapObject::FlagRoadPoint2 | (Piece.bJoinB ? FlagJoin : 0));
				}
				const TCHAR* BridgeType = FGenTerrainRoads::Get().FindBridge(Env.Bridge) ? Env.Bridge : TEXT("EuropeanBridgeWide");
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					if (!C.bFord)
					{
						AddObject(BridgeType, C.From, 0.f, FGenMapObject::FlagBridgePoint1);
						AddObject(BridgeType, C.To, 0.f, FGenMapObject::FlagBridgePoint2);
					}
				}
			}

			void MarkDisc(TArray<uint8>& Grid, const FVector2f& Center, float Radius) const
			{
				const int32 X0 = CellX(Center.X - Radius), X1 = CellX(Center.X + Radius);
				const int32 Y0 = CellY(Center.Y - Radius), Y1 = CellY(Center.Y + Radius);
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						if (Dist2D(FVector2f(WorldX(X) + 5.f, WorldY(Y) + 5.f), Center) <= Radius)
						{
							Grid[Index(X, Y)] = 1;
						}
					}
				}
			}

			void MarkRect(TArray<uint8>& Grid, const FVector2f& Center, float HalfX, float HalfY) const
			{
				const int32 X0 = CellX(Center.X - HalfX), X1 = CellX(Center.X + HalfX);
				const int32 Y0 = CellY(Center.Y - HalfY), Y1 = CellY(Center.Y + HalfY);
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						Grid[Index(X, Y)] = 1;
					}
				}
			}

			bool RectFree(const FVector2f& Center, float HalfX, float HalfY) const
			{
				if (Center.X - HalfX < 30.f || Center.Y - HalfY < 30.f || Center.X + HalfX > Plan.SizeX - 30.f || Center.Y + HalfY > Plan.SizeY - 30.f)
				{
					return false;
				}
				const int32 X0 = CellX(Center.X - HalfX), X1 = CellX(Center.X + HalfX);
				const int32 Y0 = CellY(Center.Y - HalfY), Y1 = CellY(Center.Y + HalfY);
				float Lo = FLT_MAX, Hi = -FLT_MAX;
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						if (Taken[Index(X, Y)])
						{
							return false;
						}
						Lo = FMath::Min(Lo, Z[Index(X, Y)]);
						Hi = FMath::Max(Hi, Z[Index(X, Y)]);
					}
				}
				return Hi - Lo < 6.f && (!bWater || Lo > WaterZ + 1.f);
			}

			void MarkTaken()
			{
				Taken.Init(0, W * H);
				NoNature.Init(0, W * H);
				// Roads stay free, the bases stay free for their owners to build on.
				for (const FRoadPiece& Piece : RoadPieces)
				{
					const FGenRoadType* Type = FGenTerrainRoads::Get().FindRoad(Piece.Type);
					const float Half = (Type ? Type->Width * 0.5f : 20.f) + 2.f;
					const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Dist2D(Piece.A, Piece.B) / 6.f));
					for (int32 k = 0; k <= Steps; ++k)
					{
						const FVector2f P = Piece.A + (Piece.B - Piece.A) * (float(k) / float(Steps));
						MarkDisc(Taken, P, Half);
						MarkDisc(NoNature, P, Half + 6.f);
					}
				}
				for (const FVector2f& S : Plan.Starts)
				{
					MarkDisc(Taken, S, BaseFlat + 40.f);
					MarkDisc(NoNature, S, BaseFlat + 80.f);
				}
				for (const FPlanSite& S : Plan.Sites)
				{
					MarkDisc(Taken, S.Pos, SitePadRadius(S.Kind));
					MarkDisc(NoNature, S.Pos, SitePadRadius(S.Kind) + 18.f);
				}
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					MarkDisc(Taken, C.From, 90.f);
					MarkDisc(Taken, C.To, 90.f);
					MarkDisc(NoNature, C.From, 110.f);
					MarkDisc(NoNature, C.To, 110.f);
				}
				for (const FPlanTown& T : Plan.Towns)
				{
					MarkDisc(NoNature, T.Center, T.Radius + (T.bFarm ? 10.f : 30.f));
				}
			}

			void PlaceSites()
			{
				for (const FPlanSite& S : Plan.Sites)
				{
					const FString Name = SiteTemplate(S.Kind);
					const float Radians = TurnsToRadians(S.Turns);
					if (const TCHAR* Bib = FindBib(Name))
					{
						AddObject(Bib, S.Pos, Radians);
					}
					AddObject(Name, S.Pos, Radians);
				}
			}

			// ------------------------------------------------------------------------------- towns

			static void HalfSize(const FString& Name, float& OutMajor, float& OutMinor)
			{
				OutMajor = OutMinor = 14.f;
				if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Name))
				{
					if (T->GeometryMajorRadius > 0.f)
					{
						OutMajor = T->GeometryMajorRadius;
						OutMinor = T->IsBox() ? T->GeometryMinorRadius : T->GeometryMajorRadius;
					}
				}
			}

			/** A model's front is its x axis; the axis that way, in turns. */
			static float FacingTurns(const FVector2f& Face)
			{
				if (Face.X > 0.5f) { return 0.f; }
				if (Face.Y > 0.5f) { return 0.25f; }
				if (Face.X < -0.5f) { return 0.5f; }
				return 0.75f;
			}

			const TCHAR* PickBuilding(const FPlanTown& T, const FVector2f& Pos, FDetRandom& R) const
			{
				if (T.bCity)
				{
					const float D = Dist2D(Pos, T.Center) / T.Radius;
					const float Roll = R.Frac();
					if (D < 0.5f)
					{
						return PickWeighted(Roll < 0.65f && Env.Large.Num() > 0 ? Env.Large : Env.Shops, R);
					}
					if (Roll < 0.15f && Env.Large.Num() > 0) { return PickWeighted(Env.Large, R); }
					return PickWeighted(Roll < 0.55f ? Env.Shops : Env.Houses, R);
				}
				return PickWeighted(R.Chance(0.25f) ? Env.Shops : Env.Houses, R);
			}

			void PlaceAlongStreet(const FRoadPiece& Piece, const FPlanTown& T, int32& InOutBudget, FDetRandom& R)
			{
				const FVector2f Delta = Piece.B - Piece.A;
				const float Len = FMath::Sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y);
				if (Len < 30.f)
				{
					return;
				}
				const FVector2f Dir = Delta * (1.f / Len);
				const FVector2f Normal(-Dir.Y, Dir.X);
				const FGenRoadType* RoadType = FGenTerrainRoads::Get().FindRoad(Piece.Type);
				// (A cell's width off the road: what stands beside it must not reach the cells the road is on.)
				const float RoadHalf = (RoadType ? RoadType->Width * 0.5f : 18.f) + 12.f;
				for (float Side : { 1.f, -1.f })
				{
					float Along = R.Between(10.f, 36.f);
					int32 Guard = 0;
					while (Along < Len && InOutBudget > 0 && ++Guard < 24)
					{
						const FVector2f Probe = Piece.A + Dir * Along + Normal * (Side * (RoadHalf + 25.f));
						const FString Name = PickBuilding(T, Probe, R);
						if (Name.IsEmpty() || !FGenGameData::Get().FindTemplate(Name))
						{
							Along += 14.f;
							continue;
						}
						float Major, Minor;
						HalfSize(Name, Major, Minor);
						const FVector2f Center = Piece.A + Dir * (Along + Minor) + Normal * (Side * (RoadHalf + Major));
						const FVector2f Face = Normal * -Side;
						const bool bFaceAlongX = FMath::Abs(Face.X) > 0.5f;
						const float HalfX = (bFaceAlongX ? Major : Minor) + 3.f, HalfY = (bFaceAlongX ? Minor : Major) + 3.f;
						const bool bInTown = Dist2D(Center, T.Center) + FMath::Max(Major, Minor) < T.Radius + 90.f;
						if (bInTown && RectFree(Center, HalfX, HalfY))
						{
							const float Radians = TurnsToRadians(FacingTurns(Face));
							if (const TCHAR* Bib = FindBib(Name))
							{
								AddObject(Bib, Center, Radians);
							}
							AddObject(Name, Center, Radians);
							MarkRect(Taken, Center, HalfX, HalfY);
							++NumBuildings;
							--InOutBudget;
							Along += 2.f * Minor + R.Between(8.f, 30.f);
						}
						else
						{
							Along += 14.f;
						}
					}
				}
			}

			void PlaceFarm(const FPlanTown& T, FDetRandom& R)
			{
				// The fields lie on one side (PaintGround), the yard on the other.
				const FVector2f Along = T.bVertical ? FVector2f(0.f, 1.f) : FVector2f(1.f, 0.f);
				const FVector2f Across = T.bVertical ? FVector2f(1.f, 0.f) : FVector2f(0.f, 1.f);
				const int32 Count = R.Range(2, 3);
				for (int32 i = 0; i < Count; ++i)
				{
					const FString Name = PickWeighted(Env.FarmBuildings, R);
					if (Name.IsEmpty() || !FGenGameData::Get().FindTemplate(Name))
					{
						continue;
					}
					float Major, Minor;
					HalfSize(Name, Major, Minor);
					const FVector2f Center = T.Center + Along * ((float(i) - float(Count - 1) * 0.5f) * 75.f) - Across * (T.Radius * 0.5f + Major * 0.4f);
					const FVector2f Face = Across;
					const bool bFaceAlongX = FMath::Abs(Face.X) > 0.5f;
					const float HalfX = (bFaceAlongX ? Major : Minor) + 3.f, HalfY = (bFaceAlongX ? Minor : Major) + 3.f;
					if (RectFree(Center, HalfX, HalfY))
					{
						AddObject(Name, Center, TurnsToRadians(FacingTurns(Face)));
						MarkRect(Taken, Center, HalfX, HalfY);
						++NumBuildings;
					}
				}
				const int32 Props = R.Range(3, 7);
				for (int32 i = 0; i < Props && Env.FarmProps.Num() > 0; ++i)
				{
					const FVector2f Pos = T.Center + Along * R.Between(-T.Radius, T.Radius) + Across * R.Between(-T.Radius * 0.2f, T.Radius);
					if (RectFree(Pos, 8.f, 8.f))
					{
						AddObject(PickWeighted(Env.FarmProps, R), Pos, TurnsToRadians(R.Frac()));
						MarkRect(Taken, Pos, 8.f, 8.f);
					}
				}
			}

			void PlaceTowns()
			{
				FDetRandom R(Plan.Seed, 204u);
				for (int32 t = 0; t < Plan.Towns.Num(); ++t)
				{
					const FPlanTown& T = Plan.Towns[t];
					if (T.bFarm)
					{
						PlaceFarm(T, R);
						continue;
					}
					const FTownStreets& S = Towns[t];
					// A landmark first, where it fits, near the middle: not on a street.
					if (Env.Landmarks.Num() > 0 && R.Chance(T.bCity ? 0.9f : 0.35f))
					{
						const FString Name = Env.Landmarks[R.Range(0, Env.Landmarks.Num() - 1)];
						if (FGenGameData::Get().FindTemplate(Name))
						{
							float Major, Minor;
							HalfSize(Name, Major, Minor);
							const float Half = FMath::Max(Major, Minor) + 4.f;
							for (int32 Try = 0; Try < 12; ++Try)
							{
								const FVector2f Pos = T.Center + FVector2f(R.Between(-1.f, 1.f), R.Between(-1.f, 1.f)) * (T.Radius * 0.55f);
								if (RectFree(Pos, Half, Half))
								{
									AddObject(Name, Pos, TurnsToRadians(float(R.Range(0, 3)) * 0.25f));
									MarkRect(Taken, Pos, Half, Half);
									break;
								}
							}
						}
					}
					// As many buildings as an official map has in such a place (Tournament City: 123, a village: a dozen). They go up from
					// the middle outwards, not quite in order, and the streets further out are left with gaps.
					const float Cells = float(Plan.CellsX) * float(Plan.CellsY);
					int32 Budget = T.bCity ? FMath::Clamp(FMath::RoundToInt32(Cells * 0.0006f), 50, 140) : FMath::Clamp(FMath::RoundToInt32(T.Radius / 14.f), 6, 18);
					TArray<TPair<float, int32>> Order;
					for (int32 p = 0; p < S.Pieces.Num(); ++p)
					{
						const FVector2f PieceMiddle = (S.Pieces[p].A + S.Pieces[p].B) * 0.5f;
						Order.Emplace(Dist2D(PieceMiddle, T.Center) + R.Between(0.f, T.Radius * 0.7f), p);
					}
					Order.StableSort([](const TPair<float, int32>& A, const TPair<float, int32>& C) { return A.Key < C.Key; });
					for (const TPair<float, int32>& Item : Order)
					{
						PlaceAlongStreet(S.Pieces[Item.Value], T, Budget, R);
					}
					// What stands by the road: lamps, stalls, parked cars.
					for (const FRoadPiece& Piece : S.Pieces)
					{
						const FVector2f Delta = Piece.B - Piece.A;
						const float Len = FMath::Sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y);
						if (Len < 30.f)
						{
							continue;
						}
						const FVector2f Dir = Delta * (1.f / Len);
						const FVector2f Normal(-Dir.Y, Dir.X);
						const FGenRoadType* RoadType = FGenTerrainRoads::Get().FindRoad(Piece.Type);
						const float RoadHalf = (RoadType ? RoadType->Width * 0.5f : 18.f) + 7.f;
						const float Side = R.Chance(0.5f) ? 1.f : -1.f;
						if (Env.StreetLamp)
						{
							const FVector2f Lamp = Piece.A + Dir * (Len * 0.5f) + Normal * (Side * RoadHalf);
							if (RectFree(Lamp, 3.f, 3.f))
							{
								AddObject(Env.StreetLamp, Lamp, TurnsToRadians(FacingTurns(Normal * -Side)));
								MarkRect(Taken, Lamp, 3.f, 3.f);
							}
						}
						if (R.Chance(0.45f) && Env.StreetProps.Num() > 0)
						{
							const FVector2f Prop = Piece.A + Dir * R.Between(0.f, Len) + Normal * (-Side * (RoadHalf + 3.f));
							if (RectFree(Prop, 7.f, 7.f))
							{
								AddObject(PickWeighted(Env.StreetProps, R), Prop, TurnsToRadians(FacingTurns(Normal * Side)));
								MarkRect(Taken, Prop, 7.f, 7.f);
							}
						}
						if (R.Chance(0.25f) && Env.Cars.Num() > 0)
						{
							const FVector2f Car = Piece.A + Dir * R.Between(0.f, Len) + Normal * (Side * (RoadHalf + 7.f));
							const bool bAlongX = FMath::Abs(Dir.X) > 0.5f;
							if (RectFree(Car, bAlongX ? 13.f : 7.f, bAlongX ? 7.f : 13.f))
							{
								AddObject(PickWeighted(Env.Cars, R), Car, TurnsToRadians(FacingTurns(Dir)));
								MarkRect(Taken, Car, bAlongX ? 13.f : 7.f, bAlongX ? 7.f : 13.f);
							}
						}
					}
					// And what stands in the yards behind.
					const int32 Yards = FMath::RoundToInt32(T.Radius * (T.bCity ? 0.05f : 0.035f));
					for (int32 i = 0; i < Yards && Env.YardProps.Num() > 0; ++i)
					{
						const FVector2f Pos = T.Center + FVector2f(R.Between(-1.f, 1.f), R.Between(-1.f, 1.f)) * T.Radius;
						if (Dist2D(Pos, T.Center) < T.Radius && RectFree(Pos, 8.f, 8.f))
						{
							AddObject(PickWeighted(Env.YardProps, R), Pos, TurnsToRadians(R.Frac()));
							MarkRect(Taken, Pos, 8.f, 8.f);
						}
					}
					// A watchtower at the edge of it.
					if (Env.Towers.Num() > 0 && R.Chance(0.7f))
					{
						const FString Name = PickWeighted(Env.Towers, R);
						float Major, Minor;
						HalfSize(Name, Major, Minor);
						for (int32 Try = 0; Try < 10 && FGenGameData::Get().FindTemplate(Name); ++Try)
						{
							float SinV, CosV;
							DetSinCos(R.Frac(), SinV, CosV);
							const FVector2f Pos = T.Center + FVector2f(CosV, SinV) * (T.Radius + 40.f);
							const float Half = FMath::Max(Major, Minor) + 3.f;
							if (RectFree(Pos, Half, Half))
							{
								const float Radians = TurnsToRadians(float(R.Range(0, 3)) * 0.25f);
								if (const TCHAR* Bib = FindBib(Name))
								{
									AddObject(Bib, Pos, Radians);
								}
								AddObject(Name, Pos, Radians);
								MarkRect(Taken, Pos, Half, Half);
								break;
							}
						}
					}
				}
			}

			// ------------------------------------------------------------------------------- what grows

			float SlopeAt(int32 X, int32 Y) const
			{
				const int32 X1 = FMath::Min(X + 1, W - 1), Y1 = FMath::Min(Y + 1, H - 1);
				const float H0 = Z[Index(X, Y)], H1 = Z[Index(X1, Y)], H2 = Z[Index(X, Y1)], H3 = Z[Index(X1, Y1)];
				return FMath::Max(FMath::Max(H0, H1), FMath::Max(H2, H3)) - FMath::Min(FMath::Min(H0, H1), FMath::Min(H2, H3));
			}

			void PlaceNature()
			{
				float LayoutFactor = 1.f;
				switch (Plan.Layout)
				{
				case ELayout::Highlands: LayoutFactor = 0.8f; break;
				case ELayout::Lake: LayoutFactor = 0.9f; break;
				case ELayout::Canyons: LayoutFactor = 0.6f; break;
				case ELayout::Coast: LayoutFactor = 0.85f; break;
				case ELayout::City: LayoutFactor = 0.55f; break;
				default: break;
				}
				const float Cells = float(Plan.CellsX) * float(Plan.CellsY);
				const int32 MaxTrees = FMath::RoundToInt32(Cells * 0.018f * Env.Forest * LayoutFactor);
				const uint32 Seed = Plan.Seed * 134775813u;
				// Woods where the noise says so, a tree here and there elsewhere; a wood is mostly of one kind.
				FDetRandom R(Plan.Seed, 201u);
				// (About 8 trees to 1000 cells come of these numbers. No more than 1500 in all, the most an official map of such a size
				// has: a big map's woods are thinner, not cut off at the last row.)
				const float Thin = FMath::Min(1.f, 1500.f / FMath::Max(1.f, Cells * 0.008f * Env.Forest * LayoutFactor));
				const float InWood = 0.46f * Env.Forest * LayoutFactor * Thin;
				const float Stray = 0.012f * Env.Forest * Thin;
				constexpr float Step = 30.f;
				for (float WY = 15.f; WY < Plan.SizeY - 10.f; WY += Step)
				{
					for (float WX = 15.f; WX < Plan.SizeX - 10.f; WX += Step)
					{
						const FVector2f P(WX + R.Between(-11.f, 11.f), WY + R.Between(-11.f, 11.f));
						const float Roll = R.Frac();
						const uint32 PickSeed = R.Next();
						const int32 CX = CellX(P.X), CY = CellY(P.Y);
						const int32 Cell = Index(CX, CY);
						if (Taken[Cell] || NoNature[Cell] || NumTrees >= MaxTrees)
						{
							continue;
						}
						if ((bWater && Z[Cell] < WaterZ + 4.f) || SlopeAt(CX, CY) > 6.f)
						{
							continue;
						}
						const float Wood = FractalNoise(P.X / 400.f, P.Y / 400.f, Seed ^ 0xBEEFu, 3);
						// (The ways between places stay mostly clear.)
						const float Open = Smooth(50.f, CorridorHalf, float(CorridorDist[Cell]));
						const float Chance = (Wood > 0.1f ? InWood * Smooth(0.1f, 0.26f, Wood) : Stray) * (0.12f + 0.88f * Open);
						if (Roll >= Chance)
						{
							continue;
						}
						FDetRandom Pick(PickSeed, 7u);
						FDetRandom Grove(uint32(FMath::FloorToInt32(P.X / 260.f)) * 7919u + uint32(FMath::FloorToInt32(P.Y / 260.f)) * 104729u + Plan.Seed, 11u);
						const TCHAR* Tree = Pick.Chance(0.65f) ? PickWeighted(Env.Trees, Grove) : PickWeighted(Env.Trees, Pick);
						AddObject(Tree, P, TurnsToRadians(Pick.Frac()));
						++NumTrees;
					}
				}
				FDetRandom RB(Plan.Seed, 202u);
				const int32 NumBushes = FMath::RoundToInt32(Cells * 0.0022f * (0.5f + 0.5f * Env.Forest));
				for (int32 i = 0; i < NumBushes && Env.Bushes.Num() > 0; ++i)
				{
					const FVector2f P(RB.Between(20.f, Plan.SizeX - 20.f), RB.Between(20.f, Plan.SizeY - 20.f));
					const int32 CX = CellX(P.X), CY = CellY(P.Y);
					if (!Taken[Index(CX, CY)] && !NoNature[Index(CX, CY)] && SlopeAt(CX, CY) < 5.f && (!bWater || Z[Index(CX, CY)] > WaterZ + 2.f))
					{
						AddObject(PickWeighted(Env.Bushes, RB), P, TurnsToRadians(RB.Frac()));
					}
				}
				// Rocks lie mostly where the land is steep.
				FDetRandom RR(Plan.Seed, 203u);
				const int32 NumRocks = FMath::RoundToInt32(Cells * 0.0026f);
				for (int32 i = 0; i < NumRocks && Env.Rocks.Num() > 0; ++i)
				{
					const FVector2f P(RR.Between(20.f, Plan.SizeX - 20.f), RR.Between(20.f, Plan.SizeY - 20.f));
					const int32 CX = CellX(P.X), CY = CellY(P.Y);
					const float Slope = SlopeAt(CX, CY);
					const bool bKeep = RR.Chance(Slope > 1.6f ? 0.9f : 0.16f);
					const bool bBig = RR.Chance(0.14f);
					const float Turn = RR.Frac();
					if (bKeep && !Taken[Index(CX, CY)] && !NoNature[Index(CX, CY)] && Slope < 9.f && (!bWater || Z[Index(CX, CY)] > WaterZ + 1.f)
						&& float(CorridorDist[Index(CX, CY)]) > 90.f)
					{
						AddObject(PickWeighted(bBig && Env.BigRocks.Num() > 0 ? Env.BigRocks : Env.Rocks, RR), P, TurnsToRadians(Turn));
					}
				}
			}

			void PlaceAmbience()
			{
				// The sounds of the place, as thinly spread as on the official maps.
				FDetRandom R(Plan.Seed, 205u);
				const TArray<const TCHAR*>& Land = Plan.TimeOfDay == 3 ? Env.AmbientNight : Env.AmbientDay;
				constexpr float Step = 520.f;
				for (float WY = 200.f; WY < Plan.SizeY - 100.f; WY += Step)
				{
					for (float WX = 200.f; WX < Plan.SizeX - 100.f; WX += Step)
					{
						const FVector2f P(WX + R.Between(-160.f, 160.f), WY + R.Between(-160.f, 160.f));
						const float Near = WaterSdf[Index(CellX(P.X), CellY(P.Y))];
						const int32 Pick = R.Range(0, 1000);
						if (Near < 140.f && Env.AmbientWater.Num() > 0)
						{
							AddObject(Env.AmbientWater[Pick % Env.AmbientWater.Num()], P, 0.f);
						}
						else if (Land.Num() > 0)
						{
							AddObject(Land[Pick % Land.Num()], P, 0.f);
						}
					}
				}
			}

			// ------------------------------------------------------------------------------- light and weather

			void SetupWorld()
			{
				FGenDictValue Weather;
				Weather.Type = FGenDictValue::EType::Int;
				Weather.Int = Env.bSnowy ? 1 : 0;
				Map.WorldInfo.Add(TEXT("weather"), Weather);
				Map.TimeOfDay = Plan.TimeOfDay + 1;
				Map.Lighting.SetNum(4);

				// The lights of an official map of this kind of place, at this time of day.
				static TMap<FString, TArray<FGenMapLighting>> Donors;
				const FString Donor = Env.LightDonors[Plan.TimeOfDay];
				if (!Donors.Contains(Donor))
				{
					FGenMapData DonorMap;
					TArray<FGenMapLighting> Lights;
					if (DonorMap.LoadFromVfs(FString::Printf(TEXT("Maps\\%s\\%s.map"), *Donor, *Donor)))
					{
						Lights = DonorMap.Lighting;
					}
					Donors.Add(Donor, MoveTemp(Lights));
				}
				const TArray<FGenMapLighting>& Lights = Donors[Donor];
				if (Lights.IsValidIndex(Plan.TimeOfDay))
				{
					Map.Lighting = Lights;
				}
				else
				{
					Problem(FString::Printf(TEXT("no lights from '%s'"), *Donor));
					static const float Ambient[4][3] = { { 0.5f, 0.39f, 0.3f }, { 0.22f, 0.2f, 0.17f }, { 0.25f, 0.23f, 0.2f }, { 0.1f, 0.1f, 0.15f } };
					static const float Diffuse[4][3] = { { 0.9f, 0.71f, 0.6f }, { 1.f, 1.f, 1.f }, { 0.6f, 0.5f, 0.4f }, { 0.2f, 0.2f, 0.3f } };
					for (int32 t = 0; t < 4; ++t)
					{
						FGenMapLight Light;
						Light.Ambient = FLinearColor(Ambient[t][0], Ambient[t][1], Ambient[t][2]);
						Light.Diffuse = FLinearColor(Diffuse[t][0], Diffuse[t][1], Diffuse[t][2]);
						Light.Direction = FVector3f(-0.81f, 0.39f, -0.44f);
						Map.Lighting[t].Terrain.Add(Light);
						Map.Lighting[t].Objects.Add(Light);
					}
				}
			}
		};
	}

	bool BuildMap(const FPlan& Plan, FGenMapData& Out, TArray<FString>* OutProblems)
	{
		const double Start = FPlatformTime::Seconds();
		FMapBuilder Builder(Plan, Out, OutProblems);
		Builder.Run();
		UE_LOG(LogGenerals, Log, TEXT("Random map '%s': %s, %s, %d x %d cells, %d objects (%d trees, %d buildings), %d towns, %d crossings, %d terrain classes, %d blends, %d ways carved; %.0f ms"),
			*Plan.Params.ToName(), GetEnvironmentDefs()[Plan.Environment].Key, *GenRandomMap::GetLayouts()[int32(Plan.Layout) + 1], Plan.CellsX, Plan.CellsY,
			Out.Objects.Num(), Builder.NumTrees, Builder.NumBuildings, Plan.Towns.Num(), Plan.Crossings.Num(), Out.TextureClasses.Num(), Out.BlendedTiles.Num(), Builder.NumCarved,
			(FPlatformTime::Seconds() - Start) * 1000.0);
		return true;
	}

	namespace
	{
		/** The colour of a terrain class's picture, on average. */
		FColor TerrainColor(const FString& Name)
		{
			static TMap<FString, FColor> Known;
			if (const FColor* Found = Known.Find(Name))
			{
				return *Found;
			}
			FColor Average(128, 128, 128, 255);
			if (const FGenTerrainType* Type = FGenGameData::Get().FindTerrain(Name))
			{
				TArray<uint8> Bytes;
				FGenImage Image;
				if (FGenFileSystem::Get().ReadFile(TEXT("Art\\Terrain\\") + Type->Texture, Bytes) && GenTextureLoader::DecodeTGA(Bytes, Image) && Image.Pixels.Num() > 0)
				{
					uint64 SumR = 0, SumG = 0, SumB = 0;
					for (const FColor& C : Image.Pixels)
					{
						SumR += C.R;
						SumG += C.G;
						SumB += C.B;
					}
					const uint64 N = uint64(Image.Pixels.Num());
					Average = FColor(uint8(SumR / N), uint8(SumG / N), uint8(SumB / N), 255);
				}
			}
			Known.Add(Name, Average);
			return Average;
		}

		uint32 HashMap(const FGenMapData& Map)
		{
			uint32 Crc = FCrc::MemCrc32(Map.Heights.GetData(), Map.Heights.Num());
			Crc = FCrc::MemCrc32(Map.TileNdx.GetData(), Map.TileNdx.Num() * sizeof(int16), Crc);
			Crc = FCrc::MemCrc32(Map.BlendNdx.GetData(), Map.BlendNdx.Num() * sizeof(int16), Crc);
			Crc = FCrc::MemCrc32(Map.ExtraBlendNdx.GetData(), Map.ExtraBlendNdx.Num() * sizeof(int16), Crc);
			Crc = FCrc::MemCrc32(Map.CliffState.GetData(), Map.CliffState.Num(), Crc);
			for (const FGenMapObject& O : Map.Objects)
			{
				const float Numbers[4] = { O.Location.X, O.Location.Y, O.Location.Z, O.Angle };
				Crc = FCrc::MemCrc32(Numbers, sizeof(Numbers), Crc);
				Crc = FCrc::StrCrc32(*O.TemplateName, Crc);
			}
			return Crc;
		}

		void DrawPreview(const FGenMapData& Map, int32 TimeOfDay, FGenImage& Out)
		{
			const FIntPoint Cells = Map.Boundaries.Num() > 0 ? Map.Boundaries[0] : FIntPoint(Map.Width - 2 * Map.Border, Map.Height - 2 * Map.Border);
			const float Scale = 320.f / float(FMath::Max(Cells.X, Cells.Y));
			Out.Width = FMath::Max(1, FMath::RoundToInt32(float(Cells.X) * Scale));
			Out.Height = FMath::Max(1, FMath::RoundToInt32(float(Cells.Y) * Scale));
			Out.Pixels.SetNumUninitialized(Out.Width * Out.Height);

			// Which colour each tile belongs to.
			TArray<FColor> ClassColors;
			for (const FGenTextureClass& C : Map.TextureClasses)
			{
				ClassColors.Add(TerrainColor(C.Name));
			}
			auto ClassColor = [&](int32 TileIndex)
			{
				const int32 Tile = TileIndex >> 2;
				for (int32 i = 0; i < Map.TextureClasses.Num(); ++i)
				{
					const FGenTextureClass& C = Map.TextureClasses[i];
					if (Tile >= C.FirstTile && Tile < C.FirstTile + C.NumTiles)
					{
						return ClassColors[i];
					}
				}
				return FColor(128, 128, 128, 255);
			};
			static const float Tint[4][3] = { { 1.f, 0.94f, 0.86f }, { 1.f, 1.f, 1.f }, { 0.92f, 0.74f, 0.58f }, { 0.46f, 0.5f, 0.72f } };
			const float* Light = Tint[FMath::Clamp(TimeOfDay, 0, 3)];
			const bool bHasWater = Map.PolygonTriggers.ContainsByPredicate([](const FGenPolygonTrigger& T) { return T.bWater; });
			auto Put = [&](int32 PX, int32 PY, float RV, float GV, float BV)
			{
				if (PX >= 0 && PY >= 0 && PX < Out.Width && PY < Out.Height)
				{
					Out.At(PX, PY) = FColor(uint8(FMath::Clamp(RV * Light[0], 0.f, 255.f)), uint8(FMath::Clamp(GV * Light[1], 0.f, 255.f)), uint8(FMath::Clamp(BV * Light[2], 0.f, 255.f)), 255);
				}
			};
			const float Cell = GenCoords::MapXYFactor;
			for (int32 PY = 0; PY < Out.Height; ++PY)
			{
				// (North, the far end of y, is the top row.)
				const float CY = float(Cells.Y) * (1.f - (float(PY) + 0.5f) / float(Out.Height));
				const int32 IY = FMath::Clamp(FMath::FloorToInt32(CY) + Map.Border, 1, Map.Height - 2);
				for (int32 PX = 0; PX < Out.Width; ++PX)
				{
					const float CX = float(Cells.X) * (float(PX) + 0.5f) / float(Out.Width);
					const int32 IX = FMath::Clamp(FMath::FloorToInt32(CX) + Map.Border, 1, Map.Width - 2);
					const float Ground = Map.GetHeightAt(IX, IY);
					if (bHasWater && Ground < WaterZ)
					{
						const float Depth = FMath::Clamp((WaterZ - Ground) / 10.f, 0.f, 1.f);
						Put(PX, PY, 62.f - 22.f * Depth, 104.f - 28.f * Depth, 132.f - 20.f * Depth);
						continue;
					}
					const FColor C = ClassColor(Map.TileNdx[IY * Map.Width + IX]);
					const float Shade = 1.f + FMath::Clamp((Map.GetHeightAt(IX - 1, IY + 1) - Map.GetHeightAt(IX + 1, IY - 1)) * 0.075f, -0.5f, 0.5f);
					Put(PX, PY, float(C.R) * Shade, float(C.G) * Shade, float(C.B) * Shade);
				}
			}
			auto ToPixelX = [&](float WX) { return FMath::FloorToInt32(WX / Cell * Scale); };
			auto ToPixelY = [&](float WY) { return Out.Height - 1 - FMath::FloorToInt32(WY / Cell * Scale); };
			const TArray<FGenMapObject>& Objects = Map.Objects;
			// Roads and bridges.
			for (int32 i = 0; i + 1 < Objects.Num(); ++i)
			{
				const FGenMapObject& A = Objects[i];
				const FGenMapObject& O = Objects[i + 1];
				const bool bRoad = (A.Flags & FGenMapObject::FlagRoadPoint1) && (O.Flags & FGenMapObject::FlagRoadPoint2);
				const bool bBridge = (A.Flags & FGenMapObject::FlagBridgePoint1) && (O.Flags & FGenMapObject::FlagBridgePoint2);
				if (!bRoad && !bBridge)
				{
					continue;
				}
				const FVector2f From(A.Location.X, A.Location.Y), To(O.Location.X, O.Location.Y);
				const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Dist2D(From, To) / Cell * Scale));
				for (int32 k = 0; k <= Steps; ++k)
				{
					const FVector2f P = From + (To - From) * (float(k) / float(Steps));
					if (bBridge)
					{
						const int32 PXc = ToPixelX(P.X), PYc = ToPixelY(P.Y);
						Put(PXc, PYc, 196.f, 186.f, 160.f);
						Put(PXc + 1, PYc, 196.f, 186.f, 160.f);
						Put(PXc, PYc + 1, 196.f, 186.f, 160.f);
					}
					else
					{
						Put(ToPixelX(P.X), ToPixelY(P.Y), 148.f, 134.f, 108.f);
					}
				}
			}
			// What stands on the land: buildings as blocks, trees as dark spots.
			for (const FGenMapObject& O : Objects)
			{
				if (O.IsWaypoint() || (O.Flags & (FGenMapObject::FlagRoadFlags | FGenMapObject::FlagBridgeFlags)) != 0)
				{
					continue;
				}
				const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(O.TemplateName);
				if (!T)
				{
					continue;
				}
				const int32 PX = ToPixelX(O.Location.X), PY = ToPixelY(O.Location.Y);
				if (T->IsKindOf(TEXT("SHRUBBERY")))
				{
					if (O.TemplateName.StartsWith(TEXT("Bush")) || O.TemplateName.StartsWith(TEXT("DesertShrub")))
					{
						continue;
					}
					const FColor C = Out.Pixels.IsValidIndex(PY * Out.Width + PX) && PX >= 0 && PX < Out.Width ? Out.At(PX, PY) : FColor::Black;
					const float Darker = 0.55f;
					const float RV = float(C.R) / FMath::Max(Light[0], 0.01f) * Darker, GV = float(C.G) / FMath::Max(Light[1], 0.01f) * Darker, BV = float(C.B) / FMath::Max(Light[2], 0.01f) * Darker;
					Put(PX, PY, RV * 0.8f, GV, BV * 0.7f);
					Put(PX + 1, PY, RV * 0.8f, GV, BV * 0.7f);
				}
				else if (T->IsKindOf(TEXT("STRUCTURE")) && T->GeometryMajorRadius > 8.f)
				{
					const int32 Half = FMath::Clamp(FMath::RoundToInt32(T->GeometryMajorRadius / Cell * Scale), 1, 5);
					for (int32 DY = -Half; DY <= Half; ++DY)
					{
						for (int32 DX = -Half; DX <= Half; ++DX)
						{
							const bool bEdge = FMath::Abs(DX) == Half || FMath::Abs(DY) == Half;
							Put(PX + DX, PY + DY, bEdge ? 70.f : 214.f, bEdge ? 66.f : 206.f, bEdge ? 60.f : 190.f);
						}
					}
				}
			}
		}
	}
}

using namespace GenRandomMapDetail;

bool GenRandomMap::MakePreview(const FString& Name, FGenImage& Out)
{
	FGenRandomMapParams Params;
	FPlan Plan;
	if (!FGenRandomMapParams::FromName(Name, Params) || !MakePlan(Params, Plan))
	{
		return false;
	}
	FGenMapData Map;
	if (!BuildMap(Plan, Map))
	{
		return false;
	}
	DrawPreview(Map, Plan.TimeOfDay, Out);
	return true;
}

void GenRandomMap::RunSelfTest(int32 Count)
{
	// Names that don't exist in the data show up here once, not as bare patches of a map in a game.
	const FGenGameData& GD = FGenGameData::Get();
	const FGenTerrainRoads& Roads = FGenTerrainRoads::Get();
	for (const FEnvironmentDef& E : GetEnvironmentDefs())
	{
		TArray<FString> Missing;
		TArray<const TCHAR*> Classes = E.Ground;
		Classes.Append({ E.Patch, E.Dirt, E.Rock, E.Cliff, E.Shore, E.Bed, E.Base, E.Town });
		if (E.Crops) { Classes.Add(E.Crops); }
		for (const TCHAR* C : Classes)
		{
			if (GetTerrainWidth(C) <= 0) { Missing.Add(FString::Printf(TEXT("terrain %s"), C)); }
		}
		auto CheckList = [&](const TArray<FWeighted>& List)
		{
			for (const FWeighted& Item : List)
			{
				if (!GD.FindTemplate(Item.Name)) { Missing.Add(Item.Name); }
			}
		};
		for (const TArray<FWeighted>* List : { &E.Trees, &E.Bushes, &E.Rocks, &E.BigRocks, &E.Houses, &E.Shops, &E.Large, &E.Towers, &E.FarmBuildings, &E.StreetProps, &E.YardProps, &E.FarmProps, &E.Cars })
		{
			CheckList(*List);
		}
		for (const TArray<const TCHAR*>* List : { &E.Landmarks, &E.AmbientDay, &E.AmbientNight, &E.AmbientWater })
		{
			for (const TCHAR* Name : *List)
			{
				if (!GD.FindTemplate(Name)) { Missing.Add(Name); }
			}
		}
		if (E.StreetLamp && !GD.FindTemplate(E.StreetLamp)) { Missing.Add(E.StreetLamp); }
		for (const TCHAR* Road : { E.RoadMain, E.RoadTown, E.RoadAvenue })
		{
			if (!Roads.FindRoad(Road)) { Missing.Add(FString::Printf(TEXT("road %s"), Road)); }
		}
		if (!Roads.FindBridge(E.Bridge)) { Missing.Add(FString::Printf(TEXT("bridge %s"), E.Bridge)); }
		for (int32 t = 0; t < 4; ++t)
		{
			if (!FGenFileSystem::Get().Exists(FString::Printf(TEXT("Maps\\%s\\%s.map"), E.LightDonors[t], E.LightDonors[t]))) { Missing.Add(FString::Printf(TEXT("light donor %s"), E.LightDonors[t])); }
		}
		if (!FGenFileSystem::Get().Exists(FString::Printf(TEXT("Maps\\%s\\map.ini"), E.MapIniDonor))) { Missing.Add(FString::Printf(TEXT("map.ini donor %s"), E.MapIniDonor)); }
		UE_LOG(LogGenerals, Log, TEXT("Random map test: environment %s: %s"), E.Key, Missing.Num() == 0 ? TEXT("all names found") : *FString::Printf(TEXT("MISSING %s"), *FString::Join(Missing, TEXT(", "))));
	}

	const FString Folder = FPaths::ProjectSavedDir() / TEXT("RandomMaps");
	int32 Bad = 0;
	double TotalMs = 0.0, WorstMs = 0.0;
	for (int32 i = 0; i < Count; ++i)
	{
		FGenRandomMapParams Params;
		Params.Environment = i % GetEnvironmentDefs().Num();
		Params.Layout = (i + i / GetEnvironmentDefs().Num()) % int32(ELayout::Count);
		Params.Size = i % 4;
		Params.TimeOfDay = i % 4;
		Params.Resources = i % 3;
		Params.Players = 2 + i % 7;
		Params.Seed = 1000 + i * 37;
		FPlan Plan;
		TArray<FString> Problems;
		if (!MakePlan(Params, Plan))
		{
			UE_LOG(LogGenerals, Error, TEXT("Random map test %d: no plan for %s"), i, *Params.ToName());
			++Bad;
			continue;
		}
		const double Start = FPlatformTime::Seconds();
		FGenMapData Map;
		BuildMap(Plan, Map, &Problems);
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		TotalMs += Ms;
		WorstMs = FMath::Max(WorstMs, Ms);
		const uint32 Hash = HashMap(Map);
		// The same name must make the same map.
		if (i < 7)
		{
			FGenMapData Again;
			BuildMap(Plan, Again);
			if (HashMap(Again) != Hash)
			{
				Problems.Add(TEXT("two builds differ"));
			}
		}
		FGenRandomMapParams Parsed;
		if (!FGenRandomMapParams::FromName(Params.ToName(), Parsed) || Parsed.ToName() != Params.ToName())
		{
			Problems.Add(TEXT("the name doesn't read back"));
		}
		for (const FVector2f& S : Plan.Starts)
		{
			if (!Map.FindWaypoint(FString::Printf(TEXT("Player_%d_Start"), Plan.Starts.IndexOfByKey(S) + 1)))
			{
				Problems.Add(TEXT("a start has no waypoint"));
			}
			if (S.X < 300.f || S.Y < 300.f || S.X > Plan.SizeX - 300.f || S.Y > Plan.SizeY - 300.f)
			{
				Problems.Add(TEXT("a start too near the edge"));
			}
		}
		const int32 Docks = Plan.Sites.FilterByPredicate([](const FPlanSite& S) { return S.Kind == FPlanSite::Dock; }).Num();
		if (Docks < Plan.Players)
		{
			Problems.Add(FString::Printf(TEXT("only %d supply docks for %d players"), Docks, Plan.Players));
		}
		FGenImage Image;
		DrawPreview(Map, Plan.TimeOfDay, Image);
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Image.Width, Image.Height, TArrayView64<const FColor>(Image.Pixels.GetData(), Image.Pixels.Num()), Png);
		FFileHelper::SaveArrayToFile(Png, *FString::Printf(TEXT("%s/%03d_%s_%s_%dp.png"), *Folder, i, GetEnvironmentDefs()[Plan.Environment].Key, GetLayoutKey(Plan.Layout), Plan.Players));
		UE_LOG(LogGenerals, Log, TEXT("Random map test %d: %s hash %08x, %.0f ms: %s"), i, *Params.ToName(), Hash, Ms, Problems.Num() == 0 ? TEXT("fine") : *FString::Printf(TEXT("PROBLEMS: %s"), *FString::Join(Problems, TEXT("; "))));
		Bad += Problems.Num() > 0 ? 1 : 0;
	}
	UE_LOG(LogGenerals, Log, TEXT("Random map test: %d maps, %d with problems; %.0f ms on average, %.0f ms at most. Pictures in %s"), Count, Bad, Count > 0 ? TotalMs / double(Count) : 0.0, WorstMs, *Folder);
}
