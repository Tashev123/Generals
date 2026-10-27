#pragma once

#include "CoreMinimal.h"
#include "Core/GenIni.h"
#include "Logic/GenConditions.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenUpgrades.h"
#include "Logic/GenRadiusDecal.h"

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
	int32 MovePriority = 1;          // GroupMovementPriority: 0 MOVES_BACK, 1 MOVES_MIDDLE, 2 MOVES_FRONT
	bool bStickToGround = false;
	bool bCanMoveBackwards = false;
	FString ZAxisBehavior;

	// Flight
	float Lift = 0.f;                // units/frame^2
	float LiftDamaged = -1.f;
	float SpeedLimitZ = 999999.f;    // units/frame
	float PreferredHeightDamping = 1.f;
	float MaxThrustAngle = 0.f;      // radians: how far off its nose a THRUST locomotor can push
	bool bApply2DFrictionWhenAirborne = false;
	bool bAllowAirborneMotiveForce = false;
	// How the body leans with its speed (Drawable::calcPhysicsXformHoverOrWings), per 30 Hz frame
	float PitchStiffness = 0.1f;
	float RollStiffness = 0.1f;
	float PitchDamping = 0.9f;
	float RollDamping = 0.9f;
	float ForwardVelocityPitchFactor = 0.f;
	float LateralVelocityRollFactor = 0.f;
	float UniformAxialDamping = 1.f;
	float PitchInDirectionOfZVelFactor = 0.f;   // nose up while climbing, down while descending
	// How a ground vehicle's body sits and rocks (Drawable::calcPhysicsXformTreads / Wheels / Motorcycle), per 30 Hz frame
	float AccelPitchLimit = 0.f;     // AccelerationPitchLimit (radians)
	float DecelPitchLimit = 0.f;
	float BounceKick = 0.f;          // BounceAmount (radians per frame)
	float ForwardAccelCoef = 0.f;    // ForwardAccelerationPitchFactor
	float LateralAccelCoef = 0.f;    // LateralAccelerationRollFactor
	bool bHasSuspension = false;     // the wheels' springs and steering are worked out
	float WheelTurnAngle = 0.f;      // FrontWheelTurnAngle (radians)
	float MaxWheelExtension = 0.f;   // MaximumWheelExtension (negative: down)
	/** Higher above the ground than this, only anti-air weapons can target the unit (default: never). */
	int32 AirborneTargetingHeight = MAX_int32;
	/** Radius of the circle a winged aircraft flies while it has nowhere to go (negative: the other way round; 0: as tight as it can). */
	float CirclingRadius = 0.f;

	/** Moves only through the air: no ground pathfinding, no collisions with ground units. */
	bool IsAirOnly() const { return (Surfaces & GenSurface::Air) != 0 && (Surfaces & ~uint32(GenSurface::Air)) == 0; }
	/** Z_SURFACE_RELATIVE_HEIGHT / Z_ABSOLUTE_HEIGHT: holds PreferredHeight with lift. */
	bool UsesLift() const { return PreferredHeight != 0.f && (ZAxisBehavior.Equals(TEXT("SURFACE_RELATIVE_HEIGHT"), ESearchCase::IgnoreCase)
		|| ZAxisBehavior.Equals(TEXT("ABSOLUTE_HEIGHT"), ESearchCase::IgnoreCase) || IsLayerRelative()); }
	/** Z_SMOOTH_RELATIVE_TO_HIGHEST_LAYER (the drones): the preferred height is over the ground or the bridge it is on. */
	bool IsLayerRelative() const { return ZAxisBehavior.Equals(TEXT("RELATIVE_TO_HIGHEST_LAYER"), ESearchCase::IgnoreCase); }
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

/** WeaponReloadType (AutoReloadsClip). */
enum class EGenReloadType : uint8
{
	Auto,           // an empty clip reloads by itself
	None,           // never reloads
	ReturnToBase,   // only reloads while parked at an airfield
};

/** "WeaponBonus = PLAYER_UPGRADE DAMAGE 125%": what a weapon gains while its owner has a bonus condition. */
struct GENERALS1_API FGenWeaponBonus
{
	enum EField : uint8 { Damage, Radius, Range, RateOfFire, PreAttack };
	FString Condition;                  // upper case: PLAYER_UPGRADE, FRENZY_ONE, ...
	EField Field = Damage;
	float Value = 1.f;
};

/** Contain modules (OpenContain and what is built on it) that carry or shelter units. */
enum class EGenContainType : uint8
{
	None,
	Transport,      // TransportContain (HelixContain too, as far as carrying goes): vehicles and aircraft with passengers
	Garrison,       // GarrisonContain: buildings that infantry fight from
	Heal,           // HealContain: a barracks; hurt infantry go in, are healed, and come out again
	Tunnel,         // TunnelContain: all the tunnels of a player share one space
};

struct GENERALS1_API FGenContainData
{
	EGenContainType Type = EGenContainType::None;
	int32 ContainMax = 0;               // Slots / ContainMax (tunnels: GameData's MaxTunnelCapacity)
	TArray<FString> AllowInsideKindOf;  // any of these (empty: anything)
	TArray<FString> ForbidInsideKindOf;
	bool bAllowAllies = true;
	bool bAllowEnemies = true;
	bool bAllowNeutral = true;
	bool bPassengersAllowedToFire = false;
	bool bShouldDrawPips = true;        // ShouldDrawPips = No: how full it is isn't shown (a gunship's only "passenger" is its gun)
	float DamagePercentToUnits = 0.f;   // share of their health the occupants lose when the container dies (1 = all die)
	bool bBurnedDeathToUnits = true;
	int32 NumberOfExitPaths = 1;        // ExitStartNN / ExitEndNN bones
	int32 ExitDelay = 0;                // frames between one rider leaving and the next
	bool bGoAggressiveOnExit = false;
	float HealthRegenPercentPerSec = 0.f;
	FString InitialPayload;             // riders it comes with
	int32 InitialPayloadCount = 0;
	bool bHealObjects = false;
	int32 FramesForFullHeal = 0;
	bool bEnclosing = true;             // IsEnclosingContainer: occupants are out of sight
	bool bArmedRidersUpgradeWeaponSet = false;
	bool bDelayExitInAir = false;
	bool bKeepVelocityOnExit = false;   // KeepContainerVelocityOnExit: what leaves it keeps its speed (a cargo plane's drops)
	bool bInternetHack = false;         // InternetHackContain: whoever comes in starts hacking the internet
	bool bImmuneToClearBuildingAttacks = false; // what is inside can't be killed by a flash-bang, a flame, a microwave
	bool bOverlord = false;             // OverlordContain: its one "passenger" is an add-on riding in the open; whoever boards after that goes into the add-on (a bunker)
	bool bHelix = false;                // HelixContain: one add-on in the open besides its passengers
	bool bPassengersInTurret = false;   // PassengersInTurret: the bone the add-on stands on is in the turret
	bool bExperienceSinkForRider = false; // ExperienceSinkForRider: what the add-on kills counts for its carrier
	FString EnterSound;                 // OpenContain's EnterSound / ExitSound: the container's own, once a frame (a garrison's door)
	FString ExitSound;
	TArray<FString> PayloadTemplateNames; // PayloadTemplateName: comes with these aboard (an Avenger's laser turret)
};

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
	TArray<FVector2f> ScatterTargets;   // ScatterTarget table: every shot of a clip goes to one of these (times the scalar), each once
	float ScatterTargetScalar = 0.f;
	int32 ShotsPerBarrel = 1;           // shots from one launch bone before the next one's turn
	float ScatterRadiusVsInfantry = 0.f;
	FString DamageType = TEXT("EXPLOSION");
	FString DeathType = TEXT("NORMAL");
	float WeaponSpeed = 999999.f;       // per frame
	float WeaponRecoil = 0.f;           // radians
	FString ProjectileObject;           // empty = instant/delayed damage
	bool bAllowAttackGarrisonedBldgs = false; // may be aimed at a garrisoned building whatever its damage (flash-bangs, flames, toxin)
	bool bAntiMine = false;                    // AntiMine: may be aimed at mines and demo traps (a dozer's disarming tool)
	float ContinueAttackRange = 0.f;           // with its victim gone, on to the nearest like it within this of where that was
	FString FireFX;
	FString ProjectileDetonationFX;
	FString FireOCL;
	FString ProjectileDetonationOCL;
	FString ProjectileExhaust;          // particle system attached to the projectile
	FString LaserName;                  // the shot is a beam: an object with W3DLaserDraw that lives a moment
	FString LaserBoneName;              // ... from this bone of the shooter's model
	FString FireSound;
	int32 FireSoundLoopTime = 0;        // frames: the fire sound loops for as long as shots come no further apart than this
	int32 ClipSize = 0;                 // 0 = unlimited
	int32 ClipReloadTime = 0;           // frames
	// FiringTracker: a weapon that speeds up as it keeps firing (its CONTINUOUS_FIRE_MEAN / _FAST bonuses).
	int32 ContinuousFireOne = MAX_int32;  // more shots than this in a row: "continuous fire"
	int32 ContinuousFireTwo = MAX_int32;  // ... than this: "continuous fire two"
	int32 ContinuousFireCoast = 0;        // frames it may go without firing before it has to start over
	EGenReloadType ReloadType = EGenReloadType::Auto;
	int32 DelayBetweenShotsMin = 0;     // frames
	int32 DelayBetweenShotsMax = 0;
	int32 PreAttackDelay = 0;           // frames
	/** WeaponPrefireType: when the delay before a shot applies. */
	enum class EPrefire : uint8 { PerShot, PerAttack, PerClip };
	EPrefire PrefireType = EPrefire::PerShot;
	float MinTargetPitch = -PI;         // "we may not target anything outside of this pitch range"
	float MaxTargetPitch = PI;
	uint32 RadiusDamageAffects = GenWeaponAffects::Allies | GenWeaponAffects::Enemies | GenWeaponAffects::Neutrals;
	bool bAntiGround = true;
	bool bAntiAirborneVehicle = false;
	bool bAntiAirborneInfantry = false;
	bool bDamageDealtAtSelfPosition = false;
	TArray<FGenWeaponBonus> Bonuses;

	bool IsProjectile() const { return !ProjectileObject.IsEmpty(); }
	bool IsLaser() const { return !LaserName.IsEmpty(); }
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

/** DamageFX.ini entry: what shows on a body when it is hit, by the kind of damage and the rank of whoever did it. */
struct GENERALS1_API FGenDamageFX
{
	struct FEntry
	{
		float AmountForMajor = 0.f;     // damage from which the major effect plays instead of the minor one
		FString MajorFX;
		FString MinorFX;
		int32 ThrottleFrames = 0;       // "when doing damagefx of the same type, wait at least this" long
	};
	struct FLevels
	{
		FEntry L[4];                    // regular .. heroic
	};
	FString Name;
	FLevels Default;
	TMap<FString, FLevels> ByType;      // damage types the file has lines of their own for
	const FEntry& Find(const FString& DamageType, int32 Level) const
	{
		const FLevels* F = ByType.Find(DamageType);
		return (F ? *F : Default).L[FMath::Clamp(Level, 0, 3)];
	}
};

/** FlammableUpdate: what flame damage does to an object beyond the damage itself. */
struct GENERALS1_API FGenFlammable
{
	bool bValid = false;
	int32 BurnedDelay = 0;              // frames aflame until it counts as burned (0: never)
	int32 AflameDuration = 0;           // frames it burns
	int32 AflameDamageDelay = 0;        // frames between the fire's own bites (0: none)
	float AflameDamageAmount = 0.f;
	FString BurningSound;
	float FlameDamageLimit = 20.f;      // flame damage it takes before it catches fire
	int32 FlameDamageExpiration = 0;    // frames without flame damage after which that count starts afresh
};

/** FireSpreadUpdate: a burning object sets light to what stands near it. */
struct GENERALS1_API FGenFireSpread
{
	bool bValid = false;
	FString OCLEmbers;
	int32 MinDelay = 0;                 // frames between tries
	int32 MaxDelay = 0;
	float Range = 0.f;                  // the nearest thing that can burn within this is lit (0: embers only)
};

/** ToppleUpdate (lamp posts, signs, the old trees) and the toppling of W3DTreeDraw's trees: pushed over by what drives into them. */
struct GENERALS1_API FGenTopple
{
	bool bValid = false;
	bool bTree = false;                 // a W3DTreeDraw tree: no stump, no death - it sinks into the ground and is gone
	FString ToppleFX;
	FString BounceFX;
	FString StumpName;
	bool bKillWhenStartToppling = false;
	bool bKillWhenToppled = true;
	bool bKillStumpWhenToppled = false;
	bool bLeftOrRightOnly = false;      // a fence: falls to one of its two sides
	bool bReorientToppledRubble = false;
	float InitialVelocityPercent = 0.2f;
	float InitialAccelPercent = 0.01f;
	float BounceVelocityPercent = 0.3f;
	float MinimumToppleSpeed = 0.f;     // trees: "Won't let trees fall slower than this"
	int32 SinkFrames = 0;               // trees: how long the fallen tree takes to sink away
	float SinkDistance = 0.f;
};

/** StructureCollapseUpdate: a dead building shudders, then sinks into the ground in bursts of debris. */
struct GENERALS1_API FGenStructureCollapse
{
	enum { Initial, Delay, Burst, Final, NumPhases };
	bool bValid = false;
	TArray<FString> DeathTypes;
	int32 MinCollapseDelay = 0;         // frames between dying and starting to go down
	int32 MaxCollapseDelay = 0;
	int32 MinBurstDelay = 9999;         // frames between bursts on the way down
	int32 MaxBurstDelay = 9999;
	float CollapseDamping = 0.f;        // share of gravity it is spared
	float MaxShudder = 0.f;
	int32 BigBurstFrequency = 0;        // one burst in so many is a big one
	TArray<FString> FX[NumPhases];
	TArray<FString> OCL[NumPhases];
};

/** StructureToppleUpdate: a dead tower falls over, away from what killed it, and crushes what it lands on. */
struct GENERALS1_API FGenStructureTopple
{
	enum { Initial, Delay, Final, NumPhases };
	bool bValid = false;
	TArray<FString> DeathTypes;
	int32 MinToppleDelay = 0;
	int32 MaxToppleDelay = 0;
	int32 MinBurstDelay = 0;
	int32 MaxBurstDelay = 0;
	float StructuralIntegrity = 0.1f;   // how much of the fall's acceleration it holds back at first ...
	float StructuralDecay = 0.f;        // ... and what is left of that each frame
	FString ToppleStartFX, ToppleDelayFX, ToppleDoneFX, CrushingFX;
	FString CrushingWeapon;
	TArray<TPair<float, FString>> AngleFX; // (radians, effect): played as it passes that angle
	TArray<FString> OCL[NumPhases];
};

/** ActiveBody::updateBodyParticleSystems: fire or smoke on the bones of a model that are named for it. */
struct GENERALS1_API FGenAutoParticles
{
	FString Prefix;                     // bones "<Prefix>01", "<Prefix>02", ...
	FString System;
	int32 Max = 0;                      // on at most this many of them
};

/** WeaponSet / ArmorSet entry on a ThingTemplate. */
struct GENERALS1_API FGenWeaponSetEntry
{
	TArray<FString> Conditions;         // empty = None
	FString Weapons[3];                 // PRIMARY, SECONDARY, TERTIARY
	bool bNoAutoChoose[3] = { false, false, false }; // "AutoChooseSources = SECONDARY NONE": only used when asked for by name
	TArray<FString> PreferredAgainst[3]; // "PreferredAgainst = PRIMARY INFANTRY": always chosen against any of these
};

struct GENERALS1_API FGenArmorSetEntry
{
	TArray<FString> Conditions;
	FString Armor;
	FString DamageFX;
};

/** AIUpdateInterface Turret / AltTurret block (TurretAIData). */
struct GENERALS1_API FGenTurretData
{
	bool bValid = false;
	float TurnRate = 0.f;               // radians per frame
	float NaturalAngle = 0.f;           // radians
	uint32 ControlledSlots = 0;         // bit per weapon slot
	int32 RecenterTime = 0;             // frames
	bool bAllowsPitch = false;          // the barrel goes up and down as well
	float PitchRate = 0.01f;            // radians per frame
	float NaturalPitch = 0.f;
	float FirePitch = 0.f;              // above 0: it fires at this pitch, wherever the target is (artillery)
	float MinPitch = 0.f;               // MinPhysicalPitch
	float GroundUnitPitch = 0.f;        // above 0: against things on the ground it aims this much higher at full range
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
	TArray<FString> ExemptStatus;       // ... and the raw DieMuxData ExemptStatus tokens
	TArray<FString> FX[NumPhases];      // "FX = INITIAL name": one is picked at random per phase
	TArray<FString> OCL[NumPhases];
	TArray<FString> Weapons[NumPhases]; // "Weapon = FINAL name": fired where the object is
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

/** JetSlowDeathBehavior: a plane shot down in flight rolls, falls, hits the ground and blows up. */
struct GENERALS1_API FGenJetDeath
{
	bool bValid = false;
	TArray<FString> DeathTypes;
	FString FXOnGroundDeath, OCLOnGroundDeath;     // killed while on the ground: just this, then gone
	FString FXInitialDeath, OCLInitialDeath;
	FString FXSecondary, OCLSecondary;
	FString FXHitGround, OCLHitGround;
	FString FXFinalBlowUp, OCLFinalBlowUp;
	float RollRate = 0.f;               // radians per 30 Hz frame
	float RollRateDelta = 1.f;          // the roll rate is multiplied by this every frame
	float PitchRate = 0.f;
	float FallHowFast = 0.f;            // share of gravity the wreck falls with
	int32 DelaySecondaryFromInitialDeath = 0;   // frames
	int32 DelayFinalBlowUpFromHitGround = 0;
};

/** HelicopterSlowDeathBehavior: a helicopter shot down spirals to the ground, spinning, loses its rotor and blows up. */
struct GENERALS1_API FGenHeliDeath
{
	bool bValid = false;
	TArray<FString> DeathTypes;
	float SpiralOrbitTurnRate = 0.f;            // radians per frame
	float SpiralOrbitForwardSpeed = 0.f;        // push along the spiral, per frame
	float SpiralOrbitForwardSpeedDamping = 1.f;
	float MinSelfSpin = 0.f;                    // radians per frame
	float MaxSelfSpin = 0.f;
	int32 SelfSpinUpdateDelay = 0;              // frames
	float SelfSpinUpdateAmount = 0.f;           // radians (per second)
	float FallHowFast = 0.5f;                   // share of gravity it falls with
	int32 MinBladeFlyOffDelay = 0;              // frames
	int32 MaxBladeFlyOffDelay = 0;
	FString AttachParticle;                     // smoke trailing behind it
	FString BladeBone;
	FString FXBlade, OCLBlade;
	FString OCLEjectPilot;              // the pilot of a veteran helicopter gets out as the rotor comes off
	FString SoundDeathLoop;             // heard from the moment it is hit until it meets the ground
	FString FXHitGround, OCLHitGround;
	FString FXFinalBlowUp, OCLFinalBlowUp;
	int32 DelayFromGroundToFinalDeath = 0;      // frames
	FString FinalRubbleObject;                  // the hull left lying there
};

/** Die modules that all run when their DeathTypes apply (unlike slow deaths, where one is chosen). */
enum class EGenDieModule : uint8 { FXList, CreateObject, Destroy, Instant, EjectPilot, CreateCrate };

struct GENERALS1_API FGenDieModule
{
	EGenDieModule Type = EGenDieModule::Destroy;
	TArray<FString> DeathTypes;
	uint8 VeterancyMask = 0xF;          // DieMuxData VeterancyLevels: bit per level it applies at (regular .. heroic)
	TArray<FString> ExemptStatus;       // ... and ExemptStatus: the raw tokens, e.g. "ALL -UNDER_CONSTRUCTION"
	FString GroundOCL;                  // EjectPilotDie: what comes out of a wreck on the ground ...
	FString AirOCL;                     // ... and out of one in the air
	TArray<FString> FX;                 // FXListDie DeathFX / InstantDeathBehavior FX (random pick)
	TArray<FString> OCL;                // CreateObjectDie CreationList / InstantDeathBehavior OCL
	bool bTransferPreviousHealth = false; // CreateObjectDie: what it makes has the health this had, and those who attacked this attack that
	TArray<FString> CrateData;          // CreateCrateDie: the crate data to try, each in turn
};

/** Crate.ini's CrateData (CrateTemplate): when something that dies leaves a crate, and which. */
struct GENERALS1_API FGenCrateData
{
	FString Name;
	float CreationChance = 0.f;
	int32 VeterancyLevel = INDEX_NONE;          // only when what died had this rank
	TArray<FString> KilledByKindOf;             // only when its killer is all of these (upper case)
	FString KillerScience;                      // ... and the killer's player has this science
	TArray<TPair<FString, float>> CrateObjects; // (template, its share of the chance)
	bool bOwnedByMaker = false;                 // the crate is the dead thing's player's, not nobody's
};

/** CountermeasuresBehavior: flares that draw missiles off an aircraft. */
struct GENERALS1_API FGenCountermeasures
{
	bool bValid = false;
	FString FlareTemplate;
	int32 VolleySize = 0;               // flares per volley
	float VolleyArcAngle = 0.f;         // radians: they fan out this far to either side
	float VolleyVelocityFactor = 1.f;
	int32 FramesBetweenVolleys = 0;
	int32 NumberOfVolleys = 0;          // ... before it has to reload
	int32 ReloadFrames = 0;             // 0: at the airfield only
	float EvasionRate = 0.f;            // share of the missiles fired at it that go for a flare
	int32 ReactionFrames = 0;           // from being shot at to the first volley
	int32 MissileDecoyFrames = 0;       // from launch to the moment a fooled missile turns away
};

/** DumbProjectileBehavior flight shape. */
struct GENERALS1_API FGenDumbProjectile
{
	bool bValid = false;
	float FirstHeight = 0.f;
	float SecondHeight = 0.f;
	float FirstPercentIndent = 0.3f;
	float SecondPercentIndent = 0.7f;
	int32 GarrisonHitKillCount = 0;           // hitting a garrisoned building, it kills this many inside instead of going off
	TArray<FString> GarrisonHitKillRequired;  // ... of these kinds (all of them, upper case)
	TArray<FString> GarrisonHitKillForbidden;
	FString GarrisonHitKillFX;                // (at the building)
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
	TArray<FString> IntrinsicSciences;   // sciences the faction starts with
	FString PurchaseScienceSets[3];      // command sets of the promotion screen: rank 1, rank 3, rank 8 (= 5 in Zero Hour)
	FString SpecialPowerShortcutSet;
	FString SpecialPowerShortcutWinName; // the tray's layout (GenPowersShortcutBarUS.wnd)
	int32 SpecialPowerShortcutButtonCount = 0;
	FString SideIcon;                    // mapped images and music for the menus
	FString ScoreScreenImage;
	FString ScoreScreenMusic;
};

/** W3DModelDraw's "Flags =" of a condition state (ACBITS_*). */
namespace GenStateFlags
{
	enum : uint32
	{
		RandomStart = 1 << 0,                        // RANDOMSTART: the animation begins on any frame
		StartFrameFirst = 1 << 1,
		StartFrameLast = 1 << 2,                     // (a MANUAL animation held on its last frame: a door standing open)
		AdjustHeightByConstructionPercent = 1 << 3,  // the model stands as deep in the ground as the building is unfinished
		PristineBonePosInFinalFrame = 1 << 4,
		MaintainFrameAcrossStates = 1 << 5,          // the animation goes on from where the last state's had got to ...
		MaintainFrameAcrossStates2 = 1 << 6,         // ... if that state has the same one of these four
		MaintainFrameAcrossStates3 = 1 << 7,
		MaintainFrameAcrossStates4 = 1 << 8,
		RestartAnimWhenComplete = 1 << 9,
		AnyMaintainFrame = MaintainFrameAcrossStates | MaintainFrameAcrossStates2 | MaintainFrameAcrossStates3 | MaintainFrameAcrossStates4,
	};
	GENERALS1_API uint32 Parse(const TArray<FString>& Names);
}

/** One W3DModelDraw condition state (DefaultConditionState / ConditionState + aliases), or a TransitionState. */
struct GENERALS1_API FGenModelConditionState
{
	TArray<FGenConditionBits> ConditionSets; // one per ConditionState/AliasConditionState line; empty bits = default
	bool bIsDefault = false;
	FString Description;
	FString Model;                  // empty = no model (NONE)
	TArray<FString> Animations;     // "HIER.ANIM" (Animation / IdleAnimation); one is picked at random
	bool bIdleAnimations = false;   // IdleAnimation: play once, then pick another
	FString AnimationMode;          // LOOP, ONCE, ONCE_BACKWARDS, MANUAL, LOOP_BACKWARDS, PING_PONG
	uint32 Flags = 0;               // GenStateFlags
	/**
	 * TransitionKey: going from a state with one key to a state with another, the module first shows
	 * its "TransitionState = <from> <to>", if it has one, and the new state when that has played out
	 * (scaffolding going up and coming down, a soldier getting up).
	 */
	FString TransitionKey;          // lower case
	FString AllowToFinishKey;       // WaitForStateToFinishIfPossible: coming from a state with this key, let its animation finish first
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

/** NeutronMissileUpdate: how the nuclear missile flies. */
struct FGenNeutronMissile
{
	bool bValid = false;
	float InitialDist = 0.f;             // DistanceToTravelBeforeTurning
	float MaxTurnRate = 999.f;           // radians per frame
	float ForwardDamping = 0.f;
	float RelativeSpeed = 1.f;           // its push, per frame per frame
	float TargetFromDirectlyAbove = 0.f; // it makes for a spot this far over the target first
	FString LaunchFX, IgnitionFX;
	float SpecialAccelFactor = 1.f;      // the lift-off: for SpecialSpeedTime its height is set outright ...
	int32 SpecialSpeedTime = 0;          // ... frames
	float SpecialSpeedHeight = 0.f;      // ... rising this far in that time
};

/** NeutronMissileSlowDeathBehavior: the blast waves where the nuclear missile dies. */
struct FGenNeutronBlast
{
	bool bValid = false;
	float ScorchSize = 0.f;
	FString FXList;
	struct FBlast
	{
		bool bEnabled = false;
		float Delay = 0.f;        // frames
		float ScorchDelay = 0.f;  // frames
		float InnerRadius = 0.f;  // full damage inside this
		float OuterRadius = 0.f;  // some of it out to here
		float MaxDamage = 0.f;
		float MinDamage = 0.f;
		float ToppleSpeed = 0.f;
		float PushForce = 0.f;
	};
	FBlast Blasts[9];
};

/** MinefieldBehavior: a mine (it stands for NumVirtualMines of them; its health says how many are left). */
struct FGenMinefield
{
	bool bValid = false;
	FString DetonationWeapon;
	uint8 DetonatedByMask = 1 | 4;           // 1 enemies, 2 allies, 4 neutral
	bool bStopsRegenAfterCreatorDies = true;
	bool bRegenerates = false;
	bool bWorkersDetonate = false;
	int32 CreatorDeathCheckFrames = 30;
	int32 ScootFrames = 0;                   // ScootFromStartingPointTime
	float RepeatDetonateMoveThresh = 1.f;
	int32 NumVirtualMines = 1;
	float DrainPerSecond = 0.f;              // DegenPercentPerSecondAfterCreatorDies, as a share of its health
	FString CreationList;
};

/** GenerateMinefieldBehavior: the mines an object lays around itself - with an upgrade, or where it dies. */
struct FGenGenerateMinefield
{
	bool bValid = false;
	FString MineName, UpgradedMineName, UpgradedTriggeredBy;
	FString GenerationFX;
	float DistanceAroundObject = 40.f;
	float MinesPerSquareFoot = 0.004f;
	bool bOnDeath = false;
	bool bBorderOnly = true;
	bool bSmartBorder = false;
	bool bSmartBorderSkipInterior = true;
	bool bAlwaysCircular = false;
	bool bUpgradable = false;
	float RandomJitter = 0.f;
	float SkipIfThisMuchUnderStructure = 0.33f;
};

/** ParticleUplinkCannonUpdate: the particle cannon. */
/** SpectreGunshipUpdate: the gunship's run - in from beyond the map's edge, round its target for a while, out again. */
struct FGenSpectreGunship
{
	FGenRadiusDecalTemplate AttackAreaDecal;       // the area it attacks, on the ground (its owner sees it)
	FGenRadiusDecalTemplate TargetingReticleDecal; // where its guns are
	bool bValid = false;
	FString Power;
	FString GattlingTemplate;          // the gun that rides on it
	FString HowitzerWeapon;
	FString GattlingStrafeFX;          // the particle system where the gattling's bullets land
	float RandomOffsetForHowitzer = 20.f;
	float TargetingReticleRadius = 25.f;
	float OrbitInsertionSlope = 0.7f;
	float OrbitRadius = 250.f;
	int32 HowitzerFiringRate = 10;     // frames between its looks for a target (and the howitzer's shells)
	int32 HowitzerFollowLag = 0;       // frames the gattling must have been on the spot before the howitzer joins in
	float StrafingIncrement = 20.f;    // how far the gattling's aim moves in one of the original's frames
	float AttackAreaRadius = 200.f;
	int32 OrbitFrames = 0;
};

/** SpectreGunshipDeploymentUpdate: the building's power sends for a gunship. */
struct FGenGunshipDeployment
{
	FString Power;
	FString GunshipTemplate;
	FString RequiredScience;
	FString CreateLocation;
};

struct FGenParticleCannon
{
	bool bValid = false;
	FString Power;
	int32 BeginChargeFrames = 0, RaiseAntennaFrames = 0, ReadyDelayFrames = 0; // before it is ready, counted back from then
	int32 TotalFiringFrames = 0;     // how long the beam is on the ground
	int32 WidthGrowFrames = 0;       // how long the beam takes to widen (and to narrow again)
	int32 BeamTravelFrames = 0;      // from leaving the dish to reaching the ground
	float DamagePerSecond = 0.f;
	int32 TotalDamagePulses = 1;
	FString DamageType = TEXT("PARTICLE_BEAM");
	FString DeathType = TEXT("NORMAL");
	float DamageRadiusScalar = 1.f;  // times the beam's half width
	float RevealRange = 0.f;
	FString OuterEffectBone;
	int32 OuterEffectNumBones = 0;
	FString ConnectorBone, FireBone;
	FString OuterNodesLight, OuterNodesMedium, OuterNodesIntense;      // particle systems
	FString ConnectorMediumLaser, ConnectorIntenseLaser;               // laser objects
	FString ConnectorMediumFlare, ConnectorIntenseFlare;
	FString LaserBaseLight, LaserBaseMedium, LaserBaseIntense;
	FString ParticleBeamLaser;                                          // laser object: the beam itself
	FString GroundHitFX, BeamLaunchFX;
	int32 FramesBetweenLaunchFX = 30;
	int32 TotalScorchMarks = 1;
	float ScorchMarkScalar = 1.f;
	float SwathDistance = 0.f, SwathAmplitude = 0.f;
	float ManualDrivingSpeed = 0.f, ManualFastDrivingSpeed = 0.f;       // per second
	int32 DoubleClickFrames = 0;
	FString RemnantObject;
};

/** MissileLauncherBuildingUpdate: a silo's doors, opened and closed by its special power's timer. */
struct FGenMissileLauncherBuilding
{
	bool bValid = false;
	FString Power;
	int32 DoorOpenTime = 0, DoorWaitOpenTime = 0, DoorCloseTime = 0; // frames
	FString OpeningFX, OpenFX, WaitingToCloseFX, ClosingFX, ClosedFX;
};

/** Draw module summary (W3DModelDraw and derived, W3DTreeDraw, W3DPropDraw, ...). */
/** W3DTankDraw / W3DTruckDraw / W3DTankTruckDraw: what turns, rolls and throws up dirt as a vehicle drives. */
struct GENERALS1_API FGenVehicleDraw
{
	enum EKind : uint8 { None, Tank, Truck, TankTruck };
	EKind Kind = None;
	// Treads (W3DTankDraw, W3DTankTruckDraw)
	FString TreadDebrisLeft = TEXT("TrackDebrisDirtLeft");
	FString TreadDebrisRight = TEXT("TrackDebrisDirtRight");
	float TreadAnimationRate = 0.f;          // texture widths per 30 Hz frame (0: the treads don't scroll)
	float TreadPivotSpeedFraction = 0.6f;    // slower than this share of top speed while turning: the treads turn against each other
	float TreadDriveSpeedFraction = 0.3f;    // faster than this while driving: they roll
	// Wheels (W3DTruckDraw, W3DTankTruckDraw)
	FString Dust, DirtSpray, PowerslideSpray;
	enum ETire : uint8 { LeftFront, RightFront, LeftRear, RightRear, MidLeftFront, MidRightFront, MidLeftRear, MidRightRear,
		MidLeftMid, MidRightMid, NumTires };
	FString Tires[NumTires];
	float TireRotationMultiplier = 0.f;      // radians per unit driven
	float PowerslideRotationAddition = 0.f;  // the rear wheels spin this much faster in a powerslide
	FString CabBone, TrailerBone;            // W3DTruckDraw: a tractor's cab and its trailer swing with the steering
	float CabRotationMultiplier = 0.f;
	float TrailerRotationMultiplier = 0.f;
	float RotationDamping = 0.f;
	bool HasTreads() const { return Kind == Tank || Kind == TankTruck; }
	bool HasWheels() const { return Kind == Truck || Kind == TankTruck; }
};

struct GENERALS1_API FGenDrawModule
{
	FString Type;                   // "W3DTankDraw"
	FString Tag;                    // "ModuleTag_01"
	TSharedPtr<FGenIniBlock> Block;
	TArray<FGenModelConditionState> States;
	int32 DefaultStateIndex = INDEX_NONE;
	FGenConditionBits IgnoreConditions;
	bool bOkToChangeModelColor = false;
	uint8 ProjectileBoneFeedbackSlots = 0; // ProjectileBoneFeedbackEnabledSlots (bit per slot): the launch bones' sub-objects go as their shots are fired
	FString AttachToBoneInContainer;       // W3DDependencyModelDraw: riding in the open, it is drawn on this bone of what carries it
	FString RequiredScience;               // W3DScienceModelDraw: only players with this science see it (a salvage crate)
	bool bAnimationsRequirePower = true;   // AnimationsRequirePower: its animations stand still while the building is short of power
	FGenVehicleDraw Vehicle;               // W3DTankDraw, W3DTruckDraw, W3DTankTruckDraw
	TMap<FString, FGenModelConditionState> Transitions;   // TransitionState blocks, by "<from> <to>" (lower case)
	const FGenModelConditionState* FindTransition(const FString& FromKey, const FString& ToKey) const;

	const FGenModelConditionState* DefaultState() const;
	FString DefaultModel() const;

	/** SparseMatchFinder::findBestInfoSlow: most matching bits, then fewest missing bits. */
	int32 FindBestState(const FGenConditionBits& Flags) const;
};

/** Merged object definition (Object / ObjectReskin), equivalent to ThingTemplate. */
/** StealthForbiddenConditions (TheStealthLevelNames): what a stealthed object must not be doing. */
namespace GenStealth
{
	enum : uint32
	{
		Attacking = 1 << 0,
		Moving = 1 << 1,
		UsingAbility = 1 << 2,
		FiringPrimary = 1 << 3,
		FiringSecondary = 1 << 4,
		FiringTertiary = 1 << 5,
		NoBlackMarket = 1 << 6,     // STEALTH_ONLY_WITH_BLACK_MARKET
		TakingDamage = 1 << 7,
		RidersAttacking = 1 << 8,
		FiringWeapon = FiringPrimary | FiringSecondary | FiringTertiary,
	};
}

/** StealthUpdate: when the object may be stealthed, and how its own side sees it then. */
struct GENERALS1_API FGenStealthData
{
	bool bValid = false;
	int32 StealthDelay = MAX_int32 / 2;     // frames it has to behave before it is stealthed (again)
	uint32 Forbidden = 0;                   // GenStealth bits
	float MoveThresholdSpeed = 0.f;         // per frame
	bool bInnateStealth = true;             // otherwise an upgrade or a power has to give it
	float FriendlyOpacityMin = 0.5f;        // how faint its owner sees it at the low point of the pulse
	bool bDisguisesAsTeam = false;          // disguises itself instead (the bomb truck)
	int32 DisguiseFrames = 0;               // DisguiseTransitionTime: fading out, into the other look, back in
	int32 DisguiseRevealFrames = 0;         // DisguiseRevealTransitionTime
	FString DisguiseFX, DisguiseRevealFX;
	bool bOrderIdleEnemiesToAttackMeUponReveal = false;
	int32 BlackMarketCheckFrames = 0;
	bool bGrantedBySpecialPower = false;    // the module every object has, waiting for the GPS scrambler
};

/** StealthDetectorUpdate: every so often, what is stealthed nearby is shown up. */
struct GENERALS1_API FGenStealthDetector
{
	bool bValid = false;
	int32 Rate = 1;                         // frames between looks
	float Range = 0.f;                      // 0: as far as it sees
	bool bInitiallyDisabled = false;
	TArray<FString> RequiredKindOf;         // only these (a dozer only finds mines)
	TArray<FString> ForbiddenKindOf;
	bool bWhileGarrisoned = false;
	bool bWhileContained = false;
	// The "IR" show of a look, for whoever can see the detector: a ping (loud when something stealthed was in
	// range) and particle systems at a bone.
	FString PingSound;
	FString LoudPingSound;
	FString IRParticleSys;                  // IRParticleSysName (IRBrightParticleSysName when something was found)
	FString IRBrightParticleSys;
	FString IRBeaconParticleSys;            // IRBeaconParticleSysName
	FString IRParticleSysBone;
};

/** PointDefenseLaserUpdate: a laser that shoots down the missiles coming for it (a Paladin's, the Air Force
 *  General's jets'). */
struct GENERALS1_API FGenPointDefenseLaser
{
	FString Weapon;                         // WeaponTemplate (its LaserName is the beam)
	TArray<FString> PrimaryKinds;           // PrimaryTargetTypes: these first ...
	TArray<FString> SecondaryKinds;         // ... then these
	int32 ScanFrames = 0;                   // ScanRate
	float ScanRange = 0.f;
	float VelocityFactor = 0.f;             // PredictTargetVelocityFactor
};

/** BattlePlanUpdate: the strategy center's three plans - bombardment (1), hold the line (2), search and destroy (3). */
struct GENERALS1_API FGenBattlePlanData
{
	bool bValid = false;
	FString Power;                          // SpecialPowerTemplate: the buttons that pick a plan fire this
	int32 PlanFrames[3] = { 0, 0, 0 };      // ...PlanAnimationTime: unpacking (and packing) takes this long
	int32 TransitionIdleFrames = 0;         // packed up, it waits this long before unpacking the next plan
	FString MessageLabel[3];                // MESSAGE:BattlePlan...Initiated
	FString UnpackSound[3];
	FString PackSound[3];
	FString Announcement[3];
	FString SearchAndDestroyIdleSound;      // heard for as long as search and destroy is in force
	TArray<FString> ValidKindOf;            // the plans work on any of these ...
	TArray<FString> InvalidKindOf;          // ... that is none of these
	int32 ParalyzeFrames = 0;               // BattlePlanChangeParalyzeTime: the army stands still while plans change
	float HoldTheLineArmorScalar = 1.f;     // damage the army takes under hold the line
	float SearchAndDestroySightScalar = 1.f; // how much further it sees under search and destroy
	float CenterSightScalar = 1.f;          // StrategyCenterSearchAndDestroySightRangeScalar
	bool bCenterDetectsStealth = false;     // ... and its stealth detector comes on
	float CenterMaxHealthScalar = 1.f;      // StrategyCenterHoldTheLineMaxHealthScalar
	FString CenterMaxHealthChangeType;      // ... applied like this (PRESERVE_RATIO)
};

/** GrantStealthBehavior: the GPS scrambler's marker, a growing circle that stealths what it passes over. */
struct GENERALS1_API FGenGrantStealth
{
	bool bValid = false;
	float StartRadius = 0.f;
	float FinalRadius = 0.f;
	float GrowRate = 0.f;                   // per 30 Hz frame
	TArray<FString> KindOf;                 // any of these
	FString RadiusParticleSystem;
};

/** RadarPriorityType: whether and how an object shows on the radar. */
enum class EGenRadarPriority : uint8
{
	Invalid,        // not set: not on the radar
	NotOnRadar,
	Structure,
	Unit,
	LocalUnitOnly,  // only on its owner's radar
};

/** DynamicShroudClearingRangeUpdate: the area an object reveals grows, holds and shrinks again (frames). */
struct GENERALS1_API FGenDynamicShroud
{
	bool bValid = false;
	int32 ChangeInterval = 0;
	int32 GrowInterval = 0;
	int32 ShrinkDelay = 0;
	int32 ShrinkTime = 0;
	int32 GrowDelay = 0;
	int32 GrowTime = 0;
	float FinalVision = 0.f;
};

struct GENERALS1_API FGenThingTemplate
{
	FString Name;
	TSharedPtr<FGenIniBlock> Block;  // merged entries (base + overrides)
	TSet<FString> KindOf;            // upper case
	bool bKindInfantry = false;      // KindOf INFANTRY (asked about every neighbour every frame by the steering)
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
	/** Buildable: 0 Yes, 1 No (cut content: never offered), 2 Only_By_AI, 3 Ignore_Prerequisites. */
	uint8 Buildable = 0;
	float ShroudClearingRange = -1.f;   // how far it clears the fog of war (-1: as far as it sees)
	float ShroudRevealToAllRange = -1.f; // how far around it everybody else sees (superweapons)
	EGenRadarPriority RadarPriority = EGenRadarPriority::Invalid;
	bool bHasRadarUpdate = false;       // RadarUpdate: the dish that unfolds when the radar upgrade arrives
	int32 RadarExtendFrames = 0;
	FGenDynamicShroud DynamicShroud;    // DynamicShroudClearingRangeUpdate
	FGenStealthData Stealth;            // StealthUpdate
	FGenStealthDetector Detector;       // StealthDetectorUpdate
	FGenGrantStealth GrantStealth;      // GrantStealthBehavior
	TArray<FGenPointDefenseLaser> PointDefenseLasers; // PointDefenseLaserUpdate (GenPointDefense.cpp)
	FGenBattlePlanData BattlePlan;      // BattlePlanUpdate (the strategy center)
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
	bool bAutoAcquireWhenStealthed = false;     // "AutoAcquireEnemiesWhenIdle = Yes Stealthed": opens fire even though it gives it away
	bool bAutoAcquireBuildings = false;         // "... ATTACK_BUILDINGS": goes for plain buildings by itself too
	FGenTurretData Turrets[2];          // Turret, AltTurret
	/** "SET_NORMAL" -> locomotor template names. */
	TMap<FString, TArray<FString>> LocomotorSets;

	// Combat
	TArray<FGenWeaponSetEntry> WeaponSets;
	TArray<FGenArmorSetEntry> ArmorSets;
	TArray<FGenSlowDeath> SlowDeaths;
	TArray<FGenDieModule> DieModules;
	TArray<FGenTransitionFX> TransitionFX;

	// Production and construction
	FString CommandSet;                 // CommandSet.ini entry shown when this object is selected
	FString DisplayName;                // OBJECT:... label
	int32 EnergyProduction = 0;         // > 0 produces power, < 0 consumes it
	int32 EnergyBonus = 0;              // added once the power plant has its upgrade (control rods), or while overcharged
	bool bOvercharge = false;           // OverchargeBehavior: a nuclear reactor can be pushed for EnergyBonus more power ...
	float OverchargeDrainPerSecond = 0.f; // ... losing this share of its health a second ...
	float OverchargeMinHealth = 0.f;    // ... until below this share (and not allowed to start then)
	TArray<TArray<FString>> Prerequisites; // each entry: the player must own one of these objects
	TArray<FString> PrerequisiteSciences;  // ... and all of these sciences (a general's promotion unlocks the unit)
	int32 ExperienceValue = 0;          // what destroying it is worth (regular level)
	int32 SkillPointValue = -1;         // general's skill points for destroying it (-1: the experience value)
	// Veterancy (ExperienceTracker): by level REGULAR, VETERAN, ELITE, HEROIC
	int32 ExperienceValues[4] = { 0, 0, 0, 0 };     // experience its killer gets, by the victim's level
	int32 ExperienceRequired[4] = { 0, 0, 0, 0 };   // experience needed to be at each level
	int32 SkillPointValues[4] = { -1, -1, -1, -1 }; // general's skill points by the victim's level (-1: the experience value)
	bool bIsTrainable = false;                      // gains experience and levels
	int32 VeterancyStartLevel = 0;                  // VeterancyGainCreate: the level it is made at ...
	FString VeterancyStartScience;                  // ... when its player has this science (empty: always)

	// Upgrades and special powers
	TArray<FGenUpgradeModule> UpgradeModules;   // every module with TriggeredBy
	TArray<FString> GrantUpgrades;              // GrantUpgradeCreate: upgrades it has from the start
	TArray<FGenSpecialPowerModule> PowerModules;
	TArray<FGenAbilityData> Abilities;          // SpecialAbilityUpdate modules
	FGenHackInternet HackInternet;              // HackInternetAIUpdate
	FGenStickyBomb StickyBomb;                  // StickyBombUpdate
	FGenLaserDraw LaserDraw;                    // W3DLaserDraw: the object is nothing but a beam
	FGenAutoDeposit AutoDeposit;                // AutoDepositUpdate
	FGenOCLUpdate OCLUpdate;                    // OCLUpdate
	FGenCrateCollide Crate;                     // MoneyCrateCollide, SalvageCrateCollide, UnitCrateCollide
	bool bTechBuilding = false;                 // TechBuildingBehavior: shows whether it is owned; nobody's again when destroyed
	bool bKeepObjectDie = false;                // KeepObjectDie: its rubble stays
	bool bButtonHunt = false;                   // CommandButtonHuntUpdate: scripts can send it to use an ability on whatever is nearest
	int32 HuntScanFrames = 30;
	float HuntScanRange = 9999.f;
	FGenAutoHeal AutoHeal;
	FString LaserMuzzleParticleSystem;  // LaserUpdate: a flare where the beam leaves the weapon ...
	FString LaserTargetParticleSystem;  // ... and one where it strikes
	FGenFlammable Flammable;
	FGenFireSpread FireSpread;
	FGenTopple Topple;
	FGenStructureCollapse Collapse;
	FGenStructureTopple StructureTopple;
	bool bHasOwnDieModule = false;      // it brings a module that answers to its death (so the default DestroyDie is not its)
	int32 UpgradedSupplyBoost = 0;              // extra money per load once the player has Supply Lines
	// Things that fall out of aircraft (and the markers powers leave on the ground)
	bool bHeightDie = false;                    // HeightDieUpdate: dies when this close to the ground
	float HeightDieTarget = 0.f;
	bool bHeightDieOnlyWhenMovingDown = false;
	int32 HeightDieInitialDelay = 0;            // frames
	bool bHeightDieSnapToGround = false;        // SnapToGroundOnDeath
	/** FireWeaponWhenDeadBehavior: a weapon "fired" where the object dies - if it starts active or its upgrade is in, and none it conflicts with. */
	struct FDeathWeapon
	{
		FString Weapon;
		bool bStartsActive = false;
		bool bRequiresAllTriggers = false;
		TArray<FString> TriggeredBy;
		TArray<FString> ConflictsWith;
	};
	TArray<FDeathWeapon> DeathWeapons;
	bool bHasDeletion = false;                  // DeletionUpdate: silently removed after a while
	int32 DeletionMin = 0;
	int32 DeletionMax = 0;
	bool bDeliverPayloadAI = false;             // DeliverPayloadAIUpdate: a transport flying a delivery
	int32 DeliverDoorDelay = 0;                 // frames between arriving and the first drop
	float SmartBombCourseCorrection = 0.f;      // SmartBombTargetHomingUpdate: how strongly a bomb holds its line (1 = not at all)
	int32 EMPDisabledDuration = 0;              // EMPUpdate: frames that vehicles and buildings caught in it stay dead
	float EMPRadius = 0.f;
	int32 EMPStartDelay = 0;                    // ... frames after it appears that the pulse takes effect (StartFadeTime)
	bool bEMPSparesOwnBuildings = false;
	bool bParachute = false;                    // ParachuteContain: carries one soldier down
	float ParachuteOpenDist = 0.f;              // ... after falling freely this far
	FString ParachuteOpenSound;                 // ... heard from the rider when the canopy opens
	TArray<TPair<FString, int32>> FireWeaponUpdates; // FireWeaponUpdate: (weapon, initial delay in frames) fired where the object is, whenever ready
	bool bHasProduction = false;        // ProductionUpdate: can queue units
	int32 MaxQueueEntries = 9;
	TArray<TPair<FString, int32>> QuantityModifiers; // ProductionUpdate: (unit, how many come out for each one built) - Red Guards in pairs
	int32 DoorOpeningTime = 0;          // frames a factory door takes to open (DOOR_n_OPENING)
	int32 DoorCloseTime = 0;
	int32 NumDoorAnimations = 0;        // ProductionUpdate: a unit that is finished waits for its door to open
	int32 DoorWaitOpenTime = 0;         // ... which then stands open this long (DOOR_n_WAITING_OPEN)
	int32 ConstructionCompleteDuration = 0; // ... and the building shows CONSTRUCTION_COMPLETE this long
	bool bHasExit = false;              // *ProductionExitUpdate
	bool bQueueExit = false;            // QueueProductionExitUpdate: one unit out of the door per ExitDelay
	int32 ProductionExitDelay = 0;      // ... frames
	int32 ProductionInitialBurst = 0;   // ... but for the first so many
	FVector3f UnitCreatePoint = FVector3f::ZeroVector;   // in the building's frame
	FVector3f NaturalRallyPoint = FVector3f::ZeroVector;
	TArray<FString> BuildVariations;    // the template is only a name for these: an object made from it is one of them, at random
	const FGenThingTemplate* VariationOf = nullptr; // ... and each of them points back at it
	/** The template that stands for this one when types are compared (ThingTemplate::isEquivalentTo). */
	const FGenThingTemplate* Canonical() const { return VariationOf ? VariationOf : this; }
	FGenContainData Contain;            // what it can carry or shelter, if anything
	FString ButtonImage;                // mapped image shown for it in a container's list of occupants
	FString SelectPortrait;             // the picture of it in the control bar's right panel when it is selected
	FString UpgradeCameos[5];           // UpgradeCameo1..5: the upgrades shown under it there
	int32 TransportSlotCount = 0;       // slots it takes up inside a transport (0: cannot be carried)
	bool bChinookAI = false;            // ChinookAIUpdate: an aircraft that lands to let passengers on and off
	// ... and its combat drop: soldiers slide down ropes onto the ground or into a building
	int32 ChinookNumRopes = 4;
	int32 ChinookRopeDelayMin = 0;      // frames between two soldiers on one rope
	int32 ChinookRopeDelayMax = 0;
	float ChinookRopeWidth = 0.5f;
	float ChinookRopeWobbleLen = 10.f;
	float ChinookRopeWobbleAmp = 0.f;
	float ChinookRopeWobbleRate = 0.f;  // radians per frame
	float ChinookRopeFinalHeight = 0.f; // the ropes end this far above the ground
	float ChinookRopeDropSpeed = 1e10f; // per frame
	float ChinookRappelSpeed = 0.f;     // per frame
	float ChinookMinDropHeight = 30.f;  // it hovers at least this far above a building it drops into
	bool bChinookWaitForRopes = true;   // nobody starts down a rope before it is all the way out
	bool bIsDozer = false;              // DozerAIUpdate / WorkerAIUpdate: can construct buildings
	float DozerRepairPerSecond = 0.f;   // ... share of a building's health it restores per second (RepairHealthPercentPerSecond)
	int32 DozerBoredTime = 0;           // ... frames of idling before it looks for something to repair
	float DozerBoredRange = 0.f;        // ... and how far it looks

	// Supply gathering
	bool bIsSupplyTruck = false;        // SupplyTruckAIUpdate / ChinookAIUpdate / WorkerAIUpdate: carries supply boxes
	int32 MaxBoxes = 0;
	int32 SupplyCenterActionDelay = 0;      // frames before the boxes are handed over at a supply center
	int32 SupplyWarehouseActionDelay = 0;   // frames per box picked up at a warehouse
	float SupplyWarehouseScanDistance = 100.f;
	bool bSupplyWarehouse = false;      // SupplyWarehouseDockUpdate: a pile of supply boxes
	bool bSupplyCenter = false;         // SupplyCenterDockUpdate: boxes brought here become money
	bool bRepairDock = false;           // RepairDockUpdate: vehicles drive in and are mended
	int32 RepairDockFrames = 1;         // ... in this many frames, however badly damaged (TimeForFullHeal)
	int32 StartingBoxes = 1;
	bool bDeleteWhenEmpty = false;
	int32 NumberApproachPositions = 0;  // DockUpdate: queue slots; -1 = as many as needed
	bool bDockAllowsPassthrough = true;
	FString SupplyBonePrefix;           // W3DSupplyDraw: bones "<prefix>NN" carry the boxes shown while supplies last
	// SpawnBehavior (OneShot): units a finished building comes with (a supply center's first gatherer)
	FString SpawnTemplate;
	int32 SpawnNumber = 0;
	/** SpawnBehavior that keeps its units (not OneShot): the stinger site's soldiers, each replaced a while after
	 *  it dies (GenSpawner.cpp). */
	struct FSpawner
	{
		bool bValid = false;
		FString Template;                   // SpawnTemplateName
		int32 Number = 0;                   // SpawnNumber
		int32 ReplaceFrames = 0;            // SpawnReplaceDelay
		bool bRequireSpawner = false;       // SpawnedRequireSpawner: they die with it
	};
	FSpawner Spawner;
	FString SpawnPointBone;             // SpawnPointProductionExitUpdate: they stand at these bones ("SpawnPoint01.."), held there
	TArray<FString> HivePropagateDamage; // HiveStructureBody: damage of these types goes to the spawn nearest the shooter ...
	TArray<FString> HiveSwallowDamage;  // ... and of these types is lost when there is none

	// JetAIUpdate: aircraft that live at an airfield (also helicopters built on its helipad)
	bool bJetAI = false;
	bool bJetNeedsRunway = true;            // No: lands and takes off vertically (HeliAIStateMachine)
	bool bJetKeepsParkingSpace = true;      // KeepsParkingSpaceWhenAirborne
	float JetOutOfAmmoDamagePerSecond = 0.f; // share of max health lost per second while out of ammo with no airfield
	int32 JetTakeoffPause = 0;              // frames at the head of the runway before the takeoff roll
	float JetMinHeight = 0.f;               // the model is never drawn lower than this above the ground (wheels on the tarmac)
	float JetParkingOffset = 0.f;
	int32 JetReturnToBaseIdleTime = 0;      // frames of idling in the air before heading home (0 = never)
	FString JetAttackLocoSet;               // AttackLocomotorType (upper case; empty = normal)
	int32 JetAttackLocoPersistTime = 0;
	FString JetReturnLocoSet;               // ReturnForAmmoLocomotorType
	float JetSneakyOffset = 0.f;            // SneakyOffsetWhenAttacking: while it attacks, whoever shoots at it aims this far ahead of it
	int32 JetAttackersMissPersistTime = 0;  // ... and for this many frames afterwards
	FGenCountermeasures Countermeasures;

	// VeterancyCrateCollide with IsPilot (a pilot gives the vehicle it climbs into its own rank) and PilotFindVehicleUpdate
	bool bPilot = false;
	bool bPilotAddsOwnVeterancy = false;
	TArray<FString> PilotRequiredKindOf;    // upper case
	TArray<FString> PilotForbiddenKindOf;
	int32 PilotScanFrames = 0;              // a computer player's pilot looks for a vehicle this often (0: never)
	float PilotScanRange = 0.f;
	float PilotMinHealth = 0.f;             // ... and takes none in worse shape than this share of its health
	FGenJetDeath JetDeath;
	FGenHeliDeath HeliDeath;
	FGenNeutronMissile NeutronMissile;
	FGenNeutronBlast NeutronBlast;
	FGenMissileLauncherBuilding MissileLauncher;
	FGenParticleCannon ParticleCannon;
	FGenSpectreGunship Gunship;
	TArray<FGenGunshipDeployment> GunshipDeployments;
	bool bForbidPlayerCommands = false;     // AIUpdateInterface's ForbidPlayerCommands: it does only what its own modules tell it
	bool bNoIdleAutoAcquire = false;        // AutoAcquireEnemiesWhenIdle = No: left alone, it picks no targets of its own
	FGenMinefield Minefield;
	FGenGenerateMinefield GenerateMinefield;
	/** WeaponBonusUpdate: every so often whatever of ours is near gets a weapon bonus for a while (the frenzy's marker). */
	struct FWeaponBonusPulse
	{
		bool bValid = false;
		TArray<FString> RequiredKindOf, ForbiddenKindOf;
		int32 Duration = 0, Delay = 0;   // frames
		float Range = 0.f;
		FString Condition;               // FRENZY_ONE ...
	};
	FWeaponBonusPulse WeaponBonusPulse;
	/** PropagandaTowerBehavior: our units (and our allies') around it fire faster and mend. */
	struct FPropagandaTower
	{
		bool bValid = false;
		float Radius = 1.f;
		int32 Delay = 1;                     // frames between looks around (and pulses)
		float HealPercent = 0.01f;           // of its health a second
		float UpgradedHealPercent = 0.02f;   // ... once UpgradeRequired is in
		FString PulseFX, UpgradedPulseFX, UpgradeRequired;
		bool bAffectsSelf = false;
	};
	FPropagandaTower Propaganda;
	/** SlavedUpdate: a drone, kept to the unit that bought it (ranges in world units from the master). */
	struct FSlaved
	{
		bool bValid = false;
		int32 GuardMaxRange = 0, GuardWanderRange = 0;     // the master idle
		int32 AttackRange = 0, AttackWanderRange = 0;      // the master attacking
		int32 ScoutRange = 0, ScoutWanderRange = 0;        // the master on its way somewhere
		int32 RepairRange = 0;                             // how far it zips between welds
		int32 DistToTargetToGrantRangeBonus = 0;           // this near the master's target, the master shoots further
		float RepairMinAltitude = 0.f, RepairMaxAltitude = 0.f;
		float RepairRatePerSecond = 0.f;
		int32 RepairWhenBelowPercent = 0;                  // mending comes first below this much of the master's health
		int32 MinReadyFrames = 0, MaxReadyFrames = 0, MinWeldFrames = 0, MaxWeldFrames = 0;
		FString WeldingSys, WeldingFXBone;
		bool bStayOnSameLayerAsMaster = false;
	};
	FSlaved Slaved;
	FString UpgradeDieRemove;               // UpgradeDie: dying, it takes this upgrade from what made it (which can buy it again)
	/** ConvertToHijackedVehicleCrateCollide / ConvertToCarBombCrateCollide: the soldier takes a vehicle over. */
	struct FVehicleTakeover
	{
		enum EKind : uint8 { None, Hijack, CarBomb };
		EKind Kind = None;
		TArray<FString> RequiredKindOf, ForbiddenKindOf;
		FString FX;                         // ConvertToCarBombCrateCollide FXList
	};
	FVehicleTakeover VehicleTakeover;
	/** The Sabotage...CrateCollide modules (the GLA saboteur): what it does to the enemy building it gets into
	 *  (GenHijack.cpp). Frames 0: that kind of building is no target. */
	struct FSabotage
	{
		int32 PowerFrames = 0;              // SabotagePowerPlantCrateCollide: the owner's power is cut for so long
		bool bSuperweapon = false;          // SabotageSuperweaponCrateCollide: its powers start recharging over
		bool bCommandCenter = false;        // SabotageCommandCenterCrateCollide: the same for a command center's
		int32 StealCash = 0;                // SabotageSupplyCenterCrateCollide: so much of the owner's money is ours
		int32 FactoryFrames = 0;            // SabotageMilitaryFactoryCrateCollide: barracks, war factory, airfield shut down
		bool bFakeBuilding = false;         // SabotageFakeBuildingCrateCollide: a GLA fake is blown up
		int32 InternetFrames = 0;           // SabotageInternetCenterCrateCollide: hackers and satellite hacks off for so long
		bool Any() const { return PowerFrames > 0 || bSuperweapon || bCommandCenter || StealCash > 0 || FactoryFrames > 0 || bFakeBuilding || InternetFrames > 0; }
	};
	FSabotage Sabotage;
	FString HijackerParachute;              // HijackerUpdate ParachuteName
	bool bHasMineClearingSet = false;       // a weapon set for MINE_CLEARING_DETAIL (dozers and workers)
	/** DemoTrapUpdate: the weapon slot in hand is the mode (detonate now, proximity, manual). */
	struct FDemoTrap
	{
		bool bValid = false;
		bool bDefaultProximityMode = false;
		int32 DetonationSlot = 0, ProximitySlot = 1, ManualSlot = 2;
		float TriggerRange = 0.f;
		TArray<FString> IgnoreKindOf;
		int32 ScanFrames = 0;
		bool bFriendlyDetonation = false;    // AutoDetonationWithFriendsInvolved
		FString DetonationWeapon;
		bool bDetonateWhenKilled = false;
	};
	FDemoTrap DemoTrap;
	/** LeafletDropBehavior: from Delay after it appears until it is gone, enemy soldiers and vehicles near it are disabled. */
	struct FLeafletDrop
	{
		bool bValid = false;
		int32 DisabledDuration = 0, Delay = 0; // frames
		float Radius = 0.f;
		FString ParticleSystem;
	};
	FLeafletDrop LeafletDrop;

	// ParkingPlaceBehavior: an airfield
	bool bParkingPlace = false;
	int32 ParkingRows = 0;
	int32 ParkingCols = 0;
	bool bParkingHasRunways = false;
	float ParkingApproachHeight = 0.f;
	bool bParkInHangars = false;
	float ParkingHealPerSecond = 0.f;

	// Crushing: a mover with CrusherLevel above the other's CrushableLevel drives over it; anything with
	// SquishCollide (infantry) is squished by any crusher.
	int32 CrusherLevel = 0;
	float FenceWidth = 0.f;             // above 0: a fence - so long, from where it stands along its facing
	float FenceXOffset = 0.f;
	bool bCrushDie = false;             // CrushDie: crushed, it shows which end was run over (FRONTCRUSHED / BACKCRUSHED)
	FString CrushSounds[3];             // ... with a sound for a total, a back end and a front end crush
	int32 CrushSoundPercent[3] = { 100, 100, 100 };
	int32 CrushableLevel = 255;
	bool bSquishable = false;
	FGenDumbProjectile DumbProjectile;
	bool bIsMissile = false;            // MissileAIUpdate
	bool bMissileUseWeaponSpeed = false; // ... flies at the weapon's speed instead of its own locomotor's
	float MissileInitialVelocity = 0.f; // ... speed it leaves the launcher with (per frame), until the engine lights
	int32 MissileIgnitionDelay = 0;     // ... frames
	bool bMissileTryToFollowTarget = true;  // ... follows what it was fired at (otherwise it flies to where that was)
	int32 MissileFuelLifetime = 0;      // ... frames of fuel (0: no end)
	float MissileInitialDist = 0.f;     // DistanceToTravelBeforeTurning
	float MissileDiveDistance = 0.f;    // DistanceToTargetBeforeDiving: from here on the preferred height no longer counts
	float MissileLockDistance = 75.f;   // DistanceToTargetForLock: from here on it is moved straight at the target
	FString MissileIgnitionFX;
	bool bMissileDetonateOnNoFuel = false;
	bool bMissileDetonateCallsKill = false; // it dies (its Die modules run) instead of just going away
	int32 MissileKillSelfDelay = 3;     // frames it lingers after going off, "just long enough for the contrail to catch up"

	/** Weapon / armor set for the given flags (PLAYER_UPGRADE ...): the set whose conditions all hold and that names the most of them. */
	const FGenWeaponSetEntry* FindWeaponSet(const TArray<FString>& Flags = TArray<FString>()) const;
	const FGenArmorSetEntry* FindArmorSet(const TArray<FString>& Flags = TArray<FString>()) const;

	bool IsKindOf(const TCHAR* Kind) const { return KindOf.Contains(Kind); }
	const FGenDrawModule* PrimaryDraw() const;

	/** GeometryInfo::getBoundingCircleRadius. */
	float GetBoundingCircleRadius() const;
	/** GeometryInfo::getBoundingSphereRadius. */
	float GetBoundingSphereRadius() const;
	bool IsBox() const { return GeometryType.Equals(TEXT("BOX"), ESearchCase::IgnoreCase); }
	/** GeometryInfo::getMaxHeightAbovePosition */
	float GetMaxHeightAbovePosition() const { return GeometryType.Equals(TEXT("SPHERE"), ESearchCase::IgnoreCase) ? GeometryMajorRadius : GeometryHeight; }
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

	// GameData.ini fog of war settings
	float PartitionCellSize = 40.f;               // the fog of war is kept per cell of this size
	int32 UnlookPersistFrames = 30;               // what a unit saw stays clear this long after it has moved on
	uint8 ClearAlpha = 255;                       // brightness of ground in view, of fogged and of unexplored ground
	uint8 FogAlpha = 127;
	uint8 ShroudAlpha = 0;
	FColor ShroudColor = FColor(255, 255, 255);
	FColor RadarWaterColor = FColor(140, 140, 255); // Water.ini

	bool Initialize();
	bool IsInitialized() const { return bInitialized; }

	const FGenThingTemplate* FindTemplate(const FString& Name) const;
	const FGenCrateData* FindCrateData(const FString& Name) const { return CrateData.Find(Name.ToLower()); }
	TMap<FString, FGenCrateData> CrateData;      // by lower-case name
	const FGenTerrainType* FindTerrain(const FString& Name) const;
	const FGenLocomotorTemplate* FindLocomotor(const FString& Name) const;
	const FGenPlayerTemplate* FindPlayerTemplate(const FString& Name) const;
	const TArray<FGenPlayerTemplate>& GetPlayerTemplates() const { return PlayerTemplates; }
	const TMap<FString, TSharedPtr<FGenThingTemplate>>& GetTemplates() const { return Templates; }

	const FGenWeaponTemplate* FindWeapon(const FString& Name) const;
	const FGenArmorTemplate* FindArmor(const FString& Name) const;
	const FGenDamageFX* FindDamageFX(const FString& Name) const;
	/** GameData.ini AutoFireParticle... / AutoSmokeParticle... / AutoAflameParticle...: small, medium and large fire, the same of smoke, then "aflame". */
	FGenAutoParticles AutoParticles[7];

	/** GameData.ini Gravity, per frame^2 (negative). */
	float Gravity = -1.f;
	float UnitDamagedThreshold = 0.5f;
	float UnitReallyDamagedThreshold = 0.1f;

	// GameData.ini economy settings
	float SellPercentage = 0.5f;                  // share of the cost refunded when selling
	float LowEnergyPenaltyModifier = 0.4f;        // how hard a power shortage slows production
	float MinLowEnergyProductionSpeed = 0.5f;
	float MaxLowEnergyProductionSpeed = 0.8f;
	float MultipleFactory = 0.f;                  // build time is multiplied by this for every further factory of the kind (1 in the game's data)
	float AllowedHeightVariationForBuilding = 10.f;
	int32 ValuePerSupplyBox = 100;                // money for each supply box delivered
	int32 MaxTunnelCapacity = 10;                 // units a player's tunnel network holds
	float HealthBonus[4] = { 1.f, 1.2f, 1.3f, 1.5f }; // max health by veterancy level (HealthBonus_Veteran ...)
	TArray<FGenWeaponBonus> GlobalWeaponBonuses;  // GameData.ini WeaponBonus lines: apply to every weapon (GARRISONED RANGE 133% ...)
	float SupplyBuildBorder = 0.f;                // supply centers keep this far from supply piles
	float GroupMoveClickToGatherFactor = 1.f;     // GroupMoveClickToGatherAreaFactor: a click inside the group's own area (scaled so) gathers it in
	bool bDrawSkyBox = false;                     // DrawSkyBox: the sky box (new_skybox.w3d) round the camera
	bool bUseCloudMap = false;                    // UseCloudMap: cloud shadows sliding over the terrain and the roads
	bool bUseLightMap = false;                    // UseLightMap: the light map (TSNoiseUrb) multiplied over them
	float SkyBoxPositionZ = 0.f;                  // SkyBoxPositionZ: the height of its centre ("lowers center of skybox under terrain")
	float SkyBoxScale = 4.5f;                     // SkyBoxScale: the model's scale
	float StandardMinefieldDensity = 0.004f;      // mines to the square foot
	float StandardMinefieldDistance = 40.f;       // how far out from what lays them
	bool bShroudInMultiplayer = false;            // Multiplayer.ini UseShroud: skirmish maps start unexplored

	/** Applies a map's map.ini overrides (objects defined in the map folder). */
	void ApplyMapIni(const FString& MapFolder);

private:
	void AddObjectBlock(const TSharedPtr<FGenIniBlock>& Block, bool bReskin);
	TSharedPtr<FGenThingTemplate> BuildTemplate(const FString& Name, const TSharedPtr<FGenIniBlock>& Merged) const;
	static TSharedPtr<FGenIniBlock> MergeBlocks(const FGenIniBlock* Base, const FGenIniBlock& Override);

	bool bInitialized = false;
	/** Default\Object.ini's DefaultThingTemplate: the modules every object has unless it brings its own
	    (a stealth module that waits for the GPS scrambler, and self-repair for elite and heroic units). */
	TSharedPtr<FGenThingTemplate> DefaultThing;
	TMap<FString, TSharedPtr<FGenIniBlock>> ObjectBlocks;     // key: lowercase name, merged definition
	TMap<FString, TSharedPtr<FGenThingTemplate>> Templates;   // key: lowercase name
	TMap<FString, FGenTerrainType> TerrainTypes;              // key: lowercase name
	TMap<FString, FGenLocomotorTemplate> Locomotors;          // key: lowercase name
	TArray<FGenPlayerTemplate> PlayerTemplates;
	TMap<FString, FGenWeaponTemplate> Weapons;                // key: lowercase name
	TMap<FString, FGenArmorTemplate> Armors;                  // key: lowercase name
	TMap<FString, FGenDamageFX> DamageFXs;                    // key: lowercase name
};
