#pragma once

#include "CoreMinimal.h"
#include <initializer_list>
#include "GameFramework/PlayerController.h"
#include "Game/GenControlBar.h"
#include "Logic/GenLogicTypes.h"
#include "GenPlayerController.generated.h"

class AGenCameraPawn;
class UGenGameLogic;
class UGenModelComponent;
class UDecalComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
struct FGenRadiusDecalTemplate;

/**
 * RTS input (reads raw key state each tick so it works without input mapping assets):
 *   Camera:   arrow keys / screen edges / right-drag scroll, middle-drag rotate, wheel zoom, Home reset
 *   Units:    left click on an own unit selects it (Shift adds / removes); left drag box-selects own units;
 *             with units selected, left click attacks the enemy under the cursor, otherwise moves there;
 *             right click clears the selection; S = stop
 *   Select:   a click lands on what the cursor is on - the model itself, not the space round it - and the
 *             cursor says beforehand what it would do; double click an own unit: all of its kind on the
 *             screen (with Alt: on the map); E: everything like the selection; Q: every fighting unit;
 *             W: every aircraft (each on the screen first, then on the map); Ctrl + a number makes the
 *             selection a group, the number selects it (twice: looks at it), Shift + the number adds it,
 *             Alt + the number looks at it
 *   Building: left click a command bar button; construction buttons then let you place the building with
 *             a left click (right click cancels); with a dozer selected, left click an unfinished building
 *             of yours to carry on building it, or a damaged one to mend it
 *   Repairs:  with damaged vehicles selected, left click a war factory of yours to have them mended
 *   Garrison: with units selected, left click a transport of yours, a building that can be garrisoned, a
 *             tunnel or (wounded infantry) a barracks to send them in; the container's command bar lists
 *             who is inside: click one to let it out, or "evacuate" for all
 *   Rally:    with only factories selected, left click the ground to set where their new units go
 *   Orders:   A (or the button) then left click: attack-move there; G (or the button) then left click:
 *             guard that spot, or the unit or building clicked; right click cancels
 *   Powers:   left click a special power (in its building's command bar or the shortcut column on the right),
 *             then left click the target (right click cancels); the button under the money read-out opens
 *             the general's promotions
 *   Radar:    left click or drag on the radar looks there; while aiming an order or a power, a left click
 *             on the radar gives it at that spot; Space jumps to the last thing the radar reported
 *   Menu:     Esc or F10 during a game: the in-game menu (the game stands still while it is up)
 */
UCLASS()
class GENERALS1_API AGenPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AGenPlayerController();

	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;

	const TArray<FGenObjectID>& GetSelection() const { return Selection; }
	/** InGameUI::getMousedOverDrawableID: what the cursor is on (it shows its health bar), if anything. */
	FGenObjectID GetMousedOver() const { return MousedOver; }
	/** How long the cursor has been on it (its name comes up after Mouse.ini's TooltipDelayTime). */
	double GetMousedOverSeconds() const { return MousedOver != GenInvalidObjectID ? FPlatformTime::Seconds() - MousedOverSince : 0.0; }
	/** InGameUI::isInWaypointMode: Alt is held (MSG_META_BEGIN_WAYPOINTS) - a click adds a waypoint, and the
	    routes of what is selected are drawn. */
	bool IsWaypointMode() const;
	/** What kinds of thing a pick may land on (View.h's PICK_TYPE_*). */
	enum : uint32 { PickSelectable = 1, PickShrubbery = 2, PickMines = 4, PickForceAttackable = 8 };
	/** Test automation: what a click at that spot of the world would land on (the pick itself, by way of the screen). */
	FGenObjectID TestPickAt(const FVector3f& Pos, FVector2D* OutScreen = nullptr) const;
	/** Test automation: the cursor is taken to be over that spot of the world (input is off in a test run, so it
	    stays there): what it is on shows its health bar, and the cursor it would be is drawn into the picture. */
	void TestHoverAt(const FVector3f& Pos);
	/** Test automation: a left click at that spot of the world (with Ctrl held: bForce; with Alt: bWaypoint, which
	    then stays held for the rest of the run). What it did, and the cursor it showed. */
	FString TestClickAt(const FVector3f& Pos, bool bForce, bool bWaypoint = false);
	bool GetTestCursor(FVector2D& OutPos) const { OutPos = TestCursorPos; return bTestCursor; }
	/** The kind of cursor showing (Mouse.ini's names). */
	const FString& GetCursorName() const;
	/** Player::getSquadNumberForObject: the group (Ctrl + a number) a unit of ours is in; none: INDEX_NONE. */
	int32 GetGroupOf(FGenObjectID Id) const;
	/** Test automation: a selection key - "create" / "select" / "add" / "view" with a group's number, "match",
	    "matchmap", "all", "aircraft", "double" (a double click on the first thing selected). Says what came of it. */
	FString TestSelectionKey(const FString& What, int32 Group);
	void SetSelection(const TArray<FGenObjectID>& Ids) { Selection = Ids; }

	/** Off during unattended test runs, so the real mouse can't scroll the camera or issue orders. */
	void SetGenInputEnabled(bool bEnabled) { bGenInputEnabled = bEnabled; }
	bool IsGenInputEnabled() const { return bGenInputEnabled; }
	bool GetDragBox(FVector2D& OutA, FVector2D& OutB) const;

	// --- for the HUD ---
	const FGenControlBar& GetControlBar() const { return ControlBar; }
	const FVector2D& GetMousePos() const { return MouseNow; }
	/** The building being placed, where, and whether it may go there. */
	const FGenThingTemplate* GetPlacement(FVector3f& OutPos, float& OutAngle, bool& bOutLegal) const;
	/** Performs a command bar click (also used by the test automation). */
	void ExecuteBarButton(const FGenBarButton& Button);
	/** The special power being aimed: where the cursor points, the radius to show, and its button. */
	const FGenCommandButton* GetTargeting(FVector3f& OutPos, float& OutRadius, bool& bOutValid) const;
	void SetGeneralsOpen(bool bOpen) { ControlBar.bGeneralsOpen = bOpen; }
	/** Test automation: clicks the interface button that carries this command button (promotions panel,
	    shortcut column, command bar). False if it isn't shown or is greyed out. */
	bool TestClickButton(const FString& CommandButtonName);
	/** Test automation: the left click that fires the power being aimed, at this spot. */
	bool TestFireAt(const FVector3f& Pos, const float* PlaceDegrees = nullptr);
	/** A left click on the radar at that screen position: looks there, or gives the order being aimed. False off the map. */
	bool RadarClick(const FVector2D& Pixel, bool bRelease);
	/** Moves the view to a world position (Generals units). */
	void LookAt(const FVector3f& Pos);
	/** One of the units answers the order: the first of these voices its template has ("VoiceMove"). */
	void PlayVoice(std::initializer_list<const TCHAR*> Keys, const TArray<FGenObjectID>& Units);
	/** The unit that answers for the lot: the first of ours still alive. */
	const FGenObject* FindSpeaker(const TArray<FGenObjectID>& Units) const;
	/** pickAndPlayUnitVoiceResponse for an attack: the voice the weapon used calls for (the button's Slot, else the
	 *  current one). Target and bAir as the original knows them for that order. */
	enum class EAttackVoice : uint8 { Attack, ForceObject, WeaponAtObject, WeaponAtLocation };
	void PlayAttackVoice(const TArray<FGenObjectID>& Units, EAttackVoice Order, const FGenObject* Target, bool bAir, int32 Slot = INDEX_NONE,
		bool bMineClearing = false);
	/** ... for a move and its kin (attack-move, a waypoint, a salvage crate, going for repairs). */
	void PlayMoveVoice(const TArray<FGenObjectID>& Units, bool bSalvage = false, bool bWaypoint = false);
	/** "Additional sounds for terrorists in cars": MiscAudio's TerroristInCar... line, if the unit that spoke is one. */
	void PlayCarBombVoice(const FGenObject* Speaker, const TCHAR* MiscKey);

private:
	AGenCameraPawn* GetCameraPawn() const;
	void UpdateCamera(float DeltaTime, AGenCameraPawn* Cam, float MX, float MY, bool bHasMouse);
	void UpdateSelectionInput(float MX, float MY, bool bHasMouse);
	void UpdatePlacement(float MX, float MY, bool bHasMouse);
	void EndPlacement();
	void UpdateTargeting(float MX, float MY, bool bHasMouse);
	void EndTargeting();
	/** Starts aiming an order that needs a spot (attack-move, guard) for the selection. */
	void BeginOrderTargeting(const FGenCommandButton* Button);
	/** Fires the power being aimed at what is under the cursor; false if there is nothing suitable there.
	    bAllowPick: the cursor is over the world, so what is under it may be the target. */
	bool FireTargetedPower(float MX, float MY, bool bAllowPick = true, FGenObjectID ForcedTarget = GenInvalidObjectID);

	/**
	 * W3DView::pickDrawable: the nearest thing of those kinds whose model the ray through that pixel meets -
	 * its triangles or its pick box, not the space around it. (If the ray meets nothing, a few more are
	 * tried a hair's breadth around it - gen.PickSlack pixels - for soldiers seen from far above.)
	 */
	FGenObjectID PickObject(float MX, float MY, uint32 PickTypes = PickSelectable) const;

	/** What a plain left click on the world would do with the selection as it is (CommandTranslator::evaluateContextCommand). */
	enum class EClickAction : uint8
	{
		Nothing, Dock, Enter, Repair, GetRepaired, SetRally, Land, ResumeConstruction, SelectOwn, SteerPower, Attack, Move, SelectOther, Deselect,
		ForceAttackObject, ForceAttackGround, ImpossibleAttack, AddWaypoint, Salvage
	};
	struct FClickEval
	{
		EClickAction Action = EClickAction::Nothing;
		FGenObjectID Hit = GenInvalidObjectID;
		TArray<FGenObjectID> Units;            // who would carry it out
		FVector3f Pos = FVector3f::ZeroVector; // where to (a move, a rally point)
		bool bCanCommand = false;              // something of ours that moves or fights is selected
	};
	void EvaluateClick(FGenObjectID Hit, float MX, float MY, FClickEval& Out) const;
	/** ... and what it does. */
	void ExecuteClick(const FClickEval& Eval, bool bShift);
	/** InGameUI::isInForceAttackMode: Ctrl is held (MSG_META_BEGIN_FORCEATTACK). */
	bool IsForceAttackMode() const;
	bool bTestForceAttack = false;
	bool bTestWaypoint = false;
	/** getPickTypesForContext: in force attack mode props that can only be shot at are clicked on too. */
	uint32 ClickPickTypes() const { return PickSelectable | (IsForceAttackMode() ? uint32(PickForceAttackable) : 0u); }
	/** CanAttackResult, of canObjectForceAttack. */
	enum class EForceAttack : uint8 { NotPossible, Possible, InvalidShot };
	EForceAttack CanForceAttack(const FGenObject& U, const FGenObject* Victim, const FVector3f& Pos) const;
	/** InGameUI::createCommandHint (default mode): the cursor that says what a click there would do. */
	FString CursorForClick(FGenObjectID Hit, float MX, float MY) const;
	/** ... and in any mode: scrolling, over the interface, placing a building, aiming an order or a power. */
	FString ChooseCursor(float MX, float MY, bool bHasMouse, int32& OutDirection) const;
	bool IsTargetingAcceptable() const;
	/** What a FIRE_WEAPON button's shot may be aimed at. */
	bool CanFireWeaponAt(const FGenObject& Victim) const;
	void UpdateCursor(float MX, float MY, bool bHasMouse);
	/** InGameUI::selectMatchingAcrossScreen / AcrossMap: everything of ours like what is selected as well.
	    How many more that is (-1: nothing of ours was selected to go by). */
	int32 SelectMatching(bool bAcrossMap);
	/** MSG_META_SELECT_ALL / SELECT_ALL_AIRCRAFT: on the screen, or if that adds nothing, on the map. */
	int32 SelectAllUnits(bool bAircraft);
	/** InGameUI::selectNextIdleWorker: the next of our dozers / workers with nothing to do (its container if
	    it is inside something), and the view on it. */
	void SelectNextIdleWorker();
	int32 SelectAllOfKind(bool bAircraft, bool bAcrossMap);
	bool IsOnScreen(const FGenObject& O) const;
	enum class EGroupKey : uint8 { Create, Select, Add, View };
	void GroupKey(int32 Group, EGroupKey Kind);
	TArray<FGenObjectID> Groups[10];                // the player's hotkey squads
	int32 LastGroup = INDEX_NONE;                   // ... and the one last asked for, and when (a second time soon: look at it)
	double LastGroupTime = 0.0;
	FGenObjectID LastClickHit = GenInvalidObjectID; // what the last plain click selected, and when (a double click)
	double LastClickTime = 0.0;
	FString HoverCursor = TEXT("Arrow");            // by what is under the mouse and who is selected (worked out with MousedOver)
	FVector2D ScrollScreen = FVector2D::ZeroVector; // the way the mouse is scrolling the view (x right, y down); zero: it isn't
	bool bTestCursor = false;
	FVector2D TestCursorPos = FVector2D::ZeroVector;
	/** Keeps MousedOver up to date: when the mouse or the view has moved, and thirty times a second otherwise. */
	void UpdateMouseOver(float MX, float MY, bool bHasMouse);
	FGenObjectID MousedOver = GenInvalidObjectID;
	double MousedOverSince = 0.0;
	FVector MouseOverOrigin = FVector::ZeroVector, MouseOverDir = FVector::ZeroVector;
	double MouseOverTime = 0.0;
	void BoxSelect(const FVector2D& A, const FVector2D& B, bool bAdd);
	bool GetTerrainUnderCursor(float MX, float MY, FVector3f& OutGenPos) const;

	TArray<FGenObjectID> Selection;
	bool bGenInputEnabled = true;
	FGenControlBar ControlBar;

	// Building placement (after clicking a construction button)
	const FGenThingTemplate* PlacingTemplate = nullptr;
	FGenObjectID PlacingDozer = GenInvalidObjectID;
	FString PlacingPower;              // SPECIAL_POWER_CONSTRUCT: the building is what this power puts down ...
	FGenObjectID PlacingPowerSource = GenInvalidObjectID; // ... fired from here
	FVector3f PlacementPos = FVector3f::ZeroVector;
	float PlacementAngle = 0.f;        // which way the building faces (the template's view angle, or as dragged)
	bool bPlacementAnchored = false;   // InGameUI::isPlacementAnchored: the left button went down on a spot, and a drag turns the building
	FVector2D PlacementAnchorScreen = FVector2D::ZeroVector;
	bool bPlacementValid = false;      // the cursor is over terrain
	bool bPlacementLegal = false;
	const TCHAR* PlacementWhy = nullptr; // why not (InGameUI::displayCantBuildMessage's label), when that is the site
	/** Whether the building being placed may go there (bPlacementLegal, PlacementWhy). */
	void CheckPlacementAt(const FVector3f& Pos);
	/** The click while placing: the building goes down (or the power fires), or the refusal is shown and heard. */
	void ConfirmPlacement();
	UPROPERTY(Transient)
	TObjectPtr<UGenModelComponent> PlacementGhost;

	// RadiusDecal: pictures on the ground - the radius cursor of what is being aimed (InGameUI::setRadiusCursor,
	// handleRadiusCursor), our gunships' target area and reticle. Made once and reused.
	void UpdateRadiusDecals();
	void ShowRadiusDecal(int32 Index, const FGenRadiusDecalTemplate& T, const FVector3f& Pos, float Radius, const FLinearColor& PlayerColor, double Seconds);
	/** The radius cursor of the button being aimed, and its radius by its kind; none: nullptr. */
	const FGenRadiusDecalTemplate* GetRadiusCursor(float& OutRadius) const;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UDecalComponent>> RadiusDecals;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> RadiusDecalMaterials;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> RadiusDecalBase;
	TArray<const FGenRadiusDecalTemplate*> RadiusDecalShown; // the template whose texture each one has
	TWeakObjectPtr<AActor> RadiusDecalOwner;                 // the map's actor they belong to (a controller is never drawn)

	// Aiming a special power (after clicking its button)
	const FGenCommandButton* TargetingButton = nullptr;
	FGenObjectID TargetingSource = GenInvalidObjectID;
	FString TargetingPower;
	FVector3f TargetingPos = FVector3f::ZeroVector;
	bool bTargetingValid = false;      // the cursor is over terrain

	// left mouse
	bool bLeftDown = false;
	bool bLeftOnUI = false;            // the press started on the command bar
	bool bLeftOnRadar = false;         // ... on the radar
	bool bBoxSelecting = false;
	FVector2D LeftStart = FVector2D::ZeroVector;
	FVector2D MouseNow = FVector2D::ZeroVector;

	// right mouse
	bool bRightDown = false;
	bool bRightScrolling = false;
	FVector2D RightStart = FVector2D::ZeroVector;

	bool bMiddleDragging = false;
	float LastMouseX = 0.f;
};
