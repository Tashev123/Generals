// The options' controls page: scroll and zoom speeds, edge scrolling, the mouse buttons' layout, the pointer.
// Ids for test automation: see GenOptions.cpp; this page's own are "Controls.*".

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"

using namespace GenShellStyle;

void AGenHUD::OptionsLoadControls()
{
	if (const UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Options.Controls = Settings->Controls;
	}
}

void AGenHUD::DrawOptionsControls(const FBox2D& Body)
{
	const FBox2D R = ShellRect(Body.Min.X, Body.Min.Y, 0, 0);
	ShellText(TEXT("Control settings"), R.Min.X, R.Min.Y, ShellTextColor, 20.f);
}

void AGenHUD::OptionsApplyControls()
{
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->Controls = Options.Controls;
	}
}

void AGenHUD::OptionsDefaultsControls()
{
	Options.Controls = FGenControlSettings();
}

void AGenHUD::OptionsCancelControls()
{
}
