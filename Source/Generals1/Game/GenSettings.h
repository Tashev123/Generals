#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "GenSettings.generated.h"

/** Which mouse button does what (the options' controls page). */
UENUM()
enum class EGenMouseScheme : uint8
{
	Classic,    // the original's: left click selects and gives orders, right click deselects and drag-scrolls
	Modern,     // left click selects, right click gives orders (move, attack, ...)
};

/** Where the command bar's buttons take their keys from (the options' hotkeys page). */
UENUM()
enum class EGenHotkeyStyle : uint8
{
	Classic,    // the letter marked in each button's name in the game's text ("&Barracks": B)
	Grid,       // by the button's place on the bar: Q W E R T / A S D F G / Z X C V B
	Custom,     // Classic, with the player's own keys for the buttons they changed (ButtonKeys)
};

/** Scrolling and the mouse. The speeds are multipliers of the game's own (1 = as it was). */
USTRUCT()
struct FGenControlSettings
{
	GENERATED_BODY()

	UPROPERTY(config) float EdgeScrollSpeed = 1.f;
	UPROPERTY(config) float KeyScrollSpeed = 1.f;
	UPROPERTY(config) float DragScrollSpeed = 1.f;
	UPROPERTY(config) float ZoomSpeed = 1.f;
	UPROPERTY(config) float RotateSpeed = 1.f;
	UPROPERTY(config) bool bEdgeScroll = true;
	/** Edge scrolling also in a window (off: only in fullscreen, where the screen's edge is the window's). */
	UPROPERTY(config) bool bEdgeScrollWindowed = true;
	UPROPERTY(config) bool bInvertDragScroll = false;
	/** The mouse can't leave the game's window while a game is on. */
	UPROPERTY(config) bool bConfineMouse = false;
	UPROPERTY(config) EGenMouseScheme MouseScheme = EGenMouseScheme::Classic;
	/** The pointer's own speed (1 = Windows' speed, untouched). */
	UPROPERTY(config) float CursorSpeed = 1.f;
};

/** The keys. Bindings: "Action=Key" or "Action=Ctrl+Shift+Alt+Key" (GenInput's action and key names);
 *  ButtonKeys: "CommandButtonName=Key" (Custom style). What isn't listed has its default. */
USTRUCT()
struct FGenHotkeySettings
{
	GENERATED_BODY()

	UPROPERTY(config) EGenHotkeyStyle Style = EGenHotkeyStyle::Classic;
	UPROPERTY(config) TArray<FString> Bindings;
	UPROPERTY(config) TArray<FString> ButtonKeys;
};

/** Volumes, 0..1 (AudioSettings.ini's defaults are the game's; these scale them). */
USTRUCT()
struct FGenVolumeSettings
{
	GENERATED_BODY()

	UPROPERTY(config) float Master = 1.f;
	UPROPERTY(config) float Music = 1.f;
	UPROPERTY(config) float Sound = 1.f;     // world and interface sounds
	UPROPERTY(config) float Voice = 1.f;     // the units' voices
	UPROPERTY(config) float Speech = 1.f;    // the adviser (EVA)
};

/** Video settings the engine's own GameUserSettings don't have. */
USTRUCT()
struct FGenVideoExtras
{
	GENERATED_BODY()

	UPROPERTY(config) bool bRayTracing = false;
	UPROPERTY(config) bool bShowFPS = false;
	/** The game's video defaults (SetVideoDefaults) have been put over the engine's (GenOptionsVideo.cpp): a
	 *  first start, or a settings file from before they existed, gets them once. */
	UPROPERTY(config) int32 DefaultsVersion = 0;
};

/** The video page while it is being changed (the engine's settings are only touched on Apply). */
struct FGenVideoDraft
{
	FIntPoint Resolution = FIntPoint(1920, 1080);
	int32 WindowMode = 0;              // EWindowMode: 0 fullscreen, 1 borderless (windowed fullscreen), 2 windowed
	bool bVSync = true;
	float FrameRateLimit = 60.f;       // 0: none
	int32 Preset = 2;                  // 0 low .. 3 epic, 4 custom
	int32 Shadows = 2, Textures = 2, Effects = 2, ViewDistance = 2, AntiAliasing = 2, PostProcess = 2, Foliage = 2, Shading = 2;
	float ResolutionScale = 100.f;     // percent
	FGenVideoExtras Extras;
};

/**
 * The player's settings (Saved/Config/Windows/GameUserSettings.ini): the engine's own (resolution, window mode,
 * frame rate limit, VSync, quality) and the game's - controls, keys, volumes. Changed by the options screen
 * (GenOptions*.cpp); everything else reads them through Get().
 */
UCLASS(config = GameUserSettings, configdonotcheckdefaults)
class GENERALS1_API UGenUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	UGenUserSettings();

	/** The engine's settings object (GameUserSettingsClassName in DefaultEngine.ini); nullptr before the engine is up. */
	static UGenUserSettings* Get();

	UPROPERTY(config) FGenControlSettings Controls;
	UPROPERTY(config) FGenHotkeySettings Hotkeys;
	UPROPERTY(config) FGenVolumeSettings Audio;
	UPROPERTY(config) FGenVideoExtras VideoExtras;

	virtual void SetToDefaults() override;
	virtual void ApplyNonResolutionSettings() override;

	/** At the start of the game: everything stored put into effect (video, volumes, keys). */
	void ApplyAtStartup();

	// Each part puts itself into effect in its own file.
	/** The video settings the engine doesn't know (GenOptionsVideo.cpp). */
	void ApplyVideoExtras();
	/** The video defaults this game wants, on top of the engine's (GenOptionsVideo.cpp). */
	void SetVideoDefaults();
	/** The volumes (GenOptionsAudio.cpp). */
	void ApplyAudio();
};
