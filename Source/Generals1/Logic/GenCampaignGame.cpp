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

void UGenGameLogic::ScriptRevealPermanently(int32 Player, const FVector2f& Pos, float Radius)
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
	Shroud.DoReveal(S);
}

void UGenGameLogic::SetActiveBoundary(int32 Boundary)
{
	// TerrainLogic::setActiveBoundary (the map actor keeps the map; the camera and the radar read the playable area).
	if (FGenMapData* Map = MapActor ? MapActor->GetMapDataForLogic() : nullptr)
	{
		if (Map->Boundaries.IsValidIndex(Boundary))
		{
			Map->ActiveBoundary = Boundary;
		}
	}
}

void UGenGameLogic::RemoveObjectByScript(FGenObject& Obj)
{
	// GameLogic::destroyObject: gone at the end of the step, without dying (and what is inside goes with it).
	Obj.bPendingDestroy = true;
}
