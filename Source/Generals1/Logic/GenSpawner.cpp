// SpawnBehavior that keeps its units (the GLA stinger site's soldiers), with SpawnPointProductionExitUpdate (they
// stand at the building's "SpawnPoint" bones, held there) and the building's side of HiveStructureBody (in
// GenObjectCombat.cpp: some kinds of damage go to the soldiers instead).
//
// The site makes its soldiers once it is finished, and a new one SpawnReplaceDelay after each one dies; with
// SpawnedRequireSpawner they die (or go) with it. They are its weapons (SPAWNS_ARE_THE_WEAPONS): an attack order
// for the site is theirs, and they otherwise fire at whatever comes within range by themselves.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

void FGenObject::UpdateSpawner(uint32 Frame)
{
	// SpawnBehavior::update
	const FGenThingTemplate::FSpawner& D = Template->Spawner;
	if (!bSpawnsInited)
	{
		// "Set all spawns to be created as soon as possible."
		bSpawnsInited = true;
		for (int32 i = 0; i < D.Number; ++i)
		{
			SpawnReplaceFrames.Add(uint32(i));
		}
	}
	// (onSpawnDeath: one that has died or gone is replaced SpawnReplaceDelay later.)
	for (int32 i = Spawns.Num() - 1; i >= 0; --i)
	{
		const FGenObject* S = Logic->FindObject(Spawns[i]);
		if (!S || S->IsDead() || S->IsDestroyed())
		{
			Spawns.RemoveAt(i);
			SpawnReplaceFrames.Add(Frame + uint32(D.ReplaceFrames));
		}
	}
	// (SPAWN_UPDATE_RATE: "a low priority module" - twice a second.)
	if (Frame < NextSpawnUpdate)
	{
		return;
	}
	NextSpawnUpdate = Frame + uint32(FMath::Max(1, GenLogic::GetLogicFPS() / 2));
	// shouldTryToSpawn: not while it is being built or sold, nor for nobody.
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	if (bUnderConstruction || SellFrame != 0 || !Players.IsValidIndex(Owner) || Players[Owner].bNeutral)
	{
		return;
	}
	for (int32 i = 0; i < SpawnReplaceFrames.Num(); )
	{
		if (Frame > SpawnReplaceFrames[i] && CreateSpawn())
		{
			SpawnReplaceFrames.RemoveAt(i);
		}
		else
		{
			++i;
		}
	}
}

bool FGenObject::CreateSpawn()
{
	// SpawnBehavior::createSpawn
	const FGenThingTemplate* What = FGenGameData::Get().FindTemplate(Template->Spawner.Template);
	if (!What)
	{
		return false;
	}
	// SpawnPointProductionExitUpdate::reserveDoorForExit: a spawn point nobody (alive) holds, or none to be had.
	int32 Point = INDEX_NONE;
	if (!Template->SpawnPointBone.IsEmpty())
	{
		if (SpawnPoints.Num() == 0)
		{
			// initializeBonePositions: "Get all the bones of the right name", in the world.
			UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
			TArray<FTransform3f> Bones;
			for (const FGenDrawModule& Draw : Template->Draws)
			{
				if (Assets && Assets->GetPristineBones(Draw.DefaultModel(), Template->SpawnPointBone, 1, 16, Bones) > 0)
				{
					break;
				}
			}
			const FTransform3f Xf = GetTransform();
			for (const FTransform3f& Bone : Bones)
			{
				SpawnPoints.Add(Xf.TransformPositionNoScale(Bone.GetTranslation()));
				const FVector3f Forward = Xf.TransformVectorNoScale(Bone.GetRotation().GetAxisX());
				SpawnPointAngles.Add(FMath::Atan2(Forward.Y, Forward.X));
			}
			SpawnPointUsers.Init(GenInvalidObjectID, SpawnPoints.Num());
		}
		for (int32 i = 0; i < SpawnPoints.Num(); ++i)
		{
			const FGenObject* User = Logic->FindObject(SpawnPointUsers[i]);
			if (!User || User->IsDead() || User->IsDestroyed())
			{
				Point = i;
				break;
			}
		}
		if (Point == INDEX_NONE)
		{
			return false; // DOOR_NONE_AVAILABLE
		}
	}
	FVector3f Pos = Position;
	float Angle = Orientation;
	if (Point != INDEX_NONE)
	{
		Pos = SpawnPoints[Point];
		Angle = SpawnPointAngles[Point];
	}
	Pos.Z = Logic->GetGroundHeight(Pos.X, Pos.Y);
	FGenObject* Spawn = Logic->CreateObject(What, Owner, Pos, Angle);
	if (!Spawn)
	{
		return false;
	}
	Spawn->Producer = ID;
	if (What->Slaved.bValid)
	{
		Spawn->Enslave(*this); // "onEnslave( parent )"
	}
	Spawns.Add(Spawn->ID);
	if (Point != INDEX_NONE)
	{
		// exitObjectViaDoor: "You are stuck here, little man." (DISABLED_HELD: it shoots from there, it doesn't walk.)
		SpawnPointUsers[Point] = Spawn->ID;
		Spawn->bImmobile = true;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [spawn f%u] #%u %s makes #%u %s at (%.0f, %.0f) (%d of %d)"), Logic->GetFrame(), ID, *Template->Name,
		Spawn->ID, *What->Name, Pos.X, Pos.Y, Spawns.Num(), Template->Spawner.Number);
	return true;
}

void FGenObject::EndSpawns(bool bDestroy)
{
	// SpawnBehavior::onDie / onDelete: "if( modData->m_spawnedRequireSpawner )" they die - or, the building
	// sold, simply go.
	if (!Template->Spawner.bRequireSpawner)
	{
		return;
	}
	const TArray<FGenObjectID> Gone = MoveTemp(Spawns);
	for (FGenObjectID Id : Gone)
	{
		FGenObject* S = Logic->FindObject(Id);
		if (!S || S->IsDead() || S->IsDestroyed())
		{
			continue;
		}
		if (bDestroy)
		{
			S->bPendingDestroy = true;
		}
		else
		{
			S->Kill();
		}
	}
}

FGenObject* FGenObject::GetClosestSpawn(const FVector3f& Pos) const
{
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	for (FGenObjectID Id : Spawns)
	{
		FGenObject* S = Logic->FindObject(Id);
		if (!S || S->IsDead() || S->IsDestroyed())
		{
			continue;
		}
		const float DistSq = FVector3f::DistSquared(S->Position, Pos);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = S;
		}
	}
	return Best;
}

bool FGenObject::SpawnsCanAttack(const FGenObject& Target) const
{
	for (FGenObjectID Id : Spawns)
	{
		const FGenObject* S = Logic->FindObject(Id);
		if (S && !S->IsDead() && S->HasWeaponFor(Target))
		{
			return true;
		}
	}
	return false;
}
