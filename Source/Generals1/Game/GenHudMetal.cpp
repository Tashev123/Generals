// The machined metal HUD styles: the bar as three slabs of dark metal standing apart over the world, drawn from clean
// curves when the game runs. Satin metal with only a little of the studio's light on it, polished chamfers, black glass in
// milled pockets. The same shapes in every finish; a finish is only a set of colours.
#include "Game/GenHudStyle.h"
#include "W3D/GenTextureLoader.h"
#include "Async/ParallelFor.h"

namespace GenHudMetalDetail
{
	struct FFinish
	{
		uint32 Top, Bottom;   // the satin
		float Sweep;          // how strongly it reflects the softbox
		uint32 Hi, Lo;        // what the polished edges see: the light, the dark of the room
		uint32 Glass0, Glass1;
		uint32 KeyTop, KeyBottom;
		uint32 Tint;          // the light as this metal reflects it
	};

	// (Dark on purpose: the bar is there to be used, the game is what is to be looked at.)
	const FFinish Blue = { 0x303b47, 0x161c23, 0.05f, 0xa9b8c7, 0x06080b, 0x040609, 0x080e14, 0x3b4855, 0x222a33, 0x7d90a3 };
	const FFinish Red = { 0x4c1419, 0x210709, 0.05f, 0xc4a3a3, 0x0c0203, 0x060405, 0x0e0809, 0x5a1a20, 0x2e0a0e, 0xa06a6a };
	const FFinish Green = { 0x2e3a33, 0x141a16, 0.05f, 0xb3bba3, 0x060907, 0x040605, 0x080d0a, 0x39473e, 0x1f2822, 0x84917f };
	const FFinish Silver = { 0x595c61, 0x2b2d31, 0.06f, 0xc9ced4, 0x131417, 0x040508, 0x090b0f, 0x62656a, 0x3a3c41, 0xa3a8ae };
	const FFinish Graphite = { 0x2d2e31, 0x141417, 0.05f, 0xa8aaaf, 0x060607, 0x050506, 0x0a0a0c, 0x38393d, 0x1f2023, 0x85878c };
	const FFinish Titanium = { 0x47433c, 0x211f1b, 0.05f, 0xbdb5a5, 0x0b0a08, 0x050504, 0x0b0b08, 0x524d45, 0x2d2a25, 0x9c9586 };

	constexpr float CameoCorner = 3.f;

	struct FCorner { float X, Y, R; };
	struct FPocket { float X0, Y0, X1, Y1, R; };

	// ---- shapes ---------------------------------------------------------------------------------------------------

	float Smooth(float A, float B, float X)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	FVector3f Rgb(uint32 C)
	{
		return FVector3f(((C >> 16) & 255) / 255.f, ((C >> 8) & 255) / 255.f, (C & 255) / 255.f);
	}

	FVector3f Mix(const FVector3f& A, const FVector3f& B, float T)
	{
		return A + (B - A) * T;
	}

	/** Distance to a rounded box: below 0 inside. */
	float RBox(float U, float V, float X0, float Y0, float X1, float Y1, float R)
	{
		const float QX = FMath::Abs(U - (X0 + X1) * 0.5f) - (X1 - X0) * 0.5f + R;
		const float QY = FMath::Abs(V - (Y0 + Y1) * 0.5f) - (Y1 - Y0) * 0.5f + R;
		const float AX = FMath::Max(QX, 0.f), AY = FMath::Max(QY, 0.f);
		return FMath::Sqrt(AX * AX + AY * AY) + FMath::Min(FMath::Max(QX, QY), 0.f) - R;
	}

	/** A closed outline from its corners: each rounded with a soft curve with no visible start. */
	TArray<FVector2f> Rounded(const TArray<FCorner>& Corners)
	{
		TArray<FVector2f> Out;
		const int32 N = Corners.Num();
		for (int32 i = 0; i < N; ++i)
		{
			const FCorner& C = Corners[i];
			const FVector2f P0(Corners[(i + N - 1) % N].X, Corners[(i + N - 1) % N].Y), P1(C.X, C.Y), P2(Corners[(i + 1) % N].X, Corners[(i + 1) % N].Y);
			if (C.R <= 0.f)
			{
				Out.Add(P1);
				continue;
			}
			FVector2f DA = P0 - P1, DB = P2 - P1;
			const float LA = DA.Size(), LB = DB.Size();
			DA /= LA;
			DB /= LB;
			const float Angle = FMath::Acos(FMath::Clamp(FVector2f::DotProduct(DA, DB), -1.f, 1.f));
			const float T = FMath::Min3(C.R / FMath::Max(FMath::Tan(Angle * 0.5f), 1e-3f), LA * 0.49f, LB * 0.49f);
			const FVector2f From = P1 + DA * T, To = P1 + DB * T;
			constexpr int32 Steps = 14;
			for (int32 j = 0; j <= Steps; ++j)
			{
				const float F = float(j) / Steps;
				Out.Add(From * ((1.f - F) * (1.f - F)) + P1 * (2.f * F * (1.f - F)) + To * (F * F));
			}
		}
		return Out;
	}

	/** How a side's sections rise into their tabs: their corners, how far a shoulder runs per unit it rises, its two bends. */
	struct FShoulder { float Corner, Run, Foot, Head; };
	const FShoulder Shoulders[3] =
	{
		{ 9.f, 1.1f, 9.f, 7.f },    // America: smooth
		{ 9.f, 2.6f, 20.f, 16.f },  // China: long sweeps
		{ 4.f, 0.35f, 3.f, 3.f },   // GLA: hard steps
	};

	/** A section's edge: a rounded box with a tab rising from its top between X0 and X1 (X1 at the box's right side: flush with it). */
	TArray<FVector2f> PodEdge(const FShoulder& S, float W, float H, float TabH, float X0, float X1)
	{
		const float Run = TabH * S.Run;
		TArray<FCorner> C;
		C.Add({ 0.f, H, S.Corner });
		C.Add({ 0.f, TabH, S.Corner });
		C.Add({ X0 - Run, TabH, S.Foot });
		C.Add({ X0, 0.f, S.Head });
		if (X1 >= W)
		{
			C.Add({ W, 0.f, S.Corner });
		}
		else
		{
			C.Add({ X1, 0.f, S.Head });
			C.Add({ X1 + Run, TabH, S.Foot });
			C.Add({ W, TabH, S.Corner });
		}
		C.Add({ W, H, S.Corner });
		return Rounded(C);
	}

	FBox2D Box(float X0, float Y0, float X1, float Y1)
	{
		return FBox2D(FVector2D(X0, Y0), FVector2D(X1, Y1));
	}

	/** The same for all finishes: the three sections of a side, the radar's in the map's shape. */
	void MakeShape(int32 Side, float MapAspect, FGenBarShape& S)
	{
		using namespace GenBarLayout;
		constexpr float Lip = 7.f;     // the metal round a pocket
		constexpr float TabH = 20.f;   // the tabs of the money and of the general's button
		constexpr float TabMargin = 6.f;
		S.Side = FGenBarShape::ESide(Side);
		const FShoulder& Sh = Shoulders[Side];
		{
			// The map as big as it may be: no bars beside it, as far as its shape allows.
			constexpr float MapMaxW = 150.f, MapMaxH = 122.f, MapMinW = 88.f, Gasket = 1.9f, LampTab = 14.f, LampW = 28.f, LampH = 8.5f;
			float MapH = MapMaxH, MapW = MapMaxH * MapAspect;
			if (MapW > MapMaxW)
			{
				MapW = MapMaxW;
				MapH = MapMaxW / MapAspect;
			}
			MapW = FMath::Max(MapW, MapMinW);
			const float WellY = LampTab + 6.f;
			const float W = Lip + MapW + Gasket * 2.f + Lip, H = WellY + MapH + Gasket * 2.f + Lip;
			S.RadarWell = Box(Lip, WellY, W - Lip, H - Lip);
			S.Radar = S.RadarWell.ExpandBy(-double(Gasket));
			S.RadarCorner = 5.f;
			S.Lamp = Box(W - Lip - LampW, (LampTab - LampH) * 0.5f + 0.6f, W - Lip, (LampTab + LampH) * 0.5f + 0.6f);
			S.PodSize[BarPodRadar] = FVector2D(W, H);
			S.Outline[BarPodRadar] = PodEdge(Sh, W, H, LampTab, W - Lip - LampW - TabMargin, W);
		}
		{
			constexpr float WellPad = 5.f, KeyW = 30.f, KeyH = 24.f, KeyGap = 5.f, SlotH = 5.f, MoneyW = 84.f, PillH = 16.f;
			const float GridW = 7.f * CellW + 6.f * CellGapX, GridH = 2.f * CellH + CellGapY;
			const float SlotY = TabH + 6.f, WellX = Lip + KeyW + KeyGap, WellY = SlotY + SlotH + 5.f;
			const float WellW = GridW + WellPad * 2.f, WellH = GridH + WellPad * 2.f;
			const float W = WellX + WellW + Lip, H = WellY + WellH + Lip, CX = W * 0.5f;
			S.CommandWindow = Box(WellX, WellY, WellX + WellW, WellY + WellH);
			S.Grid = FVector2D(WellX + WellPad, WellY + WellPad);
			S.Power = Box(Lip, SlotY, W - Lip, SlotY + SlotH).ExpandBy(-0.7);
			S.Money = Box(CX - MoneyW * 0.5f, (TabH - PillH) * 0.5f + 1.f, CX + MoneyW * 0.5f, (TabH + PillH) * 0.5f + 1.f);
			for (int32 Row = 0; Row < 2; ++Row)
			{
				const float Y = float(S.Grid.Y) + Row * (CellH + CellGapY) + (CellH - KeyH) * 0.5f;
				S.Keys[Row] = Box(Lip, Y, Lip + KeyW, Y + KeyH);
			}
			S.PodSize[BarPodCommands] = FVector2D(W, H);
			S.Outline[BarPodCommands] = PodEdge(Sh, W, H, TabH, CX - MoneyW * 0.5f - TabMargin, CX + MoneyW * 0.5f + TabMargin);
		}
		{
			constexpr float WellPad = 5.6f, GaugeColumn = 14.f, GaugeW = 5.2f, GeneralW = 62.f, PillH = 16.f;
			const float QueueWide = 3.f * QueueW + 2.f * QueueGap, QueueTall = 3.f * QueueH + 2.f * QueueGap;
			const float WellY = TabH + 6.f;
			const float WellW = WellPad + QueueWide + GaugeColumn, WellH = WellPad * 2.f + QueueTall;
			const float W = Lip + WellW + Lip, H = WellY + WellH + Lip;
			S.RightWell = Box(Lip, WellY, Lip + WellW, WellY + WellH);
			S.Queue = FVector2D(Lip + WellPad, WellY + WellPad);
			// The general's experience: a gauge in the glass right of the queue, under a star.
			const float GaugeX = float(S.Queue.X) + QueueWide + GaugeColumn * 0.5f - 0.5f;
			S.ExpFill = Box(GaugeX - GaugeW * 0.5f, float(S.Queue.Y) + 13.5f, GaugeX + GaugeW * 0.5f, float(S.Queue.Y) + QueueTall);
			S.ExpStar = FVector2D(GaugeX, S.Queue.Y + 5.5);
			S.General = Box(W - Lip - GeneralW, (TabH - PillH) * 0.5f + 1.f, W - Lip, (TabH + PillH) * 0.5f + 1.f);
			S.PodSize[BarPodRight] = FVector2D(W, H);
			S.Outline[BarPodRight] = PodEdge(Sh, W, H, TabH, W - Lip - GeneralW - TabMargin, W);
		}
	}

	const FGenBarShape& ShapeOf(int32 Side, float MapAspect)
	{
		// (Kept: the HUD knows what it has made by a shape's address.)
		static TMap<int32, TUniquePtr<FGenBarShape>> Shapes;
		Side = FMath::Clamp(Side, 0, 2);
		const int32 Aspect = FMath::RoundToInt32(FMath::Clamp(MapAspect > 0.f ? MapAspect : 1.f, 0.25f, 4.f) * 64.f);
		TUniquePtr<FGenBarShape>& Found = Shapes.FindOrAdd(Aspect * 4 + Side);
		if (!Found)
		{
			Found = MakeUnique<FGenBarShape>();
			MakeShape(Side, Aspect / 64.f, *Found);
		}
		return *Found;
	}

	/** The crossings of a closed outline with a row, left to right. */
	void Crossings(const TArray<FVector2f>& Poly, float Y, TArray<float, TInlineAllocator<32>>& Out)
	{
		Out.Reset();
		for (int32 i = 0, N = Poly.Num(); i < N; ++i)
		{
			const FVector2f& A = Poly[i];
			const FVector2f& B = Poly[(i + 1) % N];
			if ((A.Y <= Y) != (B.Y <= Y))
			{
				Out.Add(A.X + (Y - A.Y) * (B.X - A.X) / (B.Y - A.Y));
			}
		}
		Out.Sort();
	}

	/** Signed distance to the outline in units, > 0 inside; only near the outline is it exact (beyond Range it stands at Range). */
	void OutlineDistance(const TArray<FVector2f>& Poly, int32 W, int32 H, float K, TArray<float>& D)
	{
		constexpr float Range = 30.f;
		D.SetNumUninitialized(W * H);
		ParallelFor(H, [&](int32 Row)
		{
			const float Y = (Row + 0.5f) / K;
			TArray<float, TInlineAllocator<32>> Cross;
			Crossings(Poly, Y, Cross);
			TArray<int32, TInlineAllocator<256>> Near;
			for (int32 i = 0, N = Poly.Num(); i < N; ++i)
			{
				const FVector2f& A = Poly[i];
				const FVector2f& B = Poly[(i + 1) % N];
				if (FMath::Min(A.Y, B.Y) - Range <= Y && FMath::Max(A.Y, B.Y) + Range >= Y)
				{
					Near.Add(i);
				}
			}
			int32 Crossed = 0;
			for (int32 Col = 0; Col < W; ++Col)
			{
				const float X = (Col + 0.5f) / K;
				while (Crossed < Cross.Num() && Cross[Crossed] <= X)
				{
					++Crossed;
				}
				float Best = Range * Range;
				for (int32 i : Near)
				{
					const FVector2f& A = Poly[i];
					const FVector2f& B = Poly[(i + 1) % Poly.Num()];
					if (FMath::Min(A.X, B.X) - Range > X || FMath::Max(A.X, B.X) + Range < X)
					{
						continue;
					}
					const FVector2f AB = B - A, AP(X - A.X, Y - A.Y);
					const float T = FMath::Clamp((AP.X * AB.X + AP.Y * AB.Y) / FMath::Max(AB.X * AB.X + AB.Y * AB.Y, 1e-9f), 0.f, 1.f);
					const float DX = AP.X - T * AB.X, DY = AP.Y - T * AB.Y;
					Best = FMath::Min(Best, DX * DX + DY * DY);
				}
				const float Dist = FMath::Sqrt(Best);
				D[Row * W + Col] = (Crossed % 2) == 1 ? Dist : -Dist;
			}
		});
	}

	/** Distance to a star, below 0 inside. */
	float StarDistance(float X, float Y, float CX, float CY, float R)
	{
		FVector2f P[10];
		for (int32 i = 0; i < 10; ++i)
		{
			const float A = -UE_PI * 0.5f + i * UE_PI / 5.f;
			const float Q = (i % 2) == 0 ? R : R * 0.42f;
			P[i] = FVector2f(CX + Q * FMath::Cos(A), CY + Q * FMath::Sin(A));
		}
		float Best = 1e9f;
		bool bInside = false;
		for (int32 i = 0; i < 10; ++i)
		{
			const FVector2f& A = P[i];
			const FVector2f& B = P[(i + 1) % 10];
			const FVector2f AB = B - A, AP(X - A.X, Y - A.Y);
			const float T = FMath::Clamp((AP.X * AB.X + AP.Y * AB.Y) / FMath::Max(AB.X * AB.X + AB.Y * AB.Y, 1e-9f), 0.f, 1.f);
			Best = FMath::Min(Best, FMath::Square(AP.X - T * AB.X) + FMath::Square(AP.Y - T * AB.Y));
			if (((A.Y <= Y) != (B.Y <= Y)) && X < A.X + (Y - A.Y) * AB.X / AB.Y)
			{
				bInside = !bInside;
			}
		}
		const float Dist = FMath::Sqrt(Best);
		return bInside ? -Dist : Dist;
	}

	/** The options key's picture on a 24 x 24 grid, distance to it: two overlapping windows. */
	float OptionsGlyph(float X, float Y)
	{
		const float Front = RBox(X, Y, 3.5f, 8.5f, 16.f, 18.5f, 2.f);
		const float Back = RBox(X, Y, 8.f, 5.f, 20.5f, 15.5f, 2.f);
		const float FrontLine = FMath::Abs(Front) - 0.85f;
		const float BackLine = FMath::Max(FMath::Abs(Back) - 0.85f, -(Front - 0.85f));
		return FMath::Min(FrontLine, BackLine);
	}

	/** The idle worker key's picture on a 24 x 24 grid: a hammer crossed with a wrench. */
	float WorkerGlyph(float X, float Y)
	{
		constexpr float Cos45 = 0.70710678f;
		const float DX = X - 12.f, DY = Y - 12.f;
		const float HX = 12.f + DX * Cos45 + DY * Cos45, HY = 12.f - DX * Cos45 + DY * Cos45;   // into the hammer's frame
		const float WX = 12.f + DX * Cos45 - DY * Cos45, WY = 12.f + DX * Cos45 + DY * Cos45;   // into the wrench's
		const float Hammer = FMath::Min(RBox(HX, HY, 11.f, 6.f, 13.f, 21.5f, 1.f), RBox(HX, HY, 7.5f, 3.2f, 17.3f, 7.8f, 1.3f));
		const float Head = FMath::Max(FMath::Sqrt(FMath::Square(WX - 12.f) + FMath::Square(WY - 6.f)) - 4.2f, -RBox(WX, WY, 10.7f, 0.f, 13.3f, 6.1f, 0.2f));
		const float Wrench = FMath::Min(RBox(WX, WY, 11.f, 7.5f, 13.f, 21.5f, 1.f), Head);
		return FMath::Min(Hammer, Wrench);
	}

	// ---- the picture ----------------------------------------------------------------------------------------------

	/** A picture being made, colour and alpha in 0..1, gamma space. Coordinates in units unless said. */
	class FCanvas
	{
	public:
		FCanvas(int32 InW, int32 InH, float InK, float InRows, float InWidth, const FFinish& InF)
			: W(InW), H(InH), K(InK), Rows(InRows), Width(InWidth), AA(0.5f / InK), F(InF), HiC(Rgb(InF.Hi)), LoC(Rgb(InF.Lo)), TintC(Rgb(InF.Tint))
		{
			C.SetNumUninitialized(W * H);
			constexpr int32 Across = 90;
			StreakW = FMath::Max(8, W / Across) + 2;
			Streak.SetNumUninitialized(StreakW * H);
			FRandomStream Random(4);
			for (float& V : Streak)
			{
				const float U1 = FMath::Max(Random.GetFraction(), 1e-6f), U2 = Random.GetFraction();
				V = FMath::Sqrt(-2.f * FMath::Loge(U1)) * FMath::Cos(2.f * UE_PI * U2);
			}
		}

		float U(int32 X) const { return (X + 0.5f) / K; }
		float V(int32 Y) const { return (Y + 0.5f) / K; }
		float Boxes(float Un) const
		{
			const float A = (Un - Width * 0.27f) / (Width * 0.17f), B = (Un - Width * 0.74f) / (Width * 0.13f);
			return FMath::Clamp(FMath::Exp(-A * A) + FMath::Exp(-B * B), 0.f, 1.f);
		}
		/** The fine horizontal grain of the metal: it only shows where the light is on it. */
		float Grain(int32 X, int32 Y) const
		{
			const float Fx = float(X) / W * (StreakW - 2);
			const int32 I = FMath::FloorToInt32(Fx);
			const float T = Fx - I;
			const float* Row = &Streak[Y * StreakW];
			return FMath::Lerp(Row[I], Row[I + 1], T * T * (3.f - 2.f * T));
		}
		static float Speck(int32 X, int32 Y)
		{
			uint32 S = uint32(X * 73856093) ^ uint32(Y * 19349663);
			S ^= S << 13;
			S ^= S >> 17;
			S ^= S << 5;
			return (float(S & 0xFFFF) + float(S >> 16)) / 65535.f - 1.f;
		}

		template<typename Fn>
		void Each(int32 Y0, int32 Y1, Fn&& Do)
		{
			Y0 = FMath::Clamp(Y0, 0, H);
			Y1 = FMath::Clamp(Y1, Y0, H);
			ParallelFor(Y1 - Y0, [&, Y0](int32 Row)
			{
				const int32 Y = Y0 + Row;
				for (int32 X = 0; X < W; ++X)
				{
					Do(X, Y);
				}
			});
		}
		template<typename Fn>
		void EachIn(float X0, float Y0, float X1, float Y1, Fn&& Do)
		{
			const int32 PX0 = FMath::Clamp(FMath::FloorToInt32(X0 * K) - 1, 0, W), PX1 = FMath::Clamp(FMath::CeilToInt32(X1 * K) + 1, PX0, W);
			const int32 PY0 = FMath::Clamp(FMath::FloorToInt32(Y0 * K) - 1, 0, H), PY1 = FMath::Clamp(FMath::CeilToInt32(Y1 * K) + 1, PY0, H);
			ParallelFor(PY1 - PY0, [&, PX0, PX1, PY0](int32 Row)
			{
				const int32 Y = PY0 + Row;
				for (int32 X = PX0; X < PX1; ++X)
				{
					Do(X, Y);
				}
			});
		}

		/** The satin: lit from above, holding the reflection of a long softbox: a band of light bowed a little across the width. */
		void Body()
		{
			const FVector3f Top = Rgb(F.Top), Bottom = Rgb(F.Bottom);
			Each(0, H, [&](int32 X, int32 Y)
			{
				const float Un = U(X), Vn = V(Y);
				FVector3f Out = Mix(Top, Bottom, FMath::Pow(Smooth(0.f, Rows, Vn), 0.85f));
				const float Bow = FMath::Square((Un - Width * 0.5f) / (Width * 0.5f)) * Rows * 0.05f;
				const float Lower = Rows * 0.36f + Bow;
				const float Band = Smooth(Lower + Rows * 0.26f, Lower - Rows * 0.12f, Vn);
				const float Upper = Smooth(Rows * -0.05f, Rows * 0.16f, Vn);
				const float Lateral = 0.42f + 0.58f * Boxes(Un);
				const float Spec = Band * Upper * Lateral * F.Sweep;
				Out *= 1.f - 0.14f * Smooth(Rows * 0.35f, Rows * 0.95f, Vn);
				Out += FVector3f(0.012f * Smooth(Rows * 0.86f, Rows, Vn) * Lateral);
				const float G = Grain(X, Y) * 0.0036f;
				Out += TintC * (Spec * (1.f + G * 4.f));
				Out += FVector3f(G * (0.35f + Spec * 3.f) + Speck(X, Y) * 0.0048f);
				C[Y * W + X] = Out;
			});
		}

		/** The outer edge from a distance field: the satin's roll, then a mirror chamfer: it shows the softbox, or the dark of the room. */
		void OuterEdge(const TArray<float>& D, float Chamfer = 1.f)
		{
			Each(0, H, [&](int32 X, int32 Y)
			{
				const float Dist = D[Y * W + X];
				if (Dist <= -2.f || Dist > 20.f)
				{
					return;
				}
				const float GX = D[Y * W + FMath::Min(X + 1, W - 1)] - D[Y * W + FMath::Max(X - 1, 0)];
				const float GY = D[FMath::Min(Y + 1, H - 1) * W + X] - D[FMath::Max(Y - 1, 0) * W + X];
				const float Len = FMath::Max(FMath::Sqrt(GX * GX + GY * GY), 1e-5f);
				const float L = (GY / Len) * 0.97f + (GX / Len) * 0.24f;
				const float B = Boxes(U(X));
				FVector3f& Out = C[Y * W + X];
				const float Roll = FMath::Exp(-FMath::Max(Dist - Chamfer, 0.f) / 2.8f);
				Out += FVector3f(Roll * FMath::Clamp(L, -0.6f, 1.f) * (0.03f + 0.03f * B));
				const FVector3f Cham = Mix(LoC, Mix(TintC, HiC, B), Smooth(-0.25f, 0.6f, L) * (0.5f + 0.5f * B));
				Out = Mix(Out, Cham, 1.f - Smooth(Chamfer - AA * 1.5f, Chamfer + AA * 1.5f, Dist));
				Out *= 1.f - FMath::Exp(-FMath::Square((Dist - Chamfer - 0.3f) / 0.26f)) * 0.22f;
			});
		}

		/** The polished wall of something milled in, Offset outside the rounded box. */
		void Wall(const FPocket& P, float Offset, float WallW, float Strength)
		{
			const float Pad = Offset + WallW + 2.f;
			EachIn(P.X0 - Pad, P.Y0 - Pad, P.X1 + Pad, P.Y1 + Pad, [&](int32 X, int32 Y)
			{
				const float Un = U(X), Vn = V(Y);
				const float Dist = RBox(Un, Vn, P.X0, P.Y0, P.X1, P.Y1, P.R) - Offset;
				if (Dist < -2.f * AA)
				{
					return;
				}
				const float E = 0.5f * AA;
				float GX = RBox(Un + E, Vn, P.X0, P.Y0, P.X1, P.Y1, P.R) - RBox(Un - E, Vn, P.X0, P.Y0, P.X1, P.Y1, P.R);
				float GY = RBox(Un, Vn + E, P.X0, P.Y0, P.X1, P.Y1, P.R) - RBox(Un, Vn - E, P.X0, P.Y0, P.X1, P.Y1, P.R);
				const float Len = FMath::Max(FMath::Sqrt(GX * GX + GY * GY), 1e-6f);
				// (It faces the middle: its lower side faces up.)
				const float L = (GY / Len) * 0.97f + (GX / Len) * 0.24f;
				const float B = Boxes(Un);
				const FVector3f WC = Mix(LoC, Mix(TintC, HiC, B) * Strength, Smooth(-0.2f, 0.75f, L) * (0.42f + 0.58f * B));
				const float Ring = (1.f - Smooth(WallW - AA * 1.5f, WallW + AA * 1.5f, Dist)) * Smooth(-AA * 1.5f, AA * 1.5f, Dist);
				FVector3f& Out = C[Y * W + X];
				Out = Mix(Out, WC, Ring);
				Out *= 1.f - FMath::Exp(-FMath::Square((Dist - WallW - 0.25f) / 0.3f)) * 0.3f;
			});
		}

		/** A pocket milled into the slab: a polished wall going down to black glass. */
		void Pocket(const FPocket& P, float WallW = 0.95f, float Gasket = 0.9f, float Sheen = 0.016f, bool bTint = true)
		{
			Wall(P, 0.f, WallW, 0.85f);
			const FVector3f G0 = Rgb(F.Glass0), G1 = Rgb(F.Glass1);
			const FVector3f Black(0.006f, 0.007f, 0.008f);
			EachIn(P.X0, P.Y0, P.X1, P.Y1, [&](int32 X, int32 Y)
			{
				const float Un = U(X), Vn = V(Y);
				const float Dist = RBox(Un, Vn, P.X0, P.Y0, P.X1, P.Y1, P.R);
				const float A = FMath::Clamp(0.5f - Dist / (2.f * AA), 0.f, 1.f);
				if (A <= 0.f)
				{
					return;
				}
				const float VV = (Vn - P.Y0) / FMath::Max(P.Y1 - P.Y0, 1e-3f);
				FVector3f Glass = bTint ? Mix(G0, G1, FMath::Clamp(VV, 0.f, 1.f)) : G0;
				if (Sheen > 0.f)
				{
					const float UU = (Un - P.X0) / (P.X1 - P.X0);
					Glass += FVector3f(Smooth(0.47f, 0.40f, UU * 0.9f + VV * 0.33f) * Smooth(0.f, 1.f, 1.f - VV) * Sheen);
				}
				Glass += FVector3f(FMath::Exp(-FMath::Square((Dist + Gasket) / 0.22f)) * 0.045f);
				Glass = Mix(Glass, Black, 1.f - Smooth(-Gasket - AA, -Gasket + AA, Dist));
				FVector3f& Out = C[Y * W + X];
				Out = Mix(Out, Glass, A);
			});
		}

		/** The slot a machined key stands in. */
		void KeySlot(const FPocket& P)
		{
			constexpr float Gap = 0.8f;
			const FVector3f Dark(0.008f, 0.008f, 0.01f);
			EachIn(P.X0 - 2.f, P.Y0 - 2.f, P.X1 + 2.f, P.Y1 + 2.f, [&](int32 X, int32 Y)
			{
				const float Dist = RBox(U(X), V(Y), P.X0, P.Y0, P.X1, P.Y1, P.R);
				FVector3f& Out = C[Y * W + X];
				Out = Mix(Out, Dark, 1.f - Smooth(Gap - AA, Gap + AA, Dist));
			});
			Wall(P, Gap, 0.55f, 0.6f);
		}

		/** Black inside the outline's shadow and the alpha of the outline: the finished picture. */
		void Finish(const TArray<float>& D, FGenImage& Out) const
		{
			using GenBarLayout::ShadowPad;
			Out.Width = W;
			Out.Height = H;
			Out.Pixels.SetNumUninitialized(W * H);
			for (int32 i = 0; i < W * H; ++i)
			{
				const float Dist = D[i];
				const float Cover = FMath::Clamp(Dist / (2.f * AA) + 0.5f, 0.f, 1.f);
				const float Outside = FMath::Max(-Dist, 0.f);
				// (Gone before the picture's edge.)
				const float Shadow = FMath::Clamp(FMath::Exp(-Outside / 2.2f) * 0.5f + FMath::Exp(-Outside / 8.f) * 0.2f, 0.f, 1.f) * Smooth(ShadowPad * 0.95f, ShadowPad * 0.4f, Outside);
				const float Alpha = Cover + (1.f - Cover) * Shadow;
				const FVector3f Rgba = C[i] * (Cover / FMath::Max(Alpha, 1e-3f));
				Out.Pixels[i] = Pixel(Rgba, Alpha);
			}
		}

		static FColor Pixel(const FVector3f& V, float A)
		{
			auto Byte = [](float F) { return uint8(FMath::Clamp(F, 0.f, 1.f) * 255.f + 0.5f); };
			return FColor(Byte(V.X), Byte(V.Y), Byte(V.Z), Byte(A));
		}

		TArray<FVector3f> C;
		const int32 W, H;
		const float K, Rows, Width, AA;
		const FFinish& F;
		const FVector3f HiC, LoC, TintC;
		int32 StreakW = 0;
		TArray<float> Streak;
	};

	void MakeImage(int32 W, int32 H, FGenImage& Out)
	{
		Out.Width = W;
		Out.Height = H;
		Out.Pixels.SetNumUninitialized(W * H);
	}

	/** A key's face: satin, slightly domed, with a polished edge. Coordinates in pixels; returns colour, alpha is the shape's. */
	FVector3f KeyFace(const FFinish& F, float PX, float PY, int32 W, int32 H, float CornerPx, float EdgePx, float Light, float& OutAlpha)
	{
		const float Dist = RBox(PX, PY, 0.f, 0.f, float(W), float(H), CornerPx);
		OutAlpha = FMath::Clamp(0.5f - Dist, 0.f, 1.f);
		const float T = PY / H;
		const float Dome = FMath::Exp(-FMath::Square((T - 0.28f) / 0.28f));
		FVector3f Face = Mix(Rgb(F.KeyTop), Rgb(F.KeyBottom), T);
		Face += FVector3f(Dome * 0.03f * Light + FCanvas::Speck(int32(PX), int32(PY)) * 0.004f);
		Face *= Light;
		const float E = 0.5f;
		const float GX = RBox(PX + E, PY, 0.f, 0.f, float(W), float(H), CornerPx) - RBox(PX - E, PY, 0.f, 0.f, float(W), float(H), CornerPx);
		const float GY = RBox(PX, PY + E, 0.f, 0.f, float(W), float(H), CornerPx) - RBox(PX, PY - E, 0.f, 0.f, float(W), float(H), CornerPx);
		const float Len = FMath::Max(FMath::Sqrt(GX * GX + GY * GY), 1e-6f);
		const float L = (-GY / Len) * 0.97f + (-GX / Len) * 0.24f;
		const FVector3f Edge = Mix(Rgb(F.Lo), Mix(Rgb(F.Tint), Rgb(F.Hi), 0.5f), Smooth(-0.3f, 0.6f, L) * 0.8f);
		return Mix(Face, Edge, Smooth(-EdgePx - 0.75f, -EdgePx + 0.75f, Dist));
	}

	void BuildKey(const FFinish& F, EGenHudPart Part, int32 State, float Scale, int32 W, int32 H, FGenImage& Out)
	{
		MakeImage(W, H, Out);
		const float Corner = (Part == EGenHudPart::GeneralKey ? 5.2f : 5.3f) * Scale;
		const float Glyph = H * 0.66f;
		const float GX0 = (W - Glyph) * 0.5f, GY0 = (H - Glyph) * 0.5f, GU = Glyph / 24.f;
		const float CX = W * 0.5f, CY = H * 0.5f;
		const float Light = State == HudKeyHover ? 1.4f : 1.f;
		ParallelFor(H, [&](int32 Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float PX = X + 0.5f, PY = Y + 0.5f;
				float Alpha = 0.f;
				FVector3f Col = KeyFace(F, PX, PY, W, H, Corner, 0.7f * Scale, Light, Alpha);
				if (Part == EGenHudPart::GeneralKey)
				{
					// Three stars: gold and glowing when lit, engraved when not.
					const float Dist = FMath::Min3(StarDistance(PX, PY, CX, CY, H * 0.33f),
						StarDistance(PX, PY, CX - H * 0.72f, CY + H * 0.03f, H * 0.2f), StarDistance(PX, PY, CX + H * 0.72f, CY + H * 0.03f, H * 0.2f));
					const bool bLit = State == HudKeyHover;
					if (bLit)
					{
						Col += FVector3f(1.f, 0.72f, 0.2f) * (FMath::Exp(-FMath::Max(Dist, 0.f) / (1.3f * Scale)) * 0.3f);
					}
					Col = Mix(Col, bLit ? Rgb(0xf0c040) : Rgb(0x8a8f96), FMath::Clamp(0.5f - Dist, 0.f, 1.f));
				}
				else
				{
					const float GUx = (PX - GX0) / GU, GUy = (PY - GY0) / GU;
					const float Dist = (Part == EGenHudPart::OptionsKey ? OptionsGlyph(GUx, GUy) : WorkerGlyph(GUx, GUy)) * GU;
					const FVector3f Ink = State == HudKeyDisabled ? Col * 0.62f : (Part == EGenHudPart::OptionsKey ? Rgb(State == HudKeyHover ? 0xeef2f6 : 0xb7c0ca) : Rgb(State == HudKeyHover ? 0xffd27a : 0xd0ab62));
					Col = Mix(Col, Ink, FMath::Clamp(0.5f - Dist, 0.f, 1.f) * (State == HudKeyDisabled ? 0.8f : 0.95f));
				}
				Out.At(X, Y) = FCanvas::Pixel(Col, Alpha);
			}
		});
	}

	void BuildLamp(bool bOn, int32 Bloom, int32 W, int32 H, FGenImage& Out)
	{
		MakeImage(W, H, Out);
		const float X0 = float(Bloom), Y0 = float(Bloom), X1 = float(W - Bloom), Y1 = float(H - Bloom);
		const float PW = X1 - X0, PH = Y1 - Y0, R = FMath::Min(PW, PH) * 0.5f;
		ParallelFor(H, [&](int32 Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float PX = X + 0.5f, PY = Y + 0.5f;
				const float Dist = RBox(PX, PY, X0, Y0, X1, Y1, R);
				const float Cover = FMath::Clamp(0.5f - Dist, 0.f, 1.f);
				const float U = (PX - (X0 + X1) * 0.5f) / (PW * 0.6f), V = (PY - (Y0 + PH * 0.42f)) / (PH * 0.7f);
				const float Rad = FMath::Sqrt(U * U + V * V);
				FVector3f Glass;
				if (bOn)
				{
					Glass = Rad < 0.42f ? Mix(Rgb(0xff9a8e), Rgb(0xff3b30), Rad / 0.42f) : Mix(Rgb(0xff3b30), Rgb(0xc90d12), FMath::Clamp((Rad - 0.42f) / 0.58f, 0.f, 1.f));
				}
				else
				{
					Glass = Mix(Rgb(0x1c0709), Rgb(0x2a090c), FMath::Clamp((PY - Y0) / PH, 0.f, 1.f));
				}
				// The window reflected on the glass, along its top.
				const float T = (PY - Y0) / PH;
				const float Across = FMath::Abs(PX - (X0 + X1) * 0.5f) / (PW * 0.5f);
				if (T > 0.12f && T < 0.42f && Across < 0.8f)
				{
					Glass += FVector3f((bOn ? 0.55f : 0.05f) * (1.f - (T - 0.12f) / 0.3f));
				}
				float Alpha = Cover;
				FVector3f Col = Glass;
				if (bOn && Bloom > 0 && Cover < 1.f)
				{
					// (The glow on the metal round it.)
					const float Outside = FMath::Max(Dist, 0.f) / Bloom;
					const float Glow = FMath::Clamp(FMath::Exp(-Outside * 3.2f) * 0.75f - Outside * 0.08f, 0.f, 1.f);
					Alpha = Cover + (1.f - Cover) * Glow;
					Col = Alpha > 0.f ? (Glass * Cover + FVector3f(1.f, 0.2f, 0.15f) * ((1.f - Cover) * Glow)) / Alpha : Glass;
				}
				Out.At(X, Y) = FCanvas::Pixel(Col, Alpha);
			}
		});
	}

	/** A capsule, its colour running from one end's colour to the other's: across the short side for a bar, along the length for a gauge. */
	void BuildCapsule(uint32 C0, uint32 C1, int32 W, int32 H, FGenImage& Out)
	{
		MakeImage(W, H, Out);
		const float R = FMath::Min(W, H) * 0.5f;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float Dist = RBox(X + 0.5f, Y + 0.5f, 0.f, 0.f, float(W), float(H), R);
				Out.At(X, Y) = FCanvas::Pixel(Mix(Rgb(C0), Rgb(C1), (Y + 0.5f) / H), FMath::Clamp(0.5f - Dist, 0.f, 1.f));
			}
		}
	}

	void BuildCell(const FFinish& F, EGenHudPart Part, int32 CornerPx, int32 W, int32 H, FGenImage& Out)
	{
		MakeImage(W, H, Out);
		const float Line = FMath::Max(1.f, H / 60.f);
		const FColor Glass = FCanvas::Pixel(Rgb(F.Glass0), 1.f);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float Dist = RBox(X + 0.5f, Y + 0.5f, 0.f, 0.f, float(W), float(H), float(CornerPx));
				float A = FMath::Clamp(0.5f - Dist, 0.f, 1.f);
				if (Part == EGenHudPart::CellRing)
				{
					A *= FMath::Clamp(Dist + Line + 0.5f, 0.f, 1.f);
				}
				else if (Part == EGenHudPart::CellCorners)
				{
					A = 1.f - A;
				}
				const uint8 Alpha = uint8(A * 255.f + 0.5f);
				Out.At(X, Y) = Part == EGenHudPart::CellCorners ? FColor(Glass.R, Glass.G, Glass.B, Alpha) : FColor(255, 255, 255, Alpha);
			}
		}
	}

	/** The power shortcuts' rail: a pocket per power, its right side running off the screen, with its shadow round the rest. */
	void BuildRail(const FFinish& F, int32 Count, float Scale, int32 W, int32 H, FGenImage& Out)
	{
		using namespace GenBarLayout;
		const float Wide = W / Scale, Rows = H / Scale;
		const float Pad = (Wide - RailW) * 0.5f;
		FCanvas Can(W, H, Scale, Rows, Wide, F);
		Can.Body();
		TArray<float> D;
		D.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				D[Y * W + X] = -RBox(Can.U(X), Can.V(Y), Pad, Pad, Wide + 20.f, Rows - Pad, 8.f);
			}
		}
		Can.OuterEdge(D, 0.9f);
		for (int32 i = 0; i < Count; ++i)
		{
			const float Y0 = Pad + RailTop + i * RailPitch, X0 = Pad + RailCellX;
			Can.Pocket({ X0 - 0.7f, Y0 - 0.7f, X0 + RailCellW + 0.7f, Y0 + RailCellH + 0.7f, CameoCorner + 0.5f }, 0.7f, 0.3f, 0.f);
		}
		Can.Finish(D, Out);
	}

	class FGenMetalHudStyle final : public FGenHudStyle
	{
	public:
		/** With Single: that finish on every side. Without: each side its own colour. */
		FGenMetalHudStyle(const TCHAR* InId, const TCHAR* InName, const FFinish* InSingle) : FGenHudStyle(InId, InName), Single(InSingle) {}

		const FFinish& FinishOf(int32 Side) const
		{
			return Single ? *Single : (Side == FGenBarShape::China ? Red : Side == FGenBarShape::GLA ? Green : Blue);
		}

		virtual const FGenBarShape& GetShape(int32 Side, float MapAspect) const override
		{
			return ShapeOf(Side, MapAspect);
		}

		virtual bool BuildPod(const FGenBarShape& Shape, int32 Pod, float Scale, FGenImage& Out) const override
		{
			if (Scale <= 0.f || Pod < 0 || Pod >= BarPodCount)
			{
				return false;
			}
			// (Pixel for pixel: a section's picture lies on whole pixels of the screen, a whole number of them round it.)
			const int32 PadPx = FMath::RoundToInt32(GenBarLayout::ShadowPad * Scale);
			const int32 Width = FMath::CeilToInt32(float(Shape.PodSize[Pod].X) * Scale) + PadPx * 2, Height = FMath::CeilToInt32(float(Shape.PodSize[Pod].Y) * Scale) + PadPx * 2;
			const float Pad = PadPx / Scale;
			FCanvas Can(Width, Height, Scale, Height / Scale, Width / Scale, FinishOf(Shape.Side));
			TArray<FVector2f> Edge = Shape.Outline[Pod];
			for (FVector2f& P : Edge)
			{
				P += FVector2f(Pad, Pad);
			}
			TArray<float> D;
			OutlineDistance(Edge, Width, Height, Scale, D);
			Can.Body();
			Can.OuterEdge(D);
			auto Well = [Pad](const FBox2D& R, float Corner) { return FPocket{ float(R.Min.X) + Pad, float(R.Min.Y) + Pad, float(R.Max.X) + Pad, float(R.Max.Y) + Pad, Corner }; };
			auto Pill = [&Well](const FBox2D& R) { return Well(R, float(R.GetSize().Y) * 0.5f); };
			if (Pod == BarPodRadar)
			{
				Can.Pocket(Well(Shape.RadarWell, 7.f));
				Can.Pocket(Pill(Shape.Lamp.ExpandBy(0.6)), 0.7f, 0.3f, 0.f, false);
			}
			else if (Pod == BarPodCommands)
			{
				Can.Pocket(Well(Shape.CommandWindow, 7.f));
				Can.Pocket(Pill(Shape.Money), 0.95f, 0.9f, 0.f, false);
				Can.Pocket(Pill(Shape.Power.ExpandBy(0.7)), 0.7f, 0.3f, 0.f, false);
				Can.KeySlot(Well(Shape.Keys[0], 5.3f));
				Can.KeySlot(Well(Shape.Keys[1], 5.3f));
			}
			else
			{
				Can.Pocket(Well(Shape.RightWell, 7.f));
				Can.KeySlot(Well(Shape.General, 5.2f));
				// The experience gauge's track and its star, in the glass.
				const FPocket Gauge = Well(Shape.ExpFill, 0.f);
				const float StarX = float(Shape.ExpStar.X) + Pad, StarY = float(Shape.ExpStar.Y) + Pad;
				const FVector3f Gold = Rgb(0xc9a23c);
				Can.EachIn(Gauge.X0 - 8.f, StarY - 7.f, Gauge.X1 + 8.f, Gauge.Y1 + 1.f, [&](int32 X, int32 Y)
				{
					const float Un = Can.U(X), Vn = Can.V(Y);
					FVector3f& Col = Can.C[Y * Width + X];
					const float Track = RBox(Un, Vn, Gauge.X0, Gauge.Y0, Gauge.X1, Gauge.Y1, (Gauge.X1 - Gauge.X0) * 0.5f);
					Col = Mix(Col, FVector3f(1.f, 1.f, 1.f), FMath::Clamp(0.5f - Track * Scale, 0.f, 1.f) * 0.06f);
					Col = Mix(Col, Gold, FMath::Clamp(0.5f - StarDistance(Un, Vn, StarX, StarY, 5.f) * Scale, 0.f, 1.f));
				});
			}
			Can.Finish(D, Out);
			return true;
		}

		virtual bool IsPodSolid(const FGenBarShape& Shape, int32 Pod, const FVector2D& Place) const override
		{
			if (Place.X < 0.0 || Place.Y < 0.0 || Place.X >= Shape.PodSize[Pod].X || Place.Y >= Shape.PodSize[Pod].Y)
			{
				return false;
			}
			TArray<float, TInlineAllocator<32>> Cross;
			Crossings(Shape.Outline[Pod], float(Place.Y), Cross);
			int32 Before = 0;
			for (const float C : Cross)
			{
				Before += C <= Place.X ? 1 : 0;
			}
			return (Before % 2) == 1;
		}

		virtual bool BuildPart(const FGenBarShape& Shape, EGenHudPart Part, int32 Variant, int32 Width, int32 Height, float Scale, FGenImage& Out) const override
		{
			if (Width <= 0 || Height <= 0)
			{
				return false;
			}
			const FFinish& F = FinishOf(Shape.Side);
			switch (Part)
			{
			case EGenHudPart::OptionsKey:
			case EGenHudPart::WorkerKey:
				BuildKey(F, Part, Variant, Height / 24.f, Width, Height, Out);
				return true;
			case EGenHudPart::GeneralKey:
				BuildKey(F, Part, Variant, Height / 16.f, Width, Height, Out);
				return true;
			case EGenHudPart::LampOff:
				BuildLamp(false, 0, Width, Height, Out);
				return true;
			case EGenHudPart::LampOn:
				BuildLamp(true, Variant, Width, Height, Out);
				return true;
			case EGenHudPart::PowerFillGood:
				BuildCapsule(0x62c78a, 0x239a57, Width, Height, Out);
				return true;
			case EGenHudPart::PowerFillTight:
				BuildCapsule(0xe6c565, 0xc69a26, Width, Height, Out);
				return true;
			case EGenHudPart::PowerFillShort:
				BuildCapsule(0xef7464, 0xc8332e, Width, Height, Out);
				return true;
			case EGenHudPart::ExpFill:
				BuildCapsule(0xdcb95c, 0xb0821c, Width, Height, Out);
				return true;
			case EGenHudPart::ShortcutRail:
				BuildRail(F, Variant, Scale, Width, Height, Out);
				return true;
			case EGenHudPart::CellRing:
			case EGenHudPart::CellFill:
			case EGenHudPart::CellCorners:
				BuildCell(F, Part, Variant, Width, Height, Out);
				return true;
			}
			return false;
		}

		virtual float GetCameoCorner() const override { return CameoCorner; }

	private:
		const FFinish* Single;
	};
}

void GenHudStyles::AddMetalStyles(TArray<const FGenHudStyle*>& List)
{
	using namespace GenHudMetalDetail;
	List.Add(new FGenMetalHudStyle(TEXT("Faction"), TEXT("Metal: faction colours"), nullptr));
	List.Add(new FGenMetalHudStyle(TEXT("Blue"), TEXT("Metal: anodized blue"), &Blue));
	List.Add(new FGenMetalHudStyle(TEXT("Red"), TEXT("Metal: anodized red"), &Red));
	List.Add(new FGenMetalHudStyle(TEXT("Green"), TEXT("Metal: anodized green"), &Green));
	List.Add(new FGenMetalHudStyle(TEXT("Silver"), TEXT("Metal: silver"), &Silver));
	List.Add(new FGenMetalHudStyle(TEXT("Graphite"), TEXT("Metal: graphite"), &Graphite));
	List.Add(new FGenMetalHudStyle(TEXT("Titanium"), TEXT("Metal: titanium"), &Titanium));
}
