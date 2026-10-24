#include "Logic/GenSimThread.h"
#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Generals1.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/RunnableThread.h"

FGenSimThread::FGenSimThread(UGenGameLogic* InLogic)
	: Logic(InLogic)
{
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	Thread = FRunnableThread::Create(this, TEXT("GenSimulation"), 0, TPri_AboveNormal);
}

FGenSimThread::~FGenSimThread()
{
	// (Whoever destroys it has let the step under way finish: WaitForStep.)
	bStop.store(true);
	WakeEvent->Trigger();
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
	WakeEvent = nullptr;
}

void FGenSimThread::Stop()
{
	bStop.store(true);
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

uint32 FGenSimThread::Run()
{
	SimThreadId = FPlatformTLS::GetCurrentThreadId();
	while (!bStop.load())
	{
		if (!bStepRunning.load(std::memory_order_acquire))
		{
			WakeEvent->Wait(100);
			continue;
		}
		if (!EnterSim())
		{
			break;
		}
		Logic->RunStep();
		LeaveSim();
		LastStepSeconds.store(FPlatformTime::Seconds() - RequestTime, std::memory_order_relaxed);
		bStepRunning.store(false, std::memory_order_release);
	}
	bStepRunning.store(false, std::memory_order_release);
	return 0;
}

bool FGenSimThread::EnterSim()
{
	for (;;)
	{
		// Not while the game thread is waiting: it goes first. The game thread is in for a millisecond or
		// two at a time, a few times a frame; waking from a sleep would cost a good part of what is left
		// between, so the wait is a spin - on a core of its own, and only while a step is under way.
		const double SpinStart = FPlatformTime::Seconds();
		for (;;)
		{
			for (int32 Spin = 0; Spin < 256; ++Spin)
			{
				if (!bGameWaiting.load(std::memory_order_relaxed))
				{
					int32 Expected = 0;
					if (Owner.compare_exchange_strong(Expected, 2))
					{
						return true;
					}
				}
				FPlatformProcess::Yield();
			}
			if (FPlatformTime::Seconds() - SpinStart > 0.004)
			{
				break;
			}
		}
		if (bStop.load())
		{
			return false;
		}
		// (In for longer than that - a hitch, a loading screen: sleep until it leaves.)
		WakeEvent->Wait(1);
	}
}

void FGenSimThread::LeaveSim()
{
	Owner.store(0);
}

void FGenSimThread::EnterSimWhateverItTakes()
{
	if (!EnterSim())
	{
		// Told to stop in the middle of a step, with the game thread in the logic. It can't be left half
		// done: wait for the game thread however long that takes.
		for (;;)
		{
			int32 Expected = 0;
			if (!bGameWaiting.load() && Owner.compare_exchange_strong(Expected, 2))
			{
				return;
			}
			FPlatformProcess::SleepNoStats(0.f);
		}
	}
}

void FGenSimThread::YieldSlow()
{
	GEN_PERF_SCOPE("Step.Waiting");
	LeaveSim();
	EnterSimWhateverItTakes();
}

bool FGenSimThread::BeginDetached()
{
	if (bDetached || SimThreadId == 0 || FPlatformTLS::GetCurrentThreadId() != SimThreadId || Owner.load(std::memory_order_relaxed) != 2)
	{
		return false;
	}
	bDetached = true;
	LeaveSim();
	return true;
}

void FGenSimThread::EndDetached()
{
	GEN_PERF_SCOPE("Step.Waiting");
	EnterSimWhateverItTakes();
	bDetached = false;
}

void FGenSimThread::EnterGame()
{
	check(IsInGameThread());
	if (GameDepth++ > 0)
	{
		return;
	}
	int32 Expected = 0;
	if (Owner.compare_exchange_strong(Expected, 1))
	{
		return; // (the usual case: nobody was in)
	}
	// The step is at work: it stops at its next safe point. Not long - a spin, not a sleep.
	const double Start = FPlatformTime::Seconds();
	bGameWaiting.store(true);
	for (;;)
	{
		Expected = 0;
		if (Owner.compare_exchange_weak(Expected, 1))
		{
			break;
		}
		FPlatformProcess::Yield();
	}
	bGameWaiting.store(false);
	const double Waited = FPlatformTime::Seconds() - Start;
	GateWaitTotal += Waited;
	GateWaitMax = FMath::Max(GateWaitMax, Waited);
}

void FGenSimThread::LeaveGame()
{
	check(IsInGameThread() && GameDepth > 0);
	if (--GameDepth == 0)
	{
		Owner.store(0);
		if (bStepRunning.load(std::memory_order_relaxed))
		{
			WakeEvent->Trigger();
		}
	}
}

void FGenSimThread::RequestStep()
{
	check(IsInGameThread() && !bStepRunning.load());
	RequestTime = FPlatformTime::Seconds();
	bStepRunning.store(true, std::memory_order_release);
	WakeEvent->Trigger();
}

void FGenSimThread::WaitForStep()
{
	check(IsInGameThread());
	if (!bStepRunning.load(std::memory_order_acquire))
	{
		return;
	}
	const int32 Depth = GameDepth;
	if (Depth > 0)
	{
		GameDepth = 0;
		Owner.store(0);
	}
	WakeEvent->Trigger();
	while (bStepRunning.load(std::memory_order_acquire))
	{
		FPlatformProcess::SleepNoStats(0.f);
	}
	if (Depth > 0)
	{
		EnterGame();
		GameDepth = Depth;
	}
}

void FGenSimThread::TakeGateWait(double& OutTotal, double& OutMax)
{
	OutTotal = GateWaitTotal;
	OutMax = GateWaitMax;
	GateWaitTotal = GateWaitMax = 0.0;
}
