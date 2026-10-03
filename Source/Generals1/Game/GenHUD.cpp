#include "Game/GenHUD.h"
#include "Core/GenCoords.h"
#include "Game/GenPlayerController.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Engine/Canvas.h"

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
		}
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
}
