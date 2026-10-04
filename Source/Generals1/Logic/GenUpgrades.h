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
	bool bPorted = false;           // what the power does is in the game (otherwise its button stays greyed out)
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
	bool bDisableProof = false;             // RadarUpgrade: keeps working when the power is short
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

/** Special power modules that are ported. */
enum class EGenPowerModule : uint8
{
	Other,          // shown in the command bar, but cannot be used yet
	OCL,            // OCLSpecialPower: runs an ObjectCreationList (a plane with a payload, units out of nowhere, a marker)
	CashHack,       // takes money from the owner of the targeted supply center
	CashBounty,     // passive: a share of the cost of everything this player destroys
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
	bool bScriptedOnly = false;                 // ScriptedSpecialPowerOnly: not for the command bar
	bool bStartsPaused = false;
	int32 MoneyAmount = 0;                      // CashHackSpecialPower
	TArray<TPair<FString, int32>> UpgradeMoney;
	float Bounty = 0.f;                         // CashBountyPower: share of the victim's cost
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

private:
	bool bInitialized = false;
	TMap<FString, FGenUpgradeTemplate> Upgrades;        // key: lowercase name
	TMap<FString, FGenScience> Sciences;
	TMap<FString, FGenSpecialPowerTemplate> Powers;
	TArray<FGenRank> Ranks;
};
