#include "Game/GenControlBar.h"
#include "Core/GenWnd.h"
#include "Game/GenControlBarScheme.h"
#include "Game/GenHudStyle.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

static TAutoConsoleVariable<float> CVarGenControlBarScale(
	TEXT("gen.ControlBarScale"), 0.8f,
	TEXT("Size of the control bar, the power rail and the promotions panel: 1 = the original's share of the screen's height (its 800 x 600 layouts scaled to the screen's height)."));

static TAutoConsoleVariable<float> CVarGenHudSpread(
	TEXT("gen.HudSpread"), 0.f,
	TEXT("Where the bar's sections stand: 0 = right beside the commands in the middle, 1 = the radar and the right panel in the screen's corners."));

namespace
{
	/** The side (FGenBarShape::ESide) a scheme belongs to (the generals' schemes wear their side's look). */
	int32 FindSide(const FGenControlBarScheme* Scheme)
	{
		if (Scheme)
		{
			for (const FGenControlBarImagePart& Part : Scheme->ImageParts)
			{
				if (Part.Image.Contains(TEXT("China")))
				{
					return FGenBarShape::China;
				}
				if (Part.Image.Contains(TEXT("GLA")))
				{
					return FGenBarShape::GLA;
				}
			}
		}
		return FGenBarShape::America;
	}

	/** The middle of a rectangle in a cameo's own shape (5:4): the layouts' cells were made for a 4:3 screen's pixels. */
	FBox2D FitCameo(const FBox2D& R)
	{
		if (!R.bIsValid)
		{
			return R;
		}
		const FVector2D Size = R.GetSize(), Center = R.GetCenter();
		const FVector2D Half = (Size.X > Size.Y * 1.25 ? FVector2D(Size.Y * 1.25, Size.Y) : FVector2D(Size.X, Size.X / 1.25)) * 0.5;
		return FBox2D(Center - Half, Center + Half);
	}
}

FBox2D FGenControlBar::WindowRect(const TCHAR* ShortName) const
{
	// (The layout's command window is laid onto the section's.)
	const FGenWndWindow* W = Wnd ? Wnd->Find(ShortName) : nullptr;
	const FGenWndWindow* From = Wnd ? Wnd->Find(TEXT("CommandWindow")) : nullptr;
	if (!W || !From || !Shape)
	{
		return FBox2D(ForceInit);
	}
	const FVector2D K = Shape->CommandWindow.GetSize() / From->Rect.GetSize();
	const FVector2D Min = Shape->CommandWindow.Min + (W->Rect.Min - From->Rect.Min) * K, Max = Shape->CommandWindow.Min + (W->Rect.Max - From->Rect.Min) * K;
	return ToPod(BarPodCommands, FBox2D(Min, Max));
}

FBox2D FGenControlBar::CommandCell(int32 Row, int32 Col) const
{
	using namespace GenBarLayout;
	const FVector2D Min = Shape->Grid + FVector2D(Col * (CellW + CellGapX), Row * (CellH + CellGapY));
	return ToPod(BarPodCommands, FBox2D(Min, Min + FVector2D(CellW, CellH)));
}

FBox2D FGenControlBar::QueueCell(int32 Row, int32 Col) const
{
	using namespace GenBarLayout;
	const FVector2D Min = Shape->Queue + FVector2D(Col * (QueueW + QueueGap), Row * (QueueH + QueueGap));
	return ToPod(BarPodRight, FBox2D(Min, Min + FVector2D(QueueW, QueueH)));
}

void FGenControlBar::Layout(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection, const FVector2D& ViewSize)
{
	if (Logic && Logic->GetPlayers().IsValidIndex(Logic->GetLocalPlayer()))
	{
		// onPlayerSciencePurchasePointsChanged: more points, flashing again; showPurchaseScience: the panel is open,
		// no more flashing; getStarImage: none left (or fewer than when it last flashed), none either.
		const int32 Points = Logic->GetPlayers()[Logic->GetLocalPlayer()].SciencePurchasePoints;
		if (Points > SeenPoints)
		{
			bGenStarFlash = true;
		}
		SeenPoints = Points;
		if (bGeneralsOpen)
		{
			bGenStarFlash = false;
		}
		if (LastFlashedAtPoints > Points || Points <= 0)
		{
			bGenStarFlash = false;
		}
		else
		{
			LastFlashedAtPoints = Points;
		}
		// CAMEO_FLASH: the one button the missions flash.
		if (const FGenMapScripts* Scripts = Logic->GetMapScripts(); Scripts && Scripts->IsCameoFlashing(TEXT("NonCommand_GeneralsExperience")))
		{
			bGenStarFlash = true;
		}
	}
	bVisible = false;
	bHasGenerals = false;
	bShowQueue = false;
	Subject = GenInvalidObjectID;
	Status.Reset();
	StatusRect = FBox2D(ForceInit);
	Progress = -1.f;
	ProgressRect = FBox2D(ForceInit);
	Buttons.Reset();
	Queue.Reset();
	QueueSlots.Reset();
	UnitUpgrades.Reset();
	Portrait.Reset();
	Sciences.Reset();
	GeneralsTexts.Reset();
	Shortcuts.Reset();
	ShortcutRail = FBox2D(ForceInit);
	SystemButtons.Reset();
	Radar = FBox2D(ForceInit);
	LampRect = FBox2D(ForceInit);
	Shape = nullptr;
	MenuButton = FBox2D(ForceInit);
	Panel = FBox2D(ForceInit);
	if (!Logic || !Logic->IsRunning() || ViewSize.X <= 0.0 || ViewSize.Y <= 0.0)
	{
		return;
	}
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FGenPlayerTemplate* Faction = Players.IsValidIndex(Local) ? Players[Local].Template : nullptr;
	Wnd = GenWnd::Load(TEXT("ControlBar.wnd"));
	Scheme = GenControlBarSchemes::FindBySide(Faction ? Faction->Side : FString());
	if (!Wnd)
	{
		return;
	}
	Style = &GenHudStyles::Current();
	RadarMapSize = Logic->GetMap() ? Logic->GetMap()->GetPlayableBounds().Max : FVector2f::ZeroVector;
	Shape = &Style->GetShape(FindSide(Scheme), RadarMapSize.Y > 0.f ? RadarMapSize.X / RadarMapSize.Y : 1.f);
	// ControlBarSchemeManager's m_multiplyer is the screen's size over 800 x 600 each way; here the layouts keep
	// their shape - scaled to the screen's height - whatever the screen's shape (and in a window too narrow for it,
	// to what lets the sections stand side by side).
	using namespace GenBarLayout;
	const FIntPoint Res = Scheme ? Scheme->CreationRes : FIntPoint(800, 600);
	Scale = float(ViewSize.Y / double(Res.Y)) * FMath::Clamp(CVarGenControlBarScale.GetValueOnGameThread(), 0.3f, 2.f);
	Scale = FMath::Min(Scale, float(ViewSize.X / (Shape->PodSize[BarPodRadar].X + Shape->PodSize[BarPodCommands].X + Shape->PodSize[BarPodRight].X + 2.0 * (ScreenMargin + SectionGap))));
	BarOrigin = FVector2D((ViewSize.X - double(Res.X) * Scale) * 0.5, ViewSize.Y - double(Res.Y) * Scale);
	{
		const double Margin = double(ScreenMargin) * Scale, Gap = double(SectionGap) * Scale;
		const double Spread = FMath::Clamp(double(CVarGenHudSpread.GetValueOnGameThread()), 0.0, 1.0);
		const FVector2D Left = Shape->PodSize[BarPodRadar] * double(Scale), Middle = Shape->PodSize[BarPodCommands] * double(Scale), Right = Shape->PodSize[BarPodRight] * double(Scale);
		const double MiddleX = (ViewSize.X - Middle.X) * 0.5;
		const double Bottom = ViewSize.Y - Margin;
		PodOrigin[BarPodRadar] = FVector2D(FMath::RoundToDouble(FMath::Lerp(MiddleX - Gap - Left.X, Margin, Spread)), FMath::RoundToDouble(Bottom - Left.Y));
		PodOrigin[BarPodCommands] = FVector2D(FMath::RoundToDouble(MiddleX), FMath::RoundToDouble(Bottom - Middle.Y));
		PodOrigin[BarPodRight] = FVector2D(FMath::RoundToDouble(FMath::Lerp(MiddleX + Middle.X + Gap, ViewSize.X - Margin - Right.X, Spread)), FMath::RoundToDouble(Bottom - Right.Y));
	}
	Radar = ToPod(BarPodRadar, Shape->Radar);
	{
		// The side's picture (5:4 and a bit) in the middle of the right panel's cells.
		const FBox2D Cells(QueueCell(0, 0).Min, QueueCell(2, 2).Max);
		const FVector2D Center = Cells.GetCenter(), Half(Cells.GetSize().X * 0.5, Cells.GetSize().X * 0.5 * 109.0 / 139.0);
		RightHUDRect = FBox2D(Center - Half, Center + Half);
	}
	ExpRect = ToPod(BarPodRight, Shape->ExpFill);
	MoneyRect = ToPod(BarPodCommands, Shape->Money);
	PowerRect = ToPod(BarPodCommands, Shape->Power);
	LampRect = ToPod(BarPodRadar, Shape->Lamp);
	LayoutSystemButtons(Logic);
	LayoutGenerals(Logic);
	LayoutShortcuts(Logic, ViewSize);
	LayoutSelection(Logic, Selection);
	AssignHotkeys();
}

bool FGenControlBar::GetGridCell(int32 Slot, int32& OutRow, int32& OutCol)
{
	// The cells' places in ControlBar.wnd, sorted into rows (by top edge) and columns (by left edge): 01, 03 .. 13 make
	// the top row and 02, 04 .. 14 the one under it, as the layout has them.
	constexpr int32 NumCells = 14;
	static const FGenWndLayout* CachedFor = nullptr;
	static int32 Rows[NumCells], Cols[NumCells];
	const FGenWndLayout* Layout = GenWnd::Load(TEXT("ControlBar.wnd"));
	if (!Layout || Slot < 0 || Slot >= NumCells)
	{
		return false;
	}
	if (CachedFor != Layout)
	{
		CachedFor = Layout;
		TArray<float> Tops, Lefts;
		FBox2D Cells[NumCells];
		for (int32 i = 0; i < NumCells; ++i)
		{
			const FGenWndWindow* W = Layout->Find(*FString::Printf(TEXT("ButtonCommand%02d"), i + 1));
			Cells[i] = W ? W->Rect : FBox2D(ForceInit);
			if (!Cells[i].bIsValid)
			{
				continue;
			}
			// (Cells of one row or column line up to within a few pixels.)
			auto AddUnique = [](TArray<float>& List, float V) { if (!List.ContainsByPredicate([V](float O) { return FMath::Abs(O - V) < 6.f; })) { List.Add(V); } };
			AddUnique(Tops, float(Cells[i].Min.Y));
			AddUnique(Lefts, float(Cells[i].Min.X));
		}
		Tops.Sort();
		Lefts.Sort();
		for (int32 i = 0; i < NumCells; ++i)
		{
			Rows[i] = Cols[i] = INDEX_NONE;
			if (!Cells[i].bIsValid)
			{
				continue;
			}
			const float Y = float(Cells[i].Min.Y), X = float(Cells[i].Min.X);
			Rows[i] = Tops.IndexOfByPredicate([Y](float V) { return FMath::Abs(V - Y) < 6.f; });
			Cols[i] = Lefts.IndexOfByPredicate([X](float V) { return FMath::Abs(V - X) < 6.f; });
		}
	}
	OutRow = Rows[Slot];
	OutCol = Cols[Slot];
	return OutRow != INDEX_NONE && OutCol != INDEX_NONE;
}

void FGenControlBar::AssignHotkeys()
{
	const FGenInput& Input = FGenInput::Get();
	for (TArray<FGenBarButton>* List : { &Buttons, &Sciences, &Shortcuts })
	{
		for (FGenBarButton& B : *List)
		{
			B.Hotkey = FGenKeyChord();
			if (B.Button && B.QueueIndex == INDEX_NONE && B.System == EGenBarSystem::None && !B.bGeneralsToggle)
			{
				// (The text shown: a power upgraded by a promotion takes the promotion's name, and its letter.)
				B.Hotkey = Input.GetButtonKey(B.Button->Name, B.TextLabel.IsEmpty() ? B.Button->TextLabel : B.TextLabel, B.GridRow, B.GridCol);
			}
		}
	}
}

const FGenBarButton* FGenControlBar::FindHotkey(const FGenKeyChord& Pressed) const
{
	if (!Pressed.IsValid())
	{
		return nullptr;
	}
	auto Search = [&Pressed](const TArray<FGenBarButton>& List) -> const FGenBarButton*
	{
		const FGenBarButton* Greyed = nullptr;
		for (const FGenBarButton& B : List)
		{
			if (B.Hotkey.IsValid() && B.Hotkey == Pressed)
			{
				if (B.bEnabled)
				{
					return &B;
				}
				Greyed = Greyed ? Greyed : &B;
			}
		}
		return Greyed;
	};
	const FGenBarButton* Found = nullptr;
	if (bHasGenerals && bGeneralsOpen)
	{
		Found = Search(Sciences);
	}
	if (!Found && bVisible)
	{
		Found = Search(Buttons);
	}
	if (!Found)
	{
		Found = Search(Shortcuts);
	}
	return Found;
}

void FGenControlBar::LayoutSystemButtons(UGenGameLogic* Logic)
{
	// ControlBarScheme::init: the bar's own buttons, where the scheme puts them.
	if (!Scheme)
	{
		return;
	}
	auto Add = [this](EGenBarSystem System, int32 Row, bool bEnabled) -> FBox2D
	{
		FGenBarButton& B = SystemButtons.AddDefaulted_GetRef();
		B.Rect = ToPod(BarPodCommands, Shape->Keys[Row]);
		B.System = System;
		B.bEnabled = bEnabled;
		return B.Rect;
	};
	// InGameUI::m_idleWorkers: dozers and workers of ours with nothing to do (looked for a few times a second).
	if (Logic->GetFrame() >= IdleWorkerCheckFrame || Logic->GetFrame() + 60 < IdleWorkerCheckFrame)
	{
		IdleWorkerCheckFrame = Logic->GetFrame() + 8;
		IdleWorkers = 0;
		const int32 Local = Logic->GetLocalPlayer();
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && O->GetOwner() == Local && O->IsKindOf(TEXT("DOZER")) && !O->IsDead() && O->IsAIIdle() && (!O->GetTemplate()->bIsSupplyTruck || O->GetSupplyState() == EGenSupplyState::Idle))
			{
				++IdleWorkers;
			}
		}
	}
	MenuButton = Add(EGenBarSystem::Options, 0, true);
	Add(EGenBarSystem::IdleWorker, 1, IdleWorkers > 0);
}

bool FGenControlBar::FillPower(UGenGameLogic* Logic, const FGenObject* Source, FGenBarButton& B) const
{
	const int32 Local = Logic->GetLocalPlayer();
	const FGenSpecialPowerTemplate* T = FGenUpgradeData::Get().FindPower(B.Button->SpecialPower);
	// ControlBar::populateCommand: a power whose promotion hasn't been bought isn't shown at all.
	if (!T || (!T->RequiredScience.IsEmpty() && !Logic->HasScience(Local, T->RequiredScience)))
	{
		return false;
	}
	int32 Module = INDEX_NONE;
	if (Source)
	{
		Module = Logic->FindPowerModule(*Source, T->Name);
	}
	else
	{
		Source = Logic->FindPowerSource(Local, T->Name, false, &Module); // a shortcut: whichever building has it
		if (!Source)
		{
			return false;
		}
	}
	B.Power = T->Name;
	B.Source = Source->GetID();
	// ControlBar::populateCommand: a power button that lists sciences ("These will cause the buttons to change
	// icons, nothing more") takes the picture and text of the promotion button of the best one owned - the fuel
	// air bomb becomes the MOAB, the A-10 strike shows its level.
	int32 Best = INDEX_NONE;
	for (int32 i = 0; i < B.Button->Sciences.Num() && Logic->HasScience(Local, B.Button->Sciences[i]); ++i)
	{
		Best = i;
	}
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const FGenPlayerTemplate* Faction = Players.IsValidIndex(Local) ? Players[Local].Template : nullptr;
	const FGenCommandData& CD = FGenCommandData::Get();
	for (int32 s = 0; Best != INDEX_NONE && Faction && s < 3; ++s)
	{
		const FGenCommandSet* Set = CD.FindSet(Faction->PurchaseScienceSets[s]);
		for (int32 i = 0; Set && i < FGenCommandSet::NumSlots; ++i)
		{
			const FGenCommandButton* Look = CD.FindButton(Set->Slots[i]);
			if (Look && Look->Command == EGenCommand::PurchaseScience && Look->Sciences.Num() > 0
				&& Look->Sciences[0].Equals(B.Button->Sciences[Best], ESearchCase::IgnoreCase))
			{
				// (CommandButton::copyImagesFrom / copyButtonTextFrom: a shortcut takes the "conflicting" label.)
				B.Image = Look->ButtonImage;
				const FString& Text = B.Button->bFromShortcut ? Look->ConflictingLabel : Look->TextLabel;
				B.TextLabel = Text;
				B.DescriptLabel = Look->DescriptLabel;
				Best = INDEX_NONE;
				break;
			}
		}
	}
	if (Module == INDEX_NONE)
	{
		B.Disabled = TEXT("Not available");
		return true;
	}
	B.bEnabled = Logic->IsPowerReady(*Source, Module, &B.ReadyFrame, &B.Disabled);
	if (B.bEnabled && Source->IsDisabled())
	{
		B.bEnabled = false;
		B.Disabled = TEXT("Disabled");
	}
	if (B.ReadyFrame > Logic->GetFrame() && T->ReloadTime > 0)
	{
		B.Recharge = FMath::Clamp(float(B.ReadyFrame - Logic->GetFrame()) / float(T->ReloadTime), 0.f, 1.f);
	}
	// SpecialAbilityUpdate::isPowerCurrentlyInUse: "This is the detonate charge button ... the button will be
	// grayed out" while there are no charges to set off.
	if (B.bEnabled && T->AbilityKind == EGenAbilityKind::RemoteCharges && !B.Button->bNeedTargetObject && !B.Button->bNeedTargetPos
		&& Source->GetAbilityObjectCount(Source->FindAbility(T->Name)) == 0)
	{
		B.bEnabled = false;
		B.Disabled = TEXT("No charges placed");
	}
	return true;
}

bool FGenControlBar::CanBuyForItself(const UGenGameLogic* Logic, const FGenObject& Unit, const FGenCommandButton& Button, const FGenUpgradeTemplate& Upgrade)
{
	if (!Logic->CanPlayerCommand(Logic->GetLocalPlayer(), Unit) || Unit.IsDead() || Unit.HasObjectUpgrade(Upgrade.Name) || !Unit.HasPortedUpgradeEffect(Upgrade)
		|| !Logic->CanQueueUpgrade(Unit, Upgrade, nullptr))
	{
		return false;
	}
	const FGenCommandSet* Set = FGenCommandData::Get().FindSet(Unit.GetCommandSet());
	if (Set)
	{
		for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
		{
			if (Set->Slots[i].Equals(Button.Name, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
	}
	return false;
}

void FGenControlBar::LayoutSelection(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection)
{
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenUpgradeData& UD = FGenUpgradeData::Get();
	const int32 Local = Logic->GetLocalPlayer();

	// ControlBar's structure inventory: a garrisoned building without a command set of its own still
	// lists its occupants (ten exit buttons and "evacuate").
	static const FGenCommandSet InventorySet = []
	{
		FGenCommandSet S;
		S.Name = TEXT("StructureInventory");
		for (int32 i = 0; i < 10; ++i) { S.Slots[i] = TEXT("Command_StructureExit"); }
		S.Slots[10] = TEXT("Command_Evacuate");
		return S;
	}();

	// The first selected object of ours that has a command set speaks for the selection.
	const FGenObject* Obj = nullptr;
	const FGenCommandSet* Set = nullptr;
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* O = Logic->FindObject(Id);
		const FGenCommandSet* S = (O && Logic->CanPlayerCommand(Local, *O) && !O->IsDead()) ? CD.FindSet(O->GetCommandSet()) : nullptr;
		if (!S && O && Logic->CanPlayerCommand(Local, *O) && !O->IsDead() && O->GetContain() && Logic->GetRiders(*O).Num() > 0)
		{
			S = &InventorySet;
		}
		// (The supply drop zone has no command set: "Context window is a reaction to this" - its OCL timer.)
		static const FGenCommandSet NoCommands;
		if (!S && O && Logic->CanPlayerCommand(Local, *O) && !O->IsDead() && O->GetTemplate()->OCLUpdate.bValid)
		{
			S = &NoCommands;
		}
		if (S && O && Logic->GetMapScripts())
		{
			bool bRemoved = false;
			for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
			{
				const FString* Added = Logic->GetMapScripts()->GetButtonAdded(O->GetCommandSet(), i);
				if (Added || Logic->GetMapScripts()->IsButtonRemoved(O->GetCommandSet(), i))
				{
					if (!bRemoved)
					{
						ScriptFilteredSet = *S;
						bRemoved = true;
					}
					ScriptFilteredSet.Slots[i] = Added ? *Added : FString();
				}
			}
			if (bRemoved)
			{
				S = &ScriptFilteredSet;
			}
		}
		if (S)
		{
			Obj = O;
			Set = S;
			break;
		}
	}
	// (The right panel shows what is selected even when it is somebody else's: its portrait.)
	const FGenObject* Shown = Obj;
	for (int32 i = 0; !Shown && i < Selection.Num(); ++i)
	{
		Shown = Logic->FindObject(Selection[i]);
	}
	LayoutRightPanel(Logic, Shown);
	{
		static FString LastPanel;
		const FString PanelText = FString::Printf(TEXT("%s: %s, portrait '%s' at %s, %d upgrade cameo(s)"), Shown ? *Shown->GetTemplate()->Name : TEXT("nothing"),
			bShowQueue ? TEXT("queue") : TEXT("unit"), *Portrait, *PortraitRect.ToString(), UnitUpgrades.Num());
		UE_CLOG(Logic->IsTracing() && PanelText != LastPanel, LogGenerals, Log, TEXT("  [bar f%u] right panel %s"), Logic->GetFrame(), *PanelText);
		LastPanel = PanelText;
	}
	if (!Obj)
	{
		return;
	}
	bVisible = true;
	Subject = Obj->GetID();
	Panel = WindowRect(TEXT("CommandWindow"));

	if (Obj->IsBeingSold())
	{
		Status = TEXT("Being sold");
		StatusRect = WindowRect(TEXT("UnderConstructionDesc"));
		return;
	}
	if (Obj->IsUnderConstruction())
	{
		// ControlBar::populateUnderConstruction: "CONTROLBAR:UnderConstructionDesc" and the button that gives the
		// site up (Command_CancelConstruction on ButtonCancelConstruction).
		FString Desc = CD.GetText(TEXT("CONTROLBAR:UnderConstructionDesc"));
		if (Desc.IsEmpty() || Desc.StartsWith(TEXT("CONTROLBAR:")))
		{
			Desc = TEXT("Under construction %d%%");
		}
		Desc.ReplaceInline(TEXT("%.0f"), TEXT("%d"));
		Desc.ReplaceInline(TEXT("%%"), TEXT("%"));
		Desc.ReplaceInline(TEXT("%d"), *FString::FromInt(FMath::FloorToInt32(Obj->GetConstructionPercent())));
		Status = Desc;
		StatusRect = WindowRect(TEXT("UnderConstructionDesc"));
		if (Logic->CanPlayerCommand(Local, *Obj))
		{
			FGenBarButton B;
			B.Rect = FitCameo(WindowRect(TEXT("ButtonCancelConstruction")));
			B.Button = CD.FindButton(TEXT("Command_CancelConstruction"));
			B.Source = Subject;
			B.bEnabled = B.Button != nullptr;
			if (B.Rect.bIsValid && B.Button)
			{
				Buttons.Add(MoveTemp(B));
			}
		}
		return;
	}
	if (Obj->GetTemplate()->OCLUpdate.bValid)
	{
		// ControlBar::populateOCLTimer (CB_CONTEXT_OCL_TIMER takes the place of the command set): when the next
		// delivery comes, a bar filling up to it, and the sell button - or, for a tech building with its own rally
		// point, that button instead.
		const uint32 Now = Logic->GetFrame();
		const int32 Seconds = int32(Obj->GetOCLRemainingFrames(Now) / uint32(FMath::Max(1, GenLogic::GetLogicFPS())));
		const int32 Min = Seconds / 60, Sec = Seconds % 60;
		FString Desc = CD.GetText(Sec < 10 ? TEXT("CONTROLBAR:OCLTimerDescWithPadding") : TEXT("CONTROLBAR:OCLTimerDesc"));
		if (Desc.IsEmpty() || Desc.StartsWith(TEXT("CONTROLBAR:")))
		{
			Desc = TEXT("Next delivery in %d:%02d");
		}
		// (The string's own "%d" / "%02d" in turn: minutes, then seconds.)
		for (const int32 Value : { Min, Sec })
		{
			int32 At = Desc.Find(TEXT("%"));
			while (At != INDEX_NONE && At + 1 < Desc.Len() && Desc[At + 1] == TEXT('%'))
			{
				At = Desc.Find(TEXT("%"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 2);
			}
			if (At == INDEX_NONE)
			{
				break;
			}
			int32 End = At + 1;
			while (End < Desc.Len() && !FChar::IsAlpha(Desc[End]))
			{
				++End;
			}
			const bool bPad = Desc.Mid(At, End - At).Contains(TEXT("02"));
			Desc = Desc.Left(At) + (bPad ? FString::Printf(TEXT("%02d"), Value) : FString::FromInt(Value)) + Desc.Mid(End + 1);
		}
		Status = Desc;
		StatusRect = WindowRect(TEXT("OCLTimerStaticText"));
		Progress = Obj->GetOCLCountdownPercent(Now);
		ProgressRect = WindowRect(TEXT("OCLTimerProgressBar"));
		if (Logic->CanPlayerCommand(Local, *Obj))
		{
			const bool bRally = Obj->IsKindOf(TEXT("TECH_BUILDING")) && Obj->IsKindOf(TEXT("AUTO_RALLYPOINT"));
			if (!Obj->IsKindOf(TEXT("TECH_BUILDING")) || bRally)
			{
				FGenBarButton B;
				B.Rect = CommandCell(1, 6); // (where the sell button of every other building is)
				B.Button = CD.FindButton(bRally ? TEXT("Command_SetRallyPoint") : TEXT("Command_Sell"));
				B.Source = Subject;
				B.bEnabled = B.Button != nullptr && (!bRally || Logic->CanSetRallyPoint(*Obj));
				if (B.Rect.bIsValid && B.Button)
				{
					Buttons.Add(MoveTemp(B));
				}
			}
		}
		return;
	}
	if (Obj->IsDisabled())
	{
		Status = TEXT("Disabled");
		StatusRect = WindowRect(TEXT("UnderConstructionDesc"));
		return;
	}

	constexpr int32 NumCommandButtons = 14; // ControlBar.wnd's ButtonCommand01..14
	const bool bQueueFull = Obj->GetProductionQueue().Num() >= Obj->GetTemplate()->MaxQueueEntries;
	const TArray<FGenObjectID>& Riders = Logic->GetRiders(*Obj);
	int32 NextRider = 0;
	for (int32 Slot = 0; Slot < NumCommandButtons; ++Slot)
	{
		const FGenCommandButton* Btn = CD.FindButton(Set->Slots[Slot]);
		// (ControlBar::setControlCommand: "commandButton == NULL || BitIsSet( commandButton->getOptions(), SCRIPT_ONLY )": no button.)
		if (!Btn || Btn->bScriptOnly)
		{
			continue;
		}
		FGenBarButton B;
		// ControlBar.wnd's ButtonCommandNN for slot NN (1 and 2 share the first column, 13 and 14 the last), in its
		// row and column of the command window's cells.
		if (!GetGridCell(Slot, B.GridRow, B.GridCol))
		{
			continue;
		}
		B.Rect = CommandCell(B.GridRow, B.GridCol);
		B.Button = Btn;
		B.Source = Subject;
		B.Thing = FGenGameData::Get().FindTemplate(Btn->Object);
		// getCommandAvailability: "BSTATUS_NO" (and Only_By_AI for a person) - COMMAND_HIDDEN.
		if ((Btn->Command == EGenCommand::UnitBuild || Btn->Command == EGenCommand::DozerConstruct) && B.Thing
			&& (Logic->GetBuildable(*B.Thing) == 1 || (Logic->GetBuildable(*B.Thing) == 2 && !Logic->GetPlayers()[Local].bSkirmishAI)))
		{
			continue;
		}
		switch (Btn->Command)
		{
		case EGenCommand::UnitBuild:
			B.bEnabled = B.Thing && Logic->CanMake(Local, *B.Thing, &B.Disabled);
			if (B.bEnabled && bQueueFull)
			{
				B.bEnabled = false;
				B.Disabled = TEXT("Production queue is full");
			}
			else if (B.bEnabled && Obj->GetParking() && !Obj->GetParking()->HasAvailableSpaceFor(*B.Thing))
			{
				B.bEnabled = false;
				B.Disabled = TEXT("No parking space left at this airfield");
			}
			break;
		case EGenCommand::DozerConstruct:
			B.bEnabled = B.Thing && Logic->CanMake(Local, *B.Thing, &B.Disabled);
			break;
		case EGenCommand::Sell:
			if (Obj->IsScriptUnsellable())
			{
				B.Disabled = TEXT("Not available"); // (ControlBar::getCommandAvailability: "OBJECT_STATUS_SCRIPT_UNSELLABLE")
				break;
			}
			// fall through
		case EGenCommand::Stop:
			// ("OPTION_ONE ... designates special case handling to hide stop button when not applicable": the
			// strategy center's, there only to stop the bombardment gun.)
			B.bEnabled = !(Btn->CommandOption == 1 && Obj->GetTemplate()->BattlePlan.bValid && Obj->GetActiveBattlePlan() != 1);
			B.Disabled = TEXT("Only with the bombardment plan in force");
			break;
		case EGenCommand::AttackMove:
			// (A ship's deck moves nothing itself: FlightDeckBehavior::aiDoCommand passes these on to its aircraft.)
			B.bEnabled = (Obj->CanMove() && Obj->HasWeapons()) || (Obj->GetParking() && Obj->GetParking()->IsFlightDeck());
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::Guard:
		case EGenCommand::GuardWithoutPursuit:
		case EGenCommand::GuardFlyingOnly:
			B.bEnabled = Obj->CanMove() || (Obj->GetParking() && Obj->GetParking()->IsFlightDeck());
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::PlayerUpgrade:
		case EGenCommand::ObjectUpgrade:
		{
			B.Upgrade = UD.FindUpgrade(Btn->Upgrade);
			if (!B.Upgrade)
			{
				B.Disabled = TEXT("Not available");
				break;
			}
			B.Cost = B.Upgrade->BuildCost;
			B.bOwned = B.Upgrade->bObjectUpgrade ? Obj->HasObjectUpgrade(B.Upgrade->Name) : Logic->HasUpgradeComplete(Local, B.Upgrade->Name);
			if (B.bOwned)
			{
				B.Disabled = B.Upgrade->bObjectUpgrade ? TEXT("Already installed") : TEXT("Already researched");
			}
			else if (B.Upgrade->bObjectUpgrade && !Obj->HasPortedUpgradeEffect(*B.Upgrade))
			{
				B.Disabled = TEXT("Not available yet"); // drones, add-ons: what they create isn't in the game so far
			}
			else
			{
				B.bEnabled = Logic->CanQueueUpgrade(*Obj, *B.Upgrade, &B.Disabled);
				// The button may name promotions that are needed first.
				for (const FString& Needed : Btn->Sciences)
				{
					if (B.bEnabled && !Logic->HasScience(Local, Needed))
					{
						B.bEnabled = false;
						B.Disabled = TEXT("Requires a general's promotion");
					}
				}
			}
			// (What each unit buys for itself is bought for everybody selected: the button works if any of them can.)
			if (!B.bEnabled && B.Upgrade->bObjectUpgrade)
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* Other = Logic->FindObject(Id);
					if (Other && Other != Obj && CanBuyForItself(Logic, *Other, *Btn, *B.Upgrade))
					{
						B.bEnabled = true;
						B.bOwned = false;
						B.Disabled.Reset();
						break;
					}
				}
			}
			break;
		}
		case EGenCommand::SpecialPower:
			if (!FillPower(Logic, Btn->bFromShortcut ? nullptr : Obj, B))
			{
				continue; // hidden
			}
			// ControlBar::getCommandAvailability, SPECIAL_CHANGE_BATTLE_PLANS: "COMMAND_ACTIVE" (lit) for the plan
			// the strategy center was last told to take up.
			if (Btn->CommandOption != 0 && Obj->GetTemplate()->BattlePlan.bValid && Obj->GetDesiredBattlePlan() == Btn->CommandOption)
			{
				B.bOwned = true;
				if (B.Disabled.IsEmpty()) { B.Disabled = TEXT("The current battle plan"); }
			}
			break;
		case EGenCommand::ExitContainer:
		{
			// One button per occupant, in the order they got in; the spare ones don't show.
			const FGenObject* Rider = Riders.IsValidIndex(NextRider) ? Logic->FindObject(Riders[NextRider]) : nullptr;
			++NextRider;
			if (!Rider)
			{
				continue;
			}
			B.Rider = Rider->GetID();
			B.Image = Rider->GetTemplate()->ButtonImage;
			B.Label = CD.GetText(Rider->GetTemplate()->DisplayName);
			B.bEnabled = !Rider->WantsToExit();
			B.Disabled = TEXT("On the way out");
			break;
		}
		case EGenCommand::Evacuate:
			B.bEnabled = Riders.Num() > 0;
			B.Disabled = TEXT("Nobody inside");
			break;
		case EGenCommand::SabotageBuilding:
			B.bEnabled = Obj->GetTemplate()->Sabotage.Any() && !Obj->IsContained();
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::HijackVehicle:
		case EGenCommand::ConvertToCarBomb:
			// (ConvertToHijackedVehicleCrateCollide / ConvertToCarBombCrateCollide: the soldier's own way in.)
			B.bEnabled = Obj->GetTemplate()->VehicleTakeover.Kind != FGenThingTemplate::FVehicleTakeover::None && !Obj->IsContained();
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::HackInternet:
			B.bEnabled = Obj->GetTemplate()->HackInternet.bValid && !Obj->IsContained();
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::CombatDrop:
			// ControlBar::getCommandAvailability: "if (getRappellerCount(obj) <= 0) return COMMAND_RESTRICTED"
			B.bEnabled = Logic->HasRappeller(*Obj);
			B.Disabled = TEXT("Nobody on board who can slide down a rope");
			break;
		case EGenCommand::SwitchWeapon:
		{
			// ControlBar::getCommandAvailability: restricted without a weapon in that slot; "active" (shown
			// lit) when every selected one of ours already has it in hand.
			B.bEnabled = Btn->WeaponSlot != INDEX_NONE && Obj->GetWeapon(Btn->WeaponSlot).Template != nullptr;
			B.Disabled = TEXT("Not available");
			bool bAllUsing = B.bEnabled;
			for (FGenObjectID Id : Selection)
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (O && Logic->CanPlayerCommand(Local, *O) && O->GetTemplate() == Obj->GetTemplate() && O->GetCurrentWeaponSlot() != Btn->WeaponSlot)
				{
					bAllUsing = false;
				}
			}
			B.bOwned = bAllUsing;
			break;
		}
		case EGenCommand::SetRallyPoint:
			B.bEnabled = Logic->CanSetRallyPoint(*Obj);
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::ToggleOvercharge:
			// (Lit while it is on; restricted when the reactor is too damaged to start one.)
			B.bOwned = Obj->IsOverchargeActive();
			B.bEnabled = B.bOwned || Obj->CanOvercharge();
			B.Disabled = TEXT("Too damaged to overcharge");
			break;
		case EGenCommand::FireWeapon:
			// (Only the kind that is aimed at something: a knife. Blowing oneself up comes with the bomb trucks.)
			// ("If this is a mine-clearing button but we don't have the right weaponset, just declare it
			// available... we'll switch weaponsets when the time comes.")
			B.bEnabled = Btn->WeaponSlot != INDEX_NONE
				&& (Obj->GetWeapon(Btn->WeaponSlot).Template != nullptr || (Btn->bUsesMineClearingWeaponSet && Obj->GetTemplate()->bHasMineClearingSet));
			B.Disabled = Btn->bNeedTargetObject ? TEXT("Not available") : TEXT("Not available yet");
			// ControlBar::getCommandAvailability: "only draw the clock when reloading a clip, not when merely between
			// shots" - and "We need to show the button as always available for anything with a 0 clip reload time."
			if (B.bEnabled && Btn->WeaponSlot != INDEX_NONE)
			{
				const FGenWeaponInstance& Weapon = Obj->GetWeapon(Btn->WeaponSlot);
				const uint32 Now = Logic->GetFrame();
				if (Weapon.Template && Weapon.Template->ClipReloadTime != 0 && (Weapon.ReadyFrame > Now || (Weapon.Template->ClipSize > 0 && Weapon.AmmoInClip <= 0)))
				{
					B.bEnabled = false;
					B.Disabled = TEXT("Reloading");
					if (Weapon.bReloading && Weapon.ReadyFrame > Now)
					{
						B.Recharge = FMath::Clamp(float(Weapon.ReadyFrame - Now) / float(Weapon.Template->ClipReloadTime), 0.f, 1.f);
					}
				}
			}
			break;
		default:
			B.Disabled = TEXT("Not available yet");
			break;
		}
		Buttons.Add(MoveTemp(B));
	}

	if (bShowQueue)
	{
		const TArray<FGenObject::FProduction>& Prod = Obj->GetProductionQueue();
		for (int32 i = 0; i < Prod.Num(); ++i)
		{
			if (!QueueSlots.IsValidIndex(i))
			{
				break;
			}
			FGenBarButton& Q = Queue.AddDefaulted_GetRef();
			Q.Rect = QueueSlots[i];
			Q.Thing = Prod[i].Unit;
			Q.Upgrade = Prod[i].Upgrade;
			Q.Source = Subject;
			Q.QueueIndex = i;
			Q.bEnabled = true;
			// Reuse the icon of the button that builds this unit / researches this upgrade.
			for (const FGenBarButton& B : Buttons)
			{
				if ((Q.Upgrade && B.Upgrade == Q.Upgrade) || (!Q.Upgrade && B.Thing && B.Thing == Q.Thing))
				{
					Q.Button = B.Button;
					break;
				}
			}
		}
	}
}

void FGenControlBar::LayoutRightPanel(UGenGameLogic* Logic, const FGenObject* Obj)
{
	// The right panel: a factory's production queue (ProductionQueueWindow), or the selected unit's portrait
	// and upgrade cameos (WinUnitSelected), or - nothing selected - the side's picture (RightHUDImage).
	// (ButtonQueue01..09 run row by row. The portrait takes four of the same cells, the upgrades as many as are left,
	// in the order of UnitUpgrade1..5: down the right, then back along the bottom.)
	for (int32 i = 0; i < 9; ++i)
	{
		QueueSlots.Add(QueueCell(i / 3, i % 3));
	}
	PortraitRect = FBox2D(QueueCell(0, 0).Min, QueueCell(1, 1).Max);
	if (!Obj)
	{
		return;
	}
	const int32 Local = Logic->GetLocalPlayer();
	// ("for objects that are able to create units, we show a build queue if we actually have something in
	// production, otherwise we show the selection portrait")
	bShowQueue = Logic->CanPlayerCommand(Local, *Obj) && Obj->GetTemplate()->bHasProduction && Obj->GetProductionQueue().Num() > 0;
	if (bShowQueue)
	{
		return; // (filled in with the command buttons, whose icons it shows)
	}
	Portrait = Obj->GetTemplate()->SelectPortrait;
	const FGenUpgradeData& UD = FGenUpgradeData::Get();
	static const FIntPoint UpgradeCells[5] = { { 0, 2 }, { 1, 2 }, { 2, 2 }, { 2, 1 }, { 2, 0 } }; // (row, column)
	for (int32 i = 0; i < 5; ++i)
	{
		// ControlBar::populateUpgradeCameos (?): the upgrade's button image, lit once the unit (or its player) has it.
		const FGenUpgradeTemplate* U = UD.FindUpgrade(Obj->GetTemplate()->UpgradeCameos[i]);
		if (!U)
		{
			continue;
		}
		FGenBarButton& B = UnitUpgrades.AddDefaulted_GetRef();
		B.Rect = QueueCell(UpgradeCells[i].X, UpgradeCells[i].Y);
		B.Upgrade = U;
		B.Image = U->ButtonImage;
		B.bOwned = U->bObjectUpgrade ? Obj->HasObjectUpgrade(U->Name) : Logic->HasUpgradeComplete(Obj->GetOwner(), U->Name);
		B.bEnabled = B.bOwned;
		B.Disabled = B.bOwned ? FString() : TEXT("Not yet");
	}
}

void FGenControlBar::LayoutGenerals(UGenGameLogic* Logic)
{
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	if (!Players.IsValidIndex(Local) || !Players[Local].Template || Players[Local].Template->PurchaseScienceSets[0].IsEmpty())
	{
		bGeneralsOpen = false;
		return;
	}
	const FGenPlayer& P = Players[Local];
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenUpgradeData& UD = FGenUpgradeData::Get();
	bHasGenerals = true;

	// The general's button (ButtonGeneral, where the scheme puts it).
	GeneralsButton = FGenBarButton();
	GeneralsButton.Rect = ToPod(BarPodRight, Shape->General);
	GeneralsButton.bGeneralsToggle = true;
	GeneralsButton.bEnabled = true;
	GeneralsButton.Cost = P.SciencePurchasePoints;
	if (!bGeneralsOpen)
	{
		return;
	}

	// GeneralsExpPoints.wnd: the panel, its texts, the purchase buttons of rank 1, 3 and 8 (5 in Zero Hour) and
	// the close button. (ControlBarScheme::init: the panel takes the size of the side's PowerPurchaseImage.)
	const FGenWndLayout* Exp = GenWnd::Load(TEXT("GeneralsExpPoints.wnd"));
	const FGenWndWindow* Parent = Exp ? Exp->Find(TEXT("GenExpParent")) : nullptr;
	if (!Parent)
	{
		bGeneralsOpen = false;
		return;
	}
	FBox2D PanelRect = Parent->Rect;
	GeneralsImage = Scheme && !Scheme->PowerPurchaseImage.IsEmpty() ? Scheme->PowerPurchaseImage : Parent->GetImage();
	if (const FGenMappedImage* Img = CD.FindImage(GeneralsImage))
	{
		PanelRect.Max = PanelRect.Min + FVector2D(Img->Size.X, Img->Size.Y);
	}
	GeneralsPanel = ToScreen(PanelRect);
	const FGenWndWindow* XPBar = Exp->Find(TEXT("ProgressBarExperience"));
	ExperienceRect = XPBar ? ToScreen(XPBar->Rect) : FBox2D(ForceInit);

	// Its texts: the fixed ones as they stand, the title and the points to spend filled in.
	const TArray<FGenRank>& Ranks = UD.GetRanks();
	TFunction<void(const FGenWndWindow&)> Texts = [&](const FGenWndWindow& W)
	{
		if (W.HasStatus(TEXT("HIDDEN")))
		{
			return;
		}
		if (W.Type == TEXT("STATICTEXT"))
		{
			FGenBarText T;
			T.Rect = ToScreen(W.Rect);
			T.Color = W.TextColor;
			T.Size = float(FMath::Max(W.FontSize, 10)) * Scale;
			if (W.ShortName == TEXT("StaticTextTitle"))
			{
				T.Text = FString::Printf(TEXT("Rank %d"), P.RankLevel);
			}
			else if (W.ShortName == TEXT("StaticTextRankPointsAvailable"))
			{
				T.Text = FString::FromInt(P.SciencePurchasePoints);
			}
			else if (!W.Text.IsEmpty())
			{
				T.Text = CD.GetText(W.Text);
			}
			if (!T.Text.IsEmpty())
			{
				GeneralsTexts.Add(T);
			}
		}
		for (const TSharedPtr<FGenWndWindow>& C : W.Children)
		{
			Texts(*C);
		}
	};
	Texts(*Parent);
	(void)Ranks;

	static const TCHAR* const Prefixes[3] = { TEXT("ButtonRank1Number"), TEXT("ButtonRank3Number"), TEXT("ButtonRank8Number") };
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenCommandSet* Set = CD.FindSet(P.Template->PurchaseScienceSets[s]);
		for (int32 i = 0; Set && i < FGenCommandSet::NumSlots; ++i)
		{
			const FGenWndWindow* W = Exp->Find(FString(Prefixes[s]) + FString::FromInt(i));
			const FGenCommandButton* Btn = CD.FindButton(Set->Slots[i]);
			// (SCRIPT_ONLY: "Hide it so we can't purchase it in the interface" - the MOAB, granted by an upgrade.)
			if (!W || !Btn || Btn->Command != EGenCommand::PurchaseScience || Btn->Sciences.Num() == 0 || Btn->bScriptOnly)
			{
				continue;
			}
			// (ControlBar::populatePurchaseScience: a mission's "Hidden" promotions aren't there; its "Disabled" ones are, greyed.)
			if (Logic->GetMapScripts() && Logic->GetMapScripts()->IsScienceHidden(Local, Btn->Sciences[0]))
			{
				continue;
			}
			FGenBarButton& B = Sciences.AddDefaulted_GetRef();
			B.Rect = FitCameo(ToScreen(W->Rect));
			B.Button = Btn;
			B.Science = Btn->Sciences[0];
			const FGenScience* Sc = UD.FindScience(B.Science);
			B.Cost = Sc ? Sc->PurchaseCost : 0;
			B.bOwned = Logic->HasScience(Local, B.Science);
			B.bEnabled = !B.bOwned && Logic->CanPurchaseScience(Local, B.Science, &B.Disabled);
			if (B.bOwned)
			{
				B.Disabled = TEXT("Acquired");
			}
		}
	}
	if (const FGenWndWindow* Exit = Exp->Find(TEXT("ButtonExit")))
	{
		FGenBarButton& B = Sciences.AddDefaulted_GetRef();
		B.Rect = ToScreen(Exit->Rect);
		B.System = EGenBarSystem::GeneralsExit;
		B.bEnabled = true;
		B.Label = CD.GetText(Exit->Text);
		B.Image = Exit->GetImage();
	}
}

void FGenControlBar::LayoutShortcuts(UGenGameLogic* Logic, const FVector2D& ViewSize)
{
	// The special power shortcuts (the faction's SpecialPowerShortcutSet): the powers the player has, from the bottom up,
	// in a rail on the screen's right edge, which grows upwards, out of the way of the view.
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenPlayerTemplate* Faction = Players.IsValidIndex(Local) ? Players[Local].Template : nullptr;
	const FGenCommandSet* Set = Faction ? CD.FindSet(Faction->SpecialPowerShortcutSet) : nullptr;
	if (!Set)
	{
		return;
	}
	using namespace GenBarLayout;
	const int32 MaxButtons = Faction->SpecialPowerShortcutButtonCount > 0 ? Faction->SpecialPowerShortcutButtonCount : 11;
	const double RailX = FMath::RoundToDouble(ViewSize.X - double(RailW) * Scale);
	const double RailBottom = FMath::RoundToDouble(ViewSize.Y - double(RailLift) * Scale);
	int32 Next = 1;
	for (int32 i = 0; i < FGenCommandSet::NumSlots && Next <= MaxButtons; ++i)
	{
		const FGenCommandButton* Btn = CD.FindButton(Set->Slots[i]);
		if (!Btn || Btn->bScriptOnly || (Btn->Command != EGenCommand::SpecialPower && Btn->Command != EGenCommand::SelectAllUnitsOfType))
		{
			continue;
		}
		FGenBarButton B;
		B.Button = Btn;
		if (Btn->Command == EGenCommand::SelectAllUnitsOfType)
		{
			// ControlBar::populateSpecialPowerShortcut: "Make sure we actually have an object of type that we want to be able to select."
			const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(Btn->Object);
			bool bHave = false;
			for (const FGenObject* O : Logic->GetObjectsOf(Local))
			{
				bHave |= Thing && !O->IsDestroyed() && !O->IsDead() && O->GetTemplate()->Canonical() == Thing->Canonical();
			}
			if (!bHave)
			{
				continue;
			}
			B.bEnabled = true;
			B.Label = CD.GetText(Btn->TextLabel);
			B.Image = Btn->ButtonImage;
		}
		else if (!FillPower(Logic, nullptr, B))
		{
			continue;
		}
		const FVector2D Min(RailX + double(RailCellX) * Scale, RailBottom - double(Next * RailPitch) * Scale);
		B.Rect = FBox2D(Min, Min + FVector2D(RailCellW, RailCellH) * double(Scale));
		Shortcuts.Add(MoveTemp(B));
		++Next;
	}
	if (Shortcuts.Num() > 0)
	{
		const double Top = FMath::RoundToDouble(RailBottom - double(RailTop + Shortcuts.Num() * RailPitch) * Scale);
		ShortcutRail = FBox2D(FVector2D(RailX, Top), FVector2D(ViewSize.X, RailBottom));
	}
}

bool FGenControlBar::IsOverPanel(const FVector2D& P) const
{
	if (bHasGenerals && bGeneralsOpen && GeneralsPanel.IsInside(P))
	{
		return true;
	}
	if (ShortcutRail.bIsValid && ShortcutRail.IsInside(P))
	{
		return true;
	}
	// The bar's sections, exactly where they are: the world shows and takes clicks between them and beside their tabs.
	if (Shape && Style && Scale > 0.f)
	{
		for (int32 Pod = 0; Pod < BarPodCount; ++Pod)
		{
			if (Style->IsPodSolid(*Shape, Pod, (P - PodOrigin[Pod]) / double(Scale)))
			{
				return true;
			}
		}
	}
	return false;
}

const FGenBarButton* FGenControlBar::HitTest(const FVector2D& P) const
{
	if (bHasGenerals)
	{
		if (bGeneralsOpen)
		{
			for (const FGenBarButton& B : Sciences)
			{
				if (B.Rect.IsInside(P)) { return &B; }
			}
			if (GeneralsPanel.IsInside(P))
			{
				return nullptr; // (the panel covers what is under it)
			}
		}
		if (GeneralsButton.Rect.IsInside(P)) { return &GeneralsButton; }
	}
	for (const FGenBarButton& B : Shortcuts)
	{
		if (B.Rect.IsInside(P)) { return &B; }
	}
	for (const FGenBarButton& B : SystemButtons)
	{
		if (B.Rect.IsInside(P)) { return &B; }
	}
	for (const FGenBarButton& B : UnitUpgrades)
	{
		if (B.Rect.IsInside(P)) { return &B; }
	}
	if (!bVisible)
	{
		return nullptr;
	}
	for (const FGenBarButton& Q : Queue)
	{
		if (Q.Rect.IsInside(P)) { return &Q; }
	}
	for (const FGenBarButton& B : Buttons)
	{
		if (B.Rect.IsInside(P)) { return &B; }
	}
	return nullptr;
}
