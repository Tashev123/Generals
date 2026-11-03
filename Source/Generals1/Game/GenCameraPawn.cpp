#include "Game/GenCameraPawn.h"
#include "Core/GenCoords.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "Logic/GenGameLogic.h"
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
	if (bScripted)
	{
		return; // the map's scripts have the camera
	}
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
	if (bScripted)
	{
		return;
	}
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
	if (bScripted)
	{
		UpdateScript();
		if (bScripted)
		{
			return;
		}
	}
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

// ---- the map's scripts drive the camera (W3DView's scripted movements) ---------------------------------------------

namespace
{
	constexpr float W3DFrameMs = 33.f; // TheW3DFrameLengthInMsec: the original moved its camera once per drawn frame

	/** normAngle: to +-PI. */
	float NormAngle(float A)
	{
		if (A < -10.f * PI || A > 10.f * PI)
		{
			return 0.f;
		}
		while (A < -PI) { A += 2.f * PI; }
		while (A > PI) { A -= 2.f * PI; }
		return A;
	}

	/** The camera angle that looks along a direction (the default camera looks up +Y: "rotated 90 degrees"). */
	bool AngleToward(const FVector2f& Dir, float& Out)
	{
		const float Len = Dir.Size();
		if (Len < 0.1f)
		{
			return false;
		}
		float A = FMath::Acos(FMath::Clamp(Dir.X / Len, -1.f, 1.f));
		if (Dir.Y < 0.f)
		{
			A = -A;
		}
		Out = NormAngle(A - PI / 2.f);
		return true;
	}
}

void AGenCameraPawn::FEase::Set(float EaseIn, float EaseOut)
{
	// ParabolicEase::setEaseTimes
	In = FMath::Clamp(EaseIn, 0.f, 1.f);
	Out = FMath::Clamp(1.f - EaseOut, 0.f, 1.f);
	In = FMath::Min(In, Out);
}

float AGenCameraPawn::FEase::operator()(float T) const
{
	// ParabolicEase::operator(): accelerate, glide, decelerate
	T = FMath::Clamp(T, 0.f, 1.f);
	const float V0 = 1.f + Out - In;
	if (T < In)
	{
		return T * T / (V0 * In);
	}
	if (T <= Out)
	{
		return (In + 2.f * (T - In)) / V0;
	}
	return (In + 2.f * (Out - In) + (2.f * (T - Out) + Out * Out - T * T) / (1.f - Out)) / V0;
}

double AGenCameraPawn::GetScriptTime() const
{
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	return Logic ? Logic->GetRenderFrameTime() : 0.0;
}

double AGenCameraPawn::FramesSince(double Start) const
{
	return (GetScriptTime() - Start) * 30.0 / double(GenLogic::GetLogicFPS());
}

float AGenCameraPawn::GroundAt(float X, float Y) const
{
	const AGenMapActor* MapActor = FindMap();
	const FGenMapData* Map = MapActor ? MapActor->GetMapData() : nullptr;
	return Map ? Map->GetGroundHeight(X, Y) : 0.f;
}

float AGenCameraPawn::HeightAround(float X, float Y) const
{
	constexpr float S = 40.f; // TERRAIN_SAMPLE_SIZE
	return FMath::Max(GroundAt(X, Y), FMath::Max(FMath::Max(GroundAt(X + S, Y - S), GroundAt(X - S, Y - S)), FMath::Max(GroundAt(X + S, Y + S), GroundAt(X - S, Y + S))));
}

void AGenCameraPawn::BeginScript()
{
	if (bScripted)
	{
		return;
	}
	// Take over from the player's camera where it is: same spot, angle and height.
	bScripted = true;
	const FVector Gen = GenCoords::FromUE(Target);
	ScriptPos = FVector2f(Gen.X, Gen.Y);
	ScriptGround = GroundAt(ScriptPos.X, ScriptPos.Y);
	if (ScriptOffsetZ <= 0.f)
	{
		ScriptOffsetZ = FMath::Min(ScriptGround, 120.f) + CameraHeight; // initHeightForMap
	}
	ScriptAngle = NormAngle(FMath::DegreesToRadians(Yaw));
	ScriptZoom = (Height + ScriptGround) / ScriptOffsetZ;
	ScriptFXPitch = 1.f;
}

void AGenCameraPawn::EndScript()
{
	// The player has the camera again, where the scripts left it - and, the user's word of 2026-11-01, zoomed out as a
	// skirmish starts, after every cut scene: from the scene's height it eases back there.
	bScripted = false;
	Path.bActive = Rotate.bActive = ZoomMove.bActive = PitchMove.bActive = false;
	Yaw = FMath::RadiansToDegrees(ScriptAngle);
	Height = FMath::Clamp(ScriptOffsetZ * ScriptZoom - ScriptGround, MinCameraHeight, MaxCameraHeight);
	TargetHeight = GetInitialHeight();
	SetTarget(GenCoords::ToUE(ScriptPos.X, ScriptPos.Y, ScriptGround));
}

void AGenCameraPawn::ScriptFollow(const FVector2f& Pos)
{
	// W3DView's camera lock: it stays over the thing. (A movement under way goes first.)
	if (!bScripted)
	{
		SetTarget(GenCoords::ToUE(Pos.X, Pos.Y, GroundZ / GenCoords::UnitsToCm));
	}
	else if (!Path.bActive)
	{
		ScriptPos = Pos;
		ScriptGround = GroundAt(Pos.X, Pos.Y);
	}
}

void AGenCameraPawn::StartValueMove(FValueMove& Move, float From, float To, float Ms, float EaseInMs, float EaseOutMs)
{
	// zoomCamera / pitchCamera / rotateCamera: numFrames = ms / frame length, eased by fractions of the time.
	Ms = FMath::Max(Ms, 1.f);
	Move.bActive = true;
	Move.From = From;
	Move.To = To;
	Move.Frames = FMath::Max(1.f, FMath::FloorToFloat(Ms / W3DFrameMs));
	Move.Start = GetScriptTime();
	Move.Ease.Set(EaseInMs / Ms, EaseOutMs / Ms);
}

void AGenCameraPawn::SetupPath(bool bOrient)
{
	// W3DView::setupWaypointPath. Points[1..Num] are the path (1 is where the camera is), 0 and Num+1 pad its ends.
	FPathMove& P = Path;
	const int32 N = P.Num;
	P.Angles.SetNumZeroed(N + 2);
	P.SegLen.SetNumZeroed(N + 2);
	P.Ground.SetNumZeroed(N + 2);
	P.Points.SetNum(N + 2);
	P.TotalDist = 0.f;
	float Angle = ScriptAngle;
	for (int32 i = 1; i < N; ++i)
	{
		const FVector2f Dir = P.Points[i + 1] - P.Points[i];
		P.SegLen[i] = Dir.Size();
		P.TotalDist += P.SegLen[i];
		if (bOrient)
		{
			AngleToward(Dir, Angle);
		}
		P.Angles[i] = Angle;
	}
	P.Angles[0] = ScriptAngle;
	P.Angles[1] = ScriptAngle;
	P.Angles[N] = P.Angles[N - 1];
	for (int32 i = N - 1; i > 1; --i)
	{
		P.Angles[i] = (P.Angles[i] + P.Angles[i - 1]) / 2.f;
	}
	P.SegLen[N + 1] = P.SegLen[N];
	if (P.TotalDist < 1.f)
	{
		P.SegLen[N - 1] += 1.f - P.TotalDist;
		P.TotalDist = 1.f;
	}
	const float NewGround = GroundAt(P.Points[N].X, P.Points[N].Y);
	float Dist = 0.f;
	for (int32 i = 0; i <= N + 1; ++i)
	{
		const float F2 = Dist / P.TotalDist;
		P.Ground[i] = ScriptGround * (1.f - F2) + NewGround * F2;
		Dist += P.SegLen[i];
	}
	P.Points[N + 1] = P.Points[N] + (P.Points[N] - P.Points[N - 1]);
	P.Angles[N + 1] = P.Angles[N];
	P.Ground[N + 1] = NewGround;
	P.Points[0] -= P.Points[2] - P.Points[1];
	P.bActive = N > 1;
	P.Start = GetScriptTime();
	Rotate.bActive = false;
}

void AGenCameraPawn::ScriptMoveTo(const FVector2f& Pos, float Ms, float EaseInMs, float EaseOutMs, bool bOrient)
{
	BeginScript();
	Ms = FMath::Max(Ms, 1.f);
	Path.Num = 2;
	Path.Points = { ScriptPos, ScriptPos, Pos, Pos };
	Path.TotalMs = Ms;
	Path.Ease.Set(EaseInMs / Ms, EaseOutMs / Ms);
	SetupPath(bOrient);
}

void AGenCameraPawn::ScriptModFreezeAngle()
{
	Rotate.bActive = false;
	if (Path.bActive)
	{
		for (int32 i = 0; i <= Path.Num + 1; ++i)
		{
			Path.Angles[i] = ScriptAngle;
		}
	}
}

void AGenCameraPawn::ScriptMoveAlong(const TArray<FVector2f>& Points, float Ms, float EaseInMs, float EaseOutMs)
{
	BeginScript();
	Ms = FMath::Max(Ms, 1.f);
	Path.Points = { ScriptPos, ScriptPos };
	for (int32 i = 0; i < Points.Num(); ++i)
	{
		// (MIN_DELTA: one that close is dropped, the last one takes the place of the one before it.)
		if (FVector2f::Distance(Points[i], Path.Points.Last()) >= GenCoords::MapXYFactor) { Path.Points.Add(Points[i]); }
		else if (i == Points.Num() - 1 && Path.Points.Num() > 2) { Path.Points.Last() = Points[i]; }
	}
	if (Path.Points.Num() < 3)
	{
		Path.Points.Add(ScriptPos);
	}
	Path.Num = Path.Points.Num() - 1;
	const FVector2f PathEnd = Path.Points.Last();
	Path.Points.Add(PathEnd);
	Path.TotalMs = Ms;
	Path.Ease.Set(EaseInMs / Ms, EaseOutMs / Ms);
	SetupPath(true);
}

void AGenCameraPawn::ScriptSetup(const FVector2f& Pos, float ZoomFraction, float Pitch, const FVector2f& LookAt)
{
	// doSetupCamera: moveCameraTo(pos, 0) + cameraModLookToward + cameraModFinalPitch + cameraModFinalZoom, all at once.
	ScriptMoveTo(Pos, 0.f, 0.f, 0.f, true);
	ScriptModLookToward(LookAt);
	ScriptModFinalPitch(Pitch, 0.f, 0.f);
	ScriptModFinalZoom(ZoomFraction, 0.f, 0.f);
	UpdateScript();
}

void AGenCameraPawn::ScriptReset(const FVector2f& Pos, float Ms, float EaseInMs, float EaseOutMs)
{
	// W3DView::resetCamera
	ScriptMoveTo(Pos, Ms, EaseInMs, EaseOutMs, false);
	Path.Angles[2] = 0.f;
	Path.Angles[3] = 0.f;
	// (The original's default zoom is the player's starting height in this game, as in a skirmish.)
	ScriptZoomTo((GetInitialHeight() + GroundAt(Pos.X, Pos.Y)) / ScriptOffsetZ, Ms, EaseInMs, EaseOutMs);
	ScriptPitchTo(1.f, Ms, EaseInMs, EaseOutMs);
}

void AGenCameraPawn::ScriptRotate(float Rotations, float Ms, float EaseInMs, float EaseOutMs)
{
	BeginScript();
	StartValueMove(Rotate, ScriptAngle, ScriptAngle + 2.f * PI * Rotations, Ms, EaseInMs, EaseOutMs);
	Path.bActive = false;
}

void AGenCameraPawn::ScriptRotateToward(const FVector2f& Pos, float Ms, float EaseInMs, float EaseOutMs, bool bReverse)
{
	// W3DView::rotateCameraTowardPosition
	BeginScript();
	float Angle;
	if (!AngleToward(Pos - ScriptPos, Angle))
	{
		return;
	}
	if (bReverse)
	{
		Angle += ScriptAngle < Angle ? -2.f * PI : 2.f * PI;
	}
	StartValueMove(Rotate, ScriptAngle, Angle, Ms, EaseInMs, EaseOutMs);
	Path.bActive = false;
}

void AGenCameraPawn::ScriptZoomTo(float Zoom, float Ms, float EaseInMs, float EaseOutMs)
{
	BeginScript();
	StartValueMove(ZoomMove, ScriptZoom, Zoom, Ms, EaseInMs, EaseOutMs);
}

void AGenCameraPawn::ScriptPitchTo(float Pitch, float Ms, float EaseInMs, float EaseOutMs)
{
	BeginScript();
	StartValueMove(PitchMove, ScriptFXPitch, Pitch, Ms, EaseInMs, EaseOutMs);
}

void AGenCameraPawn::ScriptModFinalZoom(float ZoomFraction, float EaseIn, float EaseOut)
{
	// W3DView::cameraModFinalZoom: over what is left of the rotation or the move.
	BeginScript();
	if (Rotate.bActive)
	{
		const float Ms = FMath::Max(0.f, Rotate.Frames - float(FramesSince(Rotate.Start))) * W3DFrameMs;
		ScriptZoomTo(ZoomFraction * MaxZoomAt(ScriptPos), Ms, Ms * EaseIn, Ms * EaseOut);
	}
	if (Path.bActive)
	{
		const float Ms = FMath::Max(0.f, Path.TotalMs - float(FramesSince(Path.Start)) * W3DFrameMs);
		ScriptZoomTo(ZoomFraction * MaxZoomAt(Path.Points[Path.Num]), Ms, Ms * EaseIn, Ms * EaseOut);
	}
}

void AGenCameraPawn::ScriptModFinalPitch(float Pitch, float EaseIn, float EaseOut)
{
	BeginScript();
	if (Rotate.bActive)
	{
		const float Ms = FMath::Max(0.f, Rotate.Frames - float(FramesSince(Rotate.Start))) * W3DFrameMs;
		ScriptPitchTo(Pitch, Ms, Ms * EaseIn, Ms * EaseOut);
	}
	if (Path.bActive)
	{
		const float Ms = FMath::Max(0.f, Path.TotalMs - float(FramesSince(Path.Start)) * W3DFrameMs);
		ScriptPitchTo(Pitch, Ms, Ms * EaseIn, Ms * EaseOut);
	}
}

void AGenCameraPawn::ScriptModLookToward(const FVector2f& Pos, bool bFinalOnly)
{
	// W3DView::cameraModLookToward: each waypoint of the move turns to face the point from where the curve passes it.
	// (cameraModFinalLookToward: only the last ones.)
	if (Rotate.bActive || !Path.bActive)
	{
		return;
	}
	for (int32 i = bFinalOnly ? FMath::Max(Path.Num - 1, 2) : 2; i <= Path.Num; ++i)
	{
		const FVector2f Start = (Path.Points[i - 1] + Path.Points[i]) / 2.f;
		const FVector2f Mid = Path.Points[i];
		const FVector2f End = (Path.Points[i] + Path.Points[i + 1]) / 2.f;
		const FVector2f At = Start + 0.5f * (End - Start) + 0.25f * (Mid - End + Mid - Start);
		AngleToward(Pos - At, Path.Angles[i]);
	}
	Path.Angles[Path.Num + 1] = Path.Angles[Path.Num];
}

void AGenCameraPawn::UpdateScript()
{
	// rotateCameraOneFrame / zoomCameraOneFrame / pitchCameraOneFrame
	auto Advance = [this](FValueMove& M, float& Value)
	{
		if (!M.bActive)
		{
			return;
		}
		const float T = float(FramesSince(M.Start)) / M.Frames;
		if (T >= 1.f)
		{
			Value = M.To;
			M.bActive = false;
		}
		else
		{
			Value = FMath::Lerp(M.From, M.To, M.Ease(T));
		}
	};
	Advance(ZoomMove, ScriptZoom);
	Advance(PitchMove, ScriptFXPitch);
	if (Rotate.bActive)
	{
		Advance(Rotate, ScriptAngle);
		ScriptAngle = NormAngle(ScriptAngle);
	}
	else if (Path.bActive)
	{
		// moveAlongWaypointPath, worked out from the time gone rather than frame by frame.
		const int32 N = Path.Num;
		const float Elapsed = float(FramesSince(Path.Start)) * W3DFrameMs;
		float Dist = Path.TotalMs <= 1.f ? Path.TotalDist : Path.Ease(Elapsed / Path.TotalMs) * Path.TotalDist;
		int32 Seg = 1;
		while (Seg < N && Dist >= Path.SegLen[Seg])
		{
			Dist -= Path.SegLen[Seg];
			++Seg;
		}
		if (Seg >= N || Elapsed > Path.TotalMs)
		{
			ScriptAngle = Path.Angles[N];
			ScriptGround = Path.Ground[N];
			ScriptPos = Path.Points[N];
			Path.bActive = false;
		}
		else
		{
			float F = Dist / FMath::Max(Path.SegLen[Seg], 0.001f);
			float A1 = Path.Angles[Seg];
			const float A2 = Path.Angles[Seg + 1];
			if (A2 - A1 > PI) { A1 += 2.f * PI; }
			if (A2 - A1 < -PI) { A1 -= 2.f * PI; }
			ScriptAngle = NormAngle(A1 * (1.f - F) + A2 * F);
			ScriptGround = Path.Ground[Seg] * (1.f - F) + Path.Ground[Seg + 1] * F;
			FVector2f Start, Mid, End;
			if (F < 0.5f)
			{
				Start = (Path.Points[Seg - 1] + Path.Points[Seg]) / 2.f;
				Mid = Path.Points[Seg];
				End = (Path.Points[Seg] + Path.Points[Seg + 1]) / 2.f;
				F += 0.5f;
			}
			else
			{
				Start = (Path.Points[Seg] + Path.Points[Seg + 1]) / 2.f;
				Mid = Path.Points[Seg + 1];
				End = (Path.Points[Seg + 1] + Path.Points[Seg + 2]) / 2.f;
				F -= 0.5f;
			}
			ScriptPos = Start + F * (End - Start) + (1.f - F) * F * (Mid - End + Mid - Start);
		}
	}
	if (!IsScriptMoving() && !bScriptHold)
	{
		EndScript();
		return;
	}
	ApplyScriptView();
}

void AGenCameraPawn::ApplyScriptView()
{
	// W3DView::buildCameraTransform: the eye at OffsetZ * zoom, looking at the ground under the spot; FXPitch below
	// 1 raises the point looked at (0: straight ahead, below 0: up), above 1 brings the eye in over it.
	const FVector2f Spot = ScriptPos + ShakeOffset;
	const float EyeZ = ScriptOffsetZ * ScriptZoom;
	const float Factor = 1.f - ScriptGround / FMath::Max(EyeZ, 1.f);
	const float Back = ScriptOffsetZ / FMath::Tan(FMath::DegreesToRadians(CameraPitch)) * ScriptZoom * Factor;
	FVector2f Eye = Spot + FVector2f(Back * FMath::Sin(ScriptAngle), -Back * FMath::Cos(ScriptAngle));
	float LookZ = ScriptGround;
	if (ScriptFXPitch <= 1.f)
	{
		LookZ = EyeZ - (EyeZ - ScriptGround) * ScriptFXPitch;
	}
	else
	{
		Eye = Spot + (Eye - Spot) / ScriptFXPitch;
	}
	Target = GenCoords::ToUE(ScriptPos.X, ScriptPos.Y, ScriptGround);
	GroundZ = Target.Z;
	Yaw = FMath::RadiansToDegrees(ScriptAngle);
	SetActorLocation(Target);
	const FVector EyeUE = GenCoords::ToUE(Eye.X, Eye.Y, EyeZ);
	const FVector LookUE = GenCoords::ToUE(Spot.X, Spot.Y, LookZ);
	Camera->SetWorldLocation(EyeUE);
	Camera->SetWorldRotation((LookUE - EyeUE).Rotation());
}
