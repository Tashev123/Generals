#pragma once

#include "CoreMinimal.h"

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
	FString ButtonImage;     // mapped image name
	FString Upgrade;         // upgrade researched, or (NEED_UPGRADE) needed before the button works
	TArray<FString> Sciences; // science bought; for a power, the sciences of which one is needed
	FString SpecialPower;
	bool bNeedTargetPos = false;       // NEED_TARGET_POS: pick a spot on the ground next
	bool bNeedTargetObject = false;    // NEED_TARGET_ENEMY_OBJECT / NEUTRAL / ALLY
	bool bNeedSpecialPowerScience = false;
	bool bNeedUpgrade = false;
	bool bFromShortcut = false;        // the power is fired from whichever building of ours has it
	float RadiusCursor = 0.f;          // radius shown while aiming (from the power's template)
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

	/** String for a label such as "CONTROLBAR:Sell" (hotkey markers removed); the label itself if unknown. */
	FString GetText(const FString& Label) const;

private:
	void LoadStrings();

	bool bInitialized = false;
	TMap<FString, FGenCommandButton> Buttons;   // key: lowercase name
	TMap<FString, FGenCommandSet> Sets;
	TMap<FString, FGenMappedImage> Images;
	TMap<FString, FString> Strings;             // key: lowercase label
};
