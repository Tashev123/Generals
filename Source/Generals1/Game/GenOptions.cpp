// The options screen: its frame - the title, the tabs, Apply / Cancel / Defaults - over the main menu or the
// in-game menu. Each page is drawn by its own file (GenOptionsVideo.cpp, GenOptionsControls.cpp,
// GenOptionsHotkeys.cpp, GenOptionsAudio.cpp) from a copy of the settings ("the draft") that Apply puts into
// effect and saves; Cancel (or Esc) drops it.
//
// Ids for test automation (-GenTestMenu="seconds,click,Id;..."):
//   Options.Open                    opens it from wherever the shell is
//   Options.Tab=<Video|Controls|Hotkeys|Audio>
//   Options.Apply, Options.Cancel, Options.Defaults (the page shown)

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"
#include "Engine/Canvas.h"
#include "Generals1.h"

using namespace GenShellStyle;

void AGenHUD::OpenOptions()
{
	if (Options.bOpen)
	{
		return;
	}
	Options.bOpen = true;
	Options.CaptureId.Reset();
	for (float& S : Options.Scroll)
	{
		S = 0.f;
	}
	OptionsLoadVideo();
	OptionsLoadControls();
	OptionsLoadHotkeys();
	OptionsLoadAudio();
}

void AGenHUD::CloseOptions(bool bApply)
{
	if (!Options.bOpen)
	{
		return;
	}
	if (bApply)
	{
		OptionsApplyVideo();
		OptionsApplyControls();
		OptionsApplyHotkeys();
		OptionsApplyAudio();
		if (UGenUserSettings* Settings = UGenUserSettings::Get())
		{
			Settings->SaveSettings();
		}
		UE_LOG(LogGenerals, Log, TEXT("Options: applied and saved"));
	}
	else
	{
		OptionsCancelVideo();
		OptionsCancelControls();
		OptionsCancelHotkeys();
		OptionsCancelAudio();
	}
	Options.bOpen = false;
	Options.CaptureId.Reset();
	Shell.DragId.Reset();
	Shell.OpenId.Reset();
}

void AGenHUD::DrawOptions()
{
	if (!Options.bOpen && ShellTestHit(TEXT("Options.Open")))
	{
		OpenOptions();
		Shell.bUnderOptions = false; // (opened this frame: the rest of the screen was drawn before)
	}
	if (!Options.bOpen)
	{
		return;
	}
	Shell.bUnderOptions = false;

	// Everything else is behind a dark veil, and the panel itself is solid: nothing shows through it.
	DrawRect(FLinearColor(0.f, 0.004f, 0.015f, 0.8f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	ShellPanel(ShellRect(180, 60, 1240, 780), 1.f);
	ShellTextEx(TEXT("OPTIONS"), ShellRect(220, 0, 0, 0).Min.X, ShellRect(0, 84, 0, 0).Min.Y, ShellGold, 32.f, 0, true, 140);
	ShellTextEx(TEXT("ESC  CANCEL"), ShellRect(1380, 0, 0, 0).Min.X, ShellRect(0, 100, 0, 0).Min.Y, ShellDim * 1.3f, 12.f, 2, true, 160);

	static const TArray<FString> TabNames = { TEXT("Video"), TEXT("Controls"), TEXT("Hotkeys"), TEXT("Audio") };
	const int32 Tab = ShellTabs(TEXT("Options.Tab"), ShellRect(220, 150, 1160, 50), TabNames, Options.Tab);
	if (Tab != Options.Tab)
	{
		Options.Tab = Tab;
		Options.CaptureId.Reset(); // (a key button waiting on another page stops waiting)
	}

	// Esc: a key button that waits has it (the page sees it - asked before the page is drawn, which may stop the waiting); otherwise it is Cancel.
	const APlayerController* PC = GetOwningPlayerController();
	const bool bEsc = Options.CaptureId.IsEmpty() && PC && PC->WasInputKeyJustPressed(EKeys::Escape);

	// The page: 1140 x 520 design units under the tabs.
	const FBox2D Body(FVector2D(230, 220), FVector2D(1370, 740));
	switch (Options.Tab)
	{
	case OptionsVideo: DrawOptionsVideo(Body); break;
	case OptionsControls: DrawOptionsControls(Body); break;
	case OptionsHotkeys: DrawOptionsHotkeys(Body); break;
	case OptionsAudio: DrawOptionsAudio(Body); break;
	default: break;
	}

	// The buttons under a line of their own.
	{
		const FBox2D Rule = ShellRect(220, 752, 1160, 0);
		DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, Shell.Scale));
	}
	if (ShellButton(TEXT("Options.Defaults"), ShellRect(220, 770, 260, 50), TEXT("RESET TO DEFAULTS")))
	{
		switch (Options.Tab)
		{
		case OptionsVideo: OptionsDefaultsVideo(); break;
		case OptionsControls: OptionsDefaultsControls(); break;
		case OptionsHotkeys: OptionsDefaultsHotkeys(); break;
		case OptionsAudio: OptionsDefaultsAudio(); break;
		default: break;
		}
	}
	if (ShellButton(TEXT("Options.Cancel"), ShellRect(940, 770, 210, 50), TEXT("CANCEL")) || bEsc)
	{
		CloseOptions(false);
	}
	else if (ShellPrimaryButton(TEXT("Options.Apply"), ShellRect(1170, 770, 210, 50), TEXT("APPLY")))
	{
		CloseOptions(true);
	}
}
