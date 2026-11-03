#include "Map/GenRandomMap.h"
#include "Map/GenRandomMapInternal.h"
#include "Game/GenShellData.h"
#include "Generals1.h"

// The plan of a random map: everything that is decided before any terrain exists - what kind of place it is, how big, where
// the players start, where the water runs, which ways stay open, where supplies, tech buildings and towns go. The way a
// random map script of Age of Empires II goes about it: players first, then the land around them, fair to all of them.
// (The terrain and what stands on it: GenRandomMapBuild.cpp.)

namespace GenRandomMapDetail
{
	void DetSinCos(float Turns, float& OutSin, float& OutCos)
	{
		float T = Turns - float(FMath::FloorToInt32(Turns));
		T *= 4.f;
		const int32 Quadrant = FMath::Clamp(FMath::FloorToInt32(T), 0, 3);
		const float X = (T - float(Quadrant)) * 1.5707963f;
		const float X2 = X * X;
		const float S = X * (1.f - X2 / 6.f * (1.f - X2 / 20.f * (1.f - X2 / 42.f)));
		const float C = 1.f - X2 / 2.f * (1.f - X2 / 12.f * (1.f - X2 / 30.f * (1.f - X2 / 56.f)));
		switch (Quadrant)
		{
		case 0: OutSin = S; OutCos = C; break;
		case 1: OutSin = C; OutCos = -S; break;
		case 2: OutSin = -S; OutCos = -C; break;
		default: OutSin = -C; OutCos = S; break;
		}
	}

	static uint32 NoiseHash(int32 X, int32 Y, uint32 Seed)
	{
		uint32 H = uint32(X) * 0x8DA6B343u ^ uint32(Y) * 0xD8163841u ^ Seed * 0xCB1AB31Fu;
		H ^= H >> 13;
		H *= 0x5BD1E995u;
		H ^= H >> 15;
		return H;
	}

	float GradientNoise(float X, float Y, uint32 Seed)
	{
		static const float GX[8] = { 1.f, -1.f, 0.f, 0.f, 0.7071f, -0.7071f, 0.7071f, -0.7071f };
		static const float GY[8] = { 0.f, 0.f, 1.f, -1.f, 0.7071f, 0.7071f, -0.7071f, -0.7071f };
		const int32 IX = FMath::FloorToInt32(X), IY = FMath::FloorToInt32(Y);
		const float FX = X - float(IX), FY = Y - float(IY);
		auto Corner = [&](int32 DX, int32 DY)
		{
			const uint32 H = NoiseHash(IX + DX, IY + DY, Seed) & 7u;
			return GX[H] * (FX - float(DX)) + GY[H] * (FY - float(DY));
		};
		const float U = FX * FX * FX * (FX * (FX * 6.f - 15.f) + 10.f);
		const float V = FY * FY * FY * (FY * (FY * 6.f - 15.f) + 10.f);
		const float Near = Corner(0, 0) + (Corner(1, 0) - Corner(0, 0)) * U;
		const float Far = Corner(0, 1) + (Corner(1, 1) - Corner(0, 1)) * U;
		return (Near + (Far - Near) * V) * 1.5f;
	}

	float FractalNoise(float X, float Y, uint32 Seed, int32 Octaves)
	{
		float Sum = 0.f, Amp = 1.f, Norm = 0.f;
		for (int32 i = 0; i < Octaves; ++i)
		{
			Sum += Amp * GradientNoise(X, Y, Seed + uint32(i) * 101u);
			Norm += Amp;
			Amp *= 0.5f;
			X = X * 2.03f + 17.3f;
			Y = Y * 2.03f - 9.1f;
		}
		return Sum / Norm;
	}

	const TCHAR* PickWeighted(const TArray<FWeighted>& List, FDetRandom& Random)
	{
		int32 Total = 0;
		for (const FWeighted& W : List)
		{
			Total += W.Weight;
		}
		if (Total <= 0)
		{
			return nullptr;
		}
		int32 Roll = Random.Range(0, Total - 1);
		for (const FWeighted& W : List)
		{
			Roll -= W.Weight;
			if (Roll < 0)
			{
				return W.Name;
			}
		}
		return List.Last().Name;
	}

	namespace
	{
		const TCHAR* LayoutKeys[int32(ELayout::Count)] = { TEXT("plains"), TEXT("highlands"), TEXT("rivers"), TEXT("lake"), TEXT("canyons"), TEXT("coast"), TEXT("city") };
		const TCHAR* LayoutLabels[int32(ELayout::Count)] = { TEXT("Open Land"), TEXT("Highlands"), TEXT("River Crossing"), TEXT("Central Lake"), TEXT("Fortress Canyons"), TEXT("Coast"), TEXT("City") };
		const TCHAR* LayoutTexts[int32(ELayout::Count)] = {
			TEXT("Rolling open ground, woods and villages. Attacks can come from anywhere."),
			TEXT("Plateaus and cliffs. The bases stand high, ramps and passes are the ways between them."),
			TEXT("A river cuts the map in two. Bridges and fords are what everybody fights over."),
			TEXT("A big lake in the middle. The ways between the players run around its shores."),
			TEXT("Every base lies behind a wall of rock with a few gates. The middle is open."),
			TEXT("The sea along one side of the map, beaches, and the land behind them."),
			TEXT("A big town in the middle, its streets and buildings between the bases."),
		};
		const TCHAR* SizeKeys[] = { TEXT("small"), TEXT("medium"), TEXT("large"), TEXT("huge") };
		const TCHAR* TimeKeys[] = { TEXT("morning"), TEXT("afternoon"), TEXT("evening"), TEXT("night") };
		const TCHAR* ResourceKeys[] = { TEXT("scarce"), TEXT("normal"), TEXT("rich") };
		const TCHAR* AnyKey = TEXT("any");

		int32 FindKey(const FString& Text, const TCHAR* const* Keys, int32 Num)
		{
			for (int32 i = 0; i < Num; ++i)
			{
				if (Text.Equals(Keys[i], ESearchCase::IgnoreCase))
				{
					return i;
				}
			}
			return INDEX_NONE;
		}

		FVector2f Rotated(const FVector2f& V, float Turns)
		{
			float S, C;
			DetSinCos(Turns, S, C);
			return FVector2f(V.X * C - V.Y * S, V.X * S + V.Y * C);
		}

		FVector2f Perpendicular(const FVector2f& V) { return FVector2f(-V.Y, V.X); }

		FVector2f Unit(const FVector2f& V, const FVector2f& Fallback = FVector2f(1.f, 0.f))
		{
			const float Size = FMath::Sqrt(V.X * V.X + V.Y * V.Y);
			return Size > 0.001f ? FVector2f(V.X / Size, V.Y / Size) : Fallback;
		}

		float Dist(const FVector2f& A, const FVector2f& B)
		{
			const float DX = A.X - B.X, DY = A.Y - B.Y;
			return FMath::Sqrt(DX * DX + DY * DY);
		}

		float DistToSegment(const FVector2f& P, const FVector2f& A, const FVector2f& B)
		{
			const FVector2f AB = B - A;
			const float Len2 = AB.X * AB.X + AB.Y * AB.Y;
			const float T = Len2 > 0.001f ? FMath::Clamp(((P.X - A.X) * AB.X + (P.Y - A.Y) * AB.Y) / Len2, 0.f, 1.f) : 0.f;
			return Dist(P, FVector2f(A.X + AB.X * T, A.Y + AB.Y * T));
		}

		bool SegmentsCross(const FVector2f& A, const FVector2f& B, const FVector2f& C, const FVector2f& D, FVector2f& OutPoint)
		{
			const FVector2f R = B - A, S = D - C;
			const float Denom = R.X * S.Y - R.Y * S.X;
			if (FMath::Abs(Denom) < 0.0001f)
			{
				return false;
			}
			const FVector2f AC = C - A;
			const float T = (AC.X * S.Y - AC.Y * S.X) / Denom;
			const float U = (AC.X * R.Y - AC.Y * R.X) / Denom;
			if (T < 0.f || T > 1.f || U < 0.f || U > 1.f)
			{
				return false;
			}
			OutPoint = FVector2f(A.X + R.X * T, A.Y + R.Y * T);
			return true;
		}

		/** A point of the ring the bases stand on: an ellipse swollen towards a rectangle, so that they get the corners. */
		FVector2f RingPoint(float Turns, const FVector2f& Center, float RX, float RY)
		{
			float S, C;
			DetSinCos(Turns, S, C);
			const float K = FMath::Sqrt(FMath::Sqrt(C * C * C * C + S * S * S * S));
			return FVector2f(Center.X + RX * C / K, Center.Y + RY * S / K);
		}

		constexpr float BaseMargin = 470.f;     // from a start position to the edge of the land (the official maps: 450 to 550)

		class FPlanner
		{
		public:
			FPlanner(const FGenRandomMapParams& Params, FPlan& InPlan) : P(Params), Plan(InPlan), R(uint32(Params.Seed), 101u) {}

			void Run()
			{
				Decide();
				PlaceStarts();
				if (Plan.Layout == ELayout::Rivers)
				{
					PlanRivers();
				}
				PlanCorridors();
				SplitAtWater();
				PlanTowns();
				PlanRoadsOnCorridors();
				PlanResources();
			}

		private:
			const FGenRandomMapParams& P;
			FPlan& Plan;
			FDetRandom R;
			FVector2f LandMin = FVector2f::ZeroVector, LandMax = FVector2f::ZeroVector;
			float StartTurn = 0.f;
			int32 Sign = 1;

			void Decide()
			{
				const int32 NumEnvironments = GetEnvironmentDefs().Num();
				// (Always all three draws: choosing an environment yourself doesn't change the rest of what a seed means.)
				const int32 DrawnEnvironment = R.Range(0, NumEnvironments - 1);
				const int32 DrawnLayout = R.Range(0, int32(ELayout::Count) - 1);
				const int32 DrawnTime = R.Range(0, 9);
				Plan.Params = P;
				Plan.Seed = uint32(P.Seed);
				Plan.Players = FMath::Clamp(P.Players, 2, 8);
				Plan.Environment = (P.Environment >= 0 && P.Environment < NumEnvironments) ? P.Environment : DrawnEnvironment;
				Plan.Layout = ELayout((P.Layout >= 0 && P.Layout < int32(ELayout::Count)) ? P.Layout : DrawnLayout);
				// (Left to chance, mostly daylight.)
				Plan.TimeOfDay = (P.TimeOfDay >= 0 && P.TimeOfDay <= 3) ? P.TimeOfDay : (DrawnTime < 3 ? 0 : (DrawnTime < 7 ? 1 : (DrawnTime < 9 ? 2 : 3)));

				static const int32 MediumCells[9] = { 0, 0, 270, 310, 350, 390, 420, 450, 480 };
				static const float SizeScale[4] = { 0.82f, 1.f, 1.2f, 1.4f };
				float Cells = float(MediumCells[Plan.Players]) * SizeScale[FMath::Clamp(P.Size, 0, 3)];
				switch (Plan.Layout)
				{
				case ELayout::Lake: Cells *= Plan.Players <= 3 ? 1.25f : 1.12f; break;
				case ELayout::Coast: Cells *= 1.15f; break;
				case ELayout::City: Cells *= 1.15f; break;
				case ELayout::Rivers: Cells *= 1.06f; break;
				default: break;
				}
				Cells = FMath::Clamp(Cells, 220.f, 560.f);
				const float Aspect = R.Between(-0.07f, 0.07f);
				Plan.CellsX = FMath::RoundToInt32(Cells * (1.f + Aspect) / 5.f) * 5;
				Plan.CellsY = FMath::RoundToInt32(Cells * (1.f - Aspect) / 5.f) * 5;
				Plan.SizeX = float(Plan.CellsX) * 10.f;
				Plan.SizeY = float(Plan.CellsY) * 10.f;
				LandMin = FVector2f::ZeroVector;
				LandMax = FVector2f(Plan.SizeX, Plan.SizeY);
				Sign = R.Chance(0.5f) ? 1 : -1;

				if (Plan.Layout == ELayout::Coast)
				{
					// The sea takes a fifth of the map along one edge.
					Plan.bCoast = true;
					const int32 Side = R.Range(0, 3);
					const float DepthX = Plan.SizeX * 0.2f, DepthY = Plan.SizeY * 0.2f;
					switch (Side)
					{
					case 0: LandMin.X = DepthX; Plan.CoastInland = FVector2f(1.f, 0.f); Plan.CoastPoint = FVector2f(DepthX, Plan.SizeY * 0.5f); break;
					case 1: LandMax.X -= DepthX; Plan.CoastInland = FVector2f(-1.f, 0.f); Plan.CoastPoint = FVector2f(Plan.SizeX - DepthX, Plan.SizeY * 0.5f); break;
					case 2: LandMin.Y = DepthY; Plan.CoastInland = FVector2f(0.f, 1.f); Plan.CoastPoint = FVector2f(Plan.SizeX * 0.5f, DepthY); break;
					default: LandMax.Y -= DepthY; Plan.CoastInland = FVector2f(0.f, -1.f); Plan.CoastPoint = FVector2f(Plan.SizeX * 0.5f, Plan.SizeY - DepthY); break;
					}
				}
				Plan.Middle = (LandMin + LandMax) * 0.5f;
			}

			void PlaceStarts()
			{
				const int32 N = Plan.Players;
				const float RX = (LandMax.X - LandMin.X) * 0.5f - BaseMargin;
				const float RY = (LandMax.Y - LandMin.Y) * 0.5f - BaseMargin;
				if (N == 2)
				{
					// Face to face; over a lake, corner to corner, which leaves it the room.
					StartTurn = Plan.Layout == ELayout::Lake ? (R.Chance(0.5f) ? 0.125f : 0.375f) : R.Frac();
				}
				else if (N == 4)
				{
					StartTurn = R.Chance(0.6f) ? 0.125f : 0.f;
				}
				else
				{
					StartTurn = R.Frac();
				}
				for (int32 i = 0; i < N; ++i)
				{
					const float Turn = StartTurn + float(i) / float(N) + R.Between(-0.008f, 0.008f);
					Plan.Starts.Add(RingPoint(Turn, Plan.Middle, RX, RY));
				}
				if (Plan.Layout == ELayout::Lake)
				{
					Plan.bLake = true;
					float Nearest = FLT_MAX;
					for (const FVector2f& S : Plan.Starts)
					{
						Nearest = FMath::Min(Nearest, Dist(S, Plan.Middle));
					}
					Plan.LakeCenter = Plan.Middle;
					// (Its shore wanders up to 26% out: the bases and their way round stay dry.)
					Plan.LakeRadius = FMath::Max(240.f, (Nearest - 640.f) / 1.26f);
				}
			}

			void PlanRivers()
			{
				// A line through the middle, as far from all the start positions as a line can be.
				auto Clearance = [this](float Turn)
				{
					float S, C;
					DetSinCos(Turn, S, C);
					float Best = FLT_MAX;
					for (const FVector2f& Start : Plan.Starts)
					{
						const FVector2f D = Start - Plan.Middle;
						Best = FMath::Min(Best, FMath::Abs(D.X * S - D.Y * C));
					}
					return Best;
				};
				const float Offset = R.Frac();
				float FirstTurn = 0.f, FirstClear = -1.f;
				for (int32 i = 0; i < 64; ++i)
				{
					const float Turn = (float(i) + Offset) / 64.f * 0.5f;
					const float Clear = Clearance(Turn);
					if (Clear > FirstClear)
					{
						FirstClear = Clear;
						FirstTurn = Turn;
					}
				}
				Plan.RiverHalfWidth = R.Between(70.f, 105.f) * (P.Size >= 2 ? 1.2f : 1.f);
				AddRiver(FirstTurn, FirstClear);
				if (Plan.Players >= 4 && R.Chance(0.4f))
				{
					float SecondTurn = 0.f, SecondClear = -1.f;
					for (int32 i = 0; i < 64; ++i)
					{
						const float Turn = (float(i) + Offset) / 64.f * 0.5f;
						float Apart = FMath::Abs(Turn - FirstTurn);
						Apart = FMath::Min(Apart, 0.5f - Apart);
						const float Clear = Clearance(Turn);
						if (Apart > 0.18f && Clear > SecondClear)
						{
							SecondClear = Clear;
							SecondTurn = Turn;
						}
					}
					if (SecondClear > 600.f)
					{
						AddRiver(SecondTurn, SecondClear);
					}
				}
			}

			void AddRiver(float Turn, float Clear)
			{
				float S, C;
				DetSinCos(Turn, S, C);
				const FVector2f Along(C, S), Across(-S, C);
				const float Reach = FMath::Sqrt(Plan.SizeX * Plan.SizeX + Plan.SizeY * Plan.SizeY) * 0.5f + 300.f;
				const float Amplitude = FMath::Clamp(Clear - 440.f - Plan.RiverHalfWidth * 1.3f, 30.f, 190.f);
				const uint32 Seed = R.Next();
				TArray<FVector2f>& River = Plan.Rivers.AddDefaulted_GetRef();
				for (float T = -Reach; T <= Reach; T += 120.f)
				{
					// (It goes through the middle exactly: that is where the middle crossing is.)
					const float Wander = FractalNoise(T / 640.f, 3.7f, Seed, 2) * Amplitude * 1.6f * Smooth(0.f, 500.f, FMath::Abs(T));
					River.Add(Plan.Middle + Along * T + Across * Wander);
				}
			}

			FVector2f ClampToMap(const FVector2f& V, float Margin) const
			{
				return FVector2f(FMath::Clamp(V.X, Margin, Plan.SizeX - Margin), FMath::Clamp(V.Y, Margin, Plan.SizeY - Margin));
			}

			FPlanCorridor MakeCorridor(const FVector2f& A, const FVector2f& B, float Wobble)
			{
				FPlanCorridor Out;
				const float Len = Dist(A, B);
				const int32 Pieces = FMath::Max(2, FMath::RoundToInt32(Len / 240.f));
				const FVector2f Across = Perpendicular(Unit(B - A));
				const uint32 Seed = R.Next();
				for (int32 i = 0; i <= Pieces; ++i)
				{
					const float T = float(i) / float(Pieces);
					const float Taper = 4.f * T * (1.f - T);
					FVector2f Pt = A + (B - A) * T + Across * (Wobble * Taper * FractalNoise(T * Len / 520.f, 5.1f, Seed, 2) * 1.6f);
					if (i > 0 && i < Pieces)
					{
						Pt = ClampToMap(Pt, 70.f);
						if (Plan.bLake)
						{
							// Round the lake, not through it.
							const float Need = Plan.LakeRadius * 1.3f + 170.f;
							const float D = Dist(Pt, Plan.LakeCenter);
							if (D < Need)
							{
								Pt = ClampToMap(Plan.LakeCenter + Unit(Pt - Plan.LakeCenter) * Need, 70.f);
							}
						}
					}
					Out.Points.Add(Pt);
				}
				return Out;
			}

			void PlanCorridors()
			{
				const int32 N = Plan.Players;
				const TArray<FVector2f>& S = Plan.Starts;
				// The ring: each base to the next one.
				if (N > 2)
				{
					for (int32 i = 0; i < N; ++i)
					{
						Plan.Corridors.Add(MakeCorridor(S[i], S[(i + 1) % N], 90.f));
					}
				}
				else
				{
					// Two players: a way round on either side of the middle.
					const FVector2f Across = Perpendicular(Unit(S[1] - S[0]));
					const float Bulge = FMath::Min(Plan.SizeX, Plan.SizeY) * 0.28f;
					for (float Side : { -1.f, 1.f })
					{
						const FVector2f Via = ClampToMap(Plan.Middle + Across * (Side * Bulge), 200.f);
						FPlanCorridor First = MakeCorridor(S[0], Via, 70.f);
						const FPlanCorridor Second = MakeCorridor(Via, S[1], 70.f);
						First.Points.Append(Second.Points.GetData() + 1, Second.Points.Num() - 1);
						Plan.Corridors.Add(MoveTemp(First));
					}
				}
				if (Plan.Layout == ELayout::Lake)
				{
					return;
				}
				// The spokes: each base to the middle. Over a river they end at the crossing in the middle.
				FVector2f BridgeFrom = Plan.Middle, BridgeTo = Plan.Middle;
				FVector2f BridgeAcross(1.f, 0.f);
				const bool bOneRiver = Plan.Rivers.Num() == 1;
				if (bOneRiver)
				{
					const TArray<FVector2f>& River = Plan.Rivers[0];
					const int32 Mid = River.Num() / 2;
					BridgeAcross = Perpendicular(Unit(River[Mid + 1] - River[Mid - 1]));
					const float Half = Plan.RiverHalfWidth * 1.3f + 55.f;
					BridgeFrom = Plan.Middle - BridgeAcross * Half;
					BridgeTo = Plan.Middle + BridgeAcross * Half;
					FPlanCrossing& Crossing = Plan.Crossings.AddDefaulted_GetRef();
					Crossing.From = BridgeFrom;
					Crossing.To = BridgeTo;
				}
				for (const FVector2f& Start : S)
				{
					FVector2f End = Plan.Middle;
					if (bOneRiver)
					{
						const FVector2f D = Start - Plan.Middle;
						End = (D.X * BridgeAcross.X + D.Y * BridgeAcross.Y) > 0.f ? BridgeTo : BridgeFrom;
					}
					else if (Plan.Rivers.Num() > 1)
					{
						End = Plan.Middle + Unit(Start - Plan.Middle) * (Plan.RiverHalfWidth * 2.6f + 90.f);
					}
					Plan.Corridors.Add(MakeCorridor(Start, End, 70.f));
				}
			}

			void SplitAtWater()
			{
				if (Plan.Rivers.Num() == 0)
				{
					return;
				}
				// Where a way meets the river it stops at the bank, and a bridge (now and then a ford) takes it across.
				const float Half = Plan.RiverHalfWidth * 1.3f + 55.f;
				TArray<FPlanCorridor> Work = MoveTemp(Plan.Corridors);
				Plan.Corridors.Reset();
				int32 Guard = 0;
				while (Work.Num() > 0 && ++Guard < 200)
				{
					FPlanCorridor C = Work.Pop(EAllowShrinking::No);
					bool bSplit = false;
					for (int32 i = 0; i + 1 < C.Points.Num() && !bSplit; ++i)
					{
						for (const TArray<FVector2f>& River : Plan.Rivers)
						{
							for (int32 j = 0; j + 1 < River.Num() && !bSplit; ++j)
							{
								FVector2f Hit;
								if (!SegmentsCross(C.Points[i], C.Points[i + 1], River[j], River[j + 1], Hit))
								{
									continue;
								}
								FVector2f Across = Perpendicular(Unit(River[j + 1] - River[j]));
								const FVector2f Way = C.Points[i + 1] - C.Points[i];
								if (Way.X * Across.X + Way.Y * Across.Y < 0.f)
								{
									Across = Across * -1.f;
								}
								FVector2f From = Hit - Across * Half, To = Hit + Across * Half;
								// Another way crossing here already? They share its crossing.
								bool bKnown = false;
								for (const FPlanCrossing& Other : Plan.Crossings)
								{
									const FVector2f OtherMid = (Other.From + Other.To) * 0.5f;
									if (Dist(OtherMid, Hit) < 300.f)
									{
										const bool bSame = Dist(From, Other.From) < Dist(From, Other.To);
										From = bSame ? Other.From : Other.To;
										To = bSame ? Other.To : Other.From;
										bKnown = true;
										break;
									}
								}
								if (!bKnown)
								{
									FPlanCrossing& Crossing = Plan.Crossings.AddDefaulted_GetRef();
									Crossing.From = From;
									Crossing.To = To;
									Crossing.bFord = R.Chance(0.25f);
								}
								const float Keep = Half + 50.f;
								FPlanCorridor Before, After;
								for (int32 k = 0; k <= i; ++k)
								{
									if (Dist(C.Points[k], Hit) > Keep || k == 0) { Before.Points.Add(C.Points[k]); }
								}
								Before.Points.Add(From);
								After.Points.Add(To);
								for (int32 k = i + 1; k < C.Points.Num(); ++k)
								{
									if (Dist(C.Points[k], Hit) > Keep || k == C.Points.Num() - 1) { After.Points.Add(C.Points[k]); }
								}
								Plan.Corridors.Add(MoveTemp(Before));
								Work.Add(MoveTemp(After));
								bSplit = true;
							}
							if (bSplit)
							{
								break;
							}
						}
					}
					if (!bSplit)
					{
						Plan.Corridors.Add(MoveTemp(C));
					}
				}
			}

			/** How close to the water that is (world units beyond the widest it can get; negative: in it). */
			float WaterClearance(const FVector2f& Pos) const
			{
				float Best = FLT_MAX;
				if (Plan.bLake)
				{
					Best = FMath::Min(Best, Dist(Pos, Plan.LakeCenter) - Plan.LakeRadius * 1.3f);
				}
				for (const TArray<FVector2f>& River : Plan.Rivers)
				{
					for (int32 i = 0; i + 1 < River.Num(); ++i)
					{
						Best = FMath::Min(Best, DistToSegment(Pos, River[i], River[i + 1]) - Plan.RiverHalfWidth * 1.3f);
					}
				}
				if (Plan.bCoast)
				{
					Best = FMath::Min(Best, (Pos.X - Plan.CoastPoint.X) * Plan.CoastInland.X + (Pos.Y - Plan.CoastPoint.Y) * Plan.CoastInland.Y - 80.f);
				}
				return Best;
			}

			bool InLand(const FVector2f& Pos, float Margin) const
			{
				return Pos.X >= Margin && Pos.Y >= Margin && Pos.X <= Plan.SizeX - Margin && Pos.Y <= Plan.SizeY - Margin;
			}

			bool TownFits(const FVector2f& Pos, float Radius) const
			{
				if (!InLand(Pos, Radius + 70.f) || WaterClearance(Pos) < Radius + 70.f)
				{
					return false;
				}
				for (const FVector2f& S : Plan.Starts)
				{
					if (Dist(S, Pos) < 640.f + Radius * 0.5f)
					{
						return false;
					}
				}
				for (const FPlanTown& T : Plan.Towns)
				{
					if (Dist(T.Center, Pos) < T.Radius + Radius + 150.f)
					{
						return false;
					}
				}
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					if (Dist(C.From, Pos) < Radius + 170.f || Dist(C.To, Pos) < Radius + 170.f)
					{
						return false;
					}
				}
				return true;
			}

			void PlanTowns()
			{
				const int32 N = Plan.Players;
				float Nearest = FLT_MAX;
				for (const FVector2f& S : Plan.Starts)
				{
					Nearest = FMath::Min(Nearest, Dist(S, Plan.Middle));
				}
				float RingChance = 0.5f, MiddleChance = 0.5f, FarmCount = float(N) * 0.5f;
				switch (Plan.Layout)
				{
				case ELayout::Plains: RingChance = 0.65f; MiddleChance = 0.6f; break;
				case ELayout::Highlands: RingChance = 0.35f; MiddleChance = 0.35f; FarmCount *= 0.5f; break;
				case ELayout::Rivers: RingChance = 0.55f; MiddleChance = 0.f; break;
				case ELayout::Lake: RingChance = 0.75f; MiddleChance = 0.f; break;
				case ELayout::Canyons: RingChance = 0.3f; MiddleChance = 0.5f; FarmCount = 0.f; break;
				case ELayout::Coast: RingChance = 0.65f; MiddleChance = 0.55f; break;
				case ELayout::City: RingChance = 0.7f; MiddleChance = 0.f; FarmCount = 0.f; break;
				default: break;
				}
				if (Plan.Layout == ELayout::City)
				{
					FPlanTown City;
					City.Center = Plan.Middle;
					City.Radius = FMath::Clamp(FMath::Min(Plan.SizeX, Plan.SizeY) * 0.16f, 300.f, FMath::Clamp(Nearest - 730.f, 300.f, 720.f));
					City.bCity = true;
					City.bVertical = R.Chance(0.5f);
					Plan.Towns.Add(City);
				}
				if (R.Chance(MiddleChance))
				{
					const float Radius = R.Between(170.f, 240.f);
					if (TownFits(Plan.Middle, Radius))
					{
						FPlanTown Town;
						Town.Center = Plan.Middle;
						Town.Radius = Radius;
						Town.bVertical = R.Chance(0.5f);
						Plan.Towns.Add(Town);
					}
				}
				const int32 Edges = N == 2 ? 2 : N;
				for (int32 i = 0; i < Edges; ++i)
				{
					FVector2f Pos;
					if (N == 2)
					{
						const FVector2f Across = Perpendicular(Unit(Plan.Starts[1] - Plan.Starts[0]));
						Pos = Plan.Middle + Across * ((i == 0 ? -1.f : 1.f) * FMath::Min(Plan.SizeX, Plan.SizeY) * 0.3f);
					}
					else
					{
						Pos = (Plan.Starts[i] + Plan.Starts[(i + 1) % N]) * 0.5f;
						// Inwards a little, over the lake's side outwards.
						Pos = Plan.bLake ? Pos : Pos + (Plan.Middle - Pos) * 0.12f;
					}
					const bool bWanted = R.Chance(RingChance);
					const float Radius = R.Between(130.f, 200.f);
					const bool bVertical = R.Chance(0.5f);
					if (bWanted && TownFits(Pos, Radius))
					{
						FPlanTown Town;
						Town.Center = Pos;
						Town.Radius = Radius;
						Town.bVertical = bVertical;
						Plan.Towns.Add(Town);
					}
				}
				// Farms in the open, in the places that have fields or huts.
				const FEnvironmentDef& Env = GetEnvironmentDefs()[Plan.Environment];
				const int32 Farms = FMath::FloorToInt32(FarmCount + R.Frac());
				for (int32 i = 0; i < Farms && Env.FarmBuildings.Num() > 0; ++i)
				{
					for (int32 Try = 0; Try < 12; ++Try)
					{
						const FVector2f Pos(R.Between(200.f, Plan.SizeX - 200.f), R.Between(200.f, Plan.SizeY - 200.f));
						const float Radius = R.Between(95.f, 130.f);
						if (TownFits(Pos, Radius))
						{
							FPlanTown Farm;
							Farm.Center = Pos;
							Farm.Radius = Radius;
							Farm.bFarm = true;
							Farm.bVertical = R.Chance(0.5f);
							Plan.Towns.Add(Farm);
							break;
						}
					}
				}
			}

			void PlanRoadsOnCorridors()
			{
				float Chance = 0.7f;
				switch (Plan.Layout)
				{
				case ELayout::Rivers: Chance = 0.85f; break;
				case ELayout::Lake: Chance = 0.8f; break;
				case ELayout::Canyons: Chance = 0.5f; break;
				case ELayout::Highlands: Chance = 0.55f; break;
				case ELayout::City: Chance = 1.f; break;
				default: break;
				}
				for (FPlanCorridor& C : Plan.Corridors)
				{
					C.bRoad = R.Chance(Chance);
				}
			}

			bool SiteFits(const FVector2f& Pos, float Radius, float MinFromOtherStarts, int32 Owner) const
			{
				if (!InLand(Pos, Radius + 50.f) || WaterClearance(Pos) < Radius + 70.f)
				{
					return false;
				}
				for (int32 i = 0; i < Plan.Starts.Num(); ++i)
				{
					const float D = Dist(Plan.Starts[i], Pos);
					if (i == Owner ? D < 400.f + Radius : D < FMath::Max(MinFromOtherStarts, 400.f + Radius))
					{
						return false;
					}
				}
				for (const FPlanSite& S : Plan.Sites)
				{
					if (Dist(S.Pos, Pos) < Radius + (S.Kind == FPlanSite::Pile ? 40.f : 90.f))
					{
						return false;
					}
				}
				for (const FPlanTown& T : Plan.Towns)
				{
					if (Dist(T.Center, Pos) < T.Radius + Radius + 40.f)
					{
						return false;
					}
				}
				for (const FPlanCrossing& C : Plan.Crossings)
				{
					if (Dist(C.From, Pos) < Radius + 130.f || Dist(C.To, Pos) < Radius + 130.f)
					{
						return false;
					}
				}
				return true;
			}

			void AddSite(FPlanSite::EKind Kind, const FVector2f& Pos)
			{
				FPlanSite& S = Plan.Sites.AddDefaulted_GetRef();
				S.Kind = Kind;
				S.Pos = Pos;
				S.Turns = float(R.Range(0, 7)) / 8.f;
			}

			/** Tries the offsets in turn (turns off the way to the middle, mirrored the same for everybody): true if one fits. */
			bool PlaceAround(FPlanSite::EKind Kind, int32 Player, float Distance, float Radius, float MinOther, std::initializer_list<float> Offsets)
			{
				const FVector2f Start = Plan.Starts[Player];
				const FVector2f Inward = Unit(Plan.Middle - Start);
				for (float Offset : Offsets)
				{
					for (float Mirror : { 1.f, -1.f })
					{
						const FVector2f Pos = Start + Rotated(Inward, Offset * Mirror * float(Sign)) * Distance;
						if (SiteFits(Pos, Radius, MinOther, Player))
						{
							AddSite(Kind, Pos);
							return true;
						}
					}
				}
				return false;
			}

			void PlanResources()
			{
				// Everybody gets the same: the same things at the same distances, turned the same way as far as the land allows.
				const int32 Rich = FMath::Clamp(P.Resources, 0, 2);
				const int32 N = Plan.Players;
				for (int32 i = 0; i < N; ++i)
				{
					// The base's own supply dock (official maps: 450 to 600 from the start).
					PlaceAround(FPlanSite::Dock, i, 510.f, 80.f, 450.f, { 0.34f, 0.28f, 0.4f, 0.22f, 0.46f, 0.15f, 0.5f, 0.08f });
				}
				for (int32 i = 0; i < N; ++i)
				{
					if (Rich >= 1)
					{
						// The next one lies out towards the neighbours.
						PlaceAround(FPlanSite::Dock, i, 1020.f, 80.f, 760.f, { 0.1f, 0.17f, 0.04f, 0.24f, 0.0f });
					}
					// Oil.
					PlaceAround(FPlanSite::Derrick, i, 840.f, 40.f, 620.f, { 0.2f, 0.28f, 0.14f, 0.34f, 0.08f, 0.4f, 0.02f });
					if (Rich >= 2)
					{
						PlaceAround(FPlanSite::Derrick, i, 1130.f, 40.f, 700.f, { 0.03f, 0.09f, 0.15f, 0.21f });
					}
					// Small heaps of supplies near the base.
					const int32 Piles = Rich * 2;
					for (int32 k = 0; k < Piles; ++k)
					{
						const float Offset = 0.42f - 0.035f * float(k);
						PlaceAround(FPlanSite::Pile, i, 650.f + 26.f * float(k), 22.f, 520.f, { Offset, Offset + 0.06f, Offset - 0.06f, Offset - 0.12f });
					}
					if (Rich >= 1 && R.Chance(0.6f))
					{
						PlaceAround(FPlanSite::Bunker, i, 770.f, 24.f, 500.f, { 0.1f, 0.16f, 0.06f });
					}
				}
				// What everybody fights over: around the middle, the same way from each base.
				const bool bMiddleFree = !Plan.bLake && Plan.Rivers.Num() == 0 && Plan.Layout != ELayout::City;
				if (bMiddleFree && N >= 3 && SiteFits(Plan.Middle, 80.f, 500.f, INDEX_NONE))
				{
					AddSite(FPlanSite::Dock, Plan.Middle);
				}
				static const FPlanSite::EKind TechKinds[] = { FPlanSite::Reinforcement, FPlanSite::Refinery, FPlanSite::Artillery, FPlanSite::Hospital, FPlanSite::RepairBay };
				int32 NumTech = Rich == 0 ? 1 : (Rich == 1 ? FMath::Max(2, N / 2) : N);
				if (N == 2)
				{
					NumTech = Rich == 0 ? 2 : (Rich == 1 ? 2 : 4); // (in twos, one on either side: neither player is nearer)
				}
				const FVector2f Across = Perpendicular(Unit(Plan.Starts[1] - Plan.Starts[0]));
				const int32 FirstKind = R.Range(0, UE_ARRAY_COUNT(TechKinds) - 1);
				float Spread = FMath::Min(Plan.SizeX, Plan.SizeY) * 0.17f;
				if (Plan.bLake)
				{
					Spread = Plan.LakeRadius * 1.3f + 190.f;
				}
				else if (Plan.Layout == ELayout::City && Plan.Towns.Num() > 0)
				{
					Spread = Plan.Towns[0].Radius + 150.f;
				}
				else if (Plan.Rivers.Num() > 0)
				{
					Spread = FMath::Max(Spread, Plan.RiverHalfWidth * 1.3f + 200.f);
				}
				for (int32 k = 0; k < NumTech; ++k)
				{
					const FPlanSite::EKind Kind = TechKinds[(FirstKind + (N == 2 ? k / 2 : k)) % UE_ARRAY_COUNT(TechKinds)];
					const float Turn = N == 2 ? (k % 2 == 0 ? 0.f : 0.5f) + float(k / 2) * 0.09f : StartTurn + (float(k) + 0.5f) / float(NumTech);
					const FVector2f Dir = N == 2 ? Rotated(Across, Turn) : Rotated(FVector2f(1.f, 0.f), Turn);
					for (int32 Try = 0; Try < 10; ++Try)
					{
						const FVector2f Pos = Plan.Middle + Rotated(Dir, float(Try) * 0.021f * ((Try & 1) ? 1.f : -1.f)) * (Spread + float(Try) * 45.f);
						if (SiteFits(Pos, 70.f, 520.f, INDEX_NONE))
						{
							AddSite(Kind, Pos);
							break;
						}
					}
				}
			}
		};
	}

	bool MakePlan(const FGenRandomMapParams& Params, FPlan& Out)
	{
		Out = FPlan();
		FPlanner Planner(Params, Out);
		Planner.Run();
		return Out.Starts.Num() >= 2;
	}

	const TCHAR* GetLayoutKey(ELayout Layout)
	{
		return LayoutKeys[int32(Layout)];
	}
}

using namespace GenRandomMapDetail;

FString FGenRandomMapParams::ToName() const
{
	const TArray<FEnvironmentDef>& Envs = GetEnvironmentDefs();
	return FString::Printf(TEXT("random:%s:%s:%s:%s:%s:%d:%d"),
		Envs.IsValidIndex(Environment) ? Envs[Environment].Key : AnyKey,
		(Layout >= 0 && Layout < int32(ELayout::Count)) ? LayoutKeys[Layout] : AnyKey,
		SizeKeys[FMath::Clamp(Size, 0, 3)],
		(TimeOfDay >= 0 && TimeOfDay <= 3) ? TimeKeys[TimeOfDay] : AnyKey,
		ResourceKeys[FMath::Clamp(Resources, 0, 2)],
		FMath::Clamp(Players, 2, 8),
		Seed);
}

bool FGenRandomMapParams::FromName(const FString& Name, FGenRandomMapParams& Out)
{
	if (!GenRandomMap::IsRandomName(Name))
	{
		return false;
	}
	TArray<FString> Parts;
	Name.ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() != 8)
	{
		return false;
	}
	FGenRandomMapParams P;
	const TArray<FEnvironmentDef>& Envs = GetEnvironmentDefs();
	P.Environment = Envs.IndexOfByPredicate([&Parts](const FEnvironmentDef& E) { return Parts[1].Equals(E.Key, ESearchCase::IgnoreCase); });
	P.Layout = FindKey(Parts[2], LayoutKeys, int32(ELayout::Count));
	P.Size = FindKey(Parts[3], SizeKeys, UE_ARRAY_COUNT(SizeKeys));
	P.TimeOfDay = FindKey(Parts[4], TimeKeys, UE_ARRAY_COUNT(TimeKeys));
	P.Resources = FindKey(Parts[5], ResourceKeys, UE_ARRAY_COUNT(ResourceKeys));
	P.Players = FCString::Atoi(*Parts[6]);
	P.Seed = FCString::Atoi(*Parts[7]);
	const bool bEnvironmentOk = P.Environment != INDEX_NONE || Parts[1].Equals(AnyKey, ESearchCase::IgnoreCase);
	const bool bLayoutOk = P.Layout != INDEX_NONE || Parts[2].Equals(AnyKey, ESearchCase::IgnoreCase);
	const bool bTimeOk = P.TimeOfDay != INDEX_NONE || Parts[4].Equals(AnyKey, ESearchCase::IgnoreCase);
	if (!bEnvironmentOk || !bLayoutOk || !bTimeOk || P.Size == INDEX_NONE || P.Resources == INDEX_NONE || P.Players < 2 || P.Players > 8)
	{
		return false;
	}
	// (INDEX_NONE is Any: "left to the seed".)
	Out = P;
	return true;
}

bool GenRandomMap::IsRandomName(const FString& Name)
{
	return Name.StartsWith(TEXT("random:"), ESearchCase::IgnoreCase);
}

const TArray<FString>& GenRandomMap::GetEnvironments()
{
	static const TArray<FString> Labels = []()
	{
		TArray<FString> Out;
		Out.Add(TEXT("Random"));
		for (const FEnvironmentDef& E : GetEnvironmentDefs())
		{
			Out.Add(E.Label);
		}
		return Out;
	}();
	return Labels;
}

const TArray<FString>& GenRandomMap::GetLayouts()
{
	static const TArray<FString> Labels = []()
	{
		TArray<FString> Out;
		Out.Add(TEXT("Random"));
		for (const TCHAR* L : LayoutLabels)
		{
			Out.Add(L);
		}
		return Out;
	}();
	return Labels;
}

FString GenRandomMap::GetLayoutDescription(int32 Layout)
{
	return (Layout >= 0 && Layout < int32(ELayout::Count)) ? FString(LayoutTexts[Layout]) : FString(TEXT("The seed decides what kind of land it is."));
}

bool GenRandomMap::MakeInfo(const FString& Name, FGenMapInfo& Out)
{
	FGenRandomMapParams Params;
	FPlan Plan;
	if (!FGenRandomMapParams::FromName(Name, Params) || !MakePlan(Params, Plan))
	{
		return false;
	}
	Out = FGenMapInfo();
	Out.Name = Name;
	Out.DisplayName = FString::Printf(TEXT("%s - %s (%d)"), GetEnvironmentDefs()[Plan.Environment].Label, LayoutLabels[int32(Plan.Layout)], Plan.Params.Seed);
	Out.NumPlayers = Plan.Players;
	Out.ExtentMin = FVector2f::ZeroVector;
	Out.ExtentMax = FVector2f(Plan.SizeX, Plan.SizeY);
	Out.Starts = Plan.Starts;
	for (const FPlanSite& S : Plan.Sites)
	{
		if (S.Kind == FPlanSite::Dock || S.Kind == FPlanSite::Pile)
		{
			Out.SupplyPositions.Add(S.Pos);
		}
		else if (S.Kind != FPlanSite::Bunker)
		{
			Out.TechPositions.Add(S.Pos);
		}
	}
	return true;
}

bool GenRandomMap::Generate(const FString& Name, FGenMapData& Out)
{
	FGenRandomMapParams Params;
	FPlan Plan;
	if (!FGenRandomMapParams::FromName(Name, Params) || !MakePlan(Params, Plan))
	{
		UE_LOG(LogGenerals, Error, TEXT("Random map: '%s' is not a valid name"), *Name);
		return false;
	}
	return BuildMap(Plan, Out);
}

FString GenRandomMap::GetMapIniFolder(const FString& Name)
{
	FGenRandomMapParams Params;
	FPlan Plan;
	if (!FGenRandomMapParams::FromName(Name, Params) || !MakePlan(Params, Plan))
	{
		return FString();
	}
	return FString::Printf(TEXT("Maps\\%s"), GetEnvironmentDefs()[Plan.Environment].MapIniDonor);
}
