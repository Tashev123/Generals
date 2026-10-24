// Crates: what the dead leave behind (CreateCrateDie with Crate.ini's CrateData - the crate system),
// and what a crate does for whoever touches it (CrateCollide and the modules built on it:
// MoneyCrateCollide, SalvageCrateCollide, UnitCrateCollide).
//
// In the game's data only one kind is ever left behind: the salvage crate - by anything a GLA
// player's "salvager" kills (the lines that gave veteran tanks their money and level-up crates are
// commented out, and VeterancyCrateCollide does nothing for a crate that is no pilot: it asks the
// crate's own AI where it is going). A salvage crate is seen and picked up by GLA players only. To a
// salvager it is, in this order: better armour (ARMOR_SALVAGER: the battle bus, twice), a better weapon
// (WEAPON_SALVAGER: the technical's cannon and then its rockets, the Marauder's second gun...), with
// LevelChance a rank, or else some money.
// The original also tells a computer player's unit of the crate its kill left - and forgets the
// crate's id before looking it up (AIUpdateInterface::update), so no computer unit ever goes for one:
// they pick up what they happen to drive over, as here.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Logic/GenUpgrades.h"
#include "Audio/GenAudioData.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	int32 FindNeutralPlayer(const UGenGameLogic& Logic)
	{
		const TArray<FGenPlayer>& Players = Logic.GetPlayers();
		for (int32 i = 0; i < Players.Num(); ++i)
		{
			if (Players[i].bNeutral)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}
}

void FGenObject::CreateCrates(const FGenDieModule& Module, FGenObjectID KillerId)
{
	// CreateCrateDie::onDie
	const FGenGameData& GD = FGenGameData::Get();
	FRandomStream& Rand = Logic->GetRandom();
	const FGenObject* Killer = Logic->FindObject(KillerId);
	if (Killer && Logic->GetRelationship(Killer->Owner, Owner) == EGenRelationship::Allies)
	{
		return; // "Nope, no crate for killing ally at all."
	}
	for (const FString& Name : Module.CrateData)
	{
		const FGenCrateData* C = GD.FindCrateData(Name);
		if (!C)
		{
			continue;
		}
		if (!(Rand.FRand() < C->CreationChance)) // "always test this"
		{
			continue;
		}
		if (C->VeterancyLevel != INDEX_NONE && C->VeterancyLevel != VeterancyLevel)
		{
			continue;
		}
		if (C->KilledByKindOf.Num() > 0)
		{
			// "Must match the whole group of bits set in the KilledBy description."
			bool bKinds = Killer != nullptr;
			for (const FString& Kind : C->KilledByKindOf) { bKinds = bKinds && Killer->IsKindOf(*Kind); }
			if (!bKinds)
			{
				continue;
			}
		}
		if (!C->KillerScience.IsEmpty() && !(Killer && Logic->HasScience(Killer->Owner, C->KillerScience)))
		{
			continue;
		}

		// createCrate: "CreationChance is used for the success of this block, but this block can have any
		// number of potential actual crates" - a weighted pick, which comes to nothing when the shares
		// don't add up to one.
		const float Pick = Rand.FRand();
		float Total = 0.f;
		const FGenThingTemplate* CrateT = nullptr;
		for (const TPair<FString, float>& Possible : C->CrateObjects)
		{
			Total += Possible.Value;
			if (Total > Pick)
			{
				CrateT = GD.FindTemplate(Possible.Key);
				break;
			}
		}
		const int32 CrateOwner = C->bOwnedByMaker ? Owner : FindNeutralPlayer(*Logic);
		if (!CrateT || CrateOwner == INDEX_NONE)
		{
			continue;
		}
		// Where the dead thing is ("so the dead guy won't block, nor will his dead hulk") - or, by "a great
		// big scan", at the edge of a dead building.
		FVector3f Spot = Position;
		if (Layer == 0)
		{
			Spot = Logic->FindGroundSpotAround(Position, 0.f, IsKindOf(TEXT("STRUCTURE")) ? 125.f : 5.f);
		}
		if (FGenObject* Crate = Logic->CreateObject(CrateT, CrateOwner, Spot, Rand.FRandRange(0.f, 2.f * PI)))
		{
			Crate->Layer = Layer;
			Crate->SnapshotPrevious();
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [crate f%u] #%u %s leaves #%u %s at (%.0f, %.0f) (killed by #%u %s)"), Logic->GetFrame(), ID,
				*Template->Name, Crate->ID, *CrateT->Name, Spot.X, Spot.Y, KillerId, Killer ? *Killer->Template->Name : TEXT("nobody"));
		}
	}
}

void FGenObject::UpdateCrate(uint32 Frame)
{
	// CrateCollide::onCollide / isValidToExecute, for whatever is touching the crate (the first made of
	// them, when there are several).
	GEN_PERF_SCOPE("Obj.Crate");
	const FGenCrateCollide& D = Template->Crate;
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const bool bAboveTerrain = Position.Z - Logic->GetGroundHeight(Position.X, Position.Y) > 0.5f;
	FGenObject* Taker = nullptr;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), Template->GetBoundingCircleRadius() + 4.f, [&](FGenObject& O)
	{
		if (&O == this || O.bDead || O.bPendingDestroy || (Taker && Taker->ID < O.ID) || !Players.IsValidIndex(O.Owner) || Players[O.Owner].bNeutral)
		{
			return; // ("Nothing Neutral can pick up any type of crate")
		}
		const bool bBuilding = D.bBuildingPickup && O.IsKindOf(TEXT("STRUCTURE"));
		if ((!O.Template->bHasAI && !bBuilding) || (bAboveTerrain && !bBuilding) || O.IsKindOf(TEXT("PARACHUTE")) || O.IsContained()
			|| (D.bForbidOwnerPlayer && O.Owner == Owner) || (D.bHumanOnly && !Players[O.Owner].bHuman)
			|| (!D.PickupScience.IsEmpty() && !Logic->HasScience(O.Owner, D.PickupScience))
			|| (D.Type == EGenCrate::Salvage && !O.IsKindOf(TEXT("SALVAGER")))) // "Only salvage units can pick up a Salvage crate"
		{
			return;
		}
		for (const FString& Kind : D.RequiredKindOf) { if (!O.IsKindOf(*Kind)) { return; } }
		for (const FString& Kind : D.ForbiddenKindOf) { if (O.IsKindOf(*Kind)) { return; } }
		if (GeometryOverlaps(O, 1.f))
		{
			Taker = &O;
		}
	});
	if (!Taker || !ExecuteCrate(*Taker, Frame))
	{
		return;
	}
	if (!D.ExecuteFX.IsEmpty())
	{
		Logic->DoFXObj(D.ExecuteFX, *Taker); // ("we pass in other here, because the crate is owned by the neutral player")
	}
	bPendingDestroy = true;
}

bool FGenObject::ExecuteCrate(FGenObject& Other, uint32 Frame)
{
	const FGenCrateCollide& D = Template->Crate;
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const FGenAudioData& Sounds = FGenAudioData::Get();
	FRandomStream& Rand = Logic->GetRandom();
	auto GiveMoney = [&](int32 Money)
	{
		Logic->AddMoney(Other.Owner, Money);
		Logic->ScoreMoneyEarned(Other.Owner, Money);
		Logic->PlaySoundAt(Sounds.GetMisc(TEXT("CrateMoney")), Other);
		// (the original shows its "MoneyPickUp" picture rising from a money crate; here the sum does)
		Logic->AddFloatingText(FString::Printf(TEXT("+$%d"), Money), Position + FVector3f(0.f, 0.f, 10.f), Players[Other.Owner].Color);
	};
	FString What;
	switch (D.Type)
	{
	case EGenCrate::Money:
	{
		// MoneyCrateCollide::executeCrateBehavior
		int32 Money = D.Money;
		for (const TPair<FString, int32>& Up : D.UpgradeBoost)
		{
			if (Logic->HasUpgradeComplete(Other.Owner, Up.Key))
			{
				Money += Up.Value;
				break;
			}
		}
		GiveMoney(Money);
		What = FString::Printf(TEXT("$%d"), Money);
		break;
	}
	case EGenCrate::Salvage:
	{
		// SalvageCrateCollide::executeCrateBehavior
		static const int32 ArmorBits[2] = { GenConditions::Index(TEXT("ARMORSET_CRATEUPGRADE_ONE")), GenConditions::Index(TEXT("ARMORSET_CRATEUPGRADE_TWO")) };
		static const int32 WeaponBits[2] = { GenConditions::Index(TEXT("WEAPONSET_CRATEUPGRADE_ONE")), GenConditions::Index(TEXT("WEAPONSET_CRATEUPGRADE_TWO")) };
		static const FString WeaponFlags[2] = { TEXT("CRATEUPGRADE_ONE"), TEXT("CRATEUPGRADE_TWO") };
		if (Other.IsKindOf(TEXT("ARMOR_SALVAGER")) && Other.CrateArmorLevel < 2)
		{
			// "No percent chance on this one, if you can get it, you get it."
			++Other.CrateArmorLevel;
			Other.RefreshArmorSet();
			Other.SetModelCondition(ArmorBits[0], Other.CrateArmorLevel == 1);
			Other.SetModelCondition(ArmorBits[1], Other.CrateArmorLevel == 2);
			Logic->PlaySoundAt(Sounds.GetMisc(TEXT("CrateSalvage")), Other);
			What = FString::Printf(TEXT("armour %d"), int32(Other.CrateArmorLevel));
		}
		else if (Other.IsKindOf(TEXT("WEAPON_SALVAGER")) && Other.CrateWeaponLevel < 2 && (D.WeaponChance >= 1.f || Rand.FRand() < D.WeaponChance))
		{
			++Other.CrateWeaponLevel;
			Other.SetFlags.Remove(WeaponFlags[0]);
			Other.SetFlags.AddUnique(WeaponFlags[Other.CrateWeaponLevel - 1]);
			Other.RefreshWeaponSet();
			Other.SetModelCondition(WeaponBits[0], Other.CrateWeaponLevel == 1);
			Other.SetModelCondition(WeaponBits[1], Other.CrateWeaponLevel == 2);
			Logic->PlaySoundAt(Sounds.GetMisc(TEXT("CrateSalvage")), Other);
			What = FString::Printf(TEXT("weapon %d (%s)"), int32(Other.CrateWeaponLevel), Other.Weapons[0].Template ? *Other.Weapons[0].Template->Name : TEXT("-"));
		}
		else if (Other.Template->bIsTrainable && Other.VeterancyLevel < 3 && (D.LevelChance >= 1.f || Rand.FRand() < D.LevelChance))
		{
			Other.SetVeterancyLevel(Other.VeterancyLevel + 1); // ExperienceTracker::gainExpForLevel(1)
			What = FString::Printf(TEXT("rank %d"), Other.VeterancyLevel);
		}
		else
		{
			const int32 Money = D.MinMoney != D.MaxMoney ? Rand.RandRange(FMath::Min(D.MinMoney, D.MaxMoney), FMath::Max(D.MinMoney, D.MaxMoney)) : D.MinMoney;
			if (Money > 0)
			{
				GiveMoney(Money);
			}
			What = FString::Printf(TEXT("$%d"), Money);
		}
		break;
	}
	case EGenCrate::Unit:
	{
		// UnitCrateCollide::executeCrateBehavior
		const FGenThingTemplate* UnitT = FGenGameData::Get().FindTemplate(D.UnitName);
		if (!UnitT)
		{
			return false;
		}
		for (int32 i = 0; i < D.UnitCount; ++i)
		{
			const FVector3f Spot = Logic->FindGroundSpotAround(Other.Position, 0.f, 20.f);
			if (FGenObject* Unit = Logic->CreateObject(UnitT, Other.Owner, Spot, Other.Orientation))
			{
				Unit->SnapshotPrevious();
			}
		}
		Logic->PlaySoundAt(Sounds.GetMisc(TEXT("CrateFreeUnit")), Other);
		What = FString::Printf(TEXT("%d %s"), D.UnitCount, *D.UnitName);
		break;
	}
	default:
		return false;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [crate f%u] #%u %s collected by #%u p%d %s: %s"), Frame, ID, *Template->Name, Other.ID, Other.Owner,
		*Other.Template->Name, *What);
	return true;
}
