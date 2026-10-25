#include "Game/GenInput.h"
#include "Game/GenSettings.h"
#include "Logic/GenCommandData.h"
#include "GameFramework/PlayerController.h"

// ---- chords ---------------------------------------------------------------------------------------

FString FGenKeyChord::ToString() const
{
	if (!Key.IsValid())
	{
		return FString();
	}
	FString S;
	if (bCtrl) { S += TEXT("Ctrl+"); }
	if (bShift) { S += TEXT("Shift+"); }
	if (bAlt) { S += TEXT("Alt+"); }
	return S + Key.GetFName().ToString();
}

FString FGenKeyChord::GetDisplayText() const
{
	if (!Key.IsValid())
	{
		return TEXT("-");
	}
	FString S;
	if (bCtrl) { S += TEXT("Ctrl + "); }
	if (bShift) { S += TEXT("Shift + "); }
	if (bAlt) { S += TEXT("Alt + "); }
	return S + Key.GetDisplayName(false).ToString();
}

FString FGenKeyChord::GetShortText() const
{
	if (!Key.IsValid())
	{
		return FString();
	}
	FString S;
	if (bCtrl) { S += TEXT("Ctrl+"); }
	if (bShift) { S += TEXT("Shift+"); }
	if (bAlt) { S += TEXT("Alt+"); }
	return S + Key.GetDisplayName(false).ToString();
}

bool FGenKeyChord::Parse(const FString& Text, FGenKeyChord& Out)
{
	Out = FGenKeyChord();
	FString Rest = Text.TrimStartAndEnd();
	if (Rest.IsEmpty() || Rest.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	for (;;)
	{
		if (Rest.StartsWith(TEXT("Ctrl+"))) { Out.bCtrl = true; Rest.RightChopInline(5); }
		else if (Rest.StartsWith(TEXT("Shift+"))) { Out.bShift = true; Rest.RightChopInline(6); }
		else if (Rest.StartsWith(TEXT("Alt+"))) { Out.bAlt = true; Rest.RightChopInline(4); }
		else { break; }
	}
	const FKey Key(*Rest);
	if (!Key.IsValid())
	{
		Out = FGenKeyChord();
		return false;
	}
	Out.Key = Key;
	return true;
}

// ---- the commands ---------------------------------------------------------------------------------

const TArray<FGenActionInfo>& FGenInput::GetActions()
{
	// In EGenAction's order. The defaults are the keys the game had before they could be changed (CommandMap.ini's).
	static const TArray<FGenActionInfo> Actions = {
		{ EGenAction::Stop,              TEXT("Stop"),              TEXT("Stop"),                         TEXT("Orders"),    FGenKeyChord(EKeys::S) },
		{ EGenAction::AttackMove,        TEXT("AttackMove"),        TEXT("Attack move"),                  TEXT("Orders"),    FGenKeyChord(EKeys::A) },
		{ EGenAction::Guard,             TEXT("Guard"),             TEXT("Guard"),                        TEXT("Orders"),    FGenKeyChord(EKeys::G) },
		{ EGenAction::Formation,         TEXT("Formation"),         TEXT("Make / break formation"),       TEXT("Orders"),    FGenKeyChord(EKeys::F, true) },
		{ EGenAction::SelectMatching,    TEXT("SelectMatching"),    TEXT("Select all of the same kind"),  TEXT("Selection"), FGenKeyChord(EKeys::E) },
		{ EGenAction::SelectAllUnits,    TEXT("SelectAllUnits"),    TEXT("Select all fighting units"),    TEXT("Selection"), FGenKeyChord(EKeys::Q) },
		{ EGenAction::SelectAllAircraft, TEXT("SelectAllAircraft"), TEXT("Select all aircraft"),          TEXT("Selection"), FGenKeyChord(EKeys::W) },
		{ EGenAction::Group0,            TEXT("Group0"),            TEXT("Group 0"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Zero) },
		{ EGenAction::Group1,            TEXT("Group1"),            TEXT("Group 1"),                      TEXT("Groups"),    FGenKeyChord(EKeys::One) },
		{ EGenAction::Group2,            TEXT("Group2"),            TEXT("Group 2"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Two) },
		{ EGenAction::Group3,            TEXT("Group3"),            TEXT("Group 3"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Three) },
		{ EGenAction::Group4,            TEXT("Group4"),            TEXT("Group 4"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Four) },
		{ EGenAction::Group5,            TEXT("Group5"),            TEXT("Group 5"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Five) },
		{ EGenAction::Group6,            TEXT("Group6"),            TEXT("Group 6"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Six) },
		{ EGenAction::Group7,            TEXT("Group7"),            TEXT("Group 7"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Seven) },
		{ EGenAction::Group8,            TEXT("Group8"),            TEXT("Group 8"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Eight) },
		{ EGenAction::Group9,            TEXT("Group9"),            TEXT("Group 9"),                      TEXT("Groups"),    FGenKeyChord(EKeys::Nine) },
		{ EGenAction::ScrollLeft,        TEXT("ScrollLeft"),        TEXT("Scroll left"),                  TEXT("Camera"),    FGenKeyChord(EKeys::Left) },
		{ EGenAction::ScrollRight,       TEXT("ScrollRight"),       TEXT("Scroll right"),                 TEXT("Camera"),    FGenKeyChord(EKeys::Right) },
		{ EGenAction::ScrollUp,          TEXT("ScrollUp"),          TEXT("Scroll up"),                    TEXT("Camera"),    FGenKeyChord(EKeys::Up) },
		{ EGenAction::ScrollDown,        TEXT("ScrollDown"),        TEXT("Scroll down"),                  TEXT("Camera"),    FGenKeyChord(EKeys::Down) },
		{ EGenAction::CameraReset,       TEXT("CameraReset"),       TEXT("Reset the camera"),             TEXT("Camera"),    FGenKeyChord(EKeys::Home) },
		{ EGenAction::JumpToEvent,       TEXT("JumpToEvent"),       TEXT("Jump to the last radar event"), TEXT("Camera"),    FGenKeyChord(EKeys::SpaceBar) },
		{ EGenAction::Menu,              TEXT("Menu"),              TEXT("Game menu (also Esc)"),         TEXT("Interface"), FGenKeyChord(EKeys::F10) },
	};
	check(Actions.Num() == int32(EGenAction::Count));
	return Actions;
}

const FGenActionInfo& FGenInput::GetInfo(EGenAction Action)
{
	return GetActions()[int32(Action)];
}

FGenInput::FGenInput()
{
	ResetToDefaults();
}

FGenInput& FGenInput::Get()
{
	static FGenInput Input;
	return Input;
}

void FGenInput::ResetToDefaults()
{
	for (const FGenActionInfo& Info : GetActions())
	{
		Chords[int32(Info.Action)] = Info.Default;
	}
	Style = EGenHotkeyStyle::Classic;
	ButtonKeys.Reset();
	ButtonNames.Reset();
}

void FGenInput::Load(const FGenHotkeySettings& Settings)
{
	ResetToDefaults();
	Style = Settings.Style;
	for (const FString& Line : Settings.ButtonKeys)
	{
		FString Name, Value;
		FGenKeyChord Chord;
		if (Line.Split(TEXT("="), &Name, &Value) && !Name.TrimStartAndEnd().IsEmpty() && FGenKeyChord::Parse(Value, Chord))
		{
			SetButtonOverride(Name.TrimStartAndEnd(), &Chord);
		}
	}
	for (const FString& Line : Settings.Bindings)
	{
		FString Name, Value;
		if (!Line.Split(TEXT("="), &Name, &Value))
		{
			continue;
		}
		const FGenActionInfo* Info = GetActions().FindByPredicate([&Name](const FGenActionInfo& I) { return Name.Equals(I.Name, ESearchCase::IgnoreCase); });
		FGenKeyChord Chord;
		if (Info && FGenKeyChord::Parse(Value, Chord))
		{
			Chords[int32(Info->Action)] = Chord;
		}
	}
}

void FGenInput::Save(FGenHotkeySettings& Settings) const
{
	Settings.Style = Style;
	Settings.ButtonKeys.Reset();
	for (const TPair<FString, FGenKeyChord>& It : ButtonKeys)
	{
		Settings.ButtonKeys.Add(FString::Printf(TEXT("%s=%s"), *ButtonNames.FindRef(It.Key), It.Value.IsValid() ? *It.Value.ToString() : TEXT("None")));
	}
	Settings.ButtonKeys.Sort();
	Settings.Bindings.Reset();
	for (const FGenActionInfo& Info : GetActions())
	{
		const FGenKeyChord& Chord = Chords[int32(Info.Action)];
		if (Chord != Info.Default)
		{
			Settings.Bindings.Add(FString::Printf(TEXT("%s=%s"), Info.Name, Chord.IsValid() ? *Chord.ToString() : TEXT("None")));
		}
	}
}

EGenAction FGenInput::FindConflict(EGenAction Action, const FGenKeyChord& Chord) const
{
	if (!Chord.IsValid())
	{
		return EGenAction::Count;
	}
	for (int32 i = 0; i < int32(EGenAction::Count); ++i)
	{
		if (i != int32(Action) && Chords[i] == Chord)
		{
			return EGenAction(i);
		}
	}
	return EGenAction::Count;
}

// ---- the command bar's buttons ------------------------------------------------------------------

FGenKeyChord FGenInput::GetClassicKey(const FString& Label)
{
	// HotKeyManager::searchHotKey: the character after the '&' of the button's text.
	const TCHAR Letter = FGenCommandData::Get().GetHotkeyLetter(Label);
	if (Letter >= TEXT('A') && Letter <= TEXT('Z'))
	{
		return FGenKeyChord(FKey(FName(FString::Chr(Letter))));
	}
	static const FKey Digits[10] = { EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
	if (Letter >= TEXT('0') && Letter <= TEXT('9'))
	{
		return FGenKeyChord(Digits[Letter - TEXT('0')]);
	}
	return FGenKeyChord();
}

FGenKeyChord FGenInput::GetGridKey(int32 Row, int32 Col)
{
	static const TCHAR* const Rows[3] = { TEXT("QWERTYUIOP"), TEXT("ASDFGHJKL"), TEXT("ZXCVBNM") };
	if (Row < 0 || Row >= 3 || Col < 0 || Col >= FCString::Strlen(Rows[Row]))
	{
		return FGenKeyChord();
	}
	return FGenKeyChord(FKey(FName(FString::Chr(Rows[Row][Col]))));
}

FGenKeyChord FGenInput::GetButtonKey(const FString& Name, const FString& Label, int32 Row, int32 Col) const
{
	if (Style == EGenHotkeyStyle::Grid && Row >= 0)
	{
		return GetGridKey(Row, Col);
	}
	if (Style == EGenHotkeyStyle::Custom)
	{
		if (const FGenKeyChord* Own = FindButtonOverride(Name))
		{
			return *Own;
		}
	}
	return GetClassicKey(Label);
}

void FGenInput::SetButtonOverride(const FString& Name, const FGenKeyChord* Chord)
{
	const FString Key = Name.ToLower();
	if (Chord)
	{
		ButtonKeys.Add(Key, *Chord);
		ButtonNames.Add(Key, Name);
	}
	else
	{
		ButtonKeys.Remove(Key);
		ButtonNames.Remove(Key);
	}
}

// ---- this frame's keys ----------------------------------------------------------------------------

bool FGenInput::CtrlDown(const APlayerController* PC)
{
	return PC && (PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::RightControl));
}

bool FGenInput::ShiftDown(const APlayerController* PC)
{
	return PC && (PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift));
}

bool FGenInput::AltDown(const APlayerController* PC)
{
	return PC && (PC->IsInputKeyDown(EKeys::LeftAlt) || PC->IsInputKeyDown(EKeys::RightAlt));
}

bool FGenInput::ModifiersHeld(const APlayerController* PC, const FGenKeyChord& Chord) const
{
	return (!Chord.bCtrl || CtrlDown(PC)) && (!Chord.bShift || ShiftDown(PC)) && (!Chord.bAlt || AltDown(PC));
}

bool FGenInput::JustPressed(const APlayerController* PC, EGenAction Action) const
{
	const FGenKeyChord& Chord = Chords[int32(Action)];
	if (!PC || !Chord.IsValid() || IsConsumed(Chord.Key) || !PC->WasInputKeyJustPressed(Chord.Key) || !ModifiersHeld(PC, Chord))
	{
		return false;
	}
	// Another command on the same key that asks for more of what is held has it.
	const int32 Mine = int32(Chord.bCtrl) + int32(Chord.bShift) + int32(Chord.bAlt);
	for (int32 i = 0; i < int32(EGenAction::Count); ++i)
	{
		const FGenKeyChord& Other = Chords[i];
		if (i != int32(Action) && Other.Key == Chord.Key && int32(Other.bCtrl) + int32(Other.bShift) + int32(Other.bAlt) > Mine && ModifiersHeld(PC, Other))
		{
			return false;
		}
	}
	return true;
}

bool FGenInput::IsDown(const APlayerController* PC, EGenAction Action) const
{
	const FGenKeyChord& Chord = Chords[int32(Action)];
	return PC && Chord.IsValid() && PC->IsInputKeyDown(Chord.Key) && ModifiersHeld(PC, Chord);
}

void FGenInput::Consume(const FKey& Key)
{
	if (ConsumedFrame != GFrameCounter)
	{
		ConsumedFrame = GFrameCounter;
		Consumed.Reset();
	}
	Consumed.AddUnique(Key);
}

bool FGenInput::IsConsumed(const FKey& Key) const
{
	return ConsumedFrame == GFrameCounter && Consumed.Contains(Key);
}
