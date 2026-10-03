#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

namespace
{
	constexpr double ClientFPS = 30.0;
	constexpr int32 MaxScorches = 500;      // MAX_SCORCH_MARKS
	constexpr int32 MaxPulseLights = 32;
	/** Manual exposure scales the scene by 1/1.2 (see AGenMapActor::SetupLighting). */
	constexpr float ExposureCompensation = 1.2f;
	const TCHAR* ScorchTexture = TEXT("EXScorch01.tga");

	FORCEINLINE FVector ToUE(const FVector3f& G) { return GenCoords::ToUE(G.X, G.Y, G.Z); }

	void AddQuad(TArray<FVector>& V, TArray<FVector2D>& UV, TArray<FLinearColor>& C,
		const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, const FLinearColor& C01, const FLinearColor& C23)
	{
		V.Add(P0); V.Add(P1); V.Add(P2); V.Add(P3);
		UV.Add(FVector2D(0, 0)); UV.Add(FVector2D(0, 1)); UV.Add(FVector2D(1, 1)); UV.Add(FVector2D(1, 0));
		C.Add(C01); C.Add(C01); C.Add(C23); C.Add(C23);
	}

	void QuadIndices(int32 NumQuads, TArray<int32>& Out)
	{
		Out.SetNumUninitialized(NumQuads * 6);
		for (int32 q = 0; q < NumQuads; ++q)
		{
			const int32 v = q * 4;
			int32* I = &Out[q * 6];
			I[0] = v; I[1] = v + 1; I[2] = v + 2;
			I[3] = v; I[4] = v + 2; I[5] = v + 3;
		}
	}
}

UGenFXManager* UGenFXManager::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UGenFXManager>() : nullptr;
}

bool UGenFXManager::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UGenFXManager::Deinitialize()
{
	Particles.Reset();
	Particles.ObjectTransform = nullptr;
	Particles.GroundHeight = nullptr;
	Batches.Reset();
	BatchLookup.Reset();
	Tracers.Reset();
	Pulses.Reset();
	MapActor = nullptr;
	Logic = nullptr;
	Super::Deinitialize();
}

void UGenFXManager::Start(AGenMapActor* InMapActor, UGenGameLogic* InLogic)
{
	MapActor = InMapActor;
	Logic = InLogic;
	FGenFXData::Get().Initialize();
	Particles.Reset();
	Particles.ObjectTransform = [this](FGenObjectID Id, FTransform3f& Out)
	{
		const FGenObject* O = Logic ? Logic->FindObject(Id) : nullptr;
		if (!O || O->IsDestroyed())
		{
			return false;
		}
		Out = O->GetTransform();
		return true;
	};
	Particles.GroundHeight = [this](float X, float Y) { return Logic ? Logic->GetGroundHeight(X, Y) : 0.f; };
}

// ---- FX lists -----------------------------------------------------------------------------------

void UGenFXManager::DoParticleNugget(const FGenFXNugget& N, const FVector3f& Primary, const FTransform3f* Xf, const FGenObject* AttachTo, float OverrideRadius)
{
	// ParticleSystemFXNugget::reallyDoFX
	const FGenParticleSystemTemplate* Template = FGenFXData::Get().FindParticleSystem(N.Name);
	if (!Template)
	{
		return;
	}
	FRandomStream& R = Particles.GetRandom();
	const FVector3f Offset = Xf ? Xf->TransformVectorNoScale(N.Offset) : N.Offset;
	for (int32 i = 0; i < N.Count; ++i)
	{
		FGenParticleSystem* Sys = Particles.Create(Template);
		if (!Sys)
		{
			continue;
		}
		const float Radius = N.Radius.Get(R);
		const float Angle = R.FRandRange(0.f, 2.f * PI);
		FVector3f NewPos(Primary.X + Offset.X + Radius * FMath::Cos(Angle), Primary.Y + Offset.Y + Radius * FMath::Sin(Angle), 0.f);
		NewPos.Z = (N.bCreateAtGroundHeight && Logic) ? Logic->GetGroundHeight(NewPos.X, NewPos.Y) : Primary.Z + Offset.Z + N.Height.Get(R);

		if (N.bOrientToObject && Xf) { Sys->SetLocalTransform(*Xf); }
		if (N.RotateX != 0.f) { Sys->RotateLocal(FVector3f::ForwardVector, N.RotateX); }
		if (N.RotateY != 0.f) { Sys->RotateLocal(FVector3f::RightVector, N.RotateY); }
		if (N.RotateZ != 0.f) { Sys->RotateLocal(FVector3f::UpVector, N.RotateZ); }

		if (N.bAttachToObject && AttachTo) { Sys->AttachToObject(AttachTo->GetID()); }
		else { Sys->SetPosition(NewPos); }

		const float DelayMs = N.InitialDelayMs.Get(R);
		if (DelayMs >= 0.f)
		{
			Sys->SetInitialDelay(uint32(FMath::CeilToInt32(DelayMs * float(ClientFPS) / 1000.f)));
		}
		if (N.bUseCallersRadius && OverrideRadius > 0.f)
		{
			Sys->SetEmissionRadius(OverrideRadius);
		}
	}
}

void UGenFXManager::DoNuggetPos(const FGenFXNugget& N, const FVector3f& Primary, const FTransform3f* PrimaryXf, float PrimarySpeed,
	const FVector3f* Secondary, float OverrideRadius)
{
	FRandomStream& R = Particles.GetRandom();
	switch (N.Type)
	{
	case EGenFXNugget::ParticleSystem:
		DoParticleNugget(N, Primary, PrimaryXf, nullptr, OverrideRadius);
		break;
	case EGenFXNugget::Tracer:
	{
		if (!Secondary || N.Probability <= R.FRandRange(0.f, 1.f))
		{
			break;
		}
		// The passed-in speed is per logic frame; tracers move per 30 Hz client frame.
		const float Speed = N.Speed != 0.f ? N.Speed : PrimarySpeed * float(GenLogic::GetLogicFPS()) / float(ClientFPS);
		const FVector3f Delta = *Secondary - Primary;
		const float Dist = Delta.Size() - N.Length;
		const float Frames = (Dist >= 0.f && Speed > 0.f) ? Dist / Speed : 1.f;
		FTracer& T = Tracers.AddDefaulted_GetRef();
		T.Pos = T.LastPos = Primary;
		T.Dir = Delta.GetSafeNormal();
		T.Speed = Speed;
		T.Length = N.Length;
		T.Width = N.Width;
		T.Color = N.Color;
		T.FramesLeft = FMath::Max(1, FMath::CeilToInt32(Frames * N.DecayAt));
		break;
	}
	case EGenFXNugget::LightPulse:
		AddLightPulse(Primary, N.Color, N.EffectRadius, N.IncreaseFrames, N.DecreaseFrames);
		break;
	case EGenFXNugget::ViewShake:
		Shake(Primary, N.Variant);
		break;
	case EGenFXNugget::TerrainScorch:
		AddScorch(Primary, N.EffectRadius, N.Variant < 0 ? R.RandRange(0, 3) : N.Variant);
		break;
	default:
		break; // Sound (audio milestone), RayEffect, FXListAtBonePos (object form only)
	}
}

void UGenFXManager::DoFXPos(const FGenFXList* FX, const FVector3f& Primary, const FTransform3f* PrimaryXf, float PrimarySpeed,
	const FVector3f* Secondary, float OverrideRadius)
{
	if (!FX || !MapActor)
	{
		return;
	}
	for (const FGenFXNugget& N : FX->Nuggets)
	{
		DoNuggetPos(N, Primary, PrimaryXf, PrimarySpeed, Secondary, OverrideRadius);
	}
}

void UGenFXManager::DoFXObj(const FGenFXList* FX, const FGenObject* Primary, const FGenObject* Secondary)
{
	if (!FX || !Primary || !MapActor)
	{
		return;
	}
	const FVector3f Pos = Primary->GetPosition();
	const FTransform3f Xf = Primary->GetTransform();
	const FVector3f SecondaryPos = Secondary ? Secondary->GetPosition() : FVector3f::ZeroVector;
	for (const FGenFXNugget& N : FX->Nuggets)
	{
		if (N.Type == EGenFXNugget::ParticleSystem)
		{
			if (N.bRicochet && Secondary)
			{
				// Face away from the attacker, so the hit carries through like a ricochet.
				const FTransform3f Aim(FQuat4f(FVector3f::UpVector, FMath::Atan2(Pos.Y - SecondaryPos.Y, Pos.X - SecondaryPos.X)));
				DoParticleNugget(N, Pos, &Aim, Primary, 0.f);
			}
			else
			{
				DoParticleNugget(N, Pos, &Xf, Primary, 0.f);
			}
		}
		else if (N.Type == EGenFXNugget::LightPulse)
		{
			const float Radius = N.RadiusPctOfObject > 0.f ? Primary->GetTemplate()->GetBoundingCircleRadius() * N.RadiusPctOfObject : N.EffectRadius;
			AddLightPulse(Pos, N.Color, Radius, N.IncreaseFrames, N.DecreaseFrames);
		}
		else
		{
			DoNuggetPos(N, Pos, &Xf, 0.f, Secondary ? &SecondaryPos : nullptr, 0.f);
		}
	}
}

bool UGenFXManager::PlayByName(const FString& Name, const FVector3f& Pos)
{
	if (const FGenFXList* List = FGenFXData::Get().FindFXList(Name))
	{
		// As a weapon would: facing north, with a target 100 units ahead.
		const FTransform3f Xf(FQuat4f(FVector3f::UpVector, PI / 2.f), Pos);
		const FVector3f Target = Pos + FVector3f(0.f, 100.f, 0.f);
		DoFXPos(List, Pos, &Xf, 10.f, &Target, 0.f);
		return true;
	}
	if (FGenParticleSystem* Sys = MapActor ? Particles.Create(Name) : nullptr)
	{
		Sys->SetPosition(Pos);
		return true;
	}
	return false;
}

FGenParticleSystem* UGenFXManager::CreateAttachedSystem(const FString& TemplateName, FGenObjectID Object)
{
	FGenParticleSystem* Sys = MapActor ? Particles.Create(TemplateName) : nullptr;
	if (Sys)
	{
		Sys->AttachToObject(Object);
	}
	return Sys;
}

void UGenFXManager::OnObjectDestroyed(FGenObjectID Object)
{
	Particles.DestroyAttachedTo(Object);
}

void UGenFXManager::AddLightPulse(const FVector3f& Pos, const FLinearColor& Color, float Radius, int32 IncreaseFrames, int32 DecreaseFrames)
{
	if (Radius <= 0.f || IncreaseFrames + DecreaseFrames <= 0)
	{
		return;
	}
	if (Pulses.Num() >= MaxPulseLights)
	{
		Pulses.RemoveAt(0);
	}
	FLightPulse& P = Pulses.AddDefaulted_GetRef();
	P.Pos = Pos;
	P.Color = Color;
	P.Radius = Radius;
	P.IncreaseFrames = IncreaseFrames;
	P.DecreaseFrames = DecreaseFrames;
	P.StartTime = ClientTime;
}

void UGenFXManager::Shake(const FVector3f& Epicenter, int32 Type)
{
	// W3DView::shake: intensity falls off with distance from the point the camera looks at.
	const FGenFXData& Data = FGenFXData::Get();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Cam)
	{
		return;
	}
	const FVector View = GenCoords::FromUE(Cam->GetTarget());
	const float Dist = FVector2f(Epicenter.X - float(View.X), Epicenter.Y - float(View.Y)).Size();
	if (Dist > Data.MaxShakeRange)
	{
		return;
	}
	const float Angle = Particles.GetRandom().FRandRange(0.f, 2.f * PI);
	ShakeDir = FVector2f(FMath::Cos(Angle), FMath::Sin(Angle));
	ShakeIntensity += Data.ShakeIntensity[FMath::Clamp(Type, 0, 5)] * (1.f - Dist / Data.MaxShakeRange);
	if (ShakeIntensity > Data.MaxShakeIntensity)
	{
		ShakeIntensity = 3.f; // as in the original: over the limit snaps to 3, not to the limit
	}
}

void UGenFXManager::AddScorch(const FVector3f& Pos, float Radius, int32 Type)
{
	if (Radius <= 0.f)
	{
		return;
	}
	if (Scorches.Num() >= MaxScorches)
	{
		Scorches.RemoveAt(0);
	}
	Scorches.Add({ Pos, Radius, FMath::Clamp(Type, 0, 8) });
	bScorchDirty = true;
}

// ---- update -------------------------------------------------------------------------------------

void UGenFXManager::StepClientFrame()
{
	Particles.Step();

	// W3DTracerDraw: fly forward, fading out towards the expiration frame.
	for (int32 i = Tracers.Num() - 1; i >= 0; --i)
	{
		FTracer& T = Tracers[i];
		T.LastPos = T.Pos;
		T.LastOpacity = T.Opacity;
		T.Opacity -= T.Opacity / float(FMath::Max(1, T.FramesLeft));
		T.Pos += T.Dir * T.Speed;
		if (--T.FramesLeft <= 0)
		{
			Tracers.RemoveAtSwap(i);
		}
	}

	// W3DView: a spring so stiff it pulls the camera to the opposite side every frame.
	LastShakeOffset = ShakeOffset;
	if (ShakeIntensity > 0.01f)
	{
		ShakeOffset = ShakeDir * ShakeIntensity;
		ShakeIntensity *= 0.75f;
		ShakeDir = -ShakeDir;
	}
	else
	{
		ShakeIntensity = 0.f;
		ShakeOffset = FVector2f::ZeroVector;
	}
}

void UGenFXManager::Update(double LogicFrameTime)
{
	if (!MapActor)
	{
		return;
	}
	ClientTime = LogicFrameTime * ClientFPS / double(GenLogic::GetLogicFPS());
	const int64 Target = int64(FMath::FloorToDouble(ClientTime));
	int32 Steps = 0;
	while (ClientFrame < Target)
	{
		if (++Steps > 8)
		{
			ClientFrame = Target; // after a long hitch: don't try to catch up frame by frame
			break;
		}
		StepClientFrame();
		++ClientFrame;
	}
	FrameAlpha = float(ClientTime - double(Target));

	RenderParticles();
	FlushBatches();
	UpdateLights();
	if (bScorchDirty)
	{
		bScorchDirty = false;
		RebuildScorchMesh();
	}

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (AGenCameraPawn* Cam = Cast<AGenCameraPawn>(PC->GetPawn()))
		{
			Cam->SetShakeOffset(FMath::Lerp(LastShakeOffset, ShakeOffset, FrameAlpha));
		}
	}
}

// ---- rendering ----------------------------------------------------------------------------------

UMaterialInterface* UGenFXManager::GetParticleMaterial(EGenParticleShader Shader)
{
	if (TObjectPtr<UMaterialInterface>* Found = BaseMaterials.Find(uint8(Shader)))
	{
		return *Found;
	}
	const TCHAR* Name = Shader == EGenParticleShader::Additive ? TEXT("MI_Particle_Additive")
		: Shader == EGenParticleShader::AlphaTest ? TEXT("MI_Particle_Masked")
		: Shader == EGenParticleShader::Multiply ? TEXT("MI_Particle_Multiply") : TEXT("MI_Particle_Alpha");
	UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Generals/Materials/%s.%s"), Name, Name));
	if (!M)
	{
		UE_LOG(LogGenerals, Error, TEXT("Particle material %s is missing: run Tools/ue_setup.py"), Name);
	}
	BaseMaterials.Add(uint8(Shader), M);
	return M;
}

int32 UGenFXManager::GetBatch(const FString& Texture, EGenParticleShader Shader, bool bFlat, bool bSprite)
{
	const FString Key = FString::Printf(TEXT("%s|%d|%d|%d"), *Texture.ToLower(), int32(Shader), bFlat ? 1 : 0, bSprite ? 1 : 0);
	if (const int32* Found = BatchLookup.Find(Key))
	{
		return *Found;
	}
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(MapActor, NAME_None, RF_Transient);
	C->SetupAttachment(MapActor->GetRootComponent());
	C->SetUsingAbsoluteLocation(true);
	C->SetUsingAbsoluteRotation(true);
	C->SetUsingAbsoluteScale(true);
	C->SetCastShadow(false);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->bReceivesDecals = false;
	// Darkening first, then smoke, then glows on top.
	C->SetTranslucentSortPriority(Shader == EGenParticleShader::Multiply ? -1 : (Shader == EGenParticleShader::Additive ? 1 : 0));
	C->RegisterComponent();
	C->SetVisibility(false);

	UMaterialInstanceDynamic* MID = nullptr;
	if (UMaterialInterface* Base = GetParticleMaterial(Shader))
	{
		MID = UMaterialInstanceDynamic::Create(Base, this);
		if (UTexture2D* Tex = Texture.IsEmpty() ? nullptr : UGenAssetSubsystem::Get()->GetTexture(Texture))
		{
			MID->SetTextureParameterValue(TEXT("BaseTexture"), Tex);
		}
		else if (!Texture.IsEmpty())
		{
			UE_LOG(LogGenerals, Warning, TEXT("Particle texture '%s' not found"), *Texture);
		}
		const bool bUsesAlpha = Shader == EGenParticleShader::Alpha || Shader == EGenParticleShader::AlphaTest;
		MID->SetScalarParameterValue(TEXT("UseAlpha"), bUsesAlpha ? 1.f : 0.f);
		// The original added colors in gamma space. Adding the gamma-space product as-is keeps fire and
		// flashes as bright against mid-tone terrain as they were (see M_Particle in Tools/ue_setup.py).
		MID->SetScalarParameterValue(TEXT("GammaAdd"), Shader == EGenParticleShader::Additive ? 1.f : 0.f);
		MID->SetScalarParameterValue(TEXT("Exposure"), Shader == EGenParticleShader::Multiply ? 1.f : ExposureCompensation);
		// Quads lying flat on the terrain would fade themselves out entirely.
		MID->SetScalarParameterValue(TEXT("SoftFade"), bFlat ? 0.f : 1.f);
		// Ribbons and tracers run their texture edge to edge; only stand-alone sprites get their borders faded.
		MID->SetScalarParameterValue(TEXT("EdgeFade"), bSprite ? 1.f : 0.f);
		C->SetMaterial(0, MID);
	}

	const int32 Index = Batches.AddDefaulted();
	BatchComponents.Add(C);
	BatchMaterials.Add(MID);
	BatchLookup.Add(Key, Index);
	return Index;
}

void UGenFXManager::RenderParticles()
{
	for (FBatch& B : Batches)
	{
		B.Vertices.Reset();
		B.UVs.Reset();
		B.Colors.Reset();
	}
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector CamPos = PC->PlayerCameraManager->GetCameraLocation();
	const FRotationMatrix CamRot(PC->PlayerCameraManager->GetCameraRotation());
	const FVector Right = CamRot.GetUnitAxis(EAxis::Y);
	const FVector Up = CamRot.GetUnitAxis(EAxis::Z);
	const float A = FrameAlpha;
	const double Cm = GenCoords::UnitsToCm;

	for (const TUniquePtr<FGenParticleSystem>& Sys : Particles.GetSystems())
	{
		const TArray<FGenParticle>& Ps = Sys->GetParticles();
		if (Ps.Num() == 0)
		{
			continue;
		}
		const FGenParticleSystemTemplate& T = Sys->GetTemplate();
		if (Sys->BatchIndex == INDEX_NONE)
		{
			const bool bStreak = T.Type == EGenParticleType::Streak;
			Sys->BatchIndex = GetBatch(T.ParticleName, T.Shader, T.bGroundAligned && !bStreak, !bStreak);
		}
		FBatch& B = Batches[Sys->BatchIndex];

		if (T.Type == EGenParticleType::Streak)
		{
			// A ribbon through the particles in creation order; the oldest end fades to nothing.
			for (int32 i = 0; i + 1 < Ps.Num(); ++i)
			{
				const FGenParticle& P0 = Ps[i];
				const FGenParticle& P1 = Ps[i + 1];
				const FVector C0 = ToUE(FMath::Lerp(P0.LastPos, P0.Pos, A));
				const FVector C1 = ToUE(FMath::Lerp(P1.LastPos, P1.Pos, A));
				const FVector Side = FVector::CrossProduct(C1 - C0, CamPos - C0).GetSafeNormal();
				const FVector S0 = Side * (FMath::Lerp(P0.LastSize, P0.Size, A) * 0.5 * Cm);
				const FVector S1 = Side * (FMath::Lerp(P1.LastSize, P1.Size, A) * 0.5 * Cm);
				const FVector3f K0 = FMath::Lerp(P0.LastColor, P0.Color, A);
				const FVector3f K1 = FMath::Lerp(P1.LastColor, P1.Color, A);
				const FLinearColor Col0 = i == 0 ? FLinearColor(0, 0, 0, 0) : FLinearColor(K0.X, K0.Y, K0.Z, FMath::Lerp(P0.LastAlpha, P0.Alpha, A));
				const FLinearColor Col1(K1.X, K1.Y, K1.Z, FMath::Lerp(P1.LastAlpha, P1.Alpha, A));
				AddQuad(B.Vertices, B.UVs, B.Colors, C0 - S0, C0 + S0, C1 + S1, C1 - S1, Col0, Col1);
			}
			continue;
		}

		for (const FGenParticle& P : Ps)
		{
			const float Size = FMath::Lerp(P.LastSize, P.Size, A);
			if (Size <= 0.f)
			{
				continue;
			}
			const FVector Center = ToUE(FMath::Lerp(P.LastPos, P.Pos, A));
			const float Angle = FMath::Lerp(P.LastAngleZ, P.AngleZ, A);
			const float Cs = FMath::Cos(Angle), Sn = FMath::Sin(Angle);
			const FVector3f K = FMath::Lerp(P.LastColor, P.Color, A);
			const FLinearColor Col(K.X, K.Y, K.Z, FMath::Lerp(P.LastAlpha, P.Alpha, A));
			if (T.bGroundAligned)
			{
				// PointGroupClass non-billboard quads: +-size in the world XY plane, rotated about Z.
				const double S = double(Size) * Cm;
				const FVector AxX = FVector(Cs, -Sn, 0.0) * S;   // Generals (cos, sin) mirrored on Y
				const FVector AxY = FVector(-Sn, -Cs, 0.0) * S;  // Generals (-sin, cos)
				const FVector Flat = Center + FVector(0.0, 0.0, 2.0); // a hair above the terrain it may be coplanar with
				AddQuad(B.Vertices, B.UVs, B.Colors, Flat + AxX + AxY, Flat + AxX - AxY, Flat - AxX - AxY, Flat - AxX + AxY, Col, Col);
			}
			else
			{
				// Screen-aligned quad of edge length Size, rotated about the view axis.
				const double H = double(Size) * 0.5 * Cm;
				const FVector AxX = (Right * Cs + Up * Sn) * H;
				const FVector AxY = (Up * Cs - Right * Sn) * H;
				AddQuad(B.Vertices, B.UVs, B.Colors, Center - AxX + AxY, Center - AxX - AxY, Center + AxX - AxY, Center + AxX + AxY, Col, Col);
			}
		}
	}

	if (Tracers.Num() > 0)
	{
		// A tracer is a thin solid line; keep it at least ~1.5 pixels wide so it doesn't shimmer when zoomed out.
		FVector2D ViewSize(1920.0, 1080.0);
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->GetViewportSize(ViewSize);
		}
		const double PixelAngle = 2.0 * FMath::Tan(FMath::DegreesToRadians(double(PC->PlayerCameraManager->GetFOVAngle())) * 0.5) / FMath::Max(1.0, ViewSize.X);
		FBatch& B = Batches[GetBatch(FString(), EGenParticleShader::Alpha, false, false)];
		for (const FTracer& T : Tracers)
		{
			const FVector Tail = ToUE(FMath::Lerp(T.LastPos, T.Pos, A));
			const FVector Head = Tail + ToUE(T.Dir * T.Length);
			const double MinWidth = 1.5 * PixelAngle * FVector::Dist(CamPos, Tail);
			const FVector Side = FVector::CrossProduct(Head - Tail, CamPos - Tail).GetSafeNormal() * (FMath::Max(double(T.Width) * Cm, MinWidth) * 0.5);
			const FLinearColor Col(T.Color.R, T.Color.G, T.Color.B, FMath::Lerp(T.LastOpacity, T.Opacity, A));
			AddQuad(B.Vertices, B.UVs, B.Colors, Tail - Side, Tail + Side, Head + Side, Head - Side, Col, Col);
		}
	}
}

void UGenFXManager::FlushBatches()
{
	TArray<int32> Indices;
	for (int32 i = 0; i < Batches.Num(); ++i)
	{
		FBatch& B = Batches[i];
		UProceduralMeshComponent* C = BatchComponents[i];
		const int32 NumQuads = B.Vertices.Num() / 4;
		if (NumQuads == 0 || !C)
		{
			if (B.bShown && C)
			{
				C->SetVisibility(false);
				B.bShown = false;
			}
			continue;
		}
		// The section keeps a fixed vertex count (unused quads collapse to a point), so most frames only
		// update vertex data; it is re-created only when it has to grow.
		const bool bGrow = NumQuads > B.Capacity;
		if (bGrow)
		{
			B.Capacity = FMath::Max<int32>(32, FMath::RoundUpToPowerOfTwo(uint32(NumQuads)));
		}
		const int32 NumVerts = B.Capacity * 4;
		const FVector Pad = B.Vertices.Last();
		const int32 Used = B.Vertices.Num();
		B.Vertices.SetNumUninitialized(NumVerts);
		for (int32 v = Used; v < NumVerts; ++v)
		{
			B.Vertices[v] = Pad;
		}
		B.UVs.SetNumZeroed(NumVerts);
		B.Colors.SetNumZeroed(NumVerts);
		if (bGrow)
		{
			QuadIndices(B.Capacity, Indices);
			C->CreateMeshSection_LinearColor(0, B.Vertices, Indices, TArray<FVector>(), B.UVs, B.Colors, TArray<FProcMeshTangent>(), false, false);
		}
		else
		{
			C->UpdateMeshSection_LinearColor(0, B.Vertices, TArray<FVector>(), B.UVs, B.Colors, TArray<FProcMeshTangent>(), false);
		}
		if (!B.bShown)
		{
			C->SetVisibility(true);
			B.bShown = true;
		}
	}
}

void UGenFXManager::UpdateLights()
{
	for (int32 i = Pulses.Num() - 1; i >= 0; --i)
	{
		if (ClientTime - Pulses[i].StartTime >= double(Pulses[i].IncreaseFrames + Pulses[i].DecreaseFrames))
		{
			Pulses.RemoveAt(i);
		}
	}
	while (PulseLights.Num() < Pulses.Num())
	{
		UPointLightComponent* L = NewObject<UPointLightComponent>(MapActor, NAME_None, RF_Transient);
		L->SetupAttachment(MapActor->GetRootComponent());
		L->SetMobility(EComponentMobility::Movable);
		L->bUseInverseSquaredFalloff = false;
		L->LightFalloffExponent = 2.f;
		L->SetCastShadows(false);
		L->RegisterComponent();
		PulseLights.Add(L);
	}
	for (int32 i = 0; i < PulseLights.Num(); ++i)
	{
		UPointLightComponent* L = PulseLights[i];
		if (!Pulses.IsValidIndex(i))
		{
			if (L->IsVisible()) { L->SetVisibility(false); }
			continue;
		}
		const FLightPulse& P = Pulses[i];
		const float Age = float(ClientTime - P.StartTime);
		const float Factor = Age < float(P.IncreaseFrames) ? Age / float(FMath::Max(1, P.IncreaseFrames))
			: 1.f - (Age - float(P.IncreaseFrames)) / float(FMath::Max(1, P.DecreaseFrames));
		// Lifted above the flash point so the light reaches the ground around it instead of grazing it.
		L->SetWorldLocation(ToUE(P.Pos) + FVector(0.0, 0.0, double(P.Radius) * 0.4 * GenCoords::UnitsToCm));
		L->SetAttenuationRadius(P.Radius * 1.5f * float(GenCoords::UnitsToCm));
		L->SetLightColor(FLinearColor(FMath::Pow(P.Color.R, 2.2f), FMath::Pow(P.Color.G, 2.2f), FMath::Pow(P.Color.B, 2.2f)));
		L->SetIntensity(PI * ExposureCompensation * FMath::Clamp(Factor, 0.f, 1.f));
		if (!L->IsVisible()) { L->SetVisibility(true); }
	}
}

void UGenFXManager::RebuildScorchMesh()
{
	// BaseHeightMapRenderObjClass::updateScorches: each mark is a patch of terrain cells textured from a
	// 3x3 atlas (marks spaced 1.5 apart in a 4-unit-wide texture), tinted by the terrain lighting.
	const FGenMapData* Map = MapActor ? MapActor->GetMapData() : nullptr;
	if (!Map)
	{
		return;
	}
	if (!ScorchMesh)
	{
		ScorchMesh = NewObject<UProceduralMeshComponent>(MapActor, NAME_None, RF_Transient);
		ScorchMesh->SetupAttachment(MapActor->GetRootComponent());
		ScorchMesh->SetCastShadow(false);
		ScorchMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ScorchMesh->bReceivesDecals = false;
		ScorchMesh->SetTranslucentSortPriority(-10);
		ScorchMesh->RegisterComponent();
		if (UMaterialInterface* Base = GetParticleMaterial(EGenParticleShader::Alpha))
		{
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
			if (UTexture2D* Tex = UGenAssetSubsystem::Get()->GetTexture(ScorchTexture))
			{
				MID->SetTextureParameterValue(TEXT("BaseTexture"), Tex);
			}
			MID->SetScalarParameterValue(TEXT("UseAlpha"), 1.f);
			MID->SetScalarParameterValue(TEXT("Exposure"), ExposureCompensation);
			MID->SetScalarParameterValue(TEXT("SoftFade"), 0.f);
			MID->SetScalarParameterValue(TEXT("EdgeFade"), 0.f);
			BatchMaterials.Add(MID);
			ScorchMesh->SetMaterial(0, MID);
		}
	}

	FLinearColor Shade(0.6f, 0.6f, 0.6f, 1.f);
	if (const FGenMapLighting* L = Map->GetCurrentLighting())
	{
		if (L->Terrain.Num() > 0)
		{
			Shade = L->Terrain[0].Ambient + L->Terrain[0].Diffuse * 0.5f;
			Shade.A = 1.f;
		}
	}

	TArray<FVector> V;
	TArray<FVector2D> UV;
	TArray<FLinearColor> C;
	TArray<int32> Tri;
	const float Cell = GenCoords::MapXYFactor;
	const float Lift = 0.35f; // keeps the patch clear of the terrain's own triangles
	for (const FScorch& S : Scorches)
	{
		const int32 MinX = FMath::FloorToInt32((S.Pos.X - S.Radius) / Cell), MaxX = FMath::CeilToInt32((S.Pos.X + S.Radius) / Cell);
		const int32 MinY = FMath::FloorToInt32((S.Pos.Y - S.Radius) / Cell), MaxY = FMath::CeilToInt32((S.Pos.Y + S.Radius) / Cell);
		const int32 NX = MaxX - MinX + 1, NY = MaxY - MinY + 1;
		if (NX < 2 || NY < 2 || NX > 64 || NY > 64)
		{
			continue;
		}
		const float UOffset = float(S.Type % 3) * 1.5f, VOffset = float(S.Type / 3) * 1.5f;
		const int32 Base = V.Num();
		for (int32 j = MinY; j <= MaxY; ++j)
		{
			for (int32 i = MinX; i <= MaxX; ++i)
			{
				const float X = float(i) * Cell, Y = float(j) * Cell;
				V.Add(GenCoords::ToUE(X, Y, Map->GetHeightAt(i + Map->Border, j + Map->Border) + Lift));
				UV.Add(FVector2D((UOffset + 0.5f + (X - S.Pos.X) / (2.f * S.Radius)) / 4.f, (VOffset + 0.5f + (Y - S.Pos.Y) / (2.f * S.Radius)) / 4.f));
				C.Add(Shade);
			}
		}
		for (int32 j = 0; j < NY - 1; ++j)
		{
			for (int32 i = 0; i < NX - 1; ++i)
			{
				const int32 v = Base + j * NX + i;
				Tri.Append({ v, v + NX, v + 1, v + 1, v + NX, v + NX + 1 });
			}
		}
	}
	ScorchMesh->ClearAllMeshSections();
	if (Tri.Num() > 0)
	{
		ScorchMesh->CreateMeshSection_LinearColor(0, V, Tri, TArray<FVector>(), UV, C, TArray<FProcMeshTangent>(), false, false);
	}
}
