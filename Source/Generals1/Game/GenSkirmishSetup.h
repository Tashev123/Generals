#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GenSkirmishSetup.generated.h"

struct FGenMapInfo;
struct FGenSkirmishSlot;

/** GameInfo.h SlotState */
enum class EGenSlotState : uint8
{
	Open,
	Closed,
	EasyAI,
	MediumAI,
	HardAI,
	Player,
};

/** GameSlot: one of the eight places in a game. -1 everywhere means "left to chance" (or no team). */
struct GENERALS1_API FGenSetupSlot
{
	EGenSlotState State = EGenSlotState::Closed;
	int32 Color = -1;       // index into FGenShellData's colours
	int32 Side = -1;        // index into FGenShellData's sides
	int32 StartPos = -1;
	int32 Team = -1;
	FString Name;           // a player's name (a network game's humans; the local game's player has PlayerName)

	bool IsAI() const { return State == EGenSlotState::EasyAI || State == EGenSlotState::MediumAI || State == EGenSlotState::HardAI; }
	bool IsOccupied() const { return State == EGenSlotState::Player || IsAI(); }
	bool operator==(const FGenSetupSlot& O) const
	{
		return State == O.State && Color == O.Color && Side == O.Side && StartPos == O.StartPos && Team == O.Team && Name == O.Name;
	}
};

/**
 * A game being set up (the original's GameInfo): the map, who is in which slot with which side, colour,
 * team and start position, the starting cash - and the rules for changing them. The skirmish menu edits
 * one (UGenSkirmishSetup); a network game's host keeps one for everybody in its lobby.
 */
struct GENERALS1_API FGenSetupData
{
	static constexpr int32 MaxSlots = 8;

	FString Map;
	FGenSetupSlot Slots[MaxSlots];
	int32 StartingCash = 10000;
	FString PlayerName;
	/** Not the original's: the players start with a battle group to try things out with. */
	bool bTestUnits = true;

	const FGenMapInfo* GetMapInfo() const;
	int32 GetNumPlayers() const;

	/** GameInfo::setMap: start positions are forgotten and the slots cut down (or opened up) to what the map holds. */
	void SetMap(const FString& Name);
	/** GameSlot::setState: anything but one computer player replacing another clears the slot's choices. Nobody
	 *  becomes a human player this way, and a human's slot can't be changed (the first slot of a skirmish is its player). */
	void SetSlotState(int32 Slot, EGenSlotState State);
	/** False (and nothing changes) if somebody else has that colour / start position. */
	bool SetSlotColor(int32 Slot, int32 Color);
	bool SetSlotStartPos(int32 Slot, int32 Pos);
	void SetSlotSide(int32 Slot, int32 Side);
	void SetSlotTeam(int32 Slot, int32 Team);
	bool IsColorTaken(int32 Color, int32 ExceptSlot) const;
	/** Which slot starts at that position, or INDEX_NONE. */
	int32 FindSlotAtStart(int32 Pos) const;
	/** GameInfo::adjustSlotsForMap: no more places than the map has start positions. */
	void AdjustSlotsForMap();

	/**
	 * GameLogic::startNewGame: what was left to chance is settled (populateRandomSideAndColor,
	 * populateRandomStartPosition) and the slots are turned into the players of a game, in slot order
	 * (OutSlotOfPlayer[i]: the slot player i + 1 came from). Returns an error to show instead if the
	 * game can't start.
	 */
	FString Resolve(FRandomStream& Random, TArray<FGenSkirmishSlot>& OutSlots, TArray<int32>* OutSlotOfPlayer = nullptr) const;

	/** For the network: the whole of it. */
	void Serialize(FArchive& Ar);
};

/**
 * The skirmish game being set up (the original's TheSkirmishGameInfo with its SkirmishPreferences).
 * It lives with the game instance, so it is still there when the level is loaded again after a game,
 * and it is written to the user's settings so the menu comes back the way it was left.
 */
UCLASS()
class GENERALS1_API UGenSkirmishSetup : public UGameInstanceSubsystem, public FGenSetupData
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxSlots = FGenSetupData::MaxSlots;

	static UGenSkirmishSetup* Get(const UObject* WorldContext);

	/** Reads the game data the menu needs and the saved setup; call before using anything else. */
	void EnsureLoaded();
	void Save() const;

	// --- carried from one load of the level to the next ---
	/** "Restart": the next load starts the same game at once instead of showing the menu. */
	bool bStartAtOnce = false;
	/** Test automation (-GenTestMenu): how far the script has got. */
	double TestSeconds = 0.0;
	int32 TestStepsDone = 0;

private:
	void Load();

	bool bLoaded = false;
	bool bPersist = true;
};
