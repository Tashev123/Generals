#include "Game/GenPlayerController.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Map/GenMapActor.h"
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
	UpdateSelectionInput(MX, MY, bHasMouse);
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
			Selection.Reset();
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
		}
		if (!bBoxSelecting && FVector2D::Distance(LeftStart, FVector2D(MX, MY)) > DragThreshold)
		{
			bBoxSelecting = true;
		}
	}
	else if (bLeftDown)
	{
		bLeftDown = false;
		if (bBoxSelecting)
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
			if (bOwn)
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
