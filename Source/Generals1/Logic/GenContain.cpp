// Garrisons and transports: units inside other objects.
// Follows OpenContain.cpp, TransportContain.cpp, GarrisonContain.cpp, HealContain.cpp, TunnelContain.cpp and
// TunnelTracker.cpp, ActionManager::canEnterObject, AIEnterState / AIExitState, and the part of
// ChinookAIUpdate.cpp that lands the helicopter for its passengers.

#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenPathfinder.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

namespace
{
	constexpr int32 MaxGarrisonPoints = 40;      // MAX_GARRISON_POINTS
	constexpr float ChinookLandedHeight = 3.f;   // ChinookTakeoffOrLandingState: close enough to the goal height

	int32 LoadedBit() { static const int32 I = GenConditions::Index(TEXT("LOADED")); return I; }
	int32 GarrisonedBit() { static const int32 I = GenConditions::Index(TEXT("GARRISONED")); return I; }
	const FString GarrisonedBonus(TEXT("GARRISONED"));
	const FString RiderUpgradeFlag(TEXT("PLAYER_UPGRADE"));

	bool HasAnyKind(const FGenThingTemplate& T, const TArray<FString>& Kinds)
	{
		for (const FString& K : Kinds)
		{
			if (T.KindOf.Contains(K))
			{
				return true;
			}
		}
		return false;
	}

	/** RiderChangeContain: the line of Rider1..8 for that soldier ("thing->isEquivalentTo( rider->getTemplate() )"). */
	const FGenContainData::FRider* FindRiderType(const FGenContainData& D, const FGenThingTemplate& T)
	{
		for (const FGenContainData::FRider& R : D.RiderTypes)
		{
			const FGenThingTemplate* RT = FGenGameData::Get().FindTemplate(R.Template);
			if (RT && RT->Canonical() == T.Canonical())
			{
				return &R;
			}
		}
		return nullptr;
	}

	/** Object::isFactionStructure: a base building of one of the sides (KindOf FS_...). */
	bool IsSideStructure(const FGenThingTemplate& T)
	{
		for (const FString& K : T.KindOf)
		{
			if (K.StartsWith(TEXT("FS_")))
			{
				return true;
			}
		}
		return false;
	}

	/** Distance from a point to an object's footprint (0 inside). */
	float GapToFootprint(const FGenObject& Obj, const FVector3f& P)
	{
		const FGenThingTemplate* T = Obj.GetTemplate();
		const float DX = P.X - Obj.GetPosition().X, DY = P.Y - Obj.GetPosition().Y;
		if (!T->IsBox())
		{
			return FMath::Max(0.f, FMath::Sqrt(DX * DX + DY * DY) - T->GeometryMajorRadius);
		}
		const float C = FMath::Cos(Obj.GetOrientation()), S = FMath::Sin(Obj.GetOrientation());
		const float OX = FMath::Max(0.f, FMath::Abs(DX * C + DY * S) - T->GeometryMajorRadius);
		const float OY = FMath::Max(0.f, FMath::Abs(-DX * S + DY * C) - T->GeometryMinorRadius);
		return FMath::Sqrt(OX * OX + OY * OY);
	}
}

// ---- who may go where ---------------------------------------------------------------------------

const FGenObject& UGenGameLogic::ResolveContainer(const FGenObject& Container) const
{
	if (Container.Contain && Container.GetTemplate()->Contain.bOverlord)
	{
		const FGenObject* Rider = FindObject(Container.Contain->Portable);
		if (Rider && Rider->Contain && !Rider->IsDead() && !Rider->IsDestroyed())
		{
			return *Rider;
		}
	}
	return Container;
}

FGenObject* UGenGameLogic::GetPortableRider(const FGenObject& Container) const
{
	FGenObject* Rider = Container.Contain ? FindObject(Container.Contain->Portable) : nullptr;
	return (Rider && !Rider->IsDead() && !Rider->IsDestroyed()) ? Rider : nullptr;
}

bool UGenGameLogic::ShowsContainPips(const FGenObject& Container) const
{
	const FGenObject& Real = ResolveContainer(Container);
	if (!Real.Contain || (Container.GetTemplate()->Contain.bOverlord && &Real == &Container))
	{
		return false;
	}
	return Real.GetTemplate()->Contain.bShouldDrawPips && GetContainMax(Real) > 0;
}

const TArray<FGenObjectID>& UGenGameLogic::GetRiders(const FGenObject& Asked) const
{
	static const TArray<FGenObjectID> None;
	const FGenObject& Container = ResolveContainer(Asked);
	if (!Container.Contain)
	{
		return None;
	}
	if (Container.GetTemplate()->Contain.Type == EGenContainType::Tunnel)
	{
		return Players.IsValidIndex(Container.GetOwner()) ? Players[Container.GetOwner()].TunnelRiders : None;
	}
	return Container.Contain->Riders;
}

int32 UGenGameLogic::GetContainMax(const FGenObject& Asked) const
{
	const FGenContainData& D = ResolveContainer(Asked).GetTemplate()->Contain;
	return D.Type == EGenContainType::Tunnel ? FGenGameData::Get().MaxTunnelCapacity : D.ContainMax;
}

int32 UGenGameLogic::GetContainCount(const FGenObject& Asked) const
{
	const FGenObject& Container = ResolveContainer(Asked);
	return GetRiders(Container).Num() + (Container.Contain ? Container.Contain->ExtraSlotsInUse : 0);
}

bool UGenGameLogic::IsValidContainerFor(const FGenObject& Container, const FGenObject& Rider, bool bCheckCapacity) const
{
	const FGenContainData& D = Container.GetTemplate()->Contain;
	const FGenThingTemplate& RT = *Rider.GetTemplate();
	if (D.Type == EGenContainType::None || !Container.Contain)
	{
		return false;
	}
	const FGenObject& Real = ResolveContainer(Container);
	if (&Real != &Container)
	{
		return IsValidContainerFor(Real, Rider, bCheckCapacity); // (an Overlord with a bunker on it takes what the bunker takes)
	}
	if ((D.bHelix || D.bOverlord) && RT.IsKindOf(TEXT("PORTABLE_STRUCTURE")))
	{
		// HelixContain::isValidContainerFor (and an Overlord's one slot): there is room for one add-on.
		return Rider.GetOwner() == Container.GetOwner() && !GetPortableRider(Container);
	}
	// OpenContain::isValidContainerFor: the kinds let in, and how the rider stands to the container.
	if ((D.AllowInsideKindOf.Num() > 0 && !HasAnyKind(RT, D.AllowInsideKindOf)) || HasAnyKind(RT, D.ForbidInsideKindOf))
	{
		return false;
	}
	const EGenRelationship Rel = GetRelationship(Rider.GetOwner(), Container.GetOwner());
	if ((Rel == EGenRelationship::Allies && !D.bAllowAllies) || (Rel == EGenRelationship::Enemies && !D.bAllowEnemies)
		|| (Rel == EGenRelationship::Neutral && !D.bAllowNeutral))
	{
		return false;
	}
	switch (D.Type)
	{
	case EGenContainType::Transport:
		// Only our own transports, and only what takes up room in one.
		if (Rider.GetOwner() != Container.GetOwner() || RT.TransportSlotCount == 0)
		{
			return false;
		}
		if (D.bRiderChange)
		{
			// RiderChangeContain::isValidContainerFor: "Don't check capacity because our rider will kick the other rider
			// out!" - only the soldiers its Rider lines name, and not once it is being scuttled.
			return Container.ScuttleFrame == 0 && FindRiderType(D, RT) != nullptr;
		}
		return !bCheckCapacity || GetContainCount(Container) + RT.TransportSlotCount <= D.ContainMax;
	case EGenContainType::Garrison:
		// A building beaten down to "really damaged" can no longer be held.
		if (Container.GetHealth() <= 0.f || (Container.GetDamageState() >= 2 && !Container.IsKindOf(TEXT("GARRISONABLE_UNTIL_DESTROYED")))
			|| RT.IsKindOf(TEXT("NO_GARRISON")))
		{
			return false;
		}
		return !bCheckCapacity || GetRiders(Container).Num() < D.ContainMax;
	case EGenContainType::Tunnel:
		// (TunnelTracker: everything but aircraft may use the tunnels.)
		if (RT.IsKindOf(TEXT("AIRCRAFT")))
		{
			return false;
		}
		return !bCheckCapacity || GetRiders(Container).Num() < GetContainMax(Container);
	default:
		return !bCheckCapacity || GetRiders(Container).Num() < D.ContainMax;
	}
}

bool UGenGameLogic::CanEnterObject(const FGenObject& Unit, const FGenObject& Container, bool bCheckCapacity) const
{
	// ActionManager::canEnterObject
	if (&Unit == &Container || Container.IsDead() || Container.IsDestroyed() || Unit.IsDead() || Unit.IsContained())
	{
		return false;
	}
	if (Unit.IsUnderConstruction() || Container.IsUnderConstruction() || Container.IsBeingSold())
	{
		return false;
	}
	// (A unit also "enters" what one of its collide modules would like to collide with: a pilot climbs into
	// a vehicle by his VeterancyCrateCollide.)
	if (Unit.CanPilotEnter(Container))
	{
		return true;
	}
	// "Special case for unmanned vehicles. Any infantry unit can take over any unmanned vehicle!"
	if (Unit.IsKindOf(TEXT("INFANTRY")) && Container.IsUnmanned() && !Unit.IsKindOf(TEXT("REJECT_UNMANNED")))
	{
		return true;
	}
	// (A hijacker's or a terrorist's way into a vehicle: the enter order is how the original sends them.)
	if (CanTakeOverVehicle(Unit, Container))
	{
		return true;
	}
	// (... and a saboteur's into an enemy building.)
	if (CanSabotage(Unit, Container))
	{
		return true;
	}
	if (Unit.IsKindOf(TEXT("IGNORED_IN_GUI")) || Unit.IsKindOf(TEXT("MOB_NEXUS")) || Container.IsKindOf(TEXT("IGNORED_IN_GUI")))
	{
		return false;
	}
	if (Unit.IsKindOf(TEXT("STRUCTURE")) || Unit.IsKindOf(TEXT("IMMOBILE")) || !Unit.CanMove())
	{
		return false; // structures don't garrison
	}
	const FGenContainData& D = Container.GetTemplate()->Contain;
	if (!Container.GetContain() || D.Type == EGenContainType::None)
	{
		return false;
	}
	if (D.Type == EGenContainType::Heal && Unit.GetHealth() >= Unit.GetMaxHealth())
	{
		return false; // a barracks takes in the wounded only
	}
	// Not ours: only when nobody is in it, and never one of a side's base buildings.
	if (Container.GetOwner() != Unit.GetOwner() && (GetRiders(Container).Num() > 0 || IsSideStructure(*Container.GetTemplate())))
	{
		return false;
	}
	if (bCheckCapacity && Unit.GetTemplate()->TransportSlotCount == 0)
	{
		return false;
	}
	return IsValidContainerFor(Container, Unit, bCheckCapacity);
}

bool UGenGameLogic::CanLandAtAirfield(const FGenObject& Unit, const FGenObject& Airfield) const
{
	// ActionManager::canEnterObject's special case for aircraft: an airfield takes in only a craft of the same
	// player, and only one that has a place there. A craft built on the helipad is never given one, so a healthy
	// helicopter is not taken in at all - a damaged one comes to the airfield through the repair path instead, and
	// takes off again as soon as it is mended.
	if (!Unit.IsKindOf(TEXT("AIRCRAFT")) || !Airfield.IsKindOf(TEXT("FS_AIRFIELD")))
	{
		return false;
	}
	const FGenParkingPlace* PP = Airfield.GetParking();
	if (!PP || Unit.GetOwner() != Airfield.GetOwner())
	{
		return false;
	}
	const FVector3f& P = Unit.GetPosition();
	if (P.Z - GetGroundHeight(P.X, P.Y) <= 0.f)
	{
		return false; // isAboveTerrain: only a craft in the air is asked to come down
	}
	return PP->HasReservedSpace(Unit.GetID())
		|| (PP->ShouldReserveDoorWhenQueued(*Unit.GetTemplate()) && PP->HasAvailableSpaceFor(*Unit.GetTemplate()));
}

bool UGenGameLogic::IsPassengerAllowedToFire(const FGenObject& Container, const FGenObject& Rider) const
{
	const FGenContainData& D = Container.GetTemplate()->Contain;
	if (!Container.Contain || Container.IsDisabled())
	{
		return false;
	}
	const bool bInfantry = Rider.IsKindOf(TEXT("INFANTRY"));
	const FGenObject* Carrier = Container.IsContained() ? FindObject(Container.GetContainedBy()) : nullptr;
	if (D.bHelix)
	{
		// HelixContain: "nested containment voids firing, always"; "riders are always allowed to fire
		// (gattling cannons)"; "passengers (proper) may only if the flag is true" - and only the soldiers.
		if (Carrier)
		{
			return false;
		}
		return Container.Contain->Portable == Rider.GetID() || (bInfantry && Container.Contain->bPassengersFire);
	}
	if (D.bOverlord)
	{
		// OverlordContain: "only allow infantry, and turrets and such. no vehicles."
		return !Carrier && (bInfantry || Rider.IsKindOf(TEXT("PORTABLE_STRUCTURE"))) && Container.Contain->bPassengersFire;
	}
	if (D.Type == EGenContainType::Transport)
	{
		if (!bInfantry)
		{
			return false;
		}
		// "But wait! I may be riding on an Overlord": then it is the Overlord's say.
		if (Carrier && Carrier->Contain && (Carrier->GetTemplate()->Contain.bOverlord || Carrier->GetTemplate()->Contain.bHelix))
		{
			return IsPassengerAllowedToFire(*Carrier, Rider);
		}
	}
	// OpenContain::isPassengerAllowedToFire: "if we are ourselves contained, our passengers need to check
	// with them if they get past us".
	return Container.Contain->bPassengersFire && (!Carrier || IsPassengerAllowedToFireFrom(*Carrier));
}

bool UGenGameLogic::CanSnipeVehicle(const FGenObject& Sniper, const FGenObject& Target) const
{
	return !Target.IsDead() && GetRelationship(Sniper.GetOwner(), Target.GetOwner()) == EGenRelationship::Enemies && Target.IsKindOf(TEXT("VEHICLE"))
		&& !Target.IsKindOf(TEXT("DRONE")) && !Target.IsAirborneTarget() && !Target.IsUnmanned();
}

int32 UGenGameLogic::GetNeutralPlayer() const
{
	for (int32 i = 0; i < Players.Num(); ++i)
	{
		if (Players[i].bNeutral)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

bool UGenGameLogic::IsClearableGarrison(const FGenObject& Building) const
{
	const FGenContainData& D = Building.GetTemplate()->Contain;
	return Building.Contain && D.Type == EGenContainType::Garrison && !D.bImmuneToClearBuildingAttacks && GetContainCount(Building) > 0;
}

int32 UGenGameLogic::KillGarrisoned(FGenObject& Building, int32 Count, const TArray<FString>& RequiredKindOf, const TArray<FString>& ForbiddenKindOf, FGenObjectID Killer)
{
	// DumbProjectileBehavior::projectileHandleCollision / ActiveBody::attemptDamage (DAMAGE_KILL_GARRISONED):
	// "garrisonable buildings subvert the normal process here".
	if (!IsClearableGarrison(Building))
	{
		return 0;
	}
	int32 Killed = 0;
	const TArray<FGenObjectID> Inside = GetRiders(Building);
	for (FGenObjectID Id : Inside)
	{
		if (Killed >= Count)
		{
			break;
		}
		FGenObject* R = FindObject(Id);
		if (!R || R->IsDead())
		{
			continue;
		}
		bool bKinds = true;
		for (const FString& K : RequiredKindOf) { bKinds = bKinds && R->IsKindOf(*K); }
		for (const FString& K : ForbiddenKindOf) { bKinds = bKinds && !R->IsKindOf(*K); }
		if (bKinds)
		{
			R->Kill(TEXT("NORMAL"), Killer); // (scoreTheKill: the killer's side is credited)
			++Killed;
		}
	}
	UE_CLOG(bTrace && Killed > 0, LogGenerals, Log, TEXT("  [contain f%u] #%u %s cleared: %d killed inside by #%u (%d left)"), Frame, Building.GetID(),
		*Building.GetTemplate()->Name, Killed, Killer, Inside.Num() - Killed);
	return Killed;
}

void UGenGameLogic::OrderRidersToAttack(const FGenObject& Container, FGenObjectID Target, bool bForce)
{
	if (!IsPassengerAllowedToFireFrom(Container))
	{
		return;
	}
	// (OverlordContain::getContainedItemsList: the bunker's soldiers when it carries a bunker, its add-on otherwise.)
	TArray<FGenObjectID> Who = GetRiders(Container);
	if (Container.GetTemplate()->Contain.bOverlord && &ResolveContainer(Container) == &Container && Container.Contain->Portable != GenInvalidObjectID)
	{
		Who.Add(Container.Contain->Portable);
	}
	for (FGenObjectID Id : Who)
	{
		FGenObject* R = FindObject(Id);
		const FGenObject* In = R ? FindObject(R->GetContainedBy()) : nullptr;
		if (R && In && !R->IsDead() && !R->IsDisabled() && R->HasWeapons() && IsPassengerAllowedToFire(*In, *R))
		{
			R->CommandAttack(Target, bForce);
		}
	}
}

// ---- in and out ---------------------------------------------------------------------------------

namespace
{
	/** TransportContain::letRidersUpgradeWeaponSet: with armed infantry on board the transport uses its upgraded weapons. */
	bool AnyRiderIsArmed(const UGenGameLogic& Logic, const FGenObject& Container)
	{
		for (FGenObjectID Id : Logic.GetRiders(Container))
		{
			const FGenObject* R = Logic.FindObject(Id);
			if (R && R->IsKindOf(TEXT("INFANTRY")) && R->HasWeapons())
			{
				return true;
			}
		}
		return false;
	}
}

void UGenGameLogic::AddToContain(FGenObject& Container, FGenObject& Rider)
{
	// OpenContain::addToContain
	if (!Container.Contain || Rider.IsContained())
	{
		return;
	}
	// OverlordContain::addToContain: with a bunker on it, whoever comes aboard goes into the bunker.
	FGenObject& Real = const_cast<FGenObject&>(ResolveContainer(Container));
	if (&Real != &Container)
	{
		AddToContain(Real, Rider);
		return;
	}
	const FGenContainData& D = Container.GetTemplate()->Contain;
	if ((D.bHelix || D.bOverlord) && Rider.IsKindOf(TEXT("PORTABLE_STRUCTURE")) && !GetPortableRider(Container))
	{
		// HelixContain::addToContain / OverlordContain::onContaining: the add-on takes nobody's seat and
		// rides where everybody sees it ("fool portable into thinking my object is his container").
		Rider.ClearAttack();
		Rider.CommandStop();
		Rider.Velocity = FVector3f::ZeroVector;
		Pathfinder.RemoveUnit(Rider.GetID());
		Container.Contain->Portable = Rider.GetID();
		Rider.ContainedBy = Container.GetID();
		Rider.ContainedFrame = Frame;
		Rider.bWantsExit = false;
		Rider.bHidden = false;
		Rider.GarrisonPoint = INDEX_NONE;
		Rider.SetPosition(Container.GetPosition());
		Rider.SetOrientation(Container.GetOrientation());
		Rider.SnapshotPrevious();
		if (D.bExperienceSinkForRider)
		{
			Rider.XPSink = Container.GetID(); // "this contain style explicitly sucks XP from our little friend"
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [contain f%u] #%u %s rides on #%u %s"), Frame, Rider.GetID(), *Rider.GetTemplate()->Name,
			Container.GetID(), *Container.GetTemplate()->Name);
		return;
	}
	const bool bTunnel = D.Type == EGenContainType::Tunnel;
	if (bTunnel && !Players.IsValidIndex(Container.GetOwner()))
	{
		return;
	}
	// "Whoops, it was mined": a trap on the building goes off under whoever walks in (unless a friend of the trapper's).
	if (Container.CheckAndDetonateBoobyTrap(&Rider) && (Container.IsDead() || Rider.IsDead()))
	{
		return;
	}
	TArray<FGenObjectID>& Riders = bTunnel ? Players[Container.GetOwner()].TunnelRiders : Container.Contain->Riders;
	const FGenContainData::FRider* RiderType = D.bRiderChange ? FindRiderType(D, *Rider.GetTemplate()) : nullptr;
	if (D.bRiderChange && Container.Contain->bPayloadCreated && Riders.Num() > 0)
	{
		// RiderChangeContain::onContaining: "Remove our existing rider" (aiEvacuateInstantly) - it hops off beside the bike.
		TGuardValue<bool> Swapping(Container.Contain->bRiderSwap, true);
		const TArray<FGenObjectID> Old = Riders;
		const float Radius = Container.GetTemplate()->GetBoundingCircleRadius();
		for (FGenObjectID Id : Old)
		{
			if (FGenObject* O = FindObject(Id))
			{
				RemoveFromContain(Container, *O);
				const float Angle = Random.FRandRange(0.f, 2.f * PI);
				const float Dist = Random.FRandRange(Radius, Radius * 1.5f);
				FVector3f Pos(Container.GetPosition().X + Dist * FMath::Cos(Angle), Container.GetPosition().Y + Dist * FMath::Sin(Angle), 0.f);
				Pos.Z = GetGroundHeight(Pos.X, Pos.Y);
				O->CommandMoveTo(Pos);
			}
		}
	}

	// Whatever the rider was doing ends here.
	Rider.StopEntering();
	Rider.ClearAttack();
	Rider.StopDocking();
	Rider.StopConstructing();
	Rider.AIMode = EGenAIMode::None;
	Rider.bHasExitRally = false;
	Rider.CommandStop();
	Rider.Velocity = FVector3f::ZeroVector;
	Pathfinder.RemoveUnit(Rider.GetID());

	Riders.Add(Rider.GetID());
	Container.Contain->Wanters.Remove(Rider.GetID());
	Rider.ContainedBy = Container.GetID();
	Rider.ContainedFrame = Frame;
	Rider.bWantsExit = false;
	Rider.bHidden = true;
	Rider.GarrisonPoint = INDEX_NONE;
	Rider.SetPosition(Container.GetPosition());
	Rider.SnapshotPrevious();

	switch (D.Type)
	{
	case EGenContainType::Transport:
		Container.Contain->ExtraSlotsInUse += Rider.GetTemplate()->TransportSlotCount - 1;
		Container.SetModelCondition(LoadedBit(), true);
		if (D.bHelix)
		{
			Rider.BonusConditions.AddUnique(GarrisonedBonus); // HelixContain::onContaining: "a garrisoned version of its weapon"
		}
		break;
	case EGenContainType::Garrison:
		Rider.BonusConditions.AddUnique(GarrisonedBonus);
		break;
	default:
		break;
	}
	if (RiderType)
	{
		// RiderChangeContain::onContaining: the rider makes the bike what it is - how it looks (and who is drawn on
		// it), its weapon, its status, its buttons and its pace - and brings its rank along.
		Container.SetModelCondition(GenConditions::Index(RiderType->Condition), true);
		Container.SetFlags.AddUnique(RiderType->WeaponFlag);
		Container.RefreshWeaponSet();
		Container.RiderStatus = RiderType->Status;
		Container.CommandSetOverride = RiderType->CommandSet;
		Container.ChooseLocomotorSet(*RiderType->LocomotorSet);
		Container.SetVeterancyLevel(Rider.GetVeterancyLevel(), false);
		Rider.SetVeterancyLevel(0, false);
		AddSelectionHandoff(Rider.GetID(), Container.GetID());
	}
	OnContainChanged(Container);
	if (bLoadSoundsEnabled)
	{
		PlayObjectSound(Container, { TEXT("SoundEnter") });
		// OpenContain::doLoadSound: the container's own EnterSound, not more than once a frame.
		if (Container.Contain->LastLoadSoundFrame != Frame)
		{
			Container.Contain->LastLoadSoundFrame = Frame;
			PlayEventForObject(D.EnterSound, Container);
		}
	}
	if (D.bInternetHack)
	{
		Rider.CommandHackInternet(); // InternetHackContain::onContaining
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [contain f%u] #%u %s enters #%u %s (%d / %d)"), Frame, Rider.GetID(), *Rider.GetTemplate()->Name,
		Container.GetID(), *Container.GetTemplate()->Name, GetContainCount(Container), GetContainMax(Container));
}

void UGenGameLogic::RemoveFromContain(FGenObject& Container, FGenObject& Rider)
{
	// OpenContain::removeFromContain: the rider is back in the world, where the container is.
	if (!Container.Contain)
	{
		return;
	}
	if (Container.Contain->Portable == Rider.GetID())
	{
		// HelixContain::removeFromContain (an add-on never gets off; this is its end)
		Container.Contain->Portable = GenInvalidObjectID;
		Rider.ContainedBy = GenInvalidObjectID;
		return;
	}
	FGenObject& Real = const_cast<FGenObject&>(ResolveContainer(Container));
	if (&Real != &Container)
	{
		RemoveFromContain(Real, Rider);
		return;
	}
	const FGenContainData& D = Container.GetTemplate()->Contain;
	const bool bTunnel = D.Type == EGenContainType::Tunnel;
	TArray<FGenObjectID>& Riders = (bTunnel && Players.IsValidIndex(Container.GetOwner())) ? Players[Container.GetOwner()].TunnelRiders : Container.Contain->Riders;
	if (Riders.Remove(Rider.GetID()) == 0)
	{
		return;
	}
	FGenContain& K = *Container.Contain;
	if (K.FirePointUsers.IsValidIndex(Rider.GarrisonPoint) && K.FirePointUsers[Rider.GarrisonPoint] == Rider.GetID())
	{
		K.FirePointUsers[Rider.GarrisonPoint] = GenInvalidObjectID;
	}
	Rider.GarrisonPoint = INDEX_NONE;
	Rider.ContainedBy = GenInvalidObjectID;
	Rider.bWantsExit = false;
	Rider.bHidden = false;
	Rider.BonusConditions.Remove(GarrisonedBonus);
	FVector3f Pos = Container.GetPosition();
	if (!Rider.IsFlying())
	{
		Pos.Z = GetGroundHeight(Pos.X, Pos.Y);
	}
	Rider.SetPosition(Pos);
	Rider.SetOrientation(Container.GetOrientation());
	Rider.Velocity = FVector3f::ZeroVector;
	Rider.SnapshotPrevious();
	Rider.NextAutoAcquireFrame = 0; // (wakeUpAndAttemptToTarget: looks around as soon as it is out)

	if (D.Type == EGenContainType::Transport)
	{
		K.ExtraSlotsInUse = FMath::Max(0, K.ExtraSlotsInUse - (Rider.GetTemplate()->TransportSlotCount - 1));
		Container.SetModelCondition(LoadedBit(), Riders.Num() > 0);
	}
	if (D.bRiderChange && !Container.IsDead())
	{
		if (const FGenContainData::FRider* R = FindRiderType(D, *Rider.GetTemplate()))
		{
			// RiderChangeContain::onRemoving: what the rider gave the bike goes with it, and so does the rank.
			Container.SetModelCondition(GenConditions::Index(R->Condition), false);
			Container.SetFlags.Remove(R->WeaponFlag);
			Container.RefreshWeaponSet();
			Container.RiderStatus.Reset();
			Rider.SetVeterancyLevel(Container.GetVeterancyLevel(), false);
			Container.SetVeterancyLevel(0, false);
		}
		if (!K.bRiderSwap)
		{
			// "Finally, scuttle the bike so nobody else can use it! <Design Spec>" - it tips over, can't be selected,
			// and (if it was standing) can't be moved; ScuttleDelay later it is gone. The selection goes to the rider.
			Container.ScuttleFrame = FMath::Max(Frame, 1u);
			Container.bUnselectable = true;
			Container.SetModelCondition(GenConditions::Index(D.ScuttleCondition), true);
			if (!Container.bMoving)
			{
				Container.bImmobile = true;
			}
			AddSelectionHandoff(Container.GetID(), Rider.GetID());
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [contain f%u] #%u %s is scuttled: its rider #%u %s got off"), Frame, Container.GetID(),
				*Container.GetTemplate()->Name, Rider.GetID(), *Rider.GetTemplate()->Name);
		}
	}
	OnContainChanged(Container);
	// OpenContain::doUnloadSound: "don't play more than one per frame (we can do meta unload all commands)".
	if (K.LastUnloadSoundFrame != Frame)
	{
		K.LastUnloadSoundFrame = Frame;
		PlayEventForObject(D.ExitSound, Container);
	}
	PlayObjectSound(Container, { TEXT("SoundExit") });
	// ("This is a misnomer": whatever the rider has for falling from a plane is heard on leaving anything.)
	PlayObjectSound(Rider, { TEXT("SoundFallingFromPlane") });
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [contain f%u] #%u %s leaves #%u %s (%d / %d)"), Frame, Rider.GetID(), *Rider.GetTemplate()->Name,
		Container.GetID(), *Container.GetTemplate()->Name, GetContainCount(Container), GetContainMax(Container));
}

void UGenGameLogic::OnContainChanged(FGenObject& Container)
{
	const FGenContainData& D = Container.GetTemplate()->Contain;
	FGenContain& K = *Container.Contain;
	if (D.bArmedRidersUpgradeWeaponSet)
	{
		// TransportContain::letRidersUpgradeWeaponSet
		if (AnyRiderIsArmed(*this, Container)) { Container.SetFlags.AddUnique(RiderUpgradeFlag); }
		else { Container.SetFlags.Remove(RiderUpgradeFlag); }
		Container.RefreshWeaponSet();
	}
	if (D.Type != EGenContainType::Garrison)
	{
		return;
	}
	// GarrisonContain::recalcApparentControllingPlayer: an occupied building belongs to whoever is in it,
	// and goes back to its owner when the last one leaves.
	if (K.OriginalOwner == INDEX_NONE)
	{
		K.OriginalOwner = Container.GetOwner();
	}
	const FGenObject* First = K.Riders.Num() > 0 ? FindObject(K.Riders[0]) : nullptr;
	if (First)
	{
		Container.SetOwner(First->GetOwner());
		Container.SetModelCondition(GarrisonedBit(), true);
	}
	else
	{
		Container.SetOwner(K.OriginalOwner);
		Container.SetModelCondition(GarrisonedBit(), false);
		K.OriginalOwner = INDEX_NONE;
	}
}

bool UGenGameLogic::ExitViaDoor(FGenObject& Container, FGenObject& Rider)
{
	if (!Container.Contain || Rider.GetContainedBy() != Container.GetID())
	{
		return false;
	}
	const FGenContainData& D = Container.GetTemplate()->Contain;
	FGenContain& K = *Container.Contain;
	// TransportContain::isExitBusy / ChinookAIUpdate::getAiFreeToExit
	if (Frame < K.ExitNotBusyFrame)
	{
		return false;
	}
	const bool bAirborne = Container.GetPosition().Z - Container.GetSurfaceHeight() > 9.f;
	if ((Container.GetTemplate()->bChinookAI && Container.GetChinookFlight() != EGenChinookFlight::Landed) || (D.bDelayExitInAir && bAirborne))
	{
		return false;
	}
	RemoveFromContain(Container, Rider);
	K.ExitNotBusyFrame = Frame + uint32(D.ExitDelay);

	const uint32 Surfaces = Rider.GetPathSurfaces();
	// (What rides on something else lets its people out around that: a bunker on an Overlord.)
	const FGenObject* Carrier = Container.IsContained() ? FindObject(Container.GetContainedBy()) : nullptr;
	const float Radius = (Carrier ? Carrier : &Container)->GetTemplate()->GetBoundingCircleRadius();
	FVector3f Start = Rider.GetPosition(), End = Rider.GetPosition();
	bool bHavePath = false;
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	const FGenDrawModule* Draw = Container.GetTemplate()->PrimaryDraw();
	if (D.Type != EGenContainType::Garrison && D.NumberOfExitPaths > 0 && Assets && Draw)
	{
		// OpenContain::exitObjectViaDoor: out along one of the ExitStart / ExitEnd bone pairs, taken in turn.
		FString Suffix;
		if (D.NumberOfExitPaths > 1)
		{
			Suffix = FString::Printf(TEXT("%02d"), K.WhichExitPath);
			K.WhichExitPath = (K.WhichExitPath % D.NumberOfExitPaths) + 1;
		}
		TArray<FTransform3f> StartBone, EndBone;
		if (Assets->GetPristineBones(Draw->DefaultModel(), TEXT("ExitStart") + Suffix, 0, 1, StartBone) > 0
			&& Assets->GetPristineBones(Draw->DefaultModel(), TEXT("ExitEnd") + Suffix, 0, 1, EndBone) > 0)
		{
			const FTransform3f Xf = Container.GetTransform();
			Start = Xf.TransformPositionNoScale(StartBone[0].GetTranslation());
			End = Xf.TransformPositionNoScale(EndBone[0].GetTranslation());
			bHavePath = true;
		}
	}
	if (!bHavePath)
	{
		// GarrisonContain::exitObjectViaDoor (bursting out of the middle), and what has no exit bones:
		// to free ground just outside.
		End = FindPositionAround(Container.GetPosition(), Radius + 5.f, Radius * 1.5f + 30.f, Surfaces);
	}
	if (!Rider.IsFlying())
	{
		Start.Z = GetGroundHeight(Start.X, Start.Y);
		End.Z = GetGroundHeight(End.X, End.Y);
	}
	Rider.SetPosition(Start);
	Rider.SnapshotPrevious();
	Rider.CommandMoveTo(End); // (ends on the nearest free spot)
	// A tunnel or a barracks with a rally point sends them on to it.
	if (Container.HasRallyPoint() && !Rider.IsFlying())
	{
		Rider.bHasExitRally = true;
		Rider.ExitRally = Container.GetRallyPoint();
	}
	return true;
}

void UGenGameLogic::PlaceAtGarrisonPoint(FGenObject& Container, FGenObject& Rider, const FGenObject* Victim)
{
	// GarrisonContain: occupants sit in the middle of the building; one with a target goes to the free
	// window (FIREPOINTnn bone) nearest to it, and moves again when another becomes nearer.
	FGenContain& K = *Container.Contain;
	if (!K.bFirePointsLoaded || K.FirePointsVersion != Container.GetConditionVersion())
	{
		// GarrisonContain::loadGarrisonPoints: the windows are bones of the model the building shows right
		// now (its garrisoned, damaged, night or snow version), so they are read again when that changes
		// and the occupants take their places anew (redeployOccupants).
		K.bFirePointsLoaded = true;
		K.FirePointsVersion = Container.GetConditionVersion();
		K.FirePoints.Reset();
		UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
		const FGenDrawModule* Draw = Container.GetTemplate()->PrimaryDraw();
		const int32 State = Draw ? Draw->FindBestState(Container.GetModelConditions()) : INDEX_NONE;
		const FString Model = (Draw && Draw->States.IsValidIndex(State) && !Draw->States[State].Model.IsEmpty()) ? Draw->States[State].Model
			: (Draw ? Draw->DefaultModel() : FString());
		TArray<FTransform3f> Bones;
		if (Assets && Assets->GetPristineBones(Model, TEXT("FIREPOINT"), 1, MaxGarrisonPoints, Bones) > 0)
		{
			const FTransform3f Xf = Container.GetTransform();
			for (const FTransform3f& Bone : Bones)
			{
				K.FirePoints.Add(Xf.TransformPositionNoScale(Bone.GetTranslation())); // (buildings don't move)
			}
		}
		K.FirePointUsers.Init(GenInvalidObjectID, K.FirePoints.Num());
	}
	int32& Index = Rider.GarrisonPoint;
	const bool bHolds = K.FirePointUsers.IsValidIndex(Index) && K.FirePointUsers[Index] == Rider.GetID();
	if (!bHolds)
	{
		// (The window it remembers went with the building's old look, or somebody else has it now. With
		// every window taken it then has none - it used to index the windows with "none" here.)
		Index = INDEX_NONE;
	}
	if (!Victim || K.FirePoints.Num() == 0)
	{
		if (bHolds)
		{
			K.FirePointUsers[Index] = GenInvalidObjectID;
		}
		Index = INDEX_NONE;
		Rider.SetPosition(Container.GetPosition());
		return;
	}
	int32 Best = bHolds ? Index : INDEX_NONE;
	float BestDistSq = bHolds ? (K.FirePoints[Index] - Victim->GetPosition()).SizeSquared() : FLT_MAX;
	for (int32 i = 0; i < K.FirePoints.Num(); ++i)
	{
		const float DistSq = (K.FirePoints[i] - Victim->GetPosition()).SizeSquared();
		if (K.FirePointUsers[i] == GenInvalidObjectID && DistSq < BestDistSq)
		{
			Best = i;
			BestDistSq = DistSq;
		}
	}
	if (Best != Index && K.FirePointUsers.IsValidIndex(Best))
	{
		if (bHolds)
		{
			K.FirePointUsers[Index] = GenInvalidObjectID;
		}
		K.FirePointUsers[Best] = Rider.GetID();
		Index = Best;
	}
	Rider.SetPosition(K.FirePoints.IsValidIndex(Index) ? K.FirePoints[Index] : Container.GetPosition());
}

// ---- the container, every frame ------------------------------------------------------------------

void UGenGameLogic::UpdateContain(FGenObject& Container)
{
	GEN_PERF_SCOPE("Obj.Contain");
	const FGenContainData& D = Container.GetTemplate()->Contain;
	FGenContain& K = *Container.Contain;
	const bool bTunnel = D.Type == EGenContainType::Tunnel;
	if (bTunnel && !Players.IsValidIndex(Container.GetOwner()))
	{
		return;
	}

	// TransportContain::createPayload: some transports come with their passengers.
	if (!K.bPayloadCreated)
	{
		K.bPayloadCreated = true;
		const FGenThingTemplate* Payload = D.InitialPayload.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(D.InitialPayload);
		TGuardValue<bool> NoLoadSounds(bLoadSoundsEnabled, false); // (enableLoadSounds: nobody is heard boarding)
		for (int32 i = 0; Payload && i < D.InitialPayloadCount; ++i)
		{
			FGenObject* Rider = CreateObject(Payload, Container.GetOwner(), Container.GetPosition(), Container.GetOrientation());
			if (Rider && IsValidContainerFor(Container, *Rider, true))
			{
				AddToContain(Container, *Rider);
			}
			else if (Rider)
			{
				Rider->bPendingDestroy = true;
			}
		}
		// OverlordContain / HelixContain::createPayload: "any number of different passengers can be loaded
		// here at init time".
		for (const FString& Name : D.PayloadTemplateNames)
		{
			const FGenThingTemplate* RiderT = FGenGameData::Get().FindTemplate(Name);
			FGenObject* Rider = RiderT ? CreateObject(RiderT, Container.GetOwner(), Container.GetPosition(), Container.GetOrientation()) : nullptr;
			if (Rider && IsValidContainerFor(Container, *Rider, true))
			{
				AddToContain(Container, *Rider);
			}
			else if (Rider)
			{
				Rider->bPendingDestroy = true;
			}
		}
	}

	if (K.Portable != GenInvalidObjectID && !GetPortableRider(Container))
	{
		K.Portable = GenInvalidObjectID;
	}
	if (D.bRiderChange && Container.ScuttleFrame != 0 && Frame >= Container.ScuttleFrame + uint32(D.ScuttleFrames))
	{
		// RiderChangeContain::update: "kill the bike in a way that will cause it to sink into the ground without any
		// real destruction ... Sneaky, eh? Toppled heheh."
		if (!Container.IsDead())
		{
			Container.Kill(TEXT("TOPPLED"));
		}
		return;
	}
	TArray<FGenObjectID>& Riders = bTunnel ? Players[Container.GetOwner()].TunnelRiders : K.Riders;
	// Riders that died, and units that gave up coming. (OpenContain::removeFromContain on a rider's death:
	// a building whose last occupant is killed goes back to its owner, and an armed transport's weapons
	// to what they were.)
	bool bLost = false;
	for (int32 i = Riders.Num() - 1; i >= 0; --i)
	{
		const FGenObject* R = FindObject(Riders[i]);
		if (!R || R->IsDead() || R->IsDestroyed() || !R->IsContained())
		{
			Riders.RemoveAt(i);
			bLost = true;
		}
	}
	if (bLost && !bTunnel)
	{
		OnContainChanged(Container);
	}
	for (int32 i = K.Wanters.Num() - 1; i >= 0; --i)
	{
		const FGenObject* W = FindObject(K.Wanters[i]);
		if (!W || W->IsDead() || W->GetEnterTarget() != Container.GetID())
		{
			K.Wanters.RemoveAt(i);
		}
	}
	if (Riders.Num() == 0)
	{
		return;
	}

	// (All the tunnels of a player look after the same occupants: once per frame is enough.)
	if (bTunnel)
	{
		FGenPlayer& P = Players[Container.GetOwner()];
		if (P.TunnelHealFrame == Frame)
		{
			return;
		}
		P.TunnelHealFrame = Frame;
	}
	const float FPS = float(GenLogic::GetLogicFPS());
	for (int32 i = Riders.Num() - 1; i >= 0; --i)
	{
		FGenObject* R = FindObject(Riders[i]);
		if (!R)
		{
			continue;
		}
		if (D.HealthRegenPercentPerSec > 0.f)
		{
			R->AttemptHealing(R->GetMaxHealth() * D.HealthRegenPercentPerSec / 100.f / FPS); // TransportContain::update
		}
		if (D.bHealObjects && D.FramesForFullHeal > 0)
		{
			// GarrisonContain::healSingleObject / HealContain::doHeal: whole again after TimeForFullHeal.
			const bool bDone = Frame - R->ContainedFrame >= uint32(D.FramesForFullHeal);
			R->AttemptHealing(bDone ? R->GetMaxHealth() : R->GetMaxHealth() / float(D.FramesForFullHeal));
		}
		if (D.Type == EGenContainType::Heal && R->GetHealth() >= R->GetMaxHealth())
		{
			R->CommandExit(); // healed: the barracks sends it out again
		}
		// GarrisonContain::onBodyDamageStateChange: a building about to come down turns its occupants out.
		if (D.Type == EGenContainType::Garrison && Container.GetDamageState() >= 2 && !Container.IsKindOf(TEXT("GARRISONABLE_UNTIL_DESTROYED")))
		{
			R->CommandExit();
		}
	}
}

FGenObject* UGenGameLogic::FindOtherTunnel(const FGenObject& Tunnel) const
{
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* O = Objects[Id].Get();
		if (O != &Tunnel && O->GetOwner() == Tunnel.GetOwner() && O->Contain && O->GetTemplate()->Contain.Type == EGenContainType::Tunnel
			&& !O->IsDead() && !O->IsDestroyed() && !O->IsUnderConstruction() && !O->IsBeingSold())
		{
			return O;
		}
	}
	return nullptr;
}

void UGenGameLogic::OnContainerSold(FGenObject& Container)
{
	// GarrisonContain::onSelling / TunnelContain::onSelling
	if (!Container.Contain)
	{
		return;
	}
	if (Container.GetTemplate()->Contain.Type == EGenContainType::Tunnel && FindOtherTunnel(Container))
	{
		OnContainerDied(Container); // they are under another tunnel now
		return;
	}
	const TArray<FGenObjectID> Riders = GetRiders(Container);
	for (FGenObjectID Id : Riders)
	{
		if (FGenObject* R = FindObject(Id))
		{
			R->ContainedBy = ResolveContainer(Container).GetID();
			Container.Contain->ExitNotBusyFrame = 0; // no time to queue at the door
			if (!ExitViaDoor(Container, *R))
			{
				RemoveFromContain(Container, *R);
			}
		}
	}
}

void UGenGameLogic::OnContainerDied(FGenObject& Container)
{
	if (!Container.Contain)
	{
		return;
	}
	const FGenContainData& D = Container.GetTemplate()->Contain;
	if (D.Type == EGenContainType::Tunnel)
	{
		// TunnelTracker::onTunnelDestroyed: when the last tunnel caves in everybody inside is lost;
		// otherwise they are simply under another one now.
		if (!Players.IsValidIndex(Container.GetOwner()))
		{
			return;
		}
		FGenObject* Other = FindOtherTunnel(Container);
		TArray<FGenObjectID>& Riders = Players[Container.GetOwner()].TunnelRiders;
		for (FGenObjectID Id : Riders)
		{
			FGenObject* R = FindObject(Id);
			if (R && !Other)
			{
				R->bPendingDestroy = true;
			}
			else if (R && R->ContainedBy == Container.GetID())
			{
				R->ContainedBy = Other->GetID();
			}
		}
		if (!Other)
		{
			UE_CLOG(bTrace && Riders.Num() > 0, LogGenerals, Log, TEXT("  [contain f%u] the last tunnel of p%d caves in on %d unit(s)"), Frame, Container.GetOwner(), Riders.Num());
			Riders.Reset();
		}
		return;
	}

	if (D.bRiderChange)
	{
		// RiderChangeContain::onRemoving: "Note if the bike dies, the rider dies too" - quietly (destroyObject): the
		// bike's own death throws a body off it (OCL_...FlyingOffBikeDeathStart), and its STATUS_RIDERn stays on the
		// wreck for the death modules that ask for it.
		for (FGenObjectID Id : Container.Contain->Riders)
		{
			if (FGenObject* R = FindObject(Id))
			{
				R->bPendingDestroy = true;
			}
		}
		Container.Contain->Riders.Reset();
		return;
	}

	// HelixContain / OverlordContain::onDie: the add-on goes with what carried it (and a bunker's
	// soldiers tumble out of the bunker as it does).
	if (FGenObject* AddOn = GetPortableRider(Container))
	{
		Container.Contain->Portable = GenInvalidObjectID;
		AddOn->Kill();
	}

	// OpenContain::onDie: the occupants take DamagePercentToUnits of their health, and whoever is left
	// tumbles out around the wreck.
	const TArray<FGenObjectID> Riders = Container.Contain->Riders;
	const FGenObject* Carrier = Container.IsContained() ? FindObject(Container.GetContainedBy()) : nullptr;
	const float Radius = (Carrier ? Carrier : &Container)->GetTemplate()->GetBoundingCircleRadius();
	for (FGenObjectID Id : Riders)
	{
		FGenObject* R = FindObject(Id);
		if (!R)
		{
			continue;
		}
		RemoveFromContain(Container, *R);
		if (D.DamagePercentToUnits > 0.f)
		{
			R->AttemptDamage(R->GetMaxHealth() * D.DamagePercentToUnits, TEXT("UNRESISTABLE"), D.bBurnedDeathToUnits ? TEXT("BURNED") : TEXT("NORMAL"), Container.GetID());
			if (!R->IsDead() && D.DamagePercentToUnits >= 1.f)
			{
				R->Kill(); // (flame proof troops included)
			}
		}
		if (!R->IsDead())
		{
			// OpenContain::scatterToNearbyPosition
			const float Angle = Random.FRandRange(0.f, 2.f * PI);
			const float Dist = Random.FRandRange(Radius, Radius * 1.5f);
			FVector3f Pos(Container.GetPosition().X + Dist * FMath::Cos(Angle), Container.GetPosition().Y + Dist * FMath::Sin(Angle), 0.f);
			Pos.Z = GetGroundHeight(Pos.X, Pos.Y);
			R->SetOrientation(Angle);
			R->CommandMoveTo(Pos);
		}
	}
}

void UGenGameLogic::AddSelectionHandoff(FGenObjectID From, FGenObjectID To)
{
	SelectionHandoffs.RemoveAll([this](const FSelectionHandoff& H) { return H.Frame + uint32(GenLogic::GetLogicFPS()) < Frame; });
	SelectionHandoffs.Add({ From, To, Frame });
}

// ---- orders -------------------------------------------------------------------------------------

void UGenGameLogic::IssueEnter(const TArray<FGenObjectID>& Units, FGenObjectID ContainerId)
{
	FGenCommand C(EGenCommandType::Enter);
	C.Units = Units;
	C.Target = ContainerId;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecEnter(const FGenCommand& Cmd)
{
	FGenObject* C = FindObject(Cmd.Target);
	for (FGenObjectID Id : Cmd.Units)
	{
		FGenObject* U = FindObject(Id);
		if (C && U && CanEnterObject(*U, *C, true))
		{
			U->OnPlayerCommand(false);
			U->CommandEnter(*C);
		}
	}
}

void UGenGameLogic::IssueExit(FGenObjectID Rider)
{
	FGenCommand C(EGenCommandType::Exit);
	C.Units = { Rider };
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecExit(const FGenCommand& Cmd)
{
	for (FGenObjectID Id : Cmd.Units)
	{
		if (FGenObject* R = FindObject(Id))
		{
			R->CommandExit();
		}
	}
}

void UGenGameLogic::EvacuateContainer(const FGenObject& Container)
{
	const TArray<FGenObjectID> Riders = GetRiders(Container);
	for (FGenObjectID Id : Riders)
	{
		if (FGenObject* R = FindObject(Id))
		{
			R->ContainedBy = ResolveContainer(Container).GetID();
			R->CommandExit();
		}
	}
}

void UGenGameLogic::IssueEvacuate(FGenObjectID ContainerId)
{
	FGenCommand C(EGenCommandType::Evacuate);
	C.Units = { ContainerId };
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecEvacuate(const FGenCommand& Cmd)
{
	const FGenObject* C = Cmd.Units.Num() > 0 ? FindObject(Cmd.Units[0]) : nullptr;
	if (!C || !C->GetContain())
	{
		return;
	}
	// OpenContain::orderAllPassengersToExit (a tunnel lets out everybody in the network).
	const TArray<FGenObjectID> Riders = GetRiders(*C);
	for (FGenObjectID Id : Riders)
	{
		if (FGenObject* R = FindObject(Id))
		{
			R->ContainedBy = ResolveContainer(*C).GetID(); // (out of this tunnel, whichever one they went in by)
			R->CommandExit();
		}
	}
}

// ---- the units --------------------------------------------------------------------------------------

void FGenObject::InitContain()
{
	if (Template->Contain.Type != EGenContainType::None)
	{
		Contain = MakeUnique<FGenContain>();
		Contain->bPassengersFire = Template->Contain.bPassengersAllowedToFire;
	}
}

void FGenObject::CommandEnter(FGenObject& Container)
{
	// AIUpdateInterface::aiEnter
	if (!Logic->CanEnterObject(*this, Container, true))
	{
		return;
	}
	StopEntering();
	ClearAttack();
	StopDocking();
	StopConstructing();
	AIMode = EGenAIMode::None;
	bHasExitRally = false;
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	EnterTarget = Container.ID;
	if (Container.Contain)
	{
		Container.Contain->Wanters.AddUnique(ID);
	}
	NextRepathFrame = 0;
	CommandStop();

	// AIEnterState::onEnter: "tell the pathfinder to ignore the enterable object" - the path to it may cross
	// the cells it stands on. Pathfinder::findPath's airfield case makes that the airfield when the target is
	// parked there ("only want to do this for things that are 'parked' on the airfield, but not for things
	// hovering over an obstacle (eg, a chinook over a supply dock)"): a plane stands on its airfield's cells,
	// so it is the airfield, not the plane, that a unit coming from afar has to be allowed to walk over.
	// Without this a pilot sent to a plane at the airfield has no way onto the runway at all.
	FGenObject* Ground = Logic->FindObject(Logic->GetPathfinder().GetFootprintOwnerAt(FVector2f(Container.Position.X, Container.Position.Y)));
	IgnoreObstacle = Ground && Ground->IsKindOf(TEXT("FS_AIRFIELD")) ? Ground->ID : Container.ID;
}

void FGenObject::StopEntering()
{
	if (EnterTarget == GenInvalidObjectID)
	{
		return;
	}
	if (FGenObject* C = Logic->FindObject(EnterTarget))
	{
		if (C->Contain)
		{
			C->Contain->Wanters.Remove(ID);
		}
	}
	// AIEnterState::onExit: "tell the pathfinder to stop ignoring the object".
	IgnoreObstacle = GenInvalidObjectID;
	EnterTarget = GenInvalidObjectID;
}

void FGenObject::UpdateEnter(uint32 Frame)
{
	// AIEnterState: make for the container; touching it is getting in (OpenContain::onCollide).
	FGenObject* C = Logic->FindObject(EnterTarget);
	if (!C || AIMode != EGenAIMode::None || bExplicitAttack || !Logic->CanEnterObject(*this, *C, true))
	{
		// It is gone, full, or we were told to do something else.
		const bool bStop = !C || (AIMode == EGenAIMode::None && !bExplicitAttack);
		StopEntering();
		if (bStop)
		{
			CommandStop();
		}
		return;
	}
	const float Gap = GapToFootprint(*C, Position) - Template->GetBoundingCircleRadius();
	if (Gap > 6.f && (bMoving || Gap > 40.f))
	{
		// (A container that moves is followed.)
		const bool bGoalStale = FVector2f::DistSquared(FVector2f(RequestedGoal.X, RequestedGoal.Y), FVector2f(C->Position.X, C->Position.Y)) > 100.f;
		if (!bMoving || (bGoalStale && Frame >= NextRepathFrame))
		{
			if (Frame >= NextRepathFrame)
			{
				StartPath(C->Position, false); // ends on the nearest free ground beside it
				NextRepathFrame = Frame + uint32(GenLogic::GetLogicFPS());
			}
		}
		return;
	}
	if (CanPilotEnter(*C))
	{
		PilotEnterVehicle(*C);
		return;
	}
	if (C->IsUnmanned() && IsKindOf(TEXT("INFANTRY")))
	{
		StopEntering();
		C->ClaimUnmanned(*this);
		return;
	}
	if (Logic->CanTakeOverVehicle(*this, *C))
	{
		StopEntering();
		TakeOverVehicle(*C);
		return;
	}
	if (Logic->CanSabotage(*this, *C))
	{
		StopEntering();
		Logic->SabotageBuilding(*this, *C);
		return;
	}
	if (C->Template->bChinookAI && C->ChinookFlight != EGenChinookFlight::Landed)
	{
		if (bMoving)
		{
			CommandStop(); // the helicopter has to come down first
		}
		return;
	}
	Logic->AddToContain(*C, *this);
}

void FGenObject::UpdateContained(uint32 Frame)
{
	FGenObject* C = Logic->FindObject(ContainedBy);
	if (!C || !C->Contain || C->bPendingDestroy)
	{
		bPendingDestroy = true; // OpenContain::onDelete: a container that is removed takes its occupants with it
		return;
	}
	Velocity = FVector3f::ZeroVector;
	bMoving = false;
	if (bWantsExit && Logic->ExitViaDoor(*C, *this))
	{
		return; // out
	}
	const bool bMayFire = HasWeapons() && Logic->IsPassengerAllowedToFire(*C, *this);
	if (C->Template->Contain.Type == EGenContainType::Garrison)
	{
		const FGenObject* Victim = (bMayFire && TargetID != GenInvalidObjectID) ? Logic->FindObject(TargetID) : nullptr;
		Logic->PlaceAtGarrisonPoint(*C, *this, Victim);
	}
	else
	{
		Position = C->Position;
	}
	if (C->Contain->Portable == ID)
	{
		// HelixContain::update / OpenContain::putObjAtNextFirePoint: the add-on faces the way its carrier
		// does - or the turret it stands on - and looks as battered (onBodyDamageStateChange; nothing can
		// hurt the add-on itself).
		Orientation = GenLogic::NormalizeAngle(C->Orientation + (C->Template->Contain.bPassengersInTurret ? C->TurretAngle[0] : 0.f));
		SetModelCondition(GenConditions::Damaged(), C->ModelConditions.Test(GenConditions::Damaged()));
		SetModelCondition(GenConditions::ReallyDamaged(), C->ModelConditions.Test(GenConditions::ReallyDamaged()));
	}
	if (bMayFire)
	{
		UpdateCombat(Frame);
	}
	else if (TargetID != GenInvalidObjectID)
	{
		ClearAttack();
	}
	if (Contain)
	{
		Logic->UpdateContain(*this); // (a bunker on an Overlord looks after who is in it)
	}
	SnapshotPrevious();
}

void FGenObject::UpdateChinookFlight(uint32 Frame)
{
	// ChinookAIUpdate::update: idle with somebody waiting to get on or off, the helicopter lands; on the
	// ground with nobody waiting (or with somewhere to go), it takes off again.
	const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
	bool bWaiting = Contain->Wanters.Num() > 0;
	for (FGenObjectID Id : Contain->Riders)
	{
		const FGenObject* R = Logic->FindObject(Id);
		bWaiting |= R && R->bWantsExit;
	}
	if (CombatDrop)
	{
		bWaiting = false; // on its way to a combat drop it lands for nobody
	}
	if (ChinookHealAirfield != GenInvalidObjectID)
	{
		// Sent for repairs: down once there (MOVE_TO_AND_LAND), mended by the airfield while on the ground, and
		// up again once whole - or once the airfield is gone.
		const FGenObject* A = Logic->FindObject(ChinookHealAirfield);
		FGenParkingPlace* PP = (A && !A->IsDead() && A->IsKindOf(TEXT("FS_AIRFIELD"))) ? A->GetParking() : nullptr;
		if (!PP || (ChinookFlight == EGenChinookFlight::Landed && Health >= MaxHealth))
		{
			SetChinookHealAirfield(GenInvalidObjectID);
		}
		else
		{
			PP->SetHealee(ID, ChinookFlight == EGenChinookFlight::Landed);
			bWaiting = true;
		}
	}
	const bool bIdle = IsAIIdle();
	switch (ChinookFlight)
	{
	case EGenChinookFlight::Flying:
		if (bIdle && bWaiting)
		{
			ChinookFlight = EGenChinookFlight::Landing;
			SetBoxes(0); // (ChinookTakeoffOrLandingState: the supplies it carries are lost)
			Velocity.X = Velocity.Y = 0.f;
		}
		break;
	case EGenChinookFlight::Landing:
		if (!bIdle || !bWaiting) { ChinookFlight = EGenChinookFlight::TakingOff; }
		else if (Position.Z - Ground <= ChinookLandedHeight) { ChinookFlight = EGenChinookFlight::Landed; }
		break;
	case EGenChinookFlight::Landed:
		if (!bIdle || !bWaiting) { ChinookFlight = EGenChinookFlight::TakingOff; }
		break;
	default: // TakingOff
		if (bIdle && bWaiting) { ChinookFlight = EGenChinookFlight::Landing; }
		else if (!Locomotor || Position.Z - Ground >= Locomotor->PreferredHeight - ChinookLandedHeight) { ChinookFlight = EGenChinookFlight::Flying; }
		break;
	}
	const bool bDown = ChinookFlight == EGenChinookFlight::Landing || ChinookFlight == EGenChinookFlight::Landed;
	bLocoPreciseZ = bDown;
	bLocoUltraAccurate = bDown;
	PreciseGoalZ = Ground;
	if (CombatDrop)
	{
		UpdateCombatDropApproach(Frame);
	}
}

void FGenObject::SetChinookHealAirfield(FGenObjectID Airfield)
{
	// ChinookAIUpdate::setAirfieldForHealing: the airfield it leaves stops mending it.
	if (ChinookHealAirfield != GenInvalidObjectID && ChinookHealAirfield != Airfield)
	{
		const FGenObject* Old = Logic->FindObject(ChinookHealAirfield);
		if (Old && Old->GetParking())
		{
			Old->GetParking()->SetHealee(ID, false);
		}
	}
	ChinookHealAirfield = Airfield;
}

void FGenObject::CommandChinookGetRepaired(const FGenObject& Airfield)
{
	// ChinookAIUpdate::privateGetRepaired: to open ground beside the airfield, within a hundred of its radius.
	if (bDead || !Template->bChinookAI || !Contain || !Airfield.GetParking())
	{
		return;
	}
	SetChinookHealAirfield(Airfield.GetID());
	CommandMoveTo(Logic->FindFreeGroundAround(Airfield.GetPosition(), Airfield.GetTemplate()->GetBoundingCircleRadius() * 100.f));
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [repair f%u] #%u %s flies to #%u %s for repairs (%.0f / %.0f)"), Logic->GetFrame(), ID,
		*Template->Name, Airfield.GetID(), *Airfield.GetTemplate()->Name, Health, MaxHealth);
}

// ---- the combat drop ------------------------------------------------------------------------------

bool UGenGameLogic::CanCombatDropInto(const FGenObject& Building) const
{
	// ActionManager::canEnterObject with COMBATDROP_INTO: "we don't care about valid container-ness, but we
	// DO care about faction structures... we aren't allowed to combat drop into them".
	return !Building.IsDead() && !Building.IsDestroyed() && Building.GetContain() && !IsSideStructure(*Building.GetTemplate());
}

bool UGenGameLogic::HasRappeller(const FGenObject& Container) const
{
	// getPotentialRappeller
	if (!Container.GetTemplate()->bChinookAI || !Container.GetContain())
	{
		return false;
	}
	for (FGenObjectID Id : GetRiders(Container))
	{
		const FGenObject* R = FindObject(Id);
		if (R && R->IsKindOf(TEXT("CAN_RAPPEL")))
		{
			return true;
		}
	}
	return false;
}

void UGenGameLogic::IssueCombatDrop(const TArray<FGenObjectID>& Units, FGenObjectID Target, const FVector3f& Pos)
{
	FGenCommand C(EGenCommandType::CombatDrop);
	C.Units = Units;
	C.Target = Target;
	C.Pos = Pos;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecCombatDrop(const FGenCommand& C)
{
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (U && !U->IsDead() && U->GetTemplate()->bChinookAI && U->Contain && !(U->CombatDrop && U->CombatDrop->bDropping))
		{
			U->OnPlayerCommand(false);
			U->CommandCombatDrop(FindObject(C.Target), C.Pos);
		}
	}
}

void UGenGameLogic::UpdateFallingRopes()
{
	// (W3DRopeDraw::doDrawModule after setRopeSpeed: a rope let go falls, and is gone five seconds later.)
	for (int32 i = FallingRopes.Num() - 1; i >= 0; --i)
	{
		FGenFallingRope& R = FallingRopes[i];
		R.Top.Z += R.Speed;
		R.Speed += FGenGameData::Get().Gravity;
		R.Phase += R.WobbleRate;
		if (Frame >= R.ExpireFrame)
		{
			FallingRopes.RemoveAtSwap(i);
		}
	}
}

void UGenGameLogic::GetVisibleRopes(TArray<FGenRopeDraw>& Out) const
{
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		const FGenCombatDrop* Drop = O ? O->GetCombatDrop() : nullptr;
		if (!Drop || !Drop->bDropping || O->GetLocalShroud() == EGenObjectShroud::Fogged || O->GetLocalShroud() == EGenObjectShroud::Shrouded
			|| GetStealthLook(*O) == EGenStealthLook::Invisible)
		{
			continue;
		}
		const FGenThingTemplate& T = *O->GetTemplate();
		for (const FGenCombatDrop::FRope& Rope : Drop->Ropes)
		{
			FGenRopeDraw& D = Out.AddDefaulted_GetRef();
			D.Top = Rope.Top;
			D.Len = FMath::Min(Rope.Len, Rope.LenMax);
			D.LenMax = Rope.LenMax;
			D.Width = T.ChinookRopeWidth;
			D.WobbleLen = T.ChinookRopeWobbleLen;
			D.WobbleAmp = T.ChinookRopeWobbleAmp;
			D.Phase = Drop->WobblePhase;
		}
	}
	for (const FGenFallingRope& R : FallingRopes)
	{
		FGenRopeDraw& D = Out.AddDefaulted_GetRef();
		D.Top = R.Top;
		D.Len = D.LenMax = R.Len;
		D.Width = R.Width;
		D.WobbleLen = R.WobbleLen;
		D.WobbleAmp = R.WobbleAmp;
		D.Phase = R.Phase;
	}
}

void FGenObject::CommandCombatDrop(FGenObject* Target, const FVector3f& InPos)
{
	// ChinookAIUpdate::privateCombatDrop
	if (!Template->bChinookAI || !Contain || bDead || !Locomotor)
	{
		return;
	}
	if (Target && !Logic->CanCombatDropInto(*Target))
	{
		return;
	}
	FVector3f Pos = Target ? Target->Position : InPos;
	if (!Target)
	{
		// "if target is null, we are dropping at a pos, not into a bldg. in this case, ensure there is no
		// structure at the pos... this can happen if you combat-drop into a spot in the fog-of-war."
		Pos = Logic->FindGroundSpotAround(InPos, 0.f, 200.f);
	}
	ClearAttack();
	CommandMoveTo(Pos);

	// ChinookMoveToBldgState::onEnter: over a building, at least MinDropHeight above its roof.
	CombatDrop = MakeUnique<FGenCombatDrop>();
	CombatDrop->Target = Target ? Target->ID : GenInvalidObjectID;
	CombatDrop->Pos = Pos;
	float Height = Locomotor->PreferredHeight;
	if (Target && Target->IsKindOf(TEXT("STRUCTURE")))
	{
		Height = FMath::Max(Height, Target->Template->GetMaxHeightAbovePosition() + Template->ChinookMinDropHeight);
	}
	CombatDrop->DestZ = Logic->GetGroundHeight(Pos.X, Pos.Y) + Height;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drop f%u] #%u %s flies to a combat drop at (%.0f, %.0f) height %.0f%s%s"), Logic->GetFrame(), ID, *Template->Name,
		Pos.X, Pos.Y, Height, Target ? TEXT(" into ") : TEXT(""), Target ? *Target->Template->Name : TEXT(""));
}

void FGenObject::UpdateCombatDropApproach(uint32 Frame)
{
	// ChinookMoveToBldgState: "the normal moveto state will bail when 2d pos matches; we need z, too"
	FGenCombatDrop& D = *CombatDrop;
	if (D.Target != GenInvalidObjectID)
	{
		const FGenObject* B = Logic->FindObject(D.Target);
		if (!B || B->bDead || B->bPendingDestroy)
		{
			EndCombatDrop(); // the building is gone: nothing to drop into
			CommandStop();
			return;
		}
	}
	if (ChinookFlight != EGenChinookFlight::Flying)
	{
		return; // still getting off the ground
	}
	bLocoPreciseZ = true;
	bLocoUltraAccurate = true;
	PreciseGoalZ = D.DestZ;
	if (!bMoving && !IsWaitingForPath() && FMath::Abs(Position.Z - D.DestZ) <= 3.f)
	{
		if (!BeginCombatDrop(Frame))
		{
			EndCombatDrop();
		}
	}
}

bool FGenObject::BeginCombatDrop(uint32 Frame)
{
	// ChinookCombatDropState::onEnter: a rope for each pair of RopeStart / RopeEnd bones of the model.
	FGenCombatDrop& D = *CombatDrop;
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	const FGenDrawModule* Draw = Template->PrimaryDraw();
	if (!Assets || !Draw)
	{
		return false;
	}
	const FString Model = Draw->DefaultModel();
	TArray<FTransform3f> Starts, Ends;
	const int32 NumStarts = Assets->GetPristineBones(Model, TEXT("RopeStart"), 1, 32, Starts);
	const int32 NumEnds = Assets->GetPristineBones(Model, TEXT("RopeEnd"), 1, 32, Ends);
	const int32 NumRopes = FMath::Min3(Template->ChinookNumRopes, NumStarts, NumEnds);
	if (NumRopes <= 0)
	{
		return false;
	}
	SetBoxes(0); // "A chinook given combat drop duty also loses his supplies."
	const FTransform3f Xf = GetTransform();
	FRandomStream& Rand = Logic->GetRandom();
	for (int32 i = 0; i < NumRopes; ++i)
	{
		FGenCombatDrop::FRope& Rope = D.Ropes.AddDefaulted_GetRef();
		Rope.Top = Xf.TransformPositionNoScale(Starts[i].GetTranslation());
		// ("rappeller->setTransformMatrix(&it->dropStartMtx)": the bone gives the soldier its attitude too.)
		const FTransform3f DropXf = Ends[i] * Xf;
		Rope.DropStart = DropXf.GetTranslation();
		Rope.DropRotation = DropXf.GetRotation().GetNormalized();
		Rope.LenMax = FMath::Max(1.f, Rope.Top.Z - Logic->GetGroundHeight(Rope.Top.X, Rope.Top.Y) - Template->ChinookRopeFinalHeight);
		Rope.NextDropFrame = Frame + uint32(Rand.RandRange(Template->ChinookRopeDelayMin, FMath::Max(Template->ChinookRopeDelayMin, Template->ChinookRopeDelayMax))
			- Template->ChinookRopeDelayMin);
	}
	D.bDropping = true;
	ChinookFlight = EGenChinookFlight::CombatDrop;
	bMoving = false;
	Path.Reset();
	Velocity.X = Velocity.Y = 0.f;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drop f%u] #%u %s lets down %d ropes of %.0f, %d on board"), Frame, ID, *Template->Name, NumRopes,
		D.Ropes[0].LenMax, Logic->GetRiders(*this).Num());
	return true;
}

void FGenObject::UpdateCombatDrop(uint32 Frame)
{
	// ChinookCombatDropState::update
	FGenCombatDrop& D = *CombatDrop;
	D.WobblePhase = FMath::Fmod(D.WobblePhase + Template->ChinookRopeWobbleRate, 2.f * PI);
	bLocoPreciseZ = true;
	bLocoUltraAccurate = true;
	PreciseGoalZ = D.DestZ;

	auto NextRappeller = [this]() -> FGenObject*
	{
		for (FGenObjectID Id : Logic->GetRiders(*this))
		{
			FGenObject* R = Logic->FindObject(Id);
			if (R && !R->bDead && R->IsKindOf(TEXT("CAN_RAPPEL")))
			{
				return R;
			}
		}
		return nullptr;
	};

	int32 RopesInUse = 0;
	const float Gravity = FMath::Abs(FGenGameData::Get().Gravity);
	FRandomStream& Rand = Logic->GetRandom();
	for (FGenCombatDrop::FRope& Rope : D.Ropes)
	{
		// "first, eliminate "done" rappellers"
		Rope.Rappellers.RemoveAll([this](FGenObjectID Id)
		{
			const FGenObject* R = Logic->FindObject(Id);
			return !R || R->bDead || !R->bRappelling;
		});
		if (Rope.Len < Rope.LenMax)
		{
			Rope.Speed = FMath::Min(Rope.Speed + Gravity, Template->ChinookRopeDropSpeed);
			Rope.Len += Rope.Speed;
			if (Template->bChinookWaitForRopes)
			{
				++Rope.NextDropFrame; // "can't use this rope till it's dropped all the way"
				RopesInUse += Rope.Rappellers.Num() > 0 ? 1 : 0;
				continue;
			}
		}
		if (Frame >= Rope.NextDropFrame)
		{
			if (FGenObject* R = NextRappeller())
			{
				Logic->RemoveFromContain(*this, *R);
				R->SetPosition(Rope.DropStart);
				R->SetRotation(Rope.DropRotation);
				R->bFreeRotation = true;
				R->BeginRappel(Logic->FindObject(D.Target), Template->ChinookRappelSpeed);
				R->SnapshotPrevious();
				Rope.Rappellers.Add(R->ID);
				Rope.NextDropFrame = Frame + uint32(Rand.RandRange(Template->ChinookRopeDelayMin, FMath::Max(Template->ChinookRopeDelayMin, Template->ChinookRopeDelayMax)));
			}
		}
		RopesInUse += Rope.Rappellers.Num() > 0 ? 1 : 0;
	}
	if (RopesInUse == 0 && !NextRappeller())
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drop f%u] #%u %s: everybody is down"), Frame, ID, *Template->Name);
		EndCombatDrop(); // "we're done!"
	}
}

void FGenObject::EndCombatDrop()
{
	// ChinookCombatDropState::onExit: the ropes are let go ("give it a little kick") and fall.
	if (!CombatDrop)
	{
		return;
	}
	if (CombatDrop->bDropping)
	{
		for (const FGenCombatDrop::FRope& Rope : CombatDrop->Ropes)
		{
			FGenFallingRope& F = Logic->FallingRopes.AddDefaulted_GetRef();
			F.Top = Rope.Top;
			F.Len = FMath::Min(Rope.Len, Rope.LenMax);
			F.Speed = FGenGameData::Get().Gravity * 30.f;
			F.Width = Template->ChinookRopeWidth;
			F.WobbleLen = Template->ChinookRopeWobbleLen;
			F.WobbleAmp = Template->ChinookRopeWobbleAmp;
			F.WobbleRate = Template->ChinookRopeWobbleRate;
			F.Phase = CombatDrop->WobblePhase;
			F.ExpireFrame = Logic->GetFrame() + uint32(GenLogic::GetLogicFPS() * 5);
		}
		ChinookFlight = EGenChinookFlight::Flying;
	}
	CombatDrop.Reset();
	bLocoPreciseZ = false;
	bLocoUltraAccurate = false;
}

void FGenObject::BeginRappel(FGenObject* Target, float Speed)
{
	// AIRappelState::onEnter
	static const int32 Rappelling = GenConditions::Index(TEXT("RAPPELLING"));
	bRappelling = true;
	bHeldByParachute = true; // (out of everybody's way, and deaf to orders, until it has landed)
	SetModelCondition(Rappelling, true);
	SetModelCondition(GenConditions::Moving(), false);
	Velocity = FVector3f::ZeroVector;
	bMoving = false;
	Path.Reset();
	RappelTarget = (Target && !Target->bDead && Target->IsKindOf(TEXT("STRUCTURE"))) ? Target->ID : GenInvalidObjectID;
	RappelDestZ = Logic->GetGroundHeight(Position.X, Position.Y) + (RappelTarget != GenInvalidObjectID ? Target->Template->GetMaxHeightAbovePosition() : 0.f);
	const float MaxRate = FMath::Abs(FGenGameData::Get().Gravity) * float(GenLogic::GetLogicFPS()) * 2.5f;
	RappelRate = FMath::Min(Speed, MaxRate);
}

void FGenObject::UpdateRappel(uint32 Frame)
{
	// AIRappelState::update: straight down at the rappel rate.
	static const int32 Rappelling = GenConditions::Index(TEXT("RAPPELLING"));
	FGenObject* Bldg = RappelTarget != GenInvalidObjectID ? Logic->FindObject(RappelTarget) : nullptr;
	if (RappelTarget != GenInvalidObjectID && (!Bldg || Bldg->bDead || Bldg->bPendingDestroy))
	{
		// "if bldg is destroyed, just head for the ground"
		RappelTarget = GenInvalidObjectID;
		Bldg = nullptr;
	}
	if (!Bldg)
	{
		RappelDestZ = Logic->GetGroundHeight(Position.X, Position.Y);
	}
	Position.Z -= RappelRate;
	Velocity = FVector3f(0.f, 0.f, -RappelRate);
	if (Position.Z > RappelDestZ)
	{
		return;
	}
	Position.Z = RappelDestZ;
	Velocity = FVector3f::ZeroVector;
	SetOrientation(Orientation); // on its feet again
	bFreeRotation = false;
	bRappelling = false;
	bHeldByParachute = false;
	RappelTarget = GenInvalidObjectID;
	SetModelCondition(Rappelling, false);
	NextAutoAcquireFrame = 0;
	if (!Bldg)
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drop f%u] #%u %s is on the ground at (%.0f, %.0f)"), Frame, ID, *Template->Name, Position.X, Position.Y);
		return;
	}

	// "if there are enemies... kill up to two. if we kill two, then we die ourselves, otherwise we enter the bldg."
	constexpr int32 MaxToKill = 2;
	int32 NumKilled = 0;
	while (NumKilled < MaxToKill)
	{
		FGenObject* Enemy = nullptr;
		for (FGenObjectID Id : Logic->GetRiders(*Bldg))
		{
			FGenObject* R = Logic->FindObject(Id);
			if (R && !R->bDead && Logic->GetRelationship(Owner, R->Owner) == EGenRelationship::Enemies)
			{
				Enemy = R;
				break;
			}
		}
		if (!Enemy)
		{
			break;
		}
		Enemy->ContainedBy = Bldg->ID;
		Logic->RemoveFromContain(*Bldg, *Enemy);
		Enemy->Health = 0.f;
		Enemy->Die(TEXT("NORMAL"), ID);
		++NumKilled;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [drop f%u] #%u %s lands on #%u %s and kills %d inside"), Frame, ID, *Template->Name, Bldg->ID,
		*Bldg->Template->Name, NumKilled);
	if (NumKilled == MaxToKill)
	{
		Kill();
		return;
	}
	if (Logic->CanEnterObject(*this, *Bldg, true))
	{
		Logic->AddToContain(*Bldg, *this);
		return;
	}
	// "this can legitimately happen if you drop into a full (or nearly full) building. let's just place
	// the guy on the ground nearby, since it sucks to fall from the top of a building."
	const float Offset = FMath::Min(Template->GetBoundingCircleRadius(), Bldg->Template->GetBoundingCircleRadius());
	const float Angle = Logic->GetRandom().FRandRange(PI, 2.f * PI);
	FVector3f Start(Bldg->Position.X + Offset * FMath::Cos(Angle), Bldg->Position.Y + Offset * FMath::Sin(Angle), 0.f);
	Start.Z = Logic->GetGroundHeight(Start.X, Start.Y);
	SetPosition(Start);
	SetOrientation(Bldg->Orientation);
	SnapshotPrevious();
	const float R = Bldg->Template->GetBoundingCircleRadius();
	CommandMoveTo(Logic->FindGroundSpotAround(Bldg->Position, R + 5.f, R + 200.f));
}
