#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

struct FGenIniBlock;

/** Upgrade.ini entry (UpgradeTemplate). */
struct GENERALS1_API FGenUpgradeTemplate
{
	FString Name;
	FString DisplayName;            // UPGRADE:... label
	bool bObjectUpgrade = false;    // Type = OBJECT: belongs to the one object that researched it; otherwise to the player
	int32 BuildCost = 0;
	float BuildTime = 0.f;          // seconds
	FString ButtonImage;
	FString ResearchSound;          // played for its owner when it is done
};

/** Science.ini entry: a general's promotion (unlocks a unit or a power, or makes a power stronger). */
struct GENERALS1_API FGenScience
{
	FString Name;
	TArray<FString> Prerequisites;  // all of these must be owned first
	int32 PurchaseCost = 0;         // general's points; 0 = cannot be bought (granted by rank or by an upgrade)
	bool bGrantable = true;
	FString DisplayName;
	FString Description;
};

/** Rank.ini entry. */
struct GENERALS1_API FGenRank
{
	int32 SkillPointsNeeded = 0;
	TArray<FString> SciencesGranted;
	int32 PurchasePointsGranted = 0;
};

/** What a unit's SpecialAbilityUpdate does - from its power's Enum (SPECIAL_...). */
enum class EGenAbilityKind : uint8
{
	Other,
	CaptureBuilding,        // SPECIAL_INFANTRY_CAPTURE_BUILDING: the flag
	LotusCapture,           // SPECIAL_BLACKLOTUS_CAPTURE_BUILDING: the same from a distance, by laptop
	LotusVehicleHack,       // SPECIAL_BLACKLOTUS_DISABLE_VEHICLE_HACK: a vehicle is dead for a while
	LotusCashHack,          // SPECIAL_BLACKLOTUS_STEAL_CASH_HACK: money out of a supply center
	HackerDisableBuilding,  // SPECIAL_HACKER_DISABLE_BUILDING: a building is dead for as long as the hacker keeps at it
	RemoteCharges,          // SPECIAL_REMOTE_CHARGES: charges that go off when told to (no target = "detonate")
	TimedCharges,           // SPECIAL_TIMED_CHARGES
	TankHunterTNT,          // SPECIAL_TANKHUNTER_TNT_ATTACK
	BoobyTrap,              // SPECIAL_BOOBY_TRAP: on an own or neutral building; goes off under whoever takes it
	LaserGuidedMissiles,    // SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES: paints a vehicle and fires faster at it
	HelixNapalmBomb,
	Disguise,
};

/** SpecialPower.ini entry (SpecialPowerTemplate). */
struct GENERALS1_API FGenSpecialPowerTemplate
{
	FString Name;
	FString Enum;                   // SPECIAL_...
	int32 ReloadTime = 0;           // frames
	FString RequiredScience;
	bool bPublicTimer = false;
	bool bSharedSyncedTimer = false; // one timer per player, whichever building the power is fired from
	float RadiusCursorRadius = 0.f;
	FString InitiateSound;          // heard by whoever fires it
	FString InitiateAtLocationSound; // heard where it is aimed
	bool bPorted = false;           // what the power does is in the game (otherwise its button stays greyed out)
	int32 ViewObjectFrames = 0;     // SpecialPowerModule::createViewObject: the spot it is fired at is seen this long ...
	float ViewObjectRange = 0.f;    // ... this far round
	EGenAbilityKind AbilityKind = EGenAbilityKind::Other;
};

/** What a module does when the upgrade that triggers it arrives (UpgradeMux and the modules built on it). */
enum class EGenUpgradeEffect : uint8
{
	Other,              // not ported: has no effect
	WeaponSet,          // WeaponSetUpgrade: the PLAYER_UPGRADE weapon set
	Armor,              // ArmorUpgrade: the PLAYER_UPGRADE armor set
	MaxHealth,
	LocomotorSet,       // SET_NORMAL_UPGRADED
	PowerPlant,         // control rods: EnergyBonus on top of EnergyProduction
	CommandSet,
	ModelCondition,
	SubObjects,
	GrantScience,
	WeaponBonus,        // the PLAYER_UPGRADE weapon bonus
	AutoHeal,           // switches the object's AutoHealBehavior on
	UnpauseSpecialPower,
	Radar,              // RadarUpgrade: its owner has a radar while it stands (and isn't knocked out)
	Stealth,            // StealthUpgrade: the object can stealth from now on (camouflage, camo netting)
	CostModifier,       // CostModifierUpgrade: things of a kind cost its owner less (or more) while it stands
	Countermeasures,    // CountermeasuresBehavior: the aircraft carries flares from now on
	GenerateMinefield,  // GenerateMinefieldBehavior: mines are laid around the object
	ObjectCreation,     // ObjectCreationUpgrade: an ObjectCreationList is run (a drone, an Overlord's add-on)
	PassengersFire,     // PassengersFireUpgrade: whoever rides in it may shoot out from now on (a Helix's bunker)
	SpyVision,          // SpyVisionUpdate (NeedsUpgrade): its owner sees what the enemy's things of some kinds see (satellite hacks)
};

struct GENERALS1_API FGenUpgradeModule
{
	EGenUpgradeEffect Effect = EGenUpgradeEffect::Other;
	FString ModuleType;
	TArray<FString> TriggeredBy;
	TArray<FString> ConflictsWith;
	bool bRequiresAllTriggers = false;
	float AddMaxHealth = 0.f;               // MaxHealthUpgrade
	FString ChangeType;                     // SAME_CURRENTHEALTH, PRESERVE_RATIO, ADD_CURRENT_HEALTH_TOO
	FString CommandSet;                     // CommandSetUpgrade
	FString CommandSetAlt;                  // ... the set used instead when TriggerAlt is owned
	FString TriggerAlt;
	FString ConditionFlag;                  // ModelConditionUpgrade
	TArray<FString> ShowSubObjects;         // SubObjectsUpgrade
	TArray<FString> HideSubObjects;
	FString Science;                        // GrantScienceUpgrade
	FString SpecialPower;                   // UnpauseSpecialPowerUpgrade
	FString OCL;                            // ObjectCreationUpgrade
	bool bDisableProof = false;             // RadarUpgrade: keeps working when the power is short
	TArray<FString> EffectKindOf;           // CostModifierUpgrade: what it applies to (all of these kinds, upper case)
	float Percentage = 0.f;                 // ... and by how much (-0.1 = 10% cheaper)
	TArray<FString> SpyOnKindOf;            // SpyVisionUpdate: whose eyes (empty: everything of the enemy's)
	int32 SpyDuration = 0;                  // ... on for this long (0: for good) ...
	int32 SpyInterval = 0;                  // ... then off for this long (SelfPoweredDuration / SelfPoweredInterval)
};

/** AutoHealBehavior: repairs the object itself (Radius 0) or what stands around it. */
struct GENERALS1_API FGenAutoHeal
{
	bool bValid = false;
	float Amount = 0.f;             // health per pulse
	int32 Delay = 1;                // frames between pulses
	int32 StartDelay = 0;           // frames without being damaged before healing (re)starts
	float Radius = 0.f;
	bool bStartsActive = false;
	bool bSingleBurst = false;      // one pulse, then done (the Emergency Repair marker)
	bool bAffectsWholePlayer = false;
	TArray<FString> KindOf;         // only these are healed (any of)
	TArray<FString> ForbiddenKindOf;
};

/** OCLUpdate: runs an ObjectCreationList every so often (a supply drop, reinforcements for a pad's owner). */
struct GENERALS1_API FGenOCLUpdate
{
	bool bValid = false;
	FString OCL;
	int32 MinDelay = 0;                 // frames
	int32 MaxDelay = 0;
	bool bCreateAtEdge = false;         // it starts from the map's edge nearest the object (an aircraft)
	bool bFactionTriggered = false;     // only while somebody owns it, and what comes depends on whose it is
	TArray<TPair<FString, FString>> FactionOCL; // (side, OCL)
};

enum class EGenCrate : uint8 { None, Money, Salvage, Unit };

/** CrateCollide and the modules built on it: something for whoever touches it. */
struct GENERALS1_API FGenCrateCollide
{
	EGenCrate Type = EGenCrate::None;
	bool IsValid() const { return Type != EGenCrate::None; }
	// CrateCollide: who may take it
	bool bBuildingPickup = false;       // a building it lands on collects it (the supply drop zone)
	bool bForbidOwnerPlayer = false;
	bool bHumanOnly = false;
	FString PickupScience;              // only a player with this science (a salvage crate: the GLA's)
	FString ExecuteFX;
	TArray<FString> RequiredKindOf;
	TArray<FString> ForbiddenKindOf;
	// MoneyCrateCollide
	int32 Money = 0;
	TArray<TPair<FString, int32>> UpgradeBoost; // (upgrade, extra)
	// SalvageCrateCollide: a better weapon first, then with LevelChance a rank, or else money
	float WeaponChance = 1.f;
	float LevelChance = 0.25f;
	int32 MinMoney = 25, MaxMoney = 75;
	// UnitCrateCollide
	FString UnitName;
	int32 UnitCount = 0;
};

/** Special power modules that are ported. */
enum class EGenPowerModule : uint8
{
	Other,          // shown in the command bar, but cannot be used yet
	OCL,            // OCLSpecialPower: runs an ObjectCreationList (a plane with a payload, units out of nowhere, a marker)
	CashHack,       // takes money from the owner of the targeted supply center
	CashBounty,     // passive: a share of the cost of everything this player destroys
	SpyVision,      // SpyVisionSpecialPower: for a while the player sees what the enemy's units see
	SpecialAbility, // SpecialAbility: a unit's own ability; its SpecialAbilityUpdate carries it out
};

struct GENERALS1_API FGenSpecialPowerModule
{
	EGenPowerModule Type = EGenPowerModule::Other;
	FString ModuleType;
	FString Power;                              // SpecialPowerTemplate
	FString OCL;
	TArray<TPair<FString, FString>> UpgradeOCLs; // (science, OCL): the first science owned decides
	FString CreateLocation;                     // CREATE_AT_EDGE_NEAR_SOURCE, CREATE_AT_LOCATION, ... (upper case)
	bool bAdjustToPassable = false;
	FString ReferenceObject;                    // OCLSpecialPower: the building the power puts down - it goes only where that could be built
	bool bScriptedOnly = false;                 // ScriptedSpecialPowerOnly: not for the command bar
	bool bStartsPaused = false;
	int32 MoneyAmount = 0;                      // CashHackSpecialPower
	TArray<TPair<FString, int32>> UpgradeMoney;
	float Bounty = 0.f;                         // CashBountyPower: share of the victim's cost
	int32 Duration = 0;                         // SpyVisionSpecialPower: BaseDuration, frames
	FString InitiateSound;                      // SpecialAbility: what the unit says when sent to do it
};

/**
 * SpecialAbilityUpdate: how a unit carries out one of its abilities - walk up to the target, unpack,
 * prepare for a while, do it, pack up again.
 */
struct GENERALS1_API FGenAbilityData
{
	FString Power;                      // SpecialPowerTemplate
	float StartAbilityRange = 1.e7f;    // SPECIAL_ABILITY_HUGE_DISTANCE
	float AbortRange = 1.e7f;
	int32 UnpackFrames = 0;
	int32 PreparationFrames = 0;
	int32 PackFrames = 0;
	float PackUnpackVariation = 0.f;
	bool bSkipPackingWithNoTarget = false;
	bool bNeedToFaceTarget = true;
	bool bDoCaptureFX = false;          // the target flashes in the capturer's colour, faster and faster
	int32 AwardXP = 0;                  // AwardXPForTriggering
	int32 SkillPoints = -1;             // SkillPointsForTriggering (-1: as AwardXP)
	FString UnpackSound, PackSound, PrepSoundLoop, TriggerSound;
	int32 PersistentPrepFrames = 0;     // > 0: once triggered it triggers again every so often, until told otherwise
	bool bPersistenceRequiresRecharge = false;
	int32 EffectDuration = 0;           // frames a hacked thing stays dead after the last trigger
	int32 EffectValue = 1;
	FString SpecialObject;              // what it makes: a charge it plants, or the beam it holds on its target
	FString SpecialObjectBone;          // SpecialObjectAttachToBone: where a beam starts
	int32 MaxSpecialObjects = 1;
	bool bSpecialObjectsPersistent = false;         // they stay when the unit turns to something else
	bool bSpecialObjectsPersistWhenOwnerDies = false;
	bool bUniqueSpecialObjectTargets = false;
	bool bFlipAfterUnpacking = false;   // FlipOwnerAfterUnpacking: the animation leaves it facing the other way
	bool bFlipAfterPacking = false;
	float FleeRange = 0.f;              // FleeRangeAfterCompletion: runs this far from what it has just done
	bool bLoseStealthOnTrigger = false;
	int32 PreTriggerUnstealthFrames = 0;
	bool bApproachRequiresLOS = true;
	FString DisableFXParticleSystem;    // on the thing being hacked
};

/** HackInternetAIUpdate: a hacker sits down, unpacks, and makes money out of thin air until told otherwise. */
struct GENERALS1_API FGenHackInternet
{
	bool bValid = false;
	int32 UnpackFrames = 0;
	int32 PackFrames = 0;
	int32 CashDelay = 0;                // frames between payments
	int32 CashDelayFast = 0;            // ... inside an Internet Center
	int32 Cash[4] = { 0, 0, 0, 0 };     // by veterancy
	int32 XpPerCash = 0;
	float PackUnpackVariation = 0.f;
};

/** StickyBombUpdate: a charge that sits on its target (and rides along if the target moves). */
struct GENERALS1_API FGenStickyBomb
{
	bool bValid = false;
	float OffsetZ = 10.f;
	FString GeometryWeapon;             // GeometryBasedDamageWeapon: damage reaching out from the target's own outline
	FString GeometryFX;
};

/** W3DLaserDraw (+ LaserUpdate): a beam between two points - a laser pointer, a hacker's stream of numbers. */
struct GENERALS1_API FGenLaserDraw
{
	bool bValid = false;
	FString Texture;
	int32 NumBeams = 1;
	float InnerBeamWidth = 0.f;
	float OuterBeamWidth = 1.f;
	FLinearColor InnerColor = FLinearColor::White;
	FLinearColor OuterColor = FLinearColor::White;
	bool bTile = false;
	float ScrollRate = 0.f;
	int32 Segments = 1;
	float ArcHeight = 0.f;
	float SegmentOverlapRatio = 0.f;
	float TilingScalar = 1.f;
	FString MuzzleParticleSystem;
	FString TargetParticleSystem;
};

/** AutoDepositUpdate: money for its owner every so often (oil derricks, black markets). */
struct GENERALS1_API FGenAutoDeposit
{
	bool bValid = false;
	int32 Frames = 0;                   // DepositTiming
	int32 Amount = 0;
	int32 CaptureBonus = 0;             // InitialCaptureBonus: paid once to whoever takes it
	bool bActualMoney = true;
	TArray<TPair<FString, int32>> UpgradeBoost; // (upgrade, extra per deposit)
};

/** Registry of Upgrade.ini, Science.ini, Rank.ini and SpecialPower.ini. */
class GENERALS1_API FGenUpgradeData
{
public:
	static FGenUpgradeData& Get();

	bool Initialize();

	const FGenUpgradeTemplate* FindUpgrade(const FString& Name) const;
	const FGenScience* FindScience(const FString& Name) const;
	const FGenSpecialPowerTemplate* FindPower(const FString& Name) const;
	/** Rank 1 is index 0. */
	const TArray<FGenRank>& GetRanks() const { return Ranks; }

	// Module parsing for object templates (called from FGenGameData).
	/** True if the block is an upgrade-triggered module (has TriggeredBy) of a kind listed in EGenUpgradeEffect. */
	static bool ParseUpgradeModule(const FString& Type, const FGenIniBlock& Block, FGenUpgradeModule& Out);
	static bool ParseSpecialPowerModule(const FString& Type, const FGenIniBlock& Block, FGenSpecialPowerModule& Out);
	static void ParseAutoHeal(const FGenIniBlock& Block, FGenAutoHeal& Out);
	static void ParseAbility(const FGenIniBlock& Block, FGenAbilityData& Out);
	static void ParseAutoDeposit(const FGenIniBlock& Block, FGenAutoDeposit& Out);
	static void ParseHackInternet(const FGenIniBlock& Block, FGenHackInternet& Out);
	static void ParseLaserDraw(const FGenIniBlock& Block, FGenLaserDraw& Out);

private:
	bool bInitialized = false;
	TMap<FString, FGenUpgradeTemplate> Upgrades;        // key: lowercase name
	TMap<FString, FGenScience> Sciences;
	TMap<FString, FGenSpecialPowerTemplate> Powers;
	TArray<FGenRank> Ranks;
};
