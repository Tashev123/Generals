#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "GenHUD.generated.h"

class AGenPlayerController;
class UGenGameLogic;
struct FGenBarButton;

/**
 * Immediate-mode in-game overlay: drag-selection box, selection circles and health bars
 * (InGameUI selection decoration), money and power, the command bar with its production queue and
 * tooltips, the general's promotions, the special power shortcuts and their targeting circle, and the
 * footprint of a building being placed. A stand-in for the original's WND control bar.
 */
UCLASS()
class GENERALS1_API AGenHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawGroundCircle(const FVector& CenterUE, float RadiusUE, const FLinearColor& Color);
	void DrawPlacement(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawEconomy(UGenGameLogic* Logic);
	void DrawControlBar(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawGenerals(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawTargeting(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawAnnouncements(UGenGameLogic* Logic);
	void DrawVeterancy(AGenPlayerController* PC, UGenGameLogic* Logic);
	void DrawGameOver(UGenGameLogic* Logic);
	void DrawBarButton(const FGenBarButton& B, bool bHovered, float Scale, UGenGameLogic* Logic);
	void DrawTooltip(const FGenBarButton& B, const FVector2D& Mouse, float Scale, UGenGameLogic* Logic);
};
