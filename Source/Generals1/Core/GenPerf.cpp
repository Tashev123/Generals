#include "Core/GenPerf.h"
#include "Generals1.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

// gen.ObjectCensus: how many engine objects there are of each class (the most numerous). Asked before
// and after a collection ("obj gc") it shows what the garbage was made of.
static FAutoConsoleCommand CmdGenObjectCensus(
	TEXT("gen.ObjectCensus"),
	TEXT("Logs the classes with the most engine objects."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		TMap<FName, int32> Counts;
		int32 Total = 0;
		for (FThreadSafeObjectIterator It; It; ++It)
		{
			++Total;
			++Counts.FindOrAdd(It->GetClass()->GetFName());
		}
		Counts.ValueSort([](int32 A, int32 B) { return A > B; });
		FString Line;
		int32 Listed = 0;
		for (const TPair<FName, int32>& It : Counts)
		{
			if (++Listed > 45) { break; }
			Line += FString::Printf(TEXT(" %s=%d"), *It.Key.ToString(), It.Value);
		}
		UE_LOG(LogGenerals, Log, TEXT("Object census: %d objects;%s"), Total, *Line);
	}));

namespace GenPerf
{
	bool bEnabled = false;

	namespace
	{
		struct FTimer
		{
			FString Name;
			double Seconds = 0.0;
			double MaxSeconds = 0.0;
			int32 Calls = 0;
		};

		// A table that never moves: the simulation's thread and the game thread both add to their own
		// timers while the other is at work (a timer belongs to one of them; the sums aren't exact to
		// the call when a report is read in the middle of things, which nobody minds).
		constexpr int32 MaxTimers = 256;
		FTimer GTimers[MaxTimers];
		int32 GNumTimers = 0;
		FCriticalSection GRegisterLock;
	}

	int32 Register(const TCHAR* Name)
	{
		FScopeLock Lock(&GRegisterLock);
		for (int32 i = 0; i < GNumTimers; ++i)
		{
			if (GTimers[i].Name == Name)
			{
				return i;
			}
		}
		if (GNumTimers >= MaxTimers)
		{
			return MaxTimers - 1; // (more names than room: the last one takes the overflow)
		}
		GTimers[GNumTimers].Name = Name;
		return GNumTimers++;
	}

	void Add(int32 Index, double Seconds)
	{
		if (Index >= 0 && Index < MaxTimers)
		{
			FTimer& T = GTimers[Index];
			T.Seconds += Seconds;
			T.MaxSeconds = FMath::Max(T.MaxSeconds, Seconds);
			++T.Calls;
		}
	}

	void Report(const TCHAR* Heading, const TCHAR* Prefix, bool bPrefixWanted, double Divisor)
	{
		TArray<FTimer*> Sorted;
		for (int32 i = 0; i < GNumTimers; ++i)
		{
			FTimer& T = GTimers[i];
			if (T.Calls > 0 && T.Name.StartsWith(Prefix) == bPrefixWanted)
			{
				Sorted.Add(&T);
			}
		}
		Sorted.Sort([](const FTimer& A, const FTimer& B) { return A.Seconds > B.Seconds; });
		FString Line;
		for (FTimer* T : Sorted)
		{
			Line += FString::Printf(TEXT(" %s=%.2f(max %.1f, x%.1f)"), *T->Name, T->Seconds * 1000.0 / FMath::Max(1.0, Divisor), T->MaxSeconds * 1000.0,
				double(T->Calls) / FMath::Max(1.0, Divisor));
			T->Seconds = 0.0;
			T->MaxSeconds = 0.0;
			T->Calls = 0;
		}
		UE_LOG(LogGenerals, Log, TEXT("  %s%s"), Heading, *Line);
	}

	void Reset()
	{
		for (int32 i = 0; i < GNumTimers; ++i)
		{
			GTimers[i].Seconds = 0.0;
			GTimers[i].MaxSeconds = 0.0;
			GTimers[i].Calls = 0;
		}
	}
}
