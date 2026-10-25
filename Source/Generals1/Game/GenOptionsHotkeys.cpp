// The options' hotkeys page: the command bar's key style, the key of every command (GenInput), and - in the Custom
// style - the player's own keys for the bar's buttons.
//
// The game has some fifteen hundred buttons, so the page shows those of one army at a time, grouped by what they do
// (buildings, units, upgrades, powers, orders, promotions); in a game it starts with the buttons of what is selected,
// in the order its bar has them. A button's own key is the button's wherever it appears (Command_Stop's is every
// unit's stop). In the Grid style the list gives way to a picture of the bar's cells and their keys.
//
// Ids for test automation: see GenOptions.cpp; this page's own are
//   Hotkeys.Style=<Classic|Grid|Custom>         the command bar's key style
//   Hotkeys.Key.<Action>                        a command's key button (GenInput's names: Hotkeys.Key.Stop); then "Key=Ctrl+F"
//   Hotkeys.Button.<CommandButton>              a bar button's key button (Custom style); then "Key=B"
//   Hotkeys.Show=<side>                         whose buttons the list shows ("Hotkeys.Show=China")
//   Hotkeys.Swap, Hotkeys.NoSwap                the answer when the key is another's already
//   Hotkeys.Commands=Top|Bottom, Hotkeys.Buttons=Top|Bottom    the lists' scrolling
// While a key button waits for a key: Esc keeps the old one, Backspace leaves it without; Esc and the mouse buttons
// can't be keys.

#include "Game/GenHUD.h"
#include "Game/GenControlBar.h"
#include "Game/GenInput.h"
#include "Game/GenPlayerController.h"
#include "Game/GenSettings.h"
#include "Game/GenShellData.h"
#include "Game/GenShellStyle.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Generals1.h"

using namespace GenShellStyle;

namespace
{
	const TCHAR* const ConflictId = TEXT("Hotkeys.Conflict"); // (Options.CaptureId while the question is up: Esc is the page's)
	const TCHAR* const KeyPrefix = TEXT("Hotkeys.Key.");
	const TCHAR* const ButtonPrefix = TEXT("Hotkeys.Button.");

	/** A line of the buttons' list: a heading, or a button. */
	struct FButtonRow
	{
		FString Heading;
		const FGenCommandButton* Button = nullptr;
		FString Text;                                  // its name as the bar's tooltip has it
	};

	/** The bars (command sets) the buttons of a list are on, for the keys that clash: bBar - a command window's
	 *  (whose cells have the Grid style's keys), not the promotions' or the power shortcuts'. */
	struct FListSet
	{
		const FGenCommandSet* Set = nullptr;
		bool bBar = true;
	};

	/** An army's buttons. */
	struct FArmyButtons
	{
		TArray<FButtonRow> Rows;
		TArray<FListSet> Sets;
	};

	/** The page's own state beside the draft (Options.Hotkeys). */
	struct FHotkeysPage
	{
		float ButtonScroll = 0.f;
		FString Show;                                  // the side whose buttons are listed; "": the selection's, if any
		// A key that is another's already: the question in the strip at the bottom.
		bool bAsking = false;
		bool bButton = false;                          // ... about a bar button's key (else a command's)
		EGenAction Action = EGenAction::Count;
		EGenAction Other = EGenAction::Count;
		const FGenCommandButton* Button = nullptr;
		const FGenCommandButton* OtherButton = nullptr;
		FGenKeyChord Chord;
		FString Question;
		FString Note;                                  // said under the lists while a key button waits
		TMap<FString, FArmyButtons> Armies;            // by PlayerTemplate.ini's Side, made when first shown
	};
	FHotkeysPage Page;

	FString StyleName(EGenHotkeyStyle Style)
	{
		return Style == EGenHotkeyStyle::Grid ? TEXT("Grid") : Style == EGenHotkeyStyle::Custom ? TEXT("Custom") : TEXT("Classic");
	}

	FString ButtonText(const FGenCommandButton& B)
	{
		FString T = B.TextLabel.IsEmpty() ? B.Name : FGenCommandData::Get().GetText(B.TextLabel);
		T.ReplaceInline(TEXT("\\n"), TEXT(" "));
		T.ReplaceInline(TEXT("\n"), TEXT(" "));
		while (T.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
		{
		}
		return T.TrimStartAndEnd();
	}

	/** The group of the army list a button goes in (GroupNames). */
	int32 ButtonGroup(const FGenCommandButton& B)
	{
		switch (B.Command)
		{
		case EGenCommand::DozerConstruct: return 0;
		case EGenCommand::UnitBuild: return 1;
		case EGenCommand::PlayerUpgrade:
		case EGenCommand::ObjectUpgrade: return 2;
		case EGenCommand::Stop:
		case EGenCommand::AttackMove:
		case EGenCommand::Guard:
		case EGenCommand::GuardWithoutPursuit:
		case EGenCommand::GuardFlyingOnly:
		case EGenCommand::Sell:
		case EGenCommand::Evacuate:
		case EGenCommand::ExitContainer:
		case EGenCommand::SetRallyPoint:
		case EGenCommand::DozerConstructCancel: return 4;
		case EGenCommand::PurchaseScience: return 5;
		default: return 3;
		}
	}
	const TCHAR* const GroupNames[] = { TEXT("Buildings"), TEXT("Units"), TEXT("Upgrades"), TEXT("Powers and abilities"), TEXT("Orders"), TEXT("Promotions") };

	/** The buttons on the bars of that army's objects, its promotions and its power shortcuts. */
	const FArmyButtons& GetArmy(const FString& Side, const FGenPlayerTemplate* Faction)
	{
		if (const FArmyButtons* Found = Page.Armies.Find(Side))
		{
			return *Found;
		}
		FArmyButtons& Army = Page.Armies.Add(Side);
		const FGenCommandData& CD = FGenCommandData::Get();
		TSet<const FGenCommandSet*> Bars;
		for (const TPair<FString, TSharedPtr<FGenThingTemplate>>& It : FGenGameData::Get().GetTemplates())
		{
			const FGenThingTemplate* T = It.Value.Get();
			// (Buildable 1: cut content, never offered.)
			const FGenCommandSet* Set = (T && T->Side == Side && T->Buildable != 1) ? CD.FindSet(T->CommandSet) : nullptr;
			if (Set && !Bars.Contains(Set))
			{
				Bars.Add(Set);
				Army.Sets.Add({ Set, true });
			}
		}
		if (Faction)
		{
			for (const FString& Name : { Faction->PurchaseScienceSets[0], Faction->PurchaseScienceSets[1], Faction->PurchaseScienceSets[2], Faction->SpecialPowerShortcutSet })
			{
				if (const FGenCommandSet* Set = CD.FindSet(Name))
				{
					Army.Sets.Add({ Set, false });
				}
			}
		}
		TSet<const FGenCommandButton*> Seen;
		TArray<FButtonRow> Buttons;
		for (const FListSet& S : Army.Sets)
		{
			for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
			{
				const FGenCommandButton* B = CD.FindButton(S.Set->Slots[i]);
				if (!B || B->bScriptOnly || B->TextLabel.IsEmpty() || Seen.Contains(B))
				{
					continue;
				}
				Seen.Add(B);
				FButtonRow& Row = Buttons.AddDefaulted_GetRef();
				Row.Button = B;
				Row.Text = ButtonText(*B);
			}
		}
		Buttons.Sort([](const FButtonRow& A, const FButtonRow& B)
		{
			const int32 GA = ButtonGroup(*A.Button), GB = ButtonGroup(*B.Button);
			return GA != GB ? GA < GB : A.Text < B.Text;
		});
		int32 Group = INDEX_NONE;
		for (FButtonRow& Row : Buttons)
		{
			if (ButtonGroup(*Row.Button) != Group)
			{
				Group = ButtonGroup(*Row.Button);
				Army.Rows.AddDefaulted_GetRef().Heading = GroupNames[Group];
			}
			Army.Rows.Add(MoveTemp(Row));
		}
		return Army;
	}

	/** A button's key in the draft, on a bar's slot (the Grid style's cell). */
	FGenKeyChord KeyOnSlot(const FGenInput& Draft, const FGenCommandButton& B, int32 Slot, bool bBar)
	{
		int32 Row = INDEX_NONE, Col = INDEX_NONE;
		if (bBar && Slot < 14)
		{
			FGenControlBar::GetGridCell(Slot, Row, Col);
		}
		return Draft.GetButtonKey(B.Name, B.TextLabel, Row, Col);
	}

	/** The buttons of these bars whose key another button on the same bar has too, and that other's name. */
	void FindClashes(const FGenInput& Draft, const TArray<FListSet>& Sets, TMap<const FGenCommandButton*, FString>& Out)
	{
		const FGenCommandData& CD = FGenCommandData::Get();
		for (const FListSet& S : Sets)
		{
			TArray<TPair<FGenKeyChord, const FGenCommandButton*>, TInlineAllocator<FGenCommandSet::NumSlots>> Keys;
			for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
			{
				const FGenCommandButton* B = CD.FindButton(S.Set->Slots[i]);
				const FGenKeyChord Key = B ? KeyOnSlot(Draft, *B, i, S.bBar) : FGenKeyChord();
				if (!Key.IsValid())
				{
					continue;
				}
				for (const TPair<FGenKeyChord, const FGenCommandButton*>& K : Keys)
				{
					if (K.Key == Key && K.Value != B)
					{
						Out.FindOrAdd(B, ButtonText(*K.Value));
						Out.FindOrAdd(K.Value, ButtonText(*B));
					}
				}
				Keys.Emplace(Key, B);
			}
		}
	}

	/** Another button that has that key on one of the bars this one is on (every army's: the key is the button's everywhere). */
	const FGenCommandButton* FindButtonClash(const FGenInput& Draft, const FGenCommandButton& Button, const FGenKeyChord& Chord)
	{
		if (!Chord.IsValid())
		{
			return nullptr;
		}
		const FGenCommandData& CD = FGenCommandData::Get();
		for (const TPair<FString, FGenCommandSet>& It : CD.GetSets())
		{
			const FGenCommandSet& Set = It.Value;
			bool bOn = false;
			for (int32 i = 0; i < FGenCommandSet::NumSlots && !bOn; ++i)
			{
				bOn = Set.Slots[i].Equals(Button.Name, ESearchCase::IgnoreCase);
			}
			for (int32 i = 0; bOn && i < FGenCommandSet::NumSlots; ++i)
			{
				const FGenCommandButton* B = CD.FindButton(Set.Slots[i]);
				if (B && B != &Button && Draft.GetButtonKey(B->Name, B->TextLabel, INDEX_NONE, INDEX_NONE) == Chord)
				{
					return B;
				}
			}
		}
		return nullptr;
	}

	/** The player's key for a button - none kept when it is the button's own letter. */
	void SetButtonKey(FGenInput& Draft, const FGenCommandButton& B, const FGenKeyChord& Chord)
	{
		Draft.SetButtonOverride(B.Name, Chord == FGenInput::GetClassicKey(B.TextLabel) ? nullptr : &Chord);
	}

	/**
	 * The key pressed for the key button that waited (CaptureId): taken, or - when it is another's already - the
	 * question asked (CaptureId is then ConflictId). True when the draft changed.
	 */
	bool TakeKey(FGenInput& Draft, FString& CaptureId, const FGenKeyChord& Chord)
	{
		if (CaptureId.StartsWith(KeyPrefix))
		{
			const FString Name = CaptureId.Mid(FCString::Strlen(KeyPrefix));
			const FGenActionInfo* Info = FGenInput::GetActions().FindByPredicate([&Name](const FGenActionInfo& I) { return Name.Equals(I.Name, ESearchCase::IgnoreCase); });
			if (!Info)
			{
				CaptureId.Reset();
				return false;
			}
			const EGenAction Other = Draft.FindConflict(Info->Action, Chord);
			if (Other != EGenAction::Count)
			{
				Page.bAsking = true;
				Page.bButton = false;
				Page.Action = Info->Action;
				Page.Other = Other;
				Page.Chord = Chord;
				Page.Question = FString::Printf(TEXT("%s is already the key of \"%s\". Swap: \"%s\" gets %s."), *Chord.GetDisplayText(),
					FGenInput::GetInfo(Other).Label, FGenInput::GetInfo(Other).Label, *Draft.GetChord(Info->Action).GetDisplayText());
				CaptureId = ConflictId;
				return false;
			}
			Draft.SetChord(Info->Action, Chord);
			CaptureId.Reset();
			return true;
		}
		if (CaptureId.StartsWith(ButtonPrefix))
		{
			const FGenCommandButton* Button = FGenCommandData::Get().FindButton(CaptureId.Mid(FCString::Strlen(ButtonPrefix)));
			if (!Button)
			{
				CaptureId.Reset();
				return false;
			}
			if (const FGenCommandButton* Other = FindButtonClash(Draft, *Button, Chord))
			{
				Page.bAsking = true;
				Page.bButton = true;
				Page.Button = Button;
				Page.OtherButton = Other;
				Page.Chord = Chord;
				const FGenKeyChord Old = Draft.GetButtonKey(Button->Name, Button->TextLabel, INDEX_NONE, INDEX_NONE);
				Page.Question = FString::Printf(TEXT("%s is already \"%s\" on the same bar. Swap: \"%s\" gets %s."), *Chord.GetDisplayText(),
					*ButtonText(*Other), *ButtonText(*Other), Old.IsValid() ? *Old.GetDisplayText() : TEXT("no key"));
				CaptureId = ConflictId;
				return false;
			}
			SetButtonKey(Draft, *Button, Chord);
			CaptureId.Reset();
			return true;
		}
		return false;
	}

	/** The question's answer: swap - the other gets this one's old key - or leave everything as it was. */
	bool Answer(FGenInput& Draft, FString& CaptureId, bool bSwap)
	{
		bool bChanged = false;
		if (bSwap && !Page.bButton && Page.Action != EGenAction::Count && Page.Other != EGenAction::Count)
		{
			const FGenKeyChord Old = Draft.GetChord(Page.Action);
			Draft.SetChord(Page.Action, Page.Chord);
			Draft.SetChord(Page.Other, Old);
			bChanged = true;
		}
		else if (bSwap && Page.bButton && Page.Button && Page.OtherButton)
		{
			const FGenKeyChord Old = Draft.GetButtonKey(Page.Button->Name, Page.Button->TextLabel, INDEX_NONE, INDEX_NONE);
			SetButtonKey(Draft, *Page.Button, Page.Chord);
			SetButtonKey(Draft, *Page.OtherButton, Old);
			bChanged = true;
		}
		Page.bAsking = false;
		CaptureId.Reset();
		return bChanged;
	}
}

void AGenHUD::OptionsLoadHotkeys()
{
	if (const UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Options.Hotkeys = Settings->Hotkeys;
	}
	Page.ButtonScroll = 0.f;
	Page.Show.Reset();
	Page.bAsking = false;
	Page.Note.Reset();
}

void AGenHUD::DrawOptionsHotkeys(const FBox2D& Body)
{
	const float X0 = float(Body.Min.X), Y0 = float(Body.Min.Y);
	auto At = [this, X0, Y0](float X, float Y, float W = 0.f, float H = 0.f) { return ShellRect(X0 + X, Y0 + Y, W, H); };
	auto Text = [this, &At](const FString& S, float X, float Y, const FLinearColor& Color, float Size, int32 Align = 0, bool bBold = false)
	{
		const FBox2D P = At(X, Y);
		ShellText(S, P.Min.X, P.Min.Y, Color, Size, Align, bBold);
	};
	// A line cut short with "..." to fit a width (design units).
	auto Fit = [this](FString S, float Size, float MaxWidth)
	{
		const float Max = MaxWidth * Shell.Scale;
		if (ShellMeasure(S, Size).X <= Max)
		{
			return S;
		}
		while (S.Len() > 1 && ShellMeasure(S + TEXT("..."), Size).X > Max)
		{
			S.LeftChopInline(1);
		}
		return S.TrimEnd() + TEXT("...");
	};
	// A key shown where it can't be changed here.
	auto KeyBox = [this](const FBox2D& R, const FString& S, const FLinearColor& Color)
	{
		const FVector2D Size = R.GetSize();
		DrawRect(FLinearColor(0.02f, 0.04f, 0.09f, 0.8f), R.Min.X, R.Min.Y, Size.X, Size.Y);
		const float TextHeight = float(ShellMeasure(S, 17.f, true).Y);
		ShellText(S, R.Min.X + Size.X * 0.5f, R.Min.Y + (Size.Y - TextHeight) * 0.5f, Color, 17.f, 1, true);
	};

	FGenInput Draft;
	Draft.Load(Options.Hotkeys);
	bool bChanged = false;
	const AGenPlayerController* PC = Cast<AGenPlayerController>(GetOwningPlayerController());
	const bool bRealKeys = PC && PC->IsGenInputEnabled();

	// The question put away from outside (another tab, the frame's Defaults).
	if (Page.bAsking && Options.CaptureId != ConflictId)
	{
		Page.bAsking = false;
	}
	// The key for the key button waiting for one.
	const bool bWaiting = Options.CaptureId.StartsWith(KeyPrefix) || Options.CaptureId.StartsWith(ButtonPrefix);
	if (bWaiting)
	{
		FGenKeyChord Got;
		bool bCancelled = false;
		if (ShellCaptureKey(Got, bCancelled))
		{
			if (Got.Key.IsMouseButton())
			{
				Page.Note = TEXT("The mouse buttons can't be keys.");
			}
			else
			{
				if (Got.Key == EKeys::BackSpace)
				{
					Got = FGenKeyChord(); // (no key)
				}
				Page.Note.Reset();
				bChanged |= TakeKey(Draft, Options.CaptureId, Got);
			}
		}
		else if (bCancelled)
		{
			Options.CaptureId.Reset();
			Page.Note.Reset();
		}
	}
	const bool bWasBlocked = Shell.bBlocked;
	Shell.bBlocked = bWasBlocked || Page.bAsking; // (the question has the page until it is answered)

	// ---- the style ----
	Text(TEXT("Command bar keys"), 0, 8, ShellTextColor, 18.f);
	{
		static const TArray<FString> Styles = { TEXT("Classic"), TEXT("Grid"), TEXT("Custom") };
		static const TArray<FLinearColor> NoColors;
		int32 Picked = INDEX_NONE;
		if (ShellDropdown(TEXT("Hotkeys.Style"), At(175, 0, 200, 38), Styles, NoColors, int32(Draft.GetStyle()), Picked))
		{
			Draft.SetStyle(EGenHotkeyStyle(Picked));
			bChanged = true;
			if (Options.CaptureId.StartsWith(ButtonPrefix))
			{
				Options.CaptureId.Reset();
			}
		}
		const TCHAR* const About[3] = {
			TEXT("Each button's own letter, as in the original: the one marked in its name."),
			TEXT("By place on the bar: the same keys in the same cells for every army."),
			TEXT("Each button's own letter, or the key you give it below."),
		};
		Text(About[FMath::Clamp(int32(Draft.GetStyle()), 0, 2)], 395, 10, ShellDim, 16.f);
	}

	// ---- the commands ----
	static constexpr float ListTop = 92.f, ListBottom = 470.f, HeadingH = 32.f, RowH = 36.f;
	Text(TEXT("COMMANDS"), 0, 58, ShellGold, 16.f, 0, true);
	{
		const TArray<FGenActionInfo>& Actions = FGenInput::GetActions();
		float ContentH = 0.f;
		const TCHAR* Last = nullptr;
		for (const FGenActionInfo& Info : Actions)
		{
			ContentH += (!Last || FCString::Strcmp(Last, Info.Category) != 0) ? HeadingH + RowH : RowH;
			Last = Info.Category;
		}
		float& Scroll = Options.Scroll[OptionsHotkeys];
		ShellScrollArea(TEXT("Hotkeys.Commands"), At(0, ListTop, 560, ListBottom - ListTop), ContentH, Scroll);
		float Y = ListTop - Scroll;
		Last = nullptr;
		auto Visible = [](float Top, float H) { return Top >= ListTop - 0.5f && Top + H <= ListBottom + 0.5f; };
		for (const FGenActionInfo& Info : Actions)
		{
			if (!Last || FCString::Strcmp(Last, Info.Category) != 0)
			{
				if (Visible(Y, HeadingH))
				{
					Text(Info.Category, 4, Y + 8, ShellGold, 15.f, 0, true);
					const FBox2D Line = At(4, Y + HeadingH - 3, 536, 1);
					DrawRect(FLinearColor(ShellEdge.R, ShellEdge.G, ShellEdge.B, 0.45f), Line.Min.X, Line.Min.Y, Line.GetSize().X, FMath::Max(1.0, Line.GetSize().Y));
				}
				Y += HeadingH;
				Last = Info.Category;
			}
			if (Visible(Y, RowH))
			{
				Text(Info.Label, 14, Y + 8, ShellTextColor, 17.f);
				const FString Id = FString(KeyPrefix) + Info.Name;
				if (ShellKeyButton(Id, At(330, Y + 3, 210, 30), Draft.GetChord(Info.Action).GetDisplayText(), Options.CaptureId == Id))
				{
					Options.CaptureId = Options.CaptureId == Id ? FString() : Id;
					Page.Note.Reset();
				}
			}
			Y += RowH;
		}
	}

	// ---- the command bar's buttons ----
	Text(TEXT("COMMAND BAR BUTTONS"), 590, 58, ShellGold, 16.f, 0, true);
	const FGenCommandData& CD = FGenCommandData::Get();
	// In a game: what is selected (the bar's subject).
	const FGenCommandSet* SelSet = nullptr;
	FString SelName;
	int32 LocalSide = INDEX_NONE;
	if (const UGenGameLogic* Logic = UGenGameLogic::Get(this); Logic && Logic->IsRunning() && PC)
	{
		if (const FGenObject* O = Logic->FindObject(PC->GetControlBar().Subject))
		{
			SelSet = CD.FindSet(O->GetCommandSet());
			SelName = CD.GetText(O->GetTemplate()->DisplayName);
			if (SelName.IsEmpty() || SelName.Contains(TEXT(":")))
			{
				SelName = O->GetTemplate()->Name;
			}
		}
		const TArray<FGenPlayer>& Players = Logic->GetPlayers();
		if (Players.IsValidIndex(Logic->GetLocalPlayer()) && Players[Logic->GetLocalPlayer()].Template)
		{
			LocalSide = FGenShellData::Get().FindSide(Players[Logic->GetLocalPlayer()].Template->Name);
		}
	}
	const TArray<FGenSideInfo>& Sides = FGenShellData::Get().GetSides();

	if (Draft.GetStyle() == EGenHotkeyStyle::Grid)
	{
		// The bar's cells and their keys, with the selection's buttons in them.
		int32 NumRows = 0, NumCols = 0;
		for (int32 Slot = 0; Slot < 14; ++Slot)
		{
			int32 Row, Col;
			if (FGenControlBar::GetGridCell(Slot, Row, Col))
			{
				NumRows = FMath::Max(NumRows, Row + 1);
				NumCols = FMath::Max(NumCols, Col + 1);
			}
		}
		constexpr float CellW = 66.f, CellH = 58.f, Gap = 9.f;
		const float GridX = 590.f + (550.f - (NumCols * CellW + (NumCols - 1) * Gap)) * 0.5f, GridY = 104.f;
		for (int32 Slot = 0; Slot < 14; ++Slot)
		{
			int32 Row, Col;
			if (!FGenControlBar::GetGridCell(Slot, Row, Col))
			{
				continue;
			}
			const FBox2D Cell = At(GridX + Col * (CellW + Gap), GridY + Row * (CellH + Gap), CellW, CellH);
			const FVector2D Size = Cell.GetSize();
			DrawRect(ShellFill, Cell.Min.X, Cell.Min.Y, Size.X, Size.Y);
			const FGenCommandButton* B = SelSet ? CD.FindButton(SelSet->Slots[Slot]) : nullptr;
			if (B)
			{
				const float Inset = 3.f * Shell.Scale;
				DrawMappedImage(B->ButtonImage, FBox2D(Cell.Min + FVector2D(Inset, Inset), Cell.Max - FVector2D(Inset, Inset)));
				ShellTooltip(Cell, ButtonText(*B));
			}
			const float T = FMath::Max(1.f, 1.f * Shell.Scale);
			DrawRect(ShellEdge, Cell.Min.X, Cell.Min.Y, Size.X, T);
			DrawRect(ShellEdge, Cell.Min.X, Cell.Max.Y - T, Size.X, T);
			DrawRect(ShellEdge, Cell.Min.X, Cell.Min.Y, T, Size.Y);
			DrawRect(ShellEdge, Cell.Max.X - T, Cell.Min.Y, T, Size.Y);
			const FString Key = FGenInput::GetGridKey(Row, Col).GetShortText();
			const FVector2D KeySize = ShellMeasure(Key, 17.f, true);
			DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.6f), Cell.Min.X + T, Cell.Min.Y + T, KeySize.X + 8.f * Shell.Scale, KeySize.Y);
			ShellText(Key, Cell.Min.X + T + 4.f * Shell.Scale, Cell.Min.Y + T, ShellGold, 17.f, 0, true);
		}
		const float TextY = GridY + NumRows * (CellH + Gap) + 14.f;
		Text(SelSet ? FString::Printf(TEXT("Shown: the bar of the %s."), *SelName) : FString(TEXT("In a game, the bar of what is selected is shown here.")), 600, TextY, ShellTextColor, 16.f);
		Text(TEXT("The same keys in the same places for every army and every"), 600, TextY + 34, ShellDim, 16.f);
		Text(TEXT("unit. The other buttons (the promotions, the power shortcuts,"), 600, TextY + 58, ShellDim, 16.f);
		Text(TEXT("cancelling a building) keep their own letters."), 600, TextY + 82, ShellDim, 16.f);
		Text(TEXT("While a button is on Q, W, E, A, S or G, the key is the button's:"), 600, TextY + 116, ShellDim, 16.f);
		Text(TEXT("give those commands other keys on the left to keep both."), 600, TextY + 140, ShellDim, 16.f);
	}
	else
	{
		// Whose buttons: the selection's, or an army's.
		TArray<FString> Items;
		TArray<int32> ItemSides; // INDEX_NONE: the selection
		if (SelSet)
		{
			Items.Add(FString::Printf(TEXT("Selected: %s"), *SelName));
			ItemSides.Add(INDEX_NONE);
		}
		for (int32 i = 0; i < Sides.Num(); ++i)
		{
			Items.Add(Sides[i].Label);
			ItemSides.Add(i);
		}
		int32 Current = Items.IndexOfByKey(Page.Show);
		if (Current == INDEX_NONE)
		{
			Current = SelSet ? 0 : FMath::Max(0, ItemSides.IndexOfByKey(LocalSide));
		}
		static const TArray<FLinearColor> NoColors;
		int32 Picked = INDEX_NONE;
		if (ShellDropdown(TEXT("Hotkeys.Show"), At(880, 50, 260, 34), Items, NoColors, Current, Picked))
		{
			Current = Picked;
			Page.ButtonScroll = 0.f;
		}
		if (Items.IsValidIndex(Current))
		{
			Page.Show = Items[Current];
		}
		Text(TEXT("Show"), 868, 56, ShellDim, 16.f, 2);

		// The rows: the selection's bar in its order, or the army's buttons in groups.
		TArray<FButtonRow> SelRows;
		TArray<FListSet> SelSets;
		const TArray<FButtonRow>* Rows = &SelRows;
		const TArray<FListSet>* Sets = &SelSets;
		if (ItemSides.IsValidIndex(Current) && ItemSides[Current] == INDEX_NONE)
		{
			SelRows.AddDefaulted_GetRef().Heading = SelName;
			for (int32 Slot = 0; Slot < 14; ++Slot)
			{
				const FGenCommandButton* B = CD.FindButton(SelSet->Slots[Slot]);
				if (B && !B->bScriptOnly)
				{
					FButtonRow& Row = SelRows.AddDefaulted_GetRef();
					Row.Button = B;
					Row.Text = ButtonText(*B);
				}
			}
			SelSets.Add({ SelSet, true });
		}
		else if (ItemSides.IsValidIndex(Current) && Sides.IsValidIndex(ItemSides[Current]))
		{
			const FGenPlayerTemplate* Faction = FGenGameData::Get().FindPlayerTemplate(Sides[ItemSides[Current]].Template);
			const FArmyButtons& Army = GetArmy(Faction ? Faction->Side : Sides[ItemSides[Current]].Template, Faction);
			Rows = &Army.Rows;
			Sets = &Army.Sets;
		}
		TMap<const FGenCommandButton*, FString> Clashes;
		FindClashes(Draft, *Sets, Clashes);

		float ContentH = 0.f;
		for (const FButtonRow& Row : *Rows)
		{
			ContentH += Row.Button ? RowH : HeadingH;
		}
		ShellScrollArea(TEXT("Hotkeys.Buttons"), At(590, ListTop, 550, ListBottom - ListTop), ContentH, Page.ButtonScroll);
		const bool bCustom = Draft.GetStyle() == EGenHotkeyStyle::Custom;
		float Y = ListTop - Page.ButtonScroll;
		for (const FButtonRow& Row : *Rows)
		{
			const float H = Row.Button ? RowH : HeadingH;
			if (Y < ListTop - 0.5f || Y + H > ListBottom + 0.5f)
			{
				Y += H;
				continue;
			}
			if (!Row.Button)
			{
				Text(Fit(Row.Heading, 15.f, 520.f), 594, Y + 8, ShellGold, 15.f, 0, true);
				const FBox2D Line = At(594, Y + HeadingH - 3, 526, 1);
				DrawRect(FLinearColor(ShellEdge.R, ShellEdge.G, ShellEdge.B, 0.45f), Line.Min.X, Line.Min.Y, Line.GetSize().X, FMath::Max(1.0, Line.GetSize().Y));
				Y += H;
				continue;
			}
			const FGenCommandButton& B = *Row.Button;
			DrawMappedImage(B.ButtonImage, At(598, Y + 3, 34, 30));
			const FString* Clash = Clashes.Find(&B);
			Text(Fit(Row.Text, 16.f, 222.f), 642, Y + 9, Clash ? ShellRed : ShellTextColor, 16.f);
			const FGenKeyChord Key = Draft.GetButtonKey(B.Name, B.TextLabel, INDEX_NONE, INDEX_NONE);
			const FString KeyText = Key.IsValid() ? Key.GetDisplayText() : FString(TEXT("-"));
			const FBox2D KeyRect = At(890, Y + 3, 230, 30);
			const FGenKeyChord* Own = bCustom ? Draft.FindButtonOverride(B.Name) : nullptr;
			if (bCustom)
			{
				const FString Id = FString(ButtonPrefix) + B.Name;
				if (ShellKeyButton(Id, KeyRect, KeyText, Options.CaptureId == Id))
				{
					Options.CaptureId = Options.CaptureId == Id ? FString() : Id;
					Page.Note.Reset();
				}
			}
			else
			{
				KeyBox(KeyRect, KeyText, Key.IsValid() ? ShellTextColor : ShellDim);
			}
			if (Own)
			{
				// Changed: a gold mark before the key.
				const FBox2D Mark = At(874, Y + 14, 8, 8);
				DrawRect(ShellGold, Mark.Min.X, Mark.Min.Y, Mark.GetSize().X, Mark.GetSize().Y);
			}
			FString Tip;
			if (Clash)
			{
				Tip = FString::Printf(TEXT("\"%s\" has the same key on a bar it shares with this one."), **Clash);
			}
			else if (Own)
			{
				const FGenKeyChord Orig = FGenInput::GetClassicKey(B.TextLabel);
				Tip = FString::Printf(TEXT("Your key (its own: %s)"), Orig.IsValid() ? *Orig.GetDisplayText() : TEXT("none"));
			}
			ShellTooltip(At(594, Y, 280, RowH), Tip);
			Y += H;
		}
		if (Rows->Num() == 0)
		{
			Text(TEXT("No buttons to show."), 600, ListTop + 10, ShellDim, 16.f);
		}
	}

	// A click anywhere else stops the waiting.
	if (bWaiting && Shell.bClick && (Options.CaptureId.StartsWith(KeyPrefix) || Options.CaptureId.StartsWith(ButtonPrefix)))
	{
		Options.CaptureId.Reset();
		Page.Note.Reset();
	}
	Shell.bBlocked = bWasBlocked;

	// ---- the strip at the bottom: the question, or what to press, or how the keys go together ----
	if (Page.bAsking)
	{
		const FBox2D Strip = At(0, 478, 1140, 44);
		ShellPanel(Strip, 0.97f);
		DrawRect(ShellGold, Strip.Min.X, Strip.Min.Y, FMath::Max(2.f, 3.f * Shell.Scale), Strip.GetSize().Y);
		Text(Fit(Page.Question, 17.f, 860.f), 16, 489, FLinearColor::White, 17.f);
		const bool bEsc = bRealKeys && PC->WasInputKeyJustPressed(EKeys::Escape);
		if (ShellButton(TEXT("Hotkeys.Swap"), At(890, 483, 118, 34), TEXT("SWAP")))
		{
			bChanged |= Answer(Draft, Options.CaptureId, true);
		}
		else if (ShellButton(TEXT("Hotkeys.NoSwap"), At(1016, 483, 118, 34), TEXT("CANCEL")) || bEsc)
		{
			bChanged |= Answer(Draft, Options.CaptureId, false);
		}
	}
	else if (Options.CaptureId.StartsWith(KeyPrefix) || Options.CaptureId.StartsWith(ButtonPrefix))
	{
		Text(TEXT("Press the new key, with Ctrl, Shift or Alt if you like.   Esc: keep the old one.   Backspace: no key."), 4, 490, ShellGold, 16.f);
		if (!Page.Note.IsEmpty())
		{
			Text(Page.Note, 1136, 490, ShellRed, 16.f, 2);
		}
	}
	else
	{
		Text(TEXT("While a button is shown, its key comes before a command's on the same key. Ctrl, Shift and Alt with a number are always the groups'."), 4, 490, ShellDim, 15.f);
	}

	if (bChanged)
	{
		Draft.Save(Options.Hotkeys);
	}
}

void AGenHUD::OptionsApplyHotkeys()
{
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->Hotkeys = Options.Hotkeys;
		FGenInput::Get().Load(Settings->Hotkeys);
		UE_LOG(LogGenerals, Log, TEXT("Options: hotkeys %s; commands changed: %s; buttons' own keys: %s"), *StyleName(Settings->Hotkeys.Style),
			Settings->Hotkeys.Bindings.Num() ? *FString::Join(Settings->Hotkeys.Bindings, TEXT(", ")) : TEXT("none"),
			Settings->Hotkeys.ButtonKeys.Num() ? *FString::Join(Settings->Hotkeys.ButtonKeys, TEXT(", ")) : TEXT("none"));
	}
	Page.bAsking = false;
}

void AGenHUD::OptionsDefaultsHotkeys()
{
	Options.Hotkeys = FGenHotkeySettings();
	Page.bAsking = false;
	if (Options.CaptureId.StartsWith(TEXT("Hotkeys.")))
	{
		Options.CaptureId.Reset();
	}
}

void AGenHUD::OptionsCancelHotkeys()
{
	Page.bAsking = false;
	Page.Note.Reset();
}
