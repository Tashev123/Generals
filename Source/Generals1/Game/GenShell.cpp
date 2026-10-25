// The shell: the main menu (MainMenu.cpp), the skirmish menu (SkirmishGameOptionsMenu.cpp, with the map
// selection of SkirmishMapSelectMenu.cpp), the online screen (host a network game or join one), the in-game
// menu (QuitMenu.cpp) and the score screen (ScoreScreen.cpp).
// Drawn by the HUD in immediate mode: each element is drawn and asked whether it was clicked in one go.
//
// The look is the original's colours and art (MainMenuBackdrop, GeneralsLogo, Mp_Load, the sides' score
// screens) in a plainer, modern cut: layered panels with soft gradients, thin borders and corners cut at an
// angle; hover and press ease in and out (ShellAnim). Everything is laid out in 1600 x 900 and scaled.
//
// Ids for test automation (-GenTestMenu="seconds,click,Id;..."). A test run starts on the skirmish setup -
// on the main menu if its script has "Main." ids:
//   Main.Skirmish, Main.Online, Main.Options, Main.Exit the main menu
//   SelectMap, Map=<name>, MapAccept, MapBack           the map selection
//   Slot<N>.State / .Color / .Side / .Team              opens that list of slot N (0 = the player's);
//   Slot<N>.State=<item> ...                            ... or chooses the item with that label at once
//   Cash, Cash=<item>, TestUnits                        starting cash, the battle group switch
//   Start<N>                                            a left click on start position N (1-based)
//   Play, Back                                          the skirmish menu's buttons (Back: to the main menu)
//   NetHost, NetJoin, NetRejoin, Online.Back            the online screen; Leave in a network game's lobby
//   Menu.Open, Menu.Resume, Menu.Options, Menu.Restart, Menu.Exit (to the score screen and the menu),
//   Menu.Quit, Menu.QuitYes, Menu.QuitNo                the in-game menu, quitting to the desktop
//   Score.Continue, Score.Watch                         the score screen

#include "Game/GenHUD.h"
#include "Audio/GenAudioData.h"
#include "Audio/GenAudioManager.h"
#include "Core/GenFileSystem.h"
#include "Game/GenGameMode.h"
#include "Game/GenPlayerController.h"
#include "Game/GenShellData.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSkirmishSetup.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Net/GenNetSubsystem.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/MemoryWriter.h"
#include "W3D/GenAssets.h"
#include "W3D/GenTextureLoader.h"
#include "Generals1.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "GlobalRenderResources.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace
{
	using namespace GenShellStyle;

	/**
	 * A crisp font at any size: the engine's Roboto, rendered as it is asked for (its other canvas fonts
	 * are bitmaps made for one size). A canvas text item wants the font as an asset, which this one is.
	 */
	FSlateFontInfo ShellFont(float Pixels, bool bBold, int32 LetterSpacing = 0)
	{
		static TWeakObjectPtr<UFont> Roboto;
		if (!Roboto.IsValid())
		{
			Roboto = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
		}
		FSlateFontInfo Font(Roboto.Get(), float(FMath::Max(6, FMath::RoundToInt32(Pixels * 0.75f))), FName(bBold ? TEXT("Bold") : TEXT("Regular")));
		Font.LetterSpacing = LetterSpacing;
		return Font;
	}

	FVector2D MeasureShellText(const FString& Text, const FSlateFontInfo& Font)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FVector2D(Text.Len() * Font.Size * 0.75, Font.Size * 1.6);
		}
		return FVector2D(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font));
	}

	/** "Units \n Created" -> "Created" (the score screen's column titles are two lines in the string file). */
	FString SecondLine(const FString& Text)
	{
		FString Left, Right;
		if (Text.Split(TEXT("\\n"), &Left, &Right) || Text.Split(TEXT("\n"), &Left, &Right))
		{
			return Right.TrimStartAndEnd();
		}
		return Text;
	}

	FLinearColor WithAlpha(FLinearColor C, float A)
	{
		C.A = A;
		return C;
	}

	// ---- drawing: gradients and polygons, as coloured triangles (the canvas' DrawRect is flat) ----

	void DrawTriangles(UCanvas* Canvas, const TArray<FCanvasUVTri>& Tris)
	{
		if (!Canvas || Tris.Num() == 0)
		{
			return;
		}
		FCanvasTriangleItem Item(Tris, GWhiteTexture);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	}

	FCanvasUVTri MakeTri(const FVector2D& P0, const FLinearColor& C0, const FVector2D& P1, const FLinearColor& C1, const FVector2D& P2, const FLinearColor& C2)
	{
		FCanvasUVTri T;
		T.V0_Pos = P0; T.V0_Color = C0; T.V0_UV = FVector2D::ZeroVector;
		T.V1_Pos = P1; T.V1_Color = C1; T.V1_UV = FVector2D::ZeroVector;
		T.V2_Pos = P2; T.V2_Color = C2; T.V2_UV = FVector2D::ZeroVector;
		return T;
	}

	/** A convex polygon, its colour going from Top (at y = Y0) to Bottom (at y = Y1). */
	void FillPolygon(UCanvas* Canvas, const TArray<FVector2D>& Points, float Y0, float Y1, const FLinearColor& Top, const FLinearColor& Bottom)
	{
		if (Points.Num() < 3)
		{
			return;
		}
		auto ColorAt = [&](const FVector2D& P) { return FMath::Lerp(Top, Bottom, Y1 > Y0 ? FMath::Clamp(float(P.Y - Y0) / (Y1 - Y0), 0.f, 1.f) : 0.f); };
		FVector2D Center = FVector2D::ZeroVector;
		for (const FVector2D& P : Points)
		{
			Center += P;
		}
		Center /= Points.Num();
		const FLinearColor CenterColor = ColorAt(Center);
		TArray<FCanvasUVTri> Tris;
		Tris.Reserve(Points.Num());
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			const FVector2D& A = Points[i];
			const FVector2D& B = Points[(i + 1) % Points.Num()];
			Tris.Add(MakeTri(Center, CenterColor, A, ColorAt(A), B, ColorAt(B)));
		}
		DrawTriangles(Canvas, Tris);
	}

	/** A rectangle with its top-left and bottom-right corners cut off at 45 degrees (Cut pixels). */
	TArray<FVector2D> Chamfer(const FBox2D& R, float Cut)
	{
		Cut = FMath::Clamp(Cut, 0.f, float(FMath::Min(R.GetSize().X, R.GetSize().Y)) * 0.5f);
		return {
			FVector2D(R.Min.X + Cut, R.Min.Y), FVector2D(R.Max.X, R.Min.Y), FVector2D(R.Max.X, R.Max.Y - Cut),
			FVector2D(R.Max.X - Cut, R.Max.Y), FVector2D(R.Min.X, R.Max.Y), FVector2D(R.Min.X, R.Min.Y + Cut) };
	}

	void FillChamfer(UCanvas* Canvas, const FBox2D& R, float Cut, const FLinearColor& Top, const FLinearColor& Bottom)
	{
		FillPolygon(Canvas, Chamfer(R, Cut), float(R.Min.Y), float(R.Max.Y), Top, Bottom);
	}

	/** Top to bottom. */
	void FillGradient(UCanvas* Canvas, const FBox2D& R, const FLinearColor& Top, const FLinearColor& Bottom)
	{
		FillChamfer(Canvas, R, 0.f, Top, Bottom);
	}

	/** Left to right. */
	void FillGradientH(UCanvas* Canvas, const FBox2D& R, const FLinearColor& Left, const FLinearColor& Right)
	{
		const FVector2D A = R.Min, B(R.Max.X, R.Min.Y), C = R.Max, D(R.Min.X, R.Max.Y);
		DrawTriangles(Canvas, { MakeTri(A, Left, B, Right, C, Right), MakeTri(A, Left, C, Right, D, Left) });
	}

	/** The outline of a polygon: a band Thick pixels wide just inside it (each edge a quad of its own). */
	void StrokePolygon(UCanvas* Canvas, const TArray<FVector2D>& Points, const FLinearColor& Color, float Thick)
	{
		if (Points.Num() < 2 || Color.A <= 0.f)
		{
			return;
		}
		// (Inside: towards the centre; for the convex shapes here that is enough.)
		FVector2D Center = FVector2D::ZeroVector;
		for (const FVector2D& P : Points)
		{
			Center += P;
		}
		Center /= Points.Num();
		TArray<FCanvasUVTri> Tris;
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			const FVector2D A = Points[i], B = Points[(i + 1) % Points.Num()];
			FVector2D N(-(B - A).Y, (B - A).X);
			N.Normalize();
			if (FVector2D::DotProduct(N, Center - (A + B) * 0.5) < 0.0)
			{
				N = -N;
			}
			const FVector2D A2 = A + N * Thick, B2 = B + N * Thick;
			Tris.Add(MakeTri(A, Color, B, Color, B2, Color));
			Tris.Add(MakeTri(A, Color, B2, Color, A2, Color));
		}
		DrawTriangles(Canvas, Tris);
	}

	TArray<FVector2D> Circle(const FVector2D& C, float Radius, int32 Sides = 20)
	{
		TArray<FVector2D> P;
		for (int32 i = 0; i < Sides; ++i)
		{
			const float A = 2.f * PI * i / Sides;
			P.Add(C + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Radius);
		}
		return P;
	}

	/** A soft shadow under a panel: a few widening layers of faint black, a little lower than it. */
	void DrawShadow(UCanvas* Canvas, const FBox2D& R, float Cut, float Scale, float Alpha)
	{
		for (int32 k = 3; k >= 1; --k)
		{
			const float Grow = k * 4.f * Scale;
			const FBox2D S(R.Min - FVector2D(Grow, Grow) + FVector2D(0.f, 5.f * Scale), R.Max + FVector2D(Grow, Grow) + FVector2D(0.f, 5.f * Scale));
			const FLinearColor C(0.f, 0.f, 0.f, 0.11f * Alpha);
			FillChamfer(Canvas, S, Cut + Grow * 0.5f, C, C);
		}
	}

	/** A button's face: blue (or gold, bPrimary), lit by T (the hover easing), darker while pressed. */
	void DrawButtonFace(UCanvas* Canvas, const FBox2D& R, float Scale, float T, bool bPressed, bool bEnabled, bool bPrimary)
	{
		const float H = float(R.GetSize().Y);
		const float Cut = FMath::Min(10.f * Scale, H * 0.3f);
		const float Thick = FMath::Max(1.f, 1.25f * Scale);
		if (!bEnabled)
		{
			FillChamfer(Canvas, R, Cut, FLinearColor(0.04f, 0.05f, 0.08f, 0.85f), FLinearColor(0.025f, 0.03f, 0.05f, 0.85f));
			StrokePolygon(Canvas, Chamfer(R, Cut), WithAlpha(ShellDim, 0.35f), Thick);
			return;
		}
		FLinearColor Top, Bottom, Edge;
		if (bPrimary)
		{
			Top = FMath::Lerp(FLinearColor(0.72f, 0.4f, 0.03f, 0.98f), FLinearColor(1.f, 0.6f, 0.1f, 1.f), T);
			Bottom = FMath::Lerp(FLinearColor(0.36f, 0.16f, 0.01f, 0.98f), FLinearColor(0.6f, 0.3f, 0.03f, 1.f), T);
			Edge = FMath::Lerp(FLinearColor(1.f, 0.78f, 0.3f, 0.8f), FLinearColor(1.f, 0.92f, 0.6f, 1.f), T);
		}
		else
		{
			Top = FMath::Lerp(FLinearColor(0.03f, 0.06f, 0.13f, 0.96f), FLinearColor(0.09f, 0.2f, 0.45f, 0.97f), T);
			Bottom = FMath::Lerp(FLinearColor(0.01f, 0.02f, 0.05f, 0.96f), FLinearColor(0.03f, 0.08f, 0.2f, 0.97f), T);
			Edge = FMath::Lerp(WithAlpha(ShellEdge, 0.7f), WithAlpha(ShellGold, 1.f), T);
		}
		if (bPressed)
		{
			Top = Top * FLinearColor(0.75f, 0.75f, 0.75f, 1.f);
			Bottom = Bottom * FLinearColor(0.75f, 0.75f, 0.75f, 1.f);
		}
		// (A glow round it as the mouse comes on.)
		if (T > 0.01f)
		{
			const FBox2D Glow(R.Min - FVector2D(3.f, 3.f) * Scale, R.Max + FVector2D(3.f, 3.f) * Scale);
			StrokePolygon(Canvas, Chamfer(Glow, Cut + 1.5f * Scale), WithAlpha(bPrimary ? ShellGold : ShellAccent, 0.18f * T), 3.f * Scale);
		}
		FillChamfer(Canvas, R, Cut, Top, Bottom);
		// the light along the top edge
		const FBox2D Sheen(FVector2D(R.Min.X + Cut, R.Min.Y), FVector2D(R.Max.X, R.Min.Y + H * 0.45f));
		FillGradient(Canvas, Sheen, FLinearColor(1.f, 1.f, 1.f, bPrimary ? 0.12f : 0.06f), FLinearColor(1.f, 1.f, 1.f, 0.f));
		StrokePolygon(Canvas, Chamfer(R, Cut), Edge, Thick);
		// the mark at the left that grows as the mouse comes on
		if (!bPrimary)
		{
			const float BarH = (H - Cut - 2.f * Thick) * (0.35f + 0.65f * T);
			const float BarY = float(R.Max.Y) - Thick - BarH;
			FillGradient(Canvas, FBox2D(FVector2D(R.Min.X + Thick, BarY), FVector2D(R.Min.X + Thick + 3.f * Scale, BarY + BarH)),
				WithAlpha(ShellGold, 0.3f + 0.7f * T), WithAlpha(ShellGold, 0.3f + 0.7f * T));
		}
	}

	/** Where the open list goes: under its button, or over it when there is no room below. */
	FBox2D OpenListRect(const FBox2D& Button, int32 NumItems, float ViewHeight)
	{
		const float ItemHeight = float(Button.GetSize().Y) * 0.86f;
		const float Height = ItemHeight * NumItems;
		const bool bBelow = Button.Max.Y + Height <= ViewHeight - 4.f || Button.Min.Y - Height < 4.f;
		const float Top = bBelow ? float(Button.Max.Y) : float(Button.Min.Y) - Height;
		return FBox2D(FVector2D(Button.Min.X, Top), FVector2D(Button.Max.X, Top + Height));
	}

	/** A mapped image and its texture, if both are there. */
	UTexture2D* FindShellTexture(const FString& Image, const FGenMappedImage*& OutImage)
	{
		OutImage = Image.IsEmpty() ? nullptr : FGenCommandData::Get().FindImage(Image);
		return OutImage ? UGenAssetSubsystem::Get()->GetTexture(OutImage->Texture) : nullptr;
	}
}

// ---- the elements ---------------------------------------------------------------------------------

FVector2D AGenHUD::ShellMeasure(const FString& Text, float Size, bool bBold) const
{
	return MeasureShellText(Text, ShellFont(Size * Shell.Scale, bBold));
}

void AGenHUD::ShellTestClick(const FString& Id)
{
	Shell.TestClick = Id;
	Shell.TestClickFrames = 0;
}

FBox2D AGenHUD::ShellRect(float X, float Y, float W, float H) const
{
	return FBox2D(Shell.Origin + FVector2D(X, Y) * Shell.Scale, Shell.Origin + FVector2D(X + W, Y + H) * Shell.Scale);
}

bool AGenHUD::ShellHover(const FBox2D& R) const
{
	return !Shell.bBlocked && !Shell.bUnderOptions && Shell.OpenId.IsEmpty() && R.IsInside(Shell.Mouse);
}

bool AGenHUD::ShellTestHit(const FString& Id)
{
	if (Shell.bBlocked || Shell.bUnderOptions || Shell.TestClick.IsEmpty() || !Shell.TestClick.Equals(Id, ESearchCase::IgnoreCase))
	{
		return false;
	}
	Shell.TestClick.Reset();
	return true;
}

float AGenHUD::ShellAnim(const FString& Id, bool bOn, float Speed)
{
	// Eases towards where it is going by a share of the way each frame: quick at first, gently at the end.
	float& A = Shell.Anim.FindOrAdd(Id);
	const float Target = bOn ? 1.f : 0.f;
	A += (Target - A) * (1.f - FMath::Exp(-Shell.DeltaTime * Speed));
	if (FMath::Abs(Target - A) < 0.002f)
	{
		A = Target;
	}
	return A;
}

void AGenHUD::ShellClickSound()
{
	if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
	{
		Audio->PlayUI(FGenAudioData::Get().GetMisc(TEXT("GUIClickSound")));
	}
}

bool AGenHUD::ShellImage(const FString& Image, const FBox2D& R, const FLinearColor& Tint)
{
	const FGenMappedImage* Img = nullptr;
	UTexture2D* Tex = FindShellTexture(Image, Img);
	if (!Tex)
	{
		return false;
	}
	DrawTexture(Tex, R.Min.X, R.Min.Y, R.GetSize().X, R.GetSize().Y, Img->UV0.X, Img->UV0.Y, Img->UV1.X - Img->UV0.X, Img->UV1.Y - Img->UV0.Y, Tint);
	return true;
}

void AGenHUD::ShellBackdrop(const TCHAR* Image, float Darken)
{
	// The picture fills the screen (cut at the sides rather than squeezed); without one, plain dark.
	const FVector2D View(Canvas->SizeX, Canvas->SizeY);
	const FGenMappedImage* Img = nullptr;
	UTexture2D* Tex = FindShellTexture(Image, Img);
	static bool bReported = false;
	if (!bReported && (!Img || !Tex))
	{
		bReported = true;
		UE_LOG(LogGenerals, Warning, TEXT("Shell: no backdrop '%s' (image %s, texture '%s' %s)"), Image, Img ? TEXT("found") : TEXT("unknown"),
			Img ? *Img->Texture : TEXT(""), Tex ? TEXT("loaded") : TEXT("not loaded"));
	}
	if (Tex && Img->Size.X > 0.f && Img->Size.Y > 0.f)
	{
		const float Fit = FMath::Max(View.X / Img->Size.X, View.Y / Img->Size.Y);
		const FVector2D Size(Img->Size.X * Fit, Img->Size.Y * Fit);
		DrawRect(FLinearColor::Black, 0.f, 0.f, View.X, View.Y);
		DrawTexture(Tex, (View.X - Size.X) * 0.5f, (View.Y - Size.Y) * 0.5f, Size.X, Size.Y, Img->UV0.X, Img->UV0.Y, Img->UV1.X - Img->UV0.X, Img->UV1.Y - Img->UV0.Y,
			FLinearColor(1.f - Darken, 1.f - Darken, 1.f - Darken));
	}
	else
	{
		FillGradient(Canvas, FBox2D(FVector2D::ZeroVector, View), FLinearColor(0.03f, 0.05f, 0.1f), FLinearColor(0.005f, 0.01f, 0.02f));
	}
	// A vignette: the edges darker, the eye to the middle.
	const float Band = View.Y * 0.22f;
	FillGradient(Canvas, FBox2D(FVector2D::ZeroVector, FVector2D(View.X, Band)), FLinearColor(0.f, 0.f, 0.f, 0.45f), FLinearColor(0.f, 0.f, 0.f, 0.f));
	FillGradient(Canvas, FBox2D(FVector2D(0.f, View.Y - Band), View), FLinearColor(0.f, 0.f, 0.f, 0.f), FLinearColor(0.f, 0.f, 0.f, 0.55f));
}

void AGenHUD::ShellPanel(const FBox2D& R, float Alpha)
{
	const float S = Shell.Scale;
	const float Cut = FMath::Min(16.f * S, float(FMath::Min(R.GetSize().X, R.GetSize().Y)) * 0.2f);
	const float Thick = FMath::Max(1.f, S);
	DrawShadow(Canvas, R, Cut, S, Alpha);
	FillChamfer(Canvas, R, Cut, WithAlpha(ShellPanelTop, Alpha), WithAlpha(ShellPanelBottom, Alpha));
	// the light along the top, the border, and a touch of gold at the cut corners
	FillGradient(Canvas, FBox2D(FVector2D(R.Min.X + Cut, R.Min.Y), FVector2D(R.Max.X, R.Min.Y + FMath::Min(60.f * S, float(R.GetSize().Y) * 0.3f))),
		FLinearColor(0.5f, 0.7f, 1.f, 0.05f), FLinearColor(0.5f, 0.7f, 1.f, 0.f));
	StrokePolygon(Canvas, Chamfer(R, Cut), WithAlpha(ShellEdge, 0.5f), Thick);
	const float Accent = FMath::Min(90.f * S, float(R.GetSize().X) * 0.25f);
	DrawRect(ShellGold, R.Min.X + Cut, R.Min.Y, Accent, 2.f * Thick);
	DrawRect(ShellGold, R.Max.X - Cut - Accent * 0.5f, R.Max.Y - 2.f * Thick, Accent * 0.5f, 2.f * Thick);
	DrawLine(R.Min.X, R.Min.Y + Cut, R.Min.X + Cut, R.Min.Y, WithAlpha(ShellGold, 0.9f), 2.f * Thick);
	DrawLine(R.Max.X - Cut, R.Max.Y, R.Max.X, R.Max.Y - Cut, WithAlpha(ShellGold, 0.9f), 2.f * Thick);
}

void AGenHUD::ShellText(const FString& Text, float X, float Y, const FLinearColor& Color, float Size, int32 Align, bool bBold)
{
	ShellTextEx(Text, X, Y, Color, Size, Align, bBold, 0, true);
}

void AGenHUD::ShellTextEx(const FString& Text, float X, float Y, const FLinearColor& Color, float Size, int32 Align, bool bBold, int32 LetterSpacing, bool bShadow)
{
	if (Text.IsEmpty())
	{
		return;
	}
	const FSlateFontInfo Font = ShellFont(Size * Shell.Scale, bBold, LetterSpacing);
	if (!Font.FontObject)
	{
		// (No such font: the plain canvas one, whatever it looks like at this size.)
		DrawText(Text, Color, X, Y, nullptr, Size * Shell.Scale / 14.f);
		return;
	}
	if (Align != 0)
	{
		const float Width = float(MeasureShellText(Text, Font).X);
		X -= Align == 1 ? Width * 0.5f : Width;
	}
	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(X), FMath::RoundToFloat(Y)), FText::FromString(Text), Font, Color);
	if (bShadow)
	{
		Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, Color.A * 0.85f), FVector2D(1.5, 1.5));
	}
	Canvas->DrawItem(Item);
}

bool AGenHUD::ShellButton(const FString& Id, const FBox2D& R, const FString& Label, bool bEnabled)
{
	const bool bHover = bEnabled && ShellHover(R);
	const bool bPressed = bHover && Shell.bMouseDown;
	const float T = ShellAnim(Id, bHover);
	DrawButtonFace(Canvas, R, Shell.Scale, T, bPressed, bEnabled, false);
	const FVector2D Size = R.GetSize();
	const float TextSize = FMath::Clamp(float(Size.Y) / Shell.Scale * 0.36f, 13.f, 24.f);
	const int32 Spacing = 70;
	const float TextHeight = float(MeasureShellText(Label, ShellFont(TextSize * Shell.Scale, true, Spacing)).Y);
	const FLinearColor TextColor = bEnabled ? FMath::Lerp(ShellTextColor, FLinearColor::White, T) : WithAlpha(ShellDim, 0.8f);
	ShellTextEx(Label, R.Min.X + Size.X * 0.5f, R.Min.Y + (Size.Y - TextHeight) * 0.5f + (bPressed ? Shell.Scale : 0.f), TextColor, TextSize, 1, true, Spacing);
	const bool bClicked = bEnabled && ((bHover && Shell.bClick) || ShellTestHit(Id));
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

bool AGenHUD::ShellPrimaryButton(const FString& Id, const FBox2D& R, const FString& Label, bool bEnabled)
{
	const bool bHover = bEnabled && ShellHover(R);
	const bool bPressed = bHover && Shell.bMouseDown;
	const float T = ShellAnim(Id, bHover);
	DrawButtonFace(Canvas, R, Shell.Scale, T, bPressed, bEnabled, true);
	const FVector2D Size = R.GetSize();
	const float TextSize = FMath::Clamp(float(Size.Y) / Shell.Scale * 0.36f, 13.f, 24.f);
	const int32 Spacing = 90;
	const float TextHeight = float(MeasureShellText(Label, ShellFont(TextSize * Shell.Scale, true, Spacing)).Y);
	const FLinearColor TextColor = bEnabled ? FMath::Lerp(FLinearColor(0.07f, 0.04f, 0.f), FLinearColor(0.02f, 0.01f, 0.f), T) : WithAlpha(ShellDim, 0.8f);
	ShellTextEx(Label, R.Min.X + Size.X * 0.5f, R.Min.Y + (Size.Y - TextHeight) * 0.5f + (bPressed ? Shell.Scale : 0.f), TextColor, TextSize, 1, true, Spacing, false);
	const bool bClicked = bEnabled && ((bHover && Shell.bClick) || ShellTestHit(Id));
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

bool AGenHUD::ShellCheckbox(const FString& Id, const FBox2D& R, const FString& Label, bool bValue)
{
	const bool bHover = ShellHover(R);
	const float T = ShellAnim(Id, bHover);
	const float On = ShellAnim(Id + TEXT("#on"), bValue, 18.f);
	const float Box = R.GetSize().Y;
	const float S = Shell.Scale;
	const FBox2D BoxR(R.Min, R.Min + FVector2D(Box, Box));
	FillChamfer(Canvas, BoxR, 5.f * S, FLinearColor(0.02f, 0.04f, 0.09f, 0.95f), FLinearColor(0.04f, 0.08f, 0.16f, 0.95f));
	StrokePolygon(Canvas, Chamfer(BoxR, 5.f * S), FMath::Lerp(WithAlpha(ShellEdge, 0.7f), ShellGold, FMath::Max(T, On * 0.6f)), FMath::Max(1.f, 1.25f * S));
	if (On > 0.01f)
	{
		// The mark grows from the middle.
		const float Inner = Box * 0.52f * On;
		const FVector2D C = BoxR.GetCenter();
		FillChamfer(Canvas, FBox2D(C - FVector2D(Inner, Inner) * 0.5, C + FVector2D(Inner, Inner) * 0.5), 3.f * S, WithAlpha(FLinearColor(1.f, 0.88f, 0.45f), On), WithAlpha(ShellGold * 0.8f, On));
	}
	const float TextSize = 18.f;
	const float TextHeight = float(MeasureShellText(Label, ShellFont(TextSize * Shell.Scale, false)).Y);
	ShellText(Label, R.Min.X + Box * 1.4f, R.Min.Y + (Box - TextHeight) * 0.5f, FMath::Lerp(ShellTextColor, FLinearColor::White, T), TextSize);
	const bool bClicked = (bHover && Shell.bClick) || ShellTestHit(Id);
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

bool AGenHUD::ShellDropdown(const FString& Id, const FBox2D& R, const TArray<FString>& Items, const TArray<FLinearColor>& Colors, int32 Current, int32& OutPicked)
{
	// What was chosen in the open list (BeginShell saw the click) is collected here.
	if (!Shell.PickedId.IsEmpty() && Shell.PickedId == Id)
	{
		const int32 Picked = Shell.PickedIndex;
		Shell.PickedId.Reset();
		if (Items.IsValidIndex(Picked) && Picked != Current)
		{
			OutPicked = Picked;
			return true;
		}
	}
	// Test automation: "Id=Item label" chooses without opening the list.
	if (!Shell.bBlocked && !Shell.bUnderOptions && Shell.TestClick.StartsWith(Id + TEXT("="), ESearchCase::IgnoreCase))
	{
		const FString Wanted = Shell.TestClick.Mid(Id.Len() + 1);
		Shell.TestClick.Reset();
		Shell.OpenId.Reset();
		const int32 Picked = Items.IndexOfByPredicate([&Wanted](const FString& Item) { return Item.Equals(Wanted, ESearchCase::IgnoreCase); });
		if (Picked == INDEX_NONE)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Test menu: '%s' has no item '%s'"), *Id, *Wanted);
		}
		else if (Picked != Current)
		{
			OutPicked = Picked;
			return true;
		}
	}

	const bool bHover = ShellHover(R);
	const bool bOpen = Shell.OpenId == Id;
	const float T = ShellAnim(Id, bHover || bOpen);
	const FVector2D Size = R.GetSize();
	const float S = Shell.Scale;
	const float Cut = FMath::Min(7.f * S, float(Size.Y) * 0.25f);
	FillChamfer(Canvas, R, Cut, FMath::Lerp(FLinearColor(0.016f, 0.032f, 0.075f, 0.95f), FLinearColor(0.08f, 0.17f, 0.36f, 0.96f), T),
		FMath::Lerp(FLinearColor(0.006f, 0.013f, 0.035f, 0.95f), FLinearColor(0.04f, 0.09f, 0.2f, 0.96f), T));
	StrokePolygon(Canvas, Chamfer(R, Cut), FMath::Lerp(WithAlpha(ShellEdge, 0.45f), WithAlpha(ShellGold, 0.95f), T), FMath::Max(1.f, S));
	const float TextSize = 18.f;
	const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(TextSize * Shell.Scale, false)).Y);
	if (Items.IsValidIndex(Current))
	{
		ShellText(Items[Current], R.Min.X + 12.f * S, R.Min.Y + (Size.Y - TextHeight) * 0.5f, Colors.IsValidIndex(Current) ? Colors[Current] : ShellTextColor, TextSize);
	}
	// The mark that says "there is a list behind this": a chevron, turned up while the list is open.
	const FLinearColor Mark = FMath::Lerp(WithAlpha(ShellAccent, 0.8f), ShellGold, T);
	const float AX = R.Max.X - 18.f * S, AY = R.Min.Y + Size.Y * 0.5f, AS = 5.f * S;
	const float Dir = bOpen ? -1.f : 1.f;
	DrawLine(AX - AS, AY - AS * 0.5f * Dir, AX, AY + AS * 0.5f * Dir, Mark, 2.f * S);
	DrawLine(AX, AY + AS * 0.5f * Dir, AX + AS, AY - AS * 0.5f * Dir, Mark, 2.f * S);

	if ((bHover && Shell.bClick) || ShellTestHit(Id))
	{
		Shell.bClick = false;
		Shell.OpenId = Id;
		Shell.OpenRect = R;
		Shell.OpenItems = Items;
		Shell.OpenColors = Colors;
		Shell.OpenCurrent = Current;
		ShellClickSound();
	}
	return false;
}

int32 AGenHUD::ShellTabs(const FString& Id, const FBox2D& R, const TArray<FString>& Labels, int32 Current)
{
	if (Labels.Num() == 0)
	{
		return Current;
	}
	int32 Chosen = Current;
	if (!Shell.bBlocked && !Shell.bUnderOptions && Shell.TestClick.StartsWith(Id + TEXT("="), ESearchCase::IgnoreCase))
	{
		const FString Wanted = Shell.TestClick.Mid(Id.Len() + 1);
		Shell.TestClick.Reset();
		const int32 Found = Labels.IndexOfByPredicate([&Wanted](const FString& L) { return L.Equals(Wanted, ESearchCase::IgnoreCase); });
		if (Found == INDEX_NONE)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Test menu: '%s' has no tab '%s'"), *Id, *Wanted);
		}
		else
		{
			Chosen = Found;
		}
	}
	const float S = Shell.Scale;
	const float W = float(R.GetSize().X) / Labels.Num();
	const float H = float(R.GetSize().Y);
	const float T = FMath::Max(1.f, S);
	// One strip under them all; the chosen tab lights up from below, with its gold line.
	FillGradient(Canvas, R, FLinearColor(0.02f, 0.04f, 0.09f, 0.55f), FLinearColor(0.02f, 0.04f, 0.09f, 0.85f));
	DrawRect(ShellLine, R.Min.X, R.Max.Y - T, R.GetSize().X, T);
	for (int32 i = 0; i < Labels.Num(); ++i)
	{
		const FBox2D Tab(FVector2D(R.Min.X + W * i, R.Min.Y), FVector2D(R.Min.X + W * (i + 1), R.Max.Y));
		const bool bHover = ShellHover(Tab);
		const float On = ShellAnim(FString::Printf(TEXT("%s#%d"), *Id, i), i == Chosen, 16.f);
		const float Hot = ShellAnim(FString::Printf(TEXT("%s#h%d"), *Id, i), bHover && i != Chosen);
		if (On > 0.01f || Hot > 0.01f)
		{
			FillGradient(Canvas, Tab, FLinearColor(0.1f, 0.22f, 0.45f, 0.f), FLinearColor(0.1f, 0.22f, 0.45f, 0.75f * On + 0.35f * Hot));
		}
		if (On > 0.01f)
		{
			const float LineW = float(Tab.GetSize().X) * On;
			DrawRect(ShellGold, Tab.GetCenter().X - LineW * 0.5f, Tab.Max.Y - 3.f * T, LineW, 3.f * T);
		}
		if (i > 0)
		{
			DrawRect(ShellLine, Tab.Min.X, Tab.Min.Y + H * 0.25f, T, H * 0.5f);
		}
		const float TextSize = FMath::Clamp(H / S * 0.36f, 13.f, 22.f);
		const int32 Spacing = 100;
		const FString Label = Labels[i].ToUpper();
		const float TextHeight = float(MeasureShellText(Label, ShellFont(TextSize * S, true, Spacing)).Y);
		const FLinearColor Color = FMath::Lerp(FMath::Lerp(ShellDim * 1.4f, FLinearColor::White, Hot), ShellGold, On);
		ShellTextEx(Label, Tab.Min.X + Tab.GetSize().X * 0.5f, Tab.Min.Y + (H - TextHeight) * 0.5f, WithAlpha(Color, 1.f), TextSize, 1, true, Spacing);
		if (bHover && Shell.bClick && i != Chosen)
		{
			Shell.bClick = false;
			ShellClickSound();
			Chosen = i;
		}
	}
	return Chosen;
}

bool AGenHUD::ShellSlider(const FString& Id, const FBox2D& R, float& Value, float Min, float Max, float Step, const FString& ValueText)
{
	const float Old = Value;
	auto Settle = [Min, Max, Step](float V)
	{
		V = FMath::Clamp(V, Min, Max);
		return Step > 0.f ? FMath::Clamp(Min + FMath::RoundToFloat((V - Min) / Step) * Step, Min, Max) : V;
	};
	// Test automation: "Id=0.75".
	if (!Shell.bBlocked && !Shell.bUnderOptions && Shell.TestClick.StartsWith(Id + TEXT("="), ESearchCase::IgnoreCase))
	{
		Value = Settle(FCString::Atof(*Shell.TestClick.Mid(Id.Len() + 1)));
		Shell.TestClick.Reset();
	}
	// The bar takes the left part; the value's text the rest.
	const float S = Shell.Scale;
	const float TextW = 90.f * S;
	const FBox2D Bar(R.Min, FVector2D(R.Max.X - TextW, R.Max.Y));
	const float H = float(R.GetSize().Y);
	const float TrackH = FMath::Max(3.f, H * 0.12f);
	const float KnobR = FMath::Max(5.f, H * 0.26f);
	const float X0 = float(Bar.Min.X) + KnobR, X1 = float(Bar.Max.X) - KnobR;
	const bool bHover = ShellHover(Bar);
	if (bHover && Shell.bClick)
	{
		Shell.bClick = false;
		Shell.DragId = Id;
	}
	const bool bDragging = Shell.DragId == Id && Shell.bMouseDown && !Shell.bBlocked && !Shell.bUnderOptions;
	if (bDragging && X1 > X0)
	{
		Value = Settle(Min + (Max - Min) * FMath::Clamp((float(Shell.Mouse.X) - X0) / (X1 - X0), 0.f, 1.f));
	}
	const float T = ShellAnim(Id, bHover || bDragging);
	const float Frac = Max > Min ? FMath::Clamp((Value - Min) / (Max - Min), 0.f, 1.f) : 0.f;
	const float CY = float(R.Min.Y) + H * 0.5f;
	const float KX = X0 + (X1 - X0) * Frac;
	// the track, the part up to the value in gold, the knob
	FillGradient(Canvas, FBox2D(FVector2D(X0, CY - TrackH * 0.5f), FVector2D(X1, CY + TrackH * 0.5f)), FLinearColor(0.01f, 0.02f, 0.05f, 0.95f), FLinearColor(0.06f, 0.1f, 0.2f, 0.95f));
	StrokePolygon(Canvas, Chamfer(FBox2D(FVector2D(X0, CY - TrackH * 0.5f), FVector2D(X1, CY + TrackH * 0.5f)), 0.f), WithAlpha(ShellEdge, 0.35f), FMath::Max(1.f, S * 0.75f));
	if (KX > X0)
	{
		FillGradientH(Canvas, FBox2D(FVector2D(X0, CY - TrackH * 0.5f), FVector2D(KX, CY + TrackH * 0.5f)), FLinearColor(0.55f, 0.35f, 0.06f, 1.f), FMath::Lerp(ShellGold * 0.9f, ShellGold, T));
	}
	const float Knob = KnobR * (1.f + 0.15f * T);
	FillPolygon(Canvas, Circle(FVector2D(KX, CY + 2.f * S), Knob + 2.f * S), CY - Knob, CY + Knob, FLinearColor(0.f, 0.f, 0.f, 0.35f), FLinearColor(0.f, 0.f, 0.f, 0.35f));
	FillPolygon(Canvas, Circle(FVector2D(KX, CY), Knob), CY - Knob, CY + Knob, FMath::Lerp(FLinearColor(0.85f, 0.88f, 0.95f), FLinearColor(1.f, 0.9f, 0.55f), T), FMath::Lerp(FLinearColor(0.5f, 0.55f, 0.65f), ShellGold * 0.85f, T));
	FillPolygon(Canvas, Circle(FVector2D(KX, CY), Knob * 0.38f), CY - Knob, CY + Knob, FLinearColor(0.05f, 0.08f, 0.15f, 0.9f), FLinearColor(0.05f, 0.08f, 0.15f, 0.9f));
	const float TextSize = 17.f;
	const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(TextSize * Shell.Scale, true)).Y);
	ShellText(ValueText, R.Max.X, CY - TextHeight * 0.5f, FMath::Lerp(ShellTextColor, ShellGold, T), TextSize, 2, true);
	return Value != Old;
}

bool AGenHUD::ShellKeyButton(const FString& Id, const FBox2D& R, const FString& Text, bool bWaiting)
{
	const bool bHover = ShellHover(R);
	const float T = ShellAnim(Id, bHover || bWaiting);
	const FVector2D Size = R.GetSize();
	const float S = Shell.Scale;
	const float Cut = FMath::Min(7.f * S, float(Size.Y) * 0.25f);
	FillChamfer(Canvas, R, Cut, FMath::Lerp(FLinearColor(0.016f, 0.032f, 0.075f, 0.95f), FLinearColor(0.09f, 0.19f, 0.4f, 0.96f), T),
		FMath::Lerp(FLinearColor(0.006f, 0.013f, 0.035f, 0.95f), FLinearColor(0.04f, 0.09f, 0.2f, 0.96f), T));
	// (Waiting for a key: the border breathes.)
	const float Pulse = bWaiting ? 0.55f + 0.45f * FMath::Sin(float(FPlatformTime::Seconds()) * 6.f) : 1.f;
	StrokePolygon(Canvas, Chamfer(R, Cut), FMath::Lerp(WithAlpha(ShellEdge, 0.45f), WithAlpha(ShellGold, Pulse), T), FMath::Max(1.f, (bWaiting ? 2.f : 1.f) * S));
	const FString Shown = bWaiting ? FString(TEXT("Press a key...")) : Text;
	const float TextSize = 17.f;
	const float TextHeight = float(MeasureShellText(Shown, ShellFont(TextSize * Shell.Scale, true)).Y);
	ShellText(Shown, R.Min.X + Size.X * 0.5f, R.Min.Y + (Size.Y - TextHeight) * 0.5f, bWaiting ? ShellGold : FMath::Lerp(ShellTextColor, FLinearColor::White, T), TextSize, 1, true);
	const bool bClicked = (bHover && Shell.bClick) || ShellTestHit(Id);
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

bool AGenHUD::ShellCaptureKey(FGenKeyChord& Out, bool& bOutCancelled)
{
	bOutCancelled = false;
	// Test automation: "Key=Ctrl+F", "Key=Escape".
	if (Shell.TestClick.StartsWith(TEXT("Key="), ESearchCase::IgnoreCase))
	{
		const FString Wanted = Shell.TestClick.Mid(4);
		Shell.TestClick.Reset();
		if (Wanted.Equals(TEXT("Escape"), ESearchCase::IgnoreCase))
		{
			bOutCancelled = true;
			return false;
		}
		if (FGenKeyChord::Parse(Wanted, Out) && Out.IsValid())
		{
			return true;
		}
		UE_LOG(LogGenerals, Warning, TEXT("Test menu: no key '%s'"), *Wanted);
		return false;
	}
	const APlayerController* PC = GetOwningPlayerController();
	const AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
	if (!PC || !GenPC || !GenPC->IsGenInputEnabled())
	{
		return false;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		bOutCancelled = true;
		return false;
	}
	static TArray<FKey> AllKeys;
	if (AllKeys.Num() == 0)
	{
		EKeys::GetAllKeys(AllKeys);
	}
	for (const FKey& Key : AllKeys)
	{
		if (Key.IsModifierKey() || Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D() || Key.IsGamepadKey() || Key.IsTouch()
			|| Key == EKeys::LeftMouseButton || Key == EKeys::RightMouseButton || Key == EKeys::AnyKey)
		{
			continue;
		}
		if (PC->WasInputKeyJustPressed(Key))
		{
			Out = FGenKeyChord(Key, FGenInput::CtrlDown(PC), FGenInput::ShiftDown(PC), FGenInput::AltDown(PC));
			return true;
		}
	}
	return false;
}

void AGenHUD::ShellScrollArea(const FString& Id, const FBox2D& R, float ContentHeight, float& Scroll)
{
	const float ViewH = float(R.GetSize().Y) / Shell.Scale;
	const float MaxScroll = FMath::Max(0.f, ContentHeight - ViewH);
	if (!Shell.bBlocked && !Shell.bUnderOptions && Shell.TestClick.StartsWith(Id + TEXT("="), ESearchCase::IgnoreCase))
	{
		Scroll = Shell.TestClick.Mid(Id.Len() + 1).Equals(TEXT("Bottom"), ESearchCase::IgnoreCase) ? MaxScroll : 0.f;
		Shell.TestClick.Reset();
	}
	if (MaxScroll <= 0.f)
	{
		Scroll = 0.f;
		return;
	}
	const bool bInside = !Shell.bBlocked && !Shell.bUnderOptions && Shell.OpenId.IsEmpty() && R.IsInside(Shell.Mouse);
	if (bInside && Shell.Wheel != 0.f)
	{
		Scroll -= Shell.Wheel * 60.f;
		Shell.Wheel = 0.f;
	}
	// The bar at the right edge, inside the area.
	const float BarW = 8.f * Shell.Scale;
	const FBox2D Track(FVector2D(R.Max.X - BarW, R.Min.Y), R.Max);
	const float TrackH = float(Track.GetSize().Y);
	const float ThumbH = FMath::Max(24.f * Shell.Scale, TrackH * ViewH / ContentHeight);
	const bool bHover = ShellHover(Track);
	if (bHover && Shell.bClick)
	{
		Shell.bClick = false;
		Shell.DragId = Id;
	}
	if (Shell.DragId == Id && Shell.bMouseDown && TrackH > ThumbH)
	{
		Scroll = MaxScroll * FMath::Clamp((float(Shell.Mouse.Y - Track.Min.Y) - ThumbH * 0.5f) / (TrackH - ThumbH), 0.f, 1.f);
	}
	Scroll = FMath::Clamp(Scroll, 0.f, MaxScroll);
	const float T = ShellAnim(Id + TEXT("#bar"), bHover || Shell.DragId == Id);
	const float ThumbY = float(Track.Min.Y) + (TrackH - ThumbH) * (Scroll / MaxScroll);
	const float Inset = BarW * 0.25f;
	DrawRect(FLinearColor(0.01f, 0.02f, 0.05f, 0.7f), Track.Min.X + Inset, Track.Min.Y, BarW - Inset * 2.f, TrackH);
	FillGradientH(Canvas, FBox2D(FVector2D(Track.Min.X, ThumbY), FVector2D(Track.Max.X, ThumbY + ThumbH)), FMath::Lerp(WithAlpha(ShellEdge, 0.8f), ShellGold, T),
		FMath::Lerp(WithAlpha(ShellEdge * 0.7f, 0.8f), ShellGold * 0.8f, T));
}

void AGenHUD::ShellTooltip(const FBox2D& R, const FString& Text)
{
	if (!Text.IsEmpty() && ShellHover(R))
	{
		Shell.Tooltip = Text;
	}
}

void AGenHUD::BeginShell(AGenPlayerController* PC)
{
	Shell.Scale = FMath::Max(0.25f, FMath::Min(Canvas->SizeX / DesignWidth, Canvas->SizeY / DesignHeight));
	Shell.Origin = FVector2D((Canvas->SizeX - DesignWidth * Shell.Scale) * 0.5f, (Canvas->SizeY - DesignHeight * Shell.Scale) * 0.5f);
	Shell.bBlocked = false;
	Shell.bUnderOptions = Options.bOpen; // (DrawOptions clears it for itself)
	Shell.Mouse = FVector2D(-10000.0, -10000.0);
	Shell.bClick = Shell.bRightClick = Shell.bMouseDown = false;
	Shell.Wheel = 0.f;
	Shell.Tooltip.Reset();
	// The easing runs by real time: the same with the game standing still behind the menu.
	const double Now = FPlatformTime::Seconds();
	Shell.DeltaTime = Shell.LastTime > 0.0 ? FMath::Clamp(float(Now - Shell.LastTime), 0.f, 0.1f) : 0.f;
	Shell.LastTime = Now;
	float MX = 0.f, MY = 0.f;
	if (PC && PC->IsGenInputEnabled())
	{
		if (PC->GetMousePosition(MX, MY))
		{
			Shell.Mouse = FVector2D(MX, MY);
		}
		Shell.bClick = PC->WasInputKeyJustPressed(EKeys::LeftMouseButton);
		Shell.bRightClick = PC->WasInputKeyJustPressed(EKeys::RightMouseButton);
		Shell.bMouseDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
		Shell.Wheel = PC->GetInputAnalogKeyState(EKeys::MouseWheelAxis);
	}
	if (!Shell.bMouseDown)
	{
		Shell.DragId.Reset();
	}
	// A click while a list is open is the list's: on an item it chooses that, anywhere else it closes the list.
	if (!Shell.OpenId.IsEmpty() && (Shell.bClick || Shell.bRightClick))
	{
		const FBox2D List = OpenListRect(Shell.OpenRect, Shell.OpenItems.Num(), Canvas->SizeY);
		if (Shell.bClick && List.IsInside(Shell.Mouse) && Shell.OpenItems.Num() > 0)
		{
			Shell.PickedId = Shell.OpenId;
			Shell.PickedIndex = FMath::Clamp(int32((Shell.Mouse.Y - List.Min.Y) / (List.GetSize().Y / Shell.OpenItems.Num())), 0, Shell.OpenItems.Num() - 1);
			ShellClickSound();
		}
		Shell.OpenId.Reset();
		Shell.bClick = Shell.bRightClick = false;
	}
}

void AGenHUD::EndShell()
{
	const float S = Shell.Scale;
	if (!Shell.OpenId.IsEmpty() && Shell.OpenItems.Num() > 0)
	{
		const FBox2D List = OpenListRect(Shell.OpenRect, Shell.OpenItems.Num(), Canvas->SizeY);
		const float ItemHeight = float(List.GetSize().Y) / Shell.OpenItems.Num();
		const float Width = float(List.GetSize().X);
		DrawShadow(Canvas, List, 0.f, S, 1.f);
		FillGradient(Canvas, List, FLinearColor(0.025f, 0.045f, 0.1f, 0.99f), FLinearColor(0.012f, 0.022f, 0.05f, 0.99f));
		const float TextSize = 17.f;
		const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(TextSize * Shell.Scale, false)).Y);
		for (int32 i = 0; i < Shell.OpenItems.Num(); ++i)
		{
			const float Y = float(List.Min.Y) + ItemHeight * i;
			const bool bHover = Shell.Mouse.X >= List.Min.X && Shell.Mouse.X < List.Max.X && Shell.Mouse.Y >= Y && Shell.Mouse.Y < Y + ItemHeight;
			const FBox2D Row(FVector2D(List.Min.X, Y), FVector2D(List.Max.X, Y + ItemHeight));
			if (bHover)
			{
				FillGradientH(Canvas, Row, FLinearColor(0.14f, 0.3f, 0.6f, 0.95f), FLinearColor(0.08f, 0.17f, 0.36f, 0.9f));
				DrawRect(ShellGold, Row.Min.X, Row.Min.Y, 3.f * S, ItemHeight);
			}
			else if (i == Shell.OpenCurrent)
			{
				DrawRect(FLinearColor(0.07f, 0.13f, 0.27f, 0.9f), Row.Min.X, Row.Min.Y, Width, ItemHeight);
				DrawRect(WithAlpha(ShellGold, 0.5f), Row.Min.X, Row.Min.Y, 2.f * S, ItemHeight);
			}
			ShellText(Shell.OpenItems[i], List.Min.X + 14.f * S, Y + (ItemHeight - TextHeight) * 0.5f,
				Shell.OpenColors.IsValidIndex(i) ? Shell.OpenColors[i] : (bHover ? FLinearColor::White : ShellTextColor), TextSize);
		}
		StrokePolygon(Canvas, Chamfer(List, 0.f), WithAlpha(ShellGold, 0.8f), FMath::Max(1.f, S));
	}
	if (!Shell.Tooltip.IsEmpty())
	{
		const float TextSize = 15.f;
		const FVector2D Size = MeasureShellText(Shell.Tooltip, ShellFont(TextSize * Shell.Scale, false));
		const float Pad = 10.f * S;
		FVector2D At = Shell.Mouse + FVector2D(18.f, 22.f) * S;
		At.X = FMath::Min(At.X, Canvas->SizeX - Size.X - Pad * 2.f - 2.f);
		At.Y = FMath::Min(At.Y, Canvas->SizeY - Size.Y - Pad * 2.f - 2.f);
		const FBox2D Box(At, At + Size + FVector2D(Pad * 2.f, Pad * 2.f));
		DrawShadow(Canvas, Box, 6.f * S, S, 1.f);
		FillChamfer(Canvas, Box, 6.f * S, FLinearColor(0.05f, 0.09f, 0.18f, 0.98f), FLinearColor(0.02f, 0.04f, 0.09f, 0.98f));
		StrokePolygon(Canvas, Chamfer(Box, 6.f * S), WithAlpha(ShellGold, 0.7f), FMath::Max(1.f, S));
		ShellText(Shell.Tooltip, At.X + Pad, At.Y + Pad, ShellTextColor, TextSize);
	}
	// A scripted click that nothing on the screen answered to.
	if (!Shell.TestClick.IsEmpty() && ++Shell.TestClickFrames > 20)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Test menu: nothing on the screen has the id '%s'"), *Shell.TestClick);
		Shell.TestClick.Reset();
	}
}

// ---- the parts every screen has --------------------------------------------------------------------

void AGenHUD::ShellHeader(const FString& Title, const FString& Crumb)
{
	const float S = Shell.Scale;
	if (!Crumb.IsEmpty())
	{
		// (The way back: where Esc goes.)
		ShellTextEx(TEXT("<  ") + Crumb.ToUpper(), ShellRect(62, 0, 0, 0).Min.X, ShellRect(0, 20, 0, 0).Min.Y, ShellDim * 1.3f, 13.f, 0, true, 160);
	}
	ShellTextEx(Title.ToUpper(), ShellRect(60, 0, 0, 0).Min.X, ShellRect(0, 38, 0, 0).Min.Y, ShellGold, 38.f, 0, true, 80);
	const FBox2D Rule = ShellRect(60, 98, 1480, 0);
	FillGradientH(Canvas, FBox2D(Rule.Min, FVector2D(Rule.Max.X, Rule.Min.Y + FMath::Max(1.f, S))), WithAlpha(ShellEdge, 0.6f), WithAlpha(ShellEdge, 0.f));
	DrawRect(ShellGold, Rule.Min.X, Rule.Min.Y - S, 90.f * S, 3.f * S);
}

void AGenHUD::ShellBottomBar()
{
	const float Top = ShellRect(0, 780, 0, 0).Min.Y;
	FillGradient(Canvas, FBox2D(FVector2D(0.f, Top), FVector2D(Canvas->SizeX, Canvas->SizeY)), FLinearColor(0.f, 0.01f, 0.03f, 0.f), FLinearColor(0.f, 0.01f, 0.03f, 0.8f));
	const FBox2D Rule = ShellRect(40, 792, 1520, 0);
	FillGradientH(Canvas, FBox2D(Rule.Min, FVector2D(Rule.GetCenter().X, Rule.Min.Y + FMath::Max(1.f, Shell.Scale))), WithAlpha(ShellEdge, 0.f), WithAlpha(ShellEdge, 0.45f));
	FillGradientH(Canvas, FBox2D(FVector2D(Rule.GetCenter().X, Rule.Min.Y), FVector2D(Rule.Max.X, Rule.Min.Y + FMath::Max(1.f, Shell.Scale))), WithAlpha(ShellEdge, 0.45f), WithAlpha(ShellEdge, 0.f));
}

void AGenHUD::ShellSection(const FString& Title, float X, float Y, float W)
{
	const float S = Shell.Scale;
	ShellTextEx(Title.ToUpper(), ShellRect(X, 0, 0, 0).Min.X, ShellRect(0, Y, 0, 0).Min.Y, ShellGold, 15.f, 0, true, 160);
	const FBox2D Rule = ShellRect(X, Y + 28.f, W, 0);
	DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, S));
	DrawRect(WithAlpha(ShellGold, 0.8f), Rule.Min.X, Rule.Min.Y - S * 0.5f, 36.f * S, 2.f * S);
}

void AGenHUD::ShellTextField(const FBox2D& R, FString& Text, int32 MaxLen, bool& bEditing, const FString& Placeholder, bool& bOutFinished)
{
	// A click on it lets it be typed into; Enter, Esc or a click elsewhere ends that.
	const float S = Shell.Scale;
	const bool bHover = ShellHover(R);
	const bool bWasEditing = bEditing;
	if (Shell.bClick && !Shell.bBlocked && !Shell.bUnderOptions)
	{
		bEditing = bHover;
		Shell.bClick = Shell.bClick && !bHover;
	}
	if (bEditing)
	{
		bool bDone = false;
		ShellTypeInto(Text, MaxLen, bDone);
		bEditing = !bDone;
	}
	bOutFinished = bWasEditing && !bEditing;
	const float T = ShellAnim(FString::Printf(TEXT("Field@%.0f,%.0f"), R.Min.X, R.Min.Y), bHover || bEditing);
	FillGradient(Canvas, R, FMath::Lerp(FLinearColor(0.01f, 0.02f, 0.05f, 0.9f), FLinearColor(0.03f, 0.07f, 0.16f, 0.95f), T), FLinearColor(0.02f, 0.04f, 0.1f, 0.9f));
	StrokePolygon(Canvas, Chamfer(R, 0.f), WithAlpha(ShellEdge, 0.25f + 0.25f * T), FMath::Max(1.f, S));
	DrawRect(bEditing ? ShellGold : FMath::Lerp(WithAlpha(ShellEdge, 0.8f), ShellAccent, T), R.Min.X, R.Max.Y - 2.f * S, R.GetSize().X, 2.f * S);
	const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(19.f * S, false)).Y);
	// (A blinking bar where the next letter goes.)
	const bool bCaret = bEditing && FMath::Fmod(float(FPlatformTime::Seconds()), 1.f) < 0.5f;
	if (Text.IsEmpty() && !bEditing)
	{
		ShellText(Placeholder, R.Min.X + 12.f * S, R.Min.Y + (R.GetSize().Y - TextHeight) * 0.5f, ShellDim, 18.f);
	}
	else
	{
		ShellText(Text + (bCaret ? TEXT("|") : TEXT("")), R.Min.X + 12.f * S, R.Min.Y + (R.GetSize().Y - TextHeight) * 0.5f, FLinearColor::White, 19.f, 0, true);
	}
}

bool AGenHUD::ShellMenuItem(const FString& Id, const FBox2D& R, const FString& Label, const FString& Sub, bool bEnabled)
{
	const float S = Shell.Scale;
	const bool bHover = bEnabled && ShellHover(R);
	const bool bPressed = bHover && Shell.bMouseDown;
	const float T = ShellAnim(Id, bHover, 12.f);
	const float H = float(R.GetSize().Y);
	// A band of light from the left that strengthens as the mouse comes on, and the gold bar at its edge.
	FillGradientH(Canvas, R, FLinearColor(0.03f, 0.06f, 0.13f, 0.55f), FLinearColor(0.03f, 0.06f, 0.13f, 0.f));
	if (T > 0.01f)
	{
		const FBox2D Lit(R.Min, FVector2D(R.Min.X + float(R.GetSize().X) * (0.55f + 0.45f * T), R.Max.Y));
		FillGradientH(Canvas, Lit, FLinearColor(0.12f, 0.27f, 0.56f, 0.85f * T * (bPressed ? 0.7f : 1.f)), FLinearColor(0.12f, 0.27f, 0.56f, 0.f));
	}
	DrawRect(WithAlpha(bEnabled ? ShellGold : ShellDim, 0.45f + 0.55f * T), R.Min.X, R.Min.Y, (3.f + 3.f * T) * S, H);
	const float LabelX = R.Min.X + (26.f + 10.f * T) * S;
	const FLinearColor LabelColor = bEnabled ? FMath::Lerp(ShellTextColor, ShellGold, T) : ShellDim;
	ShellTextEx(Label.ToUpper(), LabelX, R.Min.Y + H * 0.13f, LabelColor, 26.f, 0, true, 120);
	ShellTextEx(Sub, LabelX, R.Min.Y + H * 0.6f, bEnabled ? FMath::Lerp(ShellDim * 1.35f, ShellTextColor, T) : WithAlpha(ShellDim, 0.7f), 14.f, 0, false, 20);
	if (T > 0.01f)
	{
		// (The chevron that slides in at the right.)
		const float AX = R.Max.X - (34.f - 10.f * T) * S, AY = R.GetCenter().Y, AS = 7.f * S;
		const FLinearColor Mark = WithAlpha(ShellGold, T);
		DrawLine(AX - AS * 0.6f, AY - AS, AX + AS * 0.4f, AY, Mark, 2.5f * S);
		DrawLine(AX + AS * 0.4f, AY, AX - AS * 0.6f, AY + AS, Mark, 2.5f * S);
	}
	const bool bClicked = bEnabled && ((bHover && Shell.bClick) || ShellTestHit(Id));
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

UTexture2D* AGenHUD::GetMapPreview(const FString& MapName)
{
	// MapUtil.cpp getMapPreviewImage: the map's own picture lies next to it, as a TGA of the same name.
	const FString Key = MapName.ToLower();
	if (const TObjectPtr<UTexture2D>* Found = MapPreviews.Find(Key))
	{
		return *Found;
	}
	UTexture2D* Texture = nullptr;
	TArray<uint8> Bytes;
	FGenImage Image;
	if (FGenFileSystem::Get().ReadFile(FString::Printf(TEXT("Maps\\%s\\%s.tga"), *MapName, *MapName), Bytes) && GenTextureLoader::DecodeTGA(Bytes, Image))
	{
		Texture = GenTextureLoader::CreateFromImage(Image, TEXT("MapPreview_") + Key, true, false);
	}
	MapPreviews.Add(Key, Texture);
	return Texture;
}

// ---- the menus before a game --------------------------------------------------------------------------

void AGenHUD::DrawShellMenu(AGenGameMode* GM)
{
	// Esc goes back one screen - unless an open list, the map selection or a line being typed into has it.
	const APlayerController* PC = GetOwningPlayerController();
	const AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
	const bool bEsc = GenPC && GenPC->IsGenInputEnabled() && !Options.bOpen && !Shell.bEditingName && !Shell.bEditingAddress
		&& PC->WasInputKeyJustPressed(EKeys::Escape);
	const EGenMenuScreen Screen = GM->GetMenuScreen();
	switch (Screen)
	{
	case EGenMenuScreen::Main:
		DrawMainMenu(GM);
		break;
	case EGenMenuScreen::Online:
		DrawOnlineMenu(GM);
		break;
	default:
		DrawSkirmishMenu(GM);
		break;
	}
	if (bEsc && Screen != EGenMenuScreen::Main && GM->GetPhase() == EGenGamePhase::Menu && GM->GetMenuScreen() == Screen)
	{
		if (!Shell.OpenId.IsEmpty())
		{
			Shell.OpenId.Reset();
		}
		else if (Shell.bMapSelect)
		{
			Shell.bMapSelect = false;
		}
		else
		{
			if (UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this))
			{
				Setup->Save();
			}
			GM->SetMenuScreen(EGenMenuScreen::Main);
			ShellClickSound();
		}
	}
	// A new screen starts clean.
	if (GM->GetMenuScreen() != Screen || GM->GetPhase() != EGenGamePhase::Menu)
	{
		Shell.OpenId.Reset();
		Shell.bMapSelect = false;
		Shell.bEditingName = Shell.bEditingAddress = false;
	}
}

void AGenHUD::DrawMainMenu(AGenGameMode* GM)
{
	const float S = Shell.Scale;
	const FVector2D View(Canvas->SizeX, Canvas->SizeY);
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.18f);
	// The menu's side of the screen in shadow, fading into the picture.
	const float Solid = ShellRect(560, 0, 0, 0).Min.X;
	const float Fade = ShellRect(980, 0, 0, 0).Min.X;
	DrawRect(FLinearColor(0.005f, 0.012f, 0.03f, 0.86f), 0.f, 0.f, Solid, View.Y);
	FillGradientH(Canvas, FBox2D(FVector2D(Solid, 0.f), FVector2D(Fade, View.Y)), FLinearColor(0.005f, 0.012f, 0.03f, 0.86f), FLinearColor(0.005f, 0.012f, 0.03f, 0.f));

	// The game's logo, and the line under it.
	if (!ShellImage(TEXT("GeneralsLogo"), ShellRect(86, 64, 444, 144)))
	{
		ShellTextEx(TEXT("GENERALS"), ShellRect(96, 0, 0, 0).Min.X, ShellRect(0, 96, 0, 0).Min.Y, ShellGold, 60.f, 0, true, 120);
	}
	const FBox2D Rule = ShellRect(100, 236, 420, 0);
	FillGradientH(Canvas, FBox2D(Rule.Min, FVector2D(Rule.Max.X, Rule.Min.Y + FMath::Max(1.f, S))), WithAlpha(ShellGold, 0.9f), WithAlpha(ShellGold, 0.f));

	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	const int32 NumMaps = FGenShellData::Get().GetMaps().Num();
	float Y = 300.f;
	const float Step = 92.f;
	if (ShellMenuItem(TEXT("Main.Skirmish"), ShellRect(100, Y, 460, 76), FGenShellData::Text(TEXT("GUI:Skirmish"), TEXT("Skirmish")),
		FString::Printf(TEXT("Battle computer armies on %d maps"), NumMaps)))
	{
		GM->SetMenuScreen(EGenMenuScreen::Skirmish);
	}
	Y += Step;
	if (ShellMenuItem(TEXT("Main.Online"), ShellRect(100, Y, 460, 76), TEXT("Online"),
		Net ? FString(TEXT("Host a network game or join one by its address")) : FString(TEXT("Network play is not available")), Net != nullptr))
	{
		GM->SetMenuScreen(EGenMenuScreen::Online);
	}
	Y += Step;
	if (ShellMenuItem(TEXT("Main.Options"), ShellRect(100, Y, 460, 76), FGenShellData::Text(TEXT("GUI:Options"), TEXT("Options")), TEXT("Video, controls, hotkeys and audio")))
	{
		OpenOptions();
	}
	Y += Step;
	if (ShellMenuItem(TEXT("Main.Exit"), ShellRect(100, Y, 460, 76), FGenShellData::Text(TEXT("GUI:Exit"), TEXT("Exit")), TEXT("Back to the desktop")))
	{
		if (UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this))
		{
			Setup->Save();
		}
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, false);
	}

	// At the foot: what this is, and who is playing.
	ShellTextEx(TEXT("COMMAND & CONQUER GENERALS ZERO HOUR"), ShellRect(100, 0, 0, 0).Min.X, ShellRect(0, 836, 0, 0).Min.Y, ShellDim, 12.f, 0, true, 220);
	ShellTextEx(TEXT("Remade in Unreal Engine from the original's data"), ShellRect(100, 0, 0, 0).Min.X, ShellRect(0, 856, 0, 0).Min.Y, WithAlpha(ShellDim, 0.75f), 12.f, 0, false, 40);
	if (const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this))
	{
		ShellTextEx(TEXT("COMMANDER"), ShellRect(1540, 0, 0, 0).Min.X, ShellRect(0, 828, 0, 0).Min.Y, ShellDim, 12.f, 2, true, 220);
		ShellTextEx(Setup->PlayerName, ShellRect(1540, 0, 0, 0).Min.X, ShellRect(0, 846, 0, 0).Min.Y, ShellTextColor, 20.f, 2, true, 40);
	}
}

void AGenHUD::DrawOnlineMenu(AGenGameMode* GM)
{
	UGenSkirmishSetup* LocalSetup = UGenSkirmishSetup::Get(this);
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	if (!LocalSetup)
	{
		return;
	}
	LocalSetup->EnsureLoaded();
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.72f);
	ShellHeader(TEXT("Online"), TEXT("Main menu"));
	ShellBottomBar();

	// --- who plays ---
	ShellPanel(ShellRect(300, 140, 1000, 120), 0.9f);
	ShellSection(TEXT("Your name"), 330, 158, 940);
	bool bNameDone = false;
	ShellTextField(ShellRect(330, 200, 420, 44), LocalSetup->PlayerName, 16, Shell.bEditingName, TEXT("Player"), bNameDone);
	if (bNameDone && LocalSetup->PlayerName.TrimStartAndEnd().IsEmpty())
	{
		LocalSetup->PlayerName = FGenShellData::Text(TEXT("GUI:Player"), TEXT("Player"));
	}
	ShellText(TEXT("The other players see it in the lobby and the score."), ShellRect(780, 0, 0, 0).Min.X, ShellRect(0, 212, 0, 0).Min.Y, ShellDim, 16.f);

	auto Lines = [this](const TArray<const TCHAR*>& Text, float X, float Y)
	{
		for (const TCHAR* Line : Text)
		{
			ShellText(Line, ShellRect(X, 0, 0, 0).Min.X, ShellRect(0, Y, 0, 0).Min.Y, ShellTextColor * 0.9f, 17.f);
			Y += 26.f;
		}
	};

	// --- host ---
	ShellPanel(ShellRect(300, 290, 490, 420), 0.9f);
	ShellSection(TEXT("Host a game"), 330, 308, 430);
	Lines({ TEXT("Start a game on this computer; the others"), TEXT("join it by your address (port 8088)."), TEXT(""),
		TEXT("Your skirmish setup - the map, the computer"), TEXT("armies, the starting cash - opens the lobby,"), TEXT("where you start the game when all are in.") }, 330, 360);
	if (ShellPrimaryButton(TEXT("NetHost"), ShellRect(330, 626, 430, 56), TEXT("HOST GAME"), Net != nullptr))
	{
		LocalSetup->Save();
		GM->HostNetworkGame();
	}

	// --- join ---
	// (An address: an IP, or a name like yourname.duckdns.org for a home connection whose address changes;
	// ":port" after it if the host's isn't 8088.)
	ShellPanel(ShellRect(810, 290, 490, 420), 0.9f);
	ShellSection(TEXT("Join a game"), 840, 308, 430);
	Lines({ TEXT("The host's address: an IP, or a name like"), TEXT("yourname.duckdns.org - with :port after it"), TEXT("if the host's isn't 8088.") }, 840, 360);
	if (Net && !Shell.bJoinAddressLoaded)
	{
		Shell.bJoinAddressLoaded = true;
		if (GConfig) { GConfig->GetString(TEXT("Generals.Network"), TEXT("LastJoin"), Shell.JoinAddress, GGameUserSettingsIni); }
	}
	ShellTextEx(TEXT("HOST ADDRESS"), ShellRect(840, 0, 0, 0).Min.X, ShellRect(0, 454, 0, 0).Min.Y, ShellDim * 1.3f, 12.f, 0, true, 160);
	bool bAddressDone = false;
	ShellTextField(ShellRect(840, 476, 430, 48), Shell.JoinAddress, 64, Shell.bEditingAddress, TEXT("host address"), bAddressDone);
	FString SeatAddress, SeatName;
	uint64 Seat = 0;
	if (Net && Net->RecallSeat(SeatAddress, SeatName, Seat) && ShellButton(TEXT("NetRejoin"), ShellRect(840, 556, 430, 52), TEXT("REJOIN LAST MATCH")))
	{
		GM->RejoinNetworkGame();
	}
	if (ShellPrimaryButton(TEXT("NetJoin"), ShellRect(840, 626, 430, 56), TEXT("JOIN GAME"), Net && !Shell.JoinAddress.TrimStartAndEnd().IsEmpty()))
	{
		if (GConfig)
		{
			GConfig->SetString(TEXT("Generals.Network"), TEXT("LastJoin"), *Shell.JoinAddress, GGameUserSettingsIni);
			GConfig->Flush(false, GGameUserSettingsIni);
		}
		LocalSetup->Save();
		GM->JoinNetworkGame(Shell.JoinAddress);
	}

	if (!GM->GetShellError().IsEmpty())
	{
		ShellText(GM->GetShellError(), ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 738, 0, 0).Min.Y, ShellRed, 20.f, 1, true);
	}
	if (ShellButton(TEXT("Online.Back"), ShellRect(40, 812, 240, 56), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		LocalSetup->Save();
		GM->ClearShellError();
		GM->SetMenuScreen(EGenMenuScreen::Main);
	}
}

// ---- the skirmish menu ----------------------------------------------------------------------------

void AGenHUD::DrawMapPreview(const FGenMapInfo& Map, const FBox2D& Area, bool bInteractive)
{
	FGenSetupData* Setup = ShellSetup ? ShellSetup : static_cast<FGenSetupData*>(UGenSkirmishSetup::Get(this));
	const FGenShellData& Data = FGenShellData::Get();
	// The picture, in the map's own proportions.
	const FVector2f Extent = Map.ExtentMax - Map.ExtentMin;
	const float Aspect = (Extent.X > 1.f && Extent.Y > 1.f) ? Extent.X / Extent.Y : 1.f;
	FVector2D Size = Area.GetSize();
	if (Size.X / Size.Y > Aspect) { Size.X = Size.Y * Aspect; }
	else { Size.Y = Size.X / Aspect; }
	const FVector2D Min = Area.GetCenter() - Size * 0.5;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.85f), Area.Min.X, Area.Min.Y, Area.GetSize().X, Area.GetSize().Y);
	if (UTexture2D* Preview = GetMapPreview(Map.Name))
	{
		DrawTexture(Preview, Min.X, Min.Y, Size.X, Size.Y, 0.f, 0.f, 1.f, 1.f);
		StrokePolygon(Canvas, Chamfer(FBox2D(Min, Min + Size), 0.f), WithAlpha(ShellEdge, 0.6f), FMath::Max(1.f, Shell.Scale));
	}
	else
	{
		ShellText(Map.DisplayName, Area.GetCenter().X, Area.GetCenter().Y - 10.f * Shell.Scale, ShellDim, 18.f, 1);
	}
	auto ToScreen = [&](const FVector2f& P)
	{
		const float U = Extent.X > 1.f ? (P.X - Map.ExtentMin.X) / Extent.X : 0.5f;
		const float V = Extent.Y > 1.f ? 1.f - (P.Y - Map.ExtentMin.Y) / Extent.Y : 0.5f;
		return Min + FVector2D(U * Size.X, V * Size.Y);
	};
	// Supply docks and tech buildings (the original marks them with small icons).
	const float Dot = 7.f * Shell.Scale;
	for (const FVector2f& P : Map.SupplyPositions)
	{
		const FVector2D Sp = ToScreen(P);
		DrawRect(FLinearColor::Black, Sp.X - Dot * 0.5f - 1.f, Sp.Y - Dot * 0.5f - 1.f, Dot + 2.f, Dot + 2.f);
		DrawRect(FLinearColor(0.35f, 1.f, 0.35f), Sp.X - Dot * 0.5f, Sp.Y - Dot * 0.5f, Dot, Dot);
	}
	for (const FVector2f& P : Map.TechPositions)
	{
		const FVector2D Sp = ToScreen(P);
		DrawRect(FLinearColor::Black, Sp.X - Dot * 0.5f - 1.f, Sp.Y - Dot * 0.5f - 1.f, Dot + 2.f, Dot + 2.f);
		DrawRect(FLinearColor(1.f, 0.6f, 0.15f), Sp.X - Dot * 0.5f, Sp.Y - Dot * 0.5f, Dot, Dot);
	}
	// Start positions: whose it is shows by number and colour; a click hands it on (see below).
	const float Mark = 30.f * Shell.Scale;
	for (int32 i = 0; i < Map.NumPlayers && i < Map.Starts.Num(); ++i)
	{
		const FVector2D Sp = ToScreen(Map.Starts[i]);
		const FBox2D Box(Sp - FVector2D(Mark, Mark) * 0.5, Sp + FVector2D(Mark, Mark) * 0.5);
		const int32 Holder = (bInteractive && Setup) ? Setup->FindSlotAtStart(i) : INDEX_NONE;
		const bool bHover = bInteractive && ShellHover(Box);
		FLinearColor Tint = bHover ? FLinearColor::White : FLinearColor(0.8f, 0.8f, 0.8f);
		if (Holder != INDEX_NONE && Data.GetColors().IsValidIndex(Setup->Slots[Holder].Color))
		{
			Tint = Data.GetColors()[Setup->Slots[Holder].Color].Color;
		}
		if (!ShellImage(bHover ? TEXT("PlayerStartHilite") : TEXT("PlayerStart"), Box, Tint))
		{
			DrawRect(FLinearColor::Black, Box.Min.X, Box.Min.Y, Mark, Mark);
			DrawRect(Tint, Box.Min.X + 2.f, Box.Min.Y + 2.f, Mark - 4.f, Mark - 4.f);
		}
		if (Holder != INDEX_NONE)
		{
			ShellText(FString::FromInt(Holder + 1), Sp.X, Sp.Y - 11.f * Shell.Scale, FLinearColor::White, 17.f, 1, true);
		}
		if (!bInteractive || !Setup)
		{
			continue;
		}
		// SkirmishGameOptionsMenu: a left click on a taken spot passes it to the next player in the list
		// who has none (and frees it if there is nobody); on a free one, the first such player gets it,
		// or the human player moves there. A right click frees it.
		const bool bLeft = (bHover && Shell.bClick) || ShellTestHit(FString::Printf(TEXT("Start%d"), i + 1));
		if (bLeft)
		{
			Shell.bClick = false;
			ShellClickSound();
			auto NextWithoutSpot = [Setup](int32 From)
			{
				for (int32 j = From; j < UGenSkirmishSetup::MaxSlots; ++j)
				{
					if (Setup->Slots[j].IsOccupied() && Setup->Slots[j].StartPos < 0) { return j; }
				}
				return int32(INDEX_NONE);
			};
			if (Holder != INDEX_NONE)
			{
				const int32 Next = NextWithoutSpot(Holder + 1);
				Setup->SetSlotStartPos(Holder, -1);
				if (Next != INDEX_NONE) { Setup->SetSlotStartPos(Next, i); }
			}
			else
			{
				const int32 Next = NextWithoutSpot(0);
				Setup->SetSlotStartPos(Next != INDEX_NONE ? Next : 0, i);
			}
		}
		else if (bHover && Shell.bRightClick && Holder != INDEX_NONE)
		{
			Shell.bRightClick = false;
			Setup->SetSlotStartPos(Holder, -1);
		}
	}
}

void AGenHUD::ShellTypeInto(FString& Text, int32 MaxLen, bool& bOutDone)
{
	bOutDone = false;
	AGenPlayerController* KeyPC = Cast<AGenPlayerController>(GetOwningPlayerController());
	if (!KeyPC || !KeyPC->IsGenInputEnabled())
	{
		return;
	}
	const bool bShift = KeyPC->IsInputKeyDown(EKeys::LeftShift) || KeyPC->IsInputKeyDown(EKeys::RightShift);
	const bool bCtrl = KeyPC->IsInputKeyDown(EKeys::LeftControl) || KeyPC->IsInputKeyDown(EKeys::RightControl);
	static const FKey Letters[] = { EKeys::A, EKeys::B, EKeys::C, EKeys::D, EKeys::E, EKeys::F, EKeys::G, EKeys::H, EKeys::I, EKeys::J, EKeys::K, EKeys::L, EKeys::M,
		EKeys::N, EKeys::O, EKeys::P, EKeys::Q, EKeys::R, EKeys::S, EKeys::T, EKeys::U, EKeys::V, EKeys::W, EKeys::X, EKeys::Y, EKeys::Z };
	static const FKey Digits[] = { EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
	static const FKey PadDigits[] = { EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
		EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine };
	FString Typed;
	if (bCtrl)
	{
		// Ctrl+V: whatever is on the clipboard, with the line breaks and control characters taken out.
		if (KeyPC->WasInputKeyJustPressed(EKeys::V))
		{
			FString Clip;
			FPlatformApplicationMisc::ClipboardPaste(Clip);
			for (int32 i = 0; i < Clip.Len(); ++i)
			{
				const TCHAR C = Clip[i];
				if (C >= 32 && C != 127) { Typed.AppendChar(C); }
			}
		}
	}
	else
	{
		for (int32 k = 0; k < UE_ARRAY_COUNT(Letters); ++k)
		{
			if (KeyPC->WasInputKeyJustPressed(Letters[k])) { Typed.AppendChar(TCHAR((bShift ? TEXT('A') : TEXT('a')) + k)); }
		}
		for (int32 k = 0; k < UE_ARRAY_COUNT(Digits); ++k)
		{
			if (KeyPC->WasInputKeyJustPressed(Digits[k]) || KeyPC->WasInputKeyJustPressed(PadDigits[k])) { Typed.AppendChar(TCHAR(TEXT('0') + k)); }
		}
		if (KeyPC->WasInputKeyJustPressed(EKeys::SpaceBar)) { Typed.AppendChar(TEXT(' ')); }
		if (KeyPC->WasInputKeyJustPressed(EKeys::Hyphen) || KeyPC->WasInputKeyJustPressed(EKeys::Subtract)) { Typed.AppendChar(bShift ? TEXT('_') : TEXT('-')); }
		if (KeyPC->WasInputKeyJustPressed(EKeys::Period) || KeyPC->WasInputKeyJustPressed(EKeys::Decimal)) { Typed.AppendChar(TEXT('.')); }
		if (KeyPC->WasInputKeyJustPressed(EKeys::Semicolon) && bShift) { Typed.AppendChar(TEXT(':')); }
	}
	if (KeyPC->WasInputKeyJustPressed(EKeys::BackSpace) && Text.Len() > 0) { Text.LeftChopInline(1); }
	Text = (Text + Typed).Left(MaxLen);
	bOutDone = KeyPC->WasInputKeyJustPressed(EKeys::Enter) || KeyPC->WasInputKeyJustPressed(EKeys::Escape);
}

void AGenHUD::ShellStatic(const FBox2D& R, const FString& Text, const FLinearColor& Color)
{
	FillGradient(Canvas, R, FLinearColor(0.015f, 0.03f, 0.07f, 0.6f), FLinearColor(0.015f, 0.03f, 0.07f, 0.8f));
	DrawRect(WithAlpha(ShellEdge, 0.25f), R.Min.X, R.Max.Y - Shell.Scale, R.GetSize().X, FMath::Max(1.f, Shell.Scale));
	const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(18.f * Shell.Scale, false)).Y);
	ShellText(Text, R.Min.X + 12.f * Shell.Scale, R.Min.Y + (R.GetSize().Y - TextHeight) * 0.5f, Color, 18.f);
}

void AGenHUD::DrawSkirmishMenu(AGenGameMode* GM)
{
	UGenSkirmishSetup* LocalSetup = UGenSkirmishSetup::Get(this);
	if (!LocalSetup)
	{
		return;
	}
	LocalSetup->EnsureLoaded();
	// A network game's lobby is this screen as well: it shows the host's setup, and what this player changes in
	// it (its own place; the host also the rest that isn't another player's) goes to the host, which sends
	// everybody the result.
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	FGenNetClient* Client = (GM->GetPhase() == EGenGamePhase::NetLobby && Net) ? Net->GetClient() : nullptr;
	if (GM->GetPhase() == EGenGamePhase::NetLobby && (!Client || !Client->HasLobby() || Client->GetLocalSlot() == INDEX_NONE))
	{
		DrawNetWaiting(GM, Client);
		return;
	}
	FGenSetupData NetSetup;
	if (Client)
	{
		NetSetup = Client->GetLobby();
	}
	FGenSetupData* Setup = Client ? &NetSetup : static_cast<FGenSetupData*>(LocalSetup);
	ShellSetup = Setup;
	const int32 LocalSlot = Client ? Client->GetLocalSlot() : 0;
	const bool bHost = !Client || Client->IsHost();
	auto MayEdit = [Client, LocalSlot, bHost](int32 Slot) { return !Client || Slot == LocalSlot || (bHost && !Client->IsSlotHuman(Slot)); };
	const FGenShellData& Data = FGenShellData::Get();
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.72f);
	Shell.bBlocked = Shell.bMapSelect; // under the map selection nothing reacts

	const float S = Shell.Scale;
	ShellHeader(Client ? FString(TEXT("Network game")) : FGenShellData::Text(TEXT("GUI:Skirmish"), TEXT("Skirmish")),
		Client ? FString(TEXT("Online")) : FString(TEXT("Main menu")));
	ShellBottomBar();

	// --- who plays ---
	ShellPanel(ShellRect(40, 116, 1010, 650), 0.9f);
	const float HeadY = ShellRect(0, 134, 0, 0).Min.Y;
	auto Column = [&](const FString& Label, float X) { ShellTextEx(Label.ToUpper(), ShellRect(X, 0, 0, 0).Min.X, HeadY, ShellGold, 14.f, 0, true, 160); };
	Column(FGenShellData::Text(TEXT("GUI:Players"), TEXT("Players")), 62);
	Column(FGenShellData::Text(TEXT("GUI:Color"), TEXT("Color")), 402);
	Column(FGenShellData::Text(TEXT("GUI:Faction"), TEXT("Army")), 572);
	Column(FGenShellData::Text(TEXT("GUI:Team"), TEXT("Team")), 902);
	{
		const FBox2D Rule = ShellRect(60, 160, 970, 0);
		DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, S));
	}

	const TArray<FString> StateItems = {
		FGenShellData::Text(TEXT("GUI:Open"), TEXT("Open")), FGenShellData::Text(TEXT("GUI:Closed"), TEXT("Closed")),
		FGenShellData::Text(TEXT("GUI:EasyAI"), TEXT("Easy Army")), FGenShellData::Text(TEXT("GUI:MediumAI"), TEXT("Medium Army")),
		FGenShellData::Text(TEXT("GUI:HardAI"), TEXT("Hard Army")) };
	TArray<FString> SideItems = { FGenShellData::Text(TEXT("GUI:Random"), TEXT("Random")) };
	for (const FGenSideInfo& Side : Data.GetSides())
	{
		SideItems.Add(Side.Label);
	}
	TArray<FString> TeamItems = { FGenShellData::Text(TEXT("Team:0"), TEXT("None")) };
	for (int32 t = 1; t <= UGenSkirmishSetup::MaxSlots / 2; ++t)
	{
		TeamItems.Add(FGenShellData::Text(*FString::Printf(TEXT("Team:%d"), t), *FString::FromInt(t)));
	}
	const TArray<FLinearColor> NoColors;

	for (int32 i = 0; i < UGenSkirmishSetup::MaxSlots; ++i)
	{
		const float Y = 172.f + i * 56.f;
		const FGenSetupSlot& Slot = Setup->Slots[i];
		// (Every other row a shade lighter, and the player's own marked in gold at the left.)
		const FBox2D Band = ShellRect(52, Y - 4.f, 986, 52);
		if (i % 2 == 1)
		{
			DrawRect(FLinearColor(0.3f, 0.5f, 1.f, 0.035f), Band.Min.X, Band.Min.Y, Band.GetSize().X, Band.GetSize().Y);
		}
		if (i == LocalSlot)
		{
			DrawRect(WithAlpha(ShellGold, 0.85f), Band.Min.X, Band.Min.Y + 6.f * S, 3.f * S, Band.GetSize().Y - 12.f * S);
		}
		int32 Picked = INDEX_NONE;
		if (i == LocalSlot)
		{
			// The player's own place: a click on the name lets it be typed (Enter or a click elsewhere ends that).
			bool bNameDone = false;
			ShellTextField(ShellRect(62, Y, 318, 44), LocalSetup->PlayerName, 16, Shell.bEditingName, FString(), bNameDone);
			if (bNameDone)
			{
				if (LocalSetup->PlayerName.TrimStartAndEnd().IsEmpty()) { LocalSetup->PlayerName = FGenShellData::Text(TEXT("GUI:Player"), TEXT("Player")); }
				if (Client) { Client->SendName(LocalSetup->PlayerName); }
			}
		}
		else if (Client && Client->IsSlotHuman(i))
		{
			ShellStatic(ShellRect(62, Y, 318, 44), Slot.Name, FLinearColor::White);
		}
		else if (!MayEdit(i))
		{
			ShellStatic(ShellRect(62, Y, 318, 44), StateItems.IsValidIndex(int32(Slot.State)) ? StateItems[int32(Slot.State)] : FString(), ShellTextColor);
		}
		else if (ShellDropdown(FString::Printf(TEXT("Slot%d.State"), i), ShellRect(62, Y, 318, 44), StateItems, NoColors, int32(Slot.State), Picked))
		{
			Setup->SetSlotState(i, EGenSlotState(Picked));
		}
		if (!Slot.IsOccupied())
		{
			continue;
		}
		if (!MayEdit(i))
		{
			// (Another player's choices: shown, not changed here.)
			const bool bHasColor = Data.GetColors().IsValidIndex(Slot.Color);
			ShellStatic(ShellRect(400, Y, 150, 44), bHasColor ? Data.GetColors()[Slot.Color].Label : FGenShellData::Text(TEXT("GUI:???"), TEXT("???")),
				bHasColor ? Data.GetColors()[Slot.Color].Color : ShellTextColor);
			ShellStatic(ShellRect(570, Y, 310, 44), SideItems.IsValidIndex(Slot.Side + 1) ? SideItems[Slot.Side + 1] : FString(), ShellTextColor);
			ShellStatic(ShellRect(900, Y, 130, 44), TeamItems.IsValidIndex(Slot.Team + 1) ? TeamItems[Slot.Team + 1] : FString(), ShellTextColor);
			continue;
		}
		// The colours nobody else wears, after "???" for "any".
		TArray<FString> ColorItems = { FGenShellData::Text(TEXT("GUI:???"), TEXT("???")) };
		TArray<FLinearColor> ColorColors = { ShellTextColor };
		TArray<int32> ColorIndex = { -1 };
		for (int32 c = 0; c < Data.GetColors().Num(); ++c)
		{
			if (!Setup->IsColorTaken(c, i))
			{
				ColorItems.Add(Data.GetColors()[c].Label);
				ColorColors.Add(Data.GetColors()[c].Color);
				ColorIndex.Add(c);
			}
		}
		if (ShellDropdown(FString::Printf(TEXT("Slot%d.Color"), i), ShellRect(400, Y, 150, 44), ColorItems, ColorColors, FMath::Max(0, ColorIndex.Find(Slot.Color)), Picked))
		{
			Setup->SetSlotColor(i, ColorIndex[Picked]);
		}
		if (ShellDropdown(FString::Printf(TEXT("Slot%d.Side"), i), ShellRect(570, Y, 310, 44), SideItems, NoColors, Slot.Side + 1, Picked))
		{
			Setup->SetSlotSide(i, Picked - 1);
		}
		if (ShellDropdown(FString::Printf(TEXT("Slot%d.Team"), i), ShellRect(900, Y, 130, 44), TeamItems, NoColors, Slot.Team + 1, Picked))
		{
			Setup->SetSlotTeam(i, Picked - 1);
		}
	}

	// --- the rules ---
	{
		const FBox2D Rule = ShellRect(60, 632, 970, 0);
		DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, S));
	}
	ShellTextEx(FGenShellData::Text(TEXT("GUI:StartingMoney"), TEXT("Starting Cash")).ToUpper(), ShellRect(62, 0, 0, 0).Min.X, ShellRect(0, 650, 0, 0).Min.Y, ShellGold, 14.f, 0, true, 160);
	TArray<FString> CashItems;
	for (int32 Cash : Data.GetMoneyChoices())
	{
		CashItems.Add(FString::Printf(TEXT("$%d"), Cash));
	}
	int32 PickedCash = INDEX_NONE;
	if (!bHost)
	{
		ShellStatic(ShellRect(62, 680, 200, 44), FString::Printf(TEXT("$%d"), Setup->StartingCash), ShellTextColor);
	}
	else if (ShellDropdown(TEXT("Cash"), ShellRect(62, 680, 200, 44), CashItems, NoColors, Data.GetMoneyChoices().Find(Setup->StartingCash), PickedCash))
	{
		Setup->StartingCash = Data.GetMoneyChoices()[PickedCash];
	}
	// (The battle group is the skirmish's: a network game's players start as the original's do.)
	if (!Client && ShellCheckbox(TEXT("TestUnits"), ShellRect(400, 686, 32, 32), TEXT("Start with a battle group (for trying things out)"), Setup->bTestUnits))
	{
		Setup->bTestUnits = !Setup->bTestUnits;
	}

	// --- where ---
	ShellPanel(ShellRect(1070, 116, 490, 650), 0.9f);
	ShellSection(FGenShellData::Text(TEXT("GUI:Map"), TEXT("Map")), 1092, 134, 446);
	if (const FGenMapInfo* Map = Setup->GetMapInfo())
	{
		DrawMapPreview(*Map, ShellRect(1092, 178, 446, 400), true);
		ShellText(Map->DisplayName, ShellRect(1092, 0, 0, 0).Min.X, ShellRect(0, 588, 0, 0).Min.Y, FLinearColor::White, 24.f, 0, true);
		const bool bTooMany = Setup->GetNumPlayers() > Map->NumPlayers;
		ShellText(FString::Printf(TEXT("%s %d / %d"), *FGenShellData::Text(TEXT("GUI:NumPlayers"), TEXT("Players:")), Setup->GetNumPlayers(), Map->NumPlayers),
			ShellRect(1092, 0, 0, 0).Min.X, ShellRect(0, 624, 0, 0).Min.Y, bTooMany ? ShellRed : ShellTextColor, 17.f);
		// (What the little squares on the map are.)
		const FBox2D Supply = ShellRect(1300, 630, 9, 9), Tech = ShellRect(1410, 630, 9, 9);
		DrawRect(FLinearColor(0.35f, 1.f, 0.35f), Supply.Min.X, Supply.Min.Y, Supply.GetSize().X, Supply.GetSize().Y);
		DrawRect(FLinearColor(1.f, 0.6f, 0.15f), Tech.Min.X, Tech.Min.Y, Tech.GetSize().X, Tech.GetSize().Y);
		ShellText(TEXT("Supplies"), ShellRect(1316, 0, 0, 0).Min.X, ShellRect(0, 625, 0, 0).Min.Y, ShellDim * 1.3f, 14.f);
		ShellText(TEXT("Tech"), ShellRect(1426, 0, 0, 0).Min.X, ShellRect(0, 625, 0, 0).Min.Y, ShellDim * 1.3f, 14.f);
	}
	if (bHost && ShellButton(TEXT("SelectMap"), ShellRect(1092, 680, 446, 52), FGenShellData::Text(TEXT("GUI:SelectMap"), TEXT("SELECT MAP"))))
	{
		Shell.bMapSelect = true;
		Shell.MapChoice = Setup->Map;
		const int32 Index = Data.GetMaps().IndexOfByPredicate([Setup](const FGenMapInfo& M) { return M.Name.Equals(Setup->Map, ESearchCase::IgnoreCase); });
		Shell.MapScroll = float(FMath::Max(0, Index - 6));
		GM->ClearShellError();
	}

	// --- go ---
	const APlayerController* PC = GetOwningPlayerController();
	const AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
	const bool bEnter = !Shell.bMapSelect && !Options.bOpen && !Shell.bEditingName && !Shell.bEditingAddress && GenPC && GenPC->IsGenInputEnabled() && PC->WasInputKeyJustPressed(EKeys::Enter);
	if (Client)
	{
		// The network game: what is going on, then leave or (the host) start.
		float LineY = 806.f;
		for (int32 n = FMath::Max(0, GM->NetNotices.Num() - 2); n < GM->NetNotices.Num(); ++n)
		{
			ShellText(GM->NetNotices[n].Value, ShellRect(320, 0, 0, 0).Min.X, ShellRect(0, LineY, 0, 0).Min.Y, ShellTextColor, 16.f);
			LineY += 22.f;
		}
		ShellText(FString::Printf(TEXT("%s - ping %.0f ms"), *Client->GetStatus(), Client->GetPingMs()), ShellRect(320, 0, 0, 0).Min.X, ShellRect(0, 852, 0, 0).Min.Y,
			ShellDim, 15.f);
		if (ShellButton(TEXT("Leave"), ShellRect(40, 812, 240, 56), TEXT("LEAVE GAME")))
		{
			GM->LeaveNetworkGame();
		}
		if (ShellPrimaryButton(TEXT("Play"), ShellRect(1260, 812, 300, 56), bHost ? FString(TEXT("START GAME")) : FString(TEXT("WAITING FOR HOST")), bHost) || (bEnter && bHost))
		{
			Client->RequestStart();
		}
	}
	else
	{
		if (!GM->GetShellError().IsEmpty())
		{
			ShellText(GM->GetShellError(), ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 826, 0, 0).Min.Y, ShellRed, 20.f, 1, true);
		}
		if (ShellButton(TEXT("Back"), ShellRect(40, 812, 240, 56), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
		{
			LocalSetup->Save();
			GM->ClearShellError();
			GM->SetMenuScreen(EGenMenuScreen::Main);
		}
		if (ShellPrimaryButton(TEXT("Play"), ShellRect(1260, 812, 300, 56), FGenShellData::Text(TEXT("GUI:Start"), TEXT("PLAY GAME"))) || bEnter)
		{
			GM->RequestStart();
		}
	}

	if (Shell.bMapSelect)
	{
		Shell.bBlocked = false;
		DrawMapSelect();
	}

	// What this player changed goes to the host; the screen shows the host's lobby until it answers.
	if (Client)
	{
		TArray<uint8> Before, After;
		FMemoryWriter WB(Before), WA(After);
		FGenSetupData Old = Client->GetLobby();
		Old.Serialize(WB);
		NetSetup.Serialize(WA);
		if (Before != After)
		{
			GM->NetLobbyEdit(NetSetup);
		}
	}
	ShellSetup = nullptr;
}

void AGenHUD::DrawNetWaiting(AGenGameMode* GM, FGenNetClient* Client)
{
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.72f);
	ShellHeader(TEXT("Network game"), TEXT("Online"));
	ShellBottomBar();
	ShellPanel(ShellRect(300, 250, 1000, 320), 0.92f);
	const FString Status = Client ? Client->GetStatus() : FString(TEXT("Not connected"));
	const bool bFailed = Client && (Client->GetState() == FGenNetClient::EState::Refused || Client->GetState() == FGenNetClient::EState::Failed);
	ShellSection(bFailed ? TEXT("Can't connect") : TEXT("Connecting"), 330, 268, 940);
	ShellText(Status, ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 330, 0, 0).Min.Y, bFailed ? ShellRed : FLinearColor::White, 22.f, 1, true);
	if (Client && Client->GetState() == FGenNetClient::EState::Match)
	{
		ShellText(TEXT("Loading the match..."), ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 376, 0, 0).Min.Y, ShellTextColor, 18.f, 1);
	}
	else if (!bFailed)
	{
		// (A row of dots that run while it waits.)
		const int32 Lit = int32(FPlatformTime::Seconds() * 4.0) % 5;
		for (int32 d = 0; d < 5; ++d)
		{
			const FBox2D Dot = ShellRect(752.f + d * 20.f, 384, 10, 10);
			DrawRect(d == Lit ? ShellGold : WithAlpha(ShellEdge, 0.5f), Dot.Min.X, Dot.Min.Y, Dot.GetSize().X, Dot.GetSize().Y);
		}
	}
	float LineY = 430.f;
	for (const TPair<double, FString>& Line : GM->NetNotices)
	{
		if (LineY > 540.f)
		{
			break;
		}
		ShellText(Line.Value, ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, LineY, 0, 0).Min.Y, ShellTextColor, 17.f, 1);
		LineY += 24.f;
	}
	if (ShellButton(TEXT("Leave"), ShellRect(40, 812, 240, 56), bFailed ? FString(TEXT("BACK")) : FString(TEXT("CANCEL"))))
	{
		GM->LeaveNetworkGame();
	}
}

void AGenHUD::DrawNetOverlay(AGenGameMode* GM, UGenGameLogic* Logic)
{
	// Top middle, over the game: what the network game has to say (somebody dropped out, came back; the
	// connection is being made again; this game is catching up with the match; two games came apart).
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	FGenNetClient* Client = Net ? Net->GetClient() : nullptr;
	TArray<TPair<FString, FLinearColor>> Lines;
	if (Client && Client->GetStatus().StartsWith(TEXT("Lost")))
	{
		Lines.Emplace(Client->GetStatus() + TEXT(" - the game waits"), ShellRed);
	}
	if (Logic->IsCatchingUp() && Client)
	{
		Lines.Emplace(FString::Printf(TEXT("Catching up with the match: %d%%"), int32(100.0 * double(Logic->GetFrame()) / double(FMath::Max(1u, Client->GetReadyFrames())))), ShellGold);
	}
	if (GM)
	{
		for (const TPair<double, FString>& Notice : GM->NetNotices)
		{
			Lines.Emplace(Notice.Value, ShellTextColor);
		}
	}
	const float S = FMath::Max(0.6f, Canvas->SizeY / 1080.f);
	float Y = Canvas->SizeY * 0.12f;
	for (const TPair<FString, FLinearColor>& Line : Lines)
	{
		const float SavedScale = Shell.Scale;
		Shell.Scale = S;
		ShellText(Line.Key, Canvas->SizeX * 0.5f, Y, Line.Value, 20.f, 1, true);
		Shell.Scale = SavedScale;
		Y += 28.f * S;
	}
}

void AGenHUD::DrawMapSelect()
{
	FGenSetupData* Setup = ShellSetup ? ShellSetup : static_cast<FGenSetupData*>(UGenSkirmishSetup::Get(this));
	const TArray<FGenMapInfo>& Maps = FGenShellData::Get().GetMaps();
	DrawRect(FLinearColor(0.f, 0.f, 0.02f, 0.72f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	ShellPanel(ShellRect(230, 56, 1140, 788), 1.f);
	const float S = Shell.Scale;
	ShellTextEx(FGenShellData::Text(TEXT("GUI:SelectMap"), TEXT("SELECT MAP")).ToUpper(), ShellRect(256, 0, 0, 0).Min.X, ShellRect(0, 76, 0, 0).Min.Y, ShellGold, 28.f, 0, true, 100);
	ShellTextEx(FGenShellData::Text(TEXT("GUI:Players"), TEXT("Players")).ToUpper(), ShellRect(796, 0, 0, 0).Min.X, ShellRect(0, 98, 0, 0).Min.Y, ShellDim * 1.3f, 12.f, 2, true, 160);
	{
		const FBox2D Rule = ShellRect(250, 122, 1100, 0);
		DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, S));
	}

	// Test automation: "Map=<name>" (the name the list shows, or the map's folder).
	if (Shell.TestClick.StartsWith(TEXT("Map="), ESearchCase::IgnoreCase))
	{
		const FString Wanted = Shell.TestClick.Mid(4);
		Shell.TestClick.Reset();
		const int32 Index = Maps.IndexOfByPredicate([&Wanted](const FGenMapInfo& M)
		{
			return M.DisplayName.Equals(Wanted, ESearchCase::IgnoreCase) || M.Name.Equals(Wanted, ESearchCase::IgnoreCase);
		});
		if (Index != INDEX_NONE)
		{
			Shell.MapChoice = Maps[Index].Name;
			Shell.MapScroll = float(FMath::Max(0, Index - 6));
		}
		else
		{
			UE_LOG(LogGenerals, Warning, TEXT("Test menu: no map '%s'"), *Wanted);
		}
	}

	// The list: the wheel moves it.
	constexpr int32 Rows = 15;
	constexpr float RowHeight = 40.f;
	const FBox2D ListArea = ShellRect(250, 134, 560, Rows * RowHeight);
	const float MaxScroll = float(FMath::Max(0, Maps.Num() - Rows));
	if (ListArea.IsInside(Shell.Mouse))
	{
		Shell.MapScroll -= Shell.Wheel * 2.f;
	}
	Shell.MapScroll = FMath::Clamp(Shell.MapScroll, 0.f, MaxScroll);
	FillGradient(Canvas, ListArea, FLinearColor(0.f, 0.01f, 0.03f, 0.6f), FLinearColor(0.f, 0.01f, 0.03f, 0.75f));
	const int32 First = FMath::RoundToInt32(Shell.MapScroll);
	for (int32 r = 0; r < Rows && First + r < Maps.Num(); ++r)
	{
		const FGenMapInfo& Map = Maps[First + r];
		const FBox2D Row = ShellRect(250, 134 + r * RowHeight, 546, RowHeight - 2);
		const bool bChosen = Map.Name.Equals(Shell.MapChoice, ESearchCase::IgnoreCase);
		const bool bHover = ShellHover(Row);
		if (bChosen)
		{
			FillGradientH(Canvas, Row, FLinearColor(0.14f, 0.3f, 0.6f, 0.95f), FLinearColor(0.07f, 0.15f, 0.32f, 0.8f));
			DrawRect(ShellGold, Row.Min.X, Row.Min.Y, 3.f * S, Row.GetSize().Y);
		}
		else if (bHover)
		{
			FillGradientH(Canvas, Row, FLinearColor(0.08f, 0.16f, 0.32f, 0.85f), FLinearColor(0.08f, 0.16f, 0.32f, 0.25f));
		}
		else if (r % 2 == 1)
		{
			DrawRect(FLinearColor(0.3f, 0.5f, 1.f, 0.03f), Row.Min.X, Row.Min.Y, Row.GetSize().X, Row.GetSize().Y);
		}
		ShellText(Map.DisplayName, Row.Min.X + 16.f * S, Row.Min.Y + 8.f * S, bChosen ? FLinearColor::White : (bHover ? FLinearColor::White : ShellTextColor), 18.f, 0, bChosen);
		ShellText(FString::FromInt(Map.NumPlayers), Row.Max.X - 14.f * S, Row.Min.Y + 8.f * S, bChosen ? ShellGold : ShellDim * 1.3f, 18.f, 2, bChosen);
		if (bHover && Shell.bClick)
		{
			Shell.bClick = false;
			Shell.MapChoice = Map.Name;
			ShellClickSound();
		}
	}
	if (MaxScroll > 0.f)
	{
		// Where in the list the window is.
		const float Track = float(ListArea.GetSize().Y);
		const float Thumb = Track * Rows / Maps.Num();
		DrawRect(FLinearColor(0.01f, 0.02f, 0.05f, 0.7f), ListArea.Max.X - 5.f * S, ListArea.Min.Y, 3.f * S, Track);
		DrawRect(WithAlpha(ShellEdge, 0.9f), ListArea.Max.X - 7.f * S, ListArea.Min.Y + (Track - Thumb) * Shell.MapScroll / MaxScroll, 6.f * S, Thumb);
	}

	if (const FGenMapInfo* Map = FGenShellData::Get().FindMap(Shell.MapChoice))
	{
		DrawMapPreview(*Map, ShellRect(840, 134, 500, 500), false);
		ShellText(Map->DisplayName, ShellRect(840, 0, 0, 0).Min.X, ShellRect(0, 648, 0, 0).Min.Y, FLinearColor::White, 24.f, 0, true);
		ShellText(FString::Printf(TEXT("%s %d"), *FGenShellData::Text(TEXT("GUI:NumPlayers"), TEXT("Players:")), Map->NumPlayers),
			ShellRect(840, 0, 0, 0).Min.X, ShellRect(0, 686, 0, 0).Min.Y, ShellTextColor, 18.f);
		ShellText(FString::Printf(TEXT("%d x %d"), FMath::RoundToInt32(Map->ExtentMax.X - Map->ExtentMin.X), FMath::RoundToInt32(Map->ExtentMax.Y - Map->ExtentMin.Y)),
			ShellRect(1340, 0, 0, 0).Min.X, ShellRect(0, 686, 0, 0).Min.Y, ShellDim * 1.3f, 16.f, 2);
	}
	if (ShellButton(TEXT("MapBack"), ShellRect(250, 766, 260, 54), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		Shell.bMapSelect = false;
	}
	if (ShellPrimaryButton(TEXT("MapAccept"), ShellRect(1080, 766, 260, 54), FGenShellData::Text(TEXT("GUI:Accept"), TEXT("ACCEPT"))))
	{
		if (Setup)
		{
			Setup->SetMap(Shell.MapChoice);
		}
		Shell.bMapSelect = false;
	}
}

void AGenHUD::DrawLoading()
{
	// The original's multiplayer load screen behind, and along the bottom the map that is being loaded.
	const FGenMappedImage* Img = nullptr;
	ShellBackdrop(FindShellTexture(TEXT("Mp_Load"), Img) ? TEXT("Mp_Load") : TEXT("MainMenuBackdrop"), 0.15f);
	const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	const FGenMapInfo* Map = Setup ? Setup->GetMapInfo() : nullptr;
	const float S = Shell.Scale;
	const float Top = ShellRect(0, 740, 0, 0).Min.Y;
	FillGradient(Canvas, FBox2D(FVector2D(0.f, Top), FVector2D(Canvas->SizeX, Canvas->SizeY)), FLinearColor(0.f, 0.01f, 0.03f, 0.f), FLinearColor(0.f, 0.01f, 0.03f, 0.92f));
	ShellTextEx(FGenShellData::Text(TEXT("GUI:Loading"), TEXT("LOADING")).ToUpper(), ShellRect(100, 0, 0, 0).Min.X, ShellRect(0, 790, 0, 0).Min.Y, ShellGold, 30.f, 0, true, 200);
	if (Map)
	{
		ShellText(Map->DisplayName, ShellRect(100, 0, 0, 0).Min.X, ShellRect(0, 834, 0, 0).Min.Y, FLinearColor::White, 22.f, 0, true);
		ShellText(FString::Printf(TEXT("%d %s"), Setup->GetNumPlayers(), Setup->GetNumPlayers() == 1 ? TEXT("player") : TEXT("players")),
			ShellRect(1500, 0, 0, 0).Min.X, ShellRect(0, 838, 0, 0).Min.Y, ShellDim * 1.3f, 17.f, 2);
	}
	// (The bar: the map is read in one go, so it stands full - the line says "nearly there".)
	const FBox2D Bar = ShellRect(100, 876, 1400, 0);
	DrawRect(FLinearColor(0.02f, 0.04f, 0.09f, 0.9f), Bar.Min.X, Bar.Min.Y, Bar.GetSize().X, 3.f * S);
	FillGradientH(Canvas, FBox2D(Bar.Min, FVector2D(Bar.Min.X + Bar.GetSize().X * 0.92f, Bar.Min.Y + 3.f * S)), FLinearColor(0.55f, 0.35f, 0.06f, 1.f), ShellGold);
}

// ---- during and after a game ------------------------------------------------------------------------

void AGenHUD::DrawQuitMenu(AGenGameMode* GM)
{
	// Test automation opens it the way Esc does.
	if (ShellTestHit(TEXT("Menu.Open")) && !GM->IsQuitMenuOpen())
	{
		GM->ToggleQuitMenu();
	}
	if (!GM->IsQuitMenuOpen())
	{
		Shell.bQuitConfirm = false;
		return;
	}
	// (The options, opened from here, cover it all.)
	if (Options.bOpen)
	{
		return;
	}
	const float S = Shell.Scale;
	const bool bNetwork = UGenGameLogic::Get(this) && UGenGameLogic::Get(this)->IsNetworkGame();
	DrawRect(FLinearColor(0.f, 0.005f, 0.02f, 0.62f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	ShellPanel(ShellRect(570, 150, 460, 600), 1.f);
	if (!ShellImage(TEXT("GeneralsLogo"), ShellRect(615, 176, 370, 120)))
	{
		ShellTextEx(TEXT("GENERALS"), Canvas->SizeX * 0.5f, ShellRect(0, 206, 0, 0).Min.Y, ShellGold, 34.f, 1, true, 120);
	}
	// (A network game goes on whatever one player does: no restart there, and the game doesn't stand still.)
	ShellTextEx(bNetwork ? TEXT("NETWORK GAME  -  THE BATTLE GOES ON") : TEXT("GAME PAUSED"), Canvas->SizeX * 0.5f, ShellRect(0, 308, 0, 0).Min.Y,
		ShellDim * 1.4f, 13.f, 1, true, 220);
	{
		const FBox2D Rule = ShellRect(610, 338, 380, 0);
		FillGradientH(Canvas, FBox2D(Rule.Min, FVector2D(Rule.GetCenter().X, Rule.Min.Y + FMath::Max(1.f, S))), WithAlpha(ShellGold, 0.f), WithAlpha(ShellGold, 0.8f));
		FillGradientH(Canvas, FBox2D(FVector2D(Rule.GetCenter().X, Rule.Min.Y), FVector2D(Rule.Max.X, Rule.Min.Y + FMath::Max(1.f, S))), WithAlpha(ShellGold, 0.8f), WithAlpha(ShellGold, 0.f));
	}

	Shell.bBlocked = Shell.bQuitConfirm; // (under the question nothing reacts)
	float Y = 362.f;
	const float Step = 68.f;
	if (ShellPrimaryButton(TEXT("Menu.Resume"), ShellRect(610, Y, 380, 54), FGenShellData::Text(TEXT("GUI:ReturnToGame"), TEXT("RETURN TO GAME"))))
	{
		GM->ToggleQuitMenu();
	}
	Y += Step;
	if (ShellButton(TEXT("Menu.Options"), ShellRect(610, Y, 380, 54), FGenShellData::Text(TEXT("GUI:Options"), TEXT("OPTIONS"))))
	{
		OpenOptions();
	}
	Y += Step;
	if (ShellButton(TEXT("Menu.Restart"), ShellRect(610, Y, 380, 54), FGenShellData::Text(TEXT("GUI:RestartGame"), TEXT("RESTART GAME")), !bNetwork))
	{
		GM->RestartGame();
	}
	Y += Step;
	if (ShellButton(TEXT("Menu.Exit"), ShellRect(610, Y, 380, 54), TEXT("EXIT TO MENU")))
	{
		GM->EndGame();
	}
	Y += Step;
	if (ShellButton(TEXT("Menu.Quit"), ShellRect(610, Y, 380, 54), TEXT("QUIT TO DESKTOP")))
	{
		Shell.bQuitConfirm = true;
	}
	Shell.bBlocked = false;

	if (Shell.bQuitConfirm)
	{
		DrawRect(FLinearColor(0.f, 0.f, 0.02f, 0.55f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
		ShellPanel(ShellRect(560, 330, 480, 230), 1.f);
		ShellTextEx(TEXT("QUIT TO DESKTOP?"), Canvas->SizeX * 0.5f, ShellRect(0, 360, 0, 0).Min.Y, ShellGold, 24.f, 1, true, 100);
		ShellText(bNetwork ? TEXT("You leave the network game; the others play on.") : TEXT("The battle in progress will be lost."),
			Canvas->SizeX * 0.5f, ShellRect(0, 412, 0, 0).Min.Y, ShellTextColor, 17.f, 1);
		if (ShellButton(TEXT("Menu.QuitNo"), ShellRect(596, 476, 196, 52), FGenShellData::Text(TEXT("GUI:Cancel"), TEXT("CANCEL"))))
		{
			Shell.bQuitConfirm = false;
		}
		else if (ShellPrimaryButton(TEXT("Menu.QuitYes"), ShellRect(808, 476, 196, 52), TEXT("QUIT")))
		{
			if (UGenNetSubsystem* Net = UGenNetSubsystem::Get(this); Net && Net->IsInNetworkGame())
			{
				Net->Leave();
			}
			UE_LOG(LogGenerals, Log, TEXT("Shell: quit to the desktop from the in-game menu"));
			UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, false);
		}
	}
}

void AGenHUD::DrawScoreScreen(AGenGameMode* GM, UGenGameLogic* Logic)
{
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FGenPlayerTemplate* LocalSide = Players.IsValidIndex(Local) ? Players[Local].Template : nullptr;
	const FGenMappedImage* Img = nullptr;
	if (LocalSide && FindShellTexture(LocalSide->ScoreScreenImage, Img))
	{
		ShellBackdrop(*LocalSide->ScoreScreenImage, 0.62f);
	}
	else
	{
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.85f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	}
	const float S = Shell.Scale;

	const bool bWon = Logic->HasLocalPlayerWon();
	const bool bLost = Logic->IsPlayerDefeated(Local);
	const FString Title = bWon ? FGenShellData::Text(TEXT("GUI:Victory"), TEXT("Victory"))
		: (bLost ? FGenShellData::Text(TEXT("GUI:Defeat"), TEXT("Defeat")) : FGenShellData::Text(TEXT("GUI:Score"), TEXT("Score")));
	const FLinearColor TitleColor = bWon ? ShellGold : (bLost ? ShellRed : ShellTextColor);
	ShellTextEx(Title.ToUpper(), Canvas->SizeX * 0.5f, ShellRect(0, 40, 0, 0).Min.Y, TitleColor, 56.f, 1, true, 260);
	{
		const FBox2D Rule = ShellRect(500, 124, 600, 0);
		FillGradientH(Canvas, FBox2D(Rule.Min, FVector2D(Rule.GetCenter().X, Rule.Min.Y + 2.f * S)), WithAlpha(TitleColor, 0.f), WithAlpha(TitleColor, 0.9f));
		FillGradientH(Canvas, FBox2D(FVector2D(Rule.GetCenter().X, Rule.Min.Y), FVector2D(Rule.Max.X, Rule.Min.Y + 2.f * S)), WithAlpha(TitleColor, 0.9f), WithAlpha(TitleColor, 0.f));
	}
	if (const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this); Setup && Setup->GetMapInfo())
	{
		ShellTextEx(Setup->GetMapInfo()->DisplayName.ToUpper(), Canvas->SizeX * 0.5f, ShellRect(0, 136, 0, 0).Min.Y, ShellDim * 1.4f, 13.f, 1, true, 220);
	}

	// grabMultiPlayerInfo: everybody who played, the best score first.
	TArray<const FGenPlayer*> Rows;
	for (const FGenPlayer& P : Players)
	{
		if (!P.bNeutral && P.Template && P.StartPosition >= 0)
		{
			Rows.Add(&P);
		}
	}
	Rows.StableSort([](const FGenPlayer& A, const FGenPlayer& B) { return A.Score.Calculate(A.Index) > B.Score.Calculate(B.Index); });

	ShellPanel(ShellRect(90, 176, 1420, 120 + Rows.Num() * 56), 0.9f);
	// Three columns each for units and buildings, one for the supplies (the string file has their titles in two lines).
	const float Columns[] = { 660.f, 780.f, 900.f, 1050.f, 1170.f, 1290.f, 1470.f };
	const float GroupY = ShellRect(0, 194, 0, 0).Min.Y, HeadY = ShellRect(0, 226, 0, 0).Min.Y;
	const FString Supplies = FGenShellData::Text(TEXT("GUI:ResourcesCollected"), TEXT("Supplies \n Collected"));
	FString SuppliesTop, SuppliesRest;
	if (!Supplies.Split(TEXT("\\n"), &SuppliesTop, &SuppliesRest) && !Supplies.Split(TEXT("\n"), &SuppliesTop, &SuppliesRest))
	{
		SuppliesTop = Supplies;
	}
	ShellTextEx(FGenShellData::Text(TEXT("GUI:Units"), TEXT("Units")).ToUpper(), ShellRect(745, 0, 0, 0).Min.X, GroupY, ShellGold, 15.f, 1, true, 200);
	ShellTextEx(FGenShellData::Text(TEXT("GUI:Buildings"), TEXT("Buildings")).ToUpper(), ShellRect(1135, 0, 0, 0).Min.X, GroupY, ShellGold, 15.f, 1, true, 200);
	ShellTextEx(SuppliesTop.TrimStartAndEnd().ToUpper(), ShellRect(1470, 0, 0, 0).Min.X, GroupY, ShellGold, 15.f, 2, true, 200);
	// (A line under each group's title, over its three columns.)
	for (const float GroupX : { 590.f, 980.f })
	{
		const FBox2D Rule = ShellRect(GroupX, 218, 310, 0);
		DrawRect(WithAlpha(ShellGold, 0.35f), Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, S));
	}
	const FString Heads[] = {
		SecondLine(FGenShellData::Text(TEXT("GUI:UnitsBuilt"), TEXT("Created"))), SecondLine(FGenShellData::Text(TEXT("GUI:UnitsLost"), TEXT("Lost"))),
		SecondLine(FGenShellData::Text(TEXT("GUI:UnitsKilled"), TEXT("Destroyed"))), SecondLine(FGenShellData::Text(TEXT("GUI:BuildingsBuilt"), TEXT("Constructed"))),
		SecondLine(FGenShellData::Text(TEXT("GUI:BuildingsLost"), TEXT("Lost"))), SecondLine(FGenShellData::Text(TEXT("GUI:BuildingsKilled"), TEXT("Destroyed"))),
		SecondLine(Supplies) };
	for (int32 c = 0; c < UE_ARRAY_COUNT(Columns); ++c)
	{
		ShellText(Heads[c], ShellRect(Columns[c], 0, 0, 0).Min.X, HeadY, ShellDim * 1.3f, 15.f, 2);
	}
	{
		const FBox2D Rule = ShellRect(110, 254, 1380, 0);
		DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, S));
	}
	for (int32 r = 0; r < Rows.Num(); ++r)
	{
		const FGenPlayer& P = *Rows[r];
		const float RowY = 264.f + r * 56.f;
		const float Y = ShellRect(0, RowY + 10.f, 0, 0).Min.Y;
		FLinearColor Color = P.Color;
		Color.A = 1.f;
		// (Each row: a shade, the player's colour at the left, the local player's row lit.)
		const FBox2D Band = ShellRect(104, RowY, 1392, 50);
		if (P.Index == Local)
		{
			FillGradientH(Canvas, Band, FLinearColor(0.12f, 0.24f, 0.48f, 0.55f), FLinearColor(0.12f, 0.24f, 0.48f, 0.1f));
		}
		else if (r % 2 == 1)
		{
			DrawRect(FLinearColor(0.3f, 0.5f, 1.f, 0.04f), Band.Min.X, Band.Min.Y, Band.GetSize().X, Band.GetSize().Y);
		}
		DrawRect(Color, Band.Min.X, Band.Min.Y + 4.f * S, 4.f * S, Band.GetSize().Y - 8.f * S);
		const int32 SideIndex = FGenShellData::Get().FindSide(P.Template->Name);
		const FString Side = FGenShellData::Get().GetSides().IsValidIndex(SideIndex) ? FGenShellData::Get().GetSides()[SideIndex].Label : P.Template->Side;
		ShellImage(P.Template->SideIcon, ShellRect(118, RowY + 5.f, 40, 40));
		ShellText(P.Name, ShellRect(170, 0, 0, 0).Min.X, Y, Color, 22.f, 0, true);
		ShellText(Side, ShellRect(400, 0, 0, 0).Min.X, Y + 3.f * S, ShellDim * 1.3f, 17.f);
		const int32 Values[] = { P.Score.UnitsBuilt, P.Score.UnitsLost, P.Score.GetUnitsDestroyed(), P.Score.BuildingsBuilt, P.Score.BuildingsLost,
			P.Score.GetBuildingsDestroyed(), P.Score.MoneyEarned };
		for (int32 c = 0; c < UE_ARRAY_COUNT(Columns); ++c)
		{
			ShellText(FString::FromInt(Values[c]), ShellRect(Columns[c], 0, 0, 0).Min.X, Y, FLinearColor::White, 21.f, 2, P.Index == Local);
		}
	}

	ShellBottomBar();
	if (GM->CanKeepWatching() && ShellButton(TEXT("Score.Watch"), ShellRect(940, 812, 300, 56), FGenShellData::Text(TEXT("GUI:ReturnToGame"), TEXT("RETURN TO GAME"))))
	{
		GM->KeepWatching();
	}
	else if (ShellPrimaryButton(TEXT("Score.Continue"), ShellRect(1260, 812, 300, 56), FGenShellData::Text(TEXT("GUI:Continue"), TEXT("CONTINUE"))))
	{
		GM->ExitToMenu();
	}
}
