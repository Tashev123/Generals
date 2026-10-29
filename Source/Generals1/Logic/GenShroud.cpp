// Fog of war and what the radar knows.
// Follows the shroud parts of PartitionManager.cpp (PartitionCell::addLooker / removeLooker,
// doShroudReveal, the queue of pending unlooks, PartitionData::getShroudedStatus), DiscreteCircle.cpp,
// Object::look / unlook, DynamicShroudClearingRangeUpdate.cpp, RadarUpgrade.cpp / RadarUpdate.cpp,
// Player::hasRadar and the event half of Radar.cpp.

#include "Logic/GenShroud.h"
#include "Core/GenPerf.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenConditions.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Map/GenMapData.h"
#include "Generals1.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// ---- the cells ----------------------------------------------------------------------------------

void FGenShroud::Init(const FVector2f& MapSize, float InCellSize, int32 NumPlayers)
{
	Reset();
	CellSize = FMath::Max(1.f, InCellSize);
	CellsX = FMath::Max(1, FMath::CeilToInt32(MapSize.X / CellSize));
	CellsY = FMath::Max(1, FMath::CeilToInt32(MapSize.Y / CellSize));
	PlayerCount = FMath::Clamp(NumPlayers, 1, 32);
	// "Passive shroud": nobody has looked anywhere yet.
	Current.Init(1, PlayerCount * CellsX * CellsY);
	Serials.Init(1, PlayerCount);
}

void FGenShroud::Reset()
{
	CellsX = CellsY = 0;
	PlayerCount = 0;
	Current.Reset();
	Serials.Reset();
	Pending.Reset();
}

EGenCellShroud FGenShroud::GetStatus(int32 Player, int32 X, int32 Y) const
{
	if (Player < 0 || Player >= PlayerCount || X < 0 || Y < 0 || X >= CellsX || Y >= CellsY)
	{
		return EGenCellShroud::Shrouded;
	}
	const int16 C = Current[(Player * CellsY + Y) * CellsX + X];
	return C == 1 ? EGenCellShroud::Shrouded : (C == 0 ? EGenCellShroud::Fogged : EGenCellShroud::Clear);
}

EGenCellShroud FGenShroud::GetStatusAt(int32 Player, float WorldX, float WorldY) const
{
	int32 X, Y;
	WorldToCell(WorldX, WorldY, X, Y);
	return GetStatus(Player, X, Y);
}

void FGenShroud::AddLooker(int32 Player, int32 Index)
{
	// "The decreasing algorithm: a 1 will go straight to -1, otherwise it just gets decremented."
	int16& C = Current[Player * CellsX * CellsY + Index];
	const bool bWasClear = C < 0;
	C = int16(FMath::Min(int32(C) - 1, -1));
	if (!bWasClear)
	{
		++Serials[Player];
	}
}

void FGenShroud::RemoveLooker(int32 Player, int32 Index)
{
	// "The increasing algorithm: a -1 goes up to min(1, activeLevel), otherwise it just gets incremented."
	// (Nothing generates shroud in this port, so the active level is 0: what was seen stays explored.)
	int16& C = Current[Player * CellsX * CellsY + Index];
	if (C == -1)
	{
		C = 0;
		++Serials[Player];
	}
	else if (C < -1)
	{
		++C;
	}
}

const TArray<FGenShroud::FSpan>& FGenShroud::GetCircle(int32 Radius)
{
	if (const TArray<FSpan>* Found = Circles.Find(Radius))
	{
		return *Found;
	}
	// DiscreteCircle::generateEdgePairs (a Bresenham circle) for the upper half ...
	TArray<FSpan> Edges;
	int32 X = 0, Y = Radius, D = (1 - Radius) * 2;
	while (Y >= 0)
	{
		Edges.Add({ -X, X, Y });
		if (D + Y > 0)
		{
			--Y;
			D -= (Y * 2) - 1;
		}
		if (X > D)
		{
			++X;
			D += (X * 2) + 1;
		}
	}
	// ... removeDuplicates: of the lines on one row the last (the widest) stays ...
	TArray<FSpan>& Spans = Circles.Add(Radius);
	for (int32 i = 0; i < Edges.Num(); ++i)
	{
		if (i + 1 < Edges.Num() && Edges[i + 1].Y == Edges[i].Y)
		{
			continue;
		}
		// ... and drawCircle mirrors every row but the middle one.
		Spans.Add(Edges[i]);
		if (Edges[i].Y != 0)
		{
			Spans.Add({ Edges[i].X0, Edges[i].X1, -Edges[i].Y });
		}
	}
	return Spans;
}

template <typename FnType>
void FGenShroud::ForEachCell(const FGenSighting& S, FnType Fn)
{
	const TArray<FSpan>& Spans = GetCircle(S.CellRadius);
	for (const FSpan& Span : Spans)
	{
		const int32 Y = S.CellY + Span.Y;
		if (Y < 0 || Y >= CellsY)
		{
			continue;
		}
		const int32 X0 = FMath::Max(S.CellX + Span.X0, 0), X1 = FMath::Min(S.CellX + Span.X1, CellsX - 1);
		for (int32 X = X0; X <= X1; ++X)
		{
			Fn(Y * CellsX + X);
		}
	}
}

void FGenShroud::DoReveal(const FGenSighting& S)
{
	for (int32 Player = 0; Player < PlayerCount; ++Player)
	{
		if (S.Mask & (1u << Player))
		{
			ForEachCell(S, [this, Player](int32 Index) { AddLooker(Player, Index); });
		}
	}
}

void FGenShroud::UndoReveal(const FGenSighting& S)
{
	for (int32 Player = 0; Player < PlayerCount; ++Player)
	{
		if (S.Mask & (1u << Player))
		{
			ForEachCell(S, [this, Player](int32 Index) { RemoveLooker(Player, Index); });
		}
	}
}

void FGenShroud::QueueUndoReveal(const FGenSighting& S, uint32 Frame)
{
	Pending.Add({ S, Frame });
}

void FGenShroud::ProcessQueue(uint32 Frame)
{
	// "Keep going until the front one is in the future" (they all wait the same time, so they are in order).
	int32 Done = 0;
	while (Done < Pending.Num() && Pending[Done].Frame < Frame)
	{
		UndoReveal(Pending[Done].Sighting);
		++Done;
	}
	if (Done > 0)
	{
		Pending.RemoveAt(0, Done, EAllowShrinking::No);
	}
}

void FGenShroud::RevealMapForPlayer(int32 Player)
{
	if (Player < 0 || Player >= PlayerCount)
	{
		return;
	}
	for (int32 i = 0, Num = CellsX * CellsY; i < Num; ++i)
	{
		AddLooker(Player, i);
		RemoveLooker(Player, i);
	}
}

void FGenShroud::RevealMapForPlayerPermanently(int32 Player)
{
	if (Player < 0 || Player >= PlayerCount)
	{
		return;
	}
	for (int32 i = 0, Num = CellsX * CellsY; i < Num; ++i)
	{
		AddLooker(Player, i);
	}
}

void FGenShroud::UndoRevealMapForPlayerPermanently(int32 Player)
{
	if (Player < 0 || Player >= PlayerCount)
	{
		return;
	}
	for (int32 i = 0, Num = CellsX * CellsY; i < Num; ++i)
	{
		RemoveLooker(Player, i);
	}
}

void FGenShroud::ShroudMapForPlayer(int32 Player)
{
	if (Player < 0 || Player >= PlayerCount)
	{
		return;
	}
	for (int32 i = 0, Num = CellsX * CellsY; i < Num; ++i)
	{
		int16& C = Current[Player * CellsX * CellsY + i];
		if (C == 0)
		{
			C = 1;
			++Serials[Player];
		}
	}
}

// ---- objects looking ----------------------------------------------------------------------------

void FGenObject::InitShroud()
{
	// Object::Object: "backwards compatible, and perfectly logical default to assign".
	ShroudClearingRange = Template->ShroudClearingRange < 0.f ? Template->VisionRange : Template->ShroudClearingRange;

	const FGenDynamicShroud& D = Template->DynamicShroud;
	if (D.bValid)
	{
		// DynamicShroudClearingRangeUpdate::DynamicShroudClearingRangeUpdate
		DynShroud.StateCountDown = D.ShrinkDelay + D.ShrinkTime;
		DynShroud.ShrinkStartDeadline = DynShroud.StateCountDown - D.ShrinkDelay;
		DynShroud.GrowStartDeadline = DynShroud.StateCountDown - D.GrowDelay;
		DynShroud.SustainDeadline = DynShroud.GrowStartDeadline - D.GrowTime;
		DynShroud.DoneForeverFrame = Logic->GetFrame() + uint32(FMath::Max(DynShroud.StateCountDown, 0));
		DynShroud.NativeRange = ShroudClearingRange;
		DynShroud.CurrentRange = 0.f;
		DynShroud.State = FDynShroudState::NotStarted;
	}
}

float FGenObject::GetShroudClearingRange() const
{
	// Object::getShroudClearingRange: "the designers want constructing objects to have a reduced sight range".
	return bUnderConstruction ? Template->GetBoundingCircleRadius() : ShroudClearingRange;
}

void FGenObject::UpdateDynamicShroud(uint32 Frame)
{
	// DynamicShroudClearingRangeUpdate::update (without the grid decals)
	const FGenDynamicShroud& D = Template->DynamicShroud;
	FDynShroudState& S = DynShroud;
	if (S.State == FDynShroudState::Sleeping)
	{
		return;
	}
	if (S.StateCountDown <= 0 || Frame > S.DoneForeverFrame) { S.State = FDynShroudState::DoneForever; }
	else if (S.StateCountDown <= S.ShrinkStartDeadline) { S.State = FDynShroudState::Shrinking; }
	else if (S.StateCountDown <= S.SustainDeadline) { S.State = FDynShroudState::Sustaining; }
	else if (S.StateCountDown <= S.GrowStartDeadline) { S.State = FDynShroudState::Growing; }

	switch (S.State)
	{
	case FDynShroudState::Growing:
		S.CurrentRange += S.NativeRange / FMath::Max(1.f, float(D.GrowTime));
		if (S.CurrentRange >= S.NativeRange) { S.State = FDynShroudState::Sustaining; }
		break;
	case FDynShroudState::Sustaining:
		S.CurrentRange = S.NativeRange;
		break;
	case FDynShroudState::Shrinking:
		S.CurrentRange -= (S.NativeRange - D.FinalVision) / FMath::Max(1.f, float(D.ShrinkTime));
		break;
	case FDynShroudState::DoneForever:
		S.CurrentRange = D.FinalVision;
		break;
	default:
		break;
	}
	if (S.StateCountDown > 0)
	{
		--S.StateCountDown;
	}
	if (S.ChangeIntervalCountdown > 0)
	{
		--S.ChangeIntervalCountdown;
	}
	else
	{
		S.ChangeIntervalCountdown = S.State == FDynShroudState::Growing ? D.GrowInterval : D.ChangeInterval;
		ShroudClearingRange = S.CurrentRange; // Object::setShroudClearingRange: looked at again at the end of the frame
		if (S.State == FDynShroudState::DoneForever)
		{
			S.State = FDynShroudState::Sleeping;
		}
	}
}

void FGenObject::StartRadar(bool bDisableProof)
{
	// RadarUpgrade::upgradeImplementation: the player has one more radar, and the dish (RadarUpdate) unfolds.
	bRadarUpgraded = true;
	bRadarDisableProof = bDisableProof;
	if (Template->bHasRadarUpdate)
	{
		static const int32 Extending = GenConditions::Index(TEXT("RADAR_EXTENDING"));
		SetModelCondition(Extending, true);
		RadarExtendDoneFrame = Logic->GetFrame() + uint32(FMath::Max(Template->RadarExtendFrames, 1));
	}
}

void FGenObject::UpdateRadarDish(uint32 Frame)
{
	// RadarUpdate::update
	if (RadarExtendDoneFrame != 0 && Frame > RadarExtendDoneFrame)
	{
		static const int32 Extending = GenConditions::Index(TEXT("RADAR_EXTENDING"));
		static const int32 Upgraded = GenConditions::Index(TEXT("RADAR_UPGRADED"));
		RadarExtendDoneFrame = 0;
		SetModelCondition(Extending, false);
		SetModelCondition(Upgraded, true);
	}
}

void UGenGameLogic::InitShroud()
{
	const FGenMapData* Map = GetMap();
	const FGenGameData& GD = FGenGameData::Get();
	const FBox2f Bounds = Map->GetLargestBounds();
	Shroud.Init(Bounds.Max, GD.PartitionCellSize, Players.Num());
	RadarEvents.Reset();
	bHasLastRadarEvent = false;

	// Who sees through whose eyes: allies share what they see.
	for (FGenPlayer& P : Players)
	{
		P.AlliesMask = 0;
		P.OthersMask = 0;
		P.SpiedByMask = 0;
		P.SpyVisionEndFrame = 0;
		for (const FGenPlayer& Q : Players)
		{
			if (GetRelationship(P.Index, Q.Index) == EGenRelationship::Allies) { P.AlliesMask |= 1u << Q.Index; }
			else { P.OthersMask |= 1u << Q.Index; }
		}
		// GameLogic::startNewGame: "remove shroud for the player in MP games" - unless Multiplayer.ini
		// asks for it, a skirmish map is explored from the start, and only what moves on it is hidden.
		if (!P.bNeutral && !GD.bShroudInMultiplayer && !IsCampaign())
		{
			Shroud.RevealMapForPlayer(P.Index);
		}
	}

	// -GenNoShroud (tests): the player who played on the computer that started the match sees everything, all the time.
	bShroudOn = !Players.IsValidIndex(Match.RevealedPlayer);
	if (!bShroudOn)
	{
		Shroud.RevealMapForPlayerPermanently(Match.RevealedPlayer);
	}
}

uint32 UGenGameLogic::SpyGrantMask(const FGenObject& O) const
{
	// Player::setUnitsVisionSpied: "Being spied is now a property of the unit": the enemy's things of the kinds
	// spied on look for the spying player too. (Here it is asked of everything as it stands, so a command
	// center built after the hack is seen as well.)
	uint32 Mask = 0;
	for (const FSpyGrant& G : SpyGrants)
	{
		if (GetRelationship(G.Player, O.Owner) != EGenRelationship::Enemies)
		{
			continue;
		}
		bool bKind = G.Kinds->Num() == 0;
		for (int32 k = 0; !bKind && k < G.Kinds->Num(); ++k)
		{
			bKind = O.IsKindOf(*(*G.Kinds)[k]);
		}
		if (bKind)
		{
			Mask |= 1u << G.Player;
		}
	}
	return Mask;
}

void FGenObject::UpdateSpyVision(uint32 Frame)
{
	// SpyVisionUpdate::update, self powered: on for SelfPoweredDuration, off for SelfPoweredInterval, by turns.
	for (FSpyVision& Spy : SpyVisions)
	{
		if (Spy.bActive && Frame >= Spy.OffFrame)
		{
			Spy.bActive = false;
			Spy.OnFrame = Frame + uint32(FMath::Max(Spy.Module ? Spy.Module->SpyInterval : 0, 1));
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [spy f%u] #%u %s: off until f%u"), Frame, ID, *Template->Name, Spy.OnFrame);
		}
		else if (!Spy.bActive && Frame >= Spy.OnFrame && Spy.Module)
		{
			Spy.bActive = true;
			Spy.OffFrame = Spy.Module->SpyDuration > 0 ? Frame + uint32(Spy.Module->SpyDuration) : MAX_uint32;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [spy f%u] #%u %s: on again"), Frame, ID, *Template->Name);
		}
	}
}

void UGenGameLogic::UpdateObjectLook(FGenObject& O)
{
	// Object::look, asked again every frame instead of at every change that matters (a new cell, a
	// death, a change of owner, getting in or out of something, a building finished, a new range).
	FGenSighting Want, WantAll;
	const bool bPlays = Players.IsValidIndex(O.Owner) && !Players[O.Owner].bNeutral;
	if (bPlays && !O.bPendingDestroy && !O.bDead)
	{
		// "Don't look, 'cause you are in a tunnel, now": only those in a garrison keep looking out.
		const FGenObject* Container = O.IsContained() ? FindObject(O.ContainedBy) : nullptr;
		const bool bShutIn = Container && Container->GetTemplate()->Contain.Type != EGenContainType::Garrison;
		const float Range = O.GetShroudClearingRange();
		if (!bShutIn && Range > 0.f)
		{
			Shroud.WorldToCell(O.Position.X, O.Position.Y, Want.CellX, Want.CellY);
			Want.CellRadius = FMath::Max(1, Shroud.WorldToCellDist(Range));
			// "Other players can also be looking through our eyes."
			Want.Mask = O.IsKindOf(TEXT("REVEAL_TO_ALL")) ? MAX_uint32 : (Players[O.Owner].AlliesMask | Players[O.Owner].SpiedByMask | SpyGrantMask(O));
		}
		// What everybody else gets to see of it, however far they are (a superweapon can't be hidden).
		const float AllRange = O.Template->ShroudRevealToAllRange;
		if (!bShutIn && AllRange > 0.f && !O.bUnderConstruction)
		{
			Shroud.WorldToCell(O.Position.X, O.Position.Y, WantAll.CellX, WantAll.CellY);
			WantAll.CellRadius = FMath::Max(1, Shroud.WorldToCellDist(AllRange));
			WantAll.Mask = Players[O.Owner].OthersMask;
		}
	}
	const uint32 UnlookFrame = Frame + uint32(FGenGameData::Get().UnlookPersistFrames);
	if (Want != O.LastLook)
	{
		// Object::unlook: what it saw from the old spot lingers (UnlookPersistDuration).
		if (O.LastLook.IsValid()) { Shroud.QueueUndoReveal(O.LastLook, UnlookFrame); }
		if (Want.IsValid()) { Shroud.DoReveal(Want); }
		O.LastLook = Want;
	}
	if (WantAll != O.LastRevealAll)
	{
		if (O.LastRevealAll.IsValid()) { Shroud.QueueUndoReveal(O.LastRevealAll, UnlookFrame); }
		if (WantAll.IsValid()) { Shroud.DoReveal(WantAll); }
		O.LastRevealAll = WantAll;
	}
}

void UGenGameLogic::ForgetObjectLook(FGenObject& O)
{
	const uint32 UnlookFrame = Frame + uint32(FGenGameData::Get().UnlookPersistFrames);
	if (O.LastLook.IsValid()) { Shroud.QueueUndoReveal(O.LastLook, UnlookFrame); }
	if (O.LastRevealAll.IsValid()) { Shroud.QueueUndoReveal(O.LastRevealAll, UnlookFrame); }
	O.LastLook = FGenSighting();
	O.LastRevealAll = FGenSighting();
}

// ---- what a player can see of an object ---------------------------------------------------------

void UGenGameLogic::GetObjectCells(const FGenThingTemplate& T, const FVector3f& P, float Angle, TArray<int32, TInlineAllocator<16>>& Out) const
{
	// PartitionData::updateCellsTouched: the cells the object's footprint lies on (doSmallFill, doCircleFill, doRectFill).
	const float CS = Shroud.GetCellSize();
	const int32 NX = Shroud.GetCellsX(), NY = Shroud.GetCellsY();
	const float Major = T.GeometryMajorRadius;
	const bool bBox = !T.bGeometryIsSmall && T.IsBox();
	const float Cos = FMath::Cos(Angle), Sin = FMath::Sin(Angle);
	const float Minor = T.GeometryMinorRadius;
	float ExtX = Major, ExtY = Major;
	if (T.bGeometryIsSmall)
	{
		ExtX = ExtY = FMath::Min(Major, CS * 0.5f);
	}
	else if (bBox)
	{
		ExtX = FMath::Abs(Cos) * Major + FMath::Abs(Sin) * Minor;
		ExtY = FMath::Abs(Sin) * Major + FMath::Abs(Cos) * Minor;
	}
	int32 X0, Y0, X1, Y1;
	Shroud.WorldToCell(P.X - ExtX, P.Y - ExtY, X0, Y0);
	Shroud.WorldToCell(P.X + ExtX, P.Y + ExtY, X1, Y1);
	const float HalfOnAxes = CS * 0.5f * (FMath::Abs(Cos) + FMath::Abs(Sin)); // half a cell, measured along the box's axes
	for (int32 Y = FMath::Max(Y0, 0); Y <= FMath::Min(Y1, NY - 1); ++Y)
	{
		for (int32 X = FMath::Max(X0, 0); X <= FMath::Min(X1, NX - 1); ++X)
		{
			if (bBox)
			{
				const float DX = (float(X) + 0.5f) * CS - P.X, DY = (float(Y) + 0.5f) * CS - P.Y;
				if (FMath::Abs(DX * Cos + DY * Sin) > Major + HalfOnAxes || FMath::Abs(-DX * Sin + DY * Cos) > Minor + HalfOnAxes)
				{
					continue;
				}
			}
			else if (!T.bGeometryIsSmall)
			{
				const float NearX = FMath::Clamp(P.X, float(X) * CS, float(X + 1) * CS), NearY = FMath::Clamp(P.Y, float(Y) * CS, float(Y + 1) * CS);
				if (FMath::Square(NearX - P.X) + FMath::Square(NearY - P.Y) > Major * Major)
				{
					continue;
				}
			}
			Out.Add(Y * NX + X);
		}
	}
}

EGenObjectShroud UGenGameLogic::ComputeObjectShroud(const FGenObject& O, int32 Player, bool& bInOutEverSeen) const
{
	// Object::getShroudedStatus / PartitionData::getShroudedStatus
	if (!Shroud.IsValid() || O.IsContained() || O.IsKindOf(TEXT("ALWAYS_VISIBLE")))
	{
		return EGenObjectShroud::Clear;
	}
	TArray<int32, TInlineAllocator<16>> Cells;
	GetObjectCells(*O.Template, O.Position, O.Orientation, Cells);
	int32 NumShrouded = 0, NumFogged = 0;
	const int32 NX = Shroud.GetCellsX();
	for (int32 Index : Cells)
	{
		const EGenCellShroud S = Shroud.GetStatus(Player, Index % NX, Index / NX);
		NumShrouded += S == EGenCellShroud::Shrouded ? 1 : 0;
		NumFogged += S == EGenCellShroud::Fogged ? 1 : 0;
	}
	if (Cells.Num() == 0 || NumShrouded == Cells.Num())
	{
		bInOutEverSeen = false; // "force object as never seen by the player"
		return EGenObjectShroud::Shrouded;
	}
	if (NumShrouded + NumFogged == Cells.Num())
	{
		// In the fog: only what stays where it is can be remembered.
		if (GetRelationship(Player, O.Owner) == EGenRelationship::Neutral)
		{
			// "Anything neutral that moves around will not be rendered inside fog."
			return O.bImmobile ? EGenObjectShroud::Fogged : EGenObjectShroud::Shrouded;
		}
		// "Enemy unit will always be shrouded unless it's a building that's already been seen by the
		// player. Fogged mines are also always shrouded no matter what."
		return (O.bImmobile && bInOutEverSeen && !O.IsKindOf(TEXT("MINE"))) ? EGenObjectShroud::Fogged : EGenObjectShroud::Shrouded;
	}
	bInOutEverSeen = true;
	return (NumShrouded == 0 && NumFogged == 0) ? EGenObjectShroud::Clear : EGenObjectShroud::PartialClear;
}

EGenObjectShroud UGenGameLogic::GetLocalShroudNow(const FGenObject& O) const
{
	if (!bShroudOn)
	{
		return EGenObjectShroud::Clear;
	}
	bool bEverSeen = O.bEverSeenByLocal;
	return ComputeObjectShroud(O, LocalPlayer, bEverSeen);
}

bool UGenGameLogic::IsVisibleToLocalPlayer(const FGenObject& O) const
{
	// W3DScienceModelDraw: "only GLA players and observers can see us" (a salvage crate).
	if (const FGenDrawModule* Draw = O.GetTemplate()->PrimaryDraw())
	{
		if (!Draw->RequiredScience.IsEmpty() && !HasScience(LocalPlayer, Draw->RequiredScience))
		{
			return false;
		}
	}
	if (GetStealthLook(O) == EGenStealthLook::Invisible)
	{
		return false; // stealthed, theirs, and nobody of ours is showing it up
	}
	if (!bShroudOn || O.LocalShroud != EGenObjectShroud::Shrouded)
	{
		return true; // in view, partly in view, or a building remembered the way it was last seen
	}
	// RTS3DScene::renderOneObject: "For objects like planes, that pop out of the shroud, fire, then head
	// back, we keep drawing them for 2 seconds after they return to the fogged area" (5 if they are dying).
	if (O.ShroudClearFrame != 0)
	{
		const uint32 Limit = uint32(GenLogic::GetLogicFPS()) * (O.bDead ? 5u : 2u);
		return Frame < O.ShroudClearFrame + Limit;
	}
	return false;
}

bool UGenGameLogic::IsFrozenInFog(const FGenObject& O) const
{
	return bShroudOn && O.LocalShroud == EGenObjectShroud::Fogged;
}

bool UGenGameLogic::IsPositionClear(const FVector3f& Pos) const
{
	return !bShroudOn || Shroud.GetStatusAt(LocalPlayer, Pos.X, Pos.Y) == EGenCellShroud::Clear;
}

void UGenGameLogic::UpdateShroud()
{
	GEN_PERF_SCOPE("Shroud");
	if (!Shroud.IsValid())
	{
		return;
	}
	Shroud.ProcessQueue(Frame);

	// SpyVisionUpdate::doActivationWork: whose eyes the satellite hacks look through this time.
	SpyGrants.Reset();
	for (int32 i = SpyVisionSources.Num() - 1; i >= 0; --i)
	{
		const FGenObject* Src = FindObject(SpyVisionSources[i]);
		if (!Src || Src->IsDestroyed())
		{
			SpyVisionSources.RemoveAt(i);
			continue;
		}
		if (Src->IsDead() || Src->IsDisabled() || !Players.IsValidIndex(Src->GetOwner()) || Players[Src->GetOwner()].bNeutral)
		{
			continue; // (onDelete / onDisabledEdge: off)
		}
		for (const FGenObject::FSpyVision& Spy : Src->SpyVisions)
		{
			if (Spy.bActive && Spy.Module)
			{
				SpyGrants.Add({ Src->GetOwner(), &Spy.Module->SpyOnKindOf });
			}
		}
	}
	for (FGenPlayer& P : Players)
	{
		P.RadarCount = 0;
		P.DisableProofRadarCount = 0;
		// SpyVisionUpdate: spy vision runs out, or goes with the building it came from.
		if (P.SpyVisionEndFrame != 0)
		{
			const FGenObject* Source = FindObject(P.SpyVisionSource);
			if (Frame >= P.SpyVisionEndFrame || !Source || Source->IsDead() || Source->GetOwner() != P.Index)
			{
				P.SpyVisionEndFrame = 0;
				for (FGenPlayer& Other : Players) { Other.SpiedByMask &= ~(1u << P.Index); }
				UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] p%d spy vision ends"), Frame, P.Index);
			}
		}
	}
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject& O = *Objects[Id];
		UpdateObjectLook(O);
		// Player::addRadar / removeRadar: a radar counts while it stands and works.
		// (GrantUpgradeCreate's ExemptStatus = UNDER_CONSTRUCTION: not before the building is finished.)
		if (O.bRadarUpgraded && !O.bDead && !O.bPendingDestroy && !O.bUnderConstruction && !O.IsDisabled() && Players.IsValidIndex(O.Owner))
		{
			++Players[O.Owner].RadarCount;
			Players[O.Owner].DisableProofRadarCount += O.bRadarDisableProof ? 1 : 0;
		}
	}

	// Player::hasRadar: a power shortage switches the radar off, unless one of them doesn't need power.
	for (FGenPlayer& P : Players)
	{
		int32 Produced = 0, Consumed = 0;
		GetEnergy(P.Index, Produced, Consumed);
		const bool bBrownOut = Consumed > Produced || Frame < P.PowerSabotagedUntil; // ("if( m_powerSabotagedTillFrame > now ) return FALSE")
		P.bBrownOut = bBrownOut;
		const bool bHas = P.RadarCount > 0 && !(bBrownOut && P.DisableProofRadarCount == 0);
		if (bHas != P.bHasRadar)
		{
			P.bHasRadar = bHas;
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [radar f%u] p%d radar %s (%d radars, %d need no power, power %d / %d)"), Frame, P.Index,
				bHas ? TEXT("online") : TEXT("offline"), P.RadarCount, P.DisableProofRadarCount, Produced, Consumed);
			if (P.Index == LocalPlayer && Frame > 1)
			{
				PlayMiscSound(bHas ? TEXT("RadarNotifyOnlineSound") : TEXT("RadarNotifyOfflineSound"), P.Index);
				FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
				Text.Text = bHas ? TEXT("Radar online") : TEXT("Radar offline");
				Text.Color = bHas ? FLinearColor(0.5f, 1.f, 0.5f) : FLinearColor(1.f, 0.5f, 0.4f);
				Text.Frame = Frame;
				Text.bScreen = true;
			}
		}
	}

	// What the local player sees of everything (the client asks for this every frame for every drawable).
	int32 NumHidden = 0;
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject& O = *Objects[Id];
		if (!bShroudOn)
		{
			O.LocalShroud = EGenObjectShroud::Clear;
			continue;
		}
		O.LocalShroud = ComputeObjectShroud(O, LocalPlayer, O.bEverSeenByLocal);
		if (O.LocalShroud == EGenObjectShroud::Clear)
		{
			O.ShroudClearFrame = Frame;
		}
		NumHidden += O.LocalShroud == EGenObjectShroud::Shrouded ? 1 : 0;
	}
	if (bTrace && Frame % uint32(GenLogic::GetLogicFPS() * 5) == 0 && bShroudOn)
	{
		int32 Clear = 0, Fogged = 0;
		for (int32 Y = 0; Y < Shroud.GetCellsY(); ++Y)
		{
			for (int32 X = 0; X < Shroud.GetCellsX(); ++X)
			{
				const EGenCellShroud S = Shroud.GetStatus(LocalPlayer, X, Y);
				Clear += S == EGenCellShroud::Clear ? 1 : 0;
				Fogged += S == EGenCellShroud::Fogged ? 1 : 0;
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("  [fog f%u] p%d cells: %d clear, %d fogged, %d shrouded; objects hidden: %d of %d"), Frame, LocalPlayer,
			Clear, Fogged, Shroud.GetCellsX() * Shroud.GetCellsY() - Clear - Fogged, NumHidden, ObjectOrder.Num());
	}
}

// ---- radar --------------------------------------------------------------------------------------

bool UGenGameLogic::HasRadar(int32 Player) const
{
	// (Whoever is out of the game watches the rest of it with the radar forced on, as an observer does.)
	return Players.IsValidIndex(Player) && (Players[Player].bHasRadar || Players[Player].bDefeated);
}

void UGenGameLogic::CreateRadarEvent(const FVector3f& Pos, EGenRadarEvent Type, float SecondsToLive)
{
	// Radar::createEvent / internalCreateEvent, with the colours of radarColorLookupTable.
	static const FColor Colors[][2] = {
		{ FColor(128, 128, 255), FColor(128, 255, 255) },   // construction
		{ FColor(128, 0, 64), FColor(255, 185, 220) },      // upgrade
		{ FColor(255, 0, 0), FColor(255, 128, 128) },       // under attack
		{ FColor(255, 255, 0), FColor(255, 255, 128) },     // information
		{ FColor(0, 255, 255), FColor(128, 255, 255) },     // infiltration
		{ FColor(0, 255, 0), FColor(0, 128, 0) },           // stealth discovered
		{ FColor(0, 255, 0), FColor(0, 128, 0) },           // stealth neutralized
		{ FColor(0, 0, 0, 0), FColor(0, 0, 0, 0) },         // fake
	};
	constexpr int32 MaxRadarEvents = 64;
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	FGenRadarEvent E;
	E.Type = Type;
	E.Pos = Pos;
	E.CreateFrame = Frame;
	E.DieFrame = Frame + uint32(float(FPS) * SecondsToLive);
	E.FadeFrame = E.DieFrame - FPS / 2; // "this many seconds before we hit the die frame we start to fade away"
	E.Color1 = Colors[int32(Type)][0];
	E.Color2 = Colors[int32(Type)][1];
	if (RadarEvents.Num() >= MaxRadarEvents)
	{
		RadarEvents.RemoveAt(0); // "the oldest ones will just drop off, eh ... should be fine"
	}
	RadarEvents.Add(E);
	bHasLastRadarEvent = true;
	LastRadarEventPos = Pos;
	if (Type != EGenRadarEvent::Fake && Audio && HasRadar(LocalPlayer))
	{
		Audio->PlayUI(TEXT("RadarEvent"), LocalPlayer); // W3DRadar::drawEvents: the blip of an event appearing
	}
}

bool UGenGameLogic::TryRadarEvent(EGenRadarEvent Type, const FVector3f& Pos)
{
	// Radar::tryEvent: not if there was one of the kind lately. (The original means "nearby and lately",
	// but its distance sum lacks brackets and comes out hugely negative for any spot on a map, so in
	// effect one event of a kind every ten seconds is what the game does.)
	const uint32 FramesBetweenEvents = uint32(GenLogic::GetLogicFPS()) * 10u;
	for (const FGenRadarEvent& E : RadarEvents)
	{
		if (E.Type == Type && Frame - E.CreateFrame < FramesBetweenEvents)
		{
			return false;
		}
	}
	CreateRadarEvent(Pos, Type);
	return true;
}

void UGenGameLogic::TryUnderAttackEvent(const FGenObject& Obj)
{
	// Radar::tryUnderAttackEvent: the marker on the radar, and the message that goes with it.
	if (!TryRadarEvent(EGenRadarEvent::UnderAttack, Obj.GetPosition()))
	{
		return;
	}
	const TCHAR* Label = TEXT("RADAR:UnderAttack");
	// (The original plays its "structure under attack" sound for everything but harvesters.)
	const TCHAR* Sound = TEXT("RadarNotifyStructureUnderAttackSound");
	if (Obj.IsKindOf(TEXT("INFANTRY")) || Obj.IsKindOf(TEXT("VEHICLE")))
	{
		Label = Obj.IsKindOf(TEXT("HARVESTER")) ? TEXT("RADAR:HarvesterUnderAttack") : TEXT("RADAR:UnitUnderAttack");
		Sound = Obj.IsKindOf(TEXT("HARVESTER")) ? TEXT("RadarNotifyHarvesterUnderAttackSound") : Sound;
	}
	else if (Obj.IsKindOf(TEXT("STRUCTURE")) && Obj.IsKindOf(TEXT("MP_COUNT_FOR_VICTORY")))
	{
		Label = TEXT("RADAR:StructureUnderAttack");
		EvaEvent(TEXT("BaseUnderAttack"), Obj.GetOwner());
	}
	PlayMiscSound(Sound, Obj.GetOwner());
	FString Message = FGenCommandData::Get().GetText(Label);
	if (Message.IsEmpty() || Message.Equals(Label, ESearchCase::IgnoreCase))
	{
		Message = TEXT("We are under attack");
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [radar f%u] under attack: #%u %s at (%.0f, %.0f): %s"), Frame, Obj.GetID(), *Obj.GetTemplate()->Name,
		Obj.GetPosition().X, Obj.GetPosition().Y, *Message);
	FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
	Text.Text = Message;
	Text.Color = FLinearColor(1.f, 0.35f, 0.3f);
	Text.Frame = Frame;
	Text.bScreen = true;
}

bool UGenGameLogic::GetLastRadarEventPos(FVector3f& OutPos) const
{
	if (!bHasLastRadarEvent)
	{
		return false;
	}
	OutPos = LastRadarEventPos;
	return true;
}

bool UGenGameLogic::IsOnRadar(const FGenObject& O) const
{
	// Radar::isPriorityVisible, and W3DRadar::renderObjectList's reasons to leave an object out.
	const EGenRadarPriority Priority = O.GetTemplate()->RadarPriority;
	if (Priority == EGenRadarPriority::Invalid || Priority == EGenRadarPriority::NotOnRadar)
	{
		return false;
	}
	if (O.IsDestroyed() || O.IsHidden())
	{
		return false;
	}
	// (W3DRadar::renderObjectList: an enemy's stealthed unit only when it has been detected.)
	if (O.IsStealthedAndUndetected() && GetRelationship(LocalPlayer, O.GetOwner()) == EGenRelationship::Enemies && !IsPlayerDefeated(LocalPlayer))
	{
		return false;
	}
	if (bShroudOn && O.LocalShroud > EGenObjectShroud::PartialClear)
	{
		return false; // "object is fogged or shrouded, don't render it"
	}
	return Priority != EGenRadarPriority::LocalUnitOnly || O.GetOwner() == LocalPlayer;
}
