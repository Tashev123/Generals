#include "Game/GenHUD.h"
#include "Core/GenCoords.h"
#include "Game/GenControlBar.h"
#include "Game/GenPlayerController.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenUpgrades.h"
#include "W3D/GenAssets.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"

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
	Super::DrawHUD();
	AGenPlayerController* PC = Cast<AGenPlayerController>(GetOwningPlayerController());
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	if (!PC || !Logic || !Canvas)
	{
		return;
	}

	for (FGenObjectID Id : PC->GetSelection())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O)
		{
			continue;
		}
		const FVector3f P = Logic->GetRenderPosition(*O);
		const FGenThingTemplate* T = O->GetTemplate();
		const float Radius = FMath::Max(T->GetBoundingCircleRadius(), 4.f);
		const bool bOwn = O->GetOwner() == Logic->GetLocalPlayer();
		const FLinearColor RingColor = bOwn ? FLinearColor(0.2f, 1.f, 0.2f) : FLinearColor(1.f, 0.85f, 0.2f);
		DrawGroundCircle(GenCoords::ToUE(P.X, P.Y, P.Z + 0.5f), Radius * GenCoords::UnitsToCm, RingColor);

		// A selected factory shows where it sends its new units.
		if (bOwn && O->HasRallyPoint())
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

		// Health bar above the object.
		FVector2D Top;
		if (PC->ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z + FMath::Max(T->GeometryHeight, 8.f) + 4.f), Top))
		{
			const float W = FMath::Clamp(Radius * 2.5f, 24.f, 90.f);
			const float H = 4.f;
			const float Frac = O->GetMaxHealth() > 0.f ? FMath::Clamp(O->GetHealth() / O->GetMaxHealth(), 0.f, 1.f) : 1.f;
			const FLinearColor HealthColor = Frac > 0.5f ? FLinearColor(0.1f, 0.9f, 0.1f) : (Frac > 0.25f ? FLinearColor(1.f, 0.85f, 0.f) : FLinearColor(0.95f, 0.1f, 0.1f));
			DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.7f), Top.X - W / 2 - 1, Top.Y - 1, W + 2, H + 2);
			DrawRect(HealthColor, Top.X - W / 2, Top.Y, W * Frac, H);

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
			else if (O->GetContain() && Logic->GetContainMax(*O) > 0)
			{
				// How full a transport, a garrisoned building or the tunnel network is (the original's pips).
				Supplies = FString::Printf(TEXT("%d / %d"), Logic->GetContainCount(*O), Logic->GetContainMax(*O));
			}
			else if (T->bJetAI)
			{
				// Ammunition that is only replaced at the airfield (ShowsAmmoPips).
				for (int32 s = 0; s < 3; ++s)
				{
					const FGenWeaponInstance& WI = O->GetWeapon(s);
					if (WI.Template && WI.Template->ClipSize > 0 && WI.Template->ReloadType == EGenReloadType::ReturnToBase)
					{
						Supplies = FString::Printf(TEXT("%d / %d"), WI.AmmoInClip, WI.Template->ClipSize);
						break;
					}
				}
			}
			if (!Supplies.IsEmpty())
			{
				const float TextX = Top.X - Supplies.Len() * 3.5f, TextY = Top.Y - 17.f;
				DrawText(Supplies, FLinearColor::Black, TextX + 1.f, TextY + 1.f);
				DrawText(Supplies, FLinearColor(1.f, 0.9f, 0.3f), TextX, TextY);
			}
		}
	}

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

	DrawVeterancy(PC, Logic);
	DrawPlacement(PC, Logic);
	DrawTargeting(PC, Logic);
	DrawEconomy(Logic);
	DrawAnnouncements(Logic);
	DrawControlBar(PC, Logic);
	DrawGenerals(PC, Logic);
	DrawGameOver(Logic);
}

void AGenHUD::DrawVeterancy(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// The rank chevrons over veteran, elite and heroic units (one, two, three), everybody's.
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f);
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		const FGenObject* O = Logic->FindObject(Id);
		if (!O || O->GetVeterancyLevel() <= 0 || O->IsDead() || O->IsHidden() || O->IsProjectile())
		{
			continue;
		}
		const FVector3f P = Logic->GetRenderPosition(*O);
		FVector2D S;
		if (!PC->ProjectWorldLocationToScreen(GenCoords::ToUE(P.X, P.Y, P.Z + FMath::Max(O->GetTemplate()->GeometryHeight, 8.f) + 4.f), S)
			|| S.X < -20.0 || S.Y < -20.0 || S.X > Canvas->SizeX + 20.0 || S.Y > Canvas->SizeY + 20.0)
		{
			continue;
		}
		const FLinearColor Color = O->GetVeterancyLevel() >= 3 ? FLinearColor(1.f, 0.35f, 0.2f) : FLinearColor(1.f, 0.85f, 0.2f);
		const float W = 5.f * Scale, H = 3.f * Scale;
		const float X = float(S.X) + 14.f * Scale, Y = float(S.Y) - 2.f * Scale;
		for (int32 i = 0; i < O->GetVeterancyLevel(); ++i)
		{
			const float CY = Y - i * (H + 1.5f * Scale);
			DrawLine(X - W, CY + H, X, CY, FLinearColor::Black, 3.f * Scale);
			DrawLine(X, CY, X + W, CY + H, FLinearColor::Black, 3.f * Scale);
			DrawLine(X - W, CY + H, X, CY, Color, 1.5f * Scale);
			DrawLine(X, CY, X + W, CY + H, Color, 1.5f * Scale);
		}
	}
}

void AGenHUD::DrawGameOver(UGenGameLogic* Logic)
{
	// The game is decided: say so, large, in the middle of the screen (the original goes to the score screen).
	const bool bLost = Logic->IsPlayerDefeated(Logic->GetLocalPlayer());
	if (!Logic->IsGameOver() && !bLost)
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

void AGenHUD::DrawTargeting(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	// The special power being aimed: its area on the ground under the cursor.
	FVector3f Pos;
	float Radius = 0.f;
	bool bValid = false;
	const FGenCommandButton* Button = PC->GetTargeting(Pos, Radius, bValid);
	if (!Button)
	{
		return;
	}
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f);
	if (bValid)
	{
		const FVector Center = GenCoords::ToUE(Pos.X, Pos.Y, Pos.Z + 1.f);
		DrawGroundCircle(Center, FMath::Max(Radius, 15.f) * GenCoords::UnitsToCm, FLinearColor(1.f, 0.25f, 0.15f));
		DrawGroundCircle(Center, 4.f * GenCoords::UnitsToCm, FLinearColor(1.f, 0.9f, 0.3f));
	}
	FString Text = FString::Printf(TEXT("%s: left click the target, right click to cancel"), *FGenCommandData::Get().GetText(Button->TextLabel));
	Text.ReplaceInline(TEXT("\n"), TEXT(" "));
	const float X = Canvas->SizeX * 0.5f - Text.Len() * 4.2f * Scale, Y = 60.f * Scale;
	DrawText(Text, FLinearColor::Black, X + 1.5f, Y + 1.5f, nullptr, 1.2f * Scale);
	DrawText(Text, FLinearColor(1.f, 0.85f, 0.3f), X, Y, nullptr, 1.2f * Scale);
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

void AGenHUD::DrawEconomy(UGenGameLogic* Logic)
{
	// Money and power of the local player (top left until the real control bar exists).
	const int32 Local = Logic->GetLocalPlayer();
	int32 Produced = 0, Consumed = 0;
	Logic->GetEnergy(Local, Produced, Consumed);
	const float Scale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f) * 1.4f;
	const FString Money = FString::Printf(TEXT("$%d"), Logic->GetMoney(Local));
	const FString Power = FString::Printf(TEXT("Power %d / %d"), Produced, Consumed);
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), 8.f, 8.f, 190.f * Scale, 44.f * Scale);
	DrawText(Money, FLinearColor(1.f, 0.9f, 0.3f), 16.f, 12.f, nullptr, Scale);
	DrawText(Power, Produced >= Consumed ? FLinearColor(0.4f, 1.f, 0.4f) : FLinearColor(1.f, 0.3f, 0.2f), 16.f, 12.f + 20.f * Scale, nullptr, Scale);
}

void AGenHUD::DrawBarButton(const FGenBarButton& B, bool bHovered, float Scale, UGenGameLogic* Logic)
{
	const FGenCommandData& CD = FGenCommandData::Get();
	const FVector2D Min = B.Rect.Min, Size = B.Rect.GetSize();
	DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.9f), Min.X, Min.Y, Size.X, Size.Y);
	const FGenMappedImage* Img = !B.Image.IsEmpty() ? CD.FindImage(B.Image) : nullptr;
	if (!Img)
	{
		Img = B.Button ? CD.FindImage(B.Button->ButtonImage) : nullptr;
	}
	UTexture2D* Tex = Img ? UGenAssetSubsystem::Get()->GetTexture(Img->Texture) : nullptr;
	// (What is already researched or acquired stays bright: it is done, not unavailable.)
	const FLinearColor Tint = B.bEnabled ? (bHovered ? FLinearColor(1.25f, 1.25f, 1.25f) : FLinearColor::White)
		: (B.bOwned ? FLinearColor(0.75f, 0.9f, 0.75f) : FLinearColor(0.3f, 0.3f, 0.3f));
	if (Tex)
	{
		DrawTexture(Tex, Min.X + 1, Min.Y + 1, Size.X - 2, Size.Y - 2, Img->UV0.X, Img->UV0.Y, Img->UV1.X - Img->UV0.X, Img->UV1.Y - Img->UV0.Y, Tint);
	}
	else if (B.Button)
	{
		// No icon: fall back to the button's name.
		DrawText(CD.GetText(B.Button->TextLabel).Left(9), Tint, Min.X + 3, Min.Y + Size.Y * 0.35f, nullptr, 0.8f * Scale);
	}
	FString Cost;
	if (B.QueueIndex != INDEX_NONE || B.bOwned)
	{
		// (Queue entries and things already owned show no price.)
	}
	else if (B.Upgrade && B.Cost > 0)
	{
		Cost = FString::Printf(TEXT("$%d"), B.Cost);
	}
	else if (!B.Science.IsEmpty() && B.Cost > 0)
	{
		Cost = FString::Printf(TEXT("%d pt"), B.Cost);
	}
	else if (B.Thing && B.Thing->BuildCost > 0.f)
	{
		Cost = FString::Printf(TEXT("$%d"), int32(B.Thing->BuildCost));
	}
	if (!Cost.IsEmpty())
	{
		DrawText(Cost, FLinearColor::Black, Min.X + 4, Min.Y + Size.Y - 15.f * Scale, nullptr, 0.85f * Scale);
		DrawText(Cost, B.bEnabled ? FLinearColor(1.f, 0.95f, 0.4f) : FLinearColor(0.7f, 0.4f, 0.4f), Min.X + 3, Min.Y + Size.Y - 16.f * Scale, nullptr, 0.85f * Scale);
	}
	if (B.Recharge > 0.f)
	{
		// A special power recharging: the shade drains away, with the time left on it.
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.65f), Min.X + 1, Min.Y + 1, Size.X - 2, (Size.Y - 2) * B.Recharge);
		const int32 Seconds = FMath::CeilToInt32(float(B.ReadyFrame - FMath::Min(B.ReadyFrame, Logic->GetFrame())) / float(GenLogic::GetLogicFPS()));
		const FString Left = FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60);
		DrawText(Left, FLinearColor::Black, Min.X + Size.X * 0.5f - Left.Len() * 3.6f * Scale + 1.f, Min.Y + Size.Y * 0.32f + 1.f, nullptr, Scale);
		DrawText(Left, FLinearColor::White, Min.X + Size.X * 0.5f - Left.Len() * 3.6f * Scale, Min.Y + Size.Y * 0.32f, nullptr, Scale);
	}
	if (B.bOwned)
	{
		// Done: a green corner.
		const float M = 11.f * Scale;
		DrawRect(FLinearColor(0.15f, 0.85f, 0.25f, 0.95f), Min.X + Size.X - M - 1, Min.Y + 1, M, M);
	}
	const FLinearColor Edge = B.bOwned ? FLinearColor(0.2f, 0.8f, 0.3f)
		: (bHovered && B.bEnabled ? FLinearColor(1.f, 0.9f, 0.4f) : FLinearColor(0.35f, 0.4f, 0.5f));
	DrawLine(Min.X, Min.Y, Min.X + Size.X, Min.Y, Edge, 1.f);
	DrawLine(Min.X + Size.X, Min.Y, Min.X + Size.X, Min.Y + Size.Y, Edge, 1.f);
	DrawLine(Min.X + Size.X, Min.Y + Size.Y, Min.X, Min.Y + Size.Y, Edge, 1.f);
	DrawLine(Min.X, Min.Y + Size.Y, Min.X, Min.Y, Edge, 1.f);
}

void AGenHUD::DrawControlBar(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	const FGenControlBar& Bar = PC->GetControlBar();
	const float Scale = Bar.Scale;
	const FVector2D Mouse = PC->GetMousePos();
	const FGenBarButton* Hovered = Bar.HitTest(Mouse);

	// Special powers the player has, down the right edge.
	for (const FGenBarButton& B : Bar.Shortcuts)
	{
		DrawBarButton(B, &B == Hovered, Scale, Logic);
	}

	if (Bar.bVisible)
	{
		const FVector2D PMin = Bar.Panel.Min, PSize = Bar.Panel.GetSize();
		DrawRect(FLinearColor(0.05f, 0.07f, 0.1f, 0.8f), PMin.X, PMin.Y, PSize.X, PSize.Y);
		for (const FGenBarButton& B : Bar.Buttons)
		{
			DrawBarButton(B, &B == Hovered, Scale, Logic);
		}

		// Production queue: the first entry fills up as it is built.
		const FGenObject* Subject = Logic->FindObject(Bar.Subject);
		for (const FGenBarButton& Q : Bar.Queue)
		{
			DrawBarButton(Q, &Q == Hovered, Scale, Logic);
			if (Q.QueueIndex == 0 && Subject && Subject->GetProductionQueue().Num() > 0 && (Q.Thing || Q.Upgrade))
			{
				const float Total = Q.Upgrade ? Q.Upgrade->BuildTime * float(GenLogic::GetLogicFPS()) : Logic->CalcTimeToBuild(*Q.Thing, Subject->GetOwner());
				const float Done = FMath::Clamp(Subject->GetProductionQueue()[0].FramesDone / FMath::Max(Total, 1.f), 0.f, 1.f);
				const FVector2D Min = Q.Rect.Min, Size = Q.Rect.GetSize();
				DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.6f), Min.X, Min.Y, Size.X, Size.Y * (1.f - Done));
				DrawText(FString::Printf(TEXT("%d%%"), FMath::FloorToInt32(Done * 100.f)), FLinearColor::White, Min.X + 3, Min.Y + 2, nullptr, 0.85f * Scale);
			}
		}
		if (!Bar.Status.IsEmpty())
		{
			DrawText(Bar.Status, FLinearColor::White, PMin.X + 12.f * Scale, PMin.Y + PSize.Y * 0.4f, nullptr, 1.3f * Scale);
		}
	}

	// (The promotions panel draws its own tooltips, on top of itself.)
	if (Hovered && !Hovered->bGeneralsToggle && Hovered->Science.IsEmpty())
	{
		DrawTooltip(*Hovered, Mouse, Scale, Logic);
	}
}

void AGenHUD::DrawGenerals(AGenPlayerController* PC, UGenGameLogic* Logic)
{
	const FGenControlBar& Bar = PC->GetControlBar();
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	if (!Bar.bHasGenerals || !Players.IsValidIndex(Local))
	{
		return;
	}
	const FGenPlayer& P = Players[Local];
	const TArray<FGenRank>& Ranks = FGenUpgradeData::Get().GetRanks();
	const FVector2D Mouse = PC->GetMousePos();
	const FGenBarButton* Hovered = Bar.HitTest(Mouse);
	const float EScale = FMath::Clamp(Canvas->SizeY / 1080.f, 0.75f, 2.f) * 1.4f;

	// The general's button: rank, points to spend, and how far the next rank is.
	{
		const FVector2D Min = Bar.GeneralsButton.Rect.Min, Size = Bar.GeneralsButton.Rect.GetSize();
		const bool bHover = Hovered == &Bar.GeneralsButton;
		const bool bPoints = P.SciencePurchasePoints > 0;
		// (With points to spend it pulses, like the original's flashing star.)
		const float Pulse = bPoints ? 0.5f + 0.5f * FMath::Sin(float(GetWorld()->GetRealTimeSeconds()) * 5.f) : 0.f;
		DrawRect(FLinearColor(0.05f + 0.25f * Pulse, 0.06f + 0.2f * Pulse, 0.1f, 0.8f), Min.X, Min.Y, Size.X, Size.Y);
		const FString Text = FString::Printf(TEXT("General: rank %d   %d point%s"), P.RankLevel, P.SciencePurchasePoints, P.SciencePurchasePoints == 1 ? TEXT("") : TEXT("s"));
		DrawText(Text, bPoints ? FLinearColor(1.f, 0.9f, 0.3f) : FLinearColor(0.85f, 0.85f, 0.9f), Min.X + 8.f, Min.Y + 3.f * EScale, nullptr, 0.8f * EScale);
		if (Ranks.IsValidIndex(P.RankLevel))
		{
			const float From = float(Ranks[P.RankLevel - 1].SkillPointsNeeded), To = float(Ranks[P.RankLevel].SkillPointsNeeded);
			const float Frac = FMath::Clamp((float(P.SkillPoints) - From) / FMath::Max(To - From, 1.f), 0.f, 1.f);
			DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.8f), Min.X + 8.f, Min.Y + Size.Y - 9.f * EScale, Size.X - 16.f, 5.f * EScale);
			DrawRect(FLinearColor(0.95f, 0.75f, 0.2f), Min.X + 8.f, Min.Y + Size.Y - 9.f * EScale, (Size.X - 16.f) * Frac, 5.f * EScale);
		}
		const FLinearColor Edge = bHover || Bar.bGeneralsOpen ? FLinearColor(1.f, 0.9f, 0.4f) : FLinearColor(0.35f, 0.4f, 0.5f);
		DrawLine(Min.X, Min.Y, Min.X + Size.X, Min.Y, Edge, 1.f);
		DrawLine(Min.X + Size.X, Min.Y, Min.X + Size.X, Min.Y + Size.Y, Edge, 1.f);
		DrawLine(Min.X + Size.X, Min.Y + Size.Y, Min.X, Min.Y + Size.Y, Edge, 1.f);
		DrawLine(Min.X, Min.Y + Size.Y, Min.X, Min.Y, Edge, 1.f);
	}
	if (!Bar.bGeneralsOpen)
	{
		return;
	}

	const float Scale = Bar.Scale;
	const FVector2D PMin = Bar.GeneralsPanel.Min, PSize = Bar.GeneralsPanel.GetSize();
	DrawRect(FLinearColor(0.03f, 0.04f, 0.07f, 0.93f), PMin.X, PMin.Y, PSize.X, PSize.Y);
	DrawLine(PMin.X, PMin.Y, PMin.X + PSize.X, PMin.Y, FLinearColor(0.8f, 0.7f, 0.3f), 1.5f);
	DrawLine(PMin.X, PMin.Y + PSize.Y, PMin.X + PSize.X, PMin.Y + PSize.Y, FLinearColor(0.8f, 0.7f, 0.3f), 1.5f);
	FString Title = FString::Printf(TEXT("General's promotions      Rank %d      %d point%s to spend"), P.RankLevel, P.SciencePurchasePoints,
		P.SciencePurchasePoints == 1 ? TEXT("") : TEXT("s"));
	if (Ranks.IsValidIndex(P.RankLevel))
	{
		Title += FString::Printf(TEXT("      Experience %d / %d"), P.SkillPoints, Ranks[P.RankLevel].SkillPointsNeeded);
	}
	DrawText(Title, FLinearColor(1.f, 0.9f, 0.5f), PMin.X + 14.f * Scale, PMin.Y + 10.f * Scale, nullptr, 1.25f * Scale);
	static const TCHAR* SectionNames[3] = { TEXT("Rank 1"), TEXT("Rank 3"), TEXT("Rank 5") };
	const int32 RankNeeded[3] = { 1, 3, 5 };
	for (int32 s = 0; s < 3; ++s)
	{
		if (!Bar.SectionLabels[s].IsZero())
		{
			DrawText(SectionNames[s], P.RankLevel >= RankNeeded[s] ? FLinearColor(0.9f, 0.9f, 0.95f) : FLinearColor(0.5f, 0.5f, 0.55f),
				Bar.SectionLabels[s].X, Bar.SectionLabels[s].Y, nullptr, 1.05f * Scale);
		}
	}
	for (const FGenBarButton& B : Bar.Sciences)
	{
		DrawBarButton(B, &B == Hovered, Scale, Logic);
	}
	if (Hovered && !Hovered->Science.IsEmpty())
	{
		DrawTooltip(*Hovered, Mouse, Scale, Logic);
	}
}

void AGenHUD::DrawTooltip(const FGenBarButton& B, const FVector2D& Mouse, float Scale, UGenGameLogic* Logic)
{
	// What the button is, what it costs, why it is greyed out, and the game's own description of it.
	const FGenCommandData& CD = FGenCommandData::Get();
	FString Title = B.Button ? CD.GetText(B.Button->TextLabel) : (B.Upgrade ? B.Upgrade->Name : (B.Thing ? B.Thing->Name : FString()));
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
		Lines.Add(FString::Printf(TEXT("Cost $%d   Time %.0f s"), int32(B.Thing->BuildCost), B.Thing->BuildTime));
	}
	const int32 NumStatusLines = Lines.Num();

	// The description (generals.csf), wrapped.
	FString Descr = (B.Button && !B.Button->DescriptLabel.IsEmpty()) ? CD.GetText(B.Button->DescriptLabel) : FString();
	if (B.Button && Descr == B.Button->DescriptLabel)
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
	const float LineH = 19.f * Scale;
	const float TW = MaxLen * 8.2f * Scale + 16.f;
	const float TH = 28.f * Scale + Lines.Num() * LineH;
	const float TX = FMath::Clamp(float(Mouse.X) - TW * 0.5f, 4.f, FMath::Max(4.f, Canvas->SizeX - TW - 4.f));
	// Above the button, or below it when there is no room.
	float TY = float(B.Rect.Min.Y) - TH - 6.f;
	if (TY < 4.f)
	{
		TY = float(B.Rect.Max.Y) + 6.f;
	}
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.9f), TX, TY, TW, TH);
	DrawText(Title, FLinearColor(1.f, 0.95f, 0.6f), TX + 8.f, TY + 4.f, nullptr, 1.1f * Scale);
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FLinearColor Color = i < NumStatusLines ? (bWarn ? FLinearColor(1.f, 0.5f, 0.4f) : FLinearColor::White) : FLinearColor(0.75f, 0.8f, 0.85f);
		DrawText(Lines[i], Color, TX + 8.f, TY + 4.f + 22.f * Scale + i * LineH, nullptr, 0.95f * Scale);
	}
}
