#pragma once

#include "CoreMinimal.h"
#include "Client/GenFXData.h"
#include "Logic/GenLogicTypes.h"

class FGenParticleManager;

/** One particle (ParticleSys.cpp Particle). "Last*" hold the previous 30 Hz step for render interpolation. */
struct FGenParticle
{
	FVector3f Pos = FVector3f::ZeroVector;
	FVector3f LastPos = FVector3f::ZeroVector;
	FVector3f Vel = FVector3f::ZeroVector;
	FVector3f Accel = FVector3f::ZeroVector;
	FVector3f EmitterPos = FVector3f::ZeroVector;
	float VelDamping = 1.f;

	float AngleZ = 0.f, LastAngleZ = 0.f;
	float AngularRateZ = 0.f, AngularDamping = 1.f;

	uint32 LifetimeLeft = 0;
	uint32 Age = 0;                     // client frames since creation

	float Size = 1.f, LastSize = 1.f;
	float SizeRate = 0.f, SizeRateDamping = 1.f;

	float Alpha = 1.f, LastAlpha = 1.f, AlphaRate = 0.f;
	int32 AlphaTargetKey = 1;
	float AlphaKeyValue[FGenParticleSystemTemplate::MaxKeyframes] = {};

	FVector3f Color = FVector3f::OneVector, LastColor = FVector3f::OneVector, ColorRate = FVector3f::ZeroVector;
	int32 ColorTargetKey = 1;
	float ColorScale = 0.f;

	float WindRandomness = 1.f;
	bool bUpTowardsEmitter = false;
	uint32 ControlledSystem = 0;        // system riding on this particle (PerParticleAttachedSystem)
};

/** A running particle system (ParticleSys.cpp ParticleSystem). Coordinates are Generals world units. */
class GENERALS1_API FGenParticleSystem
{
public:
	uint32 GetID() const { return ID; }
	const FGenParticleSystemTemplate& GetTemplate() const { return *Template; }
	const TArray<FGenParticle>& GetParticles() const { return Particles; }

	void SetPosition(const FVector3f& P);
	FVector3f GetPosition() const { return LocalTransform.GetTranslation(); }
	void SetLocalTransform(const FTransform3f& T);
	void RotateLocal(const FVector3f& Axis, float Radians);
	void AttachToObject(FGenObjectID Object) { AttachedObject = Object; }
	FGenObjectID GetAttachedObject() const { return AttachedObject; }
	void SetInitialDelay(uint32 Frames) { DelayLeft = Frames; }
	/** ParticleSystem::setSystemLifetime: "lifetime of the system, not the particles". */
	void SetSystemLifetime(uint32 Frames) { bForever = Frames == 0; SystemLifetimeLeft = Frames; }
	/** UseCallersRadius: sphere / cylinder emission radius override. */
	void SetEmissionRadius(float Radius);
	void Stop() { bStopped = true; }
	void Start() { bStopped = false; }
	/** ParticleSystem::setVelocityMultiplier / setBurstCountMultiplier / setSizeMultiplier: a vehicle's dust and
	 *  dirt fly faster, thicker and bigger the faster it goes. */
	void SetVelocityMultiplier(const FVector3f& M) { VelocityMultiplier = M; }
	void SetBurstCountMultiplier(float M) { BurstCountMultiplier = M; }
	void SetSizeMultiplier(float M) { SizeMultiplier = M; }
	/** "Force a burst of particles to be emitted immediately." */
	void Trigger() { BurstDelayLeft = 0; DelayLeft = 0; }
	/** Stops emitting; the system is removed once its particles have died. */
	void Destroy() { bDestroyed = true; }

	/** Renderer cache: index of the draw batch for this system's texture and shader. */
	mutable int32 BatchIndex = INDEX_NONE;
	/** A sphere round every particle as it is and as it was a client frame ago (the screen blends between
	 *  the two), their sizes included: what the renderer asks the camera about. Worked out by each Step. */
	FVector3f BoundsCenter = FVector3f::ZeroVector;
	float BoundsRadius = 0.f;

private:
	friend class FGenParticleManager;

	bool Update(FGenParticleManager& Mgr);
	bool UpdateParticle(FGenParticle& P, FGenParticleManager& Mgr);
	void DoWindMotion(FGenParticle& P, FGenParticleManager& Mgr) const;
	void UpdateWindMotion(FGenParticleManager& Mgr);
	FVector3f ComputePosition(FRandomStream& R) const;
	FVector3f ComputeVelocity(const FVector3f& Pos, FRandomStream& R) const;
	FGenParticle GenerateParticle(int32 Num, int32 Count, FGenParticleManager& Mgr);
	void ComputeAlphaRate(FGenParticle& P) const;
	void ComputeColorRate(FGenParticle& P) const;
	bool IsInvisible(const FGenParticle& P) const;

	uint32 ID = 0;
	const FGenParticleSystemTemplate* Template = nullptr;
	TArray<FGenParticle> Particles;

	FVector3f Pos = FVector3f::ZeroVector, LastPos = FVector3f::ZeroVector;
	bool bFirstPos = true;
	FTransform3f LocalTransform = FTransform3f::Identity;
	bool bLocalIdentity = true;
	FTransform3f Transform = FTransform3f::Identity;
	bool bIdentity = true;

	FGenObjectID AttachedObject = GenInvalidObjectID;
	bool bControlled = false;            // rides on a particle of another system
	FVector3f ControlPos = FVector3f::ZeroVector;
	uint32 MasterID = 0, SlaveID = 0;

	bool bStopped = false;
	bool bDestroyed = false;
	FVector3f VelocityMultiplier = FVector3f::OneVector;
	float BurstCountMultiplier = 1.f;
	float SizeMultiplier = 1.f;
	bool bForever = true;
	uint32 SystemLifetimeLeft = 0;
	uint32 DelayLeft = 0;
	uint32 BurstDelayLeft = 0;
	float AccumulatedSizeBonus = 0.f;
	float SphereRadius = 0.f, CylRadius = 0.f;

	float WindAngle = 0.f, WindAngleChange = 0.15f;
	float WindStartAngle = 0.f, WindEndAngle = 0.f;
	bool bWindMovingToEnd = true;
};

/** ParticleSystemManager: owns the systems and steps them at 30 Hz. */
class GENERALS1_API FGenParticleManager
{
public:
	/** Modern replacement for GameData.ini MaxParticleCount (2500): above this, new particles are skipped. */
	static constexpr int32 MaxParticles = 12000;

	FGenParticleManager() = default;
	FGenParticleManager(const FGenParticleManager&) = delete;
	FGenParticleManager& operator=(const FGenParticleManager&) = delete;

	FGenParticleSystem* Create(const FGenParticleSystemTemplate* Template, bool bCreateSlaves = true);
	FGenParticleSystem* Create(const FString& TemplateName);
	FGenParticleSystem* Find(uint32 ID) const;
	void DestroyAttachedTo(FGenObjectID Object);
	void Reset();

	/** One 30 Hz client frame. */
	void Step();

	const TArray<TUniquePtr<FGenParticleSystem>>& GetSystems() const { return Systems; }
	int32 GetParticleCount() const { return ParticleCount; }
	FRandomStream& GetRandom() { return Random; }

	/** World queries supplied by the game (object transform in Generals space, ground height). */
	TFunction<bool(FGenObjectID, FTransform3f&)> ObjectTransform;
	TFunction<float(float, float)> GroundHeight;
	/** The object is hidden from the local player by the fog of war. */
	TFunction<bool(FGenObjectID)> ObjectShrouded;

private:
	friend class FGenParticleSystem;

	TArray<TUniquePtr<FGenParticleSystem>> Systems;
	TMap<uint32, FGenParticleSystem*> ByID;
	uint32 NextID = 1;
	int32 ParticleCount = 0;
	FRandomStream Random = FRandomStream(0x5eed);
};
