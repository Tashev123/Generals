#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Game/GenControlBar.h"
#include "Logic/GenLogicTypes.h"
#include "GenPlayerController.generated.h"

class AGenCameraPawn;
class UGenGameLogic;
class UGenModelComponent;

/**
 * RTS input (reads raw key state each tick so it works without input mapping assets):
 *   Camera:   arrow keys / screen edges / right-drag scroll, middle-drag rotate, wheel zoom, Home reset
 *   Units:    left click on an own unit selects it (Shift adds / removes); left drag box-selects own units;
 *             with units selected, left click attacks the enemy under the cursor, otherwise moves there;
 *             right click clears the selection; S = stop
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
	void SetSelection(const TArray<FGenObjectID>& Ids) { Selection = Ids; }

	/** Off during unattended test runs, so the real mouse can't scroll the camera or issue orders. */
	void SetGenInputEnabled(bool bEnabled) { bGenInputEnabled = bEnabled; }
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
	bool TestFireAt(const FVector3f& Pos);

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
	/** Fires the power being aimed at what is under the cursor; false if there is nothing suitable there. */
	bool FireTargetedPower(float MX, float MY);

	FGenObjectID PickObject(float MX, float MY) const;
	void BoxSelect(const FVector2D& A, const FVector2D& B, bool bAdd);
	bool GetTerrainUnderCursor(float MX, float MY, FVector3f& OutGenPos) const;
	/** Orders the selection to attack the enemy under the cursor, or to move to the ground point there. */
	void CommandAt(float MX, float MY);

	TArray<FGenObjectID> Selection;
	bool bGenInputEnabled = true;
	FGenControlBar ControlBar;

	// Building placement (after clicking a construction button)
	const FGenThingTemplate* PlacingTemplate = nullptr;
	FGenObjectID PlacingDozer = GenInvalidObjectID;
	FVector3f PlacementPos = FVector3f::ZeroVector;
	bool bPlacementValid = false;      // the cursor is over terrain
	bool bPlacementLegal = false;
	UPROPERTY(Transient)
	TObjectPtr<UGenModelComponent> PlacementGhost;

	// Aiming a special power (after clicking its button)
	const FGenCommandButton* TargetingButton = nullptr;
	FGenObjectID TargetingSource = GenInvalidObjectID;
	FString TargetingPower;
	FVector3f TargetingPos = FVector3f::ZeroVector;
	bool bTargetingValid = false;      // the cursor is over terrain

	// left mouse
	bool bLeftDown = false;
	bool bLeftOnUI = false;            // the press started on the command bar
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
