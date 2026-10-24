#pragma once

#include "CoreMinimal.h"

struct FGenIniBlock;

/**
 * RadiusDecalTemplate (GameClient/RadiusDecal.cpp): a picture laid on the ground round a spot - the radius
 * cursors of InGameUI.ini shown while a power or an order is aimed, a gunship's AttackAreaDecal and
 * TargetingReticleDecal. Every one in the data is a SHADOW_ALPHA_DECAL: the texture times Color, its alpha
 * throbbing between OpacityMin and OpacityMax.
 */
struct GENERALS1_API FGenRadiusDecalTemplate
{
	FString Texture;                      // empty: nothing is drawn
	float MinOpacity = 1.f;
	float MaxOpacity = 1.f;
	float ThrobSeconds = 1.f;             // OpacityThrobTime (LOGICFRAMES_PER_SECOND by default)
	FColor Color = FColor(0, 0, 0, 0);
	bool bPlayerColor = true;             // "m_color == 0 ? owningPlayer->getPlayerColor() : m_color"
	bool bOnlyVisibleToOwningPlayer = true;

	bool IsValid() const { return !Texture.IsEmpty(); }
	/** RadiusDecalTemplate::parseRadiusDecalTemplate; no block: the empty template. */
	static FGenRadiusDecalTemplate Parse(const FGenIniBlock* Block);
	/** RadiusDecal::update: the opacity at that time (seconds). */
	float GetOpacity(double Seconds) const;
};
