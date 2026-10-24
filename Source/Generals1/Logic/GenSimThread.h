#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include <atomic>

class FEvent;
class FRunnableThread;
class UGenGameLogic;

/**
 * The simulation's own thread, and the gate between it and the game thread.
 *
 * A logic step is a few milliseconds of work thirty times a second. Run inside a frame it makes that
 * frame late, however fast the others are; so it runs here, beside the frames, and the screen blends
 * between the last two steps that are finished (UGenGameLogic::PublishFrame).
 *
 * The two threads are never in the logic at once. The game thread is in it while its game code runs
 * (from the start of the world's tick to the end of the logic's own, and while the HUD is drawn); the
 * step runs in the rest of each frame - the engine's share of it - and at every safe point (between
 * two objects, two scripts, a few hundred cells of a path search) looks whether the game thread is
 * waiting, and lets it in. What the game thread finds there is a step half done, which is fine for
 * what it does: it draws, from positions that were published, and reads. It does not change the
 * simulation in the middle of a step: orders are queued as they always were, and the test hooks that
 * do more wait for the step to finish (WaitForStep).
 */
class FGenSimThread : public FRunnable
{
public:
	explicit FGenSimThread(UGenGameLogic* InLogic);
	virtual ~FGenSimThread();

	// --- game thread ---
	/** Into the logic (calls nest). The step under way stops at its next safe point. */
	void EnterGame();
	void LeaveGame();
	bool IsGameIn() const { return GameDepth > 0; }
	/** The next step may begin (it does when the game thread leaves the logic). */
	void RequestStep();
	bool IsStepRunning() const { return bStepRunning.load(std::memory_order_acquire); }
	/** Lets the step under way finish, if there is one (the game thread may be in the logic or not; it is as before afterwards). */
	void WaitForStep();
	/** Wall time the last finished step took, from when it was asked for. */
	double GetLastStepSeconds() const { return LastStepSeconds.load(std::memory_order_relaxed); }
	/** How long the game thread has waited at the gate since this was last asked (seconds), and the longest wait. */
	void TakeGateWait(double& OutTotal, double& OutMax);

	// --- simulation thread ---
	/** A safe point of the step: the game thread gets its turn if it is waiting. */
	FORCEINLINE void Yield()
	{
		if (bGameWaiting.load(std::memory_order_relaxed) && !bDetached)
		{
			YieldSlow();
		}
	}
	/**
	 * Out of the logic for a piece of the step that touches nothing the game thread reads or writes -
	 * a path search, which works on the pathfinder's own grids and its own scratch - and back in when
	 * it is done. The game thread comes and goes as it likes meanwhile: the search loses no time to it.
	 * False (and nothing happens) when the caller isn't the simulation's thread in the middle of a step.
	 */
	bool BeginDetached();
	void EndDetached();

	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	bool EnterSim();
	void EnterSimWhateverItTakes();
	void LeaveSim();
	void YieldSlow();
	uint32 SimThreadId = 0;
	bool bDetached = false;                 // (simulation thread's)

	UGenGameLogic* Logic = nullptr;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;            // a step was asked for, the game thread has left, or it is time to stop
	std::atomic<int32> Owner{ 0 };          // who is in the logic: 0 nobody, 1 the game thread, 2 the simulation
	std::atomic<bool> bGameWaiting{ false };
	std::atomic<bool> bStepRunning{ false };
	std::atomic<bool> bStop{ false };
	std::atomic<double> LastStepSeconds{ 0.0 };
	double RequestTime = 0.0;
	int32 GameDepth = 0;                    // (game thread's)
	double GateWaitTotal = 0.0, GateWaitMax = 0.0;
};
