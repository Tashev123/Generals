// The command bar's buttons by their keys (the original's hotkeys: the letter marked with '&' in each button's
// name in the game's text), or by their place on the bar, or the player's own (UGenUserSettings::Hotkeys).
//
// How they sit with the commands' keys (GenInput's stop, attack-move, the selection and group keys): as in the
// original (HotKeyTranslator comes before the CommandMap's translator), a button's key comes first while a button on
// that key is shown - with a Crusader selected A is its "Attack Move" button, with a barracks selected G is its
// Ranger. The key is then the bar's for that frame (FGenInput::Consume), so no command on it fires as well. A
// button's key counts only with exactly its own modifiers held (none, unless the player gave it some), so Ctrl, Shift
// and Alt with a number are always the groups' and Ctrl+F always the formation's. A greyed-out button still has its
// key: pressing it does what a click on it does (the adviser's "insufficient funds", a buzz).

#include "Game/GenPlayerController.h"
#include "Game/GenControlBar.h"
#include "Game/GenInput.h"
#include "Game/GenSettings.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameLogic.h"
#include "Generals1.h"

const FGenCommandButton* GenPressBarHotkey(AGenPlayerController* PC, const FGenKeyChord& Pressed)
{
	const FGenBarButton* Found = PC ? PC->GetControlBar().FindHotkey(Pressed) : nullptr;
	if (!Found)
	{
		return nullptr;
	}
	FGenInput::Get().Consume(Pressed.Key);
	const FGenBarButton Copy = *Found; // (the press may change the bar)
	PC->ExecuteBarButton(Copy);
	if (const UGenGameLogic* Logic = UGenGameLogic::Get(PC))
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [hotkey f%u] %s: %s%s"), Logic->GetFrame(), *Pressed.ToString(),
			Copy.Button ? *Copy.Button->Name : TEXT("?"), Copy.bEnabled ? TEXT("") : TEXT(" (greyed out)"));
	}
	return Copy.Button;
}

void AGenPlayerController::UpdateBarHotkeys()
{
	// Every key of a shown button that went down this frame, tried with the modifiers held now.
	const bool bCtrl = FGenInput::CtrlDown(this), bShift = FGenInput::ShiftDown(this), bAlt = FGenInput::AltDown(this);
	TArray<FKey, TInlineAllocator<16>> Tried;
	for (const TArray<FGenBarButton>* List : { &ControlBar.Sciences, &ControlBar.Buttons, &ControlBar.Shortcuts })
	{
		for (const FGenBarButton& B : *List)
		{
			const FKey Key = B.Hotkey.Key;
			if (!Key.IsValid() || Tried.Contains(Key))
			{
				continue;
			}
			Tried.Add(Key);
			if (WasInputKeyJustPressed(Key) && GenPressBarHotkey(this, FGenKeyChord(Key, bCtrl, bShift, bAlt)))
			{
				return; // (one button a frame: the press may have changed what the bar shows)
			}
		}
	}
}
