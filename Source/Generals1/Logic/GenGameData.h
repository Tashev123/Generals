#pragma once

#include "CoreMinimal.h"
#include "Core/GenIni.h"
#include "Logic/GenConditions.h"
#include "Logic/GenLogicTypes.h"

/** Locomotor.ini entry; rates already converted to per-frame units (LocomotorTemplate). */
struct GENERALS1_API FGenLocomotorTemplate
{
	FString Name;
	uint32 Surfaces = 0;
	float Speed = 0.f;               // units/frame
	float SpeedDamaged = -1.f;
	float TurnRate = 0.f;            // radians/frame
	float TurnRateDamaged = -1.f;
	float Acceleration = 0.f;        // units/frame^2
	float AccelerationDamaged = -1.f;
	float Braking = 99999.f;         // units/frame^2
	float MinSpeed = 0.f;
	float MinTurnSpeed = 99999.f;
	float TurnPivotOffset = 0.f;
	float Extra2DFriction = 0.f;     // per frame
	float CloseEnoughDist = 1.f;
	float PreferredHeight = 0.f;
	EGenLocoAppearance Appearance = EGenLocoAppearance::Other;
	bool bStickToGround = false;
	bool bCanMoveBackwards = false;
	FString ZAxisBehavior;
};

/** RadiusDamageAffects bits (WEAPON_AFFECTS_*). */
namespace GenWeaponAffects
{
	enum : uint32
	{
		Self = 1 << 0,
		Allies = 1 << 1,
		Enemies = 1 << 2,
		Neutrals = 1 << 3,
		KillsSelf = 1 << 4,       // SUICIDE
		NotSimilar = 1 << 5,
		NotAirborne = 1 << 6,
	};
}

/** Weapon.ini entry (WeaponTemplate); times in logic frames, speeds per frame. */
struct GENERALS1_API FGenWeaponTemplate
{
	FString Name;
	float PrimaryDamage = 0.f;
	float PrimaryDamageRadius = 0.f;
	float SecondaryDamage = 0.f;
	float SecondaryDamageRadius = 0.f;
	float AttackRange = 0.f;
	float MinimumAttackRange = 0.f;
	float AcceptableAimDelta = 0.f;     // radians
	float ScatterRadius = 0.f;
	float ScatterRadiusVsInfantry = 0.f;
	FString DamageType = TEXT("EXPLOSION");
	FString DeathType = TEXT("NORMAL");
	float WeaponSpeed = 999999.f;       // per frame
	float WeaponRecoil = 0.f;           // radians
	FString ProjectileObject;           // empty = instant/delayed damage
	FString FireFX;
	FString ProjectileDetonationFX;
	FString FireOCL;
	FString ProjectileDetonationOCL;
	FString ProjectileExhaust;          // particle system attached to the projectile
	FString FireSound;
	int32 ClipSize = 0;                 // 0 = unlimited
	int32 ClipReloadTime = 0;           // frames
	int32 DelayBetweenShotsMin = 0;     // frames
	int32 DelayBetweenShotsMax = 0;
	int32 PreAttackDelay = 0;           // frames
	uint32 RadiusDamageAffects = GenWeaponAffects::Allies | GenWeaponAffects::Enemies | GenWeaponAffects::Neutrals;
	bool bAntiGround = true;
	bool bAntiAirborneVehicle = false;
	bool bAntiAirborneInfantry = false;
	bool bDamageDealtAtSelfPosition = false;

	bool IsProjectile() const { return !ProjectileObject.IsEmpty(); }
};

/** Armor.ini entry: damage multipliers per damage type. */
struct GENERALS1_API FGenArmorTemplate
{
	FString Name;
	float DefaultCoefficient = 1.f;
	TMap<FString, float> Coefficients;  // upper-case damage type

	/** ArmorTemplate::adjustDamage */
	float AdjustDamage(const FString& DamageType, float Damage) const;
};

/** WeaponSet / ArmorSet entry on a ThingTemplate. */
struct GENERALS1_API FGenWeaponSetEntry
{
	TArray<FString> Conditions;         // empty = None
	FString Weapons[3];                 // PRIMARY, SECONDARY, TERTIARY
};

struct GENERALS1_API FGenArmorSetEntry
{
	TArray<FString> Conditions;
	FString Armor;
};

/** AIUpdateInterface Turret / AltTurret block (TurretAIData). */
struct GENERALS1_API FGenTurretData
{
	bool bValid = false;
	float TurnRate = 0.f;               // radians per frame
	float NaturalAngle = 0.f;           // radians
	uint32 ControlledSlots = 0;         // bit per weapon slot
	int32 RecenterTime = 0;             // frames
};

/** SlowDeathBehavior (subset). */
struct GENERALS1_API FGenSlowDeath
{
	enum { Initial = 0, Midpoint = 1, Final = 2, NumPhases = 3 };

	int32 DestructionDelay = 0;         // frames
	int32 DestructionDelayVariance = 0;
	int32 SinkDelay = 0;
	float SinkRate = 0.f;               // units per frame
	int32 ProbabilityModifier = 10;
	TArray<FString> DeathTypes;         // raw DeathTypes tokens
	TArray<FString> FX[NumPhases];      // "FX = INITIAL name": one is picked at random per phase
	TArray<FString> OCL[NumPhases];
};

/** One TransitionDamageFX entry: played when the body's damage state gets worse and reaches State. */
struct GENERALS1_API FGenTransitionFX
{
	enum EKind : uint8 { FXList, OCL, ParticleSystem };

	int32 State = 1;                    // 1 DAMAGED, 2 REALLYDAMAGED, 3 RUBBLE
	EKind Kind = FXList;
	FString Name;
	FVector3f Loc = FVector3f::ZeroVector; // FXList: offset in the object's frame
	FString Bone;                       // ParticleSystem: bone it sits on (empty = object origin)
	bool bRandomBone = false;           // pick one of Bone01..BoneNN
};

/** Die modules that all run when their DeathTypes apply (unlike slow deaths, where one is chosen). */
enum class EGenDieModule : uint8 { FXList, CreateObject, Destroy, Instant };

struct GENERALS1_API FGenDieModule
{
	EGenDieModule Type = EGenDieModule::Destroy;
	TArray<FString> DeathTypes;
	TArray<FString> FX;                 // FXListDie DeathFX / InstantDeathBehavior FX (random pick)
	TArray<FString> OCL;                // CreateObjectDie CreationList / InstantDeathBehavior OCL
};

/** DumbProjectileBehavior flight shape. */
struct GENERALS1_API FGenDumbProjectile
{
	bool bValid = false;
	float FirstHeight = 0.f;
	float SecondHeight = 0.f;
	float FirstPercentIndent = 0.3f;
	float SecondPercentIndent = 0.7f;
};

/** Faction definition (PlayerTemplate.ini). */
struct GENERALS1_API FGenPlayerTemplate
{
	FString Name;               // "FactionAmerica"
	FString Side;               // "America"
	FString BaseSide;           // "USA"
	bool bPlayable = false;
	bool bObserver = false;
	FString StartingBuilding;
	TArray<FString> StartingUnits;
	FColor PreferredColor = FColor::White;
	int32 StartMoney = 0;
};

/** One W3DModelDraw condition state (DefaultConditionState / ConditionState + aliases). */
struct GENERALS1_API FGenModelConditionState
{
	TArray<FGenConditionBits> ConditionSets; // one per ConditionState/AliasConditionState line; empty bits = default
	bool bIsDefault = false;
	FString Description;
	FString Model;                  // empty = no model (NONE)
	TArray<FString> Animations;     // "HIER.ANIM" (Animation / IdleAnimation); one is picked at random
	bool bIdleAnimations = false;   // IdleAnimation: play once, then pick another
	FString AnimationMode;          // LOOP, ONCE, ONCE_BACKWARDS, MANUAL, LOOP_BACKWARDS, PING_PONG
	float AnimSpeedMin = 1.f;       // AnimationSpeedFactorRange
	float AnimSpeedMax = 1.f;
	TArray<TPair<FString, bool>> HideShow; // (sub-object, bHide) in order
	FString Turret;
	FString TurretPitch;
	TArray<FString> MuzzleFlashBones; // WeaponMuzzleFlash bone prefixes (hidden unless firing)
	TArray<int32> MuzzleFlashSlots;   // weapon slot (0..2) for each entry above
	TArray<FString> RecoilBones;      // WeaponRecoilBone bone prefixes (barrels that kick back when firing)
	TArray<int32> RecoilSlots;
	TArray<TPair<FString, FString>> ParticleBones; // ParticleSysBone: (bone, particle system) alive while the state shows
	FString FireFXBone[3];            // WeaponFireFXBone per weapon slot: where FireFX plays
	FString LaunchBone[3];            // WeaponLaunchBone per weapon slot: where projectiles start
};

/** Draw module summary (W3DModelDraw and derived, W3DTreeDraw, W3DPropDraw, ...). */
struct GENERALS1_API FGenDrawModule
{
	FString Type;                   // "W3DTankDraw"
	FString Tag;                    // "ModuleTag_01"
	TSharedPtr<FGenIniBlock> Block;
	TArray<FGenModelConditionState> States;
	int32 DefaultStateIndex = INDEX_NONE;
	FGenConditionBits IgnoreConditions;
	bool bOkToChangeModelColor = false;

	const FGenModelConditionState* DefaultState() const;
	FString DefaultModel() const;

	/** SparseMatchFinder::findBestInfoSlow: most matching bits, then fewest missing bits. */
	int32 FindBestState(const FGenConditionBits& Flags) const;
};

/** Merged object definition (Object / ObjectReskin), equivalent to ThingTemplate. */
struct GENERALS1_API FGenThingTemplate
{
	FString Name;
	TSharedPtr<FGenIniBlock> Block;  // merged entries (base + overrides)
	TSet<FString> KindOf;            // upper case
	FString Side;
	FString EditorSorting;
	TArray<FGenDrawModule> Draws;
	FString GeometryType;
	float GeometryMajorRadius = 0.f;
	float GeometryMinorRadius = 0.f;
	float GeometryHeight = 0.f;
	bool bGeometryIsSmall = false;
	float BuildCost = 0.f;
	float BuildTime = 0.f;
	float VisionRange = 0.f;
	float ShroudClearingRange = 0.f;
	float PlacementViewAngle = 0.f;  // radians

	// Body module
	float MaxHealth = 0.f;
	float InitialHealth = 0.f;
	FString BodyType;

	// PhysicsBehavior (per-frame friction)
	bool bHasPhysics = false;
	float Mass = 1.f;
	float ForwardFriction = 0.15f;
	float LateralFriction = 0.15f;
	float AerodynamicFriction = 0.f;
	float PitchRollYawFactor = 2.f;
	bool bAllowBouncing = false;
	bool bKillWhenRestingOnGround = false;

	// LifetimeUpdate: the object dies after a random time in this range (frames)
	bool bHasLifetime = false;
	int32 MinLifetime = 0;
	int32 MaxLifetime = 0;

	// AI
	bool bHasAI = false;
	FString AIUpdateType;
	bool bAutoAcquireEnemiesWhenIdle = false;
	FGenTurretData Turrets[2];          // Turret, AltTurret
	/** "SET_NORMAL" -> locomotor template names. */
	TMap<FString, TArray<FString>> LocomotorSets;

	// Combat
	TArray<FGenWeaponSetEntry> WeaponSets;
	TArray<FGenArmorSetEntry> ArmorSets;
	TArray<FGenSlowDeath> SlowDeaths;
	TArray<FGenDieModule> DieModules;
	TArray<FGenTransitionFX> TransitionFX;

	// Crushing: a mover with CrusherLevel above the other's CrushableLevel drives over it; anything with
	// SquishCollide (infantry) is squished by any crusher.
	int32 CrusherLevel = 0;
	int32 CrushableLevel = 255;
	bool bSquishable = false;
	FGenDumbProjectile DumbProjectile;
	bool bIsMissile = false;            // MissileAIUpdate

	/** Weapon set for the given condition names (WeaponSet with the best match; None = default). */
	const FGenWeaponSetEntry* FindWeaponSet() const;
	const FGenArmorSetEntry* FindArmorSet() const;

	bool IsKindOf(const TCHAR* Kind) const { return KindOf.Contains(Kind); }
	const FGenDrawModule* PrimaryDraw() const;

	/** GeometryInfo::getBoundingCircleRadius. */
	float GetBoundingCircleRadius() const;
	/** GeometryInfo::getBoundingSphereRadius. */
	float GetBoundingSphereRadius() const;
	bool IsBox() const { return GeometryType.Equals(TEXT("BOX"), ESearchCase::IgnoreCase); }
};

/** Terrain.ini entry. */
struct GENERALS1_API FGenTerrainType
{
	FString Name;
	FString Texture;    // file in Art\Terrain
	FString Class;
};

/**
 * Registry of INI-defined game data, loaded from the original archives
 * (subset of GameEngine::init; grows as systems are implemented).
 */
class GENERALS1_API FGenGameData
{
public:
	static FGenGameData& Get();

	bool Initialize();
	bool IsInitialized() const { return bInitialized; }

	const FGenThingTemplate* FindTemplate(const FString& Name) const;
	const FGenTerrainType* FindTerrain(const FString& Name) const;
	const FGenLocomotorTemplate* FindLocomotor(const FString& Name) const;
	const FGenPlayerTemplate* FindPlayerTemplate(const FString& Name) const;
	const TArray<FGenPlayerTemplate>& GetPlayerTemplates() const { return PlayerTemplates; }
	const TMap<FString, TSharedPtr<FGenThingTemplate>>& GetTemplates() const { return Templates; }

	const FGenWeaponTemplate* FindWeapon(const FString& Name) const;
	const FGenArmorTemplate* FindArmor(const FString& Name) const;

	/** GameData.ini Gravity, per frame^2 (negative). */
	float Gravity = -1.f;
	float UnitDamagedThreshold = 0.5f;
	float UnitReallyDamagedThreshold = 0.1f;

	/** Applies a map's map.ini overrides (objects defined in the map folder). */
	void ApplyMapIni(const FString& MapFolder);

private:
	void AddObjectBlock(const TSharedPtr<FGenIniBlock>& Block, bool bReskin);
	TSharedPtr<FGenThingTemplate> BuildTemplate(const FString& Name, const TSharedPtr<FGenIniBlock>& Merged) const;
	static TSharedPtr<FGenIniBlock> MergeBlocks(const FGenIniBlock* Base, const FGenIniBlock& Override);

	bool bInitialized = false;
	TMap<FString, TSharedPtr<FGenIniBlock>> ObjectBlocks;     // key: lowercase name, merged definition
	TMap<FString, TSharedPtr<FGenThingTemplate>> Templates;   // key: lowercase name
	TMap<FString, FGenTerrainType> TerrainTypes;              // key: lowercase name
	TMap<FString, FGenLocomotorTemplate> Locomotors;          // key: lowercase name
	TArray<FGenPlayerTemplate> PlayerTemplates;
	TMap<FString, FGenWeaponTemplate> Weapons;                // key: lowercase name
	TMap<FString, FGenArmorTemplate> Armors;                  // key: lowercase name
};
