// A campaign mission's side of the simulation: its players are the map's sides (SidesList / Player::initFromDict),
// with the relations the map gives them; the map's scripts (FGenMapScripts) drive and decide the game, and these
// are the logic's services they use.

#include "Logic/GenGameLogic.h"
#include "Logic/GenGameData.h"
#include "Logic/GenMapScripts.h"
#include "Map/GenMapActor.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	FString DictString(const FGenDict& D, const TCHAR* Key)
	{
		const FGenDictValue* V = D.Find(Key);
		return V ? V->String : FString();
	}

	/** The colours of sides whose map says none and whose template prefers none of its own. */
	const FLinearColor CampaignPalette[] = {
		FLinearColor(0.0f, 0.35f, 1.0f), FLinearColor(1.0f, 0.1f, 0.1f), FLinearColor(0.1f, 0.85f, 0.1f), FLinearColor(1.0f, 0.85f, 0.0f),
		FLinearColor(1.0f, 0.5f, 0.0f), FLinearColor(0.0f, 0.85f, 0.85f), FLinearColor(0.7f, 0.2f, 0.9f), FLinearColor(1.0f, 0.5f, 0.75f),
	};
}

void UGenGameLogic::SetupCampaignPlayers()
{
	// SidesList: side 0 is the neutral player (it owns what is nobody's); every other side is a player, in order, so
	// that the n-th side's scripts are the n-th player's.
	const FGenMapData* Map = GetMap();
	FGenGameData& GD = FGenGameData::Get();
	if (Map->Sides.Num() == 0 || !DictString(Map->Sides[0].Properties, TEXT("playerName")).IsEmpty())
	{
		UE_LOG(LogGenerals, Warning, TEXT("Campaign map: side 0 isn't the neutral side - its scripts may go to the wrong players"));
	}
	Players[0].bScriptRelations = true;
	for (int32 Side = 1; Side < Map->Sides.Num(); ++Side)
	{
		const FGenDict& D = Map->Sides[Side].Properties;
		FGenPlayer& P = Players.AddDefaulted_GetRef();
		P.Index = Players.Num() - 1;
		P.ScriptName = DictString(D, TEXT("playerName"));
		P.Name = DictString(D, TEXT("playerDisplayName"));
		if (P.Name.IsEmpty())
		{
			P.Name = P.ScriptName;
		}
		P.Template = GD.FindPlayerTemplate(DictString(D, TEXT("playerFaction")));
		const FGenDictValue* Human = D.Find(TEXT("playerIsHuman"));
		P.bHuman = Human && Human->Bool;
		P.bScriptRelations = true;
		P.Difficulty = Match.DefaultDifficulty;
		// Player::initFromDict: the map's colour, else the faction's preferred one.
		if (const FGenDictValue* Color = D.Find(TEXT("playerColor")))
		{
			const uint32 ARGB = uint32(Color->Int);
			P.Color = FLinearColor(FColor(uint8((ARGB >> 16) & 0xff), uint8((ARGB >> 8) & 0xff), uint8(ARGB & 0xff)));
		}
		else if (P.Template && P.Template->PreferredColor != FColor::White)
		{
			P.Color = FLinearColor(P.Template->PreferredColor);
		}
		else
		{
			P.Color = CampaignPalette[P.Index % UE_ARRAY_COUNT(CampaignPalette)];
		}
		// Player::init: the template's money, else (no game info in a campaign) GameData's DefaultStartingCash.
		P.Money = (P.Template && P.Template->StartMoney > 0) ? P.Template->StartMoney : 10000;
		InitPlayerSciences(P);
		if (P.bHuman && LocalPlayer == INDEX_NONE)
		{
			LocalPlayer = P.Index;
		}
		if (!P.Template)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Campaign side '%s': unknown faction '%s'"), *P.ScriptName, *DictString(D, TEXT("playerFaction")));
		}
	}
	// "playerAllies" / "playerEnemies": names of players, separated by spaces.
	for (int32 Side = 1; Side < Map->Sides.Num() && Side < Players.Num(); ++Side)
	{
		FGenPlayer& P = Players[Side];
		for (const TCHAR* Key : { TEXT("playerAllies"), TEXT("playerEnemies") })
		{
			TArray<FString> Names;
			DictString(Map->Sides[Side].Properties, Key).ParseIntoArrayWS(Names);
			for (const FString& Name : Names)
			{
				const FGenPlayer* Other = Players.FindByPredicate([&Name](const FGenPlayer& O) { return O.ScriptName.Equals(Name, ESearchCase::CaseSensitive); });
				if (Other && Other->Index != P.Index)
				{
					P.Relations.Add(Other->Index, FCString::Strcmp(Key, TEXT("playerAllies")) == 0 ? EGenRelationship::Allies : EGenRelationship::Enemies);
				}
			}
		}
	}
	if (IsChallenge())
	{
		SetupChallengePlayer();
	}
}

void UGenGameLogic::SetupChallengePlayer()
{
	// GameLogic::startNewGame with TheChallengeGameInfo: the sides stay as the map has them, and one more comes after
	// them for the general chosen. "Some designers have used 'ThePlayer' as an empty player": it stays, and its enemies
	// become the local player's; a map without it has everybody but the neutral and civilian players against the local player.
	FGenGameData& GD = FGenGameData::Get();
	const int32 Dummy = Players.IndexOfByPredicate([](const FGenPlayer& O) { return O.ScriptName == TEXT("ThePlayer"); });
	if (Dummy != INDEX_NONE)
	{
		Players[Dummy].bHuman = true; // (nobody's: no computer player for it)
	}
	const int32 Local = Players.AddDefaulted();
	{
		FGenPlayer& P = Players[Local];
		P.Index = Local;
		P.ScriptName = TEXT("player0");
		P.Name = TEXT("Challenger");
		P.Template = GD.FindPlayerTemplate(Match.ChallengeFaction);
		P.bHuman = true;
		P.bScriptRelations = true;
		P.Difficulty = Match.DefaultDifficulty;
		P.StartPosition = 0;
		P.Color = (P.Template && P.Template->PreferredColor != FColor::White) ? FLinearColor(P.Template->PreferredColor) : CampaignPalette[0];
		P.Money = (P.Template && P.Template->StartMoney > 0) ? P.Template->StartMoney : 10000;
		InitPlayerSciences(P);
		if (!P.Template)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Challenge: unknown faction '%s'"), *Match.ChallengeFaction);
		}
	}
	LocalPlayer = Local;
	for (int32 i = 1; i < Local; ++i)
	{
		bool bEnemy;
		if (Dummy != INDEX_NONE)
		{
			const EGenRelationship* ToThem = Players[Dummy].Relations.Find(i);
			const EGenRelationship* ToDummy = Players[i].Relations.Find(Dummy);
			bEnemy = (ToThem && *ToThem == EGenRelationship::Enemies) || (ToDummy && *ToDummy == EGenRelationship::Enemies);
			if (!bEnemy)
			{
				continue;
			}
		}
		else
		{
			bEnemy = Players[i].ScriptName != TEXT("PlyrCivilian");
		}
		const EGenRelationship Relation = bEnemy ? EGenRelationship::Enemies : EGenRelationship::Neutral;
		Players[i].Relations.Add(Local, Relation);
		Players[Local].Relations.Add(i, Relation);
	}
}

void UGenGameLogic::RefreshAllianceMasks()
{
	for (FGenPlayer& P : Players)
	{
		P.AlliesMask = 0;
		P.OthersMask = 0;
		for (const FGenPlayer& Q : Players)
		{
			if (GetRelationship(P.Index, Q.Index) == EGenRelationship::Allies) { P.AlliesMask |= 1u << Q.Index; }
			else { P.OthersMask |= 1u << Q.Index; }
		}
	}
}

void UGenGameLogic::SetPlayerRelationship(int32 From, int32 To, EGenRelationship Relation)
{
	if (!Players.IsValidIndex(From) || !Players.IsValidIndex(To) || From == To)
	{
		return;
	}
	Players[From].Relations.Add(To, Relation);
	RefreshAllianceMasks();
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [script f%u] p%d (%s) now sees p%d (%s) as %s"), Frame, From, *Players[From].ScriptName, To, *Players[To].ScriptName,
		Relation == EGenRelationship::Allies ? TEXT("an ally") : (Relation == EGenRelationship::Enemies ? TEXT("an enemy") : TEXT("neutral")));
}

void UGenGameLogic::ScriptGameOver(bool bLocalWins)
{
	if (bGameOver)
	{
		return;
	}
	bGameOver = true;
	GameOverFrame = Frame;
	WinningPlayer = bLocalWins ? LocalPlayer : INDEX_NONE;
	if (!bLocalWins && Players.IsValidIndex(LocalPlayer))
	{
		Players[LocalPlayer].bDefeated = true;
	}
	UE_LOG(LogGenerals, Log, TEXT("  [victory f%u] the mission's scripts decide it: %s"), Frame, bLocalWins ? TEXT("victory") : TEXT("defeat"));
}

void UGenGameLogic::ScriptRevealMapPermanently(int32 Player, bool bReveal)
{
	if (!Shroud.IsValid())
	{
		return;
	}
	if (bReveal)
	{
		Shroud.RevealMapForPlayerPermanently(Player);
	}
	else
	{
		Shroud.UndoRevealMapForPlayerPermanently(Player);
	}
}

void UGenGameLogic::ScriptShroudMap(int32 Player)
{
	if (Shroud.IsValid())
	{
		Shroud.ShroudMapForPlayer(Player);
	}
}

void UGenGameLogic::ScriptShroudOnce(int32 Player, const FVector2f& Pos, float Radius)
{
	if (!Shroud.IsValid() || !Players.IsValidIndex(Player))
	{
		return;
	}
	FGenSighting S;
	Shroud.WorldToCell(Pos.X, Pos.Y, S.CellX, S.CellY);
	S.CellRadius = Shroud.WorldToCellDist(Radius);
	S.Mask = 1u << Player;
	Shroud.ShroudCells(S);
}

bool UGenGameLogic::GetScriptWaterHeight(const FString& Name, float& OutHeight) const
{
	const FGenMapData* Map = MapActor ? MapActor->GetMapData() : nullptr;
	const FGenPolygonTrigger* Area = Map ? Map->FindTrigger(Name) : nullptr;
	if (!Area || !Area->bWater)
	{
		return false;
	}
	OutHeight = Area->GetWaterHeight();
	return true;
}

void UGenGameLogic::SetScriptWaterHeight(const FString& Name, float Height, bool bFinal)
{
	FGenMapData* Map = MapActor ? MapActor->GetMapDataForLogic() : nullptr;
	if (!Map)
	{
		return;
	}
	for (FGenPolygonTrigger& T : Map->PolygonTriggers)
	{
		if (T.bWater && T.Name == Name)
		{
			FBox2f Bounds(ForceInit);
			for (FIntVector& Pt : T.Points)
			{
				Pt.Z = FMath::RoundToInt32(Height);
				Bounds += FVector2f(float(Pt.X), float(Pt.Y));
			}
			if (bFinal)
			{
				// (TerrainLogic::setWaterHeight: "changes in the water level force us to recalculate the pathfinding map".)
				Pathfinder.RefreshWater(*Map, Bounds);
			}
			return;
		}
	}
}

void UGenGameLogic::ScriptRevealPermanently(int32 Player, const FVector2f& Pos, float Radius, bool bUndo)
{
	// ScriptEngine::createNamedMapReveal + doNamedMapReveal: a look at the spot that is never taken back.
	if (!Shroud.IsValid() || !Players.IsValidIndex(Player))
	{
		return;
	}
	FGenSighting S;
	Shroud.WorldToCell(Pos.X, Pos.Y, S.CellX, S.CellY);
	S.CellRadius = Shroud.WorldToCellDist(Radius);
	S.Mask = 1u << Player;
	if (bUndo)
	{
		Shroud.UndoReveal(S);
	}
	else
	{
		Shroud.DoReveal(S);
	}
}

void UGenGameLogic::ScriptRevealOnce(int32 Player, const FVector2f& Pos, float Radius)
{
	// doRevealMapAtWaypoint: doShroudReveal and undoShroudReveal at once.
	ScriptRevealPermanently(Player, Pos, Radius, false);
	ScriptRevealPermanently(Player, Pos, Radius, true);
}

void UGenGameLogic::ScriptRevealMapOnce(int32 Player)
{
	// doRevealMapEntire: PartitionManager::revealMapForPlayer.
	if (Shroud.IsValid() && Players.IsValidIndex(Player))
	{
		Shroud.RevealMapForPlayer(Player);
	}
}

void UGenGameLogic::SetActiveBoundary(int32 Boundary)
{
	// TerrainLogic::setActiveBoundary (the map actor keeps the map; the camera and the radar read the playable area).
	FGenMapData* Map = MapActor ? MapActor->GetMapDataForLogic() : nullptr;
	if (!Map || !Map->Boundaries.IsValidIndex(Boundary) || Boundary == Map->ActiveBoundary)
	{
		return;
	}
	const FIntPoint New = Map->Boundaries[Boundary];
	if (New.X <= 0 || New.Y <= 0)
	{
		return;
	}
	Map->ActiveBoundary = Boundary;
	if (Shroud.IsValid())
	{
		// "causes reset of PartitionManager": the fog starts over on the new area, and only what can be carried over
		// is (PartitionManager::storeFoggedCells / restoreFoggedCells). Objects forget what they looked at, and look again.
		const FBox2f Bounds = Map->GetPlayableBounds();
		Shroud.SwitchActiveArea(FMath::CeilToInt32(Bounds.Max.X / Shroud.GetCellSize()), FMath::CeilToInt32(Bounds.Max.Y / Shroud.GetCellSize()),
			[this]()
			{
				for (FGenObjectID Id : ObjectOrder)
				{
					Objects[Id]->LastLook = FGenSighting();
					Objects[Id]->LastRevealAll = FGenSighting();
				}
			},
			[this]()
			{
				for (FGenObjectID Id : ObjectOrder)
				{
					UpdateObjectLook(*Objects[Id]);
				}
			});
	}
}

void UGenGameLogic::RemoveObjectByScript(FGenObject& Obj)
{
	// GameLogic::destroyObject: gone at the end of the step, without dying (and what is inside goes with it).
	Obj.bPendingDestroy = true;
}
