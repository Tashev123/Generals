#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Logic/GenLogicTypes.h"
#include "GenPlayerController.generated.h"

class AGenCameraPawn;
class UGenGameLogic;

/**
 * RTS input (reads raw key state each tick so it works without input mapping assets):
 *   Camera:   arrow keys / screen edges / right-drag scroll, middle-drag rotate, wheel zoom, Home reset
 *   Units:    left click on an own unit selects it (Shift adds / removes); left drag box-selects own units;
 *             with units selected, left click attacks the enemy under the cursor, otherwise moves there;
 *             right click clears the selection; S = stop
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

	/** Off during unattended test runs, so the real mouse can't scroll the camera or issue orders. */
	void SetGenInputEnabled(bool bEnabled) { bGenInputEnabled = bEnabled; }
	bool GetDragBox(FVector2D& OutA, FVector2D& OutB) const;

private:
	AGenCameraPawn* GetCameraPawn() const;
	void UpdateCamera(float DeltaTime, AGenCameraPawn* Cam, float MX, float MY, bool bHasMouse);
	void UpdateSelectionInput(float MX, float MY, bool bHasMouse);

	FGenObjectID PickObject(float MX, float MY) const;
	void BoxSelect(const FVector2D& A, const FVector2D& B, bool bAdd);
	bool GetTerrainUnderCursor(float MX, float MY, FVector3f& OutGenPos) const;
	/** Orders the selection to attack the enemy under the cursor, or to move to the ground point there. */
	void CommandAt(float MX, float MY);

	TArray<FGenObjectID> Selection;
	bool bGenInputEnabled = true;

	// left mouse
	bool bLeftDown = false;
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
