#pragma once

#include "CoreMinimal.h"

struct FGenIniBlock;

/** GameClientRandomVariable: a uniform value in [Min, Max]. */
struct FGenRandomVar
{
	float Min = 0.f;
	float Max = 0.f;

	FGenRandomVar() = default;
	FGenRandomVar(float InMin, float InMax) : Min(InMin), Max(InMax) {}
	float Get(FRandomStream& R) const { return Min == Max ? Min : R.FRandRange(FMath::Min(Min, Max), FMath::Max(Min, Max)); }
};

enum class EGenParticleShader : uint8 { None, Additive, Alpha, AlphaTest, Multiply };
enum class EGenParticleType : uint8 { None, Particle, Drawable, Streak, VolumeParticle, Smudge };
enum class EGenEmissionVelocity : uint8 { None, Ortho, Spherical, Hemispherical, Cylindrical, Outward };
enum class EGenEmissionVolume : uint8 { None, Point, Line, Box, Sphere, Cylinder };
enum class EGenWindMotion : uint8 { NotUsed, PingPong, Circular };

/**
 * ParticleSystem.ini entry (ParticleSystemInfo). Every rate is per 30 Hz client frame, exactly as in
 * the file: the particle manager steps at 30 Hz and interpolates for rendering.
 */
struct GENERALS1_API FGenParticleSystemTemplate
{
	static constexpr int32 MaxKeyframes = 8;

	struct FAlphaKey { FGenRandomVar Var; uint32 Frame = 0; };
	struct FColorKey { FVector3f Color = FVector3f::ZeroVector; uint32 Frame = 0; };

	FString Name;
	int32 Priority = 0;
	EGenParticleShader Shader = EGenParticleShader::None;
	EGenParticleType Type = EGenParticleType::None;
	FString ParticleName;               // texture file (PARTICLE / STREAK)

	FGenRandomVar AngleZ, AngularRateZ, AngularDamping, VelDamping;
	FGenRandomVar Lifetime;
	uint32 SystemLifetime = 0;          // 0 = forever
	FGenRandomVar StartSize, StartSizeRate, SizeRate, SizeRateDamping;
	FAlphaKey AlphaKeys[MaxKeyframes];
	FColorKey ColorKeys[MaxKeyframes];
	FGenRandomVar ColorScale;           // already divided by 255
	FGenRandomVar BurstDelay, BurstCount, InitialDelay;
	FVector3f DriftVelocity = FVector3f::ZeroVector;
	float Gravity = 0.f;
	FString SlaveSystem;
	FVector3f SlavePosOffset = FVector3f::ZeroVector;
	FString AttachedSystem;             // PerParticleAttachedSystem

	EGenEmissionVelocity VelocityType = EGenEmissionVelocity::None;
	FGenRandomVar VelOrtho[3];
	FGenRandomVar VelSpherical, VelHemispherical;
	FGenRandomVar VelCylRadial, VelCylNormal;
	FGenRandomVar VelOutward, VelOutwardOther;

	EGenEmissionVolume VolumeType = EGenEmissionVolume::None;
	FVector3f LineStart = FVector3f::ZeroVector, LineEnd = FVector3f::ZeroVector;
	FVector3f BoxHalfSize = FVector3f::ZeroVector;
	float SphereRadius = 0.f;
	float CylRadius = 0.f, CylLength = 0.f;

	bool bHollow = false;
	bool bGroundAligned = false;
	bool bEmitAboveGroundOnly = false;
	bool bParticleUpTowardsEmitter = false;

	EGenWindMotion WindMotion = EGenWindMotion::NotUsed;
	float WindAngleChangeMin = 0.15f, WindAngleChangeMax = 0.45f;
	float WindStartAngleMin = 0.f, WindStartAngleMax = PI / 4.f;
	float WindEndAngleMin = 2.f * PI - PI / 4.f, WindEndAngleMax = 2.f * PI;
};

enum class EGenFXNugget : uint8 { Sound, ParticleSystem, Tracer, LightPulse, ViewShake, TerrainScorch, RayEffect, FXListAtBonePos };

/** One FXList nugget (FXList.cpp). Fields are shared between nugget types where the INI keys overlap. */
struct GENERALS1_API FGenFXNugget
{
	EGenFXNugget Type = EGenFXNugget::Sound;
	FString Name;                        // particle system / sound / nested FX list
	// ParticleSystem
	int32 Count = 1;
	FVector3f Offset = FVector3f::ZeroVector;
	FGenRandomVar Radius, Height;
	FGenRandomVar InitialDelayMs = FGenRandomVar(-1.f, -1.f); // < 0: keep the system's own delay
	float RotateX = 0.f, RotateY = 0.f, RotateZ = 0.f;        // radians
	bool bOrientToObject = false;
	bool bRicochet = false;
	bool bAttachToObject = false;
	bool bCreateAtGroundHeight = false;
	bool bUseCallersRadius = false;
	// Tracer
	float Speed = 0.f;                   // per frame; 0 = the weapon's speed
	float DecayAt = 1.f;
	float Length = 10.f;
	float Width = 1.f;
	float Probability = 1.f;
	// Tracer / LightPulse
	FLinearColor Color = FLinearColor::White;
	// LightPulse / TerrainScorch
	float EffectRadius = 0.f;
	float RadiusPctOfObject = 0.f;
	int32 IncreaseFrames = 0;
	int32 DecreaseFrames = 0;
	// ViewShake (0 subtle .. 5 cine insane) / TerrainScorch (-1 random)
	int32 Variant = 0;
	// FXListAtBonePos
	FString BoneName;
	bool bOrientToBone = true;
};

struct GENERALS1_API FGenFXList
{
	FString Name;
	TArray<FGenFXNugget> Nuggets;
};

/** GenericObjectCreationNugget DebrisDisposition bits. */
namespace GenDisposition
{
	enum : uint32
	{
		LikeExisting = 1 << 0,
		OnGroundAligned = 1 << 1,
		SendItFlying = 1 << 2,
		SendItUp = 1 << 3,
		SendItOut = 1 << 4,
		RandomForce = 1 << 5,
		Floating = 1 << 6,
		InheritVelocity = 1 << 7,
		Whirling = 1 << 8,
	};
}

/** CreateObject / CreateDebris nugget of an ObjectCreationList (other nugget types are not ported yet). */
struct GENERALS1_API FGenOCLNugget
{
	bool bDebris = false;                // CreateDebris: Names are W3D models shown on a GenericDebris object
	TArray<FString> Names;
	int32 Count = 1;
	float Mass = 0.f;
	FVector3f Offset = FVector3f::ZeroVector;
	uint32 Disposition = GenDisposition::OnGroundAligned;
	float DispositionIntensity = 0.f;
	float SpinRate = -1.f, YawRate = -1.f, RollRate = -1.f, PitchRate = -1.f; // radians per frame; < 0 = unset
	float MinMag = 0.f, MaxMag = 0.f;
	float MinPitch = 0.f, MaxPitch = 0.f; // radians
	int32 MinFrames = 0, MaxFrames = 0;   // lifetime override
	float ExtraBounciness = 0.f;
	float ExtraFriction = 0.f;            // per frame
	float MinHealth = 1.f, MaxHealth = 1.f;
	FString ParticleSystem;               // attached to each created object
	FString FXFinal;
	bool bOkToChangeModelColor = false;
	bool bOrientInForceDirection = false;
	bool bSkipIfSignificantlyAirborne = false;
};

struct GENERALS1_API FGenOCL
{
	FString Name;
	TArray<FGenOCLNugget> Nuggets;
};

/** Registry of effect definitions: ParticleSystem.ini, FXList.ini, ObjectCreationList.ini. */
class GENERALS1_API FGenFXData
{
public:
	static FGenFXData& Get();

	bool Initialize();

	const FGenParticleSystemTemplate* FindParticleSystem(const FString& Name) const;
	const FGenFXList* FindFXList(const FString& Name) const;
	const FGenOCL* FindOCL(const FString& Name) const;

	/** GameData.ini camera shake settings (indexed by FGenFXNugget::Variant). */
	float ShakeIntensity[6] = { 0.5f, 1.f, 2.5f, 5.f, 8.f, 12.f };
	float MaxShakeRange = 150.f;
	float MaxShakeIntensity = 10.f;
	/** GameData.ini GroundStiffness: how much vertical speed a bounce returns. */
	float GroundStiffness = 0.5f;

private:
	bool bInitialized = false;
	TMap<FString, FGenParticleSystemTemplate> ParticleSystems; // key: lowercase name
	TMap<FString, FGenFXList> FXLists;
	TMap<FString, FGenOCL> OCLs;
};
