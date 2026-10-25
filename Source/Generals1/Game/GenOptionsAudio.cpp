// The options' audio page: the volumes.
// Ids for test automation: see GenOptions.cpp; this page's own are "Audio.*".

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"

using namespace GenShellStyle;

void UGenUserSettings::ApplyAudio()
{
}

void AGenHUD::OptionsLoadAudio()
{
	if (const UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Options.Audio = Settings->Audio;
	}
}

void AGenHUD::DrawOptionsAudio(const FBox2D& Body)
{
	const FBox2D R = ShellRect(Body.Min.X, Body.Min.Y, 0, 0);
	ShellText(TEXT("Audio settings"), R.Min.X, R.Min.Y, ShellTextColor, 20.f);
}

void AGenHUD::OptionsApplyAudio()
{
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->Audio = Options.Audio;
		Settings->ApplyAudio();
	}
}

void AGenHUD::OptionsDefaultsAudio()
{
	Options.Audio = FGenVolumeSettings();
}

void AGenHUD::OptionsCancelAudio()
{
}
