#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

class APlayerController;
struct FGenHotkeySettings;
enum class EGenHotkeyStyle : uint8;

/**
 * The game's keyboard commands (the original's CommandMap.ini), each on a key the player may change.
 * The command bar's buttons have keys of their own (GetButtonKey; pressed in GenBarHotkeys.cpp), which come first
 * while a button on that key is shown. Esc always opens the menu;
 * Ctrl, Alt and Shift as modifiers of clicks (force attack, waypoints, adding to the selection) are fixed.
 */
enum class EGenAction : uint8
{
	Stop,
	AttackMove,
	Guard,
	Formation,
	SelectMatching,
	SelectAllUnits,
	SelectAllAircraft,
	Group0, Group1, Group2, Group3, Group4, Group5, Group6, Group7, Group8, Group9,
	ScrollLeft,
	ScrollRight,
	ScrollUp,
	ScrollDown,
	CameraReset,
	JumpToEvent,
	Menu,
	Count
};

/** A key with the modifiers that must be held with it. */
struct GENERALS1_API FGenKeyChord
{
	FKey Key;
	bool bCtrl = false;
	bool bShift = false;
	bool bAlt = false;

	FGenKeyChord() = default;
	FGenKeyChord(const FKey& InKey, bool bInCtrl = false, bool bInShift = false, bool bInAlt = false)
		: Key(InKey), bCtrl(bInCtrl), bShift(bInShift), bAlt(bInAlt) {}

	bool IsValid() const { return Key.IsValid(); }
	bool operator==(const FGenKeyChord& O) const { return Key == O.Key && bCtrl == O.bCtrl && bShift == O.bShift && bAlt == O.bAlt; }
	bool operator!=(const FGenKeyChord& O) const { return !(*this == O); }
	/** "Ctrl+F" with the engine's key names (what the settings file stores); "" for none. */
	FString ToString() const;
	/** "Ctrl + F" as the player reads it. */
	FString GetDisplayText() const;
	/** "Ctrl+F", "B": the short form a button's corner and its tooltip show. */
	FString GetShortText() const;
	/** From ToString's form; "" or "None" is a valid chord without a key. False if the key is unknown. */
	static bool Parse(const FString& Text, FGenKeyChord& Out);
};

/** What the options screen lists for a command. */
struct FGenActionInfo
{
	EGenAction Action;
	const TCHAR* Name;       // in the settings file: "AttackMove"
	const TCHAR* Label;      // on the screen: "Attack move"
	const TCHAR* Category;   // "Orders", "Selection", "Groups", "Camera", "Interface"
	FGenKeyChord Default;
};

/** The key each command is on, and questions about this frame's keys put in those terms. */
class GENERALS1_API FGenInput
{
public:
	FGenInput();

	/** The game's keys (loaded from the settings by UGenUserSettings). */
	static FGenInput& Get();
	static const TArray<FGenActionInfo>& GetActions();
	static const FGenActionInfo& GetInfo(EGenAction Action);
	static EGenAction GroupAction(int32 Group) { return EGenAction(int32(EGenAction::Group0) + FMath::Clamp(Group, 0, 9)); }

	/** Everything back to its default, then the settings' changes over it (unknown names are skipped). */
	void Load(const FGenHotkeySettings& Settings);
	/** Writes the style, the bindings that differ from the defaults and the buttons' own keys into Settings. */
	void Save(FGenHotkeySettings& Settings) const;
	void ResetToDefaults();

	// ---- the command bar's buttons (GenBarHotkeys.cpp) ----
	/**
	 * The key of a command bar button: Name is its CommandButton, Label the text it shows (whose '&' letter is
	 * its key in the original), Row / Col its cell of the command window (-1: it isn't one of the fourteen).
	 * Classic: the letter; Grid: the cell's (other buttons: the letter); Custom: the player's, else the letter.
	 */
	FGenKeyChord GetButtonKey(const FString& Name, const FString& Label, int32 Row, int32 Col) const;
	/** The original's key for a button's text: its '&' letter (none: no key). */
	static FGenKeyChord GetClassicKey(const FString& Label);
	/** The Grid style's key of a cell of the command window: Q W E R T Y U / A S D F G H J / Z X C V B N M. */
	static FGenKeyChord GetGridKey(int32 Row, int32 Col);
	EGenHotkeyStyle GetStyle() const { return Style; }
	void SetStyle(EGenHotkeyStyle InStyle) { Style = InStyle; }
	/** The player's own key for that button (Custom), if one is set (it may be "none": an invalid chord). */
	const FGenKeyChord* FindButtonOverride(const FString& Name) const { return ButtonKeys.Find(Name.ToLower()); }
	/** Sets the player's own key for the button; nullptr: back to its letter. */
	void SetButtonOverride(const FString& Name, const FGenKeyChord* Chord);
	int32 NumButtonOverrides() const { return ButtonKeys.Num(); }

	const FGenKeyChord& GetChord(EGenAction Action) const { return Chords[int32(Action)]; }
	void SetChord(EGenAction Action, const FGenKeyChord& Chord) { Chords[int32(Action)] = Chord; }
	/** The other command on that chord, if any (EGenAction::Count: none). */
	EGenAction FindConflict(EGenAction Action, const FGenKeyChord& Chord) const;

	/**
	 * The command's key went down this frame with its modifiers held. Modifiers it doesn't ask for may be
	 * held too (Ctrl + a group's number is still that group's key) - unless another command is on the same key
	 * with more of the held modifiers, which then has it. A key the command bar took this frame doesn't count.
	 */
	bool JustPressed(const APlayerController* PC, EGenAction Action) const;
	/** The command's key is held (for scrolling). */
	bool IsDown(const APlayerController* PC, EGenAction Action) const;

	/** The command bar's keys (GenBarHotkeys.cpp): this key belongs to a button this frame. */
	void Consume(const FKey& Key);
	bool IsConsumed(const FKey& Key) const;

	static bool CtrlDown(const APlayerController* PC);
	static bool ShiftDown(const APlayerController* PC);
	static bool AltDown(const APlayerController* PC);

private:
	bool ModifiersHeld(const APlayerController* PC, const FGenKeyChord& Chord) const;

	FGenKeyChord Chords[int32(EGenAction::Count)];
	EGenHotkeyStyle Style = EGenHotkeyStyle(0);   // (Classic)
	TMap<FString, FGenKeyChord> ButtonKeys;         // lower-case CommandButton name -> the player's key
	TMap<FString, FString> ButtonNames;             // ... -> the name as the settings spell it
	TArray<FKey, TInlineAllocator<4>> Consumed;
	uint64 ConsumedFrame = 0;
};
