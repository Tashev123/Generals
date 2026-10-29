#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

/** CellShroudStatus: what a player knows about one cell of the map. */
enum class EGenCellShroud : uint8
{
	Clear,      // somebody of theirs is looking at it right now
	Fogged,     // explored, but nobody is looking
	Shrouded,   // never seen
};

/** ObjectShroudStatus. The order matters: anything past PartialClear is not drawn as it is. */
enum class EGenObjectShroud : uint8
{
	Clear,
	PartialClear,   // some of the cells it stands on are in view
	Fogged,         // out of view, but remembered: a building shown the way it was last seen
	Shrouded,       // not to be seen
};

/** SightingInfo: a circle of cells some players are looking at. */
struct GENERALS1_API FGenSighting
{
	int32 CellX = 0;
	int32 CellY = 0;
	int32 CellRadius = 0;
	uint32 Mask = 0;        // the players it counts for (one bit per player index); 0 = nothing

	bool IsValid() const { return Mask != 0; }
	bool operator==(const FGenSighting& O) const { return CellX == O.CellX && CellY == O.CellY && CellRadius == O.CellRadius && Mask == O.Mask; }
	bool operator!=(const FGenSighting& O) const { return !(*this == O); }
};

/** RadarEventType. */
enum class EGenRadarEvent : uint8
{
	Construction,
	Upgrade,
	UnderAttack,
	Information,
	Infiltration,
	StealthDiscovered,   // we found a stealthed unit
	StealthNeutralized,  // one of ours has been found
	Fake,           // marks a spot to jump to, but shows nothing
};

/** StealthLookType: how the local player sees a stealthed object. */
enum class EGenStealthLook : uint8
{
	None,                       // not stealthed: as usual
	VisibleFriendly,            // ours: faint, pulsing
	VisibleDetected,            // theirs, shown up by a detector
	VisibleFriendlyDetected,    // ours, and the enemy can see it
	Invisible,                  // theirs: not drawn
};

/** A marker that spins down onto a spot of the radar and fades. */
struct GENERALS1_API FGenRadarEvent
{
	EGenRadarEvent Type = EGenRadarEvent::Information;
	FVector3f Pos = FVector3f::ZeroVector;
	uint32 CreateFrame = 0;
	uint32 DieFrame = 0;
	uint32 FadeFrame = 0;
	FColor Color1 = FColor::White;
	FColor Color2 = FColor::White;
};

/**
 * The fog of war (the shroud half of PartitionManager.cpp): for every player, a count per map cell of
 * who is looking at it. A cell nobody has ever looked at is shrouded, one somebody looks at is clear,
 * and one that has been looked at but isn't any more is fogged. Kept for every player, as the original
 * does, so that anybody's view can be asked for.
 */
class GENERALS1_API FGenShroud
{
public:
	void Init(const FVector2f& MapSize, float InCellSize, int32 NumPlayers);
	void Reset();
	bool IsValid() const { return CellsX > 0 && CellsY > 0; }

	int32 GetCellsX() const { return CellsX; }
	int32 GetCellsY() const { return CellsY; }
	float GetCellSize() const { return CellSize; }
	void WorldToCell(float X, float Y, int32& OutX, int32& OutY) const
	{
		OutX = FMath::FloorToInt32(X / CellSize);
		OutY = FMath::FloorToInt32(Y / CellSize);
	}
	int32 WorldToCellDist(float W) const { return FMath::CeilToInt32(W / CellSize); }

	/** Outside the map everything is shrouded. */
	EGenCellShroud GetStatus(int32 Player, int32 X, int32 Y) const;
	EGenCellShroud GetStatusAt(int32 Player, float WorldX, float WorldY) const;

	/** PartitionManager::doShroudReveal / undoShroudReveal: the players of the mask start / stop looking at the circle. */
	void DoReveal(const FGenSighting& S);
	void UndoReveal(const FGenSighting& S);
	/** queueUndoShroudReveal: stop looking, but only at that frame (what was seen lingers a while). */
	void QueueUndoReveal(const FGenSighting& S, uint32 Frame);
	void ProcessQueue(uint32 Frame);

	/** revealMapForPlayer: everything explored (fogged), nothing more. */
	void RevealMapForPlayer(int32 Player);
	/** revealMapForPlayerPermanently: the player sees the whole map from now on. */
	void RevealMapForPlayerPermanently(int32 Player);
	/** undoRevealMapForPlayerPermanently: the permanent look is taken back (what was seen stays explored). */
	void UndoRevealMapForPlayerPermanently(int32 Player);
	/** shroudMapForPlayer: everything the player isn't looking at right now is shrouded again. */
	void ShroudMapForPlayer(int32 Player);

	/** Goes up whenever a cell changes its status for that player. */
	uint32 GetChangeSerial(int32 Player) const { return Serials.IsValidIndex(Player) ? Serials[Player] : 0; }

private:
	struct FSpan { int32 X0, X1, Y; };
	struct FPendingUndo { FGenSighting Sighting; uint32 Frame; };

	const TArray<FSpan>& GetCircle(int32 Radius);
	void AddLooker(int32 Player, int32 Index);
	void RemoveLooker(int32 Player, int32 Index);
	template <typename FnType> void ForEachCell(const FGenSighting& S, FnType Fn);

	int32 CellsX = 0;
	int32 CellsY = 0;
	float CellSize = 40.f;
	int32 PlayerCount = 0;
	TArray<int16> Current;                  // PartitionCell::m_currentShroud, [player][cell]: 1 shrouded, 0 fogged, -n n lookers
	TArray<uint32> Serials;
	TArray<FPendingUndo> Pending;           // in the order they fall due
	TMap<int32, TArray<FSpan>> Circles;     // DiscreteCircle spans by radius, around (0, 0)
};
