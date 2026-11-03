#include "Game/GenHudStyle.h"
#include "Game/GenSettings.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<FString> CVarGenHudStyle(
	TEXT("gen.HudStyle"), TEXT(""),
	TEXT("The HUD style's id, over what the options say (empty: as they say)."));

const TArray<const FGenHudStyle*>& GenHudStyles::All()
{
	static const TArray<const FGenHudStyle*> List = []
	{
		TArray<const FGenHudStyle*> L;
		// (The first is the default. A new style goes here.)
		AddMetalStyles(L);
		return L;
	}();
	return List;
}

const FGenHudStyle* GenHudStyles::Find(const FString& Id)
{
	const FGenHudStyle* const* Found = All().FindByPredicate([&Id](const FGenHudStyle* S) { return S->Id.Equals(Id, ESearchCase::IgnoreCase); });
	return Found ? *Found : nullptr;
}

const FGenHudStyle& GenHudStyles::Current()
{
	const FString Override = CVarGenHudStyle.GetValueOnGameThread();
	const FGenHudStyle* Style = Override.IsEmpty() ? nullptr : Find(Override);
	if (!Style)
	{
		const UGenUserSettings* Settings = UGenUserSettings::Get();
		Style = Settings ? Find(Settings->VideoExtras.HudStyle) : nullptr;
	}
	return Style ? *Style : *All()[0];
}
