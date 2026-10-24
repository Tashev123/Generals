// The skirmish computer player: base building, dozers, gatherers and team assembly.
// Follows AIPlayer.cpp and AISkirmishPlayer.cpp; the teams follow Team.cpp. What it builds and when
// is decided by the original skirmish scripts (GenSkirmishScripts.cpp).

#include "Logic/GenSkirmishAI.h"
#include "Core/GenPerf.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	constexpr float AICell = GenLogic::PathfindCellSize;
	constexpr float SupplyCenterCloseDist = 20.f * AICell;   // SUPPLY_CENTER_CLOSE_DIST

	bool IsGathererType(const FGenThingTemplate& T)
	{
		return T.bIsSupplyTruck && T.IsKindOf(TEXT("HARVESTER"));
	}

	bool IsAlive(const FGenObject* O)
	{
		return O && !O->IsDead() && !O->IsDestroyed();
	}
}

FGenSkirmishAI::FGenSkirmishAI(UGenGameLogic* InLogic, int32 InPlayer, EGenDifficulty InDifficulty)
	: Logic(InLogic), Player(InPlayer), Difficulty(InDifficulty)
{
	// The default team: everything of ours that belongs to no other team.
	Teams.Add(MakeUnique<FGenTeam>());
	Teams[0]->bActive = true;
}

int32 FGenSkirmishAI::GetMoney() const
{
	return Logic->GetMoney(Player);
}

// ---- setup --------------------------------------------------------------------------------------

void FGenSkirmishAI::NewMap()
{
	// AISkirmishPlayer::newMap: take the side's base layout from AIData.ini, move it to where we start.
	const FGenAIData& AI = FGenAIData::Get();
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	Side = Players[Player].Template ? Players[Player].Template->Side : FString();
	Frame = Logic->GetFrame();

	if (const TArray<FGenAIBuildListEntry>* List = AI.BuildLists.Find(Side))
	{
		for (const FGenAIBuildListEntry& E : *List)
		{
			FGenBuildListInfo& Info = BuildList.AddDefaulted_GetRef();
			Info.Template = FGenGameData::Get().FindTemplate(E.Template);
			Info.Location = FVector3f(E.Location.X, E.Location.Y, 0.f);
			Info.Angle = E.Angle;
			Info.NumRebuilds = E.Rebuilds;
			Info.bInitiallyBuilt = E.bInitiallyBuilt;
			Info.bAutomaticBuild = E.bAutomaticallyBuild;
		}
		AdjustBuildList();
		ComputeCenterAndRadiusOfBase();
		for (int32 i = 0; i < BuildList.Num(); ++i)
		{
			if (!BuildList[i].Template)
			{
				continue;
			}
			if (BuildList[i].bInitiallyBuilt)
			{
				BuildStructureNow(BuildList[i]);
			}
			else
			{
				++BuildList[i].NumRebuilds; // the first build uses up a rebuild
			}
		}
	}
	else
	{
		UE_LOG(LogGenerals, Warning, TEXT("AI player %d: no skirmish build list for side '%s'"), Player, *Side);
	}
	InitScripts();
	UE_LOG(LogGenerals, Log, TEXT("AI player %d (%s): %d build list entries, %d teams, base center (%.0f, %.0f) radius %.0f"), Player, *Side,
		BuildList.Num(), Protos.Num(), BaseCenter.X, BaseCenter.Y, BaseRadius);
}

void FGenSkirmishAI::AdjustBuildList()
{
	// AISkirmishPlayer::adjustBuildList: the layout is given around its command center; ours replaces
	// the one the game start placed, and everything else moves (and turns) with it.
	FGenObject* Start = nullptr;
	for (FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (O->IsKindOf(TEXT("COMMANDCENTER")))
		{
			Start = O;
			break;
		}
	}
	if (!Start)
	{
		return;
	}
	const FVector3f StartPos = Start->GetPosition();
	Logic->DestroyObject(Start->GetID());

	FVector2f BuildPos = FVector2f::ZeroVector;
	for (FGenBuildListInfo& Info : BuildList)
	{
		if (Info.Template && Info.Template->IsKindOf(TEXT("COMMANDCENTER")))
		{
			BuildPos = FVector2f(Info.Location.X, Info.Location.Y);
			Info.bInitiallyBuilt = true;
		}
	}

	float Angle = 0.f;
	const FGenMapData* Map = Logic->GetMap();
	if (FGenAIData::Get().bRotateSkirmishBases && Map)
	{
		// Which ninth of the map we start in decides how the base is turned.
		const FBox2f Bounds = Map->GetPlayableBounds();
		const FVector2f Size = Bounds.GetSize();
		int32 Grid = 0;
		if (StartPos.X > Bounds.Min.X + Size.X / 3.f) { ++Grid; }
		if (StartPos.X > Bounds.Min.X + 2.f * Size.X / 3.f) { ++Grid; }
		if (StartPos.Y > Bounds.Min.Y + Size.Y / 3.f) { Grid += 3; }
		if (StartPos.Y > Bounds.Min.Y + 2.f * Size.Y / 3.f) { Grid += 3; }
		static const float Angles[9] = { 0.f, PI / 4.f, PI / 2.f, -PI / 4.f, 0.f, 3.f * PI / 4.f, -PI / 2.f, -3.f * PI / 4.f, PI };
		Angle = Angles[Grid];
	}
	Angle += 3.f * PI / 4.f;
	const float S = FMath::Sin(Angle), C = FMath::Cos(Angle);
	for (FGenBuildListInfo& Info : BuildList)
	{
		const float X = Info.Location.X - BuildPos.X, Y = Info.Location.Y - BuildPos.Y;
		Info.Location = FVector3f(X * C - Y * S + StartPos.X, Y * C + X * S + StartPos.Y, 0.f);
	}
}

void FGenSkirmishAI::ComputeCenterAndRadiusOfBase()
{
	FVector2f Total = FVector2f::ZeroVector;
	int32 Num = 0;
	for (const FGenBuildListInfo& Info : BuildList)
	{
		if (Info.Template)
		{
			Total += FVector2f(Info.Location.X, Info.Location.Y);
			++Num;
		}
	}
	if (Num > 0)
	{
		Total /= float(Num);
	}
	bBaseCenterSet = Num > 0;
	BaseCenter = FVector3f(Total.X, Total.Y, 0.f);
	float MaxRadSq = 0.f;
	for (const FGenBuildListInfo& Info : BuildList)
	{
		if (!Info.Template)
		{
			continue;
		}
		const float BldgRadius = Info.Template->GetBoundingCircleRadius() * 0.4f;
		const float DX = FMath::Abs(Info.Location.X - BaseCenter.X) + BldgRadius;
		const float DY = FMath::Abs(Info.Location.Y - BaseCenter.Y) + BldgRadius;
		MaxRadSq = FMath::Max(MaxRadSq, DX * DX + DY * DY);
	}
	BaseRadius = FMath::Sqrt(MaxRadSq);
}

// ---- per frame ----------------------------------------------------------------------------------

void FGenSkirmishAI::Update(uint32 InFrame)
{
	GEN_PERF_SCOPE("AI");
	Frame = InFrame;
	PurgeDeadMembers();

	// ScriptEngine::update
	UpdateScripts();
	Logic->SimYield(TEXT("a computer player's team states"));

	// AIPlayer::update
	DoUpgradesAndSkills();
	DoBaseBuilding();
	Logic->SimYield(TEXT("a computer player's base building"));
	CheckReadyTeams();
	CheckQueuedTeams();
	DoTeamBuilding();
	Logic->SimYield(TEXT("a computer player's team building"));

	// Player::update: teams run their generic scripts.
	UpdateGenericScripts();
}

// ---- base building ------------------------------------------------------------------------------

void FGenSkirmishAI::DoBaseBuilding()
{
	if (!bCanBuildBase)
	{
		return;
	}
	const int32 FPS = GenLogic::GetLogicFPS();
	if (!bReadyToBuildStructure)
	{
		if (--StructureTimer <= 0)
		{
			bReadyToBuildStructure = true;
			BuildDelay = 0;
		}
		StructureTimer = FMath::Min(StructureTimer, 3 * FPS);
	}
	// This delay keeps us from banging on the logic every frame; when something interesting happens
	// (a building or a unit is finished) it is cut short.
	if (--BuildDelay < 1)
	{
		if (bReadyToBuildStructure)
		{
			ProcessBaseBuilding();
		}
		if (BuildDelay < 1)
		{
			BuildDelay = 2 * FPS;
		}
	}
}

void FGenSkirmishAI::ProcessBaseBuilding()
{
	// AISkirmishPlayer::processBaseBuilding: look after buildings under construction, notice destroyed
	// ones, then start the next building that is due.
	const FGenAIData& AI = FGenAIData::Get();
	const int32 FPS = GenLogic::GetLogicFPS();
	int32 Plan = INDEX_NONE;
	int32 PowerPlan = INDEX_NONE;
	bool bIsPriority = false;
	bool bPowerUnderConstruction = false;
	int32 Produced = 0, Consumed = 0;
	Logic->GetEnergy(Player, Produced, Consumed);
	const bool bUnderPowered = Produced < Consumed;

	for (int32 i = 0; i < BuildList.Num(); ++i)
	{
		FGenBuildListInfo& Info = BuildList[i];
		if (!Info.Template)
		{
			continue;
		}
		const bool bIsPower = Info.Template->IsKindOf(TEXT("FS_POWER")) && !Info.Template->IsKindOf(TEXT("CASH_GENERATOR"));
		FGenObject* Bldg = Logic->FindObject(Info.Object);
		if (Info.Object != GenInvalidObjectID)
		{
			if (!IsAlive(Bldg) || Bldg->GetOwner() != Player)
			{
				// Destroyed (or captured): wait a while, then it may be rebuilt.
				Info.Object = GenInvalidObjectID;
				Info.ObjectTimestamp = Frame + 1;
				Info.bUnderConstruction = false;
				Bldg = nullptr;
			}
			else if (Bldg->IsUnderConstruction())
			{
				bPowerUnderConstruction |= bIsPower;
				// Make sure a dozer is working on it.
				bool bHasBuilder = false;
				for (const FGenObject* D : Logic->GetObjectsOf(Player))
				{
					if (IsAlive(D) && D->GetBuildTarget() == Bldg->GetID())
					{
						bHasBuilder = true;
						break;
					}
				}
				if (!bHasBuilder)
				{
					if (FGenObject* Dozer = FindDozer(Bldg->GetPosition()))
					{
						Logic->ResumeConstruction(*Dozer, *Bldg);
					}
				}
			}
		}
		if (Info.Object == GenInvalidObjectID && Info.ObjectTimestamp > 0)
		{
			if (Info.ObjectTimestamp + uint32(AI.RebuildDelaySeconds * FPS) > Frame)
			{
				continue;
			}
			Info.ObjectTimestamp = 0; // ready to build again
		}
		if (Bldg)
		{
			continue; // already built
		}
		if (!IsLocationSafe(Info.Location, *Info.Template))
		{
			continue;
		}
		if (Info.bPriorityBuild && !bIsPriority)
		{
			Plan = i;
			bIsPriority = true;
		}
		if (bIsPower && PowerPlan == INDEX_NONE && (bUnderPowered || Info.bAutomaticBuild))
		{
			PowerPlan = i;
		}
		if (!Info.bAutomaticBuild)
		{
			continue; // only built when a script asks for it
		}
		if (!FindDozer(Info.Location))
		{
			if (bUnderPowered)
			{
				QueueDozer();
			}
			continue;
		}
		if (Info.NumRebuilds > 0 && Plan == INDEX_NONE)
		{
			Plan = i;
		}
	}
	// Short of power: a power plant comes first.
	if (PowerPlan != INDEX_NONE && (Plan == INDEX_NONE || BuildList[PowerPlan].Template != BuildList[Plan].Template) && !bPowerUnderConstruction)
	{
		Plan = PowerPlan;
	}
	if (Plan == INDEX_NONE)
	{
		return;
	}
	FGenBuildListInfo& Info = BuildList[Plan];
	if (BuildStructureWithDozer(Info))
	{
		Info.NumRebuilds = FMath::Max(0, Info.NumRebuilds - 1);
		bReadyToBuildStructure = false;
		float Timer = AI.StructureSeconds * float(FPS);
		if (GetMoney() < AI.Poor) { Timer /= FMath::Max(AI.StructuresPoorRate, 0.01f); }
		else if (GetMoney() > AI.Wealthy) { Timer /= FMath::Max(AI.StructuresWealthyRate, 0.01f); }
		StructureTimer = FMath::RoundToInt32(Timer);
	}
}

FGenObject* FGenSkirmishAI::BuildStructureNow(FGenBuildListInfo& Info)
{
	FVector3f Pos = Info.Location;
	Pos.Z = Logic->GetGroundHeight(Pos.X, Pos.Y);
	FGenObject* Bldg = Logic->CreateObject(Info.Template, Player, Pos, Info.Angle);
	if (Bldg)
	{
		Info.Object = Bldg->GetID();
		Info.ObjectTimestamp = Frame + 1;
		CheckForSupplyCenter(Info, *Bldg);
	}
	return Bldg;
}

bool FGenSkirmishAI::FindPlaceToBuild(const FGenThingTemplate& Thing, float Angle, float Limit, FVector3f& InOutPos) const
{
	// AIPlayer::buildStructureWithDozer "wiggle it a little": try the edges of growing squares around the spot.
	const FVector3f Center = InOutPos;
	auto Try = [this, &Thing, Angle, &InOutPos](float X, float Y)
	{
		const FVector3f P(X, Y, Logic->GetGroundHeight(X, Y));
		if (Logic->IsLocationLegalToBuild(Thing, P, Angle))
		{
			InOutPos = P;
			return true;
		}
		return false;
	};
	for (float PosOffset = 4.f * AICell; PosOffset < Limit; PosOffset += 4.f * AICell)
	{
		const float Offset = PosOffset * 0.5f;
		for (float X = Center.X - Offset; X <= Center.X + Offset; X += 2.f * AICell)
		{
			if (Try(X, Center.Y - Offset) || Try(X, Center.Y + Offset)) { return true; }
		}
		for (float Y = Center.Y - Offset; Y <= Center.Y + Offset; Y += 2.f * AICell)
		{
			if (Try(Center.X - Offset, Y) || Try(Center.X + Offset, Y)) { return true; }
		}
	}
	return false;
}

FGenObject* FGenSkirmishAI::BuildStructureWithDozer(FGenBuildListInfo& Info)
{
	FGenObject* Dozer = FindDozer(Info.Location);
	if (!Dozer || GetMoney() < Logic->CalcCostToBuild(Player, *Info.Template))
	{
		return nullptr;
	}
	FVector3f Pos = Info.Location;
	Pos.Z = Logic->GetGroundHeight(Pos.X, Pos.Y);
	if (!Logic->IsLocationLegalToBuild(*Info.Template, Pos, Info.Angle) && !FindPlaceToBuild(*Info.Template, Info.Angle, 120.f * AICell, Pos))
	{
		return nullptr;
	}
	FGenObject* Bldg = Logic->ConstructBuilding(Dozer, *Info.Template, Pos, Info.Angle, Player);
	if (Bldg)
	{
		Info.Object = Bldg->GetID();
		Info.ObjectTimestamp = Frame + 1;
		Info.bUnderConstruction = true;
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d starts %s at (%.0f, %.0f) with dozer #%u, money %d"), Frame, Player,
			*Info.Template->Name, Pos.X, Pos.Y, Dozer->GetID(), GetMoney());
	}
	return Bldg;
}

void FGenSkirmishAI::CheckForSupplyCenter(FGenBuildListInfo& Info, const FGenObject& Building)
{
	if (!Building.GetTemplate()->bSupplyCenter)
	{
		return;
	}
	const FGenAISideInfo* SideInfo = FGenAIData::Get().Sides.Find(Side);
	Info.bSupplyBuilding = true;
	Info.CurrentGatherers = -1;
	Info.DesiredGatherers = (SideInfo ? SideInfo->Gatherers[int32(Difficulty)] : 2) + 1; // one comes free with the building
}

void FGenSkirmishAI::OnStructureProduced(FGenObject& Building)
{
	TeamDelay = 0; // update the queues and pick the next things at once
	BuildDelay = 0;
	for (FGenBuildListInfo& Info : BuildList)
	{
		if (Info.Object == Building.GetID())
		{
			Info.bUnderConstruction = false;
			CheckForSupplyCenter(Info, Building);
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d finished %s"), Frame, Player, *Building.GetTemplate()->Name);
			return;
		}
	}
}

void FGenSkirmishAI::AddToPriorityBuildList(const FGenThingTemplate& Thing, const FVector3f& Pos, float Angle)
{
	FGenBuildListInfo Info;
	Info.Template = &Thing;
	Info.Location = FVector3f(Pos.X, Pos.Y, 0.f);
	Info.Angle = Angle;
	Info.bPriorityBuild = true;
	Info.NumRebuilds = 1;
	BuildList.Insert(Info, 0);
}

void FGenSkirmishAI::BuildSpecificBuilding(const FString& ThingName)
{
	// AISkirmishPlayer::buildSpecificAIBuilding: the next unbuilt entry of that type becomes a priority.
	for (FGenBuildListInfo& Info : BuildList)
	{
		if (!Info.Template || !Info.Template->Name.Equals(ThingName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (IsAlive(Logic->FindObject(Info.Object)) || Info.bPriorityBuild)
		{
			continue;
		}
		Info.bPriorityBuild = true;
		BuildDelay = 0;
		return;
	}
}

bool FGenSkirmishAI::IsLocationSafe(const FVector3f& Pos, const FGenThingTemplate& Thing) const
{
	// AIPlayer::isLocationSafe: no enemies (other than harvesters and dozers) near the spot.
	const float Radius = FGenAIData::Get().SupplyCenterSafeRadius + Thing.GetBoundingCircleRadius();
	// (Only what stands near the spot is looked at: the building list asks this for every building on it.)
	bool bSafe = true;
	Logic->GetPartition().ForEachInRange(FVector2f(Pos.X, Pos.Y), Radius, [&](FGenObject& Near)
	{
		const FGenObject* O = &Near;
		if (!bSafe || !IsAlive(O) || O->IsProjectile() || Logic->GetRelationship(Player, O->GetOwner()) != EGenRelationship::Enemies
			|| O->IsKindOf(TEXT("HARVESTER")) || O->IsKindOf(TEXT("DOZER")) || !O->IsAttackable())
		{
			return;
		}
		const float Reach = Radius + O->GetTemplate()->GetBoundingCircleRadius();
		if (FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(Pos.X, Pos.Y)) <= Reach * Reach)
		{
			bSafe = false;
		}
	});
	return bSafe;
}

FGenObject* FGenSkirmishAI::FindSupplyCenter(int32 MinimumCash)
{
	// AIPlayer::findSupplyCenter: the closest supply pile worth at least that much that we have no
	// supply center at yet, and that isn't clearly on the enemy's side of the map.
	FVector2f EnemyCenter = FVector2f::ZeroVector;
	const int32 Enemy = GetEnemy();
	if (Enemy != INDEX_NONE)
	{
		FBox2f Bounds;
		GetPlayerStructureBounds(Enemy, Bounds);
		EnemyCenter = Bounds.GetCenter();
	}
	const int32 BoxValue = FGenGameData::Get().ValuePerSupplyBox;
	do
	{
		FGenObject* Best = nullptr;
		float BestDistSq = 0.f;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			FGenObject* O = Logic->FindObject(Id);
			if (!IsAlive(O) || !O->GetTemplate()->bSupplyWarehouse || !O->IsKindOf(TEXT("STRUCTURE")) || !O->IsKindOf(TEXT("SUPPLY_SOURCE")))
			{
				continue;
			}
			if (O->GetBoxes() * BoxValue < MinimumCash || Logic->GetRelationship(Player, O->GetOwner()) == EGenRelationship::Enemies)
			{
				continue;
			}
			// Do we have a supply center next to it already?
			const FVector2f Pos(O->GetPosition().X, O->GetPosition().Y);
			const float Radius = SupplyCenterCloseDist + O->GetTemplate()->GetBoundingCircleRadius();
			bool bHaveCenter = false;
			for (FGenObjectID OtherId : Logic->GetObjectIDs())
			{
				const FGenObject* C = Logic->FindObject(OtherId);
				if (IsAlive(C) && C->GetOwner() == Player && C->IsKindOf(TEXT("CASH_GENERATOR")))
				{
					const float Reach = Radius + C->GetTemplate()->GetBoundingCircleRadius();
					if (FVector2f::DistSquared(Pos, FVector2f(C->GetPosition().X, C->GetPosition().Y)) <= Reach * Reach)
					{
						bHaveCenter = true;
						break;
					}
				}
			}
			if (bHaveCenter)
			{
				continue;
			}
			const float DistSq = FVector2f::DistSquared(Pos, FVector2f(BaseCenter.X, BaseCenter.Y));
			if (Enemy != INDEX_NONE && DistSq * 0.4f > FVector2f::DistSquared(Pos, EnemyCenter) * 0.6f)
			{
				continue; // much closer to the enemy than to us
			}
			if (!Best || DistSq < BestDistSq)
			{
				Best = O;
				BestDistSq = DistSq;
			}
		}
		if (Best)
		{
			return Best;
		}
		MinimumCash /= 2;
	} while (MinimumCash > 100);
	return nullptr;
}

bool FGenSkirmishAI::IsSupplySourceSafe(int32 MinSupplies)
{
	const FGenObject* Warehouse = FindSupplyCenter(MinSupplies);
	return !Warehouse || IsLocationSafe(Warehouse->GetPosition(), *Warehouse->GetTemplate());
}

void FGenSkirmishAI::BuildBySupplies(int32 MinimumCash, const FString& ThingName)
{
	// AIPlayer::buildBySupplies: a supply center goes on the base side of the pile, anything else
	// (defenses) between the pile last built at and the enemy.
	FGenObject* Warehouse = FindSupplyCenter(MinimumCash);
	const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(ThingName);
	if (!Thing)
	{
		return;
	}
	const bool bCashGenerator = Thing->IsKindOf(TEXT("CASH_GENERATOR"));
	if (!bCashGenerator)
	{
		if (FGenObject* Cur = Logic->FindObject(CurWarehouse))
		{
			Warehouse = Cur;
		}
	}
	if (!Warehouse)
	{
		return;
	}
	FVector3f Location = Warehouse->GetPosition();
	FVector2f Offset = FVector2f(Location.X - BaseCenter.X, Location.Y - BaseCenter.Y).GetSafeNormal();
	float Radius = 3.f * AICell;
	if (!bCashGenerator)
	{
		const int32 Enemy = GetEnemy();
		if (Enemy != INDEX_NONE)
		{
			FBox2f Bounds;
			GetPlayerStructureBounds(Enemy, Bounds);
			Offset = (FVector2f(Location.X, Location.Y) - Bounds.GetCenter()).GetSafeNormal();
		}
		Radius = Warehouse->GetTemplate()->GetBoundingCircleRadius();
	}
	Location.X -= Offset.X * Radius;
	Location.Y -= Offset.Y * Radius;
	Location.Z = Logic->GetGroundHeight(Location.X, Location.Y);
	const float Angle = Thing->PlacementViewAngle;
	if (!Logic->IsLocationLegalToBuild(*Thing, Location, Angle))
	{
		FindPlaceToBuild(*Thing, Angle, 2.f * SupplyCenterCloseDist, Location);
	}
	AddToPriorityBuildList(*Thing, Location, Angle);
	CurWarehouse = Warehouse->GetID();
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d wants %s by the supplies at (%.0f, %.0f)"), Frame, Player, *Thing->Name, Location.X, Location.Y);
}

void FGenSkirmishAI::BuildSpecificBuildingNearestTeam(const FString& ThingName, const FGenTeam& Team)
{
	const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(ThingName);
	FVector2f Center;
	if (!Thing || !GetTeamCenter(Team, Center))
	{
		return;
	}
	FVector3f Location(Center.X, Center.Y, Logic->GetGroundHeight(Center.X, Center.Y));
	const float Angle = Thing->PlacementViewAngle;
	if (Logic->IsLocationLegalToBuild(*Thing, Location, Angle) || FindPlaceToBuild(*Thing, Angle, 2.f * SupplyCenterCloseDist, Location))
	{
		AddToPriorityBuildList(*Thing, Location, Angle);
	}
}

void FGenSkirmishAI::BuildBaseDefenseStructure(const FString& ThingName, bool bFlank)
{
	// AISkirmishPlayer::buildAIBaseDefenseStructure: defenses go on a circle around the base, starting
	// where an approach path (center, or flank / back door alternating) comes in and spreading out
	// left and right of that point.
	const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(ThingName);
	const FGenMapData* Map = Logic->GetMap();
	if (!Thing || !Map)
	{
		return;
	}
	const int32 StartNdx = Logic->GetPlayers()[Player].StartPosition + 1;
	for (int32 Attempt = 0; Attempt < 64; ++Attempt)
	{
		const FString Label = FString::Printf(TEXT("%s%d"), bFlank ? ((CurFlankBaseDefense & 1) ? TEXT("Flank") : TEXT("Backdoor")) : TEXT("Center"), StartNdx);
		FVector2f GoalPos(BaseCenter.X, BaseCenter.Y);
		if (const FGenMapObject* Way = Map->GetClosestWaypointOnPath(GoalPos, Label))
		{
			GoalPos = FVector2f(Way->Location.X, Way->Location.Y);
		}
		else
		{
			if (bFlank)
			{
				return;
			}
			const int32 Enemy = GetEnemy();
			if (Enemy == INDEX_NONE)
			{
				return;
			}
			FBox2f Bounds;
			GetPlayerStructureBounds(Enemy, Bounds);
			GoalPos = Bounds.GetCenter();
		}
		const float DefenseDistance = BaseRadius + FGenAIData::Get().SkirmishBaseDefenseExtraDistance;
		const FVector2f Offset = (GoalPos - FVector2f(BaseCenter.X, BaseCenter.Y)).GetSafeNormal() * DefenseDistance;
		const float AngleOffset = 2.f * PI * (Thing->GetBoundingCircleRadius() * 4.f / (2.f * PI * DefenseDistance));

		float Angle = 0.f;
		if (bFlank)
		{
			const int32 Selector = CurFlankBaseDefense >> 1;
			float& Left = (CurFlankBaseDefense & 1) ? CurLeftFlankLeftDefenseAngle : CurRightFlankLeftDefenseAngle;
			float& Right = (CurFlankBaseDefense & 1) ? CurLeftFlankRightDefenseAngle : CurRightFlankRightDefenseAngle;
			if (Selector & 1) { Right -= AngleOffset; Angle = Right; }
			else { Angle = Left; Left += AngleOffset; }
			++CurFlankBaseDefense;
		}
		else
		{
			if (CurFrontBaseDefense & 1) { CurFrontRightDefenseAngle -= AngleOffset; Angle = CurFrontRightDefenseAngle; }
			else { Angle = CurFrontLeftDefenseAngle; CurFrontLeftDefenseAngle += AngleOffset; }
			++CurFrontBaseDefense;
		}
		if (Angle > PI / 3.f)
		{
			return;
		}
		const float S = FMath::Sin(Angle), C = FMath::Cos(Angle);
		FVector3f BuildPos(BaseCenter.X + Offset.X * C - Offset.Y * S, BaseCenter.Y + Offset.Y * C + Offset.X * S, 0.f);
		BuildPos.Z = Logic->GetGroundHeight(BuildPos.X, BuildPos.Y);
		if (Logic->IsLocationLegalToBuild(*Thing, BuildPos, Thing->PlacementViewAngle))
		{
			AddToPriorityBuildList(*Thing, BuildPos, Thing->PlacementViewAngle);
			return;
		}
	}
}

// ---- dozers and factories -----------------------------------------------------------------------

FGenObject* FGenSkirmishAI::FindDozer(const FVector3f& Pos)
{
	// AIPlayer::findDozer: the closest dozer that isn't building or gathering.
	FGenObject* Closest = nullptr;
	float ClosestDistSq = 0.f;
	bool bNeedDozer = true;
	for (FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (!IsAlive(O) || !O->GetTemplate()->bIsDozer)
		{
			continue;
		}
		const bool bBuilding = O->HasDozerTask(); // (isAnyTaskPending: building, or mending something)
		if (!bBuilding && O->GetTemplate()->bIsSupplyTruck)
		{
			// Workers gathering supplies are left alone.
			const EGenSupplyState State = O->GetSupplyState();
			if (State == EGenSupplyState::Wanting || State == EGenSupplyState::Docking || O->bForceWanting)
			{
				continue;
			}
		}
		bNeedDozer = false; // one exists, though it may be busy
		if (bBuilding)
		{
			continue;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(Pos.X, Pos.Y), FVector2f(O->GetPosition().X, O->GetPosition().Y));
		if (!Closest || DistSq < ClosestDistSq)
		{
			Closest = O;
			ClosestDistSq = DistSq;
		}
	}
	if (bNeedDozer)
	{
		QueueDozer();
	}
	return Closest;
}

bool FGenSkirmishAI::DozerInQueue() const
{
	for (const FGenTeamInQueue& TQ : TeamBuildQueue)
	{
		for (const FGenWorkOrder& Order : TQ.Orders)
		{
			if (Order.Thing->bIsDozer && !Order.bResourceGatherer)
			{
				return true;
			}
		}
	}
	return false;
}

const FGenThingTemplate* FGenSkirmishAI::FindBuildableUnit(TFunctionRef<bool(const FGenThingTemplate&)> Wanted, bool bBusyOK)
{
	// The original walks every template in the game and asks findFactory; the units our factories'
	// command sets offer are the same candidates, found much faster.
	const FGenCommandData& CD = FGenCommandData::Get();
	for (const FGenBuildListInfo& Info : BuildList)
	{
		const FGenObject* Factory = Logic->FindObject(Info.Object);
		if (!IsAlive(Factory) || Factory->GetOwner() != Player || !Factory->GetTemplate()->bHasProduction)
		{
			continue;
		}
		const FGenCommandSet* Set = CD.FindSet(Factory->GetCommandSet());
		for (int32 s = 0; Set && s < FGenCommandSet::NumSlots; ++s)
		{
			const FGenCommandButton* Button = CD.FindButton(Set->Slots[s]);
			const FGenThingTemplate* Unit = Button && Button->Command == EGenCommand::UnitBuild ? FGenGameData::Get().FindTemplate(Button->Object) : nullptr;
			if (Unit && Wanted(*Unit) && FindFactory(*Unit, bBusyOK))
			{
				return Unit;
			}
		}
	}
	return nullptr;
}

void FGenSkirmishAI::QueueDozer()
{
	if (DozerInQueue())
	{
		return;
	}
	const FGenThingTemplate* Dozer = FindBuildableUnit([](const FGenThingTemplate& T) { return T.bIsDozer; }, true);
	if (!Dozer)
	{
		return;
	}
	FGenTeamInQueue TQ;
	TQ.bPriorityBuild = true;
	TQ.FrameStarted = Frame;
	TQ.Team = 0;
	FGenWorkOrder& Order = TQ.Orders.AddDefaulted_GetRef();
	Order.Thing = Dozer;
	Order.bRequired = true;
	TeamBuildQueue.Insert(TQ, 0);
	TeamDelay = 0;
	StartTraining(TeamBuildQueue[0].Orders[0], true);
}

FGenObject* FGenSkirmishAI::FindFactory(const FGenThingTemplate& Thing, bool bBusyOK)
{
	// AIPlayer::findFactory: one of our finished buildings that can make it, an idle one if possible.
	FGenObject* Busy = nullptr;
	for (FGenBuildListInfo& Info : BuildList)
	{
		FGenObject* Factory = Logic->FindObject(Info.Object);
		if (!IsAlive(Factory) || Factory->GetOwner() != Player || Factory->IsUnderConstruction() || Factory->IsBeingSold()
			|| !Factory->GetTemplate()->bHasProduction || !Logic->CanFactoryMake(*Factory, Thing))
		{
			continue;
		}
		if (Factory->GetProductionQueue().Num() == 0)
		{
			return Factory;
		}
		if (bBusyOK)
		{
			Busy = Factory;
		}
	}
	return Busy;
}

bool FGenSkirmishAI::StartTraining(FGenWorkOrder& Order, bool bBusyOK)
{
	FGenObject* Factory = FindFactory(*Order.Thing, bBusyOK);
	if (Factory && Logic->QueueUnit(*Factory, *Order.Thing))
	{
		Order.Factory = Factory->GetID();
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d queues %s at #%u %s"), Frame, Player, *Order.Thing->Name, Factory->GetID(),
			*Factory->GetTemplate()->Name);
		return true;
	}
	return false;
}

// ---- gatherers ----------------------------------------------------------------------------------

int32 FGenSkirmishAI::CountGatherers(FGenObjectID Center, bool bRedock)
{
	int32 Count = 0;
	for (FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (!IsAlive(O) || !IsGathererType(*O->GetTemplate()))
		{
			continue;
		}
		if (Center == GenInvalidObjectID)
		{
			++Count;
		}
		else if (O->PreferredDock == Center)
		{
			++Count;
			if (bRedock && O->GetSupplyState() == EGenSupplyState::Idle && O->IsAIIdle())
			{
				O->bForceWanting = true; // back to work
			}
		}
	}
	return Count;
}

void FGenSkirmishAI::QueueSupplyTruck()
{
	// AIPlayer::queueSupplyTruck: every supply center of ours with supplies near it gets its share
	// of gatherers, one being built at a time.
	for (const FGenTeamInQueue& TQ : TeamBuildQueue)
	{
		for (const FGenWorkOrder& Order : TQ.Orders)
		{
			if (Order.bResourceGatherer)
			{
				return;
			}
		}
	}
	const int32 TotalHarvesters = CountGatherers(GenInvalidObjectID, false);
	for (FGenBuildListInfo& Info : BuildList)
	{
		if (!Info.bSupplyBuilding)
		{
			continue;
		}
		FGenObject* Center = Logic->FindObject(Info.Object);
		if (!IsAlive(Center) || Center->IsUnderConstruction())
		{
			continue;
		}
		if (Info.CurrentGatherers >= Info.DesiredGatherers)
		{
			// Still supplies near it? Then recount, in case gatherers died.
			const float Radius = SupplyCenterCloseDist + Center->GetTemplate()->GetBoundingCircleRadius();
			bool bSupplies = false;
			for (FGenObjectID Id : Logic->GetObjectIDs())
			{
				const FGenObject* S = Logic->FindObject(Id);
				if (IsAlive(S) && S->GetTemplate()->bSupplyWarehouse && S->GetBoxes() > 0
					&& Logic->GetRelationship(Player, S->GetOwner()) != EGenRelationship::Enemies)
				{
					const float Reach = Radius + S->GetTemplate()->GetBoundingCircleRadius();
					if (FVector2f::DistSquared(FVector2f(S->GetPosition().X, S->GetPosition().Y), FVector2f(Center->GetPosition().X, Center->GetPosition().Y)) <= Reach * Reach)
					{
						bSupplies = true;
						break;
					}
				}
			}
			if (bSupplies)
			{
				Info.CurrentGatherers = CountGatherers(Center->GetID(), true);
			}
			continue;
		}
		// Gatherers that lost their supply center join this one.
		bool bAdopted = false;
		for (FGenObject* O : Logic->GetObjectsOf(Player))
		{
			if (IsAlive(O) && IsGathererType(*O->GetTemplate()) && !IsAlive(Logic->FindObject(O->PreferredDock))
				&& (O->GetSupplyState() == EGenSupplyState::Wanting || O->GetSupplyState() == EGenSupplyState::Docking
					|| O->GetSupplyState() == EGenSupplyState::Regrouping || O->bForceWanting))
			{
				O->PreferredDock = Center->GetID();
				Info.CurrentGatherers = FMath::Max(0, Info.CurrentGatherers) + 1;
				bAdopted = true;
				break;
			}
		}
		if (bAdopted)
		{
			return;
		}
		if (TotalHarvesters >= Info.DesiredGatherers * 3)
		{
			continue; // we've got plenty of gatherers
		}
		if (Info.CurrentGatherers == -1)
		{
			// The first one comes with the building; it reports in when it appears (OnUnitProduced).
			Info.CurrentGatherers = 0;
			continue;
		}
		const FGenThingTemplate* Truck = FindBuildableUnit([](const FGenThingTemplate& T) { return IsGathererType(T); }, false);
		if (!Truck)
		{
			continue;
		}
		FGenTeamInQueue TQ;
		TQ.bPriorityBuild = true;
		TQ.FrameStarted = Frame;
		TQ.Team = 0;
		FGenWorkOrder& Order = TQ.Orders.AddDefaulted_GetRef();
		Order.Thing = Truck;
		Order.bRequired = true;
		Order.bResourceGatherer = true;
		TeamBuildQueue.Insert(TQ, 0);
		TeamDelay = 0;
		StartTraining(TeamBuildQueue[0].Orders[0], true);
		return;
	}
}

// ---- production callbacks -----------------------------------------------------------------------

void FGenSkirmishAI::OnUnitProduced(const FGenObject& Factory, FGenObject& Unit)
{
	// AIPlayer::onUnitProduced: the unit fills the work order it was built for and joins that team.
	bool bFound = false;
	bool bSupplyTruck = false;
	const bool bIsGatherer = IsGathererType(*Unit.GetTemplate());
	for (FGenTeamInQueue& TQ : TeamBuildQueue)
	{
		for (FGenWorkOrder& Order : TQ.Orders)
		{
			if (Order.Factory != Factory.GetID() || Order.NumCompleted >= Order.NumRequired || Order.Thing->Canonical() != Unit.GetTemplate()->Canonical())
			{
				continue;
			}
			++Order.NumCompleted;
			Order.Factory = GenInvalidObjectID;
			SetObjectTeam(Unit.GetID(), TQ.Team);
			if (TQ.bReinforcement)
			{
				TQ.ReinforcementID = Unit.GetID();
			}
			if (bIsGatherer)
			{
				bSupplyTruck = Order.bResourceGatherer;
				Unit.bForceWanting = bSupplyTruck;
			}
			bFound = true;
			break;
		}
		if (bFound)
		{
			break;
		}
	}
	// A gatherer that came with a supply center (or was built to gather) is assigned to a supply center that is short.
	const bool bFreeGatherer = !bFound && bIsGatherer && Factory.GetTemplate()->bSupplyCenter;
	if (bSupplyTruck || bFreeGatherer)
	{
		Unit.bForceWanting = true;
		for (FGenBuildListInfo& Info : BuildList)
		{
			if (!Info.bSupplyBuilding || !IsAlive(Logic->FindObject(Info.Object)))
			{
				continue;
			}
			if ((bFreeGatherer && Info.Object == Factory.GetID()) || (!bFreeGatherer && Info.DesiredGatherers > FMath::Max(0, Info.CurrentGatherers)))
			{
				Info.CurrentGatherers = FMath::Max(0, Info.CurrentGatherers) + 1;
				Unit.PreferredDock = Info.Object;
				break;
			}
		}
	}
	if (!bSupplyTruck && !bFreeGatherer && Unit.GetTemplate()->bIsDozer)
	{
		BuildDelay = 0;
		StructureTimer = 1;
	}
	TeamDelay = 0;
}

// ---- team building ------------------------------------------------------------------------------

void FGenSkirmishAI::DoTeamBuilding()
{
	if (!bCanBuildUnits)
	{
		return;
	}
	const int32 FPS = GenLogic::GetLogicFPS();
	if (!bReadyToBuildTeam)
	{
		if (--TeamTimer <= 0)
		{
			bReadyToBuildTeam = true;
			TeamDelay = 0;
		}
		TeamTimer = FMath::Min(TeamTimer, 3 * FPS);
	}
	if (--TeamDelay < 1)
	{
		QueueUnits();
		if (bReadyToBuildTeam && SelectTeamToBuild())
		{
			QueueUnits();
		}
		TeamDelay = 2 * FPS;
	}
}

void FGenSkirmishAI::QueueUnits()
{
	QueueSupplyTruck();
	const FVector3f Home = BaseCenter;
	for (FGenTeamInQueue& TQ : TeamBuildQueue)
	{
		FGenTeam* Team = FindTeam(TQ.Team);
		for (FGenWorkOrder& Order : TQ.Orders)
		{
			// A unit of that kind standing around (in a recruitable team) is taken before one is built.
			while (Team && Team->Id != 0 && Order.IsWaitingToBuild())
			{
				FGenObject* Unit = TryToRecruit(*Team, *Order.Thing, Home, FGenAIData::Get().MaxRecruitRadius);
				if (!Unit)
				{
					break;
				}
				++Order.NumCompleted;
				SetObjectTeam(Unit->GetID(), Team->Id);
				Unit->ClearAIMode();
				if (!Unit->IsParked())
				{
					Unit->CommandMoveTo(FVector3f(Home.X, Home.Y, Logic->GetGroundHeight(Home.X, Home.Y)));
				}
			}
			if (Order.IsWaitingToBuild())
			{
				StartTraining(Order, TQ.bPriorityBuild);
			}
			else if (Order.Factory != GenInvalidObjectID)
			{
				// WorkOrder::validateFactory: the factory may have been lost, or the order cancelled with it.
				const FGenObject* Factory = Logic->FindObject(Order.Factory);
				if (!IsAlive(Factory) || Factory->GetOwner() != Player || Factory->GetProductionQueue().Num() == 0)
				{
					Order.Factory = GenInvalidObjectID;
				}
			}
		}
	}
}

bool FGenSkirmishAI::IsPossibleToBuildTeam(const FGenTeamProto& Proto, bool bRequireIdleFactory, bool& bOutNotEnoughMoney)
{
	bool bAnyIdle = false;
	float Cost = 0.f;
	bOutNotEnoughMoney = false;
	for (const FGenTeamProto::FUnits& U : Proto.Units)
	{
		const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(U.Unit);
		if (!Thing)
		{
			continue;
		}
		if (!FindFactory(*Thing, true))
		{
			return false;
		}
		bAnyIdle |= FindFactory(*Thing, false) != nullptr;
		Cost += Thing->BuildCost * (float(U.Max + U.Min) / 2.f);
	}
	Cost *= FGenAIData::Get().TeamResourcesToStart;
	if (float(GetMoney()) < Cost)
	{
		bOutNotEnoughMoney = true;
		return false;
	}
	return bAnyIdle || !bRequireIdleFactory;
}

bool FGenSkirmishAI::IsAGoodIdeaToBuildTeam(int32 ProtoIndex)
{
	FGenTeamProto& Proto = Protos[ProtoIndex];
	if (Proto.Units.Num() == 0 || !EvaluateProductionCondition(Proto) || CountTeamInstances(ProtoIndex) >= Proto.MaxInstances)
	{
		return false;
	}
	for (const FGenTeamInQueue& TQ : TeamBuildQueue)
	{
		const FGenTeam* Team = FindTeam(TQ.Team);
		if (Team && Team->Proto == ProtoIndex)
		{
			return false; // one of these is being built already
		}
	}
	bool bNeedMoney = false;
	return IsPossibleToBuildTeam(Proto, true, bNeedMoney);
}

bool FGenSkirmishAI::SelectTeamToReinforce(int32 MinPriority)
{
	// AIPlayer::selectTeamToReinforce: an auto-reinforce team short of a unit, if it outranks the candidates.
	FGenTeam* CurTeam = nullptr;
	const FGenThingTemplate* CurThing = nullptr;
	int32 CurPriority = MinPriority;
	for (const TUniquePtr<FGenTeam>& TeamPtr : Teams)
	{
		FGenTeam& Team = *TeamPtr;
		if (!Protos.IsValidIndex(Team.Proto) || Team.Id == 0)
		{
			continue;
		}
		const FGenTeamProto& Proto = Protos[Team.Proto];
		if (!Proto.bAutoReinforce || Proto.ProductionPriority <= CurPriority || !TeamHasUnits(Team))
		{
			continue;
		}
		bool bBusy = false;
		for (const FGenTeamInQueue& TQ : TeamBuildQueue)
		{
			const FGenTeam* Q = FindTeam(TQ.Team);
			bBusy |= Q && Q->Proto == Team.Proto;
		}
		if (bBusy)
		{
			continue;
		}
		for (const FGenTeamProto::FUnits& U : Proto.Units)
		{
			const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(U.Unit);
			if (!Thing || U.Max < 1)
			{
				continue;
			}
			int32 Count = 0;
			ForEachMember(Team, [&Count, Thing](FGenObject& O) { Count += O.GetTemplate()->Canonical() == Thing->Canonical() ? 1 : 0; });
			if (Count < U.Max && FindFactory(*Thing, false))
			{
				CurTeam = &Team;
				CurPriority = Proto.ProductionPriority;
				CurThing = Thing;
			}
		}
	}
	if (!CurTeam || !CurThing)
	{
		return false;
	}
	FGenTeamInQueue TQ;
	TQ.bReinforcement = true;
	TQ.FrameStarted = Frame;
	TQ.Team = CurTeam->Id;
	FGenWorkOrder& Order = TQ.Orders.AddDefaulted_GetRef();
	Order.Thing = CurThing;
	Order.bRequired = true;
	TeamBuildQueue.Insert(TQ, 0);
	StartTraining(TeamBuildQueue[0].Orders[0], false);
	TeamDelay = 0;
	return true;
}

bool FGenSkirmishAI::SelectTeamToBuild()
{
	// AIPlayer::selectTeamToBuild: of the teams that may be built now, one of those with the highest priority.
	constexpr int32 InvalidPri = -99999;
	int32 HiPri = InvalidPri;
	TArray<int32, TInlineAllocator<32>> Candidates;
	for (int32 i = 0; i < Protos.Num(); ++i)
	{
		if (IsAGoodIdeaToBuildTeam(i))
		{
			Candidates.Add(i);
			HiPri = FMath::Max(HiPri, Protos[i].ProductionPriority);
		}
	}
	if (SelectTeamToReinforce(HiPri))
	{
		return true;
	}
	if (HiPri == InvalidPri)
	{
		return false;
	}
	Candidates.RemoveAll([this, HiPri](int32 i) { return Protos[i].ProductionPriority != HiPri; });
	const int32 Pick = Candidates[Logic->GetRandom().RandRange(0, Candidates.Num() - 1)];
	BuildSpecificTeam(Pick, false);

	const FGenAIData& AI = FGenAIData::Get();
	bReadyToBuildTeam = false;
	float Timer = AI.TeamSeconds * float(GenLogic::GetLogicFPS());
	if (GetMoney() < AI.Poor) { Timer /= FMath::Max(AI.TeamsPoorRate, 0.01f); }
	else if (GetMoney() > AI.Wealthy) { Timer /= FMath::Max(AI.TeamsWealthyRate, 0.01f); }
	TeamTimer = FMath::RoundToInt32(Timer);
	return true;
}

void FGenSkirmishAI::BuildSpecificTeam(int32 ProtoIndex, bool bPriorityBuild)
{
	// AIPlayer::buildSpecificAITeam: an (inactive) team instance plus the work orders that fill it.
	if (!Protos.IsValidIndex(ProtoIndex) || !bCanBuildUnits)
	{
		return;
	}
	FGenTeamProto& Proto = Protos[ProtoIndex];
	if (bPriorityBuild && Proto.bSingleton)
	{
		for (const TUniquePtr<FGenTeam>& T : Teams)
		{
			if (T->Proto == ProtoIndex && TeamHasUnits(*T))
			{
				return;
			}
		}
	}
	bool bNeedMoney = false;
	if (!IsPossibleToBuildTeam(Proto, false, bNeedMoney) && !bNeedMoney)
	{
		return; // the factories for it don't exist
	}
	FGenTeamInQueue TQ;
	// Required units first, then the optional ones.
	for (const FGenTeamProto::FUnits& U : Proto.Units)
	{
		if (const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(U.Unit))
		{
			FGenWorkOrder& Order = TQ.Orders.AddDefaulted_GetRef();
			Order.Thing = Thing;
			Order.NumRequired = U.Min;
			Order.bRequired = true;
		}
	}
	for (const FGenTeamProto::FUnits& U : Proto.Units)
	{
		const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(U.Unit);
		if (Thing && U.Max - U.Min > 0)
		{
			FGenWorkOrder& Order = TQ.Orders.AddDefaulted_GetRef();
			Order.Thing = Thing;
			Order.NumRequired = U.Max - U.Min;
		}
	}
	if (TQ.Orders.Num() == 0)
	{
		return;
	}
	FGenTeam& Team = CreateTeam(ProtoIndex);
	TQ.Team = Team.Id;
	TQ.bPriorityBuild = bPriorityBuild;
	TQ.FrameStarted = Frame;
	if (bPriorityBuild)
	{
		TeamBuildQueue.Insert(TQ, 0);
	}
	else
	{
		TeamBuildQueue.Add(TQ);
	}
	TeamDelay = 0;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d starts team '%s' (priority %d%s)"), Frame, Player, *Proto.Name, Proto.ProductionPriority,
		bPriorityBuild ? TEXT(", by script") : TEXT(""));
}

void FGenSkirmishAI::RecruitSpecificTeam(int32 ProtoIndex, float RecruitRadius)
{
	// AISkirmishPlayer::recruitSpecificAITeam: fill a team from units we already have.
	if (!Protos.IsValidIndex(ProtoIndex))
	{
		return;
	}
	const FGenTeamProto& Proto = Protos[ProtoIndex];
	if (RecruitRadius < 1.f)
	{
		RecruitRadius = 99999.f;
	}
	FGenTeam& Team = CreateTeam(ProtoIndex);
	int32 Recruited = 0;
	for (const FGenTeamProto::FUnits& U : Proto.Units)
	{
		const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(U.Unit);
		for (int32 n = 0; Thing && n < U.Max; ++n)
		{
			FGenObject* Unit = TryToRecruit(Team, *Thing, BaseCenter, RecruitRadius);
			if (!Unit)
			{
				break;
			}
			SetObjectTeam(Unit->GetID(), Team.Id);
			++Recruited;
		}
	}
	if (Recruited > 0)
	{
		FGenTeamInQueue TQ;
		TQ.Team = Team.Id;
		TQ.FrameStarted = Frame;
		TeamReadyQueue.Insert(TQ, 0);
	}
	else if (!Proto.bSingleton)
	{
		DeleteTeam(Team.Id);
	}
}

void FGenSkirmishAI::CheckReadyTeams()
{
	// AIPlayer::checkReadyTeams: a finished team starts once its members have come out and stand idle
	// (or after a minute).
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	for (int32 i = TeamReadyQueue.Num() - 1; i >= 0; --i)
	{
		FGenTeamInQueue& TQ = TeamReadyQueue[i];
		FGenTeam* Team = FindTeam(TQ.Team);
		if (!Team)
		{
			TeamReadyQueue.RemoveAt(i);
			continue;
		}
		bool bAllIdle = true;
		if (TQ.bReinforcement)
		{
			const FGenObject* Unit = Logic->FindObject(TQ.ReinforcementID);
			bAllIdle = !IsAlive(Unit) || Unit->IsAIIdle();
		}
		else
		{
			bAllIdle = TeamIsIdle(*Team);
		}
		if (bAllIdle || TQ.FrameStarted + 60 * FPS < Frame)
		{
			if (!TQ.bReinforcement)
			{
				SetTeamActive(*Team);
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [ai f%u] p%d team '%s' is ready (%d units)"), Frame, Player,
					Protos.IsValidIndex(Team->Proto) ? *Protos[Team->Proto].Name : TEXT("default"), Team->Members.Num());
			}
			TeamReadyQueue.RemoveAt(i);
		}
	}
}

void FGenSkirmishAI::CheckQueuedTeams()
{
	// AIPlayer::checkQueuedTeams: teams whose units are all there move to the ready queue; teams that
	// took too long start with what they have, or are given up.
	for (int32 i = TeamBuildQueue.Num() - 1; i >= 0; --i)
	{
		FGenTeamInQueue& TQ = TeamBuildQueue[i];
		FGenTeam* Team = FindTeam(TQ.Team);
		if (!Team)
		{
			TeamBuildQueue.RemoveAt(i);
			continue;
		}
		bool bAllBuilt = true, bMinimumBuilt = true, bBuildsComplete = true;
		for (const FGenWorkOrder& Order : TQ.Orders)
		{
			bAllBuilt &= Order.NumCompleted >= Order.NumRequired;
			bBuildsComplete &= Order.Factory == GenInvalidObjectID;
			if (Order.bRequired && Order.NumRequired > Order.NumCompleted + (Order.Factory != GenInvalidObjectID ? 1 : 0))
			{
				bMinimumBuilt = false;
			}
		}
		const int32 IdleFrames = Protos.IsValidIndex(Team->Proto) ? Protos[Team->Proto].InitialIdleFrames : 0;
		const bool bExpired = IdleFrames >= 1 && Frame > TQ.FrameStarted + uint32(IdleFrames);
		if (bAllBuilt || (bExpired && bMinimumBuilt && bBuildsComplete))
		{
			const FGenTeamInQueue Moved = TQ;
			TeamBuildQueue.RemoveAt(i);
			if (Moved.Team == 0)
			{
				continue; // dozers and gatherers on the default team: nothing to activate
			}
			TeamReadyQueue.Insert(Moved, 0);
		}
		else if (bExpired && !bMinimumBuilt)
		{
			// Disband: what was built goes to the default team.
			const int32 TeamId = TQ.Team;
			TeamBuildQueue.RemoveAt(i);
			if (TeamId != 0)
			{
				for (FGenObjectID Id : TArray<FGenObjectID>(Team->Members))
				{
					SetObjectTeam(Id, 0);
				}
				if (!Protos.IsValidIndex(Team->Proto) || !Protos[Team->Proto].bSingleton)
				{
					DeleteTeam(TeamId);
				}
			}
		}
	}
}

// ---- upgrades, general's promotions, special powers -----------------------------------------------

void FGenSkirmishAI::DoUpgradesAndSkills()
{
	// AIPlayer::doUpgradesAndSkills: general's points go on one of the side's skill sets, in the order it lists.
	if (Frame < 2 || Logic->GetPlayers()[Player].SciencePurchasePoints <= 0)
	{
		return;
	}
	const FGenAISideInfo* Info = FGenAIData::Get().Sides.Find(Side);
	if (!Info)
	{
		return;
	}
	if (SkillSetSelector == INDEX_NONE)
	{
		// Any of the sets that have something in them (designers fill them in order).
		int32 Limit = 0;
		while (Limit + 1 < 5 && Info->SkillSets[Limit + 1].Num() > 0)
		{
			++Limit;
		}
		SkillSetSelector = Logic->GetRandom().RandRange(0, Limit);
	}
	for (const FString& Science : Info->SkillSets[SkillSetSelector])
	{
		if (Logic->CanPurchaseScience(Player, Science))
		{
			Logic->PurchaseScience(Player, Science);
		}
	}
}

void FGenSkirmishAI::BuildUpgrade(const FString& UpgradeName)
{
	// AIPlayer::buildUpgrade: a player upgrade, at whichever of our buildings offers it.
	const FGenUpgradeTemplate* Upgrade = FGenUpgradeData::Get().FindUpgrade(UpgradeName);
	if (!Upgrade || Upgrade->bObjectUpgrade || Logic->HasUpgradeInProduction(Player, UpgradeName) || Logic->HasUpgradeComplete(Player, UpgradeName)
		|| GetMoney() < Upgrade->BuildCost)
	{
		return;
	}
	for (const FGenBuildListInfo& Info : BuildList)
	{
		FGenObject* Factory = Logic->FindObject(Info.Object);
		if (IsAlive(Factory) && Logic->QueueUpgrade(*Factory, *Upgrade))
		{
			return;
		}
	}
}

bool FGenSkirmishAI::HasCommandButton(const FGenObject& Obj, const FGenCommandButton& Button) const
{
	const FGenCommandSet* Set = FGenCommandData::Get().FindSet(Obj.GetCommandSet());
	for (int32 s = 0; Set && s < FGenCommandSet::NumSlots; ++s)
	{
		if (Set->Slots[s].Equals(Button.Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool FGenSkirmishAI::DoCommandButton(FGenObject& Obj, const FGenCommandButton& Button)
{
	// Object::doCommandButton, for the kinds of button that work so far: upgrades and unit production.
	if (Obj.IsDisabled() || Obj.IsUnderConstruction() || !HasCommandButton(Obj, Button))
	{
		return false;
	}
	switch (Button.Command)
	{
	case EGenCommand::PlayerUpgrade:
	case EGenCommand::ObjectUpgrade:
	{
		const FGenUpgradeTemplate* Upgrade = FGenUpgradeData::Get().FindUpgrade(Button.Upgrade);
		// (An object upgrade whose effect isn't there yet would be money for nothing.)
		return Upgrade && (!Upgrade->bObjectUpgrade || Obj.HasPortedUpgradeEffect(*Upgrade)) && Logic->QueueUpgrade(Obj, *Upgrade);
	}
	case EGenCommand::UnitBuild:
	{
		const FGenThingTemplate* Thing = FGenGameData::Get().FindTemplate(Button.Object);
		return Thing && Logic->QueueUnit(Obj, *Thing);
	}
	case EGenCommand::HackInternet:
		if (!Obj.GetTemplate()->HackInternet.bValid || Obj.IsContained())
		{
			return false;
		}
		Obj.CommandHackInternet();
		return true;
	default:
		return false;
	}
}

int32 FGenSkirmishAI::GetPlayerSuperweaponValue(const FVector2f& Center, int32 EnemyIndex, float Radius, bool bIncludeMilitaryUnits) const
{
	// AIPlayer::getPlayerSuperweaponValue: what the enemy has around that spot is worth, things near the middle counting most.
	Radius = FMath::Max(Radius, 4.f * GenLogic::PathfindCellSize);
	float Cash = 0.f;
	for (const FGenObject* O : Logic->GetObjectsOf(EnemyIndex))
	{
		if (!IsAlive(O) || O->IsProjectile())
		{
			continue;
		}
		bool bNegative = false;
		if (!bIncludeMilitaryUnits)
		{
			// (A sneak attack wants a rich spot that nobody defends.)
			if (O->IsKindOf(TEXT("FS_BASE_DEFENSE")) || O->IsKindOf(TEXT("TECH_BASE_DEFENSE")))
			{
				bNegative = true;
			}
			else if ((O->IsKindOf(TEXT("VEHICLE")) || O->IsKindOf(TEXT("INFANTRY"))) && !O->IsKindOf(TEXT("DOZER")) && !O->IsKindOf(TEXT("HARVESTER")))
			{
				bNegative = true;
			}
		}
		else if (O->IsKindOf(TEXT("AIRCRAFT")) && O->IsAirborneTarget())
		{
			continue; // not at aircraft in flight; parked ones are fine
		}
		const float Dist = FVector2f::Distance(Center, FVector2f(O->GetPosition().X, O->GetPosition().Y));
		if (Dist >= Radius)
		{
			continue;
		}
		const float Factor = 1.f - Dist / (2.f * Radius); // 1 in the middle, 0.5 at the rim
		float Value = float(O->GetTemplate()->BuildCost);
		if (O->IsKindOf(TEXT("COMMANDCENTER")) || O->IsKindOf(TEXT("FS_SUPERWEAPON")))
		{
			// No power takes those down in one go; a sneak attack is another matter.
			Value = bIncludeMilitaryUnits ? Value / 10.f : Value * 5.f;
		}
		Cash += bNegative ? -Factor * Value * 5.f : Factor * Value;
	}
	return int32(Cash);
}

bool FGenSkirmishAI::ComputeSuperweaponTarget(bool bTargetMilitaryUnits, int32 EnemyIndex, float WeaponRadius, FVector3f& OutPos) const
{
	// AIPlayer::computeSuperweaponTarget: a coarse grid over the enemy's base, then a finer look around the best cell.
	FBox2f Bounds(ForceInit);
	bool bHasStructures = false;
	for (const FGenObject* O : Logic->GetObjectsOf(EnemyIndex))
	{
		if (IsAlive(O) && O->IsKindOf(TEXT("STRUCTURE")))
		{
			Bounds += FVector2f(O->GetPosition().X, O->GetPosition().Y);
			bHasStructures = true;
		}
	}
	if (!bHasStructures)
	{
		// No buildings left: the whole map, for the leftovers.
		Logic->GetMapExtent(Bounds.Min, Bounds.Max, false);
	}
	WeaponRadius = FMath::Max(WeaponRadius, 1.f);
	Bounds.Min.X += WeaponRadius;
	Bounds.Max.X -= WeaponRadius;
	if (Bounds.Max.X < Bounds.Min.X) { Bounds.Max.X = Bounds.Min.X = (Bounds.Max.X + Bounds.Min.X) / 2.f; }
	if (Bounds.Max.Y < Bounds.Min.Y) { Bounds.Max.Y = Bounds.Min.Y = (Bounds.Max.Y + Bounds.Min.Y) / 2.f; }
	const FVector2f Size = Bounds.Max - Bounds.Min;
	const int32 XCount = FMath::Min(FMath::CeilToInt32(Size.X / WeaponRadius) + 1, 10);
	const int32 YCount = FMath::Min(FMath::CeilToInt32(Size.Y / WeaponRadius) + 1, 10);

	// (The grid is walked from a random corner, so that ties don't always end up in the same one.)
	const int32 Corner = Logic->GetRandom().RandRange(1, 4);
	const int32 XDelta = (Corner == 1 || Corner == 3) ? 1 : -1, YDelta = (Corner <= 2) ? 1 : -1;
	const int32 XStart = XDelta > 0 ? 0 : XCount, YStart = YDelta > 0 ? 0 : YCount;
	int32 Cash = -1;
	FVector2f BestPos = Bounds.Min;
	for (int32 x = 0, XIndex = XStart; x < XCount; ++x, XIndex += XDelta)
	{
		Logic->SimYield(TEXT("a superweapon's target"));
		for (int32 y = 0, YIndex = YStart; y < YCount; ++y, YIndex += YDelta)
		{
			const FVector2f Pos(Bounds.Min.X + Size.X * XIndex / XCount, Bounds.Min.Y + Size.Y * YIndex / YCount);
			const int32 CurCash = GetPlayerSuperweaponValue(Pos, EnemyIndex, 2.f * WeaponRadius, bTargetMilitaryUnits);
			if (CurCash > Cash)
			{
				Cash = CurCash;
				BestPos = Pos;
			}
		}
	}

	// Fine tuning. (The original steps both coordinates with the same index, so it looks along a diagonal.)
	FVector2f VeryBestPos = BestPos;
	int32 Count = 0;
	Cash = -1;
	for (int32 x = 0; x < 11; ++x)
	{
		Logic->SimYield(TEXT("a superweapon's target"));
		for (int32 y = 0; y < 11; ++y)
		{
			const FVector2f Pos(BestPos.X + (x - 5) * (WeaponRadius / 10.f), BestPos.Y + (x - 5) * (WeaponRadius / 10.f));
			const int32 CurCash = GetPlayerSuperweaponValue(Pos, EnemyIndex, WeaponRadius, bTargetMilitaryUnits);
			if (CurCash > Cash)
			{
				Cash = CurCash;
				VeryBestPos = Pos;
				Count = 1;
			}
			else if (CurCash == Cash)
			{
				VeryBestPos += Pos;
				++Count;
			}
		}
	}
	if (Count > 1)
	{
		VeryBestPos /= float(Count);
	}
	OutPos = FVector3f(VeryBestPos.X, VeryBestPos.Y, Logic->GetGroundHeight(VeryBestPos.X, VeryBestPos.Y));
	return Cash > -1;
}

void FGenSkirmishAI::FireSpecialPowerAtMostCost(const FString& PowerName)
{
	// ScriptActions::doSkirmishFireSpecialPowerAtMostCost
	const int32 Enemy = GetEnemy();
	const FGenSpecialPowerTemplate* Power = FGenUpgradeData::Get().FindPower(PowerName);
	if (Enemy == INDEX_NONE || !Power)
	{
		return;
	}
	int32 Module = INDEX_NONE;
	FGenObject* Source = Logic->FindPowerSource(Player, PowerName, true, &Module);
	if (!Source || Source->IsDisabled())
	{
		return;
	}
	const float Radius = FMath::Max(50.f, Power->RadiusCursorRadius);
	FVector3f Location;
	if (!ComputeSuperweaponTarget(Power->Enum != TEXT("SPECIAL_SNEAK_ATTACK"), Enemy, Radius, Location) || FVector2f(Location.X, Location.Y).SizeSquared() <= 0.f)
	{
		return;
	}
	// "We've already calculated the sweet spot, but we need to modify that position if we can't place it in
	// the current location" (Player::calcClosestConstructionZoneLocation, for the sneak attack's tunnel).
	const FString& ReferenceName = Source->GetTemplate()->PowerModules[Module].ReferenceObject;
	if (const FGenThingTemplate* Reference = ReferenceName.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(ReferenceName))
	{
		if (!Logic->IsLocationLegalToBuild(*Reference, Location, Reference->PlacementViewAngle)
			&& !FindPlaceToBuild(*Reference, Reference->PlacementViewAngle, 40.f * GenLogic::PathfindCellSize, Location))
		{
			return;
		}
	}
	// (A power aimed at a building -- the cash hack -- takes the supply center nearest that spot.)
	FGenObjectID TargetObject = GenInvalidObjectID;
	if (Source->GetTemplate()->PowerModules[Module].Type == EGenPowerModule::CashHack)
	{
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : Logic->GetObjectIDs())
		{
			const FGenObject* O = Logic->FindObject(Id);
			const float DistSq = O ? FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), FVector2f(Location.X, Location.Y)) : 0.f;
			if (IsAlive(O) && O->GetOwner() == Enemy && O->IsKindOf(TEXT("FS_SUPPLY_CENTER")) && DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				TargetObject = Id;
			}
		}
		if (TargetObject == GenInvalidObjectID)
		{
			return;
		}
	}
	Logic->FireSpecialPower(*Source, Module, Location, TargetObject);
}

// ---- enemy --------------------------------------------------------------------------------------

void FGenSkirmishAI::GetPlayerStructureBounds(int32 PlayerIndex, FBox2f& OutBounds) const
{
	OutBounds = FBox2f(ForceInit);
	for (const FGenObject* O : Logic->GetObjectsOf(PlayerIndex))
	{
		if (IsAlive(O) && O->IsKindOf(TEXT("STRUCTURE")))
		{
			OutBounds += FVector2f(O->GetPosition().X, O->GetPosition().Y);
		}
	}
	if (!OutBounds.bIsValid)
	{
		const FVector2f C(BaseCenter.X, BaseCenter.Y);
		OutBounds = FBox2f(C, C);
	}
}

void FGenSkirmishAI::AcquireEnemy()
{
	// AISkirmishPlayer::acquireEnemy: the closest enemy that still has something.
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	auto HasObjects = [this](int32 P, bool bStructuresOnly)
	{
		for (const FGenObject* O : Logic->GetObjectsOf(P))
		{
			if (IsAlive(O) && O->IsTargetable() && (!bStructuresOnly || O->GetTemplate()->bHasProduction))
			{
				return true;
			}
		}
		return false;
	};
	if (CurrentEnemy != INDEX_NONE && HasObjects(CurrentEnemy, true))
	{
		return;
	}
	int32 Best = INDEX_NONE;
	float BestDistSq = FLT_MAX;
	for (int32 i = 0; i < Players.Num(); ++i)
	{
		if (Logic->GetRelationship(Player, i) != EGenRelationship::Enemies || !HasObjects(i, false))
		{
			continue;
		}
		FBox2f Bounds;
		GetPlayerStructureBounds(i, Bounds);
		float DistSq = FVector2f::DistSquared(Bounds.GetCenter(), FVector2f(BaseCenter.X, BaseCenter.Y));
		if (!HasObjects(i, true))
		{
			DistSq = FLT_MAX * 0.5f; // crippled: concentrate on the others
		}
		if (DistSq < BestDistSq)
		{
			Best = i;
			BestDistSq = DistSq;
		}
	}
	if (Best != INDEX_NONE)
	{
		CurrentEnemy = Best;
	}
}

int32 FGenSkirmishAI::GetEnemy()
{
	if (Frame >= FrameToCheckEnemy)
	{
		FrameToCheckEnemy = Frame + 5 * uint32(GenLogic::GetLogicFPS());
		AcquireEnemy();
	}
	if (CurrentEnemy == INDEX_NONE)
	{
		// ScriptEngine::getSkirmishEnemyPlayer: failing that, the (first) human player.
		const TArray<FGenPlayer>& Players = Logic->GetPlayers();
		for (int32 i = 0; i < Players.Num(); ++i)
		{
			if (Players[i].bHuman)
			{
				return i;
			}
		}
	}
	return CurrentEnemy;
}

// ---- teams --------------------------------------------------------------------------------------

FGenTeam* FGenSkirmishAI::FindTeam(int32 TeamId)
{
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		if (T->Id == TeamId)
		{
			return T.Get();
		}
	}
	return nullptr;
}

int32 FGenSkirmishAI::FindProto(const FString& Name) const
{
	return Protos.IndexOfByPredicate([&Name](const FGenTeamProto& P) { return P.Name == Name; });
}

int32 FGenSkirmishAI::CountTeamInstances(int32 ProtoIndex) const
{
	int32 Count = 0;
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		Count += T->Proto == ProtoIndex ? 1 : 0;
	}
	return Count;
}

FGenTeam& FGenSkirmishAI::CreateTeam(int32 ProtoIndex)
{
	// TeamFactory::createInactiveTeam
	const FGenTeamProto& Proto = Protos[ProtoIndex];
	FGenTeam* Team = nullptr;
	if (Proto.bSingleton)
	{
		for (const TUniquePtr<FGenTeam>& T : Teams)
		{
			if (T->Proto == ProtoIndex)
			{
				Team = T.Get();
				break;
			}
		}
	}
	if (!Team)
	{
		Team = Teams.Add_GetRef(MakeUnique<FGenTeam>()).Get();
		Team->Id = NextTeamId++;
		Team->Proto = ProtoIndex;
	}
	if (Proto.bExecuteActions)
	{
		if (const FGenScript* Script = FindScript(Proto.ProductionCondition))
		{
			ExecuteActionsForTeam(Script->Actions, nullptr);
		}
	}
	return *Team;
}

void FGenSkirmishAI::SetTeamActive(FGenTeam& Team)
{
	if (!Team.bActive)
	{
		Team.bActive = true;
		Team.bCreated = true;
	}
}

void FGenSkirmishAI::DeleteTeam(int32 TeamId)
{
	if (TeamId == 0)
	{
		return;
	}
	for (int32 i = 0; i < Teams.Num(); ++i)
	{
		if (Teams[i]->Id != TeamId)
		{
			continue;
		}
		// AIPlayer::aiPreTeamDestroy / ScriptEngine::notifyOfTeamDestruction
		TeamBuildQueue.RemoveAll([TeamId](const FGenTeamInQueue& TQ) { return TQ.Team == TeamId; });
		TeamReadyQueue.RemoveAll([TeamId](const FGenTeamInQueue& TQ) { return TQ.Team == TeamId; });
		RemoveSequentialScripts(TeamId);
		if (CallingTeam == Teams[i].Get()) { CallingTeam = nullptr; }
		if (ConditionTeam == Teams[i].Get()) { ConditionTeam = nullptr; }
		for (FGenObjectID Id : Teams[i]->Members)
		{
			ObjectTeams.Remove(Id);
		}
		Teams.RemoveAt(i);
		return;
	}
}

int32 FGenSkirmishAI::GetObjectTeam(FGenObjectID Object) const
{
	const int32* Found = ObjectTeams.Find(Object);
	return Found ? *Found : 0;
}

void FGenSkirmishAI::SetObjectTeam(FGenObjectID Object, int32 TeamId)
{
	if (FGenTeam* Old = FindTeam(GetObjectTeam(Object)))
	{
		Old->Members.Remove(Object);
	}
	ObjectTeams.Remove(Object);
	if (TeamId != 0)
	{
		if (FGenTeam* New = FindTeam(TeamId))
		{
			New->Members.AddUnique(Object);
			ObjectTeams.Add(Object, TeamId);
		}
	}
}

void FGenSkirmishAI::PurgeDeadMembers()
{
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		for (int32 i = T->Members.Num() - 1; i >= 0; --i)
		{
			const FGenObject* O = Logic->FindObject(T->Members[i]);
			if (!IsAlive(O) || O->GetOwner() != Player)
			{
				ObjectTeams.Remove(T->Members[i]);
				T->Members.RemoveAt(i);
			}
		}
	}
}

void FGenSkirmishAI::ForEachMember(const FGenTeam& Team, TFunctionRef<void(FGenObject&)> Fn) const
{
	if (Team.Id != 0)
	{
		for (FGenObjectID Id : TArray<FGenObjectID>(Team.Members))
		{
			FGenObject* O = Logic->FindObject(Id);
			if (IsAlive(O))
			{
				Fn(*O);
			}
		}
		return;
	}
	// The default team: everything of ours that is on no other team.
	for (FGenObjectID Id : TArray<FGenObjectID>(Logic->GetObjectIDs()))
	{
		FGenObject* O = Logic->FindObject(Id);
		if (IsAlive(O) && O->GetOwner() == Player && !O->IsProjectile() && !ObjectTeams.Contains(Id))
		{
			Fn(*O);
		}
	}
}

bool FGenSkirmishAI::TeamHasUnits(const FGenTeam& Team) const
{
	bool bAny = false;
	ForEachMember(Team, [&bAny](FGenObject& O) { bAny |= !O.IsKindOf(TEXT("STRUCTURE")); });
	return bAny;
}

bool FGenSkirmishAI::TeamIsIdle(const FGenTeam& Team) const
{
	bool bIdle = true;
	ForEachMember(Team, [&bIdle](FGenObject& O) { bIdle &= !O.CanMove() || O.IsAIIdle(); });
	return bIdle;
}

bool FGenSkirmishAI::GetTeamCenter(const FGenTeam& Team, FVector2f& OutCenter) const
{
	FVector2f Sum = FVector2f::ZeroVector;
	int32 Num = 0;
	ForEachMember(Team, [&Sum, &Num](FGenObject& O) { Sum += FVector2f(O.GetPosition().X, O.GetPosition().Y); ++Num; });
	if (Num == 0)
	{
		return false;
	}
	OutCenter = Sum / float(Num);
	return true;
}

FGenObject* FGenSkirmishAI::TryToRecruit(const FGenTeam& ForTeam, const FGenThingTemplate& Thing, const FVector3f& Home, float MaxDist)
{
	// Team::tryToRecruit: a unit of that type on the default team or on a recruitable, lower priority team.
	const int32 MyPriority = Protos.IsValidIndex(ForTeam.Proto) ? Protos[ForTeam.Proto].ProductionPriority : 0;
	FGenObject* Recruit = nullptr;
	float DistSq = MaxDist * MaxDist;
	for (FGenObject* O : Logic->GetObjectsOf(Player))
	{
		if (!IsAlive(O) || O->GetTemplate()->Canonical() != Thing.Canonical() || O->IsUnderConstruction())
		{
			continue;
		}
		const int32 TeamId = GetObjectTeam(O->GetID());
		if (TeamId == ForTeam.Id)
		{
			continue;
		}
		const FGenTeam* Team = FindTeam(TeamId);
		if (!Team || !Team->bActive)
		{
			continue; // a team still being built keeps its members
		}
		const bool bDefault = TeamId == 0;
		const FGenTeamProto* Proto = Protos.IsValidIndex(Team->Proto) ? &Protos[Team->Proto] : nullptr;
		if (!bDefault && Proto && Proto->ProductionPriority >= MyPriority)
		{
			continue;
		}
		bool bRecruitable = bDefault || (Proto && Proto->bRecruitable);
		if (Team->Recruitable >= 0)
		{
			bRecruitable = Team->Recruitable != 0;
		}
		// Gatherers at work and dozers aren't taken away from their jobs.
		if (!bRecruitable || O->HasDozerTask()
			|| (O->GetTemplate()->bIsSupplyTruck && (O->GetSupplyState() != EGenSupplyState::Idle || O->bForceWanting)))
		{
			continue;
		}
		const float D = FVector2f::DistSquared(FVector2f(Home.X, Home.Y), FVector2f(O->GetPosition().X, O->GetPosition().Y));
		if (bDefault && !Recruit)
		{
			Recruit = O;
			DistSq = D;
		}
		if (D > DistSq)
		{
			continue;
		}
		DistSq = D;
		Recruit = O;
	}
	return Recruit;
}

void FGenSkirmishAI::UpdateTeamStates()
{
	GEN_PERF_SCOPE("AI.Teams");
	// TeamPrototype::updateState / Team::updateState: on-create, on-destroyed and on-idle scripts;
	// active teams without members are removed.
	TArray<int32, TInlineAllocator<32>> TeamIds;
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		if (T->bActive && T->Id != 0 && Protos.IsValidIndex(T->Proto))
		{
			TeamIds.Add(T->Id);
		}
	}
	for (int32 TeamId : TeamIds)
	{
		// (Scripts may disband or delete teams, so the team is looked up again after each one.)
		FGenTeam* Team = FindTeam(TeamId);
		if (!Team)
		{
			continue;
		}
		const FGenTeamProto& Proto = Protos[Team->Proto];
		int32 Alive = 0;
		bool bIdle = true;
		ForEachMember(*Team, [&Alive, &bIdle](FGenObject& O) { ++Alive; bIdle &= !O.CanMove() || O.IsAIIdle(); });
		if (Team->bCreated)
		{
			Team->bCreated = false;
			if (!Proto.OnDestroyed.IsEmpty())
			{
				Team->CurUnits = Alive;
				Team->DestroyThreshold = FMath::Clamp(Alive - int32(float(Alive) * Proto.DestroyedThreshold), 0, FMath::Max(0, Alive - 1));
			}
			if (!Proto.OnCreate.IsEmpty())
			{
				RunScript(Proto.OnCreate, Team);
				Team = FindTeam(TeamId);
			}
		}
		if (Team && !Proto.OnDestroyed.IsEmpty())
		{
			const int32 Prev = Team->CurUnits;
			Team->CurUnits = Alive;
			if (Alive != Prev && Alive <= Team->DestroyThreshold)
			{
				Team->DestroyThreshold = -1; // only once
				RunScript(Proto.OnDestroyed, Team);
				Team = FindTeam(TeamId);
			}
		}
		if (Team && !Proto.OnIdle.IsEmpty())
		{
			const bool bWasIdle = Team->bWasIdle;
			Team->bWasIdle = bIdle;
			if (Alive > 0 && bIdle && bWasIdle)
			{
				RunScript(Proto.OnIdle, Team);
			}
		}
	}
	// Empty teams go away (not singletons, not the default team, not teams still under construction).
	for (int32 i = Teams.Num() - 1; i >= 1; --i)
	{
		const FGenTeam& Team = *Teams[i];
		if (Team.bActive && Team.Members.Num() == 0 && !(Protos.IsValidIndex(Team.Proto) && Protos[Team.Proto].bSingleton))
		{
			DeleteTeam(Team.Id);
		}
	}
}

void FGenSkirmishAI::UpdateGenericScripts()
{
	GEN_PERF_SCOPE("AI.GenericScripts");
	// Team::updateGenericScripts: every team instance tries its generic scripts each frame.
	for (int32 t = 0; t < Teams.Num(); ++t)
	{
		FGenTeam* Team = Teams[t].Get();
		if (!Protos.IsValidIndex(Team->Proto))
		{
			continue;
		}
		const int32 TeamId = Team->Id;
		for (int32 g = 0; g < FGenTeamProto::MaxGenericScripts; ++g)
		{
			if (!Team->bGenericPending[g])
			{
				continue;
			}
			const FString& Name = Protos[Team->Proto].Generic[g];
			const FGenScript* Script = Name.IsEmpty() ? nullptr : FindScript(Name);
			if (!Script)
			{
				Team->bGenericPending[g] = false;
				continue;
			}
			if (EvaluateConditions(*Script, Team))
			{
				if (Script->bOneShot)
				{
					Team->bGenericPending[g] = false;
				}
				ExecuteActionsForTeam(Script->Actions, Team);
				// An action may have disbanded the team.
				Team = FindTeam(TeamId);
				if (!Team)
				{
					break;
				}
			}
		}
	}
}

FString FGenSkirmishAI::Describe() const
{
	int32 Built = 0, Wanted = 0;
	for (const FGenBuildListInfo& Info : BuildList)
	{
		Built += IsAlive(Logic->FindObject(Info.Object)) ? 1 : 0;
		Wanted += Info.bPriorityBuild ? 1 : 0;
	}
	const FCounter* Escalation = Counters.Find(TEXT("_ESCALATION"));
	const FCounter* Wave = Counters.Find(TEXT("_ATTACK_WAVE"));
	return FString::Printf(TEXT("p%d %s money=%d buildings=%d/%d requested teams=%d building=%d ready=%d escalation=%d wave=%d enemy=%d"), Player, *Side,
		GetMoney(), Built, Wanted, Teams.Num() - 1, TeamBuildQueue.Num(), TeamReadyQueue.Num(), Escalation ? Escalation->Value : 0, Wave ? Wave->Value : 0,
		CurrentEnemy);
}
