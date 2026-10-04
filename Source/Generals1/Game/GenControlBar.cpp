#include "Game/GenControlBar.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"

void FGenControlBar::Layout(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection, const FVector2D& ViewSize)
{
	bVisible = false;
	bHasGenerals = false;
	Subject = GenInvalidObjectID;
	Status.Reset();
	Buttons.Reset();
	Queue.Reset();
	Sciences.Reset();
	Shortcuts.Reset();
	if (!Logic || !Logic->IsRunning() || ViewSize.X <= 0.0 || ViewSize.Y <= 0.0)
	{
		return;
	}
	Scale = float(FMath::Clamp(ViewSize.Y / 1080.0, 0.6, 2.0));
	LayoutGenerals(Logic, ViewSize);
	LayoutShortcuts(Logic, ViewSize);
	LayoutSelection(Logic, Selection, ViewSize);
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
	return true;
}

void FGenControlBar::LayoutSelection(UGenGameLogic* Logic, const TArray<FGenObjectID>& Selection, const FVector2D& ViewSize)
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
		const FGenCommandSet* S = (O && O->GetOwner() == Local && !O->IsDead()) ? CD.FindSet(O->GetCommandSet()) : nullptr;
		if (!S && O && O->GetOwner() == Local && !O->IsDead() && O->GetContain() && Logic->GetRiders(*O).Num() > 0)
		{
			S = &InventorySet;
		}
		if (S)
		{
			Obj = O;
			Set = S;
			break;
		}
	}
	if (!Obj)
	{
		return;
	}
	bVisible = true;
	Subject = Obj->GetID();

	constexpr int32 Cols = 7, Rows = 2;
	const double BW = 64.0 * Scale, BH = 52.0 * Scale, Gap = 4.0 * Scale, QW = 40.0 * Scale, QH = 32.0 * Scale;
	const bool bShowQueue = Obj->GetTemplate()->bHasProduction;
	const double PanelW = Cols * BW + (Cols + 1) * Gap;
	const double PanelH = Rows * BH + (Rows + 1) * Gap + (bShowQueue ? QH + Gap : 0.0);
	const FVector2D Origin((ViewSize.X - PanelW) * 0.5, ViewSize.Y - PanelH - 6.0 * Scale);
	Panel = FBox2D(Origin, Origin + FVector2D(PanelW, PanelH));
	const double GridY = Origin.Y + Gap + (bShowQueue ? QH + Gap : 0.0);

	if (Obj->IsBeingSold())
	{
		Status = TEXT("Being sold");
		return;
	}
	if (Obj->IsUnderConstruction())
	{
		Status = FString::Printf(TEXT("Under construction %d%%"), FMath::FloorToInt32(Obj->GetConstructionPercent()));
		return;
	}
	if (Obj->IsDisabled())
	{
		Status = TEXT("Disabled");
		return;
	}

	const bool bQueueFull = Obj->GetProductionQueue().Num() >= Obj->GetTemplate()->MaxQueueEntries;
	const TArray<FGenObjectID>& Riders = Logic->GetRiders(*Obj);
	int32 NextRider = 0;
	for (int32 Slot = 0; Slot < Cols * Rows; ++Slot)
	{
		const FGenCommandButton* Btn = CD.FindButton(Set->Slots[Slot]);
		if (!Btn)
		{
			continue;
		}
		FGenBarButton B;
		// Slots run down the columns: 1 and 2 share the first column, 13 and 14 the last.
		const int32 Col = Slot / Rows, Row = Slot % Rows;
		const FVector2D Min(Origin.X + Gap + Col * (BW + Gap), GridY + Row * (BH + Gap));
		B.Rect = FBox2D(Min, Min + FVector2D(BW, BH));
		B.Button = Btn;
		B.Source = Subject;
		B.Thing = FGenGameData::Get().FindTemplate(Btn->Object);
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
			B.bEnabled = true;
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
			FGenBarButton& Q = Queue.AddDefaulted_GetRef();
			const FVector2D Min(Origin.X + Gap + i * (QW + Gap), Origin.Y + Gap);
			Q.Rect = FBox2D(Min, Min + FVector2D(QW, QH));
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

void FGenControlBar::LayoutGenerals(UGenGameLogic* Logic, const FVector2D& ViewSize)
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

	// The button sits under the money and power read-out (top left).
	const double EScale = FMath::Clamp(ViewSize.Y / 1080.0, 0.75, 2.0) * 1.4;
	const FVector2D ButtonMin(8.0, 8.0 + 44.0 * EScale + 6.0);
	GeneralsButton = FGenBarButton();
	GeneralsButton.Rect = FBox2D(ButtonMin, ButtonMin + FVector2D(190.0 * EScale, 34.0 * EScale));
	GeneralsButton.bGeneralsToggle = true;
	GeneralsButton.bEnabled = true;
	GeneralsButton.Cost = P.SciencePurchasePoints;
	if (!bGeneralsOpen)
	{
		return;
	}

	// Three rows of promotions: what rank 1, rank 3 and rank 5 unlock.
	const double BW = 64.0 * Scale, BH = 52.0 * Scale, Gap = 6.0 * Scale, LabelH = 22.0 * Scale, Margin = 14.0 * Scale;
	constexpr int32 PerRow = 9;
	int32 Counts[3] = { 0, 0, 0 };
	const FGenCommandSet* Sets[3] = { nullptr, nullptr, nullptr };
	double Height = Margin + 34.0 * Scale; // title
	for (int32 s = 0; s < 3; ++s)
	{
		Sets[s] = CD.FindSet(P.Template->PurchaseScienceSets[s]);
		for (int32 i = 0; Sets[s] && i < FGenCommandSet::NumSlots; ++i)
		{
			const FGenCommandButton* Btn = CD.FindButton(Sets[s]->Slots[i]);
			Counts[s] += (Btn && Btn->Command == EGenCommand::PurchaseScience && Btn->Sciences.Num() > 0) ? 1 : 0;
		}
		if (Counts[s] > 0)
		{
			Height += LabelH + ((Counts[s] + PerRow - 1) / PerRow) * (BH + Gap) + Gap;
		}
	}
	Height += Margin;
	const double Width = 2.0 * Margin + PerRow * BW + (PerRow - 1) * Gap;
	const FVector2D Origin((ViewSize.X - Width) * 0.5, FMath::Max(8.0, (ViewSize.Y - Height) * 0.4));
	GeneralsPanel = FBox2D(Origin, Origin + FVector2D(Width, Height));

	double Y = Origin.Y + Margin + 34.0 * Scale;
	for (int32 s = 0; s < 3; ++s)
	{
		SectionLabels[s] = FVector2D(Origin.X + Margin, Y);
		if (Counts[s] == 0)
		{
			continue;
		}
		Y += LabelH;
		int32 n = 0;
		for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
		{
			const FGenCommandButton* Btn = CD.FindButton(Sets[s]->Slots[i]);
			if (!Btn || Btn->Command != EGenCommand::PurchaseScience || Btn->Sciences.Num() == 0)
			{
				continue;
			}
			FGenBarButton& B = Sciences.AddDefaulted_GetRef();
			const FVector2D Min(Origin.X + Margin + (n % PerRow) * (BW + Gap), Y + (n / PerRow) * (BH + Gap));
			++n;
			B.Rect = FBox2D(Min, Min + FVector2D(BW, BH));
			B.Button = Btn;
			B.Science = Btn->Sciences[0];
			const FGenScience* S = UD.FindScience(B.Science);
			B.Cost = S ? S->PurchaseCost : 0;
			B.bOwned = Logic->HasScience(Local, B.Science);
			B.bEnabled = !B.bOwned && Logic->CanPurchaseScience(Local, B.Science, &B.Disabled);
			if (B.bOwned)
			{
				B.Disabled = TEXT("Acquired");
			}
		}
		Y += ((Counts[s] + PerRow - 1) / PerRow) * (BH + Gap) + Gap;
	}
}

void FGenControlBar::LayoutShortcuts(UGenGameLogic* Logic, const FVector2D& ViewSize)
{
	// ControlBar's special power shortcut bar: the powers the player has, down the right edge.
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenCommandSet* Set = (Players.IsValidIndex(Local) && Players[Local].Template) ? CD.FindSet(Players[Local].Template->SpecialPowerShortcutSet) : nullptr;
	if (!Set)
	{
		return;
	}
	for (int32 i = 0; i < FGenCommandSet::NumSlots; ++i)
	{
		const FGenCommandButton* Btn = CD.FindButton(Set->Slots[i]);
		if (!Btn || Btn->Command != EGenCommand::SpecialPower)
		{
			continue;
		}
		FGenBarButton B;
		B.Button = Btn;
		if (FillPower(Logic, nullptr, B))
		{
			Shortcuts.Add(MoveTemp(B));
		}
	}
	const double BW = 58.0 * Scale, BH = 47.0 * Scale, Gap = 4.0 * Scale;
	const double Total = Shortcuts.Num() * (BH + Gap);
	double Y = FMath::Max(8.0, (ViewSize.Y - Total) * 0.4);
	for (FGenBarButton& B : Shortcuts)
	{
		const FVector2D Min(ViewSize.X - BW - 8.0, Y);
		B.Rect = FBox2D(Min, Min + FVector2D(BW, BH));
		Y += BH + Gap;
	}
}

bool FGenControlBar::IsOverPanel(const FVector2D& P) const
{
	if ((bVisible && Panel.IsInside(P)) || (bHasGenerals && (GeneralsButton.Rect.IsInside(P) || (bGeneralsOpen && GeneralsPanel.IsInside(P)))))
	{
		return true;
	}
	return Shortcuts.ContainsByPredicate([&P](const FGenBarButton& B) { return B.Rect.IsInside(P); });
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
		}
		if (GeneralsButton.Rect.IsInside(P)) { return &GeneralsButton; }
	}
	for (const FGenBarButton& B : Shortcuts)
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
