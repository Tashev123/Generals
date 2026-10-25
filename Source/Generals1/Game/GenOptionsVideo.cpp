// The options' video page: resolution, window mode, VSync, frame rate limit, quality, resolution scale.
// Ids for test automation: see GenOptions.cpp; this page's own are "Video.*".

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"

using namespace GenShellStyle;

void UGenUserSettings::ApplyVideoExtras()
{
}

void UGenUserSettings::SetVideoDefaults()
{
	// A laptop runs hot at an unlimited frame rate: the game's default is 60 with VSync.
	FrameRateLimit = 60.f;
	bUseVSync = true;
}

void AGenHUD::OptionsLoadVideo()
{
}

void AGenHUD::DrawOptionsVideo(const FBox2D& Body)
{
	const FBox2D R = ShellRect(Body.Min.X, Body.Min.Y, 0, 0);
	ShellText(TEXT("Video settings"), R.Min.X, R.Min.Y, ShellTextColor, 20.f);
}

void AGenHUD::OptionsApplyVideo()
{
}

void AGenHUD::OptionsDefaultsVideo()
{
}

void AGenHUD::OptionsCancelVideo()
{
}

void AGenHUD::DrawVideoOverlay()
{
}
