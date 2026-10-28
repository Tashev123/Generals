// SpawnBehavior that keeps its units (the GLA stinger site's soldiers), with SpawnPointProductionExitUpdate (they
// stand at the building's "SpawnPoint" bones, held there) and the building's side of HiveStructureBody (in
// GenObjectCombat.cpp: some kinds of damage go to the soldiers instead).
//
// The site makes its soldiers once it is finished, and a new one SpawnReplaceDelay after each one dies; with
// SpawnedRequireSpawner they die (or go) with it. They are its weapons (SPAWNS_ARE_THE_WEAPONS): an attack order
// for the site is theirs, and they otherwise fire at whatever comes within range by themselves.
// A stealthed one of these (camo netting) is stealthed as a group: a soldier that cannot be shows them all, so
// the building comes out of invisibility when its soldiers fire (GenStealth.cpp).
//
// The GLA angry mob is one of these too: an invisible, immortal nexus (built at the barracks) that spawns its ten
// members (MobMemberSlavedUpdate, below) from a list of kinds in turn - the first five out of the barracks' door,
// the rest "budded" from whichever of them stands nearest the nexus - and one 30 s after each dies. Its health is
// theirs together (AggregateHealth), it can't be targeted, and it goes when the last of them dies. The members
// keep with the nexus as it walks and take up its target; selecting one selects the mob (GenPlayerController).

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

void FGenObject::UpdateSpawner(uint32 Frame)
{
	// SpawnBehavior::update
	const FGenThingTemplate::FSpawner& D = Template->Spawner;
	// QueueProductionExitUpdate::update (the nexus's own; a factory's counts down with its production): free to
	// let the next one out again one ExitDelay after the last.
	if (!Template->bHasProduction && Template->bQueueExit && ExitDelayLeft > 0 && ExitBurstLeft <= 0)
	{
		--ExitDelayLeft;
	}
	if (D.bAggregateHealth)
	{
		ComputeAggregateStates(); // "EVERY FRAME"
	}
	if (!bSpawnsInited)
	{
		// "Set all spawns to be created as soon as possible." (With an InitialBurst, "in rapid succession": the
		// original keeps whether it was made by a factory, 1 or 0, for the time - not the birth frame it works out.)
		bSpawnsInited = true;
		const bool bRuntimeProduced = Producer != GenInvalidObjectID;
		for (int32 i = 0; i < D.Number; ++i)
		{
			SpawnReplaceFrames.Add(D.InitialBurst > 0 ? uint32(bRuntimeProduced) : uint32(i));
		}
	}
	// (onSpawnDeath for one that has gone without dying: replaced SpawnReplaceDelay later too.)
	for (int32 i = Spawns.Num() - 1; i >= 0; --i)
	{
		const FGenObject* S = Logic->FindObject(Spawns[i]);
		if (!S || S->IsDead() || S->IsDestroyed())
		{
			OnSpawnDeath(Spawns[i], GenInvalidObjectID);
			if (bPendingDestroy)
			{
				return;
			}
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
	const FGenThingTemplate::FSpawner& D = Template->Spawner;
	// QueueProductionExitUpdate::reserveDoorForExit: "isFreeToExit() ? DOOR_1 : DOOR_NONE_AVAILABLE" - still
	// bursting, or the last one left ExitDelay ago.
	if (Template->bQueueExit && !Template->bHasProduction && ExitBurstLeft <= 0 && ExitDelayLeft != 0)
	{
		return false;
	}
	const FGenThingTemplate* What = FGenGameData::Get().FindTemplate(D.Templates[SpawnTemplateIndex % D.Templates.Num()]);
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
	FVector3f Rally = Position;
	FGenObject* Barracks = nullptr;
	const FGenObject* BudHost = nullptr;
	if (Point != INDEX_NONE)
	{
		Pos = SpawnPoints[Point];
		Angle = SpawnPointAngles[Point];
	}
	else if (D.bExitByBudding)
	{
		// The first InitialBurst leave by the door of the barracks that made us ("let parents producer exit him,
		// but he thinks it was me")...
		Barracks = SpawnInitialBurstLeft > 0 ? Logic->FindObject(Producer) : nullptr;
		if (Barracks && !Barracks->IsDead() && Barracks->IsKindOf(TEXT("STRUCTURE")))
		{
			Logic->GetFactoryExit(*Barracks, Pos, Rally);
			Angle = Barracks->Orientation;
		}
		else
		{
			// ... the rest where the one of ours nearest us stands, or where we do: "there is probably a more
			// elegant way to choose the budHost, but oh well" (QueueProductionExitUpdate::exitObjectByBudding).
			Barracks = nullptr;
			float Closest = 999999.9f;
			for (FGenObjectID Id : Spawns)
			{
				const FGenObject* S = Logic->FindObject(Id);
				const float DistSq = S ? FVector2f::DistSquared(FVector2f(S->Position.X, S->Position.Y), FVector2f(Position.X, Position.Y)) : 0.f;
				if (S && DistSq < Closest)
				{
					Closest = DistSq;
					BudHost = S;
				}
			}
			if (BudHost)
			{
				Pos = BudHost->Position;
				Angle = BudHost->Orientation;
			}
		}
	}
	Pos.Z = Logic->GetGroundHeight(Pos.X, Pos.Y);
	FGenObject* Spawn = Logic->CreateObject(What, Owner, Pos, Angle);
	if (!Spawn)
	{
		return false;
	}
	// "Each time m_spawn template is used, it will increment m_templateNameIterator ... looping back to the beginning"
	SpawnTemplateIndex = (SpawnTemplateIndex + 1) % D.Templates.Num();
	Logic->ScoreObjectBuilt(*Spawn); // "Count this unit towards our score."
	Spawn->Producer = ID;
	if (What->Slaved.bValid)
	{
		Spawn->Enslave(*this); // "onEnslave( parent )"
	}
	if (What->MobMember.bValid)
	{
		Spawn->MobSlaver = ID; // MobMemberSlavedUpdate::onEnslave
	}
	Spawns.Add(Spawn->ID);
	if (Point != INDEX_NONE)
	{
		// exitObjectViaDoor: "You are stuck here, little man." (DISABLED_HELD: it shoots from there, it doesn't walk.)
		SpawnPointUsers[Point] = Spawn->ID;
		Spawn->bImmobile = true;
	}
	else if (Barracks)
	{
		// The barracks' exitObjectViaDoor: out along its exit path (and its door is busy for its ExitDelay).
		Logic->FollowExitPath(*Barracks, *Spawn, Rally);
		if (Barracks->Template->bQueueExit)
		{
			Barracks->ExitDelayLeft = Barracks->Template->ProductionExitDelay;
			Barracks->ExitBurstLeft = FMath::Max(0, Barracks->ExitBurstLeft - 1);
		}
		--SpawnInitialBurstLeft;
	}
	else if (D.bExitByBudding)
	{
		// exitObjectByBudding: "This causes the formation to break like billard balls" - off the spot it shares.
		Spawn->CommandMoveTo(Spawn->Position);
		Spawn->LastAICommandFrame = Logic->GetFrame() + 1;
		if (Template->bQueueExit)
		{
			ExitDelayLeft = Template->ProductionExitDelay;
			ExitBurstLeft = FMath::Max(0, ExitBurstLeft - 1);
		}
	}
	SpawnCount = SpawnCount < 0 ? 1 : SpawnCount + 1;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [spawn f%u] #%u %s makes #%u %s at (%.0f, %.0f) %s (%d of %d)"), Logic->GetFrame(), ID, *Template->Name,
		Spawn->ID, *What->Name, Pos.X, Pos.Y, Barracks ? TEXT("by the barracks' door") : BudHost ? *FString::Printf(TEXT("budded from #%u"), BudHost->ID) : TEXT(""),
		Spawns.Num(), D.Number);
	return true;
}

void FGenObject::OnSpawnDeath(FGenObjectID Spawn, FGenObjectID Killer)
{
	// SpawnBehavior::onSpawnDeath: "When one dies, you push (now + delay) as the time a new one should be made".
	if (Spawns.Remove(Spawn) == 0)
	{
		return;
	}
	SpawnReplaceFrames.Add(Logic->GetFrame() + uint32(Template->Spawner.ReplaceFrames));
	--SpawnCount;
	if (SpawnCount == 0 && Template->Spawner.bAggregateHealth)
	{
		// "I'm dead without my spawn": whoever killed the last of them scores us (killer->scoreTheKill).
		Logic->OnObjectKilled(*this, Killer);
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [spawn f%u] #%u %s: the last of them is gone, so is it"), Logic->GetFrame(), ID, *Template->Name);
		bPendingDestroy = true;
	}
}

void FGenObject::ComputeAggregateStates()
{
	// SpawnBehavior::computeAggregateStates: "Health for the spawner is calc'd by aggregating the sum of all spawn
	// health, and dividing by optimal health of full population ... at full health"; veterancy goes to whoever has
	// less; "The HealthBoxPosition ... is calc'd as an average position of all the spawn". (The selection, "ONE BIG
	// HAPPY MOB", is AGenPlayerController's; "make sure no enemies are shooting at the nexus" is IsTargetable's.)
	int32 Count = 0;
	FVector3f AvgPos = FVector3f::ZeroVector;
	float AccHealth = 0.f;
	float AvgHealthMax = 0.f;
	SelfTaskingSpawns = 0;
	for (FGenObjectID Id : Spawns)
	{
		FGenObject* S = Logic->FindObject(Id);
		if (!S)
		{
			continue;
		}
		SelfTaskingSpawns += S->bMobSelfTasking ? 1 : 0;
		if (S->VeterancyLevel > VeterancyLevel)
		{
			SetVeterancyLevel(S->VeterancyLevel);
		}
		else if (S->VeterancyLevel < VeterancyLevel)
		{
			S->SetVeterancyLevel(VeterancyLevel);
		}
		AvgPos += S->Position;
		AccHealth += S->Health;
		AvgHealthMax += S->MaxHealth;
		++Count;
	}
	HealthBoxOffset = Count > 0 ? AvgPos / float(Count) - Position : FVector3f::ZeroVector;
	// setInitialHealth(100 * actualHealth): a whole percent of it, never below 1 (ImmortalBody).
	const float Ratio = Count > 0 && AvgHealthMax > 0.f ? AccHealth / (AvgHealthMax / float(Count) * float(FMath::Max(1, Template->Spawner.Number))) : 0.f;
	PreviousHealth = Health;
	Health = FMath::Max(1.f, float(int32(100.f * Ratio)) / 100.f * MaxHealth);
}

bool FGenObject::MaySpawnSelfTask(float MaxSelfTaskersRatio) const
{
	// SpawnBehavior::maySpawnSelfTaskAI: "if my last attack command was from player or script, I need to forbid my
	// spawn from disobeying that command"; else a few of them may.
	if (SpawnCount <= 0 || MaxSelfTaskersRatio == 0.f || LastAICommandFrame < LastPlayerOrderFrame)
	{
		return false;
	}
	return float(SelfTaskingSpawns) / float(SpawnCount) < MaxSelfTaskersRatio;
}

void FGenObject::UpdateMobMember(uint32 Frame)
{
	// MobMemberSlavedUpdate::update
	const FGenThingTemplate::FMobMember& D = Template->MobMember;
	FGenObject* Master = Logic->FindObject(MobSlaver);
	if (!Master || Master->IsDead() || Master->IsDestroyed())
	{
		// (onSlaverDie -> stopSlavedEffects: with no nexus, it dies.)
		MobSlaver = GenInvalidObjectID;
		Kill();
		return;
	}
	// Armed (Arm the Mob), the AK that does no damage ("for atttacking in AK-proof conditions") is never seen
	// firing: only the real one is.
	static const int32 UpgradedBit = GenConditions::Index(TEXT("WEAPONSET_PLAYER_UPGRADE"));
	if (ModelConditions.Test(UpgradedBit))
	{
		static const int32 Hidden[] = { GenConditions::Index(TEXT("RELOADING_A")), GenConditions::Index(TEXT("BETWEEN_FIRING_SHOTS_A")),
			GenConditions::Index(TEXT("PREATTACK_A")), GenConditions::Index(TEXT("FIRING_A")), GenConditions::Index(TEXT("USING_WEAPON_A")) };
		for (int32 Bit : Hidden)
		{
			SetModelCondition(Bit, false);
		}
	}
	if (++MobFramesToWait < 16)
	{
		return;
	}
	MobFramesToWait = 0;
	if (!Locomotor)
	{
		return;
	}
	const uint32 AIFrame = Frame + 1;
	FGenObject* Victim = Logic->FindObject(TargetID);
	if (Master->TargetID != GenInvalidObjectID && Logic->FindObject(Master->TargetID))
	{
		MobPrimaryVictim = Master->TargetID;
	}
	FGenObject* PrimaryVictim = Logic->FindObject(MobPrimaryVictim);
	if (PrimaryVictim && (PrimaryVictim->IsDead() || PrimaryVictim->IsDestroyed()))
	{
		PrimaryVictim = nullptr;
	}
	const float CatchUpDistSq = FVector3f::DistSquared(Position, Master->Position);
	if (CatchUpDistSq > FMath::Square(D.MustCatchUpRadius))
	{
		// "I'm too far from the nexus... I need to catch up now!"
		if (Master->bMoving)
		{
			// Getting ahead of it: slow down; lagging: "snap to it!"
			ChooseLocomotorSet(Master->GetDistanceToGoal() > GetDistanceToGoal() ? TEXT("SET_WANDER") : TEXT("SET_PANIC"));
			const FVector3f NuPos = Master->Goal;
			if (NuPos.Size() < 1.f)
			{
				CommandMoveTo(Master->Position); // "if a nasty error has sent me to map origin"
				LastAICommandFrame = AIFrame;
			}
			else if (!bMoving || FVector3f::Dist(Goal, NuPos) > 5.f * GenLogic::PathfindCellSize)
			{
				CommandMoveTo(NuPos); // "Whither thou goest..." - the mob reconverges on the way
				LastAICommandFrame = AIFrame;
			}
		}
		else
		{
			// "master is still, so let's re group in a hurry"
			ChooseLocomotorSet(TEXT("SET_PANIC"));
			CommandMoveTo(Master->Position);
			LastAICommandFrame = AIFrame;
		}
		if (CatchUpDistSq > FMath::Square(D.MustCatchUpRadius * 3.f))
		{
			// "I am critically far, now!"
			if (++MobCatchUpCrisisTimer > D.CatchUpCrisisBailTime)
			{
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mob f%u] #%u %s is too far from its nexus for too long: it dies"), Frame, ID, *Template->Name);
				Kill();
				return;
			}
			else if (MobCatchUpCrisisTimer > D.CatchUpCrisisBailTime / 3)
			{
				CommandMoveTo(Master->Position); // "NASTY BEEHIVE EFFECT"
				LastAICommandFrame = AIFrame;
			}
		}
	}
	else if (bMoving)
	{
		// "we're all on a trip, together": a change of pace now and then.
		MobCatchUpCrisisTimer = 0;
		const int32 Seed = Logic->GetRandom().RandRange(0, 10);
		if (Seed == 1)
		{
			ChooseLocomotorSet(TEXT("SET_WANDER"));
		}
		else if (Seed == 2)
		{
			ChooseLocomotorSet(TEXT("SET_PANIC"));
		}
		else if (Seed == 3)
		{
			ChooseLocomotorSet(TEXT("SET_NORMAL"));
		}
	}
	else
	{
		// "give me something to do while I'm standing here..."
		MobCatchUpCrisisTimer = 0;
		if (Master->IsAIIdle())
		{
			// "if controlling player has pressed stop, we stop! That's it!"
			CommandIdle();
			MobPrimaryVictim = GenInvalidObjectID;
			return;
		}
		if (Master->MaySpawnSelfTask(D.Squirrelliness) && LastAICommandFrame < LastPlayerOrderFrame)
		{
			// "I may have been told to attack directly more recently": somebody else around to attack.
			FGenObject* NewTarget = FindAutoTarget();
			if (NewTarget && NewTarget != Victim)
			{
				Victim = NewTarget;
				CommandAttack(NewTarget->ID);
				LastAICommandFrame = AIFrame;
				bMobSelfTasking = true;
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mob f%u] #%u %s picks a target of its own: #%u %s"), Frame, ID, *Template->Name, NewTarget->ID, *NewTarget->Template->Name);
			}
		}
		if (!Victim)
		{
			if (PrimaryVictim)
			{
				CommandAttack(PrimaryVictim->ID); // "I remember the last target"
				LastAICommandFrame = AIFrame;
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [mob f%u] #%u %s takes up the nexus's target #%u %s"), Frame, ID, *Template->Name, PrimaryVictim->ID,
					*PrimaryVictim->Template->Name);
			}
			bMobSelfTasking = false;
		}
	}
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

bool FGenObject::AllSpawnsAllowedToStealth(uint32 Frame)
{
	// SpawnBehavior::areAllSlavesStealthed: the ones that have gone are not counted, and having none counts as
	// all being stealthed.
	bool bAll = true;
	for (FGenObjectID Id : Spawns)
	{
		FGenObject* S = Logic->FindObject(Id);
		if (!S || S->IsDead() || S->IsDestroyed())
		{
			continue;
		}
		if (!S->GetTemplate()->Stealth.bValid || !S->AllowedToStealth(Frame))
		{
			bAll = false;
			break;
		}
	}
	if (!bAll)
	{
		// "If any of the slaves can't stealth, then reveal everyone!" Each is shown up for its own stealth
		// delay, which is the same time the building itself is held out of invisibility.
		for (FGenObjectID Id : Spawns)
		{
			FGenObject* S = Logic->FindObject(Id);
			if (S && !S->IsDead() && !S->IsDestroyed() && S->GetTemplate()->Stealth.bValid)
			{
				S->MarkAsDetected(0);
			}
		}
	}
	return bAll;
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
