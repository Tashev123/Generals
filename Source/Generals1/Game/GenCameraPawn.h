#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "GenCameraPawn.generated.h"

class UCameraComponent;

/**
 * RTS camera matching W3DView: the camera sits at CameraHeight above the look-at point,
 * offset back so it looks down at CameraPitch, with a 50 degree horizontal FOV.
 * Zoom scales the offset (MinCameraHeight..MaxCameraHeight); yaw rotates around the target.
 * Values are in Generals units; pitch, default height and FOV match GameData.ini, while the
 * zoom-out limit is deliberately larger than the original's.
 */
UCLASS()
class GENERALS1_API AGenCameraPawn : public APawn
{
	GENERATED_BODY()

public:
	AGenCameraPawn();

	virtual void Tick(float DeltaSeconds) override;

	/** Moves the look-at point (Unreal space, Z is snapped to the ground). */
	void SetTarget(const FVector& Location);
	FVector GetTarget() const { return Target; }

	void Pan(const FVector2D& ScreenDelta);
	void AddYaw(float Degrees) { Yaw += Degrees; }
	void Zoom(float Steps);
	void ResetView();

	/** Camera shake from explosions: a ground-plane offset in Generals units (W3DView::shake). */
	void SetShakeOffset(const FVector2f& OffsetGen) { ShakeOffset = OffsetGen; }

	float GetYaw() const { return Yaw; }
	/** Height above the look-at point now (Generals units). */
	float GetHeight() const { return Height; }
	/** ... and the height the wheel has asked for, that it is easing towards. */
	float GetTargetHeight() const { return TargetHeight; }
	UCameraComponent* GetCamera() const { return Camera; }

	UPROPERTY(EditAnywhere, Category = "Generals")
	float CameraPitch = 37.5f;

	UPROPERTY(EditAnywhere, Category = "Generals")
	float CameraHeight = 232.f;

	UPROPERTY(EditAnywhere, Category = "Generals")
	float MinCameraHeight = 120.f;

	/** The original stops at 310 (GameData.ini MaxCameraHeight); a modern RTS lets you pull back much further. */
	UPROPERTY(EditAnywhere, Category = "Generals")
	float MaxCameraHeight = 900.f;

	/** Fraction of the current height one mouse wheel step zooms by. */
	UPROPERTY(EditAnywhere, Category = "Generals")
	float ZoomStepFraction = 0.12f;

	/** How quickly the camera settles on the wheel's target height (higher = snappier). */
	UPROPERTY(EditAnywhere, Category = "Generals")
	float ZoomSmoothing = 12.f;

	/** Generals units per second at full scroll. */
	UPROPERTY(EditAnywhere, Category = "Generals")
	float ScrollSpeed = 1200.f;

private:
	void UpdateCamera();
	class AGenMapActor* FindMap() const;

	UPROPERTY(VisibleAnywhere, Category = "Generals")
	TObjectPtr<UCameraComponent> Camera;

	FVector Target = FVector::ZeroVector;
	float Yaw = 0.f;            // degrees; 0 = camera south of target looking north (CameraYaw = 0)
	float Height = 232.f;       // current height above the look-at point (Generals units)
	float TargetHeight = 232.f; // height the wheel asked for; Height eases towards it
	float GroundZ = 0.f;        // smoothed ground height under the target (Unreal)
	FVector2f ShakeOffset = FVector2f::ZeroVector;
};
