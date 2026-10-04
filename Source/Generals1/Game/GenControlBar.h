#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

class FGenObject;
class UGenGameLogic;
struct FGenCommandButton;
struct FGenThingTemplate;
struct FGenUpgradeTemplate;

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
	bool bEnabled = false;
	bool bOwned = false;                           // already researched / acquired
	bool bGeneralsToggle = false;                  // opens and closes the promotions panel
	FString Disabled;                              // why it can't be used right now
	int32 Cost = 0;                                // money for an upgrade, general's points for a promotion
	float Recharge = 0.f;                          // special power: share of its reload still to go
	uint32 ReadyFrame = 0;
	int32 QueueIndex = INDEX_NONE;                 // >= 0: production queue entry (click cancels it)
};

/**
 * Layout of the in-game interface (ControlBar.cpp, much simplified): the selected object's CommandSet as a
 * 7 x 2 grid at the bottom of the screen with the production queue above it; the general's button and its
 * promotions panel (the three PurchaseScience command sets of the faction); and the column of special power
 * shortcuts on the right. The player controller hit-tests it, the HUD draws it.
 */
class FGenControlBar
{
public:
	void Layout(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection, const FVector2D& ViewSize);

	const FGenBarButton* HitTest(const FVector2D& P) const;
	/** Over any part of the interface: a click there never reaches the world. */
	bool IsOverPanel(const FVector2D& P) const;

	bool bVisible = false;                      // the command panel (something of ours with commands is selected)
	FBox2D Panel = FBox2D(ForceInit);
	FGenObjectID Subject = GenInvalidObjectID;  // the object whose commands are shown
	FString Status;                             // e.g. "Under construction 42%"
	TArray<FGenBarButton> Buttons;
	TArray<FGenBarButton> Queue;
	float Scale = 1.f;

	// General's promotions
	bool bHasGenerals = false;
	FGenBarButton GeneralsButton;
	bool bGeneralsOpen = false;                 // kept between layouts; the controller toggles it
	FBox2D GeneralsPanel = FBox2D(ForceInit);
	TArray<FGenBarButton> Sciences;
	FVector2D SectionLabels[3] = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };

	// Special powers that can be fired without selecting their building
	TArray<FGenBarButton> Shortcuts;

private:
	void LayoutSelection(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection, const FVector2D& ViewSize);
	void LayoutGenerals(UGenGameLogic* Logic, const FVector2D& ViewSize);
	void LayoutShortcuts(UGenGameLogic* Logic, const FVector2D& ViewSize);
	/** Fills in a special power button; false if it should not be shown at all. */
	bool FillPower(UGenGameLogic* Logic, const FGenObject* Source, FGenBarButton& B) const;
};
