#pragma once

#include "CoreMinimal.h"

class UGenModelComponent;
struct FGenDrawModule;
struct FGenModelConditionState;
struct FGenThingTemplate;

/**
 * One draw module of one object on the screen: the state its model shows now and the state waiting
 * behind it (W3DModelDraw's m_curState and m_nextState). What waits is a state that was asked for
 * while a transition into it plays - scaffolding going up, a door swinging open - or one that lets
 * the current animation finish first.
 */
struct GENERALS1_API FGenDrawInstance
{
	const FGenModelConditionState* Cur = nullptr;
	const FGenModelConditionState* Next = nullptr;
	float AdjustZ = 0.f;            // ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT as applied (Generals units; 0 or below)
	bool bPaused = false;           // setPauseAnimation as applied

	/** Something is under way that has to be looked at every frame: a transition playing out, an animation that restarts itself. */
	bool IsBusy() const;
};

/** W3DModelDraw's handling of condition states, for a model component. */
namespace GenDrawable
{
	/**
	 * W3DModelDraw::setModelState: makes the module show that state - by way of the transition its
	 * keys and the current state's ask for, if there is one, or after the current animation if the
	 * state says to let that finish. Swaps the model if needed, applies hide / show, hides muzzle
	 * flashes, starts the state's animation on the frame its flags say.
	 */
	GENERALS1_API void SetModelState(UGenModelComponent* Model, const FGenDrawModule& Draw, FGenDrawInstance& Instance, const FGenModelConditionState* NewState, bool bNight);

	/**
	 * The part of W3DModelDraw::doDrawModule that watches the animation: when it has played out, the
	 * state waiting behind it is shown, or - RESTART_ANIM_WHEN_COMPLETE - it starts again. True if the
	 * state changed.
	 */
	GENERALS1_API bool UpdateModelState(UGenModelComponent* Model, const FGenDrawModule& Draw, FGenDrawInstance& Instance, bool bNight);

	/** A state shown at once, with nothing before it (a model that is not an object's: a placement preview). */
	GENERALS1_API void ApplyState(UGenModelComponent* Model, const FGenDrawModule& Draw, int32 StateIndex, bool bNight);

	/**
	 * Tells the assets which parts of the thing's models get moved or hidden - turrets, barrels that
	 * recoil, muzzle flashes, what the states and the upgrades show and hide, the animations - so that
	 * the rest of each model can be drawn as one mesh (FGenModelPlan). Once per thing; before its models
	 * are made, or they are made twice.
	 */
	GENERALS1_API void NoteTemplate(const FGenThingTemplate& Template);
}
