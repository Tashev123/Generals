// The map selection's random map tab, as Age of Empires II's lobby has it: what the generated map is to be, and the
// picture of it (the map selection draws that) changes with every choice. The choices go into the map's name, and that
// name is all the rest of the game ever sees of them (Map/GenRandomMap.h).
//
// Ids for test automation: RandomMap.Environment=<item>, RandomMap.Layout=<item>, RandomMap.Size=<item>,
// RandomMap.Time=<item>, RandomMap.Resources=<item>, RandomMap.Players=<item>, RandomMap.Seed=<number>, RandomMap.NewSeed.

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSkirmishSetup.h"
#include "Generals1.h"

using namespace GenShellStyle;

namespace
{
	/** A seed for the player to see: short enough to read out to somebody. */
	int32 NewSeed()
	{
		return FMath::RandRange(1, 999999);
	}
}

void AGenHUD::OpenRandomMapOptions(const FGenSetupData* Setup)
{
	FGenRandomMapParams Chosen;
	if (Setup && FGenRandomMapParams::FromName(Setup->Map, Chosen))
	{
		Shell.RandomMap = Chosen;
	}
	else if (!Shell.bRandomMapSet)
	{
		// To begin with, all is left to a new seed, for as many players as are at the table now.
		Shell.RandomMap = FGenRandomMapParams();
		Shell.RandomMap.Players = FMath::Clamp(Setup ? Setup->GetNumPlayers() : 2, 2, 8);
		Shell.RandomMap.Seed = NewSeed();
	}
	Shell.bRandomMapSet = true;
	Shell.RandomSeedText = FString::FromInt(Shell.RandomMap.Seed);
}

void AGenHUD::DrawRandomMapOptions()
{
	static const TArray<FString> Sizes = { TEXT("Small"), TEXT("Medium"), TEXT("Large"), TEXT("Huge") };
	static const TArray<FString> Times = { TEXT("Random"), TEXT("Morning"), TEXT("Afternoon"), TEXT("Evening"), TEXT("Night") };
	static const TArray<FString> Resources = { TEXT("Scarce"), TEXT("Normal"), TEXT("Rich") };
	static const TArray<FString> Players = { TEXT("2"), TEXT("3"), TEXT("4"), TEXT("5"), TEXT("6"), TEXT("7"), TEXT("8") };

	FGenRandomMapParams& P = Shell.RandomMap;
	const float S = Shell.Scale;
	constexpr float LabelX = 270.f, ControlX = 450.f, ControlW = 340.f, RowH = 44.f, RowStep = 58.f;
	const FBox2D Area = ShellRect(250, 134, 560, 600);
	DrawRect(FLinearColor(0.f, 0.01f, 0.03f, 0.68f), Area.Min.X, Area.Min.Y, Area.GetSize().X, Area.GetSize().Y);

	float Y = 150.f;
	bool bChanged = false;
	auto DrawLabel = [&](const TCHAR* Label)
	{
		ShellText(Label, ShellRect(LabelX, 0, 0, 0).Min.X, ShellRect(0, Y + 10.f, 0, 0).Min.Y, ShellTextColor, 18.f);
	};
	// Value - First is the item of the list (the "Random" at the head of a list is Any, which is -1).
	auto DrawChoice = [&](const TCHAR* Label, const TCHAR* Id, const TArray<FString>& Items, int32& Value, int32 First)
	{
		DrawLabel(Label);
		int32 Picked = 0;
		if (ShellDropdown(Id, ShellRect(ControlX, Y, ControlW, RowH), Items, TArray<FLinearColor>(), Value - First, Picked))
		{
			Value = Picked + First;
			bChanged = true;
		}
		Y += RowStep;
	};
	DrawChoice(TEXT("Environment"), TEXT("RandomMap.Environment"), GenRandomMap::GetEnvironments(), P.Environment, FGenRandomMapParams::Any);
	DrawChoice(TEXT("Land"), TEXT("RandomMap.Layout"), GenRandomMap::GetLayouts(), P.Layout, FGenRandomMapParams::Any);
	DrawChoice(TEXT("Size"), TEXT("RandomMap.Size"), Sizes, P.Size, 0);
	DrawChoice(TEXT("Time of day"), TEXT("RandomMap.Time"), Times, P.TimeOfDay, FGenRandomMapParams::Any);
	DrawChoice(TEXT("Resources"), TEXT("RandomMap.Resources"), Resources, P.Resources, 0);
	DrawChoice(TEXT("Players"), TEXT("RandomMap.Players"), Players, P.Players, 2);

	// The seed: the same choices and the same seed make the same map, for whoever is told them.
	DrawLabel(TEXT("Seed"));
	bool bSeedDone = false;
	ShellTextField(ShellRect(ControlX, Y, 170.f, RowH), Shell.RandomSeedText, 9, Shell.bEditingSeed, FString(), bSeedDone);
	Shell.RandomSeedText = Shell.RandomSeedText.TrimStartAndEnd();
	{
		FString Digits;
		for (const TCHAR C : Shell.RandomSeedText)
		{
			if (FChar::IsDigit(C)) { Digits.AppendChar(C); }
		}
		Shell.RandomSeedText = MoveTemp(Digits);
	}
	if (Shell.TestClick.StartsWith(TEXT("RandomMap.Seed="), ESearchCase::IgnoreCase))
	{
		Shell.RandomSeedText = Shell.TestClick.Mid(15);
		Shell.TestClick.Reset();
		bSeedDone = true;
	}
	if (bSeedDone)
	{
		if (!Shell.RandomSeedText.IsEmpty())
		{
			bChanged = bChanged || P.Seed != FCString::Atoi(*Shell.RandomSeedText);
			P.Seed = FCString::Atoi(*Shell.RandomSeedText);
		}
		Shell.RandomSeedText = FString::FromInt(P.Seed);
	}
	if (ShellButton(TEXT("RandomMap.NewSeed"), ShellRect(ControlX + 180.f, Y, ControlW - 180.f, RowH), TEXT("NEW SEED")))
	{
		P.Seed = NewSeed();
		Shell.RandomSeedText = FString::FromInt(P.Seed);
		Shell.bEditingSeed = false;
		bChanged = true;
	}
	Y += RowStep + 8.f;

	// What the land is that was chosen, in as many lines as that takes.
	const float TextX = ShellRect(LabelX, 0, 0, 0).Min.X;
	const float TextWidth = 520.f * S;
	TArray<FString> Words;
	GenRandomMap::GetLayoutDescription(P.Layout).ParseIntoArray(Words, TEXT(" "));
	FString Line;
	auto FlushLine = [&]()
	{
		ShellText(Line, TextX, ShellRect(0, Y, 0, 0).Min.Y, ShellDim * 1.4f, 16.f);
		Y += 24.f;
		Line.Reset();
	};
	for (const FString& Word : Words)
	{
		const FString Longer = Line.IsEmpty() ? Word : Line + TEXT(" ") + Word;
		if (!Line.IsEmpty() && ShellMeasure(Longer, 16.f).X > TextWidth)
		{
			FlushLine();
			Line = Word;
		}
		else
		{
			Line = Longer;
		}
	}
	FlushLine();

	if (bChanged)
	{
		Shell.MapChoice = P.ToName();
	}
}
