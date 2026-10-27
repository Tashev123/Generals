#include "Game/GenCameraPawn.h"
#include "Core/GenCoords.h"
#include "Map/GenMapActor.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"

AGenCameraPawn::AGenCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->SetFieldOfView(50.f);
	Camera->bConstrainAspectRatio = false;
	// An RTS camera scrolls constantly: motion blur just smears the battlefield.
	Camera->PostProcessSettings.bOverride_MotionBlurAmount = true;
	Camera->PostProcessSettings.MotionBlurAmount = 0.f;
	Height = TargetHeight = GetInitialHeight();
}

float AGenCameraPawn::GetInitialHeight() const
{
	// Start near full zoom out: N wheel notches inward from MaxCameraHeight.
	return FMath::Clamp(MaxCameraHeight * FMath::Pow(1.f - ZoomStepFraction, InitialZoomNotchesFromFullZoomOut), MinCameraHeight, MaxCameraHeight);
}

AGenMapActor* AGenCameraPawn::FindMap() const
{
	for (TActorIterator<AGenMapActor> It(GetWorld()); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AGenCameraPawn::SetTarget(const FVector& Location)
{
	Target = Location;
	if (AGenMapActor* Map = FindMap())
	{
		const FBox B = Map->GetPlayableBoundsUE();
		if (B.IsValid)
		{
			Target.X = FMath::Clamp(Target.X, B.Min.X, B.Max.X);
			Target.Y = FMath::Clamp(Target.Y, B.Min.Y, B.Max.Y);
		}
		GroundZ = Map->GetGroundZ(Target);
	}
	Target.Z = GroundZ;
	UpdateCamera();
}

void AGenCameraPawn::Pan(const FVector2D& ScreenDelta)
{
	// Screen up = direction the camera looks (projected on the ground), screen right = 90 deg clockwise.
	const FRotator YawRot(0.f, Camera->GetComponentRotation().Yaw, 0.f);
	const FVector Forward = YawRot.Vector();
	const FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);
	const float Scale = Height / CameraHeight; // scroll faster when zoomed out
	SetTarget(Target + (Forward * ScreenDelta.Y + Right * ScreenDelta.X) * Scale);
}

void AGenCameraPawn::Zoom(float Steps)
{
	// Proportional steps feel the same at every zoom level; Tick eases Height towards the target.
	TargetHeight = FMath::Clamp(TargetHeight * FMath::Pow(1.f - ZoomStepFraction, Steps), MinCameraHeight, MaxCameraHeight);
}

void AGenCameraPawn::ResetView()
{
	Yaw = 0.f;
	Height = TargetHeight = GetInitialHeight();
	UpdateCamera();
}

void AGenCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Height = FMath::FInterpTo(Height, TargetHeight, DeltaSeconds, ZoomSmoothing);
	if (AGenMapActor* Map = FindMap())
	{
		// CameraAdjustSpeed-like smoothing of the ground height under the target.
		const float Desired = Map->GetGroundZ(Target);
		GroundZ = FMath::FInterpTo(GroundZ, Desired, DeltaSeconds, 6.f);
		Target.Z = GroundZ;
	}
	UpdateCamera();
}

void AGenCameraPawn::UpdateCamera()
{
	// W3DView: offset = (0, -h / tan(pitch), h) in Generals space, rotated by the view angle.
	const float PitchRad = FMath::DegreesToRadians(CameraPitch);
	const FVector OffsetGen(0.f, -Height / FMath::Tan(PitchRad), Height);
	const FVector OffsetUE = GenCoords::ToUE(OffsetGen).RotateAngleAxis(-Yaw, FVector::UpVector);
	SetActorLocation(Target + GenCoords::ToUE(ShakeOffset.X, ShakeOffset.Y, 0.f));
	Camera->SetRelativeLocation(OffsetUE);
	Camera->SetWorldRotation((-OffsetUE).Rotation());
}
