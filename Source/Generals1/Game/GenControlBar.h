#pragma once

#include "CoreMinimal.h"
#include "Game/GenInput.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenCommandData.h"

class AGenPlayerController;
class FGenObject;
class UGenGameLogic;
struct FGenCommandButton;
struct FGenThingTemplate;
struct FGenUpgradeTemplate;
struct FGenBarShape;
struct FGenControlBarScheme;
struct FGenWndLayout;
struct FGenWndWindow;

/** The bar's own buttons (ControlBar.wnd's, dressed by the scheme): what a click on one does. */
enum class EGenBarSystem : uint8
{
	None,
	Options,       // ButtonOptions: the in-game menu
	IdleWorker,    // ButtonIdleWorker: InGameUI::selectNextIdleWorker
	GeneralsExit,  // GeneralsExpPoints.wnd:ButtonExit
};

/** One clickable cell: a command button, an entry of the production queue, a promotion, a power shortcut. */
struct FGenBarButton
{
	FBox2D Rect = FBox2D(ForceInit);
	const FGenCommandButton* Button = nullptr;
	const FGenThingTemplate* Thing = nullptr;      // what the button builds, if anything
	const FGenUpgradeTemplate* Upgrade = nullptr;  // what the button researches (or the queue entry is)
	FString Science;                               // the promotion the button buys
	FString Power;                                 // the special power the button fires
	FGenObjectID Source = GenInvalidObjectID;      // the object that builds, researches or fires
	FGenObjectID Rider = GenInvalidObjectID;       // the occupant this button lets out of the container
	FString Image;                                 // mapped image to show instead of the button's own
	FString Label;                                 // tooltip title to show instead of the button's own
	FString TextLabel;                             // the button's name and description labels, when they aren't its own
	FString DescriptLabel;                         // (a power upgraded by a promotion: the fuel air bomb becomes the MOAB)
	bool bEnabled = false;
	bool bOwned = false;                           // already researched / acquired
	bool bGeneralsToggle = false;                  // opens and closes the promotions panel
	EGenBarSystem System = EGenBarSystem::None;    // one of the bar's own buttons
	FString Disabled;                              // why it can't be used right now
	int32 Cost = 0;                                // money for an upgrade, general's points for a promotion
	float Recharge = 0.f;                          // special power: share of its reload still to go
	uint32 ReadyFrame = 0;
	int32 QueueIndex = INDEX_NONE;                 // >= 0: production queue entry (click cancels it)
	int32 GridRow = INDEX_NONE;                    // the command window's cell it is in (ButtonCommand01..14), by
	int32 GridCol = INDEX_NONE;                    // ... row and column of ControlBar.wnd's layout
	FGenKeyChord Hotkey;                           // its key (GenInput's style; none: no key)
};

/** A text of a layout drawn as it stands (a static text window): where, what, how big. */
struct FGenBarText
{
	FBox2D Rect = FBox2D(ForceInit);
	FString Text;
	FColor Color = FColor::White;
	float Size = 1.f;                              // the window's font size in screen pixels
	bool bCentered = true;
};

/**
 * Layout of the in-game interface from the game's own layouts (ControlBar.cpp): ControlBar.wnd - the
 * selected object's CommandSet in ButtonCommand01..14, the production queue in ButtonQueue01..09 or the
 * selected unit's portrait and upgrades, the radar in LeftHUD, money and power, the general's button and
 * the bar's own buttons, dressed by the side's ControlBarScheme; the special power shortcuts in a rail of their
 * own; the promotions panel (GeneralsExpPoints.wnd). The 800 x 600 layouts are scaled evenly to the screen's
 * height (gen.ControlBarScale), the promotions panel centred. The bar itself is three sections standing apart
 * (FGenBarShape, the HUD style's): the radar, the commands and the right panel, side by side at the bottom of the
 * screen (gen.HudSpread); ControlBar.wnd's windows in the command window follow it (WindowRect). The options and idle
 * worker buttons are the bar's only own buttons: no beacon, no communicator and no minimize button, which never
 * did anything here.
 * The player controller hit-tests it, the HUD draws it.
 */
class FGenControlBar
{
public:
	void Layout(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection, const FVector2D& ViewSize);

	const FGenBarButton* HitTest(const FVector2D& P) const;
	/**
	 * The shown button on that key with exactly those modifiers (HotKeyTranslator: a button's key while it is shown):
	 * the open promotions panel's first, then the command window's, then the power shortcut tray's; one that can be
	 * used before one greyed out. None: nullptr.
	 */
	const FGenBarButton* FindHotkey(const FGenKeyChord& Pressed) const;
	/** The row and column of the command window's slot (0-based: ButtonCommand01 is 0) in ControlBar.wnd's layout:
	 *  its two rows of seven. False if the layout has no such button. */
	static bool GetGridCell(int32 Slot, int32& OutRow, int32& OutCol);
	/** Over any part of the interface: a click there never reaches the world. */
	bool IsOverPanel(const FVector2D& P) const;
	/** Whether that unit, on the player's side, has that button and can buy the upgrade for itself right now. */
	static bool CanBuyForItself(const UGenGameLogic* Logic, const FGenObject& Unit, const FGenCommandButton& Button, const FGenUpgradeTemplate& Upgrade);
	/** A rectangle of the 800 x 600 bar layouts on the screen. */
	FBox2D ToScreen(const FBox2D& R) const { return FBox2D(BarOrigin + R.Min * double(Scale), BarOrigin + R.Max * double(Scale)); }
	/** A rectangle in a section's own units (EGenBarPod, FGenBarShape) on the screen. */
	FBox2D ToPod(int32 Pod, const FBox2D& R) const { return FBox2D(PodOrigin[Pod] + R.Min * double(Scale), PodOrigin[Pod] + R.Max * double(Scale)); }

	float Scale = 1.f;                          // layout pixels -> screen pixels
	FVector2D BarOrigin = FVector2D::ZeroVector;  // where the layouts' (0, 0) lies on the screen
	FVector2D PodOrigin[3] = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };  // where each section's (0, 0) lies on the screen, on whole pixels
	const FGenControlBarScheme* Scheme = nullptr;
	const class FGenHudStyle* Style = nullptr;  // the look chosen in the options
	const FGenBarShape* Shape = nullptr;        // its sections on the scheme's side
	FBox2D LampRect = FBox2D(ForceInit);        // WinUAttack: the "under attack" light
	const FGenWndLayout* Wnd = nullptr;         // ControlBar.wnd

	bool bVisible = false;                      // the command window (something of ours with commands is selected)
	FBox2D Panel = FBox2D(ForceInit);           // the command window
	FGenObjectID Subject = GenInvalidObjectID;  // the object whose commands are shown
	FString Status;                             // e.g. "Under construction 42%"
	FBox2D StatusRect = FBox2D(ForceInit);
	float Progress = -1.f;                      // OCLTimerProgressBar: share done (below 0: no bar)
	FBox2D ProgressRect = FBox2D(ForceInit);
	TArray<FGenBarButton> Buttons;
	TArray<FGenBarButton> Queue;
	TArray<FBox2D> QueueSlots;                  // the queue's nine cells, used or not
	bool bShowQueue = false;                    // the right panel shows the production queue ...
	FBox2D PortraitRect = FBox2D(ForceInit);    // ... or the selected unit's portrait (CameoWindow)
	FString Portrait;                           // its SelectPortrait (none: the scheme's RightHUDImage over the panel)
	FBox2D RightHUDRect = FBox2D(ForceInit);
	TArray<FGenBarButton> UnitUpgrades;         // UnitUpgrade1..5: the upgrade cameos of the selected unit

	// Money, power, experience
	FBox2D MoneyRect = FBox2D(ForceInit);
	FBox2D PowerRect = FBox2D(ForceInit);
	FBox2D ExpRect = FBox2D(ForceInit);          // GeneralsExp: filled as the rank's experience grows

	// The bar's own buttons
	TArray<FGenBarButton> SystemButtons;
	int32 IdleWorkers = 0;

	// General's promotions
	bool bHasGenerals = false;
	FGenBarButton GeneralsButton;
	bool bGeneralsOpen = false;                 // kept between layouts; the controller toggles it
	// ControlBar::m_genStarFlash: the general's button flashes when promotion points come in, until the panel is
	// opened (even with points left) or they are spent.
	bool bGenStarFlash = true;
	int32 LastFlashedAtPoints = -1;
	int32 SeenPoints = 0;
	FBox2D GeneralsPanel = FBox2D(ForceInit);
	FString GeneralsImage;                      // the panel's picture (the scheme's PowerPurchaseImage)
	TArray<FGenBarButton> Sciences;
	TArray<FGenBarText> GeneralsTexts;
	FBox2D ExperienceRect = FBox2D(ForceInit);  // ProgressBarExperience

	// Special powers that can be fired without selecting their building (the shortcut tray)
	TArray<FGenBarButton> Shortcuts;
	FBox2D ShortcutRail = FBox2D(ForceInit);    // the rail they stand in

	// The radar window and the size of the map it shows
	FBox2D Radar = FBox2D(ForceInit);
	/** Opens the in-game menu (as Esc does): the bar's options button. */
	FBox2D MenuButton = FBox2D(ForceInit);
	FVector2f RadarMapSize = FVector2f::ZeroVector;

private:
	/** The selection's command set without the buttons a map's scripts have taken out (COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE). */
	FGenCommandSet ScriptFilteredSet;
	void LayoutSelection(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection);
	void LayoutRightPanel(UGenGameLogic* Logic, const FGenObject* Obj);
	void LayoutGenerals(UGenGameLogic* Logic);
	void LayoutShortcuts(UGenGameLogic* Logic, const FVector2D& ViewSize);
	void LayoutSystemButtons(UGenGameLogic* Logic);
	/** Every button's key (FGenBarButton::Hotkey), after the layout. */
	void AssignHotkeys();
	/** Fills in a special power button; false if it should not be shown at all. */
	bool FillPower(UGenGameLogic* Logic, const FGenObject* Source, FGenBarButton& B) const;
	/** The screen rectangle of a window of ControlBar.wnd's command window, on the commands' section (empty if it isn't there). */
	FBox2D WindowRect(const TCHAR* ShortName) const;
	/** A command cell by its row and column; the production queue's nine cells by theirs. */
	FBox2D CommandCell(int32 Row, int32 Col) const;
	FBox2D QueueCell(int32 Row, int32 Col) const;
	uint32 IdleWorkerCheckFrame = 0;
};

/** GenBarHotkeys.cpp: presses the shown button on that key (FGenControlBar::FindHotkey) as a click would, and takes
 *  the key for this frame (FGenInput::Consume). The command button pressed (maybe greyed out), or nullptr. */
const FGenCommandButton* GenPressBarHotkey(AGenPlayerController* PC, const FGenKeyChord& Pressed);
