#include "Game/GenControlBar.h"
#include "Core/GenWnd.h"
#include "Game/GenControlBarScheme.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

static TAutoConsoleVariable<float> CVarGenControlBarScale(
	TEXT("gen.ControlBarScale"), 1.f,
	TEXT("Size of the control bar, the power tray and the promotions panel: 1 = the original's share of the screen's height (its 800 x 600 layouts scaled to the screen's height)."));

namespace
{
	/** A rectangle of a window of a layout, on the screen. */
	FBox2D MapRect(const FBox2D& R, const FVector2D& Origin, float Scale)
	{
		return R.bIsValid ? FBox2D(Origin + R.Min * double(Scale), Origin + R.Max * double(Scale)) : FBox2D(ForceInit);
	}
}

FBox2D FGenControlBar::WindowRect(const TCHAR* ShortName) const
{
	const FGenWndWindow* W = Wnd ? Wnd->Find(ShortName) : nullptr;
	return W ? ToScreen(W->Rect) : FBox2D(ForceInit);
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
	ShortcutTrays.Reset();
	SystemButtons.Reset();
	InputRects.Reset();
	Radar = FBox2D(ForceInit);
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
	// ControlBarSchemeManager's m_multiplyer is the screen's size over 800 x 600 each way; here the layouts keep
	// their shape - scaled to the screen's height, the bar centred at the bottom - whatever the screen's shape.
	const FIntPoint Res = Scheme ? Scheme->CreationRes : FIntPoint(800, 600);
	Scale = float(ViewSize.Y / double(Res.Y)) * FMath::Clamp(CVarGenControlBarScale.GetValueOnGameThread(), 0.3f, 2.f);
	BarOrigin = FVector2D((ViewSize.X - double(Res.X) * Scale) * 0.5, ViewSize.Y - double(Res.Y) * Scale);
	if (!Wnd)
	{
		return;
	}
	Wnd->GetInputRects(InputRects);
	for (FBox2D& R : InputRects)
	{
		R = ToScreen(R);
	}
	Radar = WindowRect(TEXT("LeftHUD"));
	RadarMapSize = Logic->GetMap() ? Logic->GetMap()->GetPlayableBounds().Max : FVector2f::ZeroVector;
	RightHUDRect = WindowRect(TEXT("RightHUD"));
	ExpRect = WindowRect(TEXT("GeneralsExp"));
	ExpForegroundRect = WindowRect(TEXT("ExpBarForeground"));
	MoneyRect = Scheme && Scheme->MoneyRect.bIsValid ? ToScreen(Scheme->MoneyRect) : WindowRect(TEXT("MoneyDisplay"));
	PowerRect = Scheme && Scheme->PowerBarRect.bIsValid ? ToScreen(Scheme->PowerBarRect) : WindowRect(TEXT("PowerWindow"));
	LayoutSystemButtons(Logic);
	LayoutGenerals(Logic);
	LayoutShortcuts(Logic, ViewSize);
	LayoutSelection(Logic, Selection);
}

void FGenControlBar::LayoutSystemButtons(UGenGameLogic* Logic)
{
	// ControlBarScheme::init: the bar's own buttons, where the scheme puts them.
	if (!Scheme)
	{
		return;
	}
	auto Add = [this](EGenBarSystem System, const FBox2D& R, bool bEnabled, const FString& Image)
	{
		if (!R.bIsValid)
		{
			return;
		}
		FGenBarButton& B = SystemButtons.AddDefaulted_GetRef();
		B.Rect = ToScreen(R);
		B.System = System;
		B.bEnabled = bEnabled;
		B.Image = Image;
		InputRects.Add(B.Rect);
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
	Add(EGenBarSystem::Options, Scheme->OptionsRect, true, Scheme->Options.Enable);
	Add(EGenBarSystem::IdleWorker, Scheme->WorkerRect, IdleWorkers > 0, IdleWorkers > 0 ? Scheme->IdleWorker.Enable : Scheme->IdleWorker.Disabled);
	Add(EGenBarSystem::Beacon, Scheme->BeaconRect, false, Scheme->Beacon.Disabled);
	Add(EGenBarSystem::Chat, Scheme->ChatRect, false, Scheme->Buddy.Disabled);
	Add(EGenBarSystem::MinMax, Scheme->MinMaxRect, false, Scheme->ToggleDown.Enable);
	MenuButton = Scheme->OptionsRect.bIsValid ? ToScreen(Scheme->OptionsRect) : FBox2D(ForceInit);
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
			B.Rect = WindowRect(TEXT("ButtonCancelConstruction"));
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
				B.Rect = WindowRect(TEXT("OCLTimerSellButton"));
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
		if (!Btn)
		{
			continue;
		}
		FGenBarButton B;
		// ControlBar.wnd's ButtonCommandNN for slot NN (1 and 2 share the first column, 13 and 14 the last).
		B.Rect = WindowRect(*FString::Printf(TEXT("ButtonCommand%02d"), Slot + 1));
		if (!B.Rect.bIsValid)
		{
			continue;
		}
		B.Button = Btn;
		B.Source = Subject;
		B.Thing = FGenGameData::Get().FindTemplate(Btn->Object);
		// getCommandAvailability: "BSTATUS_NO" (and Only_By_AI for a person) - COMMAND_HIDDEN.
		if ((Btn->Command == EGenCommand::UnitBuild || Btn->Command == EGenCommand::DozerConstruct) && B.Thing
			&& (B.Thing->Buildable == 1 || (B.Thing->Buildable == 2 && !Logic->GetPlayers()[Local].bSkirmishAI)))
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
		case EGenCommand::Stop:
			// ("OPTION_ONE ... designates special case handling to hide stop button when not applicable": the
			// strategy center's, there only to stop the bombardment gun.)
			B.bEnabled = !(Btn->CommandOption == 1 && Obj->GetTemplate()->BattlePlan.bValid && Obj->GetActiveBattlePlan() != 1);
			B.Disabled = TEXT("Only with the bombardment plan in force");
			break;
		case EGenCommand::AttackMove:
			B.bEnabled = Obj->CanMove() && Obj->HasWeapons();
			B.Disabled = TEXT("Not available");
			break;
		case EGenCommand::Guard:
		case EGenCommand::GuardWithoutPursuit:
		case EGenCommand::GuardFlyingOnly:
			B.bEnabled = Obj->CanMove();
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
				break;
			}
			if (B.Upgrade->bObjectUpgrade && !Obj->HasPortedUpgradeEffect(*B.Upgrade))
			{
				B.Disabled = TEXT("Not available yet"); // drones, add-ons: what they create isn't in the game so far
				break;
			}
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
	for (int32 i = 1; i <= 9; ++i)
	{
		const FBox2D R = WindowRect(*FString::Printf(TEXT("ButtonQueue%02d"), i));
		if (R.bIsValid)
		{
			QueueSlots.Add(R);
		}
	}
	PortraitRect = WindowRect(TEXT("CameoWindow"));
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
	for (int32 i = 0; i < 5; ++i)
	{
		// ControlBar::populateUpgradeCameos (?): the upgrade's button image, lit once the unit (or its player) has it.
		const FGenUpgradeTemplate* U = UD.FindUpgrade(Obj->GetTemplate()->UpgradeCameos[i]);
		const FBox2D R = WindowRect(*FString::Printf(TEXT("UnitUpgrade%d"), i + 1));
		if (!U || !R.bIsValid)
		{
			continue;
		}
		FGenBarButton& B = UnitUpgrades.AddDefaulted_GetRef();
		B.Rect = R;
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
	GeneralsButton.Rect = Scheme && Scheme->GeneralRect.bIsValid ? ToScreen(Scheme->GeneralRect) : WindowRect(TEXT("ButtonGeneral"));
	GeneralsButton.bGeneralsToggle = true;
	GeneralsButton.bEnabled = true;
	GeneralsButton.Cost = P.SciencePurchasePoints;
	InputRects.Add(GeneralsButton.Rect);
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
			FGenBarButton& B = Sciences.AddDefaulted_GetRef();
			B.Rect = ToScreen(W->Rect);
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
	// The special power shortcut tray (the faction's SpecialPowerShortcutWinName): the powers the player has,
	// in ButtonCommand1.. from the bottom up, each in its ButtonParent. Anchored to the screen's right edge.
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenPlayerTemplate* Faction = Players.IsValidIndex(Local) ? Players[Local].Template : nullptr;
	const FGenCommandSet* Set = Faction ? CD.FindSet(Faction->SpecialPowerShortcutSet) : nullptr;
	const FGenWndLayout* Tray = Faction && !Faction->SpecialPowerShortcutWinName.IsEmpty() ? GenWnd::Load(Faction->SpecialPowerShortcutWinName) : nullptr;
	ShortcutArea = FBox2D(ForceInit);
	if (!Set || !Tray)
	{
		return;
	}
	const FIntPoint Res = Scheme ? Scheme->CreationRes : FIntPoint(800, 600);
	const FVector2D TrayOrigin(ViewSize.X - double(Res.X) * Scale, ViewSize.Y - double(Res.Y) * Scale);
	const int32 MaxButtons = Faction->SpecialPowerShortcutButtonCount > 0 ? Faction->SpecialPowerShortcutButtonCount : 11;
	int32 Next = 1;
	for (int32 i = 0; i < FGenCommandSet::NumSlots && Next <= MaxButtons; ++i)
	{
		const FGenCommandButton* Btn = CD.FindButton(Set->Slots[i]);
		if (!Btn || Btn->Command != EGenCommand::SpecialPower)
		{
			continue;
		}
		FGenBarButton B;
		B.Button = Btn;
		if (!FillPower(Logic, nullptr, B))
		{
			continue;
		}
		const FGenWndWindow* W = Tray->Find(FString::Printf(TEXT("ButtonCommand%d"), Next));
		const FGenWndWindow* Back = Tray->Find(FString::Printf(TEXT("ButtonParent%d"), Next));
		++Next;
		if (!W)
		{
			continue;
		}
		B.Rect = MapRect(W->Rect, TrayOrigin, Scale);
		Shortcuts.Add(MoveTemp(B));
		if (Back)
		{
			const FBox2D BackRect = MapRect(Back->Rect, TrayOrigin, Scale);
			ShortcutTrays.Add(BackRect);
			ShortcutArea += BackRect;
			ShortcutTrayImage = Back->GetImage();
		}
	}
}

bool FGenControlBar::IsOverPanel(const FVector2D& P) const
{
	if (bHasGenerals && bGeneralsOpen && GeneralsPanel.IsInside(P))
	{
		return true;
	}
	if (ShortcutArea.bIsValid && ShortcutArea.IsInside(P))
	{
		return true;
	}
	if (Shortcuts.ContainsByPredicate([&P](const FGenBarButton& B) { return B.Rect.IsInside(P); }))
	{
		return true;
	}
	// The bar: its windows that take the mouse (the frame's blocking parts, the buttons, the radar...).
	return InputRects.ContainsByPredicate([&P](const FBox2D& R) { return R.IsInside(P); });
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
