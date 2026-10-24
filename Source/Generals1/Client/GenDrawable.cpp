#include "Client/GenDrawable.h"
#include "Logic/GenGameData.h"
#include "W3D/GenModelComponent.h"
#include "W3D/GenAssets.h"

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

	/** W3DModelDraw::getCurrentAnimFraction: how far through its animation a state that maintains its frame has got (-1: it doesn't). */
	float GetAnimFraction(const UGenModelComponent* Model, const FGenModelConditionState* State)
	{
		const int32 NumFrames = Model->GetAnimNumFrames();
		if (!State || !(State->Flags & GenStateFlags::AnyMaintainFrame) || NumFrames <= 0)
		{
			return -1.f;
		}
		return NumFrames > 1 ? FMath::Clamp(Model->GetAnimFrame() / float(NumFrames - 1), 0.f, 1.f) : 0.f;
	}

	/** W3DModelDraw::adjustAnimation: the current state's animation, started where its flags say. */
	void AdjustAnimation(UGenModelComponent* Model, const FGenDrawInstance& Instance, const FGenModelConditionState* Prev, float PrevFraction, bool bModelChanged)
	{
		const FGenModelConditionState& S = *Instance.Cur;
		if (S.Model.IsEmpty())
		{
			return;
		}
		if (S.Animations.Num() == 0)
		{
			// (As the original: a state without an animation of its own leaves the model's running, unless the model is new.)
			if (bModelChanged)
			{
				Model->StopAnimation();
			}
			return;
		}
		const float Speed = FMath::FRandRange(S.AnimSpeedMin, FMath::Max(S.AnimSpeedMin, S.AnimSpeedMax));
		Model->PlayAnimationSet(S.Animations, S.bIdleAnimations ? EGenAnimMode::Once : ParseMode(S.AnimationMode), Speed, S.bIdleAnimations);
		const int32 NumFrames = Model->GetAnimNumFrames();
		if (NumFrames <= 0)
		{
			return;
		}
		// (Backwards modes start on the last frame by themselves.)
		int32 Start = INDEX_NONE;
		if (S.Flags & GenStateFlags::RandomStart)
		{
			Start = FMath::RandRange(0, NumFrames - 1);
		}
		else if (S.Flags & GenStateFlags::StartFrameFirst)
		{
			Start = 0;
		}
		else if (S.Flags & GenStateFlags::StartFrameLast)
		{
			Start = NumFrames - 1;
		}
		// "order is important here: MAINTAIN_FRAME_ACROSS_STATES is overridden by the other bits, above."
		else if (Prev && Prev != &S && (S.Flags & Prev->Flags & GenStateFlags::AnyMaintainFrame) != 0 && PrevFraction >= 0.f)
		{
			Start = FMath::Clamp(int32(PrevFraction * float(NumFrames) - 1.f), 0, NumFrames - 1);
		}
		if (Start != INDEX_NONE)
		{
			Model->SetAnimFrame(float(Start));
		}
	}
}

bool FGenDrawInstance::IsBusy() const
{
	return Next != nullptr || (Cur && (Cur->Flags & GenStateFlags::RestartAnimWhenComplete) != 0);
}

void GenDrawable::NoteTemplate(const FGenThingTemplate& Template)
{
	static TSet<const FGenThingTemplate*> Noted;
	bool bAlready = false;
	Noted.Add(&Template, &bAlready);
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (bAlready || !Assets)
	{
		return;
	}
	for (const FGenDrawModule& Draw : Template.Draws)
	{
		// W3DSupplyDraw: the boxes on bones "<prefix>NN" go as the stock does.
		const bool bSupply = !Template.SupplyBonePrefix.IsEmpty() && Draw.Type.Equals(TEXT("W3DSupplyDraw"), ESearchCase::IgnoreCase);
		auto Note = [&](const FGenModelConditionState& S)
		{
			if (S.Model.IsEmpty())
			{
				return;
			}
			for (const TPair<FString, bool>& HS : S.HideShow)
			{
				Assets->NoteToggledSubObject(S.Model, HS.Key);
			}
			for (const FGenUpgradeModule& Module : Template.UpgradeModules)
			{
				for (const FString& Name : Module.ShowSubObjects) { Assets->NoteToggledSubObject(S.Model, Name); }
				for (const FString& Name : Module.HideSubObjects) { Assets->NoteToggledSubObject(S.Model, Name); }
			}
			Assets->NoteMovedBonePrefix(S.Model, S.Turret);
			Assets->NoteMovedBonePrefix(S.Model, S.TurretPitch);
			for (const FString& Tire : Draw.Vehicle.Tires) { Assets->NoteMovedBonePrefix(S.Model, Tire); }
			Assets->NoteMovedBonePrefix(S.Model, Draw.Vehicle.CabBone);
			Assets->NoteMovedBonePrefix(S.Model, Draw.Vehicle.TrailerBone);
			for (const FString& Bone : S.RecoilBones) { Assets->NoteMovedBonePrefix(S.Model, Bone); }
			for (const FString& Bone : S.MuzzleFlashBones) { Assets->NoteMovedBonePrefix(S.Model, Bone); }
			if (bSupply)
			{
				Assets->NoteMovedBonePrefix(S.Model, Template.SupplyBonePrefix);
			}
			for (const FString& Animation : S.Animations)
			{
				Assets->NotePossibleAnimation(S.Model, Animation);
			}
		};
		for (const FGenModelConditionState& S : Draw.States)
		{
			Note(S);
		}
		for (const TPair<FString, FGenModelConditionState>& It : Draw.Transitions)
		{
			Note(It.Value);
		}
	}
}

void GenDrawable::SetModelState(UGenModelComponent* Model, const FGenDrawModule& Draw, FGenDrawInstance& Instance, const FGenModelConditionState* NewState, bool bNight)
{
	if (!Model || !NewState)
	{
		return;
	}
	const FGenModelConditionState* NextState = nullptr;
	if (Instance.Cur)
	{
		// The state asked for is the one showing (with nothing waiting), or the one waiting: nothing to do.
		if ((Instance.Cur == NewState && !Instance.Next) || Instance.Next == NewState)
		{
			return;
		}
		// WaitForStateToFinishIfPossible: the animation of the state we are in is let run out first.
		if (NewState != Instance.Cur && !NewState->AllowToFinishKey.IsEmpty() && NewState->AllowToFinishKey == Instance.Cur->TransitionKey
			&& !Model->GetModelName().IsEmpty() && Model->HasAnimation() && !Model->IsAnimationComplete())
		{
			Instance.Next = NewState;
			return;
		}
		// From one keyed state to another: by way of their transition state, if the module has one.
		if (NewState != Instance.Cur && !Instance.Cur->TransitionKey.IsEmpty() && !NewState->TransitionKey.IsEmpty())
		{
			if (const FGenModelConditionState* Transition = Draw.FindTransition(Instance.Cur->TransitionKey, NewState->TransitionKey))
			{
				NextState = NewState;
				NewState = Transition;
			}
		}
	}

	// (Asked before anything changes: adjustAnimation wants to know where the old animation had got to.)
	const float PrevFraction = GetAnimFraction(Model, Instance.Cur);

	const FGenModelConditionState& S = *NewState;
	bool bModelChanged = false;
	if (S.Model.IsEmpty())
	{
		Model->SetModel(FString()); // NONE: nothing rendered in this state
	}
	else
	{
		bModelChanged = !S.Model.Equals(Model->GetModelName(), ESearchCase::IgnoreCase);
		if (!bModelChanged || Model->SetModel(S.Model))
		{
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
		}
	}

	const FGenModelConditionState* Prev = Instance.Cur;
	Instance.Cur = NewState;
	Instance.Next = NextState;
	if (!Model->GetModelName().IsEmpty())
	{
		AdjustAnimation(Model, Instance, Prev, PrevFraction, bModelChanged);
	}
}

bool GenDrawable::UpdateModelState(UGenModelComponent* Model, const FGenDrawModule& Draw, FGenDrawInstance& Instance, bool bNight)
{
	if (!Model || !Instance.Cur)
	{
		return false;
	}
	// ("isAnimationComplete": true as well when there is no model to animate.)
	const bool bHasModel = !Model->GetModelName().IsEmpty();
	if (bHasModel && !Model->IsAnimationComplete())
	{
		return false;
	}
	if (Instance.Next)
	{
		const FGenModelConditionState* Next = Instance.Next;
		Instance.Next = nullptr;
		SetModelState(Model, Draw, Instance, Next, bNight);
		return true;
	}
	// (An idle animation that has played out is followed by another of its state by the model itself.)
	if (bHasModel && Model->HasAnimation() && Instance.Cur->Animations.Num() > 0 && !Instance.Cur->bIdleAnimations
		&& (Instance.Cur->Flags & GenStateFlags::RestartAnimWhenComplete) != 0)
	{
		AdjustAnimation(Model, Instance, Instance.Cur, -1.f, false);
	}
	return false;
}

void GenDrawable::ApplyState(UGenModelComponent* Model, const FGenDrawModule& Draw, int32 StateIndex, bool bNight)
{
	if (Model && Draw.States.IsValidIndex(StateIndex))
	{
		FGenDrawInstance Instance;
		SetModelState(Model, Draw, Instance, &Draw.States[StateIndex], bNight);
	}
}
