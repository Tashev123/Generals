#pragma once

#include "CoreMinimal.h"

/**
 * Where the time goes (-GenPerf): named timers that add up over a stretch of the game and are logged
 * and cleared every so often. Costs one branch per scope when switched off.
 */
namespace GenPerf
{
	GENERALS1_API extern bool bEnabled;

	/** The slot of a named timer (made on first use). */
	GENERALS1_API int32 Register(const TCHAR* Name);
	GENERALS1_API void Add(int32 Index, double Seconds);
	/** One line per timer that ran: total ms divided by Divisor (steps or frames), and how often it ran. Clears them. */
	GENERALS1_API void Report(const TCHAR* Heading, const TCHAR* Prefix, bool bPrefixWanted, double Divisor);
	GENERALS1_API void Reset();

	struct FScope
	{
		int32 Index;
		double Start;
		explicit FScope(int32 InIndex) : Index(InIndex), Start(bEnabled ? FPlatformTime::Seconds() : 0.0) {}
		~FScope()
		{
			if (bEnabled && Start != 0.0)
			{
				Add(Index, FPlatformTime::Seconds() - Start);
			}
		}
	};
}

/** Counts under that name (shown in the report as "so many per step"; names of counts end in '#'). */
#define GEN_PERF_COUNT(Name, N) \
	do { if (GenPerf::bEnabled) { static const int32 GenPerfCountIndex = GenPerf::Register(TEXT(Name)); GenPerf::Add(GenPerfCountIndex, double(N) / 1000.0); } } while (0)

#define GEN_PERF_CAT2(A, B) A##B
#define GEN_PERF_CAT(A, B) GEN_PERF_CAT2(A, B)
/** Times the rest of the enclosing block under that name. */
#define GEN_PERF_SCOPE(Name) \
	static const int32 GEN_PERF_CAT(GenPerfIndex, __LINE__) = GenPerf::Register(TEXT(Name)); \
	GenPerf::FScope GEN_PERF_CAT(GenPerfScope, __LINE__)(GEN_PERF_CAT(GenPerfIndex, __LINE__))
