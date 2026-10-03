#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "GenHUD.generated.h"

/**
 * Immediate-mode in-game overlay: drag-selection box, selection circles and health bars
 * (InGameUI selection decoration). The real control bar UI comes later.
 */
UCLASS()
class GENERALS1_API AGenHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawGroundCircle(const FVector& CenterUE, float RadiusUE, const FLinearColor& Color);
};
