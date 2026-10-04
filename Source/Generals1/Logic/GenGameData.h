#pragma once

#include "CoreMinimal.h"
#include "Core/GenIni.h"
#include "Logic/GenConditions.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenUpgrades.h"

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

	// Flight
	float Lift = 0.f;                // units/frame^2
	float LiftDamaged = -1.f;
	float SpeedLimitZ = 999999.f;    // units/frame
	float PreferredHeightDamping = 1.f;
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
	/** Higher above the ground than this, only anti-air weapons can target the unit (default: never). */
	int32 AirborneTargetingHeight = MAX_int32;
	/** Radius of the circle a winged aircraft flies while it has nowhere to go (negative: the other way round; 0: as tight as it can). */
	float CirclingRadius = 0.f;

	/** Moves only through the air: no ground pathfinding, no collisions with ground units. */
	bool IsAirOnly() const { return (Surfaces & GenSurface::Air) != 0 && (Surfaces & ~uint32(GenSurface::Air)) == 0; }
	/** Z_SURFACE_RELATIVE_HEIGHT / Z_ABSOLUTE_HEIGHT: holds PreferredHeight with lift. */
	bool UsesLift() const { return PreferredHeight != 0.f && (ZAxisBehavior.Equals(TEXT("SURFACE_RELATIVE_HEIGHT"), ESearchCase::IgnoreCase)
		|| ZAxisBehavior.Equals(TEXT("ABSOLUTE_HEIGHT"), ESearchCase::IgnoreCase)); }
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
	EGenReloadType ReloadType = EGenReloadType::Auto;
	int32 DelayBetweenShotsMin = 0;     // frames
	int32 DelayBetweenShotsMax = 0;
	int32 PreAttackDelay = 0;           // frames
	uint32 RadiusDamageAffects = GenWeaponAffects::Allies | GenWeaponAffects::Enemies | GenWeaponAffects::Neutrals;
	bool bAntiGround = true;
	bool bAntiAirborneVehicle = false;
	bool bAntiAirborneInfantry = false;
	bool bDamageDealtAtSelfPosition = false;
	TArray<FGenWeaponBonus> Bonuses;

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
	FString FXHitGround, OCLHitGround;
	FString FXFinalBlowUp, OCLFinalBlowUp;
	int32 DelayFromGroundToFinalDeath = 0;      // frames
	FString FinalRubbleObject;                  // the hull left lying there
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
	TArray<FString> IntrinsicSciences;   // sciences the faction starts with
	FString PurchaseScienceSets[3];      // command sets of the promotion screen: rank 1, rank 3, rank 8 (= 5 in Zero Hour)
	FString SpecialPowerShortcutSet;
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
	bool bStartFrameLast = false;   // Flags = START_FRAME_LAST: a MANUAL animation holds its last frame (a door standing open)
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
	float ShroudClearingRange = -1.f;   // how far it clears the fog of war (-1: as far as it sees)
	float ShroudRevealToAllRange = -1.f; // how far around it everybody else sees (superweapons)
	EGenRadarPriority RadarPriority = EGenRadarPriority::Invalid;
	bool bHasRadarUpdate = false;       // RadarUpdate: the dish that unfolds when the radar upgrade arrives
	int32 RadarExtendFrames = 0;
	FGenDynamicShroud DynamicShroud;    // DynamicShroudClearingRangeUpdate
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

	// Production and construction
	FString CommandSet;                 // CommandSet.ini entry shown when this object is selected
	FString DisplayName;                // OBJECT:... label
	int32 EnergyProduction = 0;         // > 0 produces power, < 0 consumes it
	int32 EnergyBonus = 0;              // added once the power plant has its upgrade (control rods)
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
	FGenAutoHeal AutoHeal;
	int32 UpgradedSupplyBoost = 0;              // extra money per load once the player has Supply Lines
	// Things that fall out of aircraft (and the markers powers leave on the ground)
	bool bHeightDie = false;                    // HeightDieUpdate: dies when this close to the ground
	float HeightDieTarget = 0.f;
	bool bHeightDieOnlyWhenMovingDown = false;
	int32 HeightDieInitialDelay = 0;            // frames
	TArray<FString> DeathWeapons;               // FireWeaponWhenDeadBehavior (the ones that start active)
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
	TArray<TPair<FString, int32>> FireWeaponUpdates; // FireWeaponUpdate: (weapon, initial delay in frames) fired where the object is, whenever ready
	bool bHasProduction = false;        // ProductionUpdate: can queue units
	int32 MaxQueueEntries = 9;
	int32 DoorOpeningTime = 0;          // frames a factory door takes to open (DOOR_n_OPENING)
	int32 DoorCloseTime = 0;
	bool bHasExit = false;              // *ProductionExitUpdate
	FVector3f UnitCreatePoint = FVector3f::ZeroVector;   // in the building's frame
	FVector3f NaturalRallyPoint = FVector3f::ZeroVector;
	TArray<FString> BuildVariations;    // the template is only a name for these: an object made from it is one of them, at random
	const FGenThingTemplate* VariationOf = nullptr; // ... and each of them points back at it
	/** The template that stands for this one when types are compared (ThingTemplate::isEquivalentTo). */
	const FGenThingTemplate* Canonical() const { return VariationOf ? VariationOf : this; }
	FGenContainData Contain;            // what it can carry or shelter, if anything
	FString ButtonImage;                // mapped image shown for it in a container's list of occupants
	int32 TransportSlotCount = 0;       // slots it takes up inside a transport (0: cannot be carried)
	bool bChinookAI = false;            // ChinookAIUpdate: an aircraft that lands to let passengers on and off
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
	FGenJetDeath JetDeath;
	FGenHeliDeath HeliDeath;

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
	int32 CrushableLevel = 255;
	bool bSquishable = false;
	FGenDumbProjectile DumbProjectile;
	bool bIsMissile = false;            // MissileAIUpdate
	bool bMissileUseWeaponSpeed = false; // ... flies at the weapon's speed instead of its own locomotor's
	float MissileInitialVelocity = 0.f; // ... speed it leaves the launcher with (per frame), until the engine lights
	int32 MissileIgnitionDelay = 0;     // ... frames

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

	// GameData.ini economy settings
	float SellPercentage = 0.5f;                  // share of the cost refunded when selling
	float LowEnergyPenaltyModifier = 0.4f;        // how hard a power shortage slows production
	float MinLowEnergyProductionSpeed = 0.5f;
	float MaxLowEnergyProductionSpeed = 0.8f;
	float AllowedHeightVariationForBuilding = 10.f;
	int32 ValuePerSupplyBox = 100;                // money for each supply box delivered
	int32 MaxTunnelCapacity = 10;                 // units a player's tunnel network holds
	float HealthBonus[4] = { 1.f, 1.2f, 1.3f, 1.5f }; // max health by veterancy level (HealthBonus_Veteran ...)
	TArray<FGenWeaponBonus> GlobalWeaponBonuses;  // GameData.ini WeaponBonus lines: apply to every weapon (GARRISONED RANGE 133% ...)
	float SupplyBuildBorder = 0.f;                // supply centers keep this far from supply piles
	bool bShroudInMultiplayer = false;            // Multiplayer.ini UseShroud: skirmish maps start unexplored

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
