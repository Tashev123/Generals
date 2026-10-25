// The options' audio page: the volumes - master, music, sound effects, the units' voices and the adviser's
// speech - heard as they are dragged (the audio manager plays by the draft until Apply or Cancel).
// Ids for test automation: see GenOptions.cpp; this page's own are "Audio.*":
//   Audio.Master, Audio.Music, Audio.Sound, Audio.Voice, Audio.Speech   the sliders ("Audio.Music=0.5" sets one)

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"
#include "Audio/GenAudioManager.h"
#include "Generals1.h"

using namespace GenShellStyle;

namespace
{
	void SetVolumes(const FGenVolumeSettings& V)
	{
		UGenAudioManager::SetUserVolumes(V.Master, V.Music, V.Sound, V.Voice, V.Speech);
	}
}

void UGenUserSettings::ApplyAudio()
{
	// (The audio manager keeps them for the whole run; it puts them on every sound it plays.)
	SetVolumes(Audio);
	UE_LOG(LogGenerals, Log, TEXT("Audio: volumes master %.0f%%, music %.0f%%, sound %.0f%%, voices %.0f%%, speech %.0f%%"),
		Audio.Master * 100.f, Audio.Music * 100.f, Audio.Sound * 100.f, Audio.Voice * 100.f, Audio.Speech * 100.f);
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
	struct FRow
	{
		const TCHAR* Id;
		const TCHAR* Label;
		const TCHAR* Hint;
		float* Value;
	};
	FGenVolumeSettings& A = Options.Audio;
	const FRow Rows[] = {
		{ TEXT("Audio.Master"), TEXT("Master volume"), TEXT("Everything the game plays"), &A.Master },
		{ TEXT("Audio.Music"), TEXT("Music"), TEXT("The soundtrack of the menus and the battle"), &A.Music },
		{ TEXT("Audio.Sound"), TEXT("Sound effects"), TEXT("Weapons, engines, explosions and the interface"), &A.Sound },
		{ TEXT("Audio.Voice"), TEXT("Unit voices"), TEXT("Units answering their orders"), &A.Voice },
		{ TEXT("Audio.Speech"), TEXT("Speech (EVA)"), TEXT("The adviser's announcements"), &A.Speech },
	};

	const float X = Body.Min.X + 40.f, W = float(Body.GetSize().X) - 80.f;
	ShellSection(TEXT("VOLUME"), X, Body.Min.Y + 10.f, W);
	bool bChanged = false;
	float Y = Body.Min.Y + 70.f;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Rows); ++i)
	{
		const FRow& Row = Rows[i];
		ShellText(Row.Label, ShellRect(X, Y, 0, 0).Min.X, ShellRect(0, Y + 2.f, 0, 0).Min.Y, ShellTextColor, 20.f, 0, i == 0);
		ShellText(Row.Hint, ShellRect(X, Y, 0, 0).Min.X, ShellRect(0, Y + 30.f, 0, 0).Min.Y, ShellDim, 14.f);
		const FString Percent = FString::Printf(TEXT("%d%%"), FMath::RoundToInt32(*Row.Value * 100.f));
		bChanged |= ShellSlider(Row.Id, ShellRect(X + 400.f, Y + 6.f, W - 400.f, 40.f), *Row.Value, 0.f, 1.f, 0.01f, Percent);
		// (The master volume stands apart from the four it scales.)
		Y += i == 0 ? 92.f : 74.f;
		if (i == 0)
		{
			const FBox2D Rule = ShellRect(X, Y - 26.f, W, 0);
			DrawRect(ShellLine, Rule.Min.X, Rule.Min.Y, Rule.GetSize().X, FMath::Max(1.f, Shell.Scale));
		}
	}
	ShellText(TEXT("Changes are heard at once; Cancel puts the volumes back as they were."), ShellRect(X, 0, 0, 0).Min.X,
		ShellRect(0, Body.Max.Y - 30.f, 0, 0).Min.Y, ShellDim, 14.f);
	if (bChanged)
	{
		// Heard while dragging: the draft plays until Apply keeps it or Cancel drops it.
		SetVolumes(A);
		if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
		{
			Audio->ApplyUserVolumes();
		}
	}
}

void AGenHUD::OptionsApplyAudio()
{
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->Audio = Options.Audio;
		Settings->ApplyAudio();
	}
	if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
	{
		Audio->ApplyUserVolumes();
	}
}

void AGenHUD::OptionsDefaultsAudio()
{
	Options.Audio = FGenVolumeSettings();
	SetVolumes(Options.Audio);
	if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
	{
		Audio->ApplyUserVolumes();
	}
}

void AGenHUD::OptionsCancelAudio()
{
	// What was tried out goes: the volumes in effect are the saved ones again.
	if (const UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		SetVolumes(Settings->Audio);
		Options.Audio = Settings->Audio;
	}
	if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
	{
		Audio->ApplyUserVolumes();
	}
}
