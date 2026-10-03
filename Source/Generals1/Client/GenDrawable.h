#pragma once

#include "CoreMinimal.h"

class UGenModelComponent;
struct FGenDrawModule;

/** W3DModelDraw::setModelState equivalent: applies a condition state to a model component. */
namespace GenDrawable
{
	/** Swaps model if needed, applies hide/show, hides muzzle flashes, starts the state's animation. */
	GENERALS1_API void ApplyState(UGenModelComponent* Model, const FGenDrawModule& Draw, int32 StateIndex, bool bNight);
}
