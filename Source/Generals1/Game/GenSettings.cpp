#include "Game/GenSettings.h"
#include "Game/GenInput.h"
#include "Engine/Engine.h"

UGenUserSettings::UGenUserSettings()
{
}

UGenUserSettings* UGenUserSettings::Get()
{
	return GEngine ? Cast<UGenUserSettings>(GEngine->GetGameUserSettings()) : nullptr;
}

void UGenUserSettings::SetToDefaults()
{
	Super::SetToDefaults();
	Controls = FGenControlSettings();
	Hotkeys = FGenHotkeySettings();
	Audio = FGenVolumeSettings();
	VideoExtras = FGenVideoExtras();
	SetVideoDefaults();
}

void UGenUserSettings::ApplyNonResolutionSettings()
{
	Super::ApplyNonResolutionSettings();
	ApplyVideoExtras();
	ApplyAudio();
	FGenInput::Get().Load(Hotkeys);
}

void UGenUserSettings::ApplyAtStartup()
{
	ApplyVideoExtras();
	ApplyAudio();
	FGenInput::Get().Load(Hotkeys);
}
