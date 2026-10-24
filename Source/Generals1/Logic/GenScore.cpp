// The score (ScoreKeeper.cpp and the places that feed it: Player::onUnitCreated,
// Player::onStructureConstructionComplete, Object::scoreTheKill, the money a player earns).

#include "Logic/GenGameLogic.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"

namespace
{
	enum class EScoreKind : uint8 { None, Unit, Building };

	/**
	 * What an object counts as. A building: a STRUCTURE with SCORE, or with the flag for this occasion
	 * (SCORE_CREATE when built, SCORE_DESTROY when lost or destroyed). A unit: infantry or a vehicle with
	 * SCORE or that flag.
	 */
	EScoreKind KindFor(const FGenObject& Obj, const TCHAR* OccasionFlag)
	{
		const bool bScore = Obj.IsKindOf(TEXT("SCORE")) || Obj.IsKindOf(OccasionFlag);
		if (Obj.IsKindOf(TEXT("STRUCTURE")))
		{
			return bScore ? EScoreKind::Building : EScoreKind::None;
		}
		if (Obj.IsKindOf(TEXT("INFANTRY")) || Obj.IsKindOf(TEXT("VEHICLE")))
		{
			return bScore ? EScoreKind::Unit : EScoreKind::None;
		}
		return EScoreKind::None;
	}
}

int32 FGenScore::GetUnitsDestroyed() const
{
	// ("Design change, display even if we killed our own")
	int32 Count = 0;
	for (int32 N : UnitsDestroyed) { Count += N; }
	return Count;
}

int32 FGenScore::GetBuildingsDestroyed() const
{
	int32 Count = 0;
	for (int32 N : BuildingsDestroyed) { Count += N; }
	return Count;
}

int32 FGenScore::Calculate(int32 OwnIndex) const
{
	int32 Score = UnitsBuilt * 100 + MoneyEarned + BuildingsBuilt * 100;
	for (int32 i = 0; i < MaxPlayers; ++i)
	{
		if (i != OwnIndex)
		{
			Score += UnitsDestroyed[i] * 100 + BuildingsDestroyed[i] * 100;
		}
	}
	return Score;
}

void UGenGameLogic::ScoreObjectBuilt(const FGenObject& Obj)
{
	if (!Players.IsValidIndex(Obj.GetOwner()))
	{
		return;
	}
	FGenScore& Score = Players[Obj.GetOwner()].Score;
	switch (KindFor(Obj, TEXT("SCORE_CREATE")))
	{
	case EScoreKind::Building: ++Score.BuildingsBuilt; break;
	case EScoreKind::Unit: ++Score.UnitsBuilt; break;
	default: break;
	}
}

void UGenGameLogic::ScoreMoneyEarned(int32 Player, int32 Amount)
{
	if (Players.IsValidIndex(Player) && Amount > 0)
	{
		Players[Player].Score.MoneyEarned += Amount;
	}
}

void UGenGameLogic::ScoreMoneySpent(int32 Player, int32 Amount)
{
	if (Players.IsValidIndex(Player) && Amount > 0)
	{
		Players[Player].Score.MoneySpent += Amount;
	}
}

void UGenGameLogic::ScoreKill(const FGenObject& Victim, const FGenObject& Killer)
{
	// Object::scoreTheKill: kills of what belongs to nobody playing don't count, nor do things the
	// interface ignores; what is still being built is neither lost nor destroyed.
	const int32 VictimOwner = Victim.GetOwner();
	if (!Players.IsValidIndex(VictimOwner) || Players[VictimOwner].bNeutral || !Players[VictimOwner].Template || !Players[VictimOwner].Template->bPlayable
		|| Victim.IsKindOf(TEXT("IGNORED_IN_GUI")) || Victim.IsUnderConstruction())
	{
		return;
	}
	const EScoreKind Kind = KindFor(Victim, TEXT("SCORE_DESTROY"));
	if (Kind == EScoreKind::None)
	{
		return;
	}
	FGenScore& Lost = Players[VictimOwner].Score;
	++(Kind == EScoreKind::Building ? Lost.BuildingsLost : Lost.UnitsLost);

	// "Don't count kills that I do on my own buildings or units, cause thats just silly."
	const int32 KillerOwner = Killer.GetOwner();
	if (!Players.IsValidIndex(KillerOwner) || KillerOwner == VictimOwner || VictimOwner >= FGenScore::MaxPlayers
		|| GetRelationship(KillerOwner, VictimOwner) != EGenRelationship::Enemies)
	{
		return;
	}
	FGenScore& Won = Players[KillerOwner].Score;
	++(Kind == EScoreKind::Building ? Won.BuildingsDestroyed[VictimOwner] : Won.UnitsDestroyed[VictimOwner]);
}
