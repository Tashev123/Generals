#include "Logic/GenConditions.h"
#include "Generals1.h"

int32 GenConditions::Index(const FString& Name)
{
	static TMap<FString, int32> Names;
	static FCriticalSection Lock;
	FScopeLock Guard(&Lock);
	const FString Key = Name.ToUpper();
	if (const int32* Found = Names.Find(Key))
	{
		return *Found;
	}
	const int32 New = Names.Num();
	if (New >= FGenConditionBits::MaxBits)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Too many model condition names (%s)"), *Name);
		return -1;
	}
	Names.Add(Key, New);
	return New;
}

FGenConditionBits GenConditions::Parse(const TArray<FString>& InNames)
{
	FGenConditionBits Bits;
	for (const FString& N : InNames)
	{
		if (!N.Equals(TEXT("NONE"), ESearchCase::IgnoreCase))
		{
			Bits.Set(Index(N));
		}
	}
	return Bits;
}

int32 GenConditions::Moving() { static const int32 I = Index(TEXT("MOVING")); return I; }
int32 GenConditions::Damaged() { static const int32 I = Index(TEXT("DAMAGED")); return I; }
int32 GenConditions::ReallyDamaged() { static const int32 I = Index(TEXT("REALLYDAMAGED")); return I; }
int32 GenConditions::Rubble() { static const int32 I = Index(TEXT("RUBBLE")); return I; }
int32 GenConditions::Night() { static const int32 I = Index(TEXT("NIGHT")); return I; }
int32 GenConditions::Snow() { static const int32 I = Index(TEXT("SNOW")); return I; }
