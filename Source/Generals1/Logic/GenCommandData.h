#pragma once

#include "CoreMinimal.h"
#include "Logic/GenRadiusDecal.h"

/** CommandButton "Command" values that are implemented; everything else is shown but inert. */
enum class EGenCommand : uint8
{
	Other,
	UnitBuild,        // UNIT_BUILD: queue Object at this factory
	DozerConstruct,   // DOZER_CONSTRUCT: place Object as a building
	Sell,
	Stop,
	PlayerUpgrade,    // PLAYER_UPGRADE: research Upgrade for the whole player
	ObjectUpgrade,    // OBJECT_UPGRADE: research Upgrade for this object
	PurchaseScience,  // PURCHASE_SCIENCE: spend a general's point on Science
	SpecialPower,     // SPECIAL_POWER (and ..._FROM_SHORTCUT): fire SpecialPower, usually at a target
	ExitContainer,    // EXIT_CONTAINER: one occupant of the selected container gets out (one button per occupant)
	Evacuate,         // EVACUATE: everybody out
	AttackMove,       // ATTACK_MOVE: go there, fighting whatever comes into view on the way
	Guard,            // GUARD: hold a spot (or escort an object), attacking what comes near and chasing it a little
	GuardWithoutPursuit, // GUARD_WITHOUT_PURSUIT: the same, but never leaving the post
	GuardFlyingOnly,  // GUARD_FLYING_UNITS_ONLY: the same, against aircraft only
	HackInternet,     // HACK_INTERNET: the hacker sits down where it is and makes money
	FireWeapon,       // FIRE_WEAPON: use the weapon of WeaponSlot on a target (a knife, a sniper's shot at a driver)
	CombatDrop,       // COMBATDROP: the soldiers on board slide down ropes at the spot, or into the building, clicked
	SwitchWeapon,     // SWITCH_WEAPON: the weapon of WeaponSlot from now on (a Ranger's flash-bangs, and back to the gun)
	SetRallyPoint,    // SET_RALLY_POINT: pick the spot where what the building makes goes
	DozerConstructCancel, // DOZER_CONSTRUCT_CANCEL: the building site is given up, its price refunded
	ToggleOvercharge, // TOGGLE_OVERCHARGE: a nuclear reactor's overcharge on or off
	HijackVehicle,    // HIJACK_VEHICLE: the hijacker takes the enemy vehicle clicked
	ConvertToCarBomb, // CONVERT_TO_CARBOMB: the terrorist turns the vehicle clicked into a car bomb
	SabotageBuilding, // SABOTAGE_BUILDING: the saboteur goes into the enemy building clicked
};

/** CommandButton.ini entry. */
struct GENERALS1_API FGenCommandButton
{
	FString Name;
	EGenCommand Command = EGenCommand::Other;
	FString CommandName;     // raw "Command" value
	FString Object;          // template built / constructed
	FString TextLabel;       // CONTROLBAR:... (generals.csf)
	FString DescriptLabel;
	FString ConflictingLabel; // a promotion button's text for the power's shortcut (OBJECT:MOAB)
	FString ButtonImage;     // mapped image name
	FString Upgrade;         // upgrade researched, or (NEED_UPGRADE) needed before the button works
	TArray<FString> Sciences; // science bought; for a power, the sciences of which one is needed
	FString SpecialPower;
	bool bNeedTargetPos = false;       // NEED_TARGET_POS: pick a spot on the ground next
	bool bNeedTargetObject = false;    // NEED_TARGET_ENEMY_OBJECT / NEUTRAL / ALLY
	bool bUsesMineClearingWeaponSet = false; // USES_MINE_CLEARING_WEAPONSET: the weapon is the mine-clearing set's
	bool bNeedSpecialPowerScience = false;
	bool bNeedUpgrade = false;
	bool bFromShortcut = false;        // the power is fired from whichever building of ours has it
	bool bConstruct = false;           // SPECIAL_POWER_CONSTRUCT: the power puts Object down, at a spot picked like a building's site
	bool bContextMode = false;         // CONTEXTMODE_COMMAND
	bool bScriptOnly = false;          // SCRIPT_ONLY: never shown (the MOAB's promotion, there for its picture and text)
	int32 WeaponSlot = INDEX_NONE;     // FIRE_WEAPON: 0 PRIMARY, 1 SECONDARY, 2 TERTIARY
	int32 CommandOption = 0;           // OPTION_ONE / TWO / THREE: 1, 2, 3 (which battle plan; the strategy center's stop)
	int32 MaxShotsToFire = MAX_int32;  // FIRE_WEAPON: shots before the attack is over ("huge number")
	uint8 BorderType = 0;              // ButtonBorderType: 0 NONE, 1 BUILD, 2 UPGRADE, 3 ACTION, 4 SYSTEM (the scheme's border colours)
	float RadiusCursor = 0.f;          // radius shown while aiming (from the power's template)
	FString RadiusCursorType;          // RadiusCursorType: which of InGameUI.ini's radius cursors shows while it is aimed
	FString CursorName;                // the cursor while aiming it at something it can be used on (Mouse.ini's names)
	FString InvalidCursorName;         // ... and at something it can't
};

/** CommandSet.ini entry: button names by slot (index 0 = slot 1). */
struct GENERALS1_API FGenCommandSet
{
	static constexpr int32 NumSlots = 18;
	FString Name;
	FString Slots[NumSlots];
};

/** MappedImages entry: a rectangle of a UI texture page. */
struct GENERALS1_API FGenMappedImage
{
	FString Texture;
	FVector2f UV0 = FVector2f::ZeroVector;   // normalized
	FVector2f UV1 = FVector2f(1.f, 1.f);
	FVector2f Size = FVector2f::ZeroVector;  // pixels
};

/**
 * Command bar data: CommandSet.ini, CommandButton.ini, the mapped image atlases and the game's string
 * file (generals.csf) for button names and tooltips.
 */
class GENERALS1_API FGenCommandData
{
public:
	static FGenCommandData& Get();

	bool Initialize();

	const FGenCommandButton* FindButton(const FString& Name) const;
	const FGenCommandSet* FindSet(const FString& Name) const;
	const FGenMappedImage* FindImage(const FString& Name) const;
	/** InGameUI.ini's radius cursor of a RadiusCursorType ("NUCLEARMISSILE"); none: nullptr. */
	const FGenRadiusDecalTemplate* FindRadiusCursor(const FString& Type) const;

	/** String for a label such as "CONTROLBAR:Sell" (hotkey markers removed); the label itself if unknown. */
	FString GetText(const FString& Label) const;
	/** The original's hotkey of a label: the letter its text marks with '&' ("&Barracks": 'B', upper case;
	 *  HotKeyManager::searchHotKey). 0: none. */
	TCHAR GetHotkeyLetter(const FString& Label) const;

	/** Every button and command set (keys: lower-case names), for the options' hotkeys page. */
	const TMap<FString, FGenCommandButton>& GetButtons() const { return Buttons; }
	const TMap<FString, FGenCommandSet>& GetSets() const { return Sets; }

private:
	void LoadStrings();

	bool bInitialized = false;
	TMap<FString, FGenCommandButton> Buttons;   // key: lowercase name
	TMap<FString, FGenCommandSet> Sets;
	TMap<FString, FGenMappedImage> Images;
	TMap<FString, FGenRadiusDecalTemplate> RadiusCursors; // key: the RadiusCursorType, upper case
	TMap<FString, FString> Strings;             // key: lowercase label
	TMap<FString, TCHAR> HotkeyLetters;         // key: lowercase label; the letter after its text's '&'
};
