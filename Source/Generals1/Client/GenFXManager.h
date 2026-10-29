#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Client/GenFXData.h"
#include "Client/GenParticles.h"
#include "GenFXManager.generated.h"

class AGenMapActor;
class FGenObject;
class UGenGameLogic;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class UProceduralMeshComponent;
class UGenQuadBatchComponent;

/**
 * Client-side effects (FXList.cpp, ParticleSys.cpp, W3DParticleSys.cpp): particle systems, tracers,
 * light pulses, camera shake and terrain scorch marks.
 *
 * Effects run on the simulation clock in the original's 30 Hz client frames, so they pause and speed
 * up with the game, and are interpolated between steps so they stay smooth at any frame rate.
 * Particles are drawn as camera-facing quads batched per texture and shader.
 */
UCLASS()
class GENERALS1_API UGenFXManager : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UGenFXManager* Get(const UObject* WorldContext);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	void Start(AGenMapActor* InMapActor, UGenGameLogic* InLogic);

	/** Advances the effects to the simulation time (in logic frames, fractional) and redraws them. */
	void Update(double LogicFrameTime);

	/** FXList::doFXPos. PrimarySpeed is in distance per logic frame (weapon speed, for tracers). */
	void DoFXPos(const FGenFXList* FX, const FVector3f& Primary, const FTransform3f* PrimaryXf, float PrimarySpeed,
		const FVector3f* Secondary, float OverrideRadius = 0.f);
	/** FXList::doFXObj. */
	void DoFXObj(const FGenFXList* FX, const FGenObject* Primary, const FGenObject* Secondary = nullptr);

	/** Test hook: plays an FX list, or failing that a single particle system, of that name at a position. */
	bool PlayByName(const FString& Name, const FVector3f& Pos);

	/** Particle system that follows an object (projectile exhaust, debris trails). */
	FGenParticleSystem* CreateAttachedSystem(const FString& TemplateName, FGenObjectID Object);
	/** A particle system by itself at a spot (the dust where a gunship's bullets land). */
	void CreateSystemAt(const FString& TemplateName, const FVector3f& Pos);
	void OnObjectDestroyed(FGenObjectID Object);
	/** Stops a system from emitting; it is removed once its particles have died. */
	void DestroySystem(uint32 SystemID);
	/** ParticleSystem::stop / start: no new particles while stopped (the ones in the air live out their time). */
	void SetSystemStopped(uint32 SystemID, bool bStopped);
	/** A running system by its id (null once it is gone). */
	FGenParticleSystem* FindSystem(uint32 SystemID);

	void AddScorch(const FVector3f& Pos, float Radius, int32 Type);

	/**
	 * A light an object keeps throwing (W3DPoliceCarDraw's flashing light bar): set every frame it shows, with its
	 * colour as it is now; one not set in a frame goes out. Radius: W3D's far attenuation end (Generals units).
	 */
	void SetObjectLight(FGenObjectID Object, const FVector3f& Pos, const FLinearColor& Color, float Radius);

	int32 GetParticleCount() const { return Particles.GetParticleCount(); }
	int32 GetSystemCount() const { return Particles.GetSystems().Num(); }

private:
	/** The quads of one texture and shader this frame (four vertices each), and what they are drawn with. */
	struct FBatch
	{
		TArray<FVector3f> Vertices;
		TArray<FVector2f> UVs;
		TArray<FColor> Colors;
		UMaterialInstanceDynamic* Material = nullptr; // (kept alive by BatchMaterials)
		int32 Layer = 0;
	};

	/** What draws the batches: one primitive per kind of blending, in the order they go on the screen. */
	enum { LayerMasked, LayerMultiply, LayerAlpha, LayerAdditive, NumLayers };

	struct FTracer
	{
		FVector3f Pos = FVector3f::ZeroVector, LastPos = FVector3f::ZeroVector;
		FVector3f Dir = FVector3f::ForwardVector;
		float Speed = 0.f, Length = 10.f, Width = 1.f;
		float Opacity = 1.f, LastOpacity = 1.f;
		FLinearColor Color = FLinearColor::White;
		int32 FramesLeft = 1;
	};

	struct FLightPulse
	{
		FVector3f Pos = FVector3f::ZeroVector;
		FLinearColor Color = FLinearColor::White;
		float Radius = 0.f;
		int32 IncreaseFrames = 0, DecreaseFrames = 0;
		double StartTime = 0.0;             // client frame time
	};

	void StepClientFrame();
	void DoParticleNugget(const FGenFXNugget& N, const FVector3f& Primary, const FTransform3f* Xf, const FGenObject* AttachTo, float OverrideRadius);
	void DoNuggetPos(const FGenFXNugget& N, const FVector3f& Primary, const FTransform3f* PrimaryXf, float PrimarySpeed,
		const FVector3f* Secondary, float OverrideRadius);
	void AddLightPulse(const FVector3f& Pos, const FLinearColor& Color, float Radius, int32 IncreaseFrames, int32 DecreaseFrames);
	void Shake(const FVector3f& Epicenter, int32 Type);

	/** Draw batch per texture and shader; bFlat = quads lying on the ground, bSprite = stand-alone quads (not ribbons). */
	int32 GetBatch(const FString& Texture, EGenParticleShader Shader, bool bFlat, bool bSprite);
	void RenderParticles();
	void FlushBatches();
	void UpdateLights();
	void UpdateSway();
	void RebuildScorchMesh();
	UMaterialInterface* GetParticleMaterial(EGenParticleShader Shader);

	UPROPERTY(Transient)
	TObjectPtr<AGenMapActor> MapActor;

	UPROPERTY(Transient)
	TObjectPtr<UGenGameLogic> Logic;

	FGenParticleManager Particles;
	double ClientTime = 0.0;                // 30 Hz client frames, fractional
	int64 ClientFrame = 0;
	float FrameAlpha = 0.f;

	TArray<FBatch> Batches;
	TMap<FString, int32> BatchLookup;

	UPROPERTY(Transient)
	TObjectPtr<UGenQuadBatchComponent> LayerComponents[4]; // (one per layer: NumLayers)

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> BatchMaterials;

	UPROPERTY(Transient)
	TMap<uint8, TObjectPtr<UMaterialInterface>> BaseMaterials;

	TArray<FTracer> Tracers;
	TArray<TPair<FVector3f, int32>> PendingShakes;  // asked for from the simulation's thread

	TArray<FLightPulse> Pulses;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UPointLightComponent>> PulseLights;

	// SetObjectLight: the lights wanted this frame, and the ones lit for them (by object).
	struct FObjectLight { FVector3f Pos; FLinearColor Color; float Radius; };
	TMap<FGenObjectID, FObjectLight> WantedObjectLights;
	UPROPERTY(Transient)
	TMap<uint32, TObjectPtr<UPointLightComponent>> ObjectLights;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UPointLightComponent>> FreeObjectLights;

	// The breeze (ScriptEngine's BreezeInfo, default as the script engine starts; no official map's scripts change
	// it) and W3DTreeBuffer's ten sway motions made from it, handed to the tree materials through MPC_GenSway.
	enum { NumSwayEntries = 100, MaxSwayTypes = 10 };
	FVector3f SwayOffsets[NumSwayEntries];
	float SwayStep[MaxSwayTypes] = {};
	float SwayFactor[MaxSwayTypes] = {};
	bool bSwayReady = false;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialParameterCollection> SwayCollection;

	// Camera shake (W3DView::shake): a stiff spring that flips direction every client frame.
	float ShakeIntensity = 0.f;
	FVector2f ShakeDir = FVector2f(1.f, 0.f);
	FVector2f ShakeOffset = FVector2f::ZeroVector, LastShakeOffset = FVector2f::ZeroVector;

	struct FScorch { FVector3f Pos; float Radius; int32 Type; };
	TArray<FScorch> Scorches;
	bool bScorchDirty = false;
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> ScorchMesh;
};
