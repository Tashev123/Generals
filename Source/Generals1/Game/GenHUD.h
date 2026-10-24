#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
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
	bool ShellButton(const FString& Id, const FBox2D& R, const FString& Label, bool bEnabled = true);
	bool ShellCheckbox(const FString& Id, const FBox2D& R, const FString& Label, bool bValue);
	/** A drop-down list: true when another item was chosen (OutPicked). A test id "Id=Item label" chooses directly. */
	bool ShellDropdown(const FString& Id, const FBox2D& R, const TArray<FString>& Items, const TArray<FLinearColor>& Colors, int32 Current, int32& OutPicked);
	UTexture2D* GetMapPreview(const FString& MapName);

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
