// The options' hotkeys page: the command bar's key style, and the key of every command (GenInput).
// Ids for test automation: see GenOptions.cpp; this page's own are "Hotkeys.*".

#include "Game/GenHUD.h"
#include "Game/GenInput.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"

using namespace GenShellStyle;

void AGenHUD::OptionsLoadHotkeys()
{
	if (const UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Options.Hotkeys = Settings->Hotkeys;
	}
}

void AGenHUD::DrawOptionsHotkeys(const FBox2D& Body)
{
	const FBox2D R = ShellRect(Body.Min.X, Body.Min.Y, 0, 0);
	ShellText(TEXT("Hotkeys"), R.Min.X, R.Min.Y, ShellTextColor, 20.f);
}

void AGenHUD::OptionsApplyHotkeys()
{
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->Hotkeys = Options.Hotkeys;
		FGenInput::Get().Load(Settings->Hotkeys);
	}
}

void AGenHUD::OptionsDefaultsHotkeys()
{
	Options.Hotkeys = FGenHotkeySettings();
}

void AGenHUD::OptionsCancelHotkeys()
{
}
