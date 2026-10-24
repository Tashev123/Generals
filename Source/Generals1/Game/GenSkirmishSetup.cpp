#include "Game/GenSkirmishSetup.h"
#include "Game/GenShellData.h"
#include "Logic/GenGameLogic.h"
#include "Generals1.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

namespace
{
	const TCHAR* ConfigSection = TEXT("Generals.Skirmish");
	const TCHAR* StateNames[] = { TEXT("Open"), TEXT("Closed"), TEXT("EasyAI"), TEXT("MediumAI"), TEXT("HardAI"), TEXT("Player") };
}

UGenSkirmishSetup* UGenSkirmishSetup::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UGenSkirmishSetup>() : nullptr;
}

void UGenSkirmishSetup::EnsureLoaded()
{
	if (bLoaded)
	{
		return;
	}
	bLoaded = true;
	FGenShellData::Get().Initialize();
	Load();
}

const FGenMapInfo* FGenSetupData::GetMapInfo() const
{
	return FGenShellData::Get().FindMap(Map);
}

int32 FGenSetupData::GetNumPlayers() const
{
	int32 Count = 0;
	for (const FGenSetupSlot& S : Slots)
	{
		Count += S.IsOccupied() ? 1 : 0;
	}
	return Count;
}

void UGenSkirmishSetup::Load()
{
	const FGenShellData& Data = FGenShellData::Get();

	// SkirmishGameOptionsMenu: the player in the first slot, an easy army in the second, the rest open.
	for (FGenSetupSlot& S : Slots)
	{
		S = FGenSetupSlot();
		S.State = EGenSlotState::Open;
	}
	Slots[0].State = EGenSlotState::Player;
	Slots[1].State = EGenSlotState::EasyAI;
	StartingCash = Data.GetDefaultMoney();
	PlayerName = FGenShellData::Text(TEXT("GUI:Player"), TEXT("Player"));
	Map = TEXT("tournament desert");

	// (A scripted run of the menu starts from the defaults and leaves the user's own setup alone.)
	FString Ignored;
	bPersist = !FParse::Value(FCommandLine::Get(), TEXT("GenTestMenu="), Ignored, false);
	if (GConfig && bPersist)
	{
		FString Value;
		if (GConfig->GetString(ConfigSection, TEXT("Map"), Value, GGameUserSettingsIni) && Data.FindMap(Value)) { Map = Value; }
		if (GConfig->GetString(ConfigSection, TEXT("PlayerName"), Value, GGameUserSettingsIni) && !Value.IsEmpty()) { PlayerName = Value.Left(24); }
		int32 Cash = 0;
		if (GConfig->GetInt(ConfigSection, TEXT("StartingCash"), Cash, GGameUserSettingsIni) && Data.GetMoneyChoices().Contains(Cash)) { StartingCash = Cash; }
		GConfig->GetBool(ConfigSection, TEXT("TestUnits"), bTestUnits, GGameUserSettingsIni);
		for (int32 i = 0; i < MaxSlots; ++i)
		{
			// "State,Colour,SideTemplate,StartPos,Team"
			TArray<FString> Parts;
			if (!GConfig->GetString(ConfigSection, *FString::Printf(TEXT("Slot%d"), i), Value, GGameUserSettingsIni) || Value.ParseIntoArray(Parts, TEXT(","), false) < 5)
			{
				continue;
			}
			FGenSetupSlot& S = Slots[i];
			for (int32 k = 0; k < UE_ARRAY_COUNT(StateNames); ++k)
			{
				if (Parts[0].Equals(StateNames[k], ESearchCase::IgnoreCase)) { S.State = EGenSlotState(k); }
			}
			S.Color = FCString::Atoi(*Parts[1]);
			S.Side = Data.FindSide(Parts[2]);
			S.StartPos = FCString::Atoi(*Parts[3]);
			S.Team = FCString::Atoi(*Parts[4]);
			if (S.Color < -1 || S.Color >= Data.GetColors().Num() || IsColorTaken(S.Color, i)) { S.Color = -1; }
			if (S.Team < -1 || S.Team >= MaxSlots / 2) { S.Team = -1; }
		}
	}
	// Whatever the file said: the first slot is the player's, and nobody else is one.
	Slots[0].State = EGenSlotState::Player;
	for (int32 i = 1; i < MaxSlots; ++i)
	{
		if (Slots[i].State == EGenSlotState::Player) { Slots[i].State = EGenSlotState::Open; }
		if (!Slots[i].IsOccupied()) { Slots[i].Color = Slots[i].Side = Slots[i].StartPos = Slots[i].Team = -1; }
	}
	if (!Data.FindMap(Map) && Data.GetMaps().Num() > 0)
	{
		Map = Data.GetMaps()[0].Name;
	}
	// Start positions the map doesn't have, or two slots on one, are forgotten.
	const FGenMapInfo* Info = GetMapInfo();
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		FGenSetupSlot& S = Slots[i];
		if (S.StartPos < -1 || !Info || S.StartPos >= Info->NumPlayers) { S.StartPos = -1; }
		for (int32 k = 0; k < i; ++k)
		{
			if (S.StartPos >= 0 && Slots[k].StartPos == S.StartPos) { S.StartPos = -1; }
		}
	}
	AdjustSlotsForMap();
}

void UGenSkirmishSetup::Save() const
{
	if (!GConfig || !bLoaded || !bPersist)
	{
		return;
	}
	const FGenShellData& Data = FGenShellData::Get();
	GConfig->SetString(ConfigSection, TEXT("Map"), *Map, GGameUserSettingsIni);
	GConfig->SetString(ConfigSection, TEXT("PlayerName"), *PlayerName, GGameUserSettingsIni);
	GConfig->SetInt(ConfigSection, TEXT("StartingCash"), StartingCash, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("TestUnits"), bTestUnits, GGameUserSettingsIni);
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		const FGenSetupSlot& S = Slots[i];
		const FString Side = Data.GetSides().IsValidIndex(S.Side) ? Data.GetSides()[S.Side].Template : FString(TEXT("Random"));
		GConfig->SetString(ConfigSection, *FString::Printf(TEXT("Slot%d"), i),
			*FString::Printf(TEXT("%s,%d,%s,%d,%d"), StateNames[int32(S.State)], S.Color, *Side, S.StartPos, S.Team), GGameUserSettingsIni);
	}
	GConfig->Flush(false, GGameUserSettingsIni);
}

void FGenSetupData::AdjustSlotsForMap()
{
	// GameInfo::adjustSlotsForMap: no more places than the map has start positions - open ones close
	// first, from the bottom, then armies go; and closed ones open up again when there is room.
	const FGenMapInfo* Info = GetMapInfo();
	if (!Info)
	{
		return;
	}
	auto CountPlaces = [this]()
	{
		int32 Count = 0;
		for (const FGenSetupSlot& S : Slots) { Count += (S.IsOccupied() || S.State == EGenSlotState::Open) ? 1 : 0; }
		return Count;
	};
	for (int32 i = MaxSlots - 1; i >= 1 && CountPlaces() > Info->NumPlayers; --i)
	{
		if (Slots[i].State == EGenSlotState::Open) { Slots[i].State = EGenSlotState::Closed; }
	}
	for (int32 i = MaxSlots - 1; i >= 1 && CountPlaces() > Info->NumPlayers; --i)
	{
		if (Slots[i].IsAI()) { SetSlotState(i, EGenSlotState::Closed); }
	}
	for (int32 i = 1; i < MaxSlots && CountPlaces() < Info->NumPlayers; ++i)
	{
		if (Slots[i].State == EGenSlotState::Closed) { Slots[i].State = EGenSlotState::Open; }
	}
}

void FGenSetupData::SetMap(const FString& Name)
{
	if (!FGenShellData::Get().FindMap(Name))
	{
		return;
	}
	Map = Name;
	for (FGenSetupSlot& S : Slots)
	{
		S.StartPos = -1;
	}
	AdjustSlotsForMap();
}

void FGenSetupData::SetSlotState(int32 Slot, EGenSlotState State)
{
	if (Slot < 0 || Slot >= MaxSlots || State == EGenSlotState::Player || Slots[Slot].State == EGenSlotState::Player)
	{
		return; // (a player's place is the player's)
	}
	FGenSetupSlot& S = Slots[Slot];
	const bool bNewIsAI = State == EGenSlotState::EasyAI || State == EGenSlotState::MediumAI || State == EGenSlotState::HardAI;
	if (!(S.IsAI() && bNewIsAI))
	{
		S.Color = S.Side = S.StartPos = S.Team = -1;
	}
	S.State = State;
}

bool FGenSetupData::IsColorTaken(int32 Color, int32 ExceptSlot) const
{
	if (Color < 0)
	{
		return false;
	}
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		if (i != ExceptSlot && Slots[i].IsOccupied() && Slots[i].Color == Color)
		{
			return true;
		}
	}
	return false;
}

bool FGenSetupData::SetSlotColor(int32 Slot, int32 Color)
{
	if (Slot < 0 || Slot >= MaxSlots || Color < -1 || Color >= FGenShellData::Get().GetColors().Num() || IsColorTaken(Color, Slot))
	{
		return false;
	}
	Slots[Slot].Color = Color;
	return true;
}

int32 FGenSetupData::FindSlotAtStart(int32 Pos) const
{
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		if (Pos >= 0 && Slots[i].IsOccupied() && Slots[i].StartPos == Pos)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

bool FGenSetupData::SetSlotStartPos(int32 Slot, int32 Pos)
{
	// handleStartPositionSelection
	const FGenMapInfo* Info = GetMapInfo();
	if (Slot < 0 || Slot >= MaxSlots || Pos < -1 || !Info || Pos >= Info->NumPlayers)
	{
		return false;
	}
	const int32 Holder = FindSlotAtStart(Pos);
	if (Pos >= 0 && Holder != INDEX_NONE && Holder != Slot)
	{
		return false;
	}
	Slots[Slot].StartPos = Pos;
	return true;
}

void FGenSetupData::SetSlotSide(int32 Slot, int32 Side)
{
	if (Slot >= 0 && Slot < MaxSlots && Side >= -1 && Side < FGenShellData::Get().GetSides().Num())
	{
		Slots[Slot].Side = Side;
	}
}

void FGenSetupData::SetSlotTeam(int32 Slot, int32 Team)
{
	if (Slot >= 0 && Slot < MaxSlots && Team >= -1 && Team < MaxSlots / 2)
	{
		Slots[Slot].Team = Team;
	}
}

FString FGenSetupData::Resolve(FRandomStream& Random, TArray<FGenSkirmishSlot>& OutSlots, TArray<int32>* OutSlotOfPlayer) const
{
	const FGenShellData& Data = FGenShellData::Get();
	const FGenMapInfo* Info = GetMapInfo();
	if (!Info)
	{
		return FGenShellData::Text(TEXT("GUI:CantFindMap"), TEXT("The map can't be found"));
	}
	if (GetNumPlayers() > Info->NumPlayers)
	{
		return FGenShellData::Text(TEXT("GUI:TooManyPlayers"), TEXT("This map only supports %d players")).Replace(TEXT("%d"), *FString::FromInt(Info->NumPlayers));
	}
	if (Data.GetSides().Num() == 0 || Data.GetColors().Num() < GetNumPlayers())
	{
		return TEXT("The game's data is incomplete");
	}

	FGenSetupSlot Work[MaxSlots];
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		Work[i] = Slots[i];
	}

	// populateRandomSideAndColor
	for (FGenSetupSlot& S : Work)
	{
		if (!S.IsOccupied())
		{
			continue;
		}
		if (!Data.GetSides().IsValidIndex(S.Side))
		{
			S.Side = Random.RandRange(0, Data.GetSides().Num() - 1);
		}
		while (!Data.GetColors().IsValidIndex(S.Color))
		{
			const int32 Pick = Random.RandRange(0, Data.GetColors().Num() - 1);
			bool bTaken = false;
			for (const FGenSetupSlot& Other : Work)
			{
				bTaken |= (&Other != &S) && Other.IsOccupied() && Other.Color == Pick;
			}
			S.Color = bTaken ? -1 : Pick;
		}
	}

	// populateRandomStartPosition: the first to be placed gets a spot at random; after that a team that
	// has no spot yet goes as far from everybody as it can, and a team mate as close to its team as it can.
	const int32 NumSpots = FMath::Min(Info->NumPlayers, MaxSlots);
	auto SpotDistance = [Info](int32 A, int32 B)
	{
		return (Info->Starts.IsValidIndex(A) && Info->Starts.IsValidIndex(B)) ? FVector2f::Distance(Info->Starts[A], Info->Starts[B]) : 1000000.f;
	};
	bool bTaken[MaxSlots];
	bool bAnyPicked = false;
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		bTaken[i] = i >= NumSpots;
	}
	for (const FGenSetupSlot& S : Work)
	{
		if (S.IsOccupied() && S.StartPos >= 0 && S.StartPos < NumSpots)
		{
			bTaken[S.StartPos] = true;
			bAnyPicked = true;
		}
	}
	int32 TeamSpot[MaxSlots];
	for (int32& T : TeamSpot)
	{
		T = -1;
	}
	for (FGenSetupSlot& S : Work)
	{
		if (!S.IsOccupied() || (S.StartPos >= 0 && S.StartPos < NumSpots))
		{
			continue;
		}
		const int32 Team = (S.Team >= 0 && S.Team < MaxSlots) ? S.Team : -1;
		int32 Pick = -1;
		if (!bAnyPicked)
		{
			while (Pick < 0)
			{
				Pick = Random.RandRange(0, NumSpots - 1);
				if (bTaken[Pick]) { Pick = -1; }
			}
			bAnyPicked = true;
			if (Team >= 0) { TeamSpot[Team] = Pick; }
		}
		else if (Team < 0 || TeamSpot[Team] < 0)
		{
			float Farthest = 0.f;
			for (int32 Spot = 0; Spot < NumSpots; ++Spot)
			{
				if (bTaken[Spot])
				{
					continue;
				}
				float Dist = 0.f;
				for (int32 n = 0; n < NumSpots; ++n)
				{
					if (bTaken[n] && n != Spot) { Dist += SpotDistance(Spot, n); }
				}
				if (Pick < 0 || Dist > Farthest)
				{
					Pick = Spot;
					Farthest = Dist;
				}
			}
			if (Team >= 0) { TeamSpot[Team] = Pick; }
		}
		else
		{
			float Closest = FLT_MAX;
			for (int32 Spot = 0; Spot < NumSpots; ++Spot)
			{
				const float Dist = SpotDistance(TeamSpot[Team], Spot);
				if (!bTaken[Spot] && Dist < Closest)
				{
					Closest = Dist;
					Pick = Spot;
				}
			}
		}
		if (Pick < 0)
		{
			return TEXT("There is no start position left");
		}
		S.StartPos = Pick;
		bTaken[Pick] = true;
	}

	OutSlots.Reset();
	if (OutSlotOfPlayer)
	{
		OutSlotOfPlayer->Reset();
	}
	for (int32 i = 0; i < MaxSlots; ++i)
	{
		const FGenSetupSlot& S = Work[i];
		if (!S.IsOccupied())
		{
			continue;
		}
		if (OutSlotOfPlayer)
		{
			OutSlotOfPlayer->Add(i);
		}
		FGenSkirmishSlot& Out = OutSlots.AddDefaulted_GetRef();
		Out.Faction = Data.GetSides()[S.Side].Template;
		Out.bHuman = S.State == EGenSlotState::Player;
		Out.StartPosition = S.StartPos;
		Out.Team = S.Team;
		Out.Color = Data.GetColors()[S.Color].Color;
		Out.Color.A = 1.f;
		switch (S.State)
		{
		case EGenSlotState::EasyAI: Out.Name = FGenShellData::Text(TEXT("GUI:EasyAI"), TEXT("Easy Army")); Out.Difficulty = 0; break;
		case EGenSlotState::MediumAI: Out.Name = FGenShellData::Text(TEXT("GUI:MediumAI"), TEXT("Medium Army")); Out.Difficulty = 1; break;
		case EGenSlotState::HardAI: Out.Name = FGenShellData::Text(TEXT("GUI:HardAI"), TEXT("Hard Army")); Out.Difficulty = 2; break;
		default: Out.Name = S.Name.IsEmpty() ? PlayerName : S.Name; break;
		}
	}
	return FString();
}

void FGenSetupData::Serialize(FArchive& Ar)
{
	Ar << Map << StartingCash << PlayerName;
	uint8 RawTestUnits = bTestUnits ? 1 : 0;
	Ar << RawTestUnits;
	bTestUnits = RawTestUnits != 0;
	for (FGenSetupSlot& S : Slots)
	{
		uint8 RawState = uint8(S.State);
		Ar << RawState;
		S.State = RawState <= uint8(EGenSlotState::Player) ? EGenSlotState(RawState) : EGenSlotState::Closed;
		Ar << S.Color << S.Side << S.StartPos << S.Team << S.Name;
	}
}
