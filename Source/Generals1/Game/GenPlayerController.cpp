#include "Game/GenPlayerController.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapActor.h"
#include "W3D/GenModelComponent.h"
#include "InputCoreTypes.h"

namespace
{
	constexpr float DragThreshold = 6.f;
}

AGenPlayerController::AGenPlayerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
}

void AGenPlayerController::BeginPlay()
{
	Super::BeginPlay();
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
	SetInputMode(Mode);
}

AGenCameraPawn* AGenPlayerController::GetCameraPawn() const
{
	return Cast<AGenCameraPawn>(GetPawn());
}

bool AGenPlayerController::GetDragBox(FVector2D& OutA, FVector2D& OutB) const
{
	if (!bBoxSelecting)
	{
		return false;
	}
	OutA = LeftStart;
	OutB = MouseNow;
	return true;
}

void AGenPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	ControlBar.Layout(UGenGameLogic::Get(this), Selection, FVector2D(ViewX, ViewY));
	if (!bGenInputEnabled)
	{
		return;
	}
	float MX = 0.f, MY = 0.f;
	const bool bHasMouse = GetMousePosition(MX, MY);
	if (bHasMouse)
	{
		MouseNow = FVector2D(MX, MY);
	}
	if (AGenCameraPawn* Cam = GetCameraPawn())
	{
		UpdateCamera(DeltaTime, Cam, MX, MY, bHasMouse);
	}
	UpdatePlacement(MX, MY, bHasMouse);
	UpdateTargeting(MX, MY, bHasMouse);
	UpdateSelectionInput(MX, MY, bHasMouse);
}

// ---- command bar and building placement ---------------------------------------------------------

const FGenThingTemplate* AGenPlayerController::GetPlacement(FVector3f& OutPos, float& OutAngle, bool& bOutLegal) const
{
	if (!PlacingTemplate || !bPlacementValid)
	{
		return nullptr;
	}
	OutPos = PlacementPos;
	OutAngle = PlacingTemplate->PlacementViewAngle;
	bOutLegal = bPlacementLegal;
	return PlacingTemplate;
}

void AGenPlayerController::ExecuteBarButton(const FGenBarButton& Button)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return;
	}
	if (Button.bGeneralsToggle)
	{
		ControlBar.bGeneralsOpen = !ControlBar.bGeneralsOpen;
		return;
	}
	if (Button.QueueIndex != INDEX_NONE)
	{
		Logic->IssueCancelUnit(Button.Source, Button.QueueIndex); // clicking a queued unit or upgrade cancels it
		return;
	}
	if (!Button.bEnabled || !Button.Button)
	{
		return;
	}
	switch (Button.Button->Command)
	{
	case EGenCommand::UnitBuild:
		Logic->IssueQueueUnit(Button.Source, Button.Thing->Name);
		break;
	case EGenCommand::DozerConstruct:
		// Pick the spot next: the building follows the cursor until it is placed or cancelled.
		EndPlacement();
		EndTargeting();
		PlacingTemplate = Button.Thing;
		PlacingDozer = Button.Source;
		break;
	case EGenCommand::Sell:
		Logic->IssueSell(Button.Source);
		break;
	case EGenCommand::Stop:
		Logic->IssueStop(Selection);
		break;
	case EGenCommand::PlayerUpgrade:
	case EGenCommand::ObjectUpgrade:
		if (Button.Upgrade)
		{
			Logic->IssueQueueUpgrade(Button.Source, Button.Upgrade->Name);
		}
		break;
	case EGenCommand::PurchaseScience:
		Logic->IssuePurchaseScience(Logic->GetLocalPlayer(), Button.Science);
		break;
	case EGenCommand::ExitContainer:
		Logic->IssueExit(Button.Rider);
		break;
	case EGenCommand::AttackMove:
	case EGenCommand::Guard:
	case EGenCommand::GuardWithoutPursuit:
	case EGenCommand::GuardFlyingOnly:
		BeginOrderTargeting(Button.Button);
		break;
	case EGenCommand::Evacuate:
		Logic->IssueEvacuate(Button.Source);
		break;
	case EGenCommand::SpecialPower:
		EndPlacement();
		EndTargeting();
		if (Button.Button->bNeedTargetPos || Button.Button->bNeedTargetObject)
		{
			// Pick the target next.
			TargetingButton = Button.Button;
			TargetingSource = Button.Source;
			TargetingPower = Button.Power;
			ControlBar.bGeneralsOpen = false;
		}
		else if (const FGenObject* Source = Logic->FindObject(Button.Source))
		{
			Logic->IssueSpecialPower(Button.Source, Button.Power, Source->GetPosition(), GenInvalidObjectID);
		}
		break;
	default:
		break;
	}
}

// ---- aiming a special power ---------------------------------------------------------------------

const FGenCommandButton* AGenPlayerController::GetTargeting(FVector3f& OutPos, float& OutRadius, bool& bOutValid) const
{
	if (!TargetingButton)
	{
		return nullptr;
	}
	const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(TargetingPower);
	OutPos = TargetingPos;
	OutRadius = FMath::Max(TargetingButton->RadiusCursor, Power ? Power->RadiusCursorRadius : 0.f);
	bOutValid = bTargetingValid;
	return TargetingButton;
}

bool AGenPlayerController::TestClickButton(const FString& CommandButtonName)
{
	auto Find = [&CommandButtonName](const TArray<FGenBarButton>& List)
	{
		return List.FindByPredicate([&CommandButtonName](const FGenBarButton& B)
		{
			return B.Button && B.QueueIndex == INDEX_NONE && B.Button->Name.Equals(CommandButtonName, ESearchCase::IgnoreCase);
		});
	};
	const FGenBarButton* Button = Find(ControlBar.Sciences);
	if (!Button) { Button = Find(ControlBar.Shortcuts); }
	if (!Button) { Button = Find(ControlBar.Buttons); }
	if (!Button || !Button->bEnabled)
	{
		return false;
	}
	const FGenBarButton Copy = *Button;
	ExecuteBarButton(Copy);
	return true;
}

bool AGenPlayerController::TestFireAt(const FVector3f& Pos)
{
	if (!TargetingButton)
	{
		return false;
	}
	TargetingPos = Pos;
	bTargetingValid = true;
	const bool bFired = FireTargetedPower(0.f, 0.f);
	if (bFired)
	{
		EndTargeting();
	}
	return bFired;
}

void AGenPlayerController::BeginOrderTargeting(const FGenCommandButton* Button)
{
	// Pick the spot next (no building fires this: the selection carries it out).
	EndPlacement();
	EndTargeting();
	TargetingButton = Button;
	ControlBar.bGeneralsOpen = false;
}

void AGenPlayerController::EndTargeting()
{
	TargetingButton = nullptr;
	TargetingSource = GenInvalidObjectID;
	TargetingPower.Reset();
	bTargetingValid = false;
}

void AGenPlayerController::UpdateTargeting(float MX, float MY, bool bHasMouse)
{
	if (!TargetingButton)
	{
		return;
	}
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		EndTargeting();
		return;
	}
	if (TargetingPower.IsEmpty())
	{
		if (Selection.Num() == 0)
		{
			EndTargeting(); // an order with nobody left to give it to
			return;
		}
	}
	else
	{
		const FGenObject* Source = Logic->FindObject(TargetingSource);
		if (!Source || Source->IsDead() || Source->GetOwner() != Logic->GetLocalPlayer())
		{
			EndTargeting(); // the building that would fire it is gone
			return;
		}
	}
	bTargetingValid = bHasMouse && !ControlBar.IsOverPanel(FVector2D(MX, MY)) && GetTerrainUnderCursor(MX, MY, TargetingPos);
	if (bTargetingValid)
	{
		TargetingPos.Z = Logic->GetGroundHeight(TargetingPos.X, TargetingPos.Y);
	}
}

bool AGenPlayerController::FireTargetedPower(float MX, float MY)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !TargetingButton)
	{
		return false;
	}
	FGenObjectID Target = GenInvalidObjectID;
	FVector3f Pos = TargetingPos;
	if (TargetingPower.IsEmpty())
	{
		// An order for the selection: attack-move to the spot, or guard the spot / the object clicked.
		if (!bTargetingValid)
		{
			return false;
		}
		if (TargetingButton->Command == EGenCommand::AttackMove)
		{
			Logic->IssueAttackMove(Selection, Pos);
			return true;
		}
		const FGenObjectID Hit = PickObject(MX, MY);
		const FGenObject* HitObj = Hit != GenInvalidObjectID ? Logic->FindObject(Hit) : nullptr;
		const bool bEscort = HitObj && Logic->GetRelationship(Logic->GetLocalPlayer(), HitObj->GetOwner()) != EGenRelationship::Enemies;
		const EGenGuardMode Mode = TargetingButton->Command == EGenCommand::GuardWithoutPursuit ? EGenGuardMode::WithoutPursuit
			: TargetingButton->Command == EGenCommand::GuardFlyingOnly ? EGenGuardMode::FlyingOnly : EGenGuardMode::Normal;
		Logic->IssueGuard(Selection, bEscort ? HitObj->GetPosition() : Pos, bEscort ? Hit : GenInvalidObjectID, Mode);
		return true;
	}
	if (TargetingButton->bNeedTargetObject && !TargetingButton->bNeedTargetPos)
	{
		// A power aimed at something (the cash hack at an enemy supply center).
		Target = PickObject(MX, MY);
		const FGenObject* Victim = Target != GenInvalidObjectID ? Logic->FindObject(Target) : nullptr;
		if (!Victim || Victim->IsDead())
		{
			return false;
		}
		Pos = Victim->GetPosition();
	}
	else if (!bTargetingValid)
	{
		return false;
	}
	Logic->IssueSpecialPower(TargetingSource, TargetingPower, Pos, Target);
	return true;
}

void AGenPlayerController::EndPlacement()
{
	PlacingTemplate = nullptr;
	PlacingDozer = GenInvalidObjectID;
	bPlacementValid = false;
	if (PlacementGhost)
	{
		PlacementGhost->DestroyComponent();
		PlacementGhost = nullptr;
	}
}

void AGenPlayerController::UpdatePlacement(float MX, float MY, bool bHasMouse)
{
	if (!PlacingTemplate)
	{
		return;
	}
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const FGenObject* Dozer = Logic ? Logic->FindObject(PlacingDozer) : nullptr;
	if (!Dozer || Dozer->IsDead() || !Selection.Contains(PlacingDozer) || !Logic->GetMapActor())
	{
		EndPlacement(); // the builder is gone or no longer selected
		return;
	}
	bPlacementValid = bHasMouse && !ControlBar.IsOverPanel(FVector2D(MX, MY)) && GetTerrainUnderCursor(MX, MY, PlacementPos);
	if (bPlacementValid)
	{
		PlacementPos.Z = Logic->GetGroundHeight(PlacementPos.X, PlacementPos.Y);
		bPlacementLegal = Logic->CanMake(Logic->GetLocalPlayer(), *PlacingTemplate)
			&& Logic->IsLocationLegalToBuild(*PlacingTemplate, PlacementPos, PlacingTemplate->PlacementViewAngle);
	}
	// A preview of the finished building under the cursor.
	if (!PlacementGhost)
	{
		const TArray<FGenPlayer>& Players = Logic->GetPlayers();
		const int32 Local = Logic->GetLocalPlayer();
		PlacementGhost = Logic->GetMapActor()->CreateObjectVisual(*PlacingTemplate, Players.IsValidIndex(Local) ? Players[Local].Color : FLinearColor::White);
	}
	if (PlacementGhost)
	{
		PlacementGhost->SetVisibility(bPlacementValid, true);
		PlacementGhost->SetRelativeLocationAndRotation(GenCoords::ToUE(PlacementPos.X, PlacementPos.Y, PlacementPos.Z),
			FRotator(0.f, GenCoords::AngleToYaw(PlacingTemplate->PlacementViewAngle), 0.f));
	}
}

void AGenPlayerController::UpdateCamera(float DeltaTime, AGenCameraPawn* Cam, float MX, float MY, bool bHasMouse)
{
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);

	FVector2D Scroll(0.f, 0.f);
	if (IsInputKeyDown(EKeys::Left)) { Scroll.X -= 1.f; }
	if (IsInputKeyDown(EKeys::Right)) { Scroll.X += 1.f; }
	if (IsInputKeyDown(EKeys::Up)) { Scroll.Y += 1.f; }
	if (IsInputKeyDown(EKeys::Down)) { Scroll.Y -= 1.f; }

	if (bHasMouse && ViewX > 0 && ViewY > 0 && !bRightScrolling && !bBoxSelecting)
	{
		const float Edge = 4.f;
		if (MX <= Edge) { Scroll.X -= 1.f; }
		if (MX >= ViewX - 1 - Edge) { Scroll.X += 1.f; }
		if (MY <= Edge) { Scroll.Y += 1.f; }
		if (MY >= ViewY - 1 - Edge) { Scroll.Y -= 1.f; }
	}
	if (!Scroll.IsZero())
	{
		Cam->Pan(Scroll.GetClampedToMaxSize(1.f) * Cam->ScrollSpeed * DeltaTime * 10.f);
	}

	// Right button: drag scrolls (RMB scroll); a click without dragging clears the selection.
	if (IsInputKeyDown(EKeys::RightMouseButton) && bHasMouse)
	{
		if (!bRightDown)
		{
			bRightDown = true;
			bRightScrolling = false;
			RightStart = FVector2D(MX, MY);
		}
		if (!bRightScrolling && FVector2D::Distance(RightStart, FVector2D(MX, MY)) > DragThreshold * 2.f)
		{
			bRightScrolling = true;
		}
		if (bRightScrolling)
		{
			const FVector2D D = (FVector2D(MX, MY) - RightStart) / FMath::Max(1, FMath::Min(ViewX, ViewY));
			Cam->Pan(FVector2D(D.X * 1.6f, -D.Y * 2.0f) * Cam->ScrollSpeed * DeltaTime * 30.f);
		}
	}
	else if (bRightDown)
	{
		bRightDown = false;
		if (!bRightScrolling)
		{
			// Right click: back out of placing a building or aiming a power, close the promotions,
			// otherwise clear the selection.
			if (PlacingTemplate) { EndPlacement(); }
			else if (TargetingButton) { EndTargeting(); }
			else if (ControlBar.bGeneralsOpen) { ControlBar.bGeneralsOpen = false; }
			else { Selection.Reset(); }
		}
		bRightScrolling = false;
	}

	// Middle-drag rotate
	if (IsInputKeyDown(EKeys::MiddleMouseButton) && bHasMouse)
	{
		if (bMiddleDragging)
		{
			Cam->AddYaw((MX - LastMouseX) * 0.3f);
		}
		bMiddleDragging = true;
		LastMouseX = MX;
	}
	else
	{
		bMiddleDragging = false;
	}

	const float Wheel = GetInputAnalogKeyState(EKeys::MouseWheelAxis);
	if (Wheel != 0.f)
	{
		Cam->Zoom(Wheel);
	}
	if (WasInputKeyJustPressed(EKeys::Home))
	{
		Cam->ResetView();
	}
}

void AGenPlayerController::UpdateSelectionInput(float MX, float MY, bool bHasMouse)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !Logic->IsRunning())
	{
		return;
	}
	const bool bShift = IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);

	if (IsInputKeyDown(EKeys::LeftMouseButton) && bHasMouse)
	{
		if (!bLeftDown)
		{
			bLeftDown = true;
			bBoxSelecting = false;
			LeftStart = FVector2D(MX, MY);
			bLeftOnUI = ControlBar.IsOverPanel(LeftStart);
		}
		// No drag box from the command bar, while placing a building or while aiming a power.
		if (!bBoxSelecting && !bLeftOnUI && !PlacingTemplate && !TargetingButton && FVector2D::Distance(LeftStart, FVector2D(MX, MY)) > DragThreshold)
		{
			bBoxSelecting = true;
		}
	}
	else if (bLeftDown)
	{
		bLeftDown = false;
		if (bLeftOnUI)
		{
			// A click on the command bar never reaches the world.
			if (const FGenBarButton* Button = ControlBar.HitTest(FVector2D(MX, MY)))
			{
				const FGenBarButton Copy = *Button;
				ExecuteBarButton(Copy);
			}
		}
		else if (PlacingTemplate)
		{
			if (bPlacementValid && bPlacementLegal)
			{
				Logic->IssueConstruct(PlacingDozer, PlacingTemplate->Name, PlacementPos, PlacingTemplate->PlacementViewAngle);
				EndPlacement();
			}
		}
		else if (TargetingButton)
		{
			if (FireTargetedPower(MX, MY))
			{
				EndTargeting();
			}
		}
		else if (bBoxSelecting)
		{
			BoxSelect(LeftStart, FVector2D(MX, MY), bShift);
		}
		else
		{
			// A plain left click: on one of our own objects it selects; otherwise, with units selected, it
			// is an order (attack the enemy under the cursor, or move there); with nothing to command it
			// selects whatever was clicked.
			const FGenObjectID Hit = PickObject(MX, MY);
			const FGenObject* HitObj = Hit != GenInvalidObjectID ? Logic->FindObject(Hit) : nullptr;
			const int32 Local = Logic->GetLocalPlayer();
			const bool bOwn = HitObj && HitObj->GetOwner() == Local;
			const bool bCanCommand = Selection.ContainsByPredicate([Logic, Local](FGenObjectID Id)
			{
				const FGenObject* O = Logic->FindObject(Id);
				return O && O->GetOwner() == Local && (O->CanMove() || O->HasWeapons());
			});
			// Dozers of ours in the selection carry on an unfinished building when it is clicked.
			TArray<FGenObjectID> Dozers;
			if (bOwn && HitObj->IsUnderConstruction())
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && O->GetTemplate()->bIsDozer) { Dozers.Add(Id); }
				}
			}
			// Gatherers of ours in the selection go to work when a supply pile, or a supply center they
			// have something for, is clicked.
			TArray<FGenObjectID> Gatherers;
			if (HitObj && HitObj->GetDock())
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && Logic->CanTransferSuppliesAt(*O, *HitObj)) { Gatherers.Add(Id); }
				}
			}
			// Aircraft of ours in the selection land at an airfield of ours when it is clicked.
			TArray<FGenObjectID> Aircraft;
			if (bOwn && HitObj->GetParking() && !HitObj->IsUnderConstruction())
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && O->GetTemplate()->bJetAI) { Aircraft.Add(Id); }
				}
			}
			// Dozers of ours in the selection mend a damaged building when it is clicked; damaged vehicles
			// of ours drive in for repairs when it is a war factory of ours.
			TArray<FGenObjectID> Repairers, Patients;
			if (HitObj)
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (!O || O->GetOwner() != Local) { continue; }
					if (Logic->CanRepairObject(*O, *HitObj)) { Repairers.Add(Id); }
					else if (Logic->CanGetRepairedAt(*O, *HitObj)) { Patients.Add(Id); }
				}
			}
			// With nothing but factories of ours selected, a click on the ground sets their rally point.
			TArray<FGenObjectID> RallyBuildings;
			if (!HitObj)
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && Logic->CanSetRallyPoint(*O))
					{
						RallyBuildings.Add(Id);
					}
					else
					{
						RallyBuildings.Reset();
						break;
					}
				}
			}
			// Units of ours in the selection get into a transport, a garrison, a tunnel or a barracks
			// when it is clicked (ActionManager::canEnterObject).
			TArray<FGenObjectID> Enterers;
			if (HitObj && HitObj->GetContain())
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* O = Logic->FindObject(Id);
					if (O && O->GetOwner() == Local && Logic->CanEnterObject(*O, *HitObj, true)) { Enterers.Add(Id); }
				}
			}
			FVector3f RallyPos;
			if (Gatherers.Num() > 0)
			{
				Logic->IssueDock(Gatherers, Hit);
			}
			else if (Enterers.Num() > 0)
			{
				Logic->IssueEnter(Enterers, Hit);
			}
			else if (Repairers.Num() > 0 && !HitObj->IsUnderConstruction())
			{
				Logic->IssueRepair(Repairers, Hit);
			}
			else if (Patients.Num() > 0)
			{
				Logic->IssueGetRepaired(Patients, Hit);
			}
			else if (RallyBuildings.Num() > 0 && GetTerrainUnderCursor(MX, MY, RallyPos))
			{
				Logic->IssueSetRallyPoint(RallyBuildings, RallyPos);
			}
			else if (Aircraft.Num() > 0)
			{
				Logic->IssueLand(Aircraft, Hit);
			}
			else if (Dozers.Num() > 0)
			{
				Logic->IssueResumeConstruction(Dozers, Hit);
			}
			else if (bOwn)
			{
				if (!bShift)
				{
					Selection.Reset();
				}
				if (bShift && Selection.Contains(Hit)) { Selection.Remove(Hit); }
				else { Selection.AddUnique(Hit); }
			}
			else if (bCanCommand)
			{
				CommandAt(MX, MY);
			}
			else
			{
				Selection.Reset();
				if (Hit != GenInvalidObjectID)
				{
					Selection.Add(Hit);
				}
			}
		}
		bBoxSelecting = false;
	}

	if (WasInputKeyJustPressed(EKeys::S) && Selection.Num() > 0)
	{
		Logic->IssueStop(Selection);
	}
	// A: attack-move, G: guard (then a left click says where).
	const bool bHasMovers = Selection.ContainsByPredicate([Logic](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return O && O->GetOwner() == Logic->GetLocalPlayer() && O->CanMove();
	});
	if (bHasMovers && WasInputKeyJustPressed(EKeys::A))
	{
		BeginOrderTargeting(FGenCommandData::Get().FindButton(TEXT("Command_AttackMove")));
	}
	if (bHasMovers && WasInputKeyJustPressed(EKeys::G))
	{
		BeginOrderTargeting(FGenCommandData::Get().FindButton(TEXT("Command_Guard")));
	}

	// Drop objects that died or no longer exist.
	Selection.RemoveAll([Logic](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return O == nullptr || !O->IsSelectable();
	});
}

FGenObjectID AGenPlayerController::PickObject(float MX, float MY) const
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FVector Origin, Dir;
	if (!Logic || !DeprojectScreenPositionToWorld(MX, MY, Origin, Dir))
	{
		return GenInvalidObjectID;
	}
	FGenObjectID Best = GenInvalidObjectID;
	double BestT = TNumericLimits<double>::Max();
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || !O->IsSelectable())
		{
			continue;
		}
		const FGenThingTemplate* T = O->GetTemplate();
		const FVector3f P = Logic->GetRenderPosition(*O);
		const float HalfH = FMath::Max(T->GeometryHeight, 6.f) * 0.5f;
		const FVector Center = GenCoords::ToUE(P.X, P.Y, P.Z + HalfH);
		const double R = FMath::Max(T->GetBoundingCircleRadius(), HalfH) * GenCoords::UnitsToCm;
		// Ray vs. sphere
		const FVector L = Center - Origin;
		const double Tca = FVector::DotProduct(L, Dir);
		const double D2 = L.SizeSquared() - Tca * Tca;
		if (Tca > 0.0 && D2 <= R * R && Tca < BestT)
		{
			BestT = Tca;
			Best = Id;
		}
	}
	return Best;
}

void AGenPlayerController::BoxSelect(const FVector2D& A, const FVector2D& B, bool bAdd)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return;
	}
	const FBox2D Box(FVector2D(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)), FVector2D(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y)));
	if (!bAdd)
	{
		Selection.Reset();
	}
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		// Drag selection takes the local player's mobile units (not structures).
		if (!O || !O->IsSelectable() || O->GetOwner() != Logic->GetLocalPlayer() || !O->CanMove())
		{
			continue;
		}
		const FVector3f P = Logic->GetRenderPosition(*O);
		FVector2D S;
		if (ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z), S) && Box.IsInside(S))
		{
			Selection.AddUnique(Id);
		}
	}
}

bool AGenPlayerController::GetTerrainUnderCursor(float MX, float MY, FVector3f& OutGenPos) const
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FVector Origin, Dir;
	if (!Logic || !Logic->GetMapActor() || !DeprojectScreenPositionToWorld(MX, MY, Origin, Dir))
	{
		return false;
	}
	FVector Hit;
	if (!Logic->GetMapActor()->RaycastTerrain(Origin, Dir, Hit))
	{
		return false;
	}
	const FVector Gen = GenCoords::FromUE(Hit);
	OutGenPos = FVector3f(Gen);
	return true;
}

void AGenPlayerController::CommandAt(float MX, float MY)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || Selection.Num() == 0)
	{
		return;
	}
	TArray<FGenObjectID> Movers;
	TArray<FGenObjectID> Shooters;
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && O->GetOwner() == Logic->GetLocalPlayer())
		{
			if (O->CanMove()) { Movers.Add(Id); }
			if (O->HasWeapons()) { Shooters.Add(Id); }
		}
	}

	// Click on an enemy: attack it.
	if (Shooters.Num() > 0)
	{
		const FGenObjectID Hit = PickObject(MX, MY);
		const FGenObject* Victim = Hit != GenInvalidObjectID ? Logic->FindObject(Hit) : nullptr;
		if (Victim && Victim->IsTargetable() && Logic->GetRelationship(Logic->GetLocalPlayer(), Victim->GetOwner()) == EGenRelationship::Enemies)
		{
			Logic->IssueAttack(Shooters, Hit);
			return;
		}
	}

	FVector3f Goal;
	if (Movers.Num() > 0 && GetTerrainUnderCursor(MX, MY, Goal))
	{
		Logic->IssueMove(Movers, Goal);
	}
}
