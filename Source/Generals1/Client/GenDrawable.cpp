#include "Client/GenDrawable.h"
#include "Logic/GenGameData.h"
#include "W3D/GenModelComponent.h"

namespace
{
	EGenAnimMode ParseMode(const FString& Mode)
	{
		if (Mode.Equals(TEXT("ONCE"), ESearchCase::IgnoreCase)) { return EGenAnimMode::Once; }
		if (Mode.Equals(TEXT("ONCE_BACKWARDS"), ESearchCase::IgnoreCase)) { return EGenAnimMode::OnceBackwards; }
		if (Mode.Equals(TEXT("LOOP_BACKWARDS"), ESearchCase::IgnoreCase)) { return EGenAnimMode::LoopBackwards; }
		if (Mode.Equals(TEXT("PING_PONG"), ESearchCase::IgnoreCase)) { return EGenAnimMode::PingPong; }
		if (Mode.Equals(TEXT("MANUAL"), ESearchCase::IgnoreCase)) { return EGenAnimMode::Manual; }
		return EGenAnimMode::Loop; // RenderObjClass default for W3DModelDraw
	}
}

void GenDrawable::ApplyState(UGenModelComponent* Model, const FGenDrawModule& Draw, int32 StateIndex, bool bNight)
{
	if (!Model || !Draw.States.IsValidIndex(StateIndex))
	{
		return;
	}
	const FGenModelConditionState& S = Draw.States[StateIndex];
	if (S.Model.IsEmpty())
	{
		Model->SetModel(FString()); // NONE: nothing rendered in this state
		return;
	}
	const bool bModelChanged = !S.Model.Equals(Model->GetModelName(), ESearchCase::IgnoreCase);
	if (bModelChanged && !Model->SetModel(S.Model))
	{
		return;
	}
	Model->ResetSubObjectVisibility();
	Model->SetHeadlightsVisible(bNight);
	for (const TPair<FString, bool>& HS : S.HideShow)
	{
		Model->ShowSubObject(HS.Key, !HS.Value);
	}
	for (const FString& Flash : S.MuzzleFlashBones)
	{
		Model->SetSubObjectsOnBonePrefixHidden(Flash, true);
	}

	if (S.Animations.Num() > 0)
	{
		const float Speed = FMath::FRandRange(S.AnimSpeedMin, FMath::Max(S.AnimSpeedMin, S.AnimSpeedMax));
		const EGenAnimMode Mode = ParseMode(S.AnimationMode);
		Model->PlayAnimationSet(S.Animations, S.bIdleAnimations ? EGenAnimMode::Once : Mode, Speed, S.bIdleAnimations, S.bStartFrameLast);
	}
	else if (bModelChanged)
	{
		Model->StopAnimation();
	}
}
