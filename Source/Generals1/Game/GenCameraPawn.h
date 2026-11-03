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
	void AddYaw(float Degrees) { if (!bScripted) { Yaw += Degrees; } }
	void Zoom(float Steps);
	void ResetView();

	/** Camera shake from explosions: a ground-plane offset in Generals units (W3DView::shake). */
	void SetShakeOffset(const FVector2f& OffsetGen) { ShakeOffset = OffsetGen; }

	// --- the map's scripts drive the camera (W3DView's scripted movements) -----------------------------------
	// Positions in Generals units; times in milliseconds of the original's 30 Hz frames, counted in logic time so
	// that a movement takes as many logic frames as the scripts wait for (CAMERA_MOVEMENT_FINISHED).
	/** W3DView::moveCameraTo */
	void ScriptMoveTo(const FVector2f& Pos, float Ms, float EaseInMs, float EaseOutMs, bool bOrient);
	/** SETUP_CAMERA: there at once, looking toward a point, at a pitch and a zoom (a fraction of the highest). */
	void ScriptSetup(const FVector2f& Pos, float ZoomFraction, float Pitch, const FVector2f& LookAt);
	/** W3DView::resetCamera: there, with the default angle, pitch and height. */
	void ScriptReset(const FVector2f& Pos, float Ms, float EaseInMs, float EaseOutMs);
	void ScriptRotate(float Rotations, float Ms, float EaseInMs, float EaseOutMs);
	void ScriptRotateToward(const FVector2f& Pos, float Ms, float EaseInMs, float EaseOutMs, bool bReverse);
	void ScriptZoomTo(float Zoom, float Ms, float EaseInMs, float EaseOutMs);
	void ScriptPitchTo(float Pitch, float Ms, float EaseInMs, float EaseOutMs);
	/** CAMERA_MOD_SET_FINAL_ZOOM / _PITCH: where the movement under way is to end up (zoom as a fraction of the highest). */
	void ScriptModFinalZoom(float ZoomFraction, float EaseIn, float EaseOut);
	void ScriptModFinalPitch(float Pitch, float EaseIn, float EaseOut);
	/** cameraModLookToward: the movement under way turns to look toward the point. */
	void ScriptModLookToward(const FVector2f& Pos, bool bFinalOnly = false);
	/** W3DView::cameraModFreezeAngle: the move under way keeps the angle it has now. */
	void ScriptModFreezeAngle();
	/** W3DView::moveCameraAlongWaypointPath: from where it is, through those points, facing the way it goes. */
	void ScriptMoveAlong(const TArray<FVector2f>& Points, float Ms, float EaseInMs, float EaseOutMs);
	/** CAMERA_FOLLOW_NAMED, each frame: the camera is over that spot. */
	void ScriptFollow(const FVector2f& Pos);
	/** The scripts keep the camera (input is off): the player gets it back once this is off and the movements are over. */
	void SetScriptHold(bool bHold) { bScriptHold = bHold; }
	bool IsScripted() const { return bScripted; }
	bool IsScriptMoving() const { return Path.bActive || Rotate.bActive || ZoomMove.bActive || PitchMove.bActive; }

	float GetYaw() const { return Yaw; }
	/** Height above the look-at point now (Generals units). */
	float GetHeight() const { return Height; }
	/** ... and the height the wheel has asked for, that it is easing towards. */
	float GetTargetHeight() const { return TargetHeight; }
	/** Settle on a given height (Generals units), clamped to the wheel's range. Test runs use it to look at something close. */
	void SetHeight(float NewHeight) { Height = TargetHeight = FMath::Clamp(NewHeight, MinCameraHeight, MaxCameraHeight); }
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

	/** Mouse-wheel notches from full zoom out at match start. */
	UPROPERTY(EditAnywhere, Category = "Generals")
	int32 InitialZoomNotchesFromFullZoomOut = 2;

	/** Generals units per second at full scroll. */
	UPROPERTY(EditAnywhere, Category = "Generals")
	float ScrollSpeed = 1200.f;

private:
	/** The height the camera should start a match at, given the zoom-out notch count. */
	float GetInitialHeight() const;
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

	// --- scripted camera (W3DView's m_pos, m_angle, m_zoom, m_FXPitch, m_groundLevel, m_cameraOffset.z) ---
	struct FEase
	{
		float In = 0.f, Out = 1.f;
		void Set(float EaseIn, float EaseOut);
		float operator()(float T) const;
	};
	/** W3DView's m_mcwpInfo: a spline through the midpoints of the waypoints, eased along its length. */
	struct FPathMove
	{
		bool bActive = false;
		int32 Num = 0;
		TArray<FVector2f> Points;
		TArray<float> Angles, SegLen, Ground;
		float TotalDist = 0.f, TotalMs = 1.f;
		double Start = 0.0;
		FEase Ease;
	};
	struct FValueMove
	{
		bool bActive = false;
		float From = 0.f, To = 0.f, Frames = 1.f;
		double Start = 0.0;
		FEase Ease;
	};
	void BeginScript();
	void EndScript();
	void SetupPath(bool bOrient);
	void UpdateScript();
	void ApplyScriptView();
	void StartValueMove(FValueMove& Move, float From, float To, float Ms, float EaseInMs, float EaseOutMs);
	/** Original 30 Hz frames since a movement began (logic time). */
	double FramesSince(double Start) const;
	double GetScriptTime() const;
	float GroundAt(float X, float Y) const;
	/** getHeightAroundPos: the highest ground around a point. */
	float HeightAround(float X, float Y) const;
	float MaxZoomAt(const FVector2f& P) const { return (HeightAround(P.X, P.Y) + 310.f) / ScriptOffsetZ; }

	bool bScripted = false;
	bool bScriptHold = false;
	FVector2f ScriptPos = FVector2f::ZeroVector;
	float ScriptAngle = 0.f;       // radians, CCW
	float ScriptZoom = 1.f;
	float ScriptFXPitch = 1.f;
	float ScriptGround = 0.f;
	float ScriptOffsetZ = 0.f;     // initHeightForMap: ground (up to 120) + CameraHeight, set once
	FPathMove Path;
	FValueMove Rotate, ZoomMove, PitchMove;
};
