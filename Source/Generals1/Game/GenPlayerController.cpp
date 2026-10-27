#include "Game/GenPlayerController.h"
#include "Game/GenGameMode.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Game/GenRadar.h"
#include "Audio/GenAudioData.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapActor.h"
#include "W3D/GenModelComponent.h"
#include "Game/GenCursors.h"
#include "Game/GenHUD.h"
#include "Game/GenInput.h"
#include "Logic/GenSkirmishAI.h"
#include "W3D/GenAssets.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Generals1.h"
#include "InputCoreTypes.h"
#include "Game/GenSettings.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/ICursor.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h" // (RECT, for ICursor::Lock)
#endif
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

static TAutoConsoleVariable<float> CVarGenPickSlack(
	TEXT("gen.PickSlack"), 3.f,
	TEXT("If the ray under the cursor meets no model, rays this many pixels around it (at 1080 lines) are tried. 0: the one ray only."));

namespace
{
	constexpr float DragThreshold = 6.f;

	/** W3DModelDraw (the render object's collision type): the kind of pick that lands on it - the last of these that applies. */
	uint32 PickTypeOf(const FGenObject& O)
	{
		const FGenThingTemplate& T = *O.GetTemplate();
		// (The template's SELECTABLE, not whether it may be selected now: a drone or a salvage crate can't be
		// selected, but is there to be clicked on - an enemy's drone to be shot at, a crate to be driven to.)
		uint32 Type = T.IsKindOf(TEXT("SELECTABLE")) && !O.IsContained() ? uint32(AGenPlayerController::PickSelectable) : 0u;
		if (T.IsKindOf(TEXT("SHRUBBERY"))) { Type = AGenPlayerController::PickShrubbery; }
		if (T.IsKindOf(TEXT("MINE"))) { Type = AGenPlayerController::PickMines; }
		if (T.IsKindOf(TEXT("FORCEATTACKABLE"))) { Type = AGenPlayerController::PickForceAttackable; }
		if (T.IsKindOf(TEXT("CLICK_THROUGH"))) { Type = 0; }
		// "A dead building -- don't allow the user to click on rubble! Treat it as a location instead." (And hulks.)
		if (Type != 0 && !T.IsKindOf(TEXT("BRIDGE")) && !T.IsKindOf(TEXT("BRIDGE_TOWER"))
			&& (O.IsDead() || (T.IsKindOf(TEXT("STRUCTURE")) && O.GetModelConditions().Test(GenConditions::Rubble()))))
		{
			Type = 0;
		}
		return Type;
	}
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
	// (Held in the window only while a game is on, and if the player wants it: UpdateMouseConfine.)
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(Mode);
}

void AGenPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The level is loaded anew between a game and the menu: the mouse goes free with this one.
	UpdateMouseConfine(false);
	Super::EndPlay(EndPlayReason);
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
	// GameLogic::selectObject: what the logic wants selected (a gunship that has just been sent for).
	if (UGenGameLogic* SelectLogic = UGenGameLogic::Get(this))
	{
		TArray<FGenObjectID> Asked;
		SelectLogic->TakeSelectRequests(Asked);
		if (Asked.Num() > 0)
		{
			Selection = MoveTemp(Asked);
			for (FGenObjectID Id : Selection)
			{
				SelectLogic->FlashSelected(Id);
			}
		}
	}
	ControlBar.Layout(UGenGameLogic::Get(this), Selection, FVector2D(ViewX, ViewY));
	// The controls page's mouse: held in the window, and the pointer's own speed - while a game is on.
	const bool bPlaying = IsPlayingWithMouse();
	UpdateMouseConfine(bPlaying);
	UpdatePointerSpeed(bPlaying);
	if (!bGenInputEnabled)
	{
		if (bTestCursor)
		{
			UpdateCursor(float(TestCursorPos.X), float(TestCursorPos.Y), true); // (its animation goes on)
		}
		UpdateRadiusDecals();
		return;
	}
	float MX = 0.f, MY = 0.f;
	const bool bHasMouse = GetMousePosition(MX, MY);
	if (bHasMouse)
	{
		MouseNow = FVector2D(MX, MY);
	}
	// The menus have the mouse while they are up (the skirmish menu, the score screen, the in-game menu).
	AGenGameMode* GM = GetWorld()->GetAuthGameMode<AGenGameMode>();
	// (While the options are up, Esc is theirs: it closes them, or stops waiting for a key.)
	const AGenHUD* GenHUD = Cast<AGenHUD>(GetHUD());
	if (GM && !(GenHUD && GenHUD->IsOptionsOpen()) && (WasInputKeyJustPressed(EKeys::Escape) || FGenInput::Get().JustPressed(this, EGenAction::Menu)))
	{
		GM->ToggleQuitMenu();
	}
	// The pause hotkey on its own, with no menu up (while the menu is up it is already standing still).
	if (GM && !GM->IsShellActive() && !(GenHUD && GenHUD->IsOptionsOpen()) && FGenInput::Get().JustPressed(this, EGenAction::Pause))
	{
		GM->TogglePause();
	}
	if (GM && GM->IsShellActive())
	{
		bLeftDown = bBoxSelecting = bRightDown = bRightScrolling = bMiddleDragging = false;
		MousedOver = GenInvalidObjectID;
		ScrollScreen = FVector2D::ZeroVector;
		UpdateCursor(MX, MY, bHasMouse);
		UpdateRadiusDecals();
		return;
	}
	if (AGenCameraPawn* Cam = GetCameraPawn())
	{
		UpdateCamera(DeltaTime, Cam, MX, MY, bHasMouse);
	}
	UpdatePlacement(MX, MY, bHasMouse);
	UpdateTargeting(MX, MY, bHasMouse);
	UpdateSelectionInput(MX, MY, bHasMouse);
	UpdateMouseOver(MX, MY, bHasMouse);
	UpdateCursor(MX, MY, bHasMouse);
	UpdateRadiusDecals();
}

// ---- pictures on the ground ---------------------------------------------------------------------

const FGenRadiusDecalTemplate* AGenPlayerController::GetRadiusCursor(float& OutRadius) const
{
	// InGameUI::setRadiusCursor: the pending command's RadiusCursorType, its size by the kind - a weapon's
	// areas and the guard range by the first of the selection (or the building a shortcut power is fired
	// from), everything else the power's RadiusCursorRadius.
	OutRadius = 0.f;
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const FGenRadiusDecalTemplate* T = (Logic && TargetingButton) ? FGenCommandData::Get().FindRadiusCursor(TargetingButton->RadiusCursorType) : nullptr;
	if (!T)
	{
		return nullptr;
	}
	const FGenObject* Obj = TargetingSource != GenInvalidObjectID ? Logic->FindObject(TargetingSource) : nullptr;
	for (int32 i = 0; !Obj && i < Selection.Num(); ++i)
	{
		Obj = Logic->FindObject(Selection[i]);
	}
	const FString& Type = TargetingButton->RadiusCursorType;
	const int32 Slot = (TargetingButton->WeaponSlot >= 0 && TargetingButton->WeaponSlot < 3) ? TargetingButton->WeaponSlot : 0;
	const FGenWeaponTemplate* W = Obj ? Obj->GetWeapon(Slot).Template : nullptr;
	if (Type == TEXT("ATTACK_DAMAGE_AREA"))
	{
		OutRadius = W ? W->PrimaryDamageRadius : 0.f;
	}
	else if (Type == TEXT("ATTACK_SCATTER_AREA"))
	{
		OutRadius = W ? W->ScatterRadius + W->ScatterTargetScalar : 0.f;
	}
	else if (Type == TEXT("ATTACK_CONTINUE_AREA") || Type == TEXT("CLEARMINES"))
	{
		OutRadius = W ? W->ContinueAttackRange : 0.f;
	}
	else if (Type == TEXT("GUARD_AREA"))
	{
		OutRadius = Obj ? Obj->GetVisionRange() * FGenAIData::Get().GuardInnerModifierHuman : 0.f; // AIGuardMachine::getStdGuardRange
	}
	else
	{
		const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(TargetingPower);
		OutRadius = Power ? Power->RadiusCursorRadius : 0.f;
	}
	return OutRadius > 0.f ? T : nullptr;
}

void AGenPlayerController::UpdateRadiusDecals()
{
	int32 Used = 0;
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const AGenGameMode* GM = GetWorld()->GetAuthGameMode<AGenGameMode>();
	if (Logic && Logic->IsRunning() && !(GM && GM->IsShellActive()))
	{
		const int32 Local = Logic->GetLocalPlayer();
		const FLinearColor PlayerColor = Logic->GetPlayers().IsValidIndex(Local) ? Logic->GetPlayers()[Local].Color : FLinearColor::White;
		// (RadiusDecal::update throbs by the logic's frame: it stands still while the game is paused.)
		const double Seconds = Logic->GetRenderFrameTime() / double(GenLogic::GetLogicFPS());
		float Radius = 0.f;
		const FGenRadiusDecalTemplate* Cursor = bTargetingValid ? GetRadiusCursor(Radius) : nullptr;
		if (Cursor)
		{
			ShowRadiusDecal(Used++, *Cursor, TargetingPos, Radius, PlayerColor, Seconds);
		}
		// DeliverPayloadAIUpdate's DeliveryDecal: where a strike of ours is going in, till the aircraft heads home.
		for (FGenObjectID Id : Logic->GetDeliveryMarkers())
		{
			const FGenObject* Carrier = Logic->FindObject(Id);
			const FGenRadiusDecalTemplate* Marker = nullptr;
			FVector3f Where;
			float MarkerRadius = 0.f;
			if (Carrier && Carrier->GetOwner() == Local && Carrier->GetDeliveryMarker(Where, MarkerRadius, Marker))
			{
				ShowRadiusDecal(Used++, *Marker, Where, MarkerRadius, PlayerColor, Seconds);
			}
		}
		// SpectreGunshipUpdate's AttackAreaDecal and TargetingReticleDecal ("OnlyVisibleToOwningPlayer").
		for (FGenObjectID Id : Logic->GetGunships())
		{
			const FGenObject* Ship = Logic->FindObject(Id);
			FVector3f Area, Reticle;
			float AreaRadius = 0.f, ReticleRadius = 0.f;
			if (Ship && Ship->GetOwner() == Local && Ship->GetGunshipTargets(Area, AreaRadius, Reticle, ReticleRadius))
			{
				const FGenSpectreGunship& G = Ship->GetTemplate()->Gunship;
				if (G.AttackAreaDecal.IsValid())
				{
					ShowRadiusDecal(Used++, G.AttackAreaDecal, Area, AreaRadius, PlayerColor, Seconds);
				}
				if (G.TargetingReticleDecal.IsValid())
				{
					ShowRadiusDecal(Used++, G.TargetingReticleDecal, Reticle, ReticleRadius, PlayerColor, Seconds);
				}
			}
		}
	}
	for (int32 i = Used; i < RadiusDecals.Num(); ++i)
	{
		if (IsValid(RadiusDecals[i]) && RadiusDecals[i]->IsVisible())
		{
			RadiusDecals[i]->SetVisibility(false);
		}
	}
}

void AGenPlayerController::ShowRadiusDecal(int32 Index, const FGenRadiusDecalTemplate& T, const FVector3f& Pos, float Radius, const FLinearColor& PlayerColor, double Seconds)
{
	// RadiusDecalTemplate::createRadiusDecal: a square of the radius's size each way, projected down onto
	// the ground; RadiusDecal::update: its opacity throbbing.
	if (!RadiusDecalBase)
	{
		RadiusDecalBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Generals/Materials/M_RadiusDecal.M_RadiusDecal"));
		if (!RadiusDecalBase)
		{
			return;
		}
	}
	// (On the map's actor: a controller is a hidden actor, and what it owns is never drawn. A new map, a new set.)
	const UGenGameLogic* OwnerLogic = UGenGameLogic::Get(this);
	AActor* MapOwner = OwnerLogic ? static_cast<AActor*>(OwnerLogic->GetMapActor()) : nullptr;
	if (!MapOwner)
	{
		return;
	}
	if (RadiusDecalOwner.Get() != MapOwner)
	{
		for (UDecalComponent* Old : RadiusDecals)
		{
			if (IsValid(Old)) { Old->DestroyComponent(); }
		}
		RadiusDecals.Reset();
		RadiusDecalMaterials.Reset();
		RadiusDecalShown.Reset();
		RadiusDecalOwner = MapOwner;
	}
	while (RadiusDecals.Num() <= Index)
	{
		UDecalComponent* D = NewObject<UDecalComponent>(MapOwner, NAME_None, RF_Transient);
		D->SetUsingAbsoluteLocation(true);
		D->SetUsingAbsoluteRotation(true);
		D->SetUsingAbsoluteScale(true);
		D->SetFadeScreenSize(0.f); // (seen from the furthest the view goes)
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(RadiusDecalBase, this);
		D->SetDecalMaterial(M);
		D->SetVisibility(false);
		D->RegisterComponent();
		RadiusDecals.Add(D);
		RadiusDecalMaterials.Add(M);
		RadiusDecalShown.Add(nullptr);
	}
	UDecalComponent* D = RadiusDecals[Index];
	UMaterialInstanceDynamic* M = RadiusDecalMaterials[Index];
	if (RadiusDecalShown[Index] != &T)
	{
		RadiusDecalShown[Index] = &T;
		const UGenGameLogic* Logic = UGenGameLogic::Get(this);
		UE_CLOG(Logic && Logic->IsTracing(), LogGenerals, Log, TEXT("  [decal] %d: %s, radius %.0f at (%.0f, %.0f)"), Index, *T.Texture, Radius, Pos.X, Pos.Y);
		if (UTexture2D* Tex = UGenAssetSubsystem::Get()->GetTexture(T.Texture))
		{
			M->SetTextureParameterValue(TEXT("DecalTexture"), Tex);
		}
	}
	// (The colour multiplies the texture as it did on screen: both are taken out of sRGB alike.)
	M->SetVectorParameterValue(TEXT("Color"), T.bPlayerColor ? PlayerColor : FLinearColor(T.Color));
	M->SetScalarParameterValue(TEXT("Opacity"), T.GetOpacity(Seconds));
	// Deep enough to reach the ground over the whole of it, a slope included.
	const FVector Size(double(Radius + 150.f) * GenCoords::UnitsToCm, double(Radius) * GenCoords::UnitsToCm, double(Radius) * GenCoords::UnitsToCm);
	if (!D->DecalSize.Equals(Size))
	{
		D->DecalSize = Size;
		D->MarkRenderStateDirty();
	}
	D->SetWorldLocationAndRotation(GenCoords::ToUE(Pos.X, Pos.Y, Pos.Z), FRotator(-90.f, 0.f, 0.f));
	if (!D->IsVisible())
	{
		D->SetVisibility(true);
	}
}

// ---- command bar and building placement ---------------------------------------------------------

const FGenThingTemplate* AGenPlayerController::GetPlacement(FVector3f& OutPos, float& OutAngle, bool& bOutLegal) const
{
	if (!PlacingTemplate || !bPlacementValid)
	{
		return nullptr;
	}
	OutPos = PlacementPos;
	OutAngle = PlacementAngle;
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
	if (Button.System != EGenBarSystem::None)
	{
		switch (Button.System)
		{
		case EGenBarSystem::Options:
			if (AGenGameMode* GameMode = GetWorld()->GetAuthGameMode<AGenGameMode>())
			{
				GameMode->ToggleQuitMenu();
			}
			break;
		case EGenBarSystem::GeneralsExit:
			ControlBar.bGeneralsOpen = false;
			break;
		case EGenBarSystem::IdleWorker:
			if (Button.bEnabled)
			{
				SelectNextIdleWorker();
			}
			break;
		default:
			break; // (beacons, the communicator and the bar's minimising aren't in the port)
		}
		if (Button.bEnabled)
		{
			Logic->PlayMiscSound(TEXT("GUIClickSound"), Logic->GetLocalPlayer());
		}
		return;
	}
	if (Button.QueueIndex != INDEX_NONE)
	{
		Logic->IssueCancelUnit(Button.Source, Button.QueueIndex); // clicking a queued unit or upgrade cancels it
		return;
	}
	if (!Button.bEnabled || !Button.Button)
	{
		// ControlBar::processCommandUI: what can't be afforded gets the adviser's comment, anything else a buzz.
		if (Button.Disabled == TEXT("Not enough money"))
		{
			Logic->EvaEvent(TEXT("InsufficientFunds"), Logic->GetLocalPlayer());
		}
		else if (Button.Button && !Button.bOwned)
		{
			Logic->PlayMiscSound(TEXT("NoCanDoSound"), Logic->GetLocalPlayer());
		}
		return;
	}
	Logic->PlayMiscSound(TEXT("GUIClickSound"), Logic->GetLocalPlayer());
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
		PlacementAngle = PlacingTemplate ? PlacingTemplate->PlacementViewAngle : 0.f;
		bPlacementAnchored = false;
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
	case EGenCommand::CombatDrop:
		BeginOrderTargeting(Button.Button);
		break;
	case EGenCommand::Evacuate:
		Logic->IssueEvacuate(Button.Source);
		PlayVoice({ TEXT("VoiceUnload") }, { Button.Source });
		break;
	case EGenCommand::SpecialPower:
		EndPlacement();
		EndTargeting();
		if (Button.Button->bConstruct)
		{
			// SPECIAL_POWER_CONSTRUCT (the sneak attack): the power puts a building down, and where is
			// picked like a building's site - the building follows the cursor.
			if (const FGenThingTemplate* What = FGenGameData::Get().FindTemplate(Button.Button->Object))
			{
				PlacingTemplate = What;
				PlacingDozer = GenInvalidObjectID;
				PlacingPower = Button.Power;
				PlacingPowerSource = Button.Source;
				PlacementAngle = What->PlacementViewAngle;
				bPlacementAnchored = false;
				ControlBar.bGeneralsOpen = false;
			}
		}
		else if (const FGenObject* Center = Button.Button->CommandOption != 0 ? Logic->FindObject(Button.Source) : nullptr;
			Center && Center->GetTemplate()->BattlePlan.bValid)
		{
			// The strategy center's plan buttons (CHECK_LIKE OPTION_ONE / TWO / THREE): the plan to unpack next.
			Logic->IssueBattlePlan(Button.Source, Button.Button->CommandOption);
		}
		else if (Button.Button->bNeedTargetPos || Button.Button->bNeedTargetObject)
		{
			// Pick the target next.
			TargetingButton = Button.Button;
			TargetingSource = Button.Source;
			TargetingPower = Button.Power;
			ControlBar.bGeneralsOpen = false;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] aiming %s (%s) from #%u"), Logic->GetFrame(), *TargetingPower, *Button.Button->Name, TargetingSource);
		}
		else if (const FGenObject* Source = Logic->FindObject(Button.Source))
		{
			// A unit's own ability (OK_FOR_MULTI_SELECT): every selected unit that has it does it (Burton's detonator).
			const int32 Module = Logic->FindPowerModule(*Source, Button.Power);
			if (Module != INDEX_NONE && Source->GetTemplate()->PowerModules[Module].Type == EGenPowerModule::SpecialAbility && Selection.Contains(Button.Source))
			{
				for (FGenObjectID Id : Selection)
				{
					const FGenObject* U = Logic->FindObject(Id);
					if (U && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *U) && Logic->FindPowerModule(*U, Button.Power) != INDEX_NONE)
					{
						Logic->IssueSpecialPower(Id, Button.Power, U->GetPosition(), GenInvalidObjectID);
					}
				}
			}
			else
			{
				Logic->IssueSpecialPower(Button.Source, Button.Power, Source->GetPosition(), GenInvalidObjectID);
			}
		}
		break;
	case EGenCommand::HijackVehicle:
	case EGenCommand::ConvertToCarBomb:
	case EGenCommand::SabotageBuilding:
		BeginOrderTargeting(Button.Button); // which vehicle (or building)
		break;
	case EGenCommand::HackInternet:
		Logic->IssueHackInternet(Selection);
		PlayVoice({ TEXT("VoiceHackInternet") }, Selection);
		break;
	case EGenCommand::FireWeapon:
		if (Button.Button->bNeedTargetObject || Button.Button->bNeedTargetPos)
		{
			BeginOrderTargeting(Button.Button); // pick the victim (or the spot) next
		}
		else
		{
			// Object::doCommandButton: "LOCATION BASED FIRE WEAPON" - where each of them stands (a bomb truck
			// blowing itself up).
			Logic->IssueFireWeaponAt(Selection, FVector3f(FLT_MAX), Button.Button->WeaponSlot, false);
		}
		break;
	case EGenCommand::DozerConstructCancel:
		Logic->IssueCancelConstruction(Button.Source);
		break;
	case EGenCommand::ToggleOvercharge:
		Logic->IssueToggleOvercharge(Button.Source);
		break;
	case EGenCommand::SetRallyPoint:
		BeginOrderTargeting(Button.Button); // where next
		break;
	case EGenCommand::SwitchWeapon:
		Logic->IssueSwitchWeapon(Selection, Button.Button->WeaponSlot);
		// "Play mode change acknowledgement"
		PlayVoice({ Button.Button->WeaponSlot == 0 ? TEXT("VoicePrimaryWeaponMode") : Button.Button->WeaponSlot == 1 ? TEXT("VoiceSecondaryWeaponMode")
			: TEXT("VoiceTertiaryWeaponMode") }, Selection);
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

bool AGenPlayerController::TestFireAt(const FVector3f& Pos, const float* PlaceDegrees)
{
	if (PlacingTemplate)
	{
		if (PlaceDegrees && PlacingPower.IsEmpty())
		{
			PlacementAngle = FMath::DegreesToRadians(*PlaceDegrees);
		}
		// A building (or a power placed like one): the click that puts it there, if it may go there - or the
		// refusal and why.
		const FString What = PlacingTemplate->Name, By = PlacingPower.IsEmpty() ? FString(TEXT("a dozer")) : PlacingPower;
		bPlacementValid = true;
		CheckPlacementAt(Pos);
		const bool bLegal = bPlacementLegal;
		UE_LOG(LogGenerals, Log, TEXT("Test: %s placed by %s at (%.0f, %.0f): legal=%d%s%s"), *What, *By, Pos.X, Pos.Y, int32(bLegal),
			PlacementWhy ? TEXT(" ") : TEXT(""), PlacementWhy ? PlacementWhy : TEXT(""));
		ConfirmPlacement();
		return bLegal;
	}
	if (!TargetingButton)
	{
		return false;
	}
	TargetingPos = Pos;
	bTargetingValid = true;
	// Something aimed at an object takes the one nearest the spot (there is no cursor to pick with).
	FGenObjectID Nearest = GenInvalidObjectID;
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (Logic && TargetingButton->bNeedTargetObject && !TargetingButton->bNeedTargetPos)
	{
		float BestDistSq = FMath::Square(100.f);
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			const float DistSq = O && !O->IsDead() ? FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(Pos.X, Pos.Y)) : FLT_MAX;
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Nearest = Id;
			}
		}
	}
	const bool bFired = FireTargetedPower(0.f, 0.f, true, Nearest);
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
		if (!Source || Source->IsDead() || !Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *Source))
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

void AGenPlayerController::PlayVoice(std::initializer_list<const TCHAR*> Keys, const TArray<FGenObjectID>& Units)
{
	if (const FGenObject* O = FindSpeaker(Units))
	{
		UGenGameLogic::Get(this)->PlayObjectSound(*O, Keys);
	}
}

const FGenObject* AGenPlayerController::FindSpeaker(const TArray<FGenObjectID>& Units) const
{
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	for (FGenObjectID Id : Units)
	{
		const FGenObject* O = Logic ? Logic->FindObject(Id) : nullptr;
		if (O && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *O) && !O->IsDead())
		{
			return O;
		}
	}
	return nullptr;
}

void AGenPlayerController::PlayAttackVoice(const TArray<FGenObjectID>& Units, EAttackVoice Order, const FGenObject* Target, bool bAir, int32 Slot,
	bool bMineClearing)
{
	// VoiceAttack (VoiceAttackAir at an aircraft) - unless the weapon calls for a voice of its own: a flash-bang
	// (VoiceSubdue, VoiceClearBuilding at a building), mine clearing (VoiceDisarm); fired from a button, the shot
	// at a driver (VoiceSnipePilot) or a knife (VoiceMelee); at a spot, flames or poison from a weapon other than
	// the first (VoiceFlameLocation, VoicePoisonLocation) and the Comanche's rocket pods (VoiceFireRocketPods).
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const FGenObject* U = FindSpeaker(Units);
	if (!Logic || !U)
	{
		return;
	}
	const TCHAR* Key = bAir ? TEXT("VoiceAttackAir") : TEXT("VoiceAttack");
	const int32 WeaponSlot = Slot != INDEX_NONE ? Slot : U->GetCurrentWeaponSlot();
	const FGenWeaponTemplate* W = (WeaponSlot >= 0 && WeaponSlot < 3) ? U->GetWeapon(WeaponSlot).Template : nullptr;
	// (Command_DisarmMinesAtPosition switched to the mine clearing weapons when its button was pressed.)
	const FString Damage = bMineClearing ? FString(TEXT("DISARM")) : W ? W->DamageType : FString();
	const bool bFromButton = Order == EAttackVoice::WeaponAtObject;
	if (Order == EAttackVoice::WeaponAtLocation)
	{
		if (Damage == TEXT("DISARM")) { Key = TEXT("VoiceDisarm"); }
		else if (Damage == TEXT("FLAME") && WeaponSlot != 0) { Key = TEXT("VoiceFlameLocation"); }
		else if (Damage == TEXT("POISON") && WeaponSlot != 0) { Key = TEXT("VoicePoisonLocation"); }
		else if (Damage != TEXT("SURRENDER") && W && W->Name == TEXT("ComancheRocketPodWeapon")) { Key = TEXT("VoiceFireRocketPods"); }
	}
	else if (Damage == TEXT("SURRENDER")) { Key = Target && Target->IsKindOf(TEXT("STRUCTURE")) ? TEXT("VoiceClearBuilding") : TEXT("VoiceSubdue"); }
	else if (Damage == TEXT("DISARM")) { Key = TEXT("VoiceDisarm"); }
	else if (Damage == TEXT("KILL_PILOT") && bFromButton) { Key = TEXT("VoiceSnipePilot"); }
	else if (Damage == TEXT("MELEE") && bFromButton) { Key = TEXT("VoiceMelee"); }
	Logic->PlayObjectSound(*U, { Key });
	if (Order == EAttackVoice::Attack || Order == EAttackVoice::ForceObject)
	{
		PlayCarBombVoice(U, TEXT("TerroristInCarAttackVoice"));
	}
}

void AGenPlayerController::PlayMoveVoice(const TArray<FGenObjectID>& Units, bool bSalvage, bool bWaypoint)
{
	// VoiceMove from the first of them - unless one has something better to say: VoiceSalvage on the way to a
	// salvage crate, or a GLA worker's VoiceMoveUpgraded once his side has the worker shoes. Adding a waypoint,
	// what is already on the move keeps quiet ("Don't want to play the sound unless he's not moving!").
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	UGenAudioManager* Audio = Logic ? Logic->GetAudio() : nullptr;
	if (!Audio)
	{
		return;
	}
	const FGenObject* Speaker = nullptr;
	const TCHAR* Key = TEXT("VoiceMove");
	for (FGenObjectID Id : Units)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || !Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *O) || O->IsDead() || (bWaypoint && O->IsMoving()))
		{
			continue;
		}
		Speaker = Speaker ? Speaker : O;
		const bool bShoes = O->IsKindOf(TEXT("INFANTRY")) && O->IsKindOf(TEXT("DOZER")) && O->IsKindOf(TEXT("HARVESTER")) // ("Only Workers fit all 3")
			&& Logic->HasUpgradeComplete(O->GetOwner(), TEXT("Upgrade_GLAWorkerShoes"));
		if (bShoes || (bSalvage && !Audio->GetTemplateSound(*O->GetTemplate(), TEXT("VoiceSalvage")).IsEmpty()))
		{
			Speaker = O;
			Key = bShoes ? TEXT("VoiceMoveUpgraded") : TEXT("VoiceSalvage");
			break;
		}
	}
	if (Speaker)
	{
		Logic->PlayObjectSound(*Speaker, { Key });
		PlayCarBombVoice(Speaker, TEXT("TerroristInCarMoveVoice"));
	}
}

void AGenPlayerController::PlayCarBombVoice(const FGenObject* Speaker, const TCHAR* MiscKey)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (Logic && Speaker && Speaker->IsCarBomb())
	{
		Logic->PlayEventForObject(FGenAudioData::Get().GetMisc(MiscKey), *Speaker);
	}
}

void AGenPlayerController::LookAt(const FVector3f& Pos)
{
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	AGenCameraPawn* Cam = GetCameraPawn();
	if (Logic && Logic->GetMapActor() && Cam)
	{
		Cam->SetTarget(Logic->GetMapActor()->GetActorTransform().TransformPosition(GenCoords::ToUE(Pos.X, Pos.Y, 0.f)));
	}
}

bool AGenPlayerController::RadarClick(const FVector2D& Pixel, bool bRelease)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FVector2f World;
	// (Without a radar the window shows nothing, and clicking it does nothing.)
	if (!Logic || !Logic->HasRadar(Logic->GetLocalPlayer()) || !GenRadar::PixelToWorld(Pixel, ControlBar.Radar, ControlBar.RadarMapSize, World))
	{
		return false;
	}
	const FVector3f Pos(World.X, World.Y, Logic->GetGroundHeight(World.X, World.Y));
	if (TargetingButton)
	{
		// Aiming an order or a power: the click on the radar says where (ControlBar's LeftHUDInput).
		if (bRelease)
		{
			TargetingPos = Pos;
			bTargetingValid = true;
			if (FireTargetedPower(0.f, 0.f, false))
			{
				EndTargeting();
			}
		}
		return true;
	}
	LookAt(Pos);
	return true;
}

bool AGenPlayerController::FireTargetedPower(float MX, float MY, bool bAllowPick, FGenObjectID ForcedTarget)
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
		if (TargetingButton->Command == EGenCommand::SetRallyPoint)
		{
			Logic->IssueSetRallyPoint(Selection, Pos);
			return true;
		}
		if (TargetingButton->Command == EGenCommand::AttackMove)
		{
			Logic->IssueAttackMove(Selection, Pos);
			PlayMoveVoice(Selection);
			return true;
		}
		if (TargetingButton->Command == EGenCommand::CombatDrop)
		{
			// Into the building clicked (anybody's but a side's own base buildings), or onto the ground there.
			const FGenObjectID Aimed = ForcedTarget != GenInvalidObjectID ? ForcedTarget : bAllowPick ? PickObject(MX, MY) : GenInvalidObjectID;
			const FGenObject* Into = Aimed != GenInvalidObjectID ? Logic->FindObject(Aimed) : nullptr;
			if (Into && !Logic->CanCombatDropInto(*Into))
			{
				Into = nullptr; // nothing to drop into: onto the ground there
			}
			Logic->IssueCombatDrop(Selection, Into ? Aimed : GenInvalidObjectID, Into ? Into->GetPosition() : Pos);
			PlayVoice({ TEXT("VoiceCombatDrop") }, Selection);
			return true;
		}
		if (TargetingButton->Command == EGenCommand::HijackVehicle || TargetingButton->Command == EGenCommand::ConvertToCarBomb
			|| TargetingButton->Command == EGenCommand::SabotageBuilding)
		{
			// GUI_COMMAND_HIJACK_VEHICLE / CONVERT_TO_CARBOMB ("CONTEXTMODE_COMMAND"): those of the selection that may
			// take the vehicle clicked over go for it - the way in a click on it gives them anyway (enter).
			const FGenObjectID Aimed = ForcedTarget != GenInvalidObjectID ? ForcedTarget : bAllowPick ? PickObject(MX, MY) : GenInvalidObjectID;
			const FGenObject* Vehicle = Aimed != GenInvalidObjectID ? Logic->FindObject(Aimed) : nullptr;
			TArray<FGenObjectID> Takers;
			for (FGenObjectID Id : Selection)
			{
				const FGenObject* U = Logic->FindObject(Id);
				if (Vehicle && U && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *U)
					&& (TargetingButton->Command == EGenCommand::SabotageBuilding ? Logic->CanSabotage(*U, *Vehicle) : Logic->CanTakeOverVehicle(*U, *Vehicle)))
				{
					Takers.Add(Id);
				}
			}
			if (Takers.Num() == 0)
			{
				return false;
			}
			Logic->IssueEnter(Takers, Aimed);
			PlayVoice({ TEXT("VoiceEnterHostile"), TEXT("VoiceEnter") }, Takers);
			return true;
		}
		if (TargetingButton->Command == EGenCommand::FireWeapon && TargetingButton->bNeedTargetPos && !TargetingButton->bNeedTargetObject)
		{
			// At a spot (MSG_DO_WEAPON_AT_LOCATION): the Comanche's rocket pods, the toxin tractor's ground
			// spraying, the Dragon tank's wall of fire - or Command_DisarmMinesAtPosition, "clear the mines around here".
			Logic->IssueFireWeaponAt(Selection, Pos, TargetingButton->WeaponSlot, TargetingButton->bUsesMineClearingWeaponSet, TargetingButton->MaxShotsToFire);
			PlayAttackVoice(Selection, EAttackVoice::WeaponAtLocation, nullptr, false, TargetingButton->WeaponSlot, TargetingButton->bUsesMineClearingWeaponSet);
			return true;
		}
		if (TargetingButton->Command == EGenCommand::FireWeapon)
		{
			// The weapon of the button's slot, at whatever is clicked that isn't ours or a friend's.
			const FGenObjectID Aimed = ForcedTarget != GenInvalidObjectID ? ForcedTarget : bAllowPick ? PickObject(MX, MY) : GenInvalidObjectID;
			const FGenObject* Victim = Aimed != GenInvalidObjectID ? Logic->FindObject(Aimed) : nullptr;
			if (!Victim || !CanFireWeaponAt(*Victim))
			{
				return false;
			}
			Logic->IssueFireWeapon(Selection, Aimed, TargetingButton->WeaponSlot);
			PlayAttackVoice(Selection, EAttackVoice::WeaponAtObject, nullptr, false, TargetingButton->WeaponSlot);
			return true;
		}
		const FGenObjectID Hit = bAllowPick ? PickObject(MX, MY) : GenInvalidObjectID;
		const FGenObject* HitObj = Hit != GenInvalidObjectID ? Logic->FindObject(Hit) : nullptr;
		const bool bEscort =HitObj && Logic->GetRelationship(Logic->GetLocalPlayer(), HitObj->GetOwner()) != EGenRelationship::Enemies;
		const EGenGuardMode Mode = TargetingButton->Command == EGenCommand::GuardWithoutPursuit ? EGenGuardMode::WithoutPursuit
			: TargetingButton->Command == EGenCommand::GuardFlyingOnly ? EGenGuardMode::FlyingOnly : EGenGuardMode::Normal;
		Logic->IssueGuard(Selection, bEscort ? HitObj->GetPosition() : Pos, bEscort ? Hit : GenInvalidObjectID, Mode);
		PlayVoice({ TEXT("VoiceGuard") }, Selection);
		return true;
	}
	if (TargetingButton->bNeedTargetObject && !TargetingButton->bNeedTargetPos)
	{
		// A power aimed at something (the cash hack at an enemy supply center).
		Target = ForcedTarget != GenInvalidObjectID ? ForcedTarget : bAllowPick ? PickObject(MX, MY) : GenInvalidObjectID;
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
	// A unit's own ability (OK_FOR_MULTI_SELECT): every selected unit that has it goes - when it was given from the
	// selection's own buttons. (From the shortcut tray the building that fires is usually not selected at all:
	// the particle cannon, whose module is one that "starts an attack", like a unit's ability.)
	const FGenObject* Src = Logic->FindObject(TargetingSource);
	const int32 SrcModule = Src ? Logic->FindPowerModule(*Src, TargetingPower) : INDEX_NONE;
	if (Src && SrcModule != INDEX_NONE && Src->GetTemplate()->PowerModules[SrcModule].Type == EGenPowerModule::SpecialAbility
		&& Selection.Contains(TargetingSource))
	{
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* U = Logic->FindObject(Id);
			if (U && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *U) && Logic->FindPowerModule(*U, TargetingPower) != INDEX_NONE)
			{
				Logic->IssueSpecialPower(Id, TargetingPower, Pos, Target);
			}
		}
		return true;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] the player fires %s from #%u at (%.0f, %.0f)"), Logic->GetFrame(), *TargetingPower,
		TargetingSource, Pos.X, Pos.Y);
	Logic->IssueSpecialPower(TargetingSource, TargetingPower, Pos, Target);
	if (TargetingButton->bFromShortcut && Src && Src->GetTemplate()->ParticleCannon.bValid)
	{
		// CommandXlat, SPECIAL_POWER_FROM_SHORTCUT: "the special power accepts input from the player (particle uplink
		// cannon, spectre gunship), simply select the object now" - the next clicks steer the beam. (A gunship
		// selects itself when it is made.)
		Selection.Reset();
		Selection.Add(TargetingSource);
	}
	return true;
}

void AGenPlayerController::EndPlacement()
{
	PlacingTemplate = nullptr;
	PlacingDozer = GenInvalidObjectID;
	PlacingPower.Reset();
	PlacingPowerSource = GenInvalidObjectID;
	bPlacementValid = false;
	bPlacementAnchored = false;
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
	const bool bForPower = !PlacingPower.IsEmpty();
	const FGenObject* Dozer = Logic ? Logic->FindObject(bForPower ? PlacingPowerSource : PlacingDozer) : nullptr;
	if (!Dozer || Dozer->IsDead() || (!bForPower && !Selection.Contains(PlacingDozer)) || !Logic->GetMapActor())
	{
		EndPlacement(); // the builder is gone or no longer selected (or the building that would fire the power)
		return;
	}
	if (bPlacementAnchored)
	{
		// InGameUI::handleBuildPlacements, anchored: the building stays where the button went down and, once the
		// mouse has moved off that spot, turns to face it ("only adjust angle if we've actually moved the mouse").
		constexpr double DragThresholdPixels = 5.0; // PLACEMENT_DRAG_THRESHOLD_DIST
		FVector3f Cursor;
		if (bHasMouse && FVector2D::Distance(PlacementAnchorScreen, FVector2D(MX, MY)) > DragThresholdPixels && GetTerrainUnderCursor(MX, MY, Cursor)
			&& FVector2f::DistSquared(FVector2f(Cursor.X, Cursor.Y), FVector2f(PlacementPos.X, PlacementPos.Y)) > 1.f)
		{
			PlacementAngle = FMath::Atan2(Cursor.Y - PlacementPos.Y, Cursor.X - PlacementPos.X);
		}
		bPlacementValid = true;
		CheckPlacementAt(PlacementPos);
	}
	else
	{
		bPlacementValid = bHasMouse && !ControlBar.IsOverPanel(FVector2D(MX, MY)) && GetTerrainUnderCursor(MX, MY, PlacementPos);
		if (bPlacementValid)
		{
			CheckPlacementAt(PlacementPos);
		}
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
			FRotator(0.f, GenCoords::AngleToYaw(PlacementAngle), 0.f));
	}
}

void AGenPlayerController::CheckPlacementAt(const FVector3f& Pos)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !PlacingTemplate)
	{
		return;
	}
	PlacementPos = FVector3f(Pos.X, Pos.Y, Logic->GetGroundHeight(Pos.X, Pos.Y));
	PlacementWhy = nullptr;
	if (Logic->IsShroudOn() && Logic->GetShroud().GetStatusAt(Logic->GetLocalPlayer(), PlacementPos.X, PlacementPos.Y) != EGenCellShroud::Clear)
	{
		// BuildAssistant's SHROUD_REVEALED, "the first check, since returning other errors for shrouded areas
		// could be used to game the system": only where we are looking right now.
		bPlacementLegal = false;
		PlacementWhy = TEXT("GUI:CantBuildShroud");
		return;
	}
	bPlacementLegal = (!PlacingPower.IsEmpty() || Logic->CanMake(Logic->GetLocalPlayer(), *PlacingTemplate))
		&& Logic->IsLocationLegalToBuild(*PlacingTemplate, PlacementPos, PlacingPower.IsEmpty() ? PlacementAngle : PlacingTemplate->PlacementViewAngle, &PlacementWhy);
}

void AGenPlayerController::ConfirmPlacement()
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !PlacingTemplate || !bPlacementValid)
	{
		return;
	}
	if (bPlacementLegal)
	{
		if (!PlacingPower.IsEmpty())
		{
			Logic->IssueSpecialPower(PlacingPowerSource, PlacingPower, PlacementPos, GenInvalidObjectID);
		}
		else
		{
			Logic->IssueConstruct(PlacingDozer, PlacingTemplate->Name, PlacementPos, PlacementAngle);
			PlayVoice({ TEXT("VoiceBuildResponse") }, { PlacingDozer });
		}
		EndPlacement();
	}
	else if (PlacementWhy)
	{
		// PlaceEventTranslator: "can't place, display why", the builder's VoiceNoBuild and the interface's
		// "can't do that" beep; the building stays on the cursor for another try.
		FString Message = FGenCommandData::Get().GetText(PlacementWhy);
		if (Message.IsEmpty() || Message.Equals(PlacementWhy, ESearchCase::IgnoreCase))
		{
			Message = TEXT("Can't build there.");
		}
		Logic->IssueScreenMessage(Message);
		PlayVoice({ TEXT("VoiceNoBuild") }, { PlacingPower.IsEmpty() ? PlacingDozer : PlacingPowerSource });
		Logic->PlayMiscSound(TEXT("NoCanDoSound"), Logic->GetLocalPlayer());
	}
}

namespace
{
	/** The settings the mouse and the camera go by (the defaults until the engine's settings are there). */
	const FGenControlSettings& ControlSettings()
	{
		static const FGenControlSettings Defaults;
		const UGenUserSettings* Settings = UGenUserSettings::Get();
		return Settings ? Settings->Controls : Defaults;
	}

	/** A speed of the controls page as a multiplier, kept within what the page offers (the file may say anything). */
	float SpeedOf(float Value)
	{
		return FMath::Clamp(Value, 0.25f, 3.f);
	}

	/** The game's window, if there is one. */
	TSharedPtr<SWindow> GameWindow()
	{
		return GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWindow() : nullptr;
	}

	/** Whether the game's window is the one in front (the mouse is the player's business otherwise). */
	bool IsGameWindowInFront()
	{
		const TSharedPtr<SWindow> Window = GameWindow();
		return Window.IsValid() && Window->GetNativeWindow().IsValid() && Window->GetNativeWindow()->IsForegroundWindow();
	}

	/** Edge scrolling, if it is on: in a window only if the player wants it there too (fullscreen, borderless or
	    not, has the screen's edge for the window's). */
	bool IsEdgeScrollOn(const FGenControlSettings& C)
	{
		if (!C.bEdgeScroll)
		{
			return false;
		}
		const TSharedPtr<SWindow> Window = GameWindow();
		return C.bEdgeScrollWindowed || !Window.IsValid() || Window->GetWindowMode() != EWindowMode::Windowed;
	}

	/** The view's movement this frame (screen x right, y up) from the scroll keys and the screen's edge, each -1..1
	    a side: each at its own speed, together no faster than the faster of them (at 1 and 1: as it always was). */
	FVector2D ScrollPan(const FVector2D& Keys, const FVector2D& Edge, const FGenControlSettings& C, float ScrollSpeed, float DeltaTime)
	{
		const float KeySpeed = SpeedOf(C.KeyScrollSpeed), EdgeSpeed = SpeedOf(C.EdgeScrollSpeed);
		const float Top = FMath::Max(Keys.IsZero() ? 0.f : KeySpeed, Edge.IsZero() ? 0.f : EdgeSpeed);
		return (Keys * KeySpeed + Edge * EdgeSpeed).GetClampedToMaxSize(Top) * ScrollSpeed * DeltaTime * 10.f;
	}

	/** ... from the right button held D (a fraction of the screen, y down) away from where it went down. */
	FVector2D DragPan(const FVector2D& D, const FGenControlSettings& C, float ScrollSpeed, float DeltaTime)
	{
		const float Sign = C.bInvertDragScroll ? -1.f : 1.f;
		return FVector2D(D.X * 1.6f, -D.Y * 2.0f) * ScrollSpeed * DeltaTime * 30.f * SpeedOf(C.DragScrollSpeed) * Sign;
	}

	/** Mouse wheel steps to zoom steps; and the middle button moved this many pixels across, to degrees. */
	float ZoomSteps(float Wheel, const FGenControlSettings& C) { return Wheel * SpeedOf(C.ZoomSpeed); }
	float RotateDegrees(float Pixels, const FGenControlSettings& C) { return Pixels * 0.3f * SpeedOf(C.RotateSpeed); }
}

void AGenPlayerController::UpdateCamera(float DeltaTime, AGenCameraPawn* Cam, float MX, float MY, bool bHasMouse)
{
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	const FGenControlSettings& Controls = ControlSettings();

	FVector2D Keys(0.f, 0.f);
	const FGenInput& Input = FGenInput::Get();
	if (Input.IsDown(this, EGenAction::ScrollLeft)) { Keys.X -= 1.f; }
	if (Input.IsDown(this, EGenAction::ScrollRight)) { Keys.X += 1.f; }
	if (Input.IsDown(this, EGenAction::ScrollUp)) { Keys.Y += 1.f; }
	if (Input.IsDown(this, EGenAction::ScrollDown)) { Keys.Y -= 1.f; }

	// The screen's edge (if the controls page has it on; and not while another window is in front).
	ScrollScreen = FVector2D::ZeroVector;
	FVector2D AtEdge(0.0, 0.0);
	if (bHasMouse && ViewX > 0 && ViewY > 0 && !bRightScrolling && !bBoxSelecting && IsEdgeScrollOn(Controls) && IsGameWindowInFront())
	{
		const float Edge = 4.f;
		if (MX <= Edge) { AtEdge.X -= 1.0; }
		if (MX >= ViewX - 1 - Edge) { AtEdge.X += 1.0; }
		if (MY <= Edge) { AtEdge.Y += 1.0; }
		if (MY >= ViewY - 1 - Edge) { AtEdge.Y -= 1.0; }
		ScrollScreen = FVector2D(AtEdge.X, -AtEdge.Y); // (the scroll cursor points the way the mouse pushes the view)
	}
	if (!Keys.IsZero() || !AtEdge.IsZero())
	{
		Cam->Pan(ScrollPan(Keys, AtEdge, Controls, Cam->ScrollSpeed, DeltaTime));
	}

	// Right button: drag scrolls (RMB scroll); a click without dragging is RightClick's.
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
			Cam->Pan(DragPan(D, Controls, Cam->ScrollSpeed, DeltaTime));
			// (The cursor points the way the view goes: against the drag when that is turned round.)
			ScrollScreen = D.IsNearlyZero() ? FVector2D(1.0, 0.0) : (Controls.bInvertDragScroll ? -D : D);
		}
	}
	else if (bRightDown)
	{
		bRightDown = false;
		if (!bRightScrolling)
		{
			RightClick(MX, MY, IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift));
		}
		bRightScrolling = false;
	}

	// Middle-drag rotate
	if (IsInputKeyDown(EKeys::MiddleMouseButton) && bHasMouse)
	{
		if (bMiddleDragging)
		{
			Cam->AddYaw(RotateDegrees(MX - LastMouseX, Controls));
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
		Cam->Zoom(ZoomSteps(Wheel, Controls));
	}
	if (Input.JustPressed(this, EGenAction::CameraReset))
	{
		Cam->ResetView();
	}
	// Space: to the last thing the radar reported (an attack, a finished building or upgrade).
	if (Input.JustPressed(this, EGenAction::JumpToEvent))
	{
		const UGenGameLogic* Logic = UGenGameLogic::Get(this);
		FVector3f EventPos;
		if (Logic && Logic->GetLastRadarEventPos(EventPos))
		{
			LookAt(EventPos);
		}
	}
}

// ---- the mouse buttons' layout, and the mouse itself (the controls page) -------------------------

bool AGenPlayerController::IsModernMouse() const
{
	return ControlSettings().MouseScheme == EGenMouseScheme::Modern;
}

void AGenPlayerController::WorldLeftClick(float MX, float MY, bool bShift, FClickEval* OutEval)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FClickEval Eval;
	if (!IsModernMouse())
	{
		// Classic: what it does is worked out by EvaluateClick - the cursor has shown it beforehand.
		EvaluateClick(PickObject(MX, MY, ClickPickTypes()), MX, MY, Eval);
	}
	else if (Logic)
	{
		// Modern: the left button only selects - one of ours (Shift adds or takes it away; twice quickly: all of
		// its kind), somebody else's thing on its own; a click on the ground deselects (with Shift, nothing).
		const FGenObjectID Hit = PickObject(MX, MY, PickSelectable);
		const FGenObject* HitObj = Hit != GenInvalidObjectID ? Logic->FindObject(Hit) : nullptr;
		const bool bSelectable = HitObj && HitObj->IsSelectable();
		Eval.Hit = HitObj ? Hit : GenInvalidObjectID;
		Eval.Action = bSelectable && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *HitObj) ? EClickAction::SelectOwn
			: bShift ? EClickAction::Nothing
			: bSelectable ? EClickAction::SelectOther
			: EClickAction::Deselect;
	}
	ExecuteClick(Eval, bShift);
	UE_CLOG(Logic && Logic->IsTracing(), LogGenerals, Log, TEXT("  [mouse f%u] %s left click: action %d by %d unit(s), %d selected now"), Logic->GetFrame(),
		IsModernMouse() ? TEXT("modern") : TEXT("classic"), int32(Eval.Action), Eval.Units.Num(), Selection.Num());
	if (OutEval)
	{
		*OutEval = MoveTemp(Eval);
	}
}

void AGenPlayerController::RightClick(float MX, float MY, bool bShift, FClickEval* OutEval)
{
	if (OutEval)
	{
		*OutEval = FClickEval();
	}
	// Either layout: back out of placing a building or aiming a power (or an order).
	if (PlacingTemplate)
	{
		EndPlacement();
	}
	else if (TargetingButton)
	{
		EndTargeting();
	}
	else if (!IsModernMouse())
	{
		// Classic: close the promotions, otherwise clear the selection.
		if (ControlBar.bGeneralsOpen) { ControlBar.bGeneralsOpen = false; }
		else { Selection.Reset(); }
	}
	else if (ControlBar.IsOverPanel(RightStart))
	{
		// Modern, on the command bar: on the radar, what moves goes there; elsewhere it closes the promotions.
		if (ControlBar.Radar.bIsValid && ControlBar.Radar.IsInside(RightStart))
		{
			RadarRightClick(FVector2D(MX, MY));
		}
		else if (ControlBar.bGeneralsOpen)
		{
			ControlBar.bGeneralsOpen = false;
		}
	}
	else
	{
		WorldRightClick(MX, MY, bShift, OutEval);
	}
}

void AGenPlayerController::WorldRightClick(float MX, float MY, bool bShift, FClickEval* OutEval)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !Logic->IsRunning())
	{
		return;
	}
	// The order a classic left click would give (EvaluateClick: move, attack, get in, mend, the rally point; with
	// Ctrl a force attack, with Alt a waypoint) - but the right button never selects: on one of ours (that takes no
	// order from the selection), or with nothing of ours to command, it does nothing.
	FClickEval Eval;
	EvaluateClick(PickObject(MX, MY, ClickPickTypes()), MX, MY, Eval);
	if (Eval.Action == EClickAction::SelectOwn || Eval.Action == EClickAction::SelectOther || Eval.Action == EClickAction::Deselect)
	{
		Eval.Action = EClickAction::Nothing;
		Eval.Units.Reset();
	}
	ExecuteClick(Eval, bShift);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mouse f%u] modern right click: action %d by %d unit(s)"), Logic->GetFrame(), int32(Eval.Action), Eval.Units.Num());
	if (OutEval)
	{
		*OutEval = MoveTemp(Eval);
	}
}

bool AGenPlayerController::RadarRightClick(const FVector2D& Pixel)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FVector2f World;
	if (!Logic || !Logic->IsRunning() || !Logic->HasRadar(Logic->GetLocalPlayer()) || !GenRadar::PixelToWorld(Pixel, ControlBar.Radar, ControlBar.RadarMapSize, World))
	{
		return false;
	}
	const int32 Local = Logic->GetLocalPlayer();
	TArray<FGenObjectID> Movers;
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && Logic->CanPlayerCommand(Local, *O) && !O->GetTemplate()->bForbidPlayerCommands && O->CanMove())
		{
			Movers.Add(Id);
		}
	}
	if (Movers.Num() == 0)
	{
		return false;
	}
	Logic->IssueMove(Movers, FVector3f(World.X, World.Y, Logic->GetGroundHeight(World.X, World.Y)));
	PlayMoveVoice(Movers);
	return true;
}

bool AGenPlayerController::IsPlayingWithMouse() const
{
	const AGenGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AGenGameMode>() : nullptr;
	const AGenHUD* GenHUD = Cast<AGenHUD>(GetHUD());
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	return bGenInputEnabled && GM && !GM->IsShellActive() && !(GenHUD && GenHUD->IsOptionsOpen()) && Logic && Logic->IsRunning();
}

void AGenPlayerController::UpdateMouseConfine(bool bPlaying)
{
	const ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(Player);
	UGameViewportClient* Client = LocalPlayer ? LocalPlayer->ViewportClient.Get() : nullptr;
	const TSharedPtr<SViewport> Widget = Client ? Client->GetGameViewportWidget() : nullptr;
	const TSharedPtr<ICursor> Cursor = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetPlatformApplication()->Cursor : nullptr;
	if (!Client || !Cursor.IsValid())
	{
		return;
	}
	const bool bWant = bPlaying && Widget.IsValid() && ControlSettings().bConfineMouse && IsGameWindowInFront();
	// Where the game's picture is on the desktop (rounded inwards); looked at every frame, the window may move or change size.
	FIntRect Bounds;
	if (bWant)
	{
		const FSlateRect R = Widget->GetCachedGeometry().GetLayoutBoundingRect();
		Bounds = FIntRect(FMath::CeilToInt(R.Left), FMath::CeilToInt(R.Top), FMath::FloorToInt(R.Right), FMath::FloorToInt(R.Bottom));
		if (Bounds.Width() <= 0 || Bounds.Height() <= 0)
		{
			return; // (not laid out yet)
		}
	}
	if (bMouseConfineSet && bWant == bMouseConfined && (!bWant || Bounds == ConfineBounds))
	{
		return;
	}
	if (bWant != bMouseConfined || !bMouseConfineSet)
	{
		// The viewport's own lock too (it locks on a click; with this, it keeps it on letting go).
		Client->SetMouseLockMode(bWant ? EMouseLockMode::LockAlways : EMouseLockMode::DoNotLock);
	}
	bMouseConfineSet = true;
	bMouseConfined = bWant;
	ConfineBounds = Bounds;
	if (bWant)
	{
		RECT Clip;
		Clip.left = Bounds.Min.X;
		Clip.top = Bounds.Min.Y;
		Clip.right = Bounds.Max.X;
		Clip.bottom = Bounds.Max.Y;
		Cursor->Lock(&Clip);
	}
	else
	{
		Cursor->Lock(nullptr);
	}
}

void AGenPlayerController::UpdatePointerSpeed(bool bPlaying)
{
	// The cursor is Windows' own: a speed other than 1 is had by moving it by hand - each frame it is put where the
	// mouse's movement since the last, scaled, takes it. Only in a game, with the window in front (never in the menus).
	const float Speed = SpeedOf(ControlSettings().CursorSpeed);
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	float MX = 0.f, MY = 0.f;
	if (!bPlaying || FMath::IsNearlyEqual(Speed, 1.f, 0.01f) || ViewX <= 0 || ViewY <= 0 || !IsGameWindowInFront() || !GetMousePosition(MX, MY))
	{
		bPointerScaling = false;
		return;
	}
	const FIntPoint Now(FMath::RoundToInt(MX), FMath::RoundToInt(MY));
	const FIntPoint Moved = Now - PointerPlaced;
	// (A first frame, or a jump no hand makes in one - the cursor came back into the window somewhere else: from there.)
	if (!bPointerScaling || FMath::Abs(Moved.X) > ViewX / 3 || FMath::Abs(Moved.Y) > ViewY / 3)
	{
		bPointerScaling = true;
		PointerExact = FVector2D(Now);
		PointerPlaced = Now;
		return;
	}
	if (Moved == FIntPoint::ZeroValue)
	{
		return;
	}
	PointerExact += FVector2D(Moved) * Speed;
	PointerExact = FVector2D(FMath::Clamp(PointerExact.X, 0.0, double(ViewX - 1)), FMath::Clamp(PointerExact.Y, 0.0, double(ViewY - 1)));
	PointerPlaced = FIntPoint(FMath::RoundToInt(PointerExact.X), FMath::RoundToInt(PointerExact.Y));
	if (PointerPlaced != Now)
	{
		SetMouseLocation(PointerPlaced.X, PointerPlaced.Y);
	}
}

FString AGenPlayerController::TestMouse(const TArray<FString>& Args)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const FString What = Args.Num() > 0 ? Args[0] : FString();
	if (What.Equals(TEXT("scheme"), ESearchCase::IgnoreCase) && Args.Num() >= 2)
	{
		UGenUserSettings* Settings = UGenUserSettings::Get();
		if (!Settings)
		{
			return TEXT("no settings");
		}
		Settings->Controls.MouseScheme = Args[1].Equals(TEXT("modern"), ESearchCase::IgnoreCase) ? EGenMouseScheme::Modern : EGenMouseScheme::Classic;
		return IsModernMouse() ? TEXT("the Modern layout") : TEXT("the Classic layout");
	}
	if ((What.Equals(TEXT("left"), ESearchCase::IgnoreCase) || What.Equals(TEXT("right"), ESearchCase::IgnoreCase)) && Args.Num() >= 3 && Logic)
	{
		const float X = FCString::Atof(*Args[1]), Y = FCString::Atof(*Args[2]);
		const FVector3f Pos(X, Y, Logic->GetGroundHeight(X, Y));
		FVector2D Screen(0.0, 0.0);
		const FGenObjectID Hit = TestPickAt(Pos, &Screen);
		const FString Cursor = CursorForClick(Hit, float(Screen.X), float(Screen.Y));
		const int32 Before = Selection.Num();
		FClickEval Eval;
		if (What.Equals(TEXT("left"), ESearchCase::IgnoreCase))
		{
			WorldLeftClick(float(Screen.X), float(Screen.Y), false, &Eval);
		}
		else
		{
			RightStart = Screen; // (where the button went down: RightClick looks there for the command bar)
			RightClick(float(Screen.X), float(Screen.Y), false, &Eval);
		}
		const FGenObject* O = Logic->FindObject(Hit);
		return FString::Printf(TEXT("%s %s click on %s: action %d by %d unit(s), selected %d -> %d, cursor %s"), IsModernMouse() ? TEXT("modern") : TEXT("classic"), *What.ToLower(),
			O ? *FString::Printf(TEXT("#%u %s"), Hit, *O->GetTemplate()->Name) : TEXT("the ground"), int32(Eval.Action), Eval.Units.Num(), Before, Selection.Num(), *Cursor);
	}
	AGenCameraPawn* Cam = GetCameraPawn();
	if (What.Equals(TEXT("camera"), ESearchCase::IgnoreCase) && Args.Num() >= 2 && Cam)
	{
		// One frame's worth of the input (at 30 frames a second), through the same sums as UpdateCamera's; the view is put back after.
		const FGenControlSettings& Controls = ControlSettings();
		const FString Kind = Args[1];
		const float Amount = Args.Num() >= 3 ? FCString::Atof(*Args[2]) : 1.f;
		const FVector Start = Cam->GetTarget();
		const float StartYaw = Cam->GetYaw(), StartHeight = Cam->GetTargetHeight();
		constexpr float Dt = 1.f / 30.f;
		if (Kind.Equals(TEXT("key"), ESearchCase::IgnoreCase)) { Cam->Pan(ScrollPan(FVector2D(1.0, 0.0), FVector2D::ZeroVector, Controls, Cam->ScrollSpeed, Dt)); }
		else if (Kind.Equals(TEXT("edge"), ESearchCase::IgnoreCase)) { Cam->Pan(ScrollPan(FVector2D::ZeroVector, FVector2D(1.0, 0.0), Controls, Cam->ScrollSpeed, Dt)); }
		else if (Kind.Equals(TEXT("drag"), ESearchCase::IgnoreCase)) { Cam->Pan(DragPan(FVector2D(Amount * 0.1, 0.0), Controls, Cam->ScrollSpeed, Dt)); }
		FString Result;
		if (Kind.Equals(TEXT("zoom"), ESearchCase::IgnoreCase))
		{
			Cam->Zoom(ZoomSteps(Amount, Controls));
			Result = FString::Printf(TEXT("%.1f wheel step(s): height %.1f -> %.1f"), Amount, StartHeight, Cam->GetTargetHeight());
			Cam->Zoom(-ZoomSteps(Amount, Controls));
		}
		else if (Kind.Equals(TEXT("rotate"), ESearchCase::IgnoreCase))
		{
			Cam->AddYaw(RotateDegrees(Amount * 100.f, Controls));
			Result = FString::Printf(TEXT("middle drag %.0f px: %.1f degrees"), Amount * 100.f, Cam->GetYaw() - StartYaw);
			Cam->AddYaw(StartYaw - Cam->GetYaw());
		}
		else
		{
			const FVector Moved = GenCoords::FromUE(Cam->GetTarget()) - GenCoords::FromUE(Start);
			Result = FString::Printf(TEXT("%s scroll, one frame: the view moved (%.1f, %.1f)"), *Kind.ToLower(), Moved.X, Moved.Y);
			Cam->SetTarget(Start);
		}
		return FString::Printf(TEXT("%s (speeds: edge %.2f, keys %.2f, drag %.2f%s, zoom %.2f, rotate %.2f)"), *Result, Controls.EdgeScrollSpeed, Controls.KeyScrollSpeed,
			Controls.DragScrollSpeed, Controls.bInvertDragScroll ? TEXT(" inverted") : TEXT(""), Controls.ZoomSpeed, Controls.RotateSpeed);
	}
	return TEXT("NOT UNDERSTOOD (left,x,y / right,x,y / scheme,classic|modern / camera,key|edge|drag|zoom|rotate[,amount])");
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
			bLeftOnRadar = bLeftOnUI && ControlBar.Radar.bIsValid && ControlBar.Radar.IsInside(LeftStart);
			// PlaceEventTranslator, MSG_RAW_MOUSE_LEFT_BUTTON_DOWN: "set this location as the placement anchor" - held
			// and dragged, the building turns; let go, it is placed. (The sneak attack's tunnel keeps its angle.)
			if (PlacingTemplate && !bLeftOnUI && bPlacementValid && PlacingPower.IsEmpty())
			{
				bPlacementAnchored = true;
				PlacementAnchorScreen = LeftStart;
			}
		}
		if (bLeftOnRadar && !TargetingButton)
		{
			RadarClick(FVector2D(MX, MY), false); // held on the radar: the view follows the cursor
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
		if (bLeftOnRadar)
		{
			RadarClick(FVector2D(MX, MY), true);
		}
		else if (bLeftOnUI)
		{
			// A click on the command bar never reaches the world.
			if (ControlBar.MenuButton.bIsValid && ControlBar.MenuButton.IsInside(LeftStart) && ControlBar.MenuButton.IsInside(FVector2D(MX, MY)))
			{
				if (AGenGameMode* GameMode = GetWorld()->GetAuthGameMode<AGenGameMode>())
				{
					GameMode->ToggleQuitMenu();
				}
			}
			else if (const FGenBarButton* Button = ControlBar.HitTest(FVector2D(MX, MY)))
			{
				const FGenBarButton Copy = *Button;
				ExecuteBarButton(Copy);
			}
		}
		else if (PlacingTemplate)
		{
			ConfirmPlacement();
			bPlacementAnchored = false; // (refused: the building follows the cursor again for another try)
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
			// A plain left click: selects, or (the Classic layout) gives the order the cursor has shown beforehand.
			WorldLeftClick(MX, MY, bShift);
		}
		bBoxSelecting = false;
		bLeftOnRadar = false;
	}

	// The command bar's buttons have their keys first (a key one of them takes is gone for the commands below).
	UpdateBarHotkeys();
	const FGenInput& Input = FGenInput::Get();
	if (Input.JustPressed(this, EGenAction::Stop) && Selection.Num() > 0)
	{
		Logic->IssueStop(Selection);
	}
	// The selection keys (CommandMap.ini): Ctrl + a number makes the selection that group, the number alone
	// selects it, with Shift adds it, with Alt looks at it; E: everything like the selection; Q: every
	// fighting unit; W: every aircraft.
	{
		const bool bCtrl = FGenInput::CtrlDown(this);
		const bool bAlt = FGenInput::AltDown(this);
		for (int32 Group = 0; Group < 10; ++Group)
		{
			if (Input.JustPressed(this, FGenInput::GroupAction(Group)))
			{
				GroupKey(Group, bCtrl ? EGroupKey::Create : bAlt ? EGroupKey::View : bShift ? EGroupKey::Add : EGroupKey::Select);
			}
		}
		if (!bCtrl && !bAlt)
		{
			if (Input.JustPressed(this, EGenAction::SelectMatching) && SelectMatching(false) == 0)
			{
				SelectMatching(true); // InGameUI::selectUnitsMatchingCurrentSelection: none more on the screen - then the map
			}
			if (Input.JustPressed(this, EGenAction::SelectAllUnits))
			{
				SelectAllUnits(false);
			}
			if (Input.JustPressed(this, EGenAction::SelectAllAircraft))
			{
				SelectAllUnits(true);
			}
		}
	}
	// A: attack-move, G: guard (then a left click says where).
	const bool bHasMovers = Selection.ContainsByPredicate([Logic](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return O && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *O) && O->CanMove();
	});
	if (bHasMovers && Input.JustPressed(this, EGenAction::AttackMove))
	{
		BeginOrderTargeting(FGenCommandData::Get().FindButton(TEXT("Command_AttackMove")));
	}
	if (bHasMovers && Input.JustPressed(this, EGenAction::Guard))
	{
		BeginOrderTargeting(FGenCommandData::Get().FindButton(TEXT("Command_Guard")));
	}
	// Ctrl+F (CREATE_FORMATION): the selected units keep their places relative to one another when they
	// move, at the pace of the slowest - or, if they are a formation already, no longer.
	if (bHasMovers && Input.JustPressed(this, EGenAction::Formation))
	{
		TArray<FGenObjectID> Own;
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *O) && O->CanMove())
			{
				Own.Add(Id);
			}
		}
		Logic->IssueCreateFormation(Own);
	}

	// Drop objects that died, no longer exist, or have gone back into the fog of war.
	Selection.RemoveAll([Logic](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return O == nullptr || !O->IsSelectable() || !Logic->IsVisibleToLocalPlayer(*O);
	});
}

void AGenPlayerController::ExecuteClick(const FClickEval& Eval, bool bShift)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return;
	}
	const FGenObject* HitObj = Logic->FindObject(Eval.Hit);
	switch (Eval.Action)
	{
	case EClickAction::Dock:
		Logic->IssueDock(Eval.Units, Eval.Hit);
		PlayVoice({ TEXT("VoiceSupply") }, Eval.Units);
		break;
	case EClickAction::Enter:
	{
		Logic->IssueEnter(Eval.Units, Eval.Hit);
		const EGenRelationship Relation = Logic->GetRelationship(Logic->GetLocalPlayer(), HitObj->GetOwner());
		const TCHAR* Voice = TEXT("VoiceEnter");
		if (HitObj->IsKindOf(TEXT("HEAL_PAD")))
		{
			Voice = TEXT("VoiceGetHealed");
		}
		else if (HitObj->IsKindOf(TEXT("STRUCTURE")))
		{
			Voice = Relation == EGenRelationship::Enemies ? TEXT("VoiceEnterHostile") : TEXT("VoiceGarrison");
		}
		else if (Relation != EGenRelationship::Allies)
		{
			Voice = TEXT("VoiceEnterHostile");
		}
		PlayVoice({ Voice }, Eval.Units);
		break;
	}
	case EClickAction::Repair:
		Logic->IssueRepair(Eval.Units, Eval.Hit);
		PlayVoice({ TEXT("VoiceRepair") }, Eval.Units);
		break;
	case EClickAction::GetRepaired:
		Logic->IssueGetRepaired(Eval.Units, Eval.Hit);
		PlayMoveVoice(Eval.Units);
		break;
	case EClickAction::SetRally:
		Logic->IssueSetRallyPoint(Eval.Units, Eval.Pos);
		break;
	case EClickAction::Land:
		Logic->IssueLand(Eval.Units, Eval.Hit);
		PlayVoice({ TEXT("VoiceMove") }, Eval.Units);
		break;
	case EClickAction::ResumeConstruction:
		Logic->IssueResumeConstruction(Eval.Units, Eval.Hit);
		PlayVoice({ TEXT("VoiceBuildResponse") }, Eval.Units);
		break;
	case EClickAction::SelectOwn:
	{
		// MSG_MOUSE_LEFT_DOUBLE_CLICK on something of ours that isn't a building: that one alone, and then
		// everything like it on the screen - with Alt, on the whole map. (Not with Ctrl held.)
		const double ClickTime = FPlatformTime::Seconds();
		const bool bDouble = Eval.Hit == LastClickHit && ClickTime - LastClickTime < 0.45 && !HitObj->IsKindOf(TEXT("STRUCTURE"))
			&& !IsInputKeyDown(EKeys::LeftControl) && !IsInputKeyDown(EKeys::RightControl);
		LastClickHit = Eval.Hit;
		LastClickTime = ClickTime;
		if (bDouble)
		{
			LastClickHit = GenInvalidObjectID;
			Selection.Reset();
			Selection.Add(Eval.Hit);
			SelectMatching(IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt));
			break;
		}
		if (!bShift)
		{
			Selection.Reset();
		}
		if (bShift && Selection.Contains(Eval.Hit))
		{
			Selection.Remove(Eval.Hit);
		}
		else
		{
			Selection.AddUnique(Eval.Hit);
			Logic->FlashSelected(Eval.Hit); // Drawable::onSelected
			PlayVoice({ TEXT("VoiceSelect") }, { Eval.Hit });
		}
		break;
	}
	case EClickAction::SteerPower:
		Logic->IssueSteerGunship(Eval.Units, Eval.Pos);
		break;
	case EClickAction::ForceAttackObject:
		// (The force attack messages carry no target for the voice: no VoiceAttackAir, no VoiceClearBuilding.)
		Logic->IssueForceAttack(Eval.Units, Eval.Hit);
		PlayAttackVoice(Eval.Units, EAttackVoice::ForceObject, nullptr, false);
		break;
	case EClickAction::ForceAttackGround:
	{
		// VoiceBombard; without one, nothing ("clear out the sound to play" - and what it falls through to wants
		// a weapon slot this order doesn't have).
		Logic->IssueForceAttackGround(Eval.Units, Eval.Pos);
		const FGenObject* Speaker = FindSpeaker(Eval.Units);
		if (Speaker && Logic->GetAudio() && !Logic->GetAudio()->GetTemplateSound(*Speaker->GetTemplate(), TEXT("VoiceBombard")).IsEmpty())
		{
			Logic->PlayObjectSound(*Speaker, { TEXT("VoiceBombard") });
			PlayCarBombVoice(Speaker, TEXT("TerroristInCarAttackVoice"));
		}
		break;
	}
	case EClickAction::AddWaypoint:
		Logic->IssueAddWaypoint(Eval.Units, Eval.Pos);
		PlayMoveVoice(Eval.Units, false, true);
		break;
	case EClickAction::Attack:
		Logic->IssueAttack(Eval.Units, Eval.Hit);
		PlayAttackVoice(Eval.Units, EAttackVoice::Attack, HitObj, HitObj->IsAirborneTarget());
		break;
	case EClickAction::Move:
		Logic->IssueMove(Eval.Units, Eval.Pos);
		PlayMoveVoice(Eval.Units);
		break;
	case EClickAction::Salvage:
		// MSG_DO_SALVAGE "is intentionally set up to mimic the moveto": to where the crate lies.
		Logic->IssueMove(Eval.Units, Eval.Pos);
		PlayMoveVoice(Eval.Units, true);
		break;
	case EClickAction::SelectOther:
		Selection.Reset();
		Selection.Add(Eval.Hit);
		Logic->FlashSelected(Eval.Hit);
		break;
	case EClickAction::Deselect:
		Selection.Reset();
		break;
	default:
		break;
	}
}

FGenObjectID AGenPlayerController::PickObject(float MX, float MY, uint32 PickTypes) const
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FVector Origin, Dir;
	if (!Logic || !DeprojectScreenPositionToWorld(MX, MY, Origin, Dir))
	{
		return GenInvalidObjectID;
	}
	// What could be under the cursor at all: of the kinds asked for, in sight, and near the ray (a model
	// reaches further than its object's shape - a crane, a flag - so "near" is taken widely).
	TArray<FGenObjectID, TInlineAllocator<32>> Near;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O)
		{
			continue;
		}
		const FGenThingTemplate* T = O->GetTemplate();
		const FVector3f P = Logic->GetRenderPosition(*O);
		const FVector L = GenCoords::ToUE(P.X, P.Y, P.Z) - Origin;
		const double Along = FVector::DotProduct(L, Dir);
		const double Reach = double(T->GetBoundingCircleRadius() * 3.f + T->GeometryHeight + 60.f) * GenCoords::UnitsToCm;
		if (Along <= 0.0 || L.SizeSquared() - Along * Along > Reach * Reach)
		{
			continue;
		}
		if ((PickTypeOf(*O) & PickTypes) == 0 || !Logic->IsVisibleToLocalPlayer(*O))
		{
			continue; // (what the fog of war hides can't be clicked)
		}
		Near.Add(Id);
	}
	if (Near.Num() == 0)
	{
		return GenInvalidObjectID;
	}
	// RTS3DScene::castRay: the nearest of them that the ray meets.
	auto Cast = [&](const FVector& RayOrigin, const FVector& RayDir, double& InOutT)
	{
		FGenObjectID Best = GenInvalidObjectID;
		for (FGenObjectID Id : Near)
		{
			if (Logic->RaycastDrawable(Id, RayOrigin, RayDir, InOutT))
			{
				Best = Id;
			}
		}
		return Best;
	};
	double BestT = TNumericLimits<double>::Max();
	FGenObjectID Best = Cast(Origin, Dir, BestT);
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	const float Slack = CVarGenPickSlack.GetValueOnGameThread() * FMath::Max(1.f, float(ViewY) / 1080.f);
	if (Best == GenInvalidObjectID && Slack > 0.f)
	{
		static const FVector2D Around[] = { { -1.0, 0.0 }, { 1.0, 0.0 }, { 0.0, -1.0 }, { 0.0, 1.0 }, { -0.7, -0.7 }, { 0.7, -0.7 }, { -0.7, 0.7 }, { 0.7, 0.7 } };
		for (const FVector2D& A : Around)
		{
			FVector O2, D2;
			if (DeprojectScreenPositionToWorld(MX + float(A.X) * Slack, MY + float(A.Y) * Slack, O2, D2))
			{
				const FGenObjectID Hit = Cast(O2, D2, BestT);
				if (Hit != GenInvalidObjectID)
				{
					Best = Hit;
				}
			}
		}
	}
	return Best;
}

FGenObjectID AGenPlayerController::TestPickAt(const FVector3f& Pos, FVector2D* OutScreen) const
{
	FVector2D Screen;
	if (!ProjectWorldLocationToScreen(GenCoords::ToUE(Pos.X, Pos.Y, Pos.Z), Screen))
	{
		return GenInvalidObjectID;
	}
	if (OutScreen)
	{
		*OutScreen = Screen;
	}
	return PickObject(float(Screen.X), float(Screen.Y), ClickPickTypes());
}

void AGenPlayerController::UpdateMouseOver(float MX, float MY, bool bHasMouse)
{
	// HintSpy: what the cursor is on. Nothing while it is over the interface, or busy dragging a box or the view.
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FVector Origin, Dir;
	if (!Logic || !Logic->IsRunning() || !bHasMouse || bBoxSelecting || bRightScrolling || bMiddleDragging
		|| ControlBar.IsOverPanel(FVector2D(MX, MY)) || !DeprojectScreenPositionToWorld(MX, MY, Origin, Dir))
	{
		MousedOver = GenInvalidObjectID;
		MouseOverDir = FVector::ZeroVector;
		HoverCursor = TEXT("Arrow");
		return;
	}
	// (Things move under a cursor that doesn't: looked at again thirty times a second all the same.)
	const double Now = FPlatformTime::Seconds();
	if (Origin.Equals(MouseOverOrigin, 0.01) && Dir.Equals(MouseOverDir, 1.e-6) && Now - MouseOverTime < 1.0 / 30.0)
	{
		return;
	}
	MouseOverOrigin = Origin;
	MouseOverDir = Dir;
	MouseOverTime = Now;
	const FGenObjectID Before = MousedOver;
	MousedOver = PickObject(MX, MY, PickSelectable | PickForceAttackable); // (SelectionTranslator: getPickTypesForContext(true))
	if (MousedOver != Before)
	{
		MousedOverSince = Now; // "Resetting tooltip delay"
	}
	HoverCursor = CursorForClick(MousedOver, MX, MY);
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
		if (!O || !O->IsSelectable() || !Logic->CanPlayerCommand(Logic->GetLocalPlayer(), *O) || !O->CanMove())
		{
			continue;
		}
		const FVector3f P = Logic->GetRenderPosition(*O);
		FVector2D S;
		if (ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z), S) && Box.IsInside(S))
		{
			if (!Selection.Contains(Id))
			{
				Selection.Add(Id);
				Logic->FlashSelected(Id); // Drawable::onSelected
			}
		}
	}
	PlayVoice({ TEXT("VoiceSelect") }, Selection);
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
	const bool bTerrain = Logic->GetMapActor()->RaycastTerrain(Origin, Dir, Hit);
	if (bTerrain)
	{
		OutGenPos = FVector3f(GenCoords::FromUE(Hit));
	}
	// TerrainLogic::pickBridge: a bridge's deck in front of the ground is what the cursor is on (an order
	// given there is for the bridge, not for what lies under it).
	const FVector3f GenOrigin = FVector3f(GenCoords::FromUE(Origin));
	const FVector3f GenDir = FVector3f(float(Dir.X), float(-Dir.Y), float(Dir.Z));
	FVector3f Deck;
	if (Logic->GetPathfinder().PickBridge(GenOrigin, GenDir, Deck)
		&& (!bTerrain || FVector3f::DistSquared(GenOrigin, Deck) < FVector3f::DistSquared(GenOrigin, OutGenPos)))
	{
		OutGenPos = Deck;
		return true;
	}
	return bTerrain;
}

void AGenPlayerController::EvaluateClick(FGenObjectID Hit, float MX, float MY, FClickEval& Out) const
{
	// On one of our own objects a click selects; otherwise, with units selected, it is an order (what they
	// can do with the thing clicked, attack the enemy under the cursor, or move there); with nothing to
	// command it selects whatever was clicked.
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	Out = FClickEval();
	if (!Logic)
	{
		return;
	}
	const FGenObject* HitObj = Hit != GenInvalidObjectID ? Logic->FindObject(Hit) : nullptr;
	Out.Hit = HitObj ? Hit : GenInvalidObjectID;
	const int32 Local = Logic->GetLocalPlayer();
	const bool bOwn = HitObj && Logic->CanPlayerCommand(Local, *HitObj) && HitObj->IsSelectable();
	// (ForbidPlayerCommands: what only its own modules command - a gunship - takes no orders.)
	Out.bCanCommand = Selection.ContainsByPredicate([Logic, Local](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return O && Logic->CanPlayerCommand(Local, *O) && !O->GetTemplate()->bForbidPlayerCommands && (O->CanMove() || O->HasWeapons() || O->GetSpawns().Num() > 0);
	});
	// Ctrl (MSG_META_BEGIN_FORCEATTACK): with something of ours to command, a click is a force attack - on the
	// thing under the cursor, whoever's it is, or else on the ground there (CommandTranslator::evaluateForceAttack;
	// canAnyForceAttack goes by the first of the selection).
	if (IsForceAttackMode() && Out.bCanCommand)
	{
		const FGenObject* First = nullptr;
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O) && !O->GetTemplate()->bForbidPlayerCommands)
			{
				First = First ? First : O;
				Out.Units.Add(Id);
			}
		}
		const bool bGround = GetTerrainUnderCursor(MX, MY, Out.Pos);
		if (First && (HitObj || bGround))
		{
			const EForceAttack Result = CanForceAttack(*First, HitObj, Out.Pos);
			Out.Action = Result == EForceAttack::InvalidShot ? EClickAction::ImpossibleAttack
				: Result == EForceAttack::NotPossible ? EClickAction::Nothing
				: HitObj ? EClickAction::ForceAttackObject : EClickAction::ForceAttackGround;
		}
		if (Out.Action != EClickAction::ForceAttackObject && Out.Action != EClickAction::ForceAttackGround)
		{
			Out.Units.Reset();
		}
		return;
	}
	// Alt (MSG_META_BEGIN_WAYPOINTS): "override any *other* commands with waypoint commands" - a click that
	// doesn't select one of ours adds a waypoint for whatever of ours moves (MSG_ADD_WAYPOINT).
	if (IsWaypointMode() && Out.bCanCommand && !bOwn)
	{
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O) && !O->GetTemplate()->bForbidPlayerCommands && O->CanMove())
			{
				Out.Units.Add(Id);
			}
		}
		if (Out.Units.Num() > 0 && GetTerrainUnderCursor(MX, MY, Out.Pos))
		{
			Out.Action = EClickAction::AddWaypoint;
			return;
		}
		Out.Units.Reset();
	}
	// A gunship of ours on its run, selected: a click says where its guns are to point
	// (doesSpecialPowerHaveOverridableDestinationActive).
	TArray<FGenObjectID> Steered;
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && Logic->CanPlayerCommand(Local, *O) && (O->IsGunshipSteerable() || O->IsParticleCannonSteerable())) { Steered.Add(Id); }
	}
	// Dozers of ours in the selection carry on an unfinished building when it is clicked.
	TArray<FGenObjectID> Dozers;
	if (bOwn && HitObj->IsUnderConstruction())
	{
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O) && O->GetTemplate()->bIsDozer) { Dozers.Add(Id); }
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
			if (O && Logic->CanPlayerCommand(Local, *O) && Logic->CanTransferSuppliesAt(*O, *HitObj)) { Gatherers.Add(Id); }
		}
	}
	// Aircraft of ours in the selection land at an airfield of ours when it is clicked.
	TArray<FGenObjectID> Aircraft;
	if (bOwn && HitObj->GetParking() && !HitObj->IsUnderConstruction())
	{
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O) && O->GetTemplate()->bJetAI) { Aircraft.Add(Id); }
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
			if (!O || !Logic->CanPlayerCommand(Local, *O)) { continue; }
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
			if (O && Logic->CanPlayerCommand(Local, *O) && Logic->CanSetRallyPoint(*O))
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
	if (HitObj)
	{
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O) && Logic->CanEnterObject(*O, *HitObj, true)) { Enterers.Add(Id); }
		}
	}
	if (Gatherers.Num() > 0)
	{
		Out.Action = EClickAction::Dock;
		Out.Units = MoveTemp(Gatherers);
	}
	else if (Enterers.Num() > 0)
	{
		Out.Action = EClickAction::Enter;
		Out.Units = MoveTemp(Enterers);
	}
	else if (Repairers.Num() > 0 && !HitObj->IsUnderConstruction())
	{
		Out.Action = EClickAction::Repair;
		Out.Units = MoveTemp(Repairers);
	}
	else if (Patients.Num() > 0)
	{
		Out.Action = EClickAction::GetRepaired;
		Out.Units = MoveTemp(Patients);
	}
	else if (RallyBuildings.Num() > 0 && GetTerrainUnderCursor(MX, MY, Out.Pos))
	{
		Out.Action = EClickAction::SetRally;
		Out.Units = MoveTemp(RallyBuildings);
	}
	else if (Aircraft.Num() > 0)
	{
		Out.Action = EClickAction::Land;
		Out.Units = MoveTemp(Aircraft);
	}
	else if (Dozers.Num() > 0)
	{
		Out.Action = EClickAction::ResumeConstruction;
		Out.Units = MoveTemp(Dozers);
	}
	else if (bOwn)
	{
		Out.Action = EClickAction::SelectOwn;
	}
	else if (Steered.Num() > 0 && GetTerrainUnderCursor(MX, MY, Out.Pos))
	{
		Out.Action = EClickAction::SteerPower;
		Out.Units = MoveTemp(Steered);
	}
	else if (Out.bCanCommand)
	{
		TArray<FGenObjectID> Movers, Shooters;
		bool bSalvager = false;
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O) && !O->GetTemplate()->bForbidPlayerCommands)
			{
				if (O->CanMove()) { Movers.Add(Id); }
				bSalvager |= O->IsKindOf(TEXT("SALVAGER"));
				// ("For the case of dozers, we don't ever want to see an attack cursor unless it's valid on a mine":
				// only what has a weapon for the thing clicked attacks it.)
				if ((O->HasWeapons() && (!HitObj || O->HasWeaponFor(*HitObj))) || (HitObj && O->SpawnsCanAttack(*HitObj))) { Shooters.Add(Id); }
			}
		}
		// On an enemy: attack it. Anywhere else: go there.
		if (Shooters.Num() > 0 && HitObj && HitObj->IsAttackable() && Logic->GetRelationship(Local, HitObj->GetOwner()) == EGenRelationship::Enemies)
		{
			Out.Action = EClickAction::Attack;
			Out.Units = MoveTemp(Shooters);
		}
		else if (HitObj && HitObj->GetTemplate()->Crate.Type == EGenCrate::Salvage && bSalvager && !IsForceAttackMode() && Movers.Num() > 0)
		{
			// canSelectionSalvage: a salvager among them, and a salvage crate clicked.
			Out.Action = EClickAction::Salvage;
			Out.Units = MoveTemp(Movers);
			Out.Pos = HitObj->GetPosition();
		}
		else if (Movers.Num() > 0 && GetTerrainUnderCursor(MX, MY, Out.Pos))
		{
			Out.Action = EClickAction::Move;
			Out.Units = MoveTemp(Movers);
		}
	}
	else
	{
		Out.Action = HitObj && HitObj->IsSelectable() ? EClickAction::SelectOther : EClickAction::Deselect;
	}
}

FString AGenPlayerController::CursorForClick(FGenObjectID Hit, float MX, float MY) const
{
	// InGameUI::createCommandHint, MOUSEMODE_DEFAULT: the hint for what the click would be, as a cursor.
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	FClickEval Eval;
	EvaluateClick(Hit, MX, MY, Eval);
	const FGenObject* HitObj = Logic ? Logic->FindObject(Eval.Hit) : nullptr;
	switch (Eval.Action)
	{
	case EClickAction::Dock: return TEXT("Dock");
	case EClickAction::Enter:
		if (HitObj && HitObj->IsKindOf(TEXT("HEAL_PAD")))
		{
			return TEXT("GetHealed");
		}
		return HitObj && Logic->GetRelationship(Logic->GetLocalPlayer(), HitObj->GetOwner()) == EGenRelationship::Enemies ? TEXT("EnterAggressive") : TEXT("EnterFriendly");
	case EClickAction::Repair: return TEXT("DoRepair");
	case EClickAction::GetRepaired: return TEXT("GetRepaired");
	case EClickAction::SetRally: return TEXT("SetRallyPoint");
	case EClickAction::Land: return TEXT("EnterFriendly");
	case EClickAction::ResumeConstruction: return TEXT("ResumeConstruction");
	case EClickAction::SelectOwn: return TEXT("Select"); // "drawSelectable && obj->isLocallyControlled()"
	case EClickAction::SteerPower: return TEXT("ParticleUplinkCannon"); // MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION_HINT
	case EClickAction::Attack: return TEXT("AttackObj");
	case EClickAction::ForceAttackObject: return TEXT("ForceAttackObj");
	case EClickAction::ForceAttackGround: return TEXT("ForceAttackGround");
	case EClickAction::ImpossibleAttack: return TEXT("GenericInvalid"); // MSG_IMPOSSIBLE_ATTACK_HINT
	case EClickAction::AddWaypoint: return TEXT("Waypoint");
	case EClickAction::Move: return TEXT("Move");
	case EClickAction::Salvage: return TEXT("Move"); // MSG_DO_SALVAGE_HINT
	case EClickAction::Nothing: return Eval.bCanCommand ? TEXT("GenericInvalid") : TEXT("Arrow");
	default: return TEXT("Arrow"); // nothing of ours to command: somebody else's thing, or the ground
	}
}

bool AGenPlayerController::IsForceAttackMode() const
{
	return bTestForceAttack || IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
}

bool AGenPlayerController::IsWaypointMode() const
{
	return bTestWaypoint || IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt);
}

AGenPlayerController::EForceAttack AGenPlayerController::CanForceAttack(const FGenObject& U, const FGenObject* Victim, const FVector3f& Pos) const
{
	// canObjectForceAttack: Object::isAbleToAttack, then WeaponSet::getAbleToAttackSpecificObject(ATTACK_NEW_TARGET_FORCED)
	// on a thing, or getAbleToUseWeaponAgainstTarget on a spot.
	if (U.IsDead() || !U.HasWeapons() || U.IsDisabled() || U.IsUnderConstruction())
	{
		return EForceAttack::NotPossible;
	}
	// "Let's just do this for all immobile objects": what can't move (or rides inside something) must reach it
	// from where it is.
	const bool bMustReach = !U.CanMove() || U.IsContained();
	auto InReach = [&U](const FGenWeaponTemplate* W, const FVector3f& At, float TargetRadius)
	{
		const float Dist = FVector2f::Distance(FVector2f(U.GetPosition().X, U.GetPosition().Y), FVector2f(At.X, At.Y));
		return W && Dist - U.GetTemplate()->GetBoundingCircleRadius() - TargetRadius <= U.GetAttackRange(*W);
	};
	if (Victim)
	{
		if (Victim == &U || Victim->IsDead() || !Victim->IsTargetable())
		{
			return EForceAttack::NotPossible;
		}
		// "If an object is stealthed and hasn't been detected yet, then it is not a valid target" - unless it is
		// one of ours ("sameOwnerForceAttack") or a disguise ("force-attack allows you to attack disguised things").
		if (Victim->IsStealthedAndUndetected() && Victim->GetOwner() != U.GetOwner() && !Victim->IsDisguised())
		{
			return EForceAttack::NotPossible;
		}
		if (!U.HasWeaponFor(*Victim))
		{
			return EForceAttack::InvalidShot;
		}
		bool bReach = !bMustReach;
		for (int32 s = 0; s < 3 && !bReach; ++s)
		{
			bReach = InReach(U.GetWeapon(s).Template, Victim->GetPosition(), Victim->GetTemplate()->GetBoundingCircleRadius());
		}
		return bReach ? EForceAttack::Possible : EForceAttack::InvalidShot;
	}
	// A spot: "almost every combat unit can force attack a position" - with a weapon that fires at the ground,
	// and in reach if it can't move.
	bool bAntiGround = false, bReach = !bMustReach;
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenWeaponTemplate* W = U.GetWeapon(s).Template;
		if (W && W->bAntiGround)
		{
			bAntiGround = true;
			bReach = bReach || InReach(W, Pos, 0.f);
		}
	}
	if (!bAntiGround || !bReach)
	{
		return EForceAttack::InvalidShot;
	}
	return U.GetTemplate()->bJetAI ? EForceAttack::NotPossible : EForceAttack::Possible; // (a plane's runs at a spot: not in the port)
}

FString AGenPlayerController::TestClickAt(const FVector3f& Pos, bool bForce, bool bWaypoint)
{
	bTestForceAttack = bForce;
	bTestWaypoint = bTestWaypoint || bWaypoint;
	FVector2D Screen(0.0, 0.0);
	const FGenObjectID Hit = TestPickAt(Pos, &Screen);
	FClickEval Eval;
	EvaluateClick(Hit, float(Screen.X), float(Screen.Y), Eval);
	const FString Cursor = CursorForClick(Hit, float(Screen.X), float(Screen.Y));
	ExecuteClick(Eval, false);
	bTestForceAttack = false;
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const FGenObject* O = Logic ? Logic->FindObject(Hit) : nullptr;
	return FString::Printf(TEXT("on %s: action %d by %d unit(s), cursor %s"), O ? *FString::Printf(TEXT("#%u %s"), Hit, *O->GetTemplate()->Name) : TEXT("the ground"),
		int32(Eval.Action), Eval.Units.Num(), *Cursor);
}

bool AGenPlayerController::CanFireWeaponAt(const FGenObject& Victim) const
{
	// ActionManager::canFireWeaponAtObject: not ours or a friend's; and a shot at a driver
	// (DAMAGE_KILLPILOT) only at a vehicle that can be sniped.
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !TargetingButton || Victim.IsDead() || Victim.GetOwner() == Logic->GetLocalPlayer()
		|| Logic->GetRelationship(Logic->GetLocalPlayer(), Victim.GetOwner()) == EGenRelationship::Allies)
	{
		return false;
	}
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* U = Logic->FindObject(Id);
		const FGenWeaponTemplate* W = (U && TargetingButton->WeaponSlot >= 0 && TargetingButton->WeaponSlot < 3) ? U->GetWeapon(TargetingButton->WeaponSlot).Template : nullptr;
		if (W && W->DamageType.Equals(TEXT("KILL_PILOT"), ESearchCase::IgnoreCase) && !Logic->CanSnipeVehicle(*U, Victim))
		{
			return false;
		}
	}
	return true;
}

bool AGenPlayerController::IsTargetingAcceptable() const
{
	// (What FireTargetedPower would take.)
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || !TargetingButton)
	{
		return false;
	}
	const FGenObject* Over = Logic->FindObject(MousedOver);
	if (TargetingPower.IsEmpty())
	{
		if (TargetingButton->Command == EGenCommand::FireWeapon)
		{
			return (TargetingButton->bNeedTargetPos && !TargetingButton->bNeedTargetObject) ? bTargetingValid : (Over && CanFireWeaponAt(*Over));
		}
		return bTargetingValid;
	}
	if (TargetingButton->bNeedTargetObject && !TargetingButton->bNeedTargetPos)
	{
		return Over && !Over->IsDead();
	}
	return bTargetingValid;
}

FString AGenPlayerController::ChooseCursor(float MX, float MY, bool bHasMouse, int32& OutDirection) const
{
	OutDirection = 0;
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const AGenGameMode* GM = GetWorld()->GetAuthGameMode<AGenGameMode>();
	if (!Logic || !Logic->IsRunning() || (GM && GM->IsShellActive()))
	{
		return TEXT("Arrow");
	}
	if (!ScrollScreen.IsNearlyZero())
	{
		// InGameUI::setScrolling; W3DMouse::setCursorDirection: "Frame 0 is assumed to point right and continue clockwise."
		const int32 Num = FMath::Max(1, FGenCursors::Get().GetNumDirections(TEXT("Scroll")));
		const double Theta = FMath::Fmod(FMath::Atan2(ScrollScreen.Y, ScrollScreen.X) + 2.0 * UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI);
		OutDirection = int32(Theta / (2.0 * UE_DOUBLE_PI / double(Num)) + 0.5) % Num;
		return TEXT("Scroll");
	}
	if (!bHasMouse || ControlBar.IsOverPanel(FVector2D(MX, MY)))
	{
		return TEXT("Arrow"); // "set cursor to normal if there is a window under the cursor"
	}
	if (bBoxSelecting)
	{
		// (m_isSelecting: no hint is made, the cursor stays what it was.)
		return FGenCursors::Get().GetCurrentName().IsEmpty() ? FString(TEXT("Arrow")) : FGenCursors::Get().GetCurrentName();
	}
	if (PlacingTemplate)
	{
		return TEXT("Build"); // (Mouse.ini gives it no picture: the building is under the mouse instead)
	}
	if (TargetingButton)
	{
		// MOUSEMODE_GUI_COMMAND: the button's cursor, or its other one where it can't be used; a button
		// that names none gets the cross ("Target").
		const FString& Name = IsTargetingAcceptable() ? TargetingButton->CursorName : TargetingButton->InvalidCursorName;
		return FGenCursors::Get().Find(Name) ? Name : FString(TEXT("Target"));
	}
	return HoverCursor;
}

bool AGenPlayerController::IsOnScreen(const FGenObject& O) const
{
	// W3DView::iterateDrawablesInRegion: by where the thing stands.
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	const FVector3f P = Logic->GetRenderPosition(O);
	FVector2D S;
	return ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z), S) && S.X >= 0.0 && S.Y >= 0.0 && S.X <= double(ViewX) && S.Y <= double(ViewY);
}

int32 AGenPlayerController::SelectMatching(bool bAcrossMap)
{
	// InGameUI::selectMatchingAcrossRegion: every kind of thing of ours that is selected, and then all of
	// ours of those kinds (buildings are picked one by one: Object::isMassSelectable).
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return -1;
	}
	const int32 Local = Logic->GetLocalPlayer();
	TSet<const FGenThingTemplate*> Kinds;
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && O->GetOwner() == Local)
		{
			Kinds.Add(O->GetTemplate()->Canonical());
		}
	}
	if (Kinds.Num() == 0)
	{
		return -1; // "nothing useful selected to begin with"
	}
	int32 Num = 0;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || O->GetOwner() != Local || !Kinds.Contains(O->GetTemplate()->Canonical()) || !O->IsSelectable() || O->IsKindOf(TEXT("STRUCTURE"))
			|| Selection.Contains(Id) || !Logic->IsVisibleToLocalPlayer(*O) || (!bAcrossMap && !IsOnScreen(*O)))
		{
			continue;
		}
		Selection.Add(Id);
		Logic->FlashSelected(Id);
		++Num;
	}
	return Num;
}

int32 AGenPlayerController::SelectAllOfKind(bool bAircraft, bool bAcrossMap)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const int32 Local = Logic->GetLocalPlayer();
	int32 Num = 0;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || O->GetOwner() != Local || !O->IsSelectable() || O->IsKindOf(TEXT("STRUCTURE")) || O->IsKindOf(TEXT("DOZER")) || O->IsKindOf(TEXT("HARVESTER"))
			|| O->IsKindOf(TEXT("IGNORES_SELECT_ALL")) || (bAircraft && !O->IsKindOf(TEXT("AIRCRAFT"))) || Selection.Contains(Id)
			|| !Logic->IsVisibleToLocalPlayer(*O) || (!bAcrossMap && !IsOnScreen(*O)))
		{
			continue;
		}
		Selection.Add(Id);
		Logic->FlashSelected(Id);
		++Num;
	}
	return Num;
}

void AGenPlayerController::SelectNextIdleWorker()
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return;
	}
	const int32 Local = Logic->GetLocalPlayer();
	TArray<FGenObjectID> Idle;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && Logic->CanPlayerCommand(Local, *O) && O->IsKindOf(TEXT("DOZER")) && !O->IsDead() && O->IsAIIdle() && (!O->GetTemplate()->bIsSupplyTruck || O->GetSupplyState() == EGenSupplyState::Idle))
		{
			Idle.Add(Id);
		}
	}
	if (Idle.Num() == 0)
	{
		return;
	}
	// "if(getSelectCount() == 0 || getSelectCount() > 1)" the first; with one of them selected, the one after it.
	FGenObjectID Next = Idle[0];
	if (Selection.Num() == 1)
	{
		const int32 At = Idle.IndexOfByKey(Selection[0]);
		if (At != INDEX_NONE)
		{
			Next = Idle[(At + 1) % Idle.Num()];
		}
	}
	const FGenObject* O = Logic->FindObject(Next);
	if (const FGenObject* Container = O && O->IsContained() ? Logic->FindObject(O->GetContainedBy()) : nullptr)
	{
		O = Container; // "If our idle worker is contained by anything, we need to select the container instead."
	}
	if (!O)
	{
		return;
	}
	Selection.Reset();
	Selection.Add(O->GetID());
	Logic->FlashSelected(O->GetID());
	PlayVoice({ TEXT("VoiceSelect") }, Selection);
	LookAt(O->GetPosition());
}

int32 AGenPlayerController::SelectAllUnits(bool bAircraft)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic)
	{
		return 0;
	}
	// "We need to deselect all the units if any of the units we have selected are incompatible with the
	// select all type we are triggering."
	for (FGenObjectID Id : Selection)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O)
		{
			continue;
		}
		const bool bOut = O->GetOwner() != Logic->GetLocalPlayer() || O->IsKindOf(TEXT("DOZER")) || O->IsKindOf(TEXT("HARVESTER")) || O->IsKindOf(TEXT("IGNORES_SELECT_ALL"));
		if (bOut || (bAircraft ? !O->IsKindOf(TEXT("AIRCRAFT")) : O->IsKindOf(TEXT("STRUCTURE"))))
		{
			Selection.Reset();
			break;
		}
	}
	// InGameUI::selectAllUnitsByType: across the screen, and if that adds none, across the map.
	int32 Num = SelectAllOfKind(bAircraft, false);
	if (Num == 0)
	{
		Num = SelectAllOfKind(bAircraft, true);
	}
	if (Num > 0)
	{
		PlayVoice({ TEXT("VoiceSelect") }, Selection);
	}
	return Num;
}

int32 AGenPlayerController::GetGroupOf(FGenObjectID Id) const
{
	for (int32 Group = 0; Group < 10; ++Group)
	{
		if (Groups[Group].Contains(Id))
		{
			return Group;
		}
	}
	return INDEX_NONE;
}

void AGenPlayerController::GroupKey(int32 Group, EGroupKey Kind)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!Logic || Group < 0 || Group >= 10)
	{
		return;
	}
	const int32 Local = Logic->GetLocalPlayer();
	if (Kind == EGroupKey::Create)
	{
		// MSG_CREATE_TEAM: what of ours is selected is that group from now on, and in no other.
		TArray<FGenObjectID> Members;
		for (FGenObjectID Id : Selection)
		{
			const FGenObject* O = Logic->FindObject(Id);
			if (O && Logic->CanPlayerCommand(Local, *O))
			{
				Members.Add(Id);
			}
		}
		for (TArray<FGenObjectID>& Other : Groups)
		{
			Other.RemoveAll([&Members](FGenObjectID Id) { return Members.Contains(Id); });
		}
		Groups[Group] = MoveTemp(Members);
		return;
	}
	TArray<FGenObjectID>& Members = Groups[Group];
	Members.RemoveAll([Logic, Local](FGenObjectID Id)
	{
		const FGenObject* O = Logic->FindObject(Id);
		return !O || O->IsDead() || !Logic->CanPlayerCommand(Local, *O); // (Squad::getLiveObjects)
	});
	// "check for double-press to jump view": the same group again within 20 frames.
	const double Now = FPlatformTime::Seconds();
	const bool bAgain = Group == LastGroup && Now - LastGroupTime < 20.0 / 30.0;
	LastGroup = Group;
	LastGroupTime = Now;
	if (Kind == EGroupKey::View || bAgain)
	{
		if (Members.Num() > 0)
		{
			LookAt(Logic->FindObject(Members.Last())->GetPosition());
		}
		return;
	}
	if (Kind == EGroupKey::Select)
	{
		Selection.Reset();
	}
	else if (Selection.Num() > 0)
	{
		// "Can't select other units if you have a structure selected. So deselect the structure".
		const FGenObject* First = Logic->FindObject(Selection[0]);
		if (First && First->IsKindOf(TEXT("STRUCTURE")))
		{
			Selection.Reset();
		}
	}
	for (FGenObjectID Id : Members)
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (O && O->IsSelectable() && !Selection.Contains(Id))
		{
			Selection.Add(Id);
			Logic->FlashSelected(Id);
		}
	}
	PlayVoice({ TEXT("VoiceSelect") }, Selection);
	PlayCarBombVoice(FindSpeaker(Selection), TEXT("TerroristInCarSelectVoice"));
}

FString AGenPlayerController::TestSelectionKey(const FString& What, int32 Group)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	int32 Result = 0;
	if (What.Equals(TEXT("create"), ESearchCase::IgnoreCase)) { GroupKey(Group, EGroupKey::Create); }
	else if (What.Equals(TEXT("select"), ESearchCase::IgnoreCase)) { GroupKey(Group, EGroupKey::Select); }
	else if (What.Equals(TEXT("add"), ESearchCase::IgnoreCase)) { GroupKey(Group, EGroupKey::Add); }
	else if (What.Equals(TEXT("view"), ESearchCase::IgnoreCase)) { GroupKey(Group, EGroupKey::View); }
	else if (What.Equals(TEXT("match"), ESearchCase::IgnoreCase)) { Result = SelectMatching(false); }
	else if (What.Equals(TEXT("matchmap"), ESearchCase::IgnoreCase)) { Result = SelectMatching(true); }
	else if (What.Equals(TEXT("all"), ESearchCase::IgnoreCase)) { Result = SelectAllUnits(false); }
	else if (What.Equals(TEXT("aircraft"), ESearchCase::IgnoreCase)) { Result = SelectAllUnits(true); }
	else if (What.Equals(TEXT("double"), ESearchCase::IgnoreCase) && Selection.Num() > 0)
	{
		const FGenObjectID First = Selection[0];
		Selection.Reset();
		Selection.Add(First);
		Result = SelectMatching(false);
	}
	else { return TEXT("NO SUCH KEY"); }
	int32 InGroups = 0;
	for (const TArray<FGenObjectID>& G : Groups) { InGroups += G.Num(); }
	const AGenCameraPawn* Cam = GetCameraPawn();
	const FVector At = Cam ? GenCoords::FromUE(Cam->GetTarget()) : FVector::ZeroVector;
	return FString::Printf(TEXT("%d more, %d selected now, %d units in groups, the view at (%.0f, %.0f)"), Result, Selection.Num(), InGroups, At.X, At.Y);
}

const FString& AGenPlayerController::GetCursorName() const
{
	return FGenCursors::Get().GetCurrentName();
}

void AGenPlayerController::UpdateCursor(float MX, float MY, bool bHasMouse)
{
	int32 Direction = 0;
	const FString Name = ChooseCursor(MX, MY, bHasMouse, Direction);
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	FGenCursors& Cursors = FGenCursors::Get();
	const bool bChanged = !Name.Equals(Cursors.GetCurrentName(), ESearchCase::IgnoreCase);
	Cursors.Show(this, Name, Direction, ViewY);
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	UE_CLOG(bChanged && Logic && Logic->IsTracing(), LogGenerals, Log, TEXT("  [cursor] %s"), *Name);
}

void AGenPlayerController::TestHoverAt(const FVector3f& Pos)
{
	FVector2D Screen(0.0, 0.0);
	MousedOver = TestPickAt(Pos, &Screen);
	MousedOverSince = FPlatformTime::Seconds() - 10.0; // (as if it had rested there a while: its name shows)
	HoverCursor = CursorForClick(MousedOver, float(Screen.X), float(Screen.Y));
	if (TargetingButton)
	{
		TargetingPos = Pos;
		bTargetingValid = true;
	}
	bTestCursor = true;
	TestCursorPos = Screen;
	UpdateCursor(float(Screen.X), float(Screen.Y), true);
}
