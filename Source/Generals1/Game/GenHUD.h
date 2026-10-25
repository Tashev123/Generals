#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Game/GenInput.h"
#include "Game/GenSettings.h"
#include "GenHUD.generated.h"

class AGenGameMode;
class AGenPlayerController;
class FGenMapData;
class FGenNetClient;
struct FGenSetupData;
class UGenGameLogic;
class UTexture2D;
struct FGenBarButton;
struct FGenMapInfo;

/**
 * Immediate-mode in-game overlay: drag-selection box, selection circles and health bars
 * (InGameUI selection decoration), money and power, the command bar with its production queue and
 * tooltips, the general's promotions, the special power shortcuts and their targeting circle, and the
 * footprint of a building being placed, and the radar (W3DRadar::draw). A stand-in for the original's
 * WND control bar.
 *
 * It also draws the shell (GenShell.cpp): the skirmish menu with its map selection, the in-game menu
 * and the score screen - stand-ins for SkirmishGameOptionsMenu.wnd, QuitMenu.wnd and ScoreScreen.wnd
 * that offer what those offer, from the same data.
 */
UCLASS()
class GENERALS1_API AGenHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

	/** Test automation: the interface element with this id counts as clicked the next time it is drawn. */
	void ShellTestClick(const FString& Id);
	bool HasShellTestClick() const { return !Shell.TestClick.IsEmpty(); }

	/** The options screen (GenOptions.cpp): over the main menu or the in-game menu. While it is up it has the keys. */
	void OpenOptions();
	bool IsOptionsOpen() const { return Options.bOpen; }

private:
	void DrawGroundCircle(const FVector& CenterUE, float RadiusUE, const FLinearColor& Color);
	void DrawPlacement(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawControlBar(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawPowerBar(const class FGenControlBar& Bar, UGenGameLogic* Logic);
	void DrawExpBar(const class FGenControlBar& Bar, UGenGameLogic* Logic);
	/** A mapped image stretched over a screen rectangle; false: there is no such image. */
	bool DrawMappedImage(const FString& Name, const FBox2D& R, const FLinearColor& Tint = FLinearColor::White);
	/** W3DDisplay::drawRemainingRectClock: the part not done yet, swept clockwise from twelve o'clock. */
	void DrawRemainingClock(const FBox2D& R, float Done, const FLinearColor& Color);
	/** A line of text in a window's rectangle at a font size in pixels. */
	void DrawBarText(const FString& Text, const FBox2D& R, const FLinearColor& Color, float PixelSize, bool bCenter);
	void DrawGenerals(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawTargeting(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawAnnouncements(UGenGameLogic* Logic);
	/** InGameUI's superweapon countdowns (every player's PublicTimer powers), and the adviser's "ready". */
	void DrawSuperweaponTimers(UGenGameLogic* Logic);
	TMap<uint64, bool> SuperweaponEvaPlayed;   // SuperweaponInfo::m_evaReadyPlayed, by object and power module
	void DrawVeterancy(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawGameOver(UGenGameLogic* Logic);
	void DrawBarButton(const FGenBarButton& B, bool bHovered, float Scale, UGenGameLogic* Logic, const struct FGenControlBarScheme* Scheme);
	void DrawTooltip(const FGenBarButton& B, const FVector2D& Mouse, float Scale, UGenGameLogic* Logic);
	void DrawRadar(AGenPlayerController* PC, UGenGameLogic* Logic);
	/** InGameUI::createMouseoverHint: the name of what the mouse has rested on for a moment. */
	void DrawMouseOverName(AGenPlayerController* PC, UGenGameLogic* Logic, const FVector2D& Mouse);
	/** A test run has no mouse: the cursor it would show, drawn into the picture where it would be. */
	void DrawTestCursor(const FVector2D& Pos);
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> CursorTextures;

	// --- the shell (GenShell.cpp) ---
	struct FShellState
	{
		// this frame's input, in pixels
		FVector2D Mouse = FVector2D(-10000.0, -10000.0);
		bool bClick = false;
		bool bRightClick = false;
		float Wheel = 0.f;
		bool bBlocked = false;          // what is being drawn lies under a dialog: it doesn't react
		bool bUnderOptions = false;     // ... under the options screen (set for everything but the options themselves)
		bool bMouseDown = false;        // the left button is held
		FString DragId;                 // the slider (or scroll bar) the left button is dragging
		FString Tooltip;                // the text of what the mouse is on, drawn last (ShellTooltip)
		// layout: the design is 1600 x 900, scaled to fit and centred
		float Scale = 1.f;
		FVector2D Origin = FVector2D::ZeroVector;
		// the open drop-down list, drawn last and on top
		FString OpenId;
		FBox2D OpenRect = FBox2D(ForceInit);
		TArray<FString> OpenItems;
		TArray<FLinearColor> OpenColors;
		int32 OpenCurrent = INDEX_NONE;
		FString PickedId;               // a choice made in the list, for its owner to collect
		int32 PickedIndex = INDEX_NONE;
		// the map selection dialog
		bool bMapSelect = false;
		FString MapChoice;
		float MapScroll = 0.f;
		bool bEditingName = false;      // the player's name has the keyboard
		bool bEditingAddress = false;   // ... the network game's address has it
		FString JoinAddress;            // what has been typed there
		bool bJoinAddressLoaded = false;
		// test automation
		FString TestClick;
		int32 TestClickFrames = 0;
	};
	/** The setup the menu shows: the skirmish's, or (a network game) a copy of the host's lobby that this frame's
	 *  clicks change and that is then sent to the host. */
	FGenSetupData* ShellSetup = nullptr;
	/** Keys typed this frame into a line of text (letters, digits, space, - . : _), or Ctrl+V for the clipboard;
	 *  Enter or Esc: bOutDone. */
	void ShellTypeInto(FString& Text, int32 MaxLen, bool& bOutDone);
	/** A box of text that can't be changed here (another player's choice). */
	void ShellStatic(const FBox2D& R, const FString& Text, const FLinearColor& Color);
	/** Before the lobby is there: who is being reached, why not. */
	void DrawNetWaiting(AGenGameMode* GM, FGenNetClient* Client);
	/** During a network game: lost connection, players dropping out, the games coming apart. */
	void DrawNetOverlay(AGenGameMode* GM, UGenGameLogic* Logic);

	/** Takes this frame's mouse state; settles a click on the open drop-down list. */
	void BeginShell(AGenPlayerController* PC);
	/** Draws the open drop-down list over everything. */
	void EndShell();
	void DrawSkirmishMenu(AGenGameMode* GM);
	void DrawMapSelect();
	void DrawMapPreview(const FGenMapInfo& Map, const FBox2D& Area, bool bInteractive);
	void DrawLoading();
	void DrawQuitMenu(AGenGameMode* GM);
	void DrawScoreScreen(AGenGameMode* GM, UGenGameLogic* Logic);

	FBox2D ShellRect(float X, float Y, float W, float H) const;
	bool ShellHover(const FBox2D& R) const;
	bool ShellTestHit(const FString& Id);
	void ShellClickSound();
	void ShellBackdrop(const TCHAR* Image, float Darken);
	bool ShellImage(const FString& Image, const FBox2D& R, const FLinearColor& Tint = FLinearColor::White);
	void ShellPanel(const FBox2D& R, float Alpha = 0.78f);
	/** Align: 0 left, 1 centred on X, 2 right. Size is in design units (about a point size). */
	void ShellText(const FString& Text, float X, float Y, const FLinearColor& Color, float Size, int32 Align = 0, bool bBold = false);
	/** The size on the screen (pixels) of a line of ShellText at that size. */
	FVector2D ShellMeasure(const FString& Text, float Size, bool bBold = false) const;
	bool ShellButton(const FString& Id, const FBox2D& R, const FString& Label, bool bEnabled = true);
	bool ShellCheckbox(const FString& Id, const FBox2D& R, const FString& Label, bool bValue);
	/** A drop-down list: true when another item was chosen (OutPicked). A test id "Id=Item label" chooses directly. */
	bool ShellDropdown(const FString& Id, const FBox2D& R, const TArray<FString>& Items, const TArray<FLinearColor>& Colors, int32 Current, int32& OutPicked);
	UTexture2D* GetMapPreview(const FString& MapName);

	// --- more elements (GenShell.cpp). Rectangles are on the screen (ShellRect); test ids as noted. ---
	/** A row of tabs: the one now chosen. Test id "Id=Label". */
	int32 ShellTabs(const FString& Id, const FBox2D& R, const TArray<FString>& Labels, int32 Current);
	/** A slider: clicked or dragged, Value goes Min..Max in Steps (0: smooth); true when it changed. ValueText is
	 *  shown at its right. Test id "Id=0.75" sets the value. */
	bool ShellSlider(const FString& Id, const FBox2D& R, float& Value, float Min, float Max, float Step, const FString& ValueText);
	/** A button showing a key ("Ctrl + F"), lit while it waits for one (bWaiting). True when clicked. */
	bool ShellKeyButton(const FString& Id, const FBox2D& R, const FString& Text, bool bWaiting);
	/**
	 * While a key button waits: true when a key went down this frame (with the modifiers held then) - Out is it;
	 * Esc: bOutCancelled. Modifier keys alone don't count. Test id "Key=Ctrl+F" (FGenKeyChord's form) presses
	 * that, "Key=Escape" cancels.
	 */
	bool ShellCaptureKey(FGenKeyChord& Out, bool& bOutCancelled);
	/**
	 * A scrolling area: the wheel over it (or its bar, dragged) moves Scroll, in design units, kept within
	 * 0 .. ContentHeight - its height. Draws the bar when the content is taller. The caller draws its rows at
	 * minus Scroll and leaves out those not wholly inside R. Test id "Id=Top" / "Id=Bottom".
	 */
	void ShellScrollArea(const FString& Id, const FBox2D& R, float ContentHeight, float& Scroll);
	/** The text shown by the mouse while it is over R (drawn by EndShell, over everything). */
	void ShellTooltip(const FBox2D& R, const FString& Text);

	// --- the options screen (GenOptions.cpp: its frame; a file for each page) ---
	struct FOptionsState
	{
		bool bOpen = false;
		int32 Tab = 0;                  // EOptionsTab
		// each page's copy of the settings, changed freely, put into effect by Apply
		FGenVideoDraft Video;
		FGenControlSettings Controls;
		FGenHotkeySettings Hotkeys;
		FGenVolumeSettings Audio;
		float Scroll[4] = { 0.f, 0.f, 0.f, 0.f };
		FString CaptureId;              // the key button waiting for a key, if any
	};
	enum EOptionsTab : int32 { OptionsVideo, OptionsControls, OptionsHotkeys, OptionsAudio, OptionsTabCount };
	FOptionsState Options;
	void DrawOptions();
	void CloseOptions(bool bApply);
	/**
	 * Each page: Load fills its draft from the settings in effect; Draw draws it into Body (design units, the
	 * 1600 x 900 layout: ShellRect(Body.Min.X + x, ...)); Apply puts the draft into the settings (the frame
	 * saves them after); Defaults sets the draft to the defaults; Cancel undoes what the page tried out live.
	 */
	void OptionsLoadVideo();
	void DrawOptionsVideo(const FBox2D& Body);
	void OptionsApplyVideo();
	void OptionsDefaultsVideo();
	void OptionsCancelVideo();
	void OptionsLoadControls();
	void DrawOptionsControls(const FBox2D& Body);
	void OptionsApplyControls();
	void OptionsDefaultsControls();
	void OptionsCancelControls();
	void OptionsLoadHotkeys();
	void DrawOptionsHotkeys(const FBox2D& Body);
	void OptionsApplyHotkeys();
	void OptionsDefaultsHotkeys();
	void OptionsCancelHotkeys();
	void OptionsLoadAudio();
	void DrawOptionsAudio(const FBox2D& Body);
	void OptionsApplyAudio();
	void OptionsDefaultsAudio();
	void OptionsCancelAudio();
	/** Over the HUD while a game is on, options or not (GenOptionsVideo.cpp: the frame rate read-out). */
	void DrawVideoOverlay();

	FShellState Shell;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> MapPreviews;

	/** The radar's picture of the terrain, made once per map. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> RadarTerrain;

	const FGenMapData* RadarMap = nullptr;
	float RadarAverageZ = 0.f;

	/** What is on the radar, one texel per radar cell (W3DRadar's overlay texture): redrawn every other logic frame. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> RadarBlips;
	double RadarBlipsTime = -1000.0;
};
