#include "Client/GenFXManager.h"
#include "Client/GenQuadBatchComponent.h"
#include "Core/GenPerf.h"
#include "Audio/GenAudioManager.h"
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
#include "Engine/Texture2D.h"
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

	// The vertices as the renderer takes them: positions relative to the map, colours as bytes (linear,
	// clamped - not gamma encoded: the material works on them as they are).
	void AddQuad(TArray<FVector3f>& V, TArray<FVector2f>& UV, TArray<FColor>& C,
		const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, const FLinearColor& C01, const FLinearColor& C23)
	{
		V.Add(FVector3f(P0)); V.Add(FVector3f(P1)); V.Add(FVector3f(P2)); V.Add(FVector3f(P3));
		UV.Add(FVector2f(0.f, 0.f)); UV.Add(FVector2f(0.f, 1.f)); UV.Add(FVector2f(1.f, 1.f)); UV.Add(FVector2f(1.f, 0.f));
		const FColor K01 = C01.ToFColor(false), K23 = C23.ToFColor(false);
		C.Add(K01); C.Add(K01); C.Add(K23); C.Add(K23);
	}

	/** A quad with its own texture coordinates: U across (0..1), V along from V0 to V1. */
	void AddQuadUV(TArray<FVector3f>& V, TArray<FVector2f>& UV, TArray<FColor>& C,
		const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, double V0, double V1, const FLinearColor& Color)
	{
		V.Add(FVector3f(P0)); V.Add(FVector3f(P1)); V.Add(FVector3f(P2)); V.Add(FVector3f(P3));
		UV.Add(FVector2f(0.f, float(V0))); UV.Add(FVector2f(1.f, float(V0))); UV.Add(FVector2f(1.f, float(V1))); UV.Add(FVector2f(0.f, float(V1)));
		const FColor K = Color.ToFColor(false);
		C.Add(K); C.Add(K); C.Add(K); C.Add(K);
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
	Particles.ObjectShrouded = nullptr;
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
		Out = Logic->GetRenderTransform(*O); // (where it is on the screen, not where the step has got it to)
		return true;
	};
	Particles.GroundHeight = [this](float X, float Y) { return Logic ? Logic->GetGroundHeight(X, Y) : 0.f; };
	// ParticleSystem::update: a system on an object the fog hides doesn't emit.
	Particles.ObjectShrouded = [this](FGenObjectID Id)
	{
		const FGenObject* O = Logic && Logic->IsShroudOn() ? Logic->FindObject(Id) : nullptr;
		return O && O->GetLocalShroud() >= EGenObjectShroud::Fogged;
	};
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
	case EGenFXNugget::Sound:
		// SoundFXNugget::doFXPos
		if (UGenAudioManager* Audio = Logic ? Logic->GetAudio() : nullptr)
		{
			Audio->PlayAt(N.Name, Primary);
		}
		break;
	default:
		break; // RayEffect, FXListAtBonePos (object form only)
	}
}

void UGenFXManager::DoFXPos(const FGenFXList* FX, const FVector3f& Primary, const FTransform3f* PrimaryXf, float PrimarySpeed,
	const FVector3f* Secondary, float OverrideRadius)
{
	if (!FX || !MapActor)
	{
		return;
	}
	// FXList::doFXPos: nothing plays where the local player isn't looking.
	if (Logic && !Logic->IsPositionClear(Primary))
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
	// FXList::doFXObj: "the primary object is fogged or shrouded so don't bother with the effect".
	if (Logic && Logic->GetLocalShroudNow(*Primary) > EGenObjectShroud::PartialClear)
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
		else if (N.Type == EGenFXNugget::Sound)
		{
			// SoundFXNugget::doFXObj: the sound is the object's, and stays with it.
			if (UGenAudioManager* Audio = Logic ? Logic->GetAudio() : nullptr)
			{
				Audio->PlayForObject(N.Name, *Primary);
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

void UGenFXManager::CreateSystemAt(const FString& TemplateName, const FVector3f& Pos)
{
	if (FGenParticleSystem* Sys = MapActor ? Particles.Create(TemplateName) : nullptr)
	{
		Sys->SetPosition(Pos);
	}
}

void UGenFXManager::OnObjectDestroyed(FGenObjectID Object)
{
	Particles.DestroyAttachedTo(Object);
}

void UGenFXManager::DestroySystem(uint32 SystemID)
{
	if (FGenParticleSystem* Sys = Particles.Find(SystemID))
	{
		Sys->Destroy();
	}
}

FGenParticleSystem* UGenFXManager::FindSystem(uint32 SystemID)
{
	return SystemID != 0 ? Particles.Find(SystemID) : nullptr;
}

void UGenFXManager::SetSystemStopped(uint32 SystemID, bool bStopped)
{
	if (FGenParticleSystem* Sys = Particles.Find(SystemID))
	{
		if (bStopped) { Sys->Stop(); } else { Sys->Start(); }
	}
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
	if (!IsInGameThread())
	{
		PendingShakes.Add({ Epicenter, Type }); // (the simulation's thread doesn't look at the camera: Update does)
		return;
	}
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
	GEN_PERF_SCOPE("R.FX.Step");
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
	GEN_PERF_SCOPE("R.FX");
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

	if (PendingShakes.Num() > 0)
	{
		const TArray<TPair<FVector3f, int32>> Shakes = MoveTemp(PendingShakes);
		PendingShakes.Reset();
		for (const TPair<FVector3f, int32>& S : Shakes)
		{
			Shake(S.Key, S.Value);
		}
	}
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
	// Darkening first, then smoke, then glows on top - and all of them after the water and the glass of
	// the models (which have no priority of their own), as the original drew its particles last.
	const int32 Layer = Shader == EGenParticleShader::Multiply ? LayerMultiply : Shader == EGenParticleShader::Additive ? LayerAdditive
		: Shader == EGenParticleShader::AlphaTest ? LayerMasked : LayerAlpha;
	static_assert(UE_ARRAY_COUNT(LayerComponents) == NumLayers, "a component per layer");
	UGenQuadBatchComponent* C = LayerComponents[Layer];
	if (!C)
	{
		C = NewObject<UGenQuadBatchComponent>(MapActor, NAME_None, RF_Transient);
		C->SetupAttachment(MapActor->GetRootComponent());
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		C->SetTranslucentSortPriority(Layer);
		C->RegisterComponent();
		LayerComponents[Layer] = C;
	}

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
		C->AddMaterial(MID);
		BatchMaterials.Add(MID);
	}

	const int32 Index = Batches.AddDefaulted();
	Batches[Index].Material = MID;
	Batches[Index].Layer = Layer;
	BatchLookup.Add(Key, Index);
	return Index;
}

void UGenFXManager::RenderParticles()
{
	GEN_PERF_SCOPE("R.FX.Build");
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

	// What the camera takes in (and a little more): a system all of whose particles are outside it is
	// passed over. On a big map most of the smoke and fire is somewhere else.
	const FVector Forward = CamRot.GetUnitAxis(EAxis::X);
	FVector2D CullViewSize(16.0, 9.0);
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(CullViewSize);
	}
	const double TanX = FMath::Tan(FMath::DegreesToRadians(double(PC->PlayerCameraManager->GetFOVAngle())) * 0.5) * 1.08;
	const double TanY = TanX * FMath::Max(1.0, CullViewSize.Y) / FMath::Max(1.0, CullViewSize.X);
	auto InView = [&](const FVector3f& Center, float Radius)
	{
		const FVector D = ToUE(Center) - CamPos;
		const double R = double(Radius) * Cm + 100.0;
		const double Depth = FVector::DotProduct(D, Forward);
		return Depth > -R && FMath::Abs(FVector::DotProduct(D, Right)) <= FMath::Max(Depth, 0.0) * TanX + R * 1.5
			&& FMath::Abs(FVector::DotProduct(D, Up)) <= FMath::Max(Depth, 0.0) * TanY + R * 1.5;
	};
	int32 NumCulled = 0;

	for (const TUniquePtr<FGenParticleSystem>& Sys : Particles.GetSystems())
	{
		const TArray<FGenParticle>& Ps = Sys->GetParticles();
		if (Ps.Num() == 0)
		{
			continue;
		}
		if (!InView(Sys->BoundsCenter, Sys->BoundsRadius))
		{
			++NumCulled;
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

	GEN_PERF_COUNT("R.FX.SystemsOutOfView#", NumCulled);

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

	// W3DRopeDraw: the ropes of a combat drop. Each is a chain of straight pieces whose joints sway a
	// little, every one along its own axis; a solid line with a softer, wider one over it.
	TArray<FGenRopeDraw> Ropes;
	if (Logic)
	{
		Logic->GetVisibleRopes(Ropes);
	}
	if (Ropes.Num() > 0)
	{
		FVector2D ViewSize(1920.0, 1080.0);
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->GetViewportSize(ViewSize);
		}
		const double PixelAngle = 2.0 * FMath::Tan(FMath::DegreesToRadians(double(PC->PlayerCameraManager->GetFOVAngle())) * 0.5) / FMath::Max(1.0, ViewSize.X);
		FBatch& B = Batches[GetBatch(FString(), EGenParticleShader::Alpha, false, false)];
		for (const FGenRopeDraw& Rope : Ropes)
		{
			const int32 NumSegs = FMath::Clamp(FMath::CeilToInt32(Rope.LenMax / FMath::Max(1.f, FMath::Min(Rope.LenMax, Rope.WobbleLen))), 1, 64);
			const float EachLen = Rope.Len / float(NumSegs);
			const float Deflection = FMath::Sin(Rope.Phase) * Rope.WobbleAmp;
			FVector3f Start = Rope.Top;
			for (int32 Seg = 0; Seg < NumSegs; ++Seg)
			{
				// (The original draws a random axis per piece when the rope is made; any fixed one does.)
				const float Axis = float((Seg * 7919 + 13) % 628) * 0.01f;
				const FVector3f End(Rope.Top.X + Deflection * FMath::Cos(Axis), Rope.Top.Y + Deflection * FMath::Sin(Axis), Start.Z - EachLen);
				const FVector Tail = ToUE(Start), Head = ToUE(End);
				const FVector SideDir = FVector::CrossProduct(Head - Tail, CamPos - Tail).GetSafeNormal();
				const double MinWidth = 1.2 * PixelAngle * FVector::Dist(CamPos, Tail);
				const FVector Hard = SideDir * (FMath::Max(double(Rope.Width) * 0.5 * Cm, MinWidth) * 0.5);
				const FVector Soft = SideDir * (FMath::Max(double(Rope.Width) * Cm, MinWidth * 2.0) * 0.5);
				AddQuad(B.Vertices, B.UVs, B.Colors, Tail - Soft, Tail + Soft, Head + Soft, Head - Soft, FLinearColor(0.f, 0.f, 0.f, 0.5f), FLinearColor(0.f, 0.f, 0.f, 0.5f));
				AddQuad(B.Vertices, B.UVs, B.Colors, Tail - Hard, Tail + Hard, Head + Hard, Head - Hard, FLinearColor(0.f, 0.f, 0.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 1.f));
				Start = End;
			}
		}
	}

	// W3DLaserDraw: the beams units hold on their targets - a laser pointer, a hacker's stream of numbers.
	// Each is a few additive lines of growing width (the beam's glow), cut into segments when it arcs.
	TArray<UGenGameLogic::FBeam> Beams;
	if (Logic)
	{
		Logic->GetVisibleBeams(Beams);
	}
	if (Beams.Num() > 0)
	{
		FVector2D ViewSize(1920.0, 1080.0);
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->GetViewportSize(ViewSize);
		}
		const double PixelAngle = 2.0 * FMath::Tan(FMath::DegreesToRadians(double(PC->PlayerCameraManager->GetFOVAngle())) * 0.5) / FMath::Max(1.0, ViewSize.X);
		for (const UGenGameLogic::FBeam& Beam : Beams)
		{
			if (Beam.WidthScale <= 0.f)
			{
				continue;
			}
			const FGenLaserDraw& L = *Beam.Draw;
			UTexture2D* Tex = L.Texture.IsEmpty() ? nullptr : UGenAssetSubsystem::Get()->GetTexture(L.Texture);
			const double Aspect = (Tex && Tex->GetSizeY() > 0) ? double(Tex->GetSizeX()) / double(Tex->GetSizeY()) : 1.0;
			FBatch& B = Batches[GetBatch(Tex ? L.Texture : FString(), EGenParticleShader::Additive, false, false)];
			const FVector3f Line = Beam.End - Beam.Start;
			const float HalfLength = FMath::Max(0.01f, Line.Size() * 0.5f);
			const FVector3f Middle = (Beam.Start + Beam.End) * 0.5f;
			const bool bArc = L.ArcHeight > 0.f && L.Segments > 1;
			const int32 Segments = bArc ? L.Segments : 1;
			// ("amount to scroll texture on each draw": per client frame here)
			const double Scroll = FMath::Fmod(double(L.ScrollRate) * (double(ClientFrame) + double(FrameAlpha)), 1.0);
			for (int32 Seg = 0; Seg < Segments; ++Seg)
			{
				FVector3f P0 = Beam.Start, P1 = Beam.End;
				if (bArc)
				{
					float R0 = float(Seg) / float(Segments), R1 = float(Seg + 1) / float(Segments);
					if (Seg > 0) { R0 -= L.SegmentOverlapRatio; }
					if (Seg < Segments - 1) { R1 += L.SegmentOverlapRatio; }
					P0 = Beam.Start + Line * R0;
					P1 = Beam.Start + Line * R1;
					// Highest over the middle, down to nothing at both ends; never under the ground.
					P0.Z += FMath::Cos((Middle - P0).Size() / HalfLength * PI * 0.5f) * L.ArcHeight;
					P1.Z += FMath::Cos((Middle - P1).Size() / HalfLength * PI * 0.5f) * L.ArcHeight;
					P0.Z = FMath::Max(P0.Z, 2.f + Logic->GetGroundHeight(P0.X, P0.Y));
					P1.Z = FMath::Max(P1.Z, 2.f + Logic->GetGroundHeight(P1.X, P1.Y));
				}
				const FVector Tail = ToUE(P0), Head = ToUE(P1);
				const FVector SideDir = FVector::CrossProduct(Head - Tail, CamPos - Tail).GetSafeNormal();
				const double MinWidth = 1.5 * PixelAngle * FVector::Dist(CamPos, Tail);
				const float SegLength = (P1 - P0).Size();
				for (int32 i = L.NumBeams - 1; i >= 0; --i)
				{
					float Width = L.InnerBeamWidth * Beam.WidthScale;
					FLinearColor Col(L.InnerColor.R * L.InnerColor.A, L.InnerColor.G * L.InnerColor.A, L.InnerColor.B * L.InnerColor.A, 1.f);
					if (L.NumBeams > 1)
					{
						const float S = float(i) / float(L.NumBeams - 1);
						Width = (L.InnerBeamWidth + S * (L.OuterBeamWidth - L.InnerBeamWidth)) * Beam.WidthScale;
						Col = FLinearColor(L.InnerColor.R + S * (L.OuterColor.R - L.InnerColor.R) * L.InnerColor.A,
							L.InnerColor.G + S * (L.OuterColor.G - L.InnerColor.G) * L.InnerColor.A,
							L.InnerColor.B + S * (L.OuterColor.B - L.InnerColor.B) * L.InnerColor.A, 1.f);
					}
					// "number of times to tile texture across each segment"
					const double Tiles = (Tex && L.bTile && Width > 0.f) ? double(SegLength) / double(Width) * Aspect * double(L.TilingScalar) : 1.0;
					const FVector Side = SideDir * (FMath::Max(double(Width) * Cm, MinWidth) * 0.5);
					AddQuadUV(B.Vertices, B.UVs, B.Colors, Tail - Side, Tail + Side, Head + Side, Head - Side, Scroll, Scroll + Tiles, Col);
				}
			}
		}
	}

	// W3DProjectileStreamDraw: the toxin truck's spray, the ambulance's hose, the Dragon tank's flame - a strip of
	// texture through the shots in flight, added onto what is behind it (_PresetAdditiveSpriteShader).
	// TILED_TEXTURE_MAP: the texture TileFactor times along each segment, across the width once, and flowing
	// outwards at ScrollRate lengths a second ("UVOffsetDeltaPerMS = rate * 0.001").
	TArray<UGenGameLogic::FStreamLine> Streams;
	if (Logic)
	{
		Logic->GetVisibleStreams(Streams);
	}
	if (Streams.Num() > 0)
	{
		FVector2D ViewSize(1920.0, 1080.0);
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->GetViewportSize(ViewSize);
		}
		const double PixelAngle = 2.0 * FMath::Tan(FMath::DegreesToRadians(double(PC->PlayerCameraManager->GetFOVAngle())) * 0.5) / FMath::Max(1.0, ViewSize.X);
		TArray<FVector, TInlineAllocator<24>> P, Side;
		for (const UGenGameLogic::FStreamLine& S : Streams)
		{
			const FGenStreamDraw& D = *S.Draw;
			UTexture2D* Tex = D.Texture.IsEmpty() ? nullptr : UGenAssetSubsystem::Get()->GetTexture(D.Texture);
			FBatch& B = Batches[GetBatch(Tex ? D.Texture : FString(), EGenParticleShader::Additive, false, false)];
			const double Scroll = FMath::Fmod(double(D.ScrollRate) * ClientTime / ClientFPS, 1.0);
			const int32 N = S.Points.Num();
			P.SetNum(N);
			Side.SetNum(N);
			for (int32 i = 0; i < N; ++i)
			{
				P[i] = ToUE(S.Points[i]);
			}
			// (Each point's side the same for the segment before it and the one after: no gaps at the bends.)
			for (int32 i = 0; i < N; ++i)
			{
				const FVector Along = P[FMath::Min(i + 1, N - 1)] - P[FMath::Max(i - 1, 0)];
				const double MinWidth = 1.5 * PixelAngle * FVector::Dist(CamPos, P[i]);
				Side[i] = FVector::CrossProduct(Along, CamPos - P[i]).GetSafeNormal() * (FMath::Max(double(D.Width) * Cm, MinWidth) * 0.5);
			}
			for (int32 i = 0; i + 1 < N; ++i)
			{
				AddQuadUV(B.Vertices, B.UVs, B.Colors, P[i] - Side[i], P[i] + Side[i], P[i + 1] + Side[i + 1], P[i + 1] - Side[i + 1],
					double(i) * D.TileFactor + Scroll, double(i + 1) * D.TileFactor + Scroll, FLinearColor::White);
			}
		}
	}
}

void UGenFXManager::FlushBatches()
{
	GEN_PERF_SCOPE("R.FX.Flush");
	// A layer's batches one after the other in one run of vertices, which goes to the render thread as
	// it is: one upload per layer, and a draw per batch out of it.
	for (int32 Layer = 0; Layer < NumLayers; ++Layer)
	{
		UGenQuadBatchComponent* C = LayerComponents[Layer];
		if (!C)
		{
			continue;
		}
		int32 NumVerts = 0;
		for (const FBatch& B : Batches)
		{
			if (B.Layer == Layer && B.Material)
			{
				NumVerts += B.Vertices.Num();
			}
		}
		FGenQuadFrame Frame;
		if (NumVerts > 0)
		{
			Frame.Positions.Reserve(NumVerts);
			Frame.UVs.Reserve(NumVerts);
			Frame.Colors.Reserve(NumVerts);
			for (const FBatch& B : Batches)
			{
				if (B.Layer != Layer || !B.Material || B.Vertices.Num() < 4)
				{
					continue;
				}
				FGenQuadDraw& Draw = Frame.Draws.AddDefaulted_GetRef();
				Draw.Material = B.Material->GetRenderProxy();
				Draw.FirstQuad = Frame.Positions.Num() / 4;
				Draw.NumQuads = B.Vertices.Num() / 4;
				Frame.Positions.Append(B.Vertices);
				Frame.UVs.Append(B.UVs);
				Frame.Colors.Append(B.Colors);
			}
		}
		GEN_PERF_COUNT("R.FX.Quads#", NumVerts / 4);
		GEN_PERF_COUNT("R.FX.Draws#", Frame.Draws.Num());
		C->SetFrame(Frame);
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
