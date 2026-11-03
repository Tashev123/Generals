// What a campaign map's scripts show over the view: the screen fade (ScriptEngine's CAMERA_FADE_*), the letterbox
// (W3DDisplay's letterbox bars) and the military caption (InGameUI::militarySubtitle).

#include "Game/GenHUD.h"
#include "Game/GenGameMode.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenMapScripts.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"

namespace
{
	constexpr double LetterboxSeconds = 1.0;     // the bars slide in and out over a second
	constexpr float LetterboxFraction = 0.12f;   // each bar: an eighth of the screen, near enough
	constexpr float CaptionDelayMs = 750.f;      // Language.ini MilitaryCaptionDelayMS: before the first line, and each next one
	constexpr float CaptionSpeedFrames = 1.f;    // MilitaryCaptionSpeed: a character a logic frame
	constexpr float CaptionX = 10.f, CaptionY = 340.f, CaptionPoints = 12.f; // InGame.ini, on an 800 x 600 screen
}

void AGenHUD::OnScriptEvent(const FGenScriptClientEvent& E, UGenGameLogic* Logic)
{
	const double Now = GetWorld()->GetRealTimeSeconds();
	switch (E.Action)
	{
	case EGenScriptAction::A_CAMERA_LETTERBOX_BEGIN:
	case EGenScriptAction::A_CAMERA_LETTERBOX_END:
	{
		const bool bOn = E.Action == EGenScriptAction::A_CAMERA_LETTERBOX_BEGIN;
		if (Cine.bLetterbox != bOn)
		{
			// (From wherever the bars are now: a change of mind half way doesn't jump.)
			const double Done = FMath::Clamp((Now - Cine.LetterboxChanged) / LetterboxSeconds, 0.0, 1.0);
			Cine.LetterboxChanged = Now - (1.0 - Done) * LetterboxSeconds;
			Cine.bLetterbox = bOn;
		}
		break;
	}
	case EGenScriptAction::A_SHOW_MILITARY_CAPTION:
		// InGameUI::militarySubtitle: the text types itself out, then stands for the given time from its start.
		Cine.Caption = FGenCommandData::Get().GetText(E.Str(0));
		Cine.Caption.ReplaceInline(TEXT("\r"), TEXT(""));
		Cine.CaptionStart = Logic ? Logic->GetRenderFrameTime() : 0.0;
		Cine.CaptionFrames = float(E.Int(1)) * float(GenLogic::GetLogicFPS()) / 1000.f;
		Cine.CaptionTyped = 0;
		break;
	default:
		break;
	}
}

void AGenHUD::DrawScriptFade(UGenGameLogic* Logic)
{
	const FGenMapScripts* Scripts = Logic->GetMapScripts();
	if (!Scripts || Scripts->GetFade().Type == FGenScriptFade::None)
	{
		return;
	}
	const FGenScriptFade& Fade = Scripts->GetFade();
	const float V = FMath::Clamp(Fade.Value, 0.f, 1.f);
	FCanvasTileItem Tile(FVector2D::ZeroVector, FVector2D(Canvas->SizeX, Canvas->SizeY), FLinearColor::Black);
	switch (Fade.Type)
	{
	case FGenScriptFade::Multiply:
		// The view times V: black over it, 1 - V of it.
		Tile.SetColor(FLinearColor(0.f, 0.f, 0.f, 1.f - V));
		Tile.BlendMode = SE_BLEND_Translucent;
		break;
	case FGenScriptFade::Add:
		Tile.SetColor(FLinearColor(V, V, V, 1.f));
		Tile.BlendMode = SE_BLEND_Additive;
		break;
	case FGenScriptFade::Subtract:
		// (No subtracting blend on the canvas: darken by as much.)
		Tile.SetColor(FLinearColor(0.f, 0.f, 0.f, V));
		Tile.BlendMode = SE_BLEND_Translucent;
		break;
	default:
		return;
	}
	Canvas->DrawItem(Tile);
}

void AGenHUD::DrawLetterbox()
{
	const double Done = FMath::Clamp((GetWorld()->GetRealTimeSeconds() - Cine.LetterboxChanged) / LetterboxSeconds, 0.0, 1.0);
	const float Amount = float(Cine.bLetterbox ? Done : 1.0 - Done);
	if (Amount <= 0.f)
	{
		return;
	}
	const float H = Canvas->SizeY * LetterboxFraction * Amount;
	DrawRect(FLinearColor::Black, 0.f, 0.f, Canvas->SizeX, H);
	DrawRect(FLinearColor::Black, 0.f, Canvas->SizeY - H, Canvas->SizeX, H);
}

void AGenHUD::DrawIntroSkip(const AGenGameMode* GM)
{
	if (!GM)
	{
		return;
	}
	const float ScreenScale = Canvas->SizeY / 600.f;
	const float Size = CaptionPoints * ScreenScale / FMath::Max(Shell.Scale, 0.01f);
	if (GM->IsSkippingIntro())
	{
		DrawRect(FLinearColor::Black, 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
		const FString Text(TEXT("Skipping..."));
		ShellTextEx(Text, (Canvas->SizeX - ShellMeasure(Text, Size, false).X) * 0.5f, Canvas->SizeY * 0.5f, FLinearColor::White, Size, 0, false, 0, true);
		return;
	}
	const float Hold = GM->GetIntroSkipHold();
	if (Hold < 0.f)
	{
		return;
	}
	// In the bottom bar, out of the way of the picture; the line under it fills while the key is held.
	const FString Text(TEXT("Hold SPACE to skip"));
	const FVector2D Measure = ShellMeasure(Text, Size, false);
	const float X = Canvas->SizeX - Measure.X - 20.f * ScreenScale;
	const float Y = Canvas->SizeY - Measure.Y - 16.f * ScreenScale;
	ShellTextEx(Text, X, Y, FLinearColor(1.f, 1.f, 1.f, 0.7f), Size, 0, false, 0, true);
	if (Hold > 0.f)
	{
		DrawRect(FLinearColor::White, X, Y + Measure.Y + 2.f * ScreenScale, Measure.X * FMath::Min(Hold, 1.f), FMath::Max(2.f * ScreenScale, 2.f));
	}
}

void AGenHUD::DrawMilitaryCaption(UGenGameLogic* Logic)
{
	if (Cine.Caption.IsEmpty())
	{
		return;
	}
	// Typed a character a frame after a pause, and a pause again at each new line; a block cursor blinks at the end
	// of what is typed (every 9 frames) until all of it is. Past its time the text fades away.
	const float Elapsed = float(Logic->GetRenderFrameTime() - Cine.CaptionStart);
	const float Delay = CaptionDelayMs * float(GenLogic::GetLogicFPS()) / 1000.f;
	float Clock = Delay;
	int32 Typed = 0;
	while (Typed < Cine.Caption.Len() && Clock < Elapsed)
	{
		Clock += Cine.Caption[Typed] == TEXT('\n') ? Delay : CaptionSpeedFrames;
		++Typed;
	}
	if (Typed > Cine.CaptionTyped)
	{
		if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
		{
			Audio->PlayUI(TEXT("MilitarySubtitlesTyping"));
		}
		Cine.CaptionTyped = Typed;
	}
	float Alpha = 1.f;
	const float Over = Elapsed - Cine.CaptionFrames;
	if (Over > 0.f)
	{
		// (The original takes an ever bigger bite of the alpha each frame: n / 10 of 255 at the n-th.)
		Alpha = 1.f - 0.05f * Over * Over / 255.f;
		if (Alpha <= 0.f)
		{
			Cine.Caption.Reset();
			return;
		}
	}
	TArray<FString> Lines;
	Cine.Caption.Left(Typed).ParseIntoArray(Lines, TEXT("\n"), false);
	if (Lines.Num() == 0)
	{
		Lines.Add(FString());
	}
	const float ScreenScale = Canvas->SizeY / 600.f;
	const float Size = CaptionPoints * ScreenScale / FMath::Max(Shell.Scale, 0.01f);
	const float X = CaptionX * Canvas->SizeX / 800.f; // (InGameUI::militarySubtitle: "m_militaryCaptionPosition" scaled by the display's width)
	float Y = CaptionY * ScreenScale;
	const FLinearColor Color(1.f, 1.f, 1.f, Alpha);
	const float LineHeight = ShellMeasure(TEXT("Ag"), Size, true).Y;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		ShellTextEx(Lines[i], X, Y, Color, Size, 0, i == 0, 0, true);
		if (i == Lines.Num() - 1 && Typed < Cine.Caption.Len() && (int32(Elapsed) / 9) % 2 == 0)
		{
			const float W = ShellMeasure(Lines[i], Size, i == 0).X;
			DrawRect(Color, X + W + 1.f, Y + LineHeight * 0.1f, LineHeight * 0.5f, LineHeight * 0.8f);
		}
		Y += LineHeight;
	}
}
