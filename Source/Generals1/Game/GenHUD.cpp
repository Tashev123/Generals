#include "Game/GenHUD.h"
#include "Core/GenPerf.h"
#include "Core/GenCoords.h"
#include "Game/GenControlBar.h"
#include "Game/GenGameMode.h"
#include "Game/GenPlayerController.h"
#include "Game/GenRadar.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenMapScripts.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "W3D/GenAssets.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Game/GenCameraPawn.h"
#include "Game/GenCursors.h"
#include "Game/GenHudStyle.h"
#include "Game/GenControlBarScheme.h"
#include "Core/GenWnd.h"
#include "W3D/GenTextureLoader.h"
#include "Generals1.h"
#include "Misc/ScopeExit.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"

void AGenHUD::DrawGroundCircle(const FVector& CenterUE, float RadiusUE, const FLinearColor& Color)
{
	APlayerController* PC = GetOwningPlayerController();
	const int32 Segments = 28;
	FVector2D Prev = FVector2D::ZeroVector;
	bool bPrevValid = false;
	for (int32 i = 0; i <= Segments; ++i)
	{
		const float A = 2.f * PI * i / Segments;
		const FVector P = CenterUE + FVector(FMath::Cos(A) * RadiusUE, FMath::Sin(A) * RadiusUE, 0.f);
		FVector2D S;
		const bool bValid = PC->ProjectWorldLocationToScreen(P, S);
		if (bValid && bPrevValid)
		{
			DrawLine(Prev.X, Prev.Y, S.X, S.Y, Color, 1.5f);
		}
		Prev = S;
		bPrevValid = bValid;
	}
}

void AGenHUD::DrawHUD()
{
	GEN_PERF_SCOPE("R.HUD");
	Super::DrawHUD();
	AGenPlayerController* PC = Cast<AGenPlayerController>(GetOwningPlayerController());
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!PC || !Logic || !Canvas)
	{
		return;
	}
	// (Drawn after the world's tick is over, when the logic step has the floor: the HUD reads the logic.)
	UGenGameLogic::FGameScope InLogic(Logic);
	// Before a game there is only the shell (GenShell.cpp).
	AGenGameMode* GM = GetWorld()->GetAuthGameMode<AGenGameMode>();
	if (GM && GM->GetPhase() == EGenGamePhase::Server)
	{
		return;
	}
	BeginShell(PC);
	if (GM && GM->GetPhase() == EGenGamePhase::Menu)
	{
		DrawShellMenu(GM); // the main menu, the skirmish setup or the online screen
		DrawVideoOverlay(); // (the frame rate, and the "keep these settings?" countdown after a display change)
		DrawOptions();
		EndShell();
		return;
	}
	if (GM && GM->GetPhase() == EGenGamePhase::NetLobby)
	{
		DrawSkirmishMenu(GM);
		DrawVideoOverlay();
		DrawOptions();
		EndShell();
		return;
	}
	if (GM && GM->GetPhase() == EGenGamePhase::Loading)
	{
		DrawLoading();
		EndShell();
		return;
	}
	if (GM && GM->IsResumingSave())
	{
		DrawResumeScreen(GM);
		EndShell();
		return;
	}
	if (!Logic->IsRunning())
	{
		EndShell();
		return;
	}

	// Drawable::drawHealthBar: "only draw health for selected drawables and drawables that have been moused
	// over by the cursor". (Nothing is drawn on the ground round a selected thing: the original has no such ring.)
	TArray<FGenObjectID, TInlineAllocator<64>> Shown;
	// (None of it in a cinematic, or while the scripts have the icons off: OPTIONS_SET_DRAWICON_UI_MODE.)
	const bool bCinematic = Cine.bLetterbox || (Logic->GetMapScripts() && !Logic->GetMapScripts()->ShouldDrawIconUI());
	if (!bCinematic)
	{
		Shown.Append(PC->GetSelection());
	}
	const int32 NumSelected = Shown.Num();
	if (!bCinematic && PC->GetMousedOver() != GenInvalidObjectID)
	{
		Shown.AddUnique(PC->GetMousedOver());
	}
	const float BarScale = GetIconScale();
	for (int32 ShownIndex = 0; ShownIndex < Shown.Num(); ++ShownIndex)
	{
		const FGenObjectID Id = Shown[ShownIndex];
		const bool bSelected = ShownIndex < NumSelected;
		const FGenObject* O = Logic->FindObject(Id);
		if (!O)
		{
			continue;
		}
		if (Logic->IsShroudOn() && O->GetLocalShroud() > EGenObjectShroud::PartialClear)
		{
			continue; // W3DView: no health bar for what is only remembered (it may not even be there any more)
		}
		const FVector3f P = Logic->GetRenderPosition(*O);
		const FGenThingTemplate* T = O->GetTemplate();
		const bool bOwn = O->GetOwner() == Logic->GetLocalPlayer();
		const FLinearColor RingColor = bOwn ? FLinearColor(0.2f, 1.f, 0.2f) : FLinearColor(1.f, 0.85f, 0.2f);

		// W3DWaypointBuffer::drawWaypoints: in waypoint mode (Alt) a selected unit's route, from where it is
		// through the points it has still to reach, a node at each.
		if (bSelected && PC->IsWaypointMode() && !T->IsKindOf(TEXT("IGNORED_IN_GUI")) && O->GetRouteIndex() < O->GetRoutePoints().Num())
		{
			const FLinearColor RouteColor(0.25f, 0.5f, 1.f);
			FVector2D From;
			bool bFrom = PC->ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z + 1.f), From);
			const TArray<FVector2f>& Route = O->GetRoutePoints();
			for (int32 i = FMath::Max(0, O->GetRouteIndex() - 1); i < Route.Num(); ++i)
			{
				if (i < O->GetRouteIndex() && !O->IsMoving())
				{
					continue; // (the point it has just set off for is still to be reached while it moves)
				}
				const FVector NodeUE = GenCoords::ToUE(Route[i].X, Route[i].Y, Logic->GetGroundHeight(Route[i].X, Route[i].Y) + 1.f);
				DrawGroundCircle(NodeUE, 3.f * GenCoords::UnitsToCm, RouteColor);
				FVector2D To;
				const bool bTo = PC->ProjectWorldLocationToScreen(NodeUE, To);
				if (bFrom && bTo)
				{
					DrawLine(From.X, From.Y, To.X, To.Y, RouteColor, 1.5f);
				}
				From = To;
				bFrom = bTo;
			}
		}

		// A selected factory shows where it sends its new units.
		if (bSelected && bOwn && O->HasRallyPoint())
		{
			const FVector3f& R = O->GetRallyPoint();
			const FVector RallyUE = GenCoords::ToUE(R.X, R.Y, R.Z + 1.f);
			const FLinearColor RallyColor(1.f, 0.9f, 0.3f);
			DrawGroundCircle(RallyUE, 7.f * GenCoords::UnitsToCm, RallyColor);
			DrawGroundCircle(RallyUE, 2.f * GenCoords::UnitsToCm, RallyColor);
			FVector2D From, To;
			const FVector3f Door = O->GetNaturalRallyPoint(false);
			if (PC->ProjectWorldLocationToScreen(GenCoords::ToUE(Door.X, Door.Y, Logic->GetGroundHeight(Door.X, Door.Y) + 1.f), From)
				&& PC->ProjectWorldLocationToScreen(RallyUE, To))
			{
				DrawLine(From.X, From.Y, To.X, To.Y, FLinearColor(1.f, 0.9f, 0.3f, 0.6f), 1.f);
				DrawText(TEXT("Rally point"), FLinearColor::Black, To.X + 9.f, To.Y - 17.f);
				DrawText(TEXT("Rally point"), RallyColor, To.X + 8.f, To.Y - 18.f);
			}
		}

		if (T->IsKindOf(TEXT("FORCEATTACKABLE")) || T->IsKindOf(TEXT("IGNORED_IN_GUI")) || O->GetMaxHealth() <= 0.f || O->GetHealth() <= 0.f)
		{
			continue;
		}
		FBox2D HealthRegion;
		if (GetHealthRegion(PC, Logic, *O, HealthRegion))
		{
			const float W = float(HealthRegion.GetSize().X);
			const float H = float(HealthRegion.GetSize().Y);
			FVector2D Top;
			const float Frac = FMath::Clamp(O->GetHealth() / O->GetMaxHealth(), 0.f, 1.f);
			FLinearColor Fill, Outline;
			if (O->IsUnderConstruction() || O->IsDisabled())
			{
				Fill = FLinearColor(0.f, Frac, 1.f);              // "blue to cyan"
				Outline = FLinearColor(0.f, Frac * 0.5f, 0.5f);
			}
			else
			{
				// "slowly go from green to red", pulled towards red when really damaged, towards green when whole
				float Red = Frac >= 0.5f ? 1.f - (Frac - 0.5f) / 0.5f : 1.f;
				float Green = Frac >= 0.5f ? 1.f : 1.f - (0.5f - Frac) / 0.5f;
				Outline = FLinearColor(Red * 0.5f, Green * 0.5f, 0.f);
				if (O->GetDamageState() >= 2)
				{
					Red = (1.f + Red) * 0.5f;
					Green *= 0.5f;
				}
				else if (O->GetDamageState() == 0)
				{
					Green = (1.f + Green) * 0.5f;
					Red *= 0.5f;
				}
				Fill = FLinearColor(Red, Green, 0.f);
			}
			// (The colours are the original's 0..255 values: as they are on the screen, not as light.)
			Fill = FLinearColor::FromSRGBColor(Fill.ToFColor(false));
			Outline = FLinearColor::FromSRGBColor(Outline.ToFColor(false));
			const float X0 = float(HealthRegion.Min.X), Y0 = float(HealthRegion.Min.Y);
			const float Line = BarScale;
			DrawRect(Outline, X0, Y0, W, Line);
			DrawRect(Outline, X0, Y0 + H - Line, W, Line);
			DrawRect(Outline, X0, Y0, Line, H);
			DrawRect(Outline, X0 + W - Line, Y0, Line, H);
			DrawRect(Fill, X0 + Line, Y0 + Line, (W - 2.f * Line) * Frac, H - 2.f * Line);
			Top.X = X0 + W * 0.5f;
			Top.Y = Y0;
			// ("isSelected() || getMousedOverDrawableID() == getID()", and its controlling player the local one.)
			if (bOwn)
			{
				DrawPips(Logic, *O, HealthRegion);
			}
			if (!bSelected)
			{
				continue;
			}

			// Drawable::drawUIText: the number of the group it is in, at the left end of its health bar ...
			const int32 Group = bOwn ? PC->GetGroupOf(Id) : INDEX_NONE;
			if (Group != INDEX_NONE)
			{
				const FString Number = FString::FromInt(Group);
				DrawText(Number, FLinearColor::Black, Top.X - W / 2 - 11.f, Top.Y - 6.f);
				DrawText(Number, FLinearColor::White, Top.X - W / 2 - 12.f, Top.Y - 7.f);
			}
			// ... and "draw an F, here" - beside the health bar of a unit that is in a formation.
			if (bOwn && O->GetFormationID() != 0)
			{
				DrawText(TEXT("F"), FLinearColor::Black, Top.X + W / 2 + 7.f, Top.Y - 6.f);
				DrawText(TEXT("F"), RingColor, Top.X + W / 2 + 6.f, Top.Y - 7.f);
			}

			// Supplies: what a pile is still worth, or how many boxes a gatherer has on board.
			FString Supplies;
			if (T->bSupplyWarehouse)
			{
				Supplies = FString::Printf(TEXT("$%d"), O->GetBoxes() * FGenGameData::Get().ValuePerSupplyBox);
			}
			else if (T->bIsSupplyTruck && O->GetBoxes() > 0)
			{
				Supplies = FString::Printf(TEXT("%d / %d"), O->GetBoxes(), T->MaxBoxes);
			}
			if (!Supplies.IsEmpty())
			{
				const float TextX = Top.X - Supplies.Len() * 3.5f, TextY = Top.Y - 17.f;
				DrawText(Supplies, FLinearColor::Black, TextX + 1.f, TextY + 1.f);
				DrawText(Supplies, FLinearColor(1.f, 0.9f, 0.3f), TextX, TextY);
			}
		}
	}

	// (A gunship's target area and reticle are decals on the ground: AGenPlayerController::UpdateRadiusDecals.)

	// Floating text ("+$600" where supplies were delivered): rises, then fades.
	const double FrameTime = Logic->GetRenderFrameTime();
	const float UIScale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f);
	for (const FGenFloatingText& F : Logic->GetFloatingTexts())
	{
		const float Age = float((FrameTime - double(F.Frame)) / double(GenLogic::GetLogicFPS()));
		FVector2D S;
		if (F.bScreen || Age < 0.f || Age > 2.f || !PC->ProjectWorldLocationToScreen(GenCoords::ToUE(F.Pos.X, F.Pos.Y, F.Pos.Z + 20.f), S))
		{
			continue;
		}
		if (!Logic->IsPositionClear(F.Pos))
		{
			continue; // InGameUI: floating text only shows where the player is looking
		}
		const float Fade = FMath::Clamp((2.f - Age) / 0.6f, 0.f, 1.f);
		const float Size = 1.5f * UIScale;
		const float X = S.X - F.Text.Len() * 4.5f * Size, Y = S.Y - Age * 30.f * UIScale;
		FLinearColor Color = FMath::Lerp(F.Color, FLinearColor::White, 0.35f);
		Color.A = Fade;
		DrawText(F.Text, FLinearColor(0.f, 0.f, 0.f, Fade), X + 1.5f, Y + 1.5f, nullptr, Size);
		DrawText(F.Text, Color, X, Y, nullptr, Size);
	}

	// Drag selection box
	FVector2D A, B;
	if (PC->GetDragBox(A, B))
	{
		const FLinearColor C(0.2f, 1.f, 0.2f, 1.f);
		DrawLine(A.X, A.Y, B.X, A.Y, C, 1.f);
		DrawLine(B.X, A.Y, B.X, B.Y, C, 1.f);
		DrawLine(B.X, B.Y, A.X, B.Y, C, 1.f);
		DrawLine(A.X, B.Y, A.X, A.Y, C, 1.f);
		DrawRect(FLinearColor(0.2f, 1.f, 0.2f, 0.08f), FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y), FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y));
	}

	DrawScriptFade(Logic);
	if (!bCinematic)
	{
		DrawStatusIcons(PC, Logic);
		DrawBombIcons(PC, Logic);
	}
	if (!Cine.bLetterbox)
	{
		DrawPlacement(PC, Logic);
		DrawTargeting(PC, Logic);
		DrawAnnouncements(Logic);
		DrawSuperweaponTimers(Logic);
		DrawControlBar(PC, Logic); // (the radar too: it has a section of the bar)
		DrawGenerals(PC, Logic);
	}
	DrawLetterbox();
	DrawMilitaryCaption(Logic);
	DrawIntroSkip(GM);
	if (Logic->IsNetworkGame())
	{
		DrawNetOverlay(GM, Logic);
	}
	if (GM && GM->GetPhase() == EGenGamePhase::Score)
	{
		DrawScoreScreen(GM, Logic);
	}
	else
	{
		DrawGameOver(Logic);
		// (The in-game menu opens from the bar's options button: the click is the player controller's business.)
		if (GM)
		{
			DrawPause(GM, Logic);
			DrawQuitMenu(GM);
		}
	}
	FVector2D TestCursor;
	const bool bTestCursor = PC->GetTestCursor(TestCursor);
	if (!GM || GM->GetPhase() == EGenGamePhase::Playing)
	{
		DrawMouseOverName(PC, Logic, bTestCursor ? TestCursor : PC->GetMousePos());
	}
	if (!GM || GM->GetPhase() == EGenGamePhase::Playing)
	{
		DrawVideoOverlay();
	}
	DrawOptions();
	if (bTestCursor)
	{
		DrawTestCursor(TestCursor);
	}
	EndShell();
}

void AGenHUD::DrawMouseOverName(AGenPlayerController* PC, UGenGameLogic* Logic, const FVector2D& Mouse)
{
	// The mouse has rested on something for Mouse.ini's TooltipDelayTime (800 ms): its name, on its owner's
	// colour (UseTooltipAltBackColor); a supply pile says what it still holds. Nothing for what the fog
	// covers, or for what has no name ("OBJECT:Prop is a blank string").
	const FGenObject* O = Logic->FindObject(PC->GetMousedOver());
	if (!O || PC->GetMousedOverSeconds() < 0.8 || (Logic->IsShroudOn() && O->GetLocalShroud() > EGenObjectShroud::PartialClear))
	{
		return;
	}
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenThingTemplate* T = O->GetTemplate();
	FString Name = T->DisplayName.IsEmpty() ? FString() : CD.GetText(T->DisplayName);
	if (Name.IsEmpty() || Name.Equals(T->DisplayName, ESearchCase::IgnoreCase))
	{
		return;
	}
	if (T->bSupplyWarehouse)
	{
		FString Worth = CD.GetText(TEXT("TOOLTIP:SupplyWarehouse"));
		if (Worth.Contains(TEXT("%d")))
		{
			Name += Worth.Replace(TEXT("%d"), *FString::FromInt(O->GetBoxes() * FGenGameData::Get().ValuePerSupplyBox));
		}
	}
	TArray<FString> Lines;
	Name.ParseIntoArrayLines(Lines, true);
	if (Lines.Num() == 0)
	{
		return;
	}
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f) * 1.1f;
	float Width = 0.f, LineHeight = 0.f;
	for (const FString& Line : Lines)
	{
		float W = 0.f, H = 0.f;
		GetTextSize(Line, W, H, nullptr, Scale);
		Width = FMath::Max(Width, W);
		LineHeight = FMath::Max(LineHeight, H);
	}
	const float Pad = 5.f * Scale;
	const FVector2D Size(Width + 2.f * Pad, LineHeight * Lines.Num() + 2.f * Pad);
	FVector2D Min(Mouse.X + 18.0 * Scale, Mouse.Y + 22.0 * Scale);
	Min.X = FMath::Clamp(Min.X, 0.0, double(Canvas->SizeX) - Size.X);
	Min.Y = FMath::Clamp(Min.Y, 0.0, double(Canvas->SizeY) - Size.Y);
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	FLinearColor Back = Players.IsValidIndex(O->GetOwner()) && !Players[O->GetOwner()].bNeutral ? Players[O->GetOwner()].Color * 0.3f : FLinearColor(0.02f, 0.02f, 0.02f);
	Back.A = 0.88f;
	const FLinearColor Border = FLinearColor::FromSRGBColor(FColor(60, 60, 155));
	DrawRect(Back, Min.X, Min.Y, Size.X, Size.Y);
	DrawLine(Min.X, Min.Y, Min.X + Size.X, Min.Y, Border, 1.f);
	DrawLine(Min.X + Size.X, Min.Y, Min.X + Size.X, Min.Y + Size.Y, Border, 1.f);
	DrawLine(Min.X + Size.X, Min.Y + Size.Y, Min.X, Min.Y + Size.Y, Border, 1.f);
	DrawLine(Min.X, Min.Y + Size.Y, Min.X, Min.Y, Border, 1.f);
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const float X = float(Min.X) + Pad, Y = float(Min.Y) + Pad + LineHeight * i;
		DrawText(Lines[i], FLinearColor::Black, X + 1.f, Y + 1.f, nullptr, Scale);
		DrawText(Lines[i], FLinearColor::FromSRGBColor(FColor(220, 220, 220)), X, Y, nullptr, Scale);
	}
}

void AGenHUD::DrawTestCursor(const FVector2D& Pos)
{
	int32 Frame = 0;
	const FGenCursorImage* Image = FGenCursors::Get().GetCurrent(Frame);
	if (!Image)
	{
		return;
	}
	TObjectPtr<UTexture2D>& Texture = CursorTextures.FindOrAdd(FString::Printf(TEXT("%s#%d"), *Image->File, Frame));
	if (!Texture)
	{
		Texture = UTexture2D::CreateTransient(Image->Width, Image->Height, PF_B8G8R8A8);
		if (!Texture)
		{
			return;
		}
		Texture->SRGB = true;
		Texture->Filter = TF_Nearest;
		void* Data = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Data, Image->Frames[Frame].GetData(), Image->Width * Image->Height * sizeof(FColor));
		Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
		Texture->UpdateResource();
	}
	const float Scale = float(FGenCursors::GetScale(Canvas->SizeY));
	DrawTexture(Texture, float(Pos.X) - Image->HotSpot.X * Scale, float(Pos.Y) - Image->HotSpot.Y * Scale, Image->Width * Scale, Image->Height * Scale,
		0.f, 0.f, 1.f, 1.f, FLinearColor::White, BLEND_Translucent);
}

float AGenHUD::GetIconScale() const
{
	// (The original's icons and bars are so many pixels, on a screen of about 1024 x 768. Whole pixels of them, here too.)
	return FMath::Max(1.f, FMath::RoundToFloat(Canvas->SizeY / 768.f));
}

bool AGenHUD::GetHealthRegion(AGenPlayerController* PC, UGenGameLogic* Logic, const FGenObject& O, FBox2D& Out) const
{
	// Object::getHealthBoxPosition ("max height above position + 10") and getHealthBoxDimensions ("just add the major and
	// minor axes"), computeHealthRegion. The original sizes it in pixels over the zoom, which at the height it looks from is
	// about a world unit a pixel: here it is that wide in the world, whatever the screen and however far the view is pulled back.
	const FGenThingTemplate* T = O.GetTemplate();
	const FVector3f P = Logic->GetRenderPosition(O);
	// (The angry mob's nexus: over the middle of its mob, and "pos.z += 20;// dear God, I confess my kluge, and repent.")
	const FVector3f& BoxOffset = O.GetHealthBoxOffset();
	const float MobLift = T->IsKindOf(TEXT("MOB_NEXUS")) ? 20.f : 0.f;
	const FVector BarWorld = GenCoords::ToUE(P.X + BoxOffset.X, P.Y + BoxOffset.Y, P.Z + BoxOffset.Z + T->GeometryHeight + 10.f + MobLift);
	const float Size = FMath::Clamp(T->GeometryMajorRadius + (T->IsBox() ? T->GeometryMinorRadius : T->GeometryMajorRadius), 20.f, 150.f);
	const FVector CameraRight = PC->PlayerCameraManager ? FRotationMatrix(PC->PlayerCameraManager->GetCameraRotation()).GetScaledAxis(EAxis::Y) : FVector::RightVector;
	FVector2D Top, Side;
	if (!PC->ProjectWorldLocationToScreen(BarWorld, Top) || !PC->ProjectWorldLocationToScreen(BarWorld + CameraRight * double(Size) * GenCoords::UnitsToCm, Side))
	{
		return false;
	}
	const float BarScale = GetIconScale();
	const float W = FMath::Max(20.f * BarScale, float(FMath::Abs(Side.X - Top.X)));
	const float H = 3.f * BarScale;
	const FVector2D Min(FMath::RoundToFloat(float(Top.X) - W * 0.45f), FMath::RoundToFloat(float(Top.Y) - H * 0.5f));
	Out = FBox2D(Min, Min + FVector2D(W, H));
	return true;
}

namespace
{
	/** Where an animation is (Anim2D::tryNextFrame). Beyond the original: also the image after this one and how far
	 *  on towards it - faded in over this one, it makes the few images of an icon move smoothly. */
	struct FAnimAt
	{
		int32 Image = 0, Next = 0;
		float Blend = 0.f;
	};

	/** With so many milliseconds to an image, and how far on it started. */
	FAnimAt GetAnimAt(double NowFrames, int32 DelayMS, int32 NumImages, bool bPingPong, int32 Start)
	{
		const int32 FramesPerImage = FMath::Max(1, FMath::CeilToInt32(float(DelayMS) * float(GenLogic::GetLogicFPS()) / 1000.f)); // (parseDurationUnsignedShort)
		const int32 Period = bPingPong ? 2 * NumImages - 2 : NumImages;
		const double Steps = NowFrames / double(FramesPerImage);
		const int32 Whole = int32(Steps);
		auto ImageAt = [&](int32 Step)
		{
			Step = (Step + Start) % Period;
			return Step < NumImages ? Step : Period - Step;
		};
		return FAnimAt{ ImageAt(Whole), ImageAt(Whole + 1), float(Steps - double(Whole)) };
	}
}

void AGenHUD::DrawStatusIcons(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	const float Scale = GetIconScale();
	// (The user, 2026-11-02: the original's sizes are too big here. Sized by the screen, about 40 and 27 pixels on 1440 lines.)
	const float AnimScale = float(Canvas->SizeY) / 1440.f;
	constexpr float WrenchSize = 0.7f, CrossSize = 0.85f;
	const double Now = Logic->GetRenderFrameTime();
	const uint32 Frame = Logic->GetFrame();
	const uint32 HealingDisplayFrames = uint32(GenLogic::GetLogicFPS()) * 3; // HEALING_ICON_DISPLAY_TIME
	const FGenCommandData& Images = FGenCommandData::Get();
	// An image, its top left corner given by how far along the bar and how far over it, in shares of its size.
	auto PlaceImage = [&](const FString& Name, float X, float Y, float ShareX, float ShareY, float ImageScale, FBox2D& OutBox)
	{
		const FGenMappedImage* Image = Images.FindImage(Name);
		if (!Image)
		{
			return false;
		}
		const FVector2D Size(Image->Size.X * ImageScale, Image->Size.Y * ImageScale);
		const FVector2D Min(X - Size.X * ShareX, Y - Size.Y * ShareY);
		OutBox = FBox2D(Min, Min + Size);
		return true;
	};
	auto DrawImage = [&](const FString& Name, float X, float Y, float ShareX, float ShareY, float ImageScale)
	{
		FBox2D Box;
		if (PlaceImage(Name, X, Y, ShareX, ShareY, ImageScale, Box))
		{
			DrawMappedImage(Name, Box);
		}
	};
	// One of an animation's images, and the next one fading in over it.
	auto DrawAnim = [&](const TCHAR* Base, int32 First, const FAnimAt& At, float X, float Y, float ShareX, float ShareY, float ImageScale)
	{
		const FString Name = FString::Printf(TEXT("%s%03d"), Base, First + At.Image);
		FBox2D Box;
		if (PlaceImage(Name, X, Y, ShareX, ShareY, ImageScale, Box))
		{
			DrawMappedImage(Name, Box);
			if (At.Next != At.Image)
			{
				DrawMappedImage(FString::Printf(TEXT("%s%03d"), Base, First + At.Next), Box, FLinearColor(1.f, 1.f, 1.f, At.Blend));
			}
		}
	};
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || O->IsDead() || O->IsHidden() || O->IsProjectile() || O->GetTemplate()->IsKindOf(TEXT("IGNORED_IN_GUI")))
		{
			continue;
		}
		const FGenThingTemplate* T = O->GetTemplate();
		// drawHealing: "see if healing has been done to us recently", "because so many things init health early in game".
		const bool bHealing = Frame > HealingDisplayFrames && Frame - O->GetLastHealingFrame() <= HealingDisplayFrames
			&& O->GetHealth() != O->GetMaxHealth() && !O->IsBeingSold() && !T->IsKindOf(TEXT("NO_HEAL_ICON"));
		const bool bEnthusiastic = O->HasBonusCondition(TEXT("ENTHUSIASTIC"));
		const bool bDisabled = O->ShowsDisabledIcon();
		const int32 Rank = O->GetVeterancyLevel();
		if ((!bHealing && !bEnthusiastic && !bDisabled && Rank <= 0)
			|| (Logic->IsShroudOn() && O->GetLocalShroud() > EGenObjectShroud::PartialClear)
			|| Logic->GetStealthLook(*O) == EGenStealthLook::Invisible)
		{
			continue; // (W3DView: no icons over what the fog of war covers, or over what is stealthed)
		}
		FBox2D Bar;
		if (!GetHealthRegion(PC, Logic, *O, Bar) || Bar.Max.X < -64.0 || Bar.Max.Y < -64.0 || Bar.Min.X > Canvas->SizeX + 64.0 || Bar.Min.Y > Canvas->SizeY + 64.0)
		{
			continue;
		}
		const float Left = float(Bar.Min.X), Top = float(Bar.Min.Y), Bottom = float(Bar.Max.Y), BarWidth = float(Bar.GetSize().X);
		const int32 RandomStart = int32(Id * 2654435761u >> 16); // ("RandomizeStartFrame", of a thing that is drawn anew every frame)
		if (bHealing)
		{
			// Standing on the bar, three quarters along. A wrench for what is mended; for the rest, a red cross.
			if (T->IsKindOf(TEXT("STRUCTURE")) || T->IsKindOf(TEXT("VEHICLE")))
			{
				DrawAnim(TEXT("SCPWrench"), 0, GetAnimAt(Now, 60, 10, true, RandomStart), Left + BarWidth * 0.75f, Top, 0.5f, 1.f, AnimScale * WrenchSize);
			}
			else
			{
				DrawAnim(TEXT("SCPCross"), 0, GetAnimAt(Now, 30, 16, false, RandomStart), Left + BarWidth * 0.75f, Top, 0.5f, 1.f, AnimScale * CrossSize);
			}
		}
		if (bEnthusiastic)
		{
			// drawEnthusiastic: under the bar, a quarter along; "Subliminal" for what has the gold border.
			const float ImageScale = T->IsKindOf(TEXT("STRUCTURE")) || T->IsKindOf(TEXT("HUGE_VEHICLE")) ? 1.f : (T->IsKindOf(TEXT("VEHICLE")) ? 0.75f : 0.5f);
			DrawAnim(O->HasBonusCondition(TEXT("SUBLIMINAL")) ? TEXT("SCPCrossB") : TEXT("SCPCross"), 3, GetAnimAt(Now, 100, 9, true, RandomStart),
				Left + BarWidth * 0.25f, Bottom, 0.5f, -0.25f, AnimScale * CrossSize * ImageScale);
		}
		if (bDisabled)
		{
			// drawDisabled: the lightning, over the left end of the bar.
			DrawAnim(TEXT("SCPLightning"), 0, GetAnimAt(Now, 66, 16, false, 0), Left, Bottom - float(Bar.GetSize().Y), 0.f, 1.f, AnimScale * CrossSize);
		}
		if (Rank > 0)
		{
			// drawVeterancy: off the right of the health box ("healthBoxWidth * scale * 0.5f", 1.3 over the zoom), everybody's.
			const float CenterX = Left + BarWidth * 0.45f, CenterY = (Top + Bottom) * 0.5f;
			DrawImage(FString::Printf(TEXT("SCVeter%d"), FMath::Min(Rank, 3)), CenterX + BarWidth * 0.65f + Scale, CenterY + Scale, 0.f, 0.f, Scale);
		}
	}
}

void AGenHUD::DrawPips(UGenGameLogic* Logic, const FGenObject& O, const FBox2D& HealthRegion)
{
	const FGenCommandData& Images = FGenCommandData::Get();
	const float Scale = GetIconScale();
	const float Left = float(HealthRegion.Min.X), Y = float(HealthRegion.GetCenter().Y);
	// "Left justify with health bar min": a row of them, a pixel apart, from where the bar is down.
	auto DrawRow = [&](int32 NumTotal, int32 NumFull, const TCHAR* Full, const TCHAR* Empty, float FullY, const FLinearColor& FirstColor, const FLinearColor& OtherColor, int32 NumFirst)
	{
		const FGenMappedImage* Image = Images.FindImage(Empty);
		if (!Image)
		{
			return;
		}
		const FVector2D Size(Image->Size.X * Scale, Image->Size.Y * Scale);
		for (int32 i = 0; i < NumTotal; ++i)
		{
			const FVector2D Min(Left + i * (Size.X + Scale), i < NumFull ? FullY : Y + Scale);
			DrawMappedImage(i < NumFull ? Full : Empty, FBox2D(Min, Min + Size), i < NumFull ? (i < NumFirst ? FirstColor : OtherColor) : FLinearColor::White);
		}
	};
	// (WeaponSet::findAmmoPipShowingWeapon)
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenWeaponInstance& WI = O.GetWeapon(s);
		if (WI.Template && WI.Template->bShowsAmmoPips)
		{
			DrawRow(WI.Template->ClipSize, WI.AmmoInClip, TEXT("SCPAmmoFull"), TEXT("SCPAmmoEmpty"), Y + Scale, FLinearColor::White, FLinearColor::White, 0);
			break;
		}
	}
	if (O.GetContain() && Logic->ShowsContainPips(O))
	{
		// How full a transport, a garrisoned building or the tunnel network is: green for infantry, blue for the rest. "If empty, don't show nothin'."
		const int32 NumFull = Logic->GetContainCount(O);
		if (NumFull > 0)
		{
			int32 NumInfantry = 0;
			for (FGenObjectID Rider : Logic->GetRiders(O))
			{
				const FGenObject* R = Logic->FindObject(Rider);
				NumInfantry += R && R->IsKindOf(TEXT("INFANTRY")) ? 1 : 0;
			}
			DrawRow(Logic->GetContainMax(O), NumFull, TEXT("SCPPipFull"), TEXT("SCPPipEmpty"), Y, FLinearColor(0.f, 1.f, 0.f), FLinearColor(0.f, 0.f, 1.f), NumInfantry);
		}
	}
}

void AGenHUD::DrawBombIcons(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// Drawable::drawBombed. A charge has no model ("No drawable because it's invisible"), and is stealthed from the enemy:
	// this is what shows it - the BombRemote animation over where it sits, and on a timed one BombTimed counting down on
	// top of that. (A bar's worth of the health box, 20 wide at the least, on an 800 x 600 screen.)
	const float ScreenScale = Canvas->SizeY / 600.f;
	const float Size = 13.f * ScreenScale;
	const double FramesPerSecond = double(GenLogic::GetLogicFPS());
	const double Now = Logic->GetRenderFrameTime();
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || !O->GetTemplate()->StickyBomb.bValid || O->IsDead() || O->GetStickyTarget() == GenInvalidObjectID
			|| (Logic->IsShroudOn() && O->GetLocalShroud() > EGenObjectShroud::PartialClear)
			|| Logic->GetStealthLook(*O) == EGenStealthLook::Invisible)
		{
			continue;
		}
		const FVector3f P = Logic->GetRenderPosition(*O);
		FVector2D S;
		if (!PC->ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z + O->GetTemplate()->GetMaxHeightAbovePosition() + 10.f), S))
		{
			continue;
		}
		const FVector2D Min(S.X - Size * 0.5f, S.Y + 5.f * ScreenScale); // BOMB_ICON_EXTRA_OFFSET
		const FBox2D R(Min, Min + FVector2D(Size, Size));
		DrawMappedImage(int32(Now / FramesPerSecond) % 2 == 0 ? TEXT("SCPClock000") : TEXT("SCPClock001"), R);
		if (const uint32 Die = O->GetLifetimeDieFrame())
		{
			// ("this anim goes from N seconds down to zero": SCPClock043 is 20 seconds, SCPClock063 none.)
			const int32 SecondsLeft = FMath::Clamp(FMath::CeilToInt32((double(Die) - Now) / FramesPerSecond), 0, 20);
			DrawMappedImage(FString::Printf(TEXT("SCPClock%03d"), 63 - SecondsLeft), R);
		}
	}
}

void AGenHUD::DrawGameOver(UGenGameLogic* Logic)
{
	// The game is decided: say so, large, in the middle of the screen (the original goes to the score screen).
	const bool bLost = Logic->IsPlayerDefeated(Logic->GetLocalPlayer());
	if (Logic->IsGameOver() ? !Logic->IsGameOverAnnounced() : !bLost)
	{
		return;
	}
	const bool bWon = Logic->HasLocalPlayerWon();
	const FString Text = bWon ? TEXT("VICTORY") : TEXT("DEFEAT");
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f) * 6.f;
	const float X = Canvas->SizeX * 0.5f - Text.Len() * 4.6f * Scale, Y = Canvas->SizeY * 0.3f;
	DrawText(Text, FLinearColor::Black, X + 4.f, Y + 4.f, nullptr, Scale);
	DrawText(Text, bWon ? FLinearColor(1.f, 0.85f, 0.2f) : FLinearColor(0.9f, 0.15f, 0.1f), X, Y, nullptr, Scale);
}

void AGenHUD::DrawPause(AGenGameMode* GM, UGenGameLogic* Logic)
{
	// Test automation has no keyboard: a test click stands for the pause hotkey.
	if (GM && ShellTestHit(TEXT("Pause.Toggle")))
	{
		GM->TogglePause();
		return;
	}
	// (While the in-game menu is up, the menu itself says it; a network game never stops.)
	if (!GM || !Logic || !Logic->IsPaused() || GM->IsQuitMenuOpen())
	{
		return;
	}
	const FString Text = TEXT("GAME PAUSED");
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f) * 3.f;
	const float X = Canvas->SizeX * 0.5f - Text.Len() * 4.6f * Scale, Y = Canvas->SizeY * 0.18f;
	DrawText(Text, FLinearColor::Black, X + 2.f, Y + 2.f, nullptr, Scale);
	DrawText(Text, FLinearColor(1.f, 0.85f, 0.2f), X, Y, nullptr, Scale);
}

void AGenHUD::DrawTargeting(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// The special power being aimed: what to do.
	FVector3f Pos;
	float Radius = 0.f;
	bool bValid = false;
	const FGenCommandButton* Button = PC->GetTargeting(Pos, Radius, bValid);
	if (!Button)
	{
		return;
	}
	// (Its area on the ground is the button's radius cursor, a decal: AGenPlayerController::UpdateRadiusDecals.)
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f);
	FString Text = FString::Printf(TEXT("%s: left click the target, right click to cancel"), *FGenCommandData::Get().GetText(Button->TextLabel));
	Text.ReplaceInline(TEXT("\n"), TEXT(" "));
	const float X = Canvas->SizeX * 0.5f - Text.Len() * 4.2f * Scale, Y = 60.f * Scale;
	DrawText(Text, FLinearColor::Black, X + 1.5f, Y + 1.5f, nullptr, 1.2f * Scale);
	DrawText(Text, FLinearColor(1.f, 0.85f, 0.3f), X, Y, nullptr, 1.2f * Scale);
}

void AGenHUD::DrawSuperweaponTimers(UGenGameLogic* Logic)
{
	// InGameUI::drawSuperweaponTimers (InGameUI.ini: SuperweaponCountdownPosition X 0.90 Y 0.01, Arial 10, the
	// ready ones bold and flashing white every 500 ms): each player's superweapons with a PublicTimer - on a
	// finished building, and not waiting for a promotion ("m_hiddenByScience") - player by player, in the
	// owner's colour, the name up to the column and the time after it. "Also responsible for Eva saying
	// 'Superweapon is ready for launch'."
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenUpgradeData& UD = FGenUpgradeData::Get();
	const uint32 Frame = Logic->GetFrame();
	const float Column = Canvas->SizeX * 0.90f;
	const float BottomMargin = Canvas->SizeY * 0.82f;
	const float TextScale = 10.f * (Canvas->SizeY / 600.f) / 16.f; // (10 points at 800 x 600; the engine's line is ~16 at scale 1)
	const bool bFlash = FMath::Fmod(GetWorld()->GetRealTimeSeconds(), 1.0) < 0.5;
	float Y = Canvas->SizeY * 0.01f;
	bool bMarginExceeded = false;
	TSet<uint64> Seen;
	struct FEntry { const FGenObject* Obj; int32 Module; const FGenSpecialPowerTemplate* Power; };
	for (const FGenPlayer& P : Players)
	{
		TArray<FEntry> Entries;
		for (const FGenObject* O : Logic->GetObjectsOf(P.Index))
		{
			if (!O || O->IsDead() || O->IsUnderConstruction() || !O->IsKindOf(TEXT("STRUCTURE"))
				|| (Logic->GetMapScripts() && Logic->GetMapScripts()->IsPowerDisplayHidden(O->GetID())))
			{
				continue;
			}
			const TArray<FGenSpecialPowerModule>& Modules = O->GetTemplate()->PowerModules;
			for (int32 m = 0; m < Modules.Num(); ++m)
			{
				const FGenSpecialPowerTemplate* T = UD.FindPower(Modules[m].Power);
				if (T && T->bPublicTimer && (T->RequiredScience.IsEmpty() || Logic->HasScience(P.Index, T->RequiredScience)))
				{
					Entries.Add({ O, m, T });
				}
			}
		}
		// (InGameUI keeps them in a map by the power's name.)
		Entries.StableSort([](const FEntry& A, const FEntry& B) { return A.Power->Name < B.Power->Name; });
		for (const FEntry& E : Entries)
		{
			uint32 ReadyFrame = 0;
			const bool bReady = Logic->IsPowerReady(*E.Obj, E.Module, &ReadyFrame); // ("IsReady includes disabledness")
			const uint64 Key = (uint64(E.Obj->GetID()) << 8) | uint64(E.Module);
			Seen.Add(Key);
			bool& bPlayed = SuperweaponEvaPlayed.FindOrAdd(Key, false);
			if (bReady && !bPlayed)
			{
				if (Frame > 0)
				{
					Logic->EvaSuperweapon(TEXT("Ready"), *E.Obj, *E.Power);
				}
				bPlayed = true;
			}
			else if (!bReady)
			{
				bPlayed = false; // "Reset Eva for next time"
			}
			if (bMarginExceeded)
			{
				continue;
			}
			const FLinearColor Color = bReady && bFlash ? FLinearColor::White : P.Color;
			if (Y >= BottomMargin)
			{
				// "enforce bottom margin of tactical view"
				DrawText(TEXT("..."), FLinearColor::Black, Column + 1.f, Y + 1.f, nullptr, TextScale);
				DrawText(TEXT("..."), FLinearColor::White, Column, Y, nullptr, TextScale);
				bMarginExceeded = true;
				continue;
			}
			const int32 Secs = ReadyFrame > Frame ? int32((ReadyFrame - Frame) / uint32(GenLogic::GetLogicFPS())) : 0;
			FString Label = CD.GetText(TEXT("GUI:") + E.Power->Name);
			if (Label.StartsWith(TEXT("GUI:")))
			{
				Label = E.Power->Name; // (no such string)
			}
			const FString Name = Label + TEXT(": ");
			const FString Time = FString::Printf(TEXT("%d:%02d"), Secs / 60, Secs % 60);
			float W = 0.f, H = 0.f;
			GetTextSize(Name, W, H, nullptr, TextScale);
			DrawText(Name, FLinearColor::Black, Column - W + 1.f, Y + 1.f, nullptr, TextScale);
			DrawText(Name, Color, Column - W, Y, nullptr, TextScale);
			DrawText(Time, FLinearColor::Black, Column + 1.f, Y + 1.f, nullptr, TextScale);
			DrawText(Time, Color, Column, Y, nullptr, TextScale);
			Y += FMath::Max(H, 12.f);
		}
	}
	// InGameUI::drawNamedTimers (InGameUI.ini: NamedTimerCountdownPosition X 0.90 Y 0.666): what a script shows.
	if (const FGenMapScripts* Scripts = Logic->GetMapScripts(); Scripts && Scripts->AreNamedTimersShown())
	{
		float TimerY = Canvas->SizeY * 0.666f;
		for (const TPair<FString, FString>& Timer : Scripts->GetNamedTimers())
		{
			const int32 Secs = FMath::Max(0, Scripts->GetCounterValue(Timer.Key)) / GenLogic::GetLogicFPS();
			const FString Name = CD.GetText(Timer.Value) + TEXT(" ");
			// (A counter is a number: isCountdown is what makes it a time.)
			const FString Time = Scripts->IsNamedCounter(Timer.Key) ? FString::FromInt(Scripts->GetCounterValue(Timer.Key)) : FString::Printf(TEXT("%d:%02d"), Secs / 60, Secs % 60);
			float W = 0.f, H = 0.f;
			GetTextSize(Name, W, H, nullptr, TextScale);
			DrawText(Name, FLinearColor::Black, Column - W + 1.f, TimerY + 1.f, nullptr, TextScale);
			DrawText(Name, FLinearColor::White, Column - W, TimerY, nullptr, TextScale);
			DrawText(Time, FLinearColor::Black, Column + 1.f, TimerY + 1.f, nullptr, TextScale);
			DrawText(Time, FLinearColor::White, Column, TimerY, nullptr, TextScale);
			TimerY += FMath::Max(H, 12.f);
		}
	}
	for (auto It = SuperweaponEvaPlayed.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
}

void AGenHUD::DrawAnnouncements(UGenGameLogic* Logic)
{
	// Announcements ("Promoted: rank 2") at the top of the screen for a few seconds.
	const double FrameTime = Logic->GetRenderFrameTime();
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f);
	float Y = 96.f * Scale;
	for (const FGenFloatingText& F : Logic->GetFloatingTexts())
	{
		const float Age = float((FrameTime - double(F.Frame)) / double(GenLogic::GetLogicFPS()));
		if (!F.bScreen || Age < 0.f || Age > 5.f)
		{
			continue;
		}
		const float Fade = FMath::Clamp((5.f - Age) / 1.f, 0.f, 1.f);
		const float Size = 1.8f * Scale;
		const float X = Canvas->SizeX * 0.5f - F.Text.Len() * 4.6f * Size;
		FLinearColor Color = F.Color;
		Color.A = Fade;
		DrawText(F.Text, FLinearColor(0.f, 0.f, 0.f, Fade), X + 2.f, Y + 2.f, nullptr, Size);
		DrawText(F.Text, Color, X, Y, nullptr, Size);
		Y += 30.f * Scale;
	}
}

void AGenHUD::DrawPlacement(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// Footprint of the building being placed: green where it may go, red where it may not.
	FVector3f Pos;
	float Angle = 0.f;
	bool bLegal = false;
	const FGenThingTemplate* T = PC->GetPlacement(Pos, Angle, bLegal);
	if (!T)
	{
		return;
	}
	const float Major = T->GeometryMajorRadius, Minor = T->IsBox() ? T->GeometryMinorRadius : T->GeometryMajorRadius;
	const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);
	const FLinearColor Color = bLegal ? FLinearColor(0.2f, 1.f, 0.2f) : FLinearColor(1.f, 0.15f, 0.1f);
	const float Corners[4][2] = { { Major, Minor }, { -Major, Minor }, { -Major, -Minor }, { Major, -Minor } };
	FVector2D Screen[4];
	bool bValid[4];
	for (int32 i = 0; i < 4; ++i)
	{
		const float X = Pos.X + Corners[i][0] * C - Corners[i][1] * S;
		const float Y = Pos.Y + Corners[i][0] * S + Corners[i][1] * C;
		bValid[i] = PC->ProjectWorldLocationToScreen(GenCoords::ToUE(X, Y, Logic->GetGroundHeight(X, Y) + 1.f), Screen[i]);
	}
	for (int32 i = 0; i < 4; ++i)
	{
		const int32 j = (i + 1) % 4;
		if (bValid[i] && bValid[j])
		{
			DrawLine(Screen[i].X, Screen[i].Y, Screen[j].X, Screen[j].Y, Color, 2.f);
		}
	}
}

bool AGenHUD::DrawMappedImage(const FString& Name, const FBox2D& R, const FLinearColor& Tint)
{
	const FGenMappedImage* Img = Name.IsEmpty() || !R.bIsValid ? nullptr : FGenCommandData::Get().FindImage(Name);
	UTexture2D* Tex = Img ? UGenAssetSubsystem::Get()->GetTexture(Img->Texture) : nullptr;
	if (!Tex)
	{
		return false;
	}
	const FVector2D Size = R.GetSize();
	DrawTexture(Tex, FMath::RoundToFloat(R.Min.X), FMath::RoundToFloat(R.Min.Y), FMath::RoundToFloat(Size.X), FMath::RoundToFloat(Size.Y), Img->UV0.X, Img->UV0.Y, Img->UV1.X - Img->UV0.X, Img->UV1.Y - Img->UV0.Y, Tint);
	return true;
}

bool AGenHUD::DrawCameo(const FString& Name, const FBox2D& R, const FLinearColor& Tint, bool bGrey)
{
	// W3DGadgetPushButtonImageDraw: "'grayscale' disabled buttons using original color artwork".
	UTexture2D* Grey = bGrey ? GetGreyCameo(Name) : nullptr;
	if (Grey)
	{
		const FVector2D Size = R.GetSize();
		DrawTexture(Grey, FMath::RoundToFloat(R.Min.X), FMath::RoundToFloat(R.Min.Y), FMath::RoundToFloat(Size.X), FMath::RoundToFloat(Size.Y), 0.f, 0.f, 1.f, 1.f);
	}
	else if (!DrawMappedImage(Name, R, Tint))
	{
		return false;
	}
	const int32 Corner = BarNow && BarNow->Style ? FMath::RoundToInt32(BarNow->Style->GetCameoCorner() * BarNow->Scale) : 0;
	if (Corner > 0)
	{
		DrawBarPart(EGenHudPart::CellCorners, Corner, R);
	}
	return true;
}

UTexture2D* AGenHUD::GetGreyCameo(const FString& Name)
{
	if (const TObjectPtr<UTexture2D>* Found = GreyCameos.Find(Name))
	{
		return *Found;
	}
	if (GreyCameoFrame != GFrameCounter)
	{
		GreyCameoFrame = GFrameCounter;
		GreyCameoSpent = 0.0;
	}
	if (GreyCameoSpent > 0.002)
	{
		return nullptr;
	}
	const double Start = FPlatformTime::Seconds();
	UTexture2D* Tex = nullptr;
	const FGenMappedImage* Img = FGenCommandData::Get().FindImage(Name);
	const FGenImage* Sheet = Img ? GenTextureLoader::LoadGameImage(Img->Texture) : nullptr;
	if (Sheet)
	{
		const int32 X0 = FMath::RoundToInt32(Img->UV0.X * Sheet->Width), Y0 = FMath::RoundToInt32(Img->UV0.Y * Sheet->Height);
		const int32 X1 = FMath::RoundToInt32(Img->UV1.X * Sheet->Width), Y1 = FMath::RoundToInt32(Img->UV1.Y * Sheet->Height);
		FGenImage Pixels;
		Pixels.Width = X1 - X0;
		Pixels.Height = Y1 - Y0;
		if (Pixels.Width > 0 && Pixels.Height > 0 && X0 >= 0 && Y0 >= 0 && X1 <= Sheet->Width && Y1 <= Sheet->Height)
		{
			Pixels.Pixels.SetNumUninitialized(Pixels.Width * Pixels.Height);
			for (int32 Y = 0; Y < Pixels.Height; ++Y)
			{
				for (int32 X = 0; X < Pixels.Width; ++X)
				{
					// (A little darker than the original's: it lies on black glass, and is not to be what is looked at.)
					const FColor& C = Sheet->At(X0 + X, Y0 + Y);
					const uint8 V = uint8(FMath::Clamp((C.R * 0.3f + C.G * 0.59f + C.B * 0.11f) * 0.62f, 0.f, 255.f));
					Pixels.At(X, Y) = FColor(V, V, V, C.A);
				}
			}
			Tex = GenTextureLoader::CreateFromImage(Pixels, Name, true, false);
		}
	}
	GreyCameos.Add(Name, Tex);
	GreyCameoSpent += FPlatformTime::Seconds() - Start;
	return Tex;
}

UTexture2D* AGenHUD::GetBarPart(EGenHudPart Part, int32 Variant, const FIntPoint& Size)
{
	if (!BarNow || !BarNow->Style || !BarNow->Shape || Size.X <= 0 || Size.Y <= 0)
	{
		return nullptr;
	}
	const uint64 Key = (uint64(Part) << 56) | (uint64(uint32(Variant) & 0xFFFF) << 40) | (uint64(uint32(Size.X) & 0xFFFFF) << 20) | uint64(uint32(Size.Y) & 0xFFFFF);
	if (UTexture2D** Found = BarParts.Find(Key))
	{
		return *Found;
	}
	FGenImage Pixels;
	UTexture2D* Tex = BarNow->Style->BuildPart(*BarNow->Shape, Part, Variant, Size.X, Size.Y, BarNow->Scale, Pixels) ? GenTextureLoader::CreateFromImage(Pixels, TEXT("GenBarPart"), true, false) : nullptr;
	if (Tex)
	{
		BarPartTextures.Add(Tex);
	}
	BarParts.Add(Key, Tex);
	return Tex;
}

bool AGenHUD::DrawBarPart(EGenHudPart Part, int32 Variant, const FBox2D& R, const FLinearColor& Tint)
{
	if (!R.bIsValid)
	{
		return false;
	}
	const FIntPoint Pixels(FMath::RoundToInt32(R.GetSize().X), FMath::RoundToInt32(R.GetSize().Y));
	UTexture2D* Tex = GetBarPart(Part, Variant, Pixels);
	if (!Tex)
	{
		return false;
	}
	DrawTexture(Tex, FMath::RoundToFloat(R.Min.X), FMath::RoundToFloat(R.Min.Y), Pixels.X, Pixels.Y, 0.f, 0.f, 1.f, 1.f, Tint);
	return true;
}

void AGenHUD::DrawCapsule(UTexture2D* Texture, const FBox2D& Whole, double Done, bool bUpwards)
{
	const float X = FMath::RoundToFloat(Whole.Min.X), Y = FMath::RoundToFloat(Whole.Min.Y);
	const float W = FMath::RoundToFloat(Whole.GetSize().X), H = FMath::RoundToFloat(Whole.GetSize().Y);
	const float Length = bUpwards ? H : W, Cap = (bUpwards ? W : H) * 0.5f;
	const float Fill = FMath::Clamp(FMath::RoundToFloat(float(Done)), 0.f, Length);
	if (Fill <= 0.f)
	{
		return;
	}
	// (The stretch from the start, then the far end's cap brought back to where it has got to.)
	const float Body = FMath::Max(Fill - Cap, 0.f), Head = Fill - Body;
	if (bUpwards)
	{
		DrawTexture(Texture, X, Y + H - Body, W, Body, 0.f, (H - Body) / H, 1.f, Body / H);
		DrawTexture(Texture, X, Y + H - Fill, W, Head, 0.f, 0.f, 1.f, Head / H);
	}
	else
	{
		DrawTexture(Texture, X, Y, Body, H, 0.f, 0.f, Body / W, 1.f);
		DrawTexture(Texture, X + Body, Y, Head, H, (W - Head) / W, 0.f, Head / W, 1.f);
	}
}

bool AGenHUD::DrawBarPods(const FGenControlBar& Bar)
{
	if (!Bar.Shape || !Bar.Style)
	{
		return false;
	}
	if (BarPodsStyle != Bar.Style || BarPodsShape != Bar.Shape || BarPodsScale != Bar.Scale)
	{
		// (Another style, side, map shape or screen: nothing made so far fits it.)
		const double Start = FPlatformTime::Seconds();
		BarParts.Reset();
		BarPartTextures.Reset();
		BarPodsStyle = Bar.Style;
		BarPodsShape = Bar.Shape;
		BarPodsScale = Bar.Scale;
		BarPods.Reset();
		for (int32 Pod = 0; Pod < BarPodCount; ++Pod)
		{
			FGenImage Image;
			BarPods.Add(Bar.Style->BuildPod(*Bar.Shape, Pod, Bar.Scale, Image) ? GenTextureLoader::CreateFromImage(Image, TEXT("GenBarPod"), true, false) : nullptr);
		}
		UE_LOG(LogGenerals, Log, TEXT("Control bar sections: style %s, side %d, %.3f pixels to the unit, made in %.0f ms"), *Bar.Style->Id, int32(Bar.Shape->Side), Bar.Scale, (FPlatformTime::Seconds() - Start) * 1000.0);
	}
	const float Pad = FMath::RoundToFloat(GenBarLayout::ShadowPad * Bar.Scale);
	for (int32 Pod = 0; Pod < BarPods.Num(); ++Pod)
	{
		if (!BarPods[Pod])
		{
			return false;
		}
		DrawTexture(BarPods[Pod], float(Bar.PodOrigin[Pod].X) - Pad, float(Bar.PodOrigin[Pod].Y) - Pad, BarPods[Pod]->GetSizeX(), BarPods[Pod]->GetSizeY(), 0.f, 0.f, 1.f, 1.f);
	}
	return true;
}

void AGenHUD::DrawRemainingClock(const FBox2D& R, float Done, const FLinearColor& Color)
{
	// W3DDisplay::drawRemainingRectClock: what is still to come covered, from where the hand stands clockwise
	// round to twelve o'clock (the hand sweeping clockwise from twelve as it is done).
	if (!R.bIsValid || Done >= 1.f)
	{
		return;
	}
	const FVector2D C = R.GetCenter(), H = R.GetExtent();
	auto Edge = [&C, &H](double Angle)
	{
		// (Angle 0 at twelve o'clock, clockwise; screen y grows downwards.)
		const FVector2D D(FMath::Sin(Angle), -FMath::Cos(Angle));
		const double T = FMath::Min(FMath::Abs(D.X) > 1e-6 ? H.X / FMath::Abs(D.X) : 1e9, FMath::Abs(D.Y) > 1e-6 ? H.Y / FMath::Abs(D.Y) : 1e9);
		return C + D * T;
	};
	TArray<double> Angles;
	const double From = 2.0 * UE_DOUBLE_PI * FMath::Max(Done, 0.f), To = 2.0 * UE_DOUBLE_PI;
	Angles.Add(From);
	// (The corners are where the border turns: each one in range is a point of the fan.)
	const double Corner = FMath::Atan2(H.X, H.Y);
	const double Corners[4] = { Corner, UE_DOUBLE_PI - Corner, UE_DOUBLE_PI + Corner, 2.0 * UE_DOUBLE_PI - Corner };
	for (double A : Corners)
	{
		if (A > From && A < To)
		{
			Angles.Add(A);
		}
	}
	Angles.Add(To);
	Angles.Sort();
	TArray<FCanvasUVTri> Tris;
	for (int32 i = 0; i + 1 < Angles.Num(); ++i)
	{
		FCanvasUVTri T;
		T.V0_Pos = C;
		T.V1_Pos = Edge(Angles[i]);
		T.V2_Pos = Edge(Angles[i + 1]);
		T.V0_Color = T.V1_Color = T.V2_Color = Color;
		Tris.Add(T);
	}
	FCanvasTriangleItem Item(Tris, GWhiteTexture);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}

void AGenHUD::DrawBarText(const FString& Text, const FBox2D& R, const FLinearColor& Color, float PixelSize, bool bCenter)
{
	if (Text.IsEmpty() || !R.bIsValid)
	{
		return;
	}
	const float Pixels = FMath::Max(PixelSize, 6.f);
	const FVector2D Size = BarTextSize(Text, Pixels, false);
	const float X = bCenter ? float(R.GetCenter().X - Size.X * 0.5) : float(R.Min.X);
	DrawBarLine(Text, X, float(R.GetCenter().Y - Size.Y * 0.5), Color, Pixels, false);
}

void AGenHUD::DrawBarButton(const FGenBarButton& B, bool bHovered, float Scale, UGenGameLogic* Logic, const FGenControlBarScheme* Scheme)
{
	// W3DGadgetPushButtonImageDraw: the button's picture (greyed when it can't be used, brighter under the
	// mouse), the border colour of its kind (ControlBar::setCommandBarBorder), and the build-up clock.
	const FGenCommandData& CD = FGenCommandData::Get();
	const FVector2D Min = B.Rect.Min, Size = B.Rect.GetSize();
	FString Image = B.Image;
	if (Image.IsEmpty() && B.Button)
	{
		Image = B.Button->ButtonImage;
	}
	// (What is already researched or acquired stays bright: it is done, not unavailable. "If the button isn't 'ready' ...
	// we want to show the button in color with just the clock overlay".)
	const bool bCharging = !B.bEnabled && B.Recharge > 0.f;
	const FLinearColor Tint = B.bEnabled ? (bHovered ? FLinearColor(1.12f, 1.12f, 1.12f) : FLinearColor::White)
		: (B.bOwned ? FLinearColor(0.85f, 0.95f, 0.85f) : bCharging ? FLinearColor::White : FLinearColor(0.35f, 0.35f, 0.35f));
	if (!DrawCameo(Image, B.Rect, Tint, !B.bEnabled && !B.bOwned && !bCharging) && B.Button)
	{
		// No icon: fall back to the button's name.
		DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.9f), Min.X, Min.Y, Size.X, Size.Y);
		DrawText(CD.GetText(B.Button->TextLabel).Left(9), Tint, Min.X + 3, Min.Y + Size.Y * 0.35f, nullptr, 0.8f * Scale);
	}
	if (B.Recharge > 0.f)
	{
		// A special power recharging (ControlBar's inverse clock): the shade sweeps away as it gets ready.
		const FColor Clock = Scheme ? Scheme->BuildUpClockColor : FColor(0, 0, 0, 160);
		DrawRemainingClock(B.Rect, 1.f - B.Recharge, FLinearColor(Clock));
	}
	if (B.bOwned)
	{
		// Done: a green corner.
		const float M = 11.f * Scale;
		DrawRect(FLinearColor(0.15f, 0.85f, 0.25f, 0.95f), Min.X + Size.X - M - 1, Min.Y + 1, M, M);
	}
	FLinearColor Edge = FLinearColor::Transparent;
	if (B.Button && Scheme && B.Button->BorderType > 0)
	{
		const FColor Colors[5] = { FColor::Black, Scheme->BorderBuild, Scheme->BorderUpgrade, Scheme->BorderAction, Scheme->BorderSystem };
		Edge = FLinearColor(Colors[FMath::Clamp(int32(B.Button->BorderType), 0, 4)]);
	}
	if (bHovered && B.bEnabled)
	{
		Edge = FLinearColor(1.f, 0.9f, 0.4f);
	}
	// (On whole pixels, as the picture is.)
	const float PX = FMath::RoundToFloat(float(Min.X)), PY = FMath::RoundToFloat(float(Min.Y));
	const float PW = FMath::RoundToFloat(float(Size.X)), PH = FMath::RoundToFloat(float(Size.Y));
	const int32 Corner = BarNow && BarNow->Style ? FMath::RoundToInt32(BarNow->Style->GetCameoCorner() * BarNow->Scale) : 0;
	if (Edge.A > 0.f)
	{
		DrawBarPart(EGenHudPart::CellRing, Corner, B.Rect, FLinearColor(Edge.R, Edge.G, Edge.B, 0.85f));
	}
	if (B.Hotkey.IsValid())
	{
		// Its key (GenBarHotkeys.cpp), small in the top left corner on a dark tab - dimmer while it can't be used.
		const FString Key = B.Hotkey.GetShortText();
		const float Pixels = FMath::Max(9.f, 13.f * Scale);
		const FVector2D KeySize = BarTextSize(Key, Pixels, true);
		const float Pad = FMath::Max(1.f, 2.f * Scale);
		const float Inset = FMath::Max(1.f, FMath::RoundToFloat(Scale * 1.5f));
		const FVector2D TabMin(PX + Inset, PY + Inset);
		const FBox2D Tab(TabMin, TabMin + FVector2D(FMath::Max(KeySize.X + Pad * 2.0, KeySize.Y), KeySize.Y));
		DrawBarPart(EGenHudPart::CellFill, FMath::RoundToInt32(KeySize.Y * 0.28), Tab, FLinearColor(0.004f, 0.005f, 0.007f, 0.66f));
		DrawBarLine(Key, float(Tab.GetCenter().X - KeySize.X * 0.5), PY + Inset, FLinearColor(1.f, 1.f, 1.f, B.bEnabled ? 0.95f : 0.5f), Pixels, true);
	}
}

void AGenHUD::DrawControlBar(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	const FGenControlBar& Bar = PC->GetControlBar();
	BarNow = &Bar;
	const FGenControlBarScheme* Scheme = Bar.Scheme;
	const float UIScale = Bar.Scale * (600.f / 1080.f); // (the old interface's size: one at 1080 lines)
	const FVector2D Mouse = PC->GetMousePos();
	const FGenBarButton* Hovered = Bar.HitTest(Mouse);
	const int32 Local = Logic->GetLocalPlayer();
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();

	// The special power shortcuts' rail, on the screen's right edge.
	if (Bar.ShortcutRail.bIsValid)
	{
		DrawBarPart(EGenHudPart::ShortcutRail, Bar.Shortcuts.Num(), Bar.ShortcutRail.ExpandBy(double(FMath::RoundToFloat(GenBarLayout::ShadowPad * Bar.Scale))));
	}
	for (const FGenBarButton& B : Bar.Shortcuts)
	{
		DrawBarButton(B, &B == Hovered, UIScale, Logic, Scheme);
	}
	if (!Scheme)
	{
		return;
	}

	if (!DrawBarPods(Bar))
	{
		return;
	}
	DrawRadar(PC, Logic); // (LeftHUD, in its section's glass)

	// ControlBar::updateRadarAttackGlow: for 150 frames after the alarm the light goes on and off every 15 (it is the
	// window's disabled image that is lit, and the alarm disables it).
	{
		const uint32 FPS = uint32(GenLogic::GetLogicFPS());
		const uint32 Since = Logic->GetFrame() - Logic->GetLastUnderAttackFrame();
		const bool bLit = Logic->HasUnderAttackGlow() && Since < 150u * FPS / 30u && (Since / FMath::Max(15u * FPS / 30u, 1u)) % 2u == 0u;
		const int32 Glow = FMath::RoundToInt32(8.f * Bar.Scale);
		if (bLit)
		{
			DrawBarPart(EGenHudPart::LampOn, Glow, Bar.LampRect.ExpandBy(double(Glow)));
		}
		else
		{
			DrawBarPart(EGenHudPart::LampOff, 0, Bar.LampRect);
		}
	}

	// Money (MoneyDisplay) and power (W3DPowerDraw).
	if (Players.IsValidIndex(Local))
	{
		FString Amount = FString::FromInt(Logic->GetMoney(Local));
		for (int32 i = Amount.Len() - 3; i > 0; i -= 3)
		{
			Amount.InsertAt(i, TEXT(','));
		}
		const float Pixels = 10.5f * Bar.Scale;
		const FString Sign = TEXT("$");
		const FVector2D SignSize = BarTextSize(Sign, Pixels, false), AmountSize = BarTextSize(Amount, Pixels, true);
		const float Gap = 1.6f * Bar.Scale, Whole = float(SignSize.X + AmountSize.X) + Gap;
		const float X = float(Bar.MoneyRect.GetCenter().X) - Whole * 0.5f, Y = float(Bar.MoneyRect.GetCenter().Y - AmountSize.Y * 0.5);
		DrawBarLine(Sign, X, Y, FLinearColor(1.f, 1.f, 1.f, 0.45f), Pixels, false);
		DrawBarLine(Amount, X + float(SignSize.X) + Gap, Y, FLinearColor(0.84f, 0.86f, 0.88f), Pixels, true);
		DrawPowerBar(Bar, Logic);
		DrawExpBar(Bar, Logic);
	}

	// The command window, or what is under way there.
	if (Bar.bVisible)
	{
		for (const FGenBarButton& B : Bar.Buttons)
		{
			DrawBarButton(B, &B == Hovered, UIScale, Logic, Scheme);
		}
		if (Bar.Progress >= 0.f && Bar.ProgressRect.bIsValid)
		{
			// (GadgetProgressBar: the share done, filled from the left: a line of light under the text, as the power bar is.)
			const double Line = FMath::RoundToDouble(5.0 * Bar.Scale);
			const FBox2D Track(FVector2D(Bar.ProgressRect.Min.X, Bar.ProgressRect.Max.Y - Line), Bar.ProgressRect.Max);
			DrawBarPart(EGenHudPart::CellFill, FMath::RoundToInt32(Line * 0.5), Track, FLinearColor(1.f, 1.f, 1.f, 0.07f));
			if (UTexture2D* Capsule = GetBarPart(EGenHudPart::ExpFill, 0, FIntPoint(FMath::RoundToInt32(Track.GetSize().X), FMath::RoundToInt32(Line))))
			{
				DrawCapsule(Capsule, Track, Track.GetSize().X * double(Bar.Progress), false);
			}
		}
		if (!Bar.Status.IsEmpty())
		{
			DrawBarText(Bar.Status, Bar.StatusRect.bIsValid ? Bar.StatusRect : Bar.Panel, FLinearColor::White, 14.f * Bar.Scale, true);
		}
	}

	// The right panel: the production queue, or the selected unit's portrait and upgrades, or the side's picture.
	if (Bar.bShowQueue)
	{
		const FGenObject* Subject = Logic->FindObject(Bar.Subject);
		for (int32 i = 0; i < Bar.QueueSlots.Num(); ++i)
		{
			if (!Bar.Queue.IsValidIndex(i))
			{
				// (An empty cell. The scheme's QueueButtonImage isn't in the game's data: a faint outline.)
				const int32 Corner = Bar.Style ? FMath::RoundToInt32(Bar.Style->GetCameoCorner() * Bar.Scale) : 0;
				DrawBarPart(EGenHudPart::CellRing, Corner, Bar.QueueSlots[i], FLinearColor(1.f, 1.f, 1.f, 0.07f));
			}
		}
		for (const FGenBarButton& Q : Bar.Queue)
		{
			DrawBarButton(Q, &Q == Hovered, UIScale, Logic, Scheme);
			if (Q.QueueIndex == 0 && Subject && Subject->GetProductionQueue().Num() > 0 && (Q.Thing || Q.Upgrade))
			{
				// ControlBar::updateContextCommand: the first entry's inverse clock (GadgetButtonDrawInverseClock).
				const float Total = Q.Upgrade ? Q.Upgrade->BuildTime * float(GenLogic::GetLogicFPS()) : Logic->CalcTimeToBuild(*Q.Thing, Subject->GetOwner());
				const float Done = FMath::Clamp(Subject->GetProductionQueue()[0].FramesDone / FMath::Max(Total, 1.f), 0.f, 1.f);
				DrawRemainingClock(Q.Rect, Done, FLinearColor(Scheme->BuildUpClockColor));
			}
		}
	}
	else if (!Bar.Portrait.IsEmpty())
	{
		DrawCameo(Bar.Portrait, Bar.PortraitRect, FLinearColor::White);
		for (const FGenBarButton& U : Bar.UnitUpgrades)
		{
			DrawCameo(U.Image, U.Rect, U.bOwned ? FLinearColor::White : FLinearColor(0.3f, 0.3f, 0.3f), !U.bOwned);
		}
	}
	else if (!Bar.bVisible)
	{
		DrawMappedImage(Scheme->RightHUDImage, Bar.RightHUDRect);
	}

	// The bar's own buttons, and the general's (flashing while there are points to spend).
	const bool bFlash = FMath::Fmod(GetWorld()->GetRealTimeSeconds(), 1.0) < 0.5;
	for (const FGenBarButton& B : Bar.SystemButtons)
	{
		const int32 State = !B.bEnabled ? HudKeyDisabled : &B == Hovered ? HudKeyHover : HudKeyNormal;
		DrawBarPart(B.System == EGenBarSystem::Options ? EGenHudPart::OptionsKey : EGenHudPart::WorkerKey, State, B.Rect);
	}
	if (Bar.bHasGenerals && Players.IsValidIndex(Local))
	{
		const bool bLit = Hovered == &Bar.GeneralsButton || Bar.bGeneralsOpen || (Bar.bGenStarFlash && bFlash);
		DrawBarPart(EGenHudPart::GeneralKey, bLit ? HudKeyHover : HudKeyNormal, Bar.GeneralsButton.Rect);
	}

	// (The promotions panel draws its own tooltips, on top of itself. Tooltips keep their size whatever the bar's: one at 1080 lines.)
	if (Hovered && !Hovered->bGeneralsToggle && Hovered->Science.IsEmpty() && Hovered->System == EGenBarSystem::None)
	{
		DrawTooltip(*Hovered, Mouse, Canvas->SizeY / 1080.f, Logic);
	}
}

void AGenHUD::DrawPowerBar(const FGenControlBar& Bar, UGenGameLogic* Logic)
{
	// W3DPowerDraw: production on a log scale (PowerBarBase 7, PowerBarIntervals 3) in green, yellow within
	// PowerBarYellowRange of running out, red when short; the slider where the consumption is.
	const FBox2D R = Bar.PowerRect;
	if (!R.bIsValid)
	{
		return;
	}
	int32 Production = 0, Consumption = 0;
	Logic->GetEnergy(Logic->GetLocalPlayer(), Production, Consumption);
	constexpr double Base = 7.0, Intervals = 3.0;
	constexpr int32 YellowRange = 5;
	const EGenHudPart FillPart = (Consumption > Production - YellowRange && Consumption <= Production) ? EGenHudPart::PowerFillTight
		: Consumption > Production ? EGenHudPart::PowerFillShort : EGenHudPart::PowerFillGood;
	const FVector2D Size = R.GetSize();
	auto Range = [&Size](double Value) { return Value > 0.0 ? FMath::Min(Size.X, FMath::LogX(Base, Value) * (Size.X / Intervals)) : 0.0; };
	const double Fill = Range(double(Production));
	if (UTexture2D* Capsule = GetBarPart(FillPart, 0, FIntPoint(FMath::RoundToInt32(Size.X), FMath::RoundToInt32(Size.Y))))
	{
		// (A line of light, and a thin needle where the consumption is. "Log(1) == 0, but we need to show something for 1 power used.")
		DrawCapsule(Capsule, R, Fill, false);
		if (Fill > 0.0)
		{
			const double Needle = Range(Consumption == 1 ? 1.5 : double(Consumption));
			const float NeedleW = FMath::Max(2.f, FMath::RoundToFloat(1.2f * Bar.Scale));
			const float Over = 1.5f * Bar.Scale;
			DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.6f), FMath::RoundToFloat(float(R.Min.X + Needle)) - NeedleW * 0.5f - 1.f, float(R.Min.Y) - Over, NeedleW + 2.f, float(Size.Y) + Over * 2.f);
			DrawRect(FLinearColor(0.72f, 0.75f, 0.78f), FMath::RoundToFloat(float(R.Min.X + Needle)) - NeedleW * 0.5f, float(R.Min.Y) - Over, NeedleW, float(Size.Y) + Over * 2.f);
		}
	}
}

void AGenHUD::DrawExpBar(const FGenControlBar& Bar, UGenGameLogic* Logic)
{
	// W3DCommandBarGenExpDraw: the general's experience towards the next rank, filling GeneralsExp from the bottom.
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const TArray<FGenRank>& Ranks = FGenUpgradeData::Get().GetRanks();
	const FBox2D R = Bar.ExpRect;
	if (R.bIsValid && Players.IsValidIndex(Local) && Ranks.IsValidIndex(Players[Local].RankLevel) && Players[Local].RankLevel >= 1)
	{
		const FGenPlayer& P = Players[Local];
		const float From = float(Ranks[P.RankLevel - 1].SkillPointsNeeded), To = float(Ranks[P.RankLevel].SkillPointsNeeded);
		const float Progress = FMath::Clamp((float(P.SkillPoints) - From) / FMath::Max(To - From, 1.f), 0.f, 1.f);
		if (Progress > 0.f)
		{
			const FVector2D Size = R.GetSize();
			if (UTexture2D* Capsule = GetBarPart(EGenHudPart::ExpFill, 0, FIntPoint(FMath::RoundToInt32(Size.X), FMath::RoundToInt32(Size.Y))))
			{
				DrawCapsule(Capsule, R, Size.Y * Progress, true);
			}
		}
	}
}

void AGenHUD::DrawGenerals(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// GeneralsExpPoints.wnd: the side's picture, its texts, the experience bar, the promotions to buy and the
	// close button.
	const FGenControlBar& Bar = PC->GetControlBar();
	BarNow = &Bar;
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	if (!Bar.bHasGenerals || !Bar.bGeneralsOpen || !Players.IsValidIndex(Local))
	{
		return;
	}
	const FGenPlayer& P = Players[Local];
	const TArray<FGenRank>& Ranks = FGenUpgradeData::Get().GetRanks();
	const FVector2D Mouse = PC->GetMousePos();
	const FGenBarButton* Hovered = Bar.HitTest(Mouse);
	const float UIScale = Bar.Scale * (600.f / 1080.f);
	if (!DrawMappedImage(Bar.GeneralsImage, Bar.GeneralsPanel))
	{
		const FVector2D PMin = Bar.GeneralsPanel.Min, PSize = Bar.GeneralsPanel.GetSize();
		DrawRect(FLinearColor(0.03f, 0.04f, 0.07f, 0.93f), PMin.X, PMin.Y, PSize.X, PSize.Y);
	}
	if (Bar.ExperienceRect.bIsValid && Ranks.IsValidIndex(P.RankLevel) && P.RankLevel >= 1)
	{
		const float From = float(Ranks[P.RankLevel - 1].SkillPointsNeeded), To = float(Ranks[P.RankLevel].SkillPointsNeeded);
		const float Frac = FMath::Clamp((float(P.SkillPoints) - From) / FMath::Max(To - From, 1.f), 0.f, 1.f);
		const FVector2D EMin = Bar.ExperienceRect.Min, ESize = Bar.ExperienceRect.GetSize();
		DrawRect(FLinearColor(0.95f, 0.75f, 0.2f), EMin.X, EMin.Y, ESize.X * Frac, ESize.Y);
	}
	for (const FGenBarText& T : Bar.GeneralsTexts)
	{
		DrawBarText(T.Text, T.Rect, FLinearColor(T.Color), T.Size, T.bCentered);
	}
	for (const FGenBarButton& B : Bar.Sciences)
	{
		if (B.System == EGenBarSystem::GeneralsExit)
		{
			const bool bHover = &B == Hovered;
			if (!DrawMappedImage(bHover ? TEXT("Buttons-HiLite-Left") : TEXT("Buttons-Left"), B.Rect))
			{
				DrawRect(FLinearColor(0.1f, 0.12f, 0.18f, 0.9f), B.Rect.Min.X, B.Rect.Min.Y, B.Rect.GetSize().X, B.Rect.GetSize().Y);
			}
			DrawBarText(B.Label, B.Rect, bHover ? FLinearColor::White : FLinearColor(0.85f, 0.85f, 0.9f), 12.f * Bar.Scale, true);
			continue;
		}
		DrawBarButton(B, &B == Hovered, UIScale, Logic, Bar.Scheme);
	}
	if (Hovered && !Hovered->Science.IsEmpty())
	{
		DrawTooltip(*Hovered, Mouse, Canvas->SizeY / 1080.f, Logic);
	}
}

void AGenHUD::DrawTooltip(const FGenBarButton& B, const FVector2D& Mouse, float Scale, UGenGameLogic* Logic)
{
	// What the button is, what it costs, why it is greyed out, and the game's own description of it.
	const FGenCommandData& CD = FGenCommandData::Get();
	FString Title = B.Button ? CD.GetText(B.TextLabel.IsEmpty() ? B.Button->TextLabel : B.TextLabel) : (B.Upgrade ? B.Upgrade->Name : (B.Thing ? B.Thing->Name : FString()));
	if (!B.Label.IsEmpty())
	{
		Title = FString::Printf(TEXT("%s: %s"), *Title, *B.Label);
	}
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));
	TArray<FString> Lines;
	bool bWarn = false;
	if (B.QueueIndex != INDEX_NONE)
	{
		Lines.Add(TEXT("Click to cancel"));
	}
	else if (B.bOwned)
	{
		Lines.Add(B.Disabled);
	}
	else if (!B.bEnabled && !B.Disabled.IsEmpty())
	{
		Lines.Add(B.Disabled);
		bWarn = true;
	}
	else if (B.Upgrade)
	{
		Lines.Add(FString::Printf(TEXT("Cost $%d   Time %.0f s"), B.Cost, B.Upgrade->BuildTime));
	}
	else if (!B.Science.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Cost: %d general's point%s"), B.Cost, B.Cost == 1 ? TEXT("") : TEXT("s")));
	}
	else if (!B.Power.IsEmpty())
	{
		Lines.Add(TEXT("Ready"));
	}
	else if (B.Thing && B.Thing->BuildCost > 0.f)
	{
		Lines.Add(FString::Printf(TEXT("Cost $%d   Time %.0f s"), Logic->CalcCostToBuild(Logic->GetLocalPlayer(), *B.Thing), B.Thing->BuildTime));
	}
	const int32 NumStatusLines = Lines.Num();

	// The description (generals.csf), wrapped.
	const FString& DescrLabel = !B.DescriptLabel.IsEmpty() ? B.DescriptLabel : (B.Button ? B.Button->DescriptLabel : B.DescriptLabel);
	FString Descr = DescrLabel.IsEmpty() ? FString() : CD.GetText(DescrLabel);
	if (Descr == DescrLabel)
	{
		Descr.Reset(); // no such string
	}
	TArray<FString> Paragraphs;
	Descr.ParseIntoArrayLines(Paragraphs, true);
	constexpr int32 Wrap = 56;
	for (const FString& Para : Paragraphs)
	{
		TArray<FString> Words;
		Para.ParseIntoArrayWS(Words);
		FString Line;
		for (const FString& W : Words)
		{
			if (Line.Len() + W.Len() + 1 > Wrap && !Line.IsEmpty())
			{
				Lines.Add(Line);
				Line.Reset();
			}
			Line += (Line.IsEmpty() ? TEXT("") : TEXT(" ")) + W;
		}
		if (!Line.IsEmpty() && Lines.Num() < NumStatusLines + 7)
		{
			Lines.Add(Line);
		}
	}

	int32 MaxLen = Title.Len();
	for (const FString& L : Lines)
	{
		MaxLen = FMath::Max(MaxLen, L.Len());
	}
	// The button's key after its name, in brackets (GenBarHotkeys.cpp).
	const FString KeyText = (B.Hotkey.IsValid() && B.QueueIndex == INDEX_NONE) ? FString::Printf(TEXT("[%s]"), *B.Hotkey.GetShortText()) : FString();
	if (!KeyText.IsEmpty())
	{
		MaxLen = FMath::Max(MaxLen, Title.Len() + KeyText.Len() + 2);
	}
	const float LineH = 19.f * Scale;
	const float TitlePixels = 17.5f * Scale, LinePixels = 15.f * Scale;
	float TextW = float(BarTextSize(KeyText.IsEmpty() ? Title : Title + TEXT("  ") + KeyText, TitlePixels, true).X);
	for (const FString& L : Lines)
	{
		TextW = FMath::Max(TextW, float(BarTextSize(L, LinePixels, false).X));
	}
	const float TW = TextW + 16.f;
	const float TH = 28.f * Scale + Lines.Num() * LineH;
	const float TX = FMath::Clamp(float(Mouse.X) - TW * 0.5f, 4.f, FMath::Max(4.f, Canvas->SizeX - TW - 4.f));
	// Above the button and the section it is on (not over the money and the power), or below it when there is no room.
	float TipBottom = float(B.Rect.Min.Y);
	for (int32 Pod = 0; BarNow && BarNow->Shape && Pod < BarPodCount; ++Pod)
	{
		if (BarNow->ToPod(Pod, FBox2D(FVector2D::ZeroVector, BarNow->Shape->PodSize[Pod])).IsInside(B.Rect.GetCenter()))
		{
			TipBottom = float(BarNow->PodOrigin[Pod].Y);
		}
	}
	float TY = TipBottom - TH - 6.f;
	if (TY < 4.f)
	{
		TY = float(B.Rect.Max.Y) + 6.f;
	}
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.9f), TX, TY, TW, TH);
	DrawBarLine(Title, TX + 8.f, TY + 4.f, FLinearColor(1.f, 0.95f, 0.6f), TitlePixels, true);
	if (!KeyText.IsEmpty())
	{
		DrawBarLine(KeyText, TX + 8.f + float(BarTextSize(Title + TEXT("  "), TitlePixels, true).X), TY + 4.f, FLinearColor(0.62f, 0.78f, 1.f), TitlePixels, true);
	}
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FLinearColor Color = i < NumStatusLines ? (bWarn ? FLinearColor(1.f, 0.5f, 0.4f) : FLinearColor::White) : FLinearColor(0.75f, 0.8f, 0.85f);
		DrawBarLine(Lines[i], TX + 8.f, TY + 4.f + 22.f * Scale + i * LineH, Color, LinePixels, false);
	}
}

void AGenHUD::DrawRadar(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// W3DRadar::draw: the terrain, the blips, the fog of war over them, the events, and the box that
	// shows what the screen is looking at.
	const FGenControlBar& Bar = PC->GetControlBar();
	const FGenMapData* Map = Logic->GetMap();
	if (!Bar.Radar.bIsValid || !Map || !Logic->GetMapActor())
	{
		return;
	}
	const FBox2D Window = Bar.Radar;
	const FVector2D WMin = Window.Min, WSize = Window.GetSize();
	const float Scale = Bar.Scale * (600.f / 1080.f);
	// (Its corners, when the style rounds them: over everything else on it.)
	ON_SCOPE_EXIT
	{
		if (Bar.Shape && Bar.Shape->RadarCorner > 0.f)
		{
			DrawBarPart(EGenHudPart::CellCorners, FMath::RoundToInt32(Bar.Shape->RadarCorner * Bar.Scale), Window);
		}
	};
	// (In LeftHUD: the glass of its section is round it.)
	DrawRect(FLinearColor::Black, WMin.X, WMin.Y, WSize.X, WSize.Y);
	auto DrawFrame = [this, &WMin, &WSize](const FLinearColor& Color)
	{
		DrawLine(WMin.X, WMin.Y, WMin.X + WSize.X, WMin.Y, Color, 1.f);
		DrawLine(WMin.X + WSize.X, WMin.Y, WMin.X + WSize.X, WMin.Y + WSize.Y, Color, 1.f);
		DrawLine(WMin.X + WSize.X, WMin.Y + WSize.Y, WMin.X, WMin.Y + WSize.Y, Color, 1.f);
		DrawLine(WMin.X, WMin.Y + WSize.Y, WMin.X, WMin.Y, Color, 1.f);
	};

	const int32 Local = Logic->GetLocalPlayer();
	const FGenMapScripts* Scripts = Logic->GetMapScripts();
	if (!Logic->HasRadar(Local) && !(Scripts && Scripts->IsRadarForced()))
	{
		// "if( !player->hasRadar() && !TheRadar->isRadarForced() ) return": nothing to see without one. A mission
		// can force the radar on with RADAR_FORCE_ENABLE.
		DrawBarText(TEXT("NO RADAR"), Window, FLinearColor(0.35f, 0.4f, 0.45f), 18.f * Scale, true);
		DrawFrame(FLinearColor(0.f, 0.f, 0.f, 0.8f));
		return;
	}

	if (RadarMap != Map || !RadarTerrain)
	{
		RadarMap = Map;
		RadarTerrain = Logic->GetMapActor()->GetTerrain() ? GenRadar::BuildTerrainTexture(*Map, *Logic->GetMapActor()->GetTerrain(), Logic->GetMapActor()->GetBridges(), RadarAverageZ) : nullptr;
	}
	const FVector2f MapSize = Bar.RadarMapSize;
	const FBox2D Rect = GenRadar::FindDrawRect(Window, MapSize);
	const FVector2D RMin = Rect.Min, RSize = Rect.GetSize();
	if (RadarTerrain)
	{
		DrawTexture(RadarTerrain, RMin.X, RMin.Y, RSize.X, RSize.Y, 0.f, 0.f, 1.f, 1.f, FLinearColor::White, BLEND_Opaque);
	}

	// W3DRadar::renderObjectList: a blip of two by two radar cells for everything in view; everybody
	// else's first (buildings under units), then the local player's on top. Into a texture of one texel
	// per radar cell, as the original's overlay is - redrawn every other logic frame, not as a
	// rectangle per object per frame.
	if (!RadarBlips)
	{
		RadarBlips = UTexture2D::CreateTransient(GenRadar::CellWidth, GenRadar::CellHeight, PF_B8G8R8A8, TEXT("GenRadarBlips"));
		if (RadarBlips)
		{
			RadarBlips->SRGB = true;
			RadarBlips->Filter = TF_Nearest;
			RadarBlips->AddressX = TA_Clamp;
			RadarBlips->AddressY = TA_Clamp;
			RadarBlips->UpdateResource();
			RadarBlipsTime = -1000.0;
		}
	}
	const double BlipsNow = Logic->GetRenderFrameTime();
	if (RadarBlips && FMath::Abs(BlipsNow - RadarBlipsTime) >= 2.0)
	{
		RadarBlipsTime = BlipsNow;
		const TArray<FGenPlayer>& Players = Logic->GetPlayers();
		const int32 W = GenRadar::CellWidth, H = GenRadar::CellHeight;
		FColor* Texels = static_cast<FColor*>(FMemory::MallocZeroed(W * H * sizeof(FColor)));
		// "Adjust the alpha for stealth units so they fade / blink on the radar": once a second.
		const float Phase = FMath::Fmod(float(BlipsNow), float(GenLogic::GetLogicFPS())) / (float(GenLogic::GetLogicFPS()) * 0.5f);
		const uint8 StealthAlpha = uint8(FMath::Clamp(32.f + FMath::Abs(Phase - 1.f) * 223.f, 0.f, 255.f));
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			const bool bLocalPass = Pass >= 2, bUnitPass = (Pass & 1) != 0;
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* O = Logic->FindObject(Id);
				if (!O || (O->GetOwner() == Local) != bLocalPass || (O->GetTemplate()->RadarPriority != EGenRadarPriority::Structure) != bUnitPass
					|| O->IsDead() || !Logic->IsOnRadar(*O))
				{
					continue;
				}
				const FVector3f P = O->GetPosition();
				const FIntPoint Cell = GenRadar::WorldToRadar(FVector2f(P.X, P.Y), MapSize);
				// (x, y), (x, y + 1), (x + 1, y + 1), (x + 1, y), those that are "legal radar points"
				const int32 X1 = FMath::Min(Cell.X + 2, W), Y1 = FMath::Min(Cell.Y + 2, H);
				const FLinearColor Linear = Players.IsValidIndex(O->GetOwner()) && !Players[O->GetOwner()].bNeutral ? Players[O->GetOwner()].Color : FLinearColor(0.8f, 0.8f, 0.8f);
				FColor Color = Linear.ToFColor(true);
				Color.A = O->IsStealthed() ? StealthAlpha : 255;
				for (int32 Y = FMath::Max(Cell.Y, 0); Y < Y1; ++Y)
				{
					for (int32 X = FMath::Max(Cell.X, 0); X < X1; ++X)
					{
						Texels[(H - 1 - Y) * W + X] = Color; // (the radar's rows run south to north, the texture's top to bottom)
					}
				}
			}
		}
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
		RadarBlips->UpdateTextureRegions(0, 1, Region, W * 4, 4, reinterpret_cast<uint8*>(Texels),
			[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
			{
				FMemory::Free(SrcData);
				delete Regions;
			});
	}
	if (RadarBlips)
	{
		DrawTexture(RadarBlips, RMin.X, RMin.Y, RSize.X, RSize.Y, 0.f, 0.f, 1.f, 1.f, FLinearColor::White, BLEND_Translucent);
	}

	// The fog of war: black over what was never seen, half as dark over what isn't in view now.
	if (UTexture2D* ShroudTex = Logic->GetShroudTexture())
	{
		const FGenShroud& Shroud = Logic->GetShroud();
		const float TexW = float(Shroud.GetCellsX() + 2), TexH = float(Shroud.GetCellsY() + 2);
		// (The texture has a border texel all round, and its rows run south to north.)
		const float UL = (MapSize.X / Shroud.GetCellSize()) / TexW, VL = (MapSize.Y / Shroud.GetCellSize()) / TexH;
		DrawTexture(ShroudTex, RMin.X, RMin.Y, RSize.X, RSize.Y, 1.f / TexW, 1.f / TexH + VL, UL, -VL, FLinearColor(0.f, 0.f, 0.f, 1.f), BLEND_Translucent);
	}

	// W3DRadar::drawSingleGenericEvent: a triangle that spins down onto the spot and fades.
	const double FrameTime = Logic->GetRenderFrameTime();
	const float FPS = float(GenLogic::GetLogicFPS());
	for (const FGenRadarEvent& E : Logic->GetRadarEvents())
	{
		if (E.Type == EGenRadarEvent::Fake || FrameTime >= double(E.DieFrame) || FrameTime < double(E.CreateFrame))
		{
			continue;
		}
		const float FrameDiff = float(FrameTime - double(E.CreateFrame));
		const float TimeToSmall = FPS * 1.5f;                       // TIME_FROM_FULL_SIZE_TO_SMALL_SIZE
		const float MaxSize = float(RSize.X) / 2.f, MinSize = 6.f;  // in radar cells, as the original has it
		const float Size = FMath::Max(MaxSize * (1.f - FrameDiff / TimeToSmall), MinSize);
		const float AddAngle = 2.f * PI * (FrameDiff / TimeToSmall);
		float Fade = 1.f;
		if (FrameTime > double(E.FadeFrame))
		{
			Fade = FMath::Clamp(1.f - float(FrameTime - double(E.FadeFrame)) / FMath::Max(float(E.DieFrame - E.FadeFrame), 1.f), 0.f, 1.f);
		}
		const FIntPoint Loc = GenRadar::WorldToRadar(FVector2f(E.Pos.X, E.Pos.Y), MapSize);
		FVector2D Tri[3];
		const float Angles[3] = { 0.f - AddAngle, 2.f * PI / 3.f - AddAngle, -2.f * PI / 3.f - AddAngle };
		for (int32 i = 0; i < 3; ++i)
		{
			Tri[i] = GenRadar::RadarToPixel(FVector2D(Loc.X + FMath::Cos(Angles[i]) * Size, Loc.Y + FMath::Sin(Angles[i]) * Size), Rect);
		}
		FLinearColor C1 = FLinearColor(E.Color1), C2 = FLinearColor(E.Color2);
		C1.A *= Fade;
		C2.A *= Fade;
		for (int32 i = 0; i < 3; ++i)
		{
			// Each side runs from the first colour to the second.
			const FVector2D From = Tri[i], To = Tri[(i + 1) % 3];
			constexpr int32 Pieces = 4;
			for (int32 k = 0; k < Pieces; ++k)
			{
				FVector2D A = FMath::Lerp(From, To, double(k) / Pieces), B = FMath::Lerp(From, To, double(k + 1) / Pieces);
				if (GenRadar::ClipLine(A, B, Rect))
				{
					DrawLine(A.X, A.Y, B.X, B.Y, FMath::Lerp(C1, C2, (k + 0.5f) / Pieces), 1.5f * Scale);
				}
			}
		}
	}

	// W3DRadar::drawViewBox: the four corners of the screen, taken down to the terrain's average height.
	FVector2D Corners[4];
	bool bBoxValid = Canvas->SizeX > 0 && Canvas->SizeY > 0;
	const FVector2D Screen[4] = { FVector2D(0.0, 0.0), FVector2D(Canvas->SizeX, 0.0), FVector2D(Canvas->SizeX, Canvas->SizeY), FVector2D(0.0, Canvas->SizeY) };
	const FTransform MapXf = Logic->GetMapActor()->GetActorTransform();
	for (int32 i = 0; i < 4 && bBoxValid; ++i)
	{
		FVector Origin, Dir;
		if (!PC->DeprojectScreenPositionToWorld(Screen[i].X, Screen[i].Y, Origin, Dir))
		{
			bBoxValid = false;
			break;
		}
		const FVector LocalOrigin = MapXf.InverseTransformPosition(Origin), LocalDir = MapXf.InverseTransformVectorNoScale(Dir);
		const double PlaneZ = double(RadarAverageZ) * GenCoords::UnitsToCm;
		if (LocalDir.Z > -1e-4)
		{
			bBoxValid = false; // looking at the sky: no box
			break;
		}
		const FVector Hit = LocalOrigin + LocalDir * ((PlaneZ - LocalOrigin.Z) / LocalDir.Z);
		const FVector Gen = GenCoords::FromUE(Hit);
		Corners[i] = GenRadar::RadarToPixel(FVector2D(Gen.X / MapSize.X * GenRadar::CellWidth, Gen.Y / MapSize.Y * GenRadar::CellHeight), Rect);
	}
	if (bBoxValid)
	{
		const FLinearColor Top(FColor(225, 225, 0)), Bottom(FColor(158, 158, 0));
		const FLinearColor Side = FMath::Lerp(Top, Bottom, 0.5f);
		const FLinearColor Colors[4] = { Top, Side, Bottom, Side };
		for (int32 i = 0; i < 4; ++i)
		{
			// "This clipping region is not over just the radar image area ... it's in the whole window."
			FVector2D A = Corners[i], B = Corners[(i + 1) % 4];
			if (GenRadar::ClipLine(A, B, Window))
			{
				DrawLine(A.X, A.Y, B.X, B.Y, Colors[i], 1.f);
			}
		}
	}
	DrawFrame(FLinearColor(0.f, 0.f, 0.f, 0.8f));
}
