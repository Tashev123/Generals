#include "Logic/GenRadiusDecal.h"
#include "Core/GenIni.h"

FGenRadiusDecalTemplate FGenRadiusDecalTemplate::Parse(const FGenIniBlock* Block)
{
	FGenRadiusDecalTemplate T;
	if (!Block)
	{
		return T;
	}
	auto Percent = [](const FString& V, float Default)
	{
		if (V.IsEmpty())
		{
			return Default;
		}
		FString S = V;
		const bool bPct = S.RemoveFromEnd(TEXT("%"));
		const float F = FCString::Atof(*S);
		return bPct ? F / 100.f : F; // INI::parsePercentToReal
	};
	T.Texture = Block->GetString(TEXT("Texture"));
	T.MinOpacity = Percent(Block->GetString(TEXT("OpacityMin")), 1.f);
	T.MaxOpacity = Percent(Block->GetString(TEXT("OpacityMax")), 1.f);
	if (const FGenIniEntry* E = Block->Find(TEXT("OpacityThrobTime")))
	{
		T.ThrobSeconds = FMath::Max(E->AsFloat() / 1000.f, 1.f / 30.f);
	}
	if (const FGenIniEntry* E = Block->Find(TEXT("Color")))
	{
		// INI::parseColorInt: "R:240 G:240 B:240 A:255"
		TArray<FString> Tokens;
		GenIni::Tokenize(E->Raw, Tokens, TEXT(" \t=:"));
		FColor C(0, 0, 0, 255);
		for (int32 i = 0; i + 1 < Tokens.Num(); ++i)
		{
			const uint8 V = uint8(FMath::Clamp(FCString::Atoi(*Tokens[i + 1]), 0, 255));
			if (Tokens[i].Equals(TEXT("R"), ESearchCase::IgnoreCase)) { C.R = V; }
			else if (Tokens[i].Equals(TEXT("G"), ESearchCase::IgnoreCase)) { C.G = V; }
			else if (Tokens[i].Equals(TEXT("B"), ESearchCase::IgnoreCase)) { C.B = V; }
			else if (Tokens[i].Equals(TEXT("A"), ESearchCase::IgnoreCase)) { C.A = V; }
		}
		T.Color = C;
		T.bPlayerColor = C.R == 0 && C.G == 0 && C.B == 0 && C.A == 0;
	}
	T.bOnlyVisibleToOwningPlayer = Block->GetBool(TEXT("OnlyVisibleToOwningPlayer"), true);
	return T;
}

float FGenRadiusDecalTemplate::GetOpacity(double Seconds) const
{
	// "theta = 2 PI * (now % throbTime) / throbTime; percent = 0.5 * (sin(theta) + 1)"
	const double Theta = 2.0 * UE_DOUBLE_PI * FMath::Fmod(Seconds, double(ThrobSeconds)) / double(ThrobSeconds);
	const float Percent = 0.5f * (float(FMath::Sin(Theta)) + 1.f);
	return FMath::Clamp(MinOpacity + Percent * (MaxOpacity - MinOpacity), 0.f, 1.f);
}
