// Garrisons and transports: units inside other objects.
// Follows OpenContain.cpp, TransportContain.cpp, GarrisonContain.cpp, HealContain.cpp, TunnelContain.cpp and
// TunnelTracker.cpp, ActionManager::canEnterObject, AIEnterState / AIExitState, and the part of
// ChinookAIUpdate.cpp that lands the helicopter for its passengers.

#include "Logic/GenGameLogic.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
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

const TArray<FGenObjectID>& UGenGameLogic::GetRiders(const FGenObject& Container) const
{
	static const TArray<FGenObjectID> None;
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

int32 UGenGameLogic::GetContainMax(const FGenObject& Container) const
{
	const FGenContainData& D = Container.GetTemplate()->Contain;
	return D.Type == EGenContainType::Tunnel ? FGenGameData::Get().MaxTunnelCapacity : D.ContainMax;
}

int32 UGenGameLogic::GetContainCount(const FGenObject& Container) const
{
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

bool UGenGameLogic::IsPassengerAllowedToFire(const FGenObject& Container, const FGenObject& Rider) const
{
	const FGenContainData& D = Container.GetTemplate()->Contain;
	if (!D.bPassengersAllowedToFire || Container.IsDisabled())
	{
		return false;
	}
	// (TransportContain: only the infantry on board shoot out.)
	return D.Type != EGenContainType::Transport || Rider.IsKindOf(TEXT("INFANTRY"));
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
	const FGenContainData& D = Container.GetTemplate()->Contain;
	const bool bTunnel = D.Type == EGenContainType::Tunnel;
	if (bTunnel && !Players.IsValidIndex(Container.GetOwner()))
	{
		return;
	}
	TArray<FGenObjectID>& Riders = bTunnel ? Players[Container.GetOwner()].TunnelRiders : Container.Contain->Riders;

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
		break;
	case EGenContainType::Garrison:
		Rider.BonusConditions.AddUnique(GarrisonedBonus);
		break;
	default:
		break;
	}
	OnContainChanged(Container);
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
	OnContainChanged(Container);
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
	const bool bAirborne = Container.GetPosition().Z - GetGroundHeight(Container.GetPosition().X, Container.GetPosition().Y) > 9.f;
	if ((Container.GetTemplate()->bChinookAI && Container.GetChinookFlight() != EGenChinookFlight::Landed) || (D.bDelayExitInAir && bAirborne))
	{
		return false;
	}
	RemoveFromContain(Container, Rider);
	K.ExitNotBusyFrame = Frame + uint32(D.ExitDelay);

	const uint32 Surfaces = Rider.Locomotor ? Rider.Locomotor->Surfaces : uint32(GenSurface::Ground);
	const float Radius = Container.GetTemplate()->GetBoundingCircleRadius();
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
	if (Best != Index)
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
	}

	TArray<FGenObjectID>& Riders = bTunnel ? Players[Container.GetOwner()].TunnelRiders : K.Riders;
	// Riders that died, and units that gave up coming.
	for (int32 i = Riders.Num() - 1; i >= 0; --i)
	{
		const FGenObject* R = FindObject(Riders[i]);
		if (!R || R->IsDead() || R->IsDestroyed() || !R->IsContained())
		{
			Riders.RemoveAt(i);
		}
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
			R->ContainedBy = Container.GetID();
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

	// OpenContain::onDie: the occupants take DamagePercentToUnits of their health, and whoever is left
	// tumbles out around the wreck.
	const TArray<FGenObjectID> Riders = Container.Contain->Riders;
	const float Radius = Container.GetTemplate()->GetBoundingCircleRadius();
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

// ---- orders -------------------------------------------------------------------------------------

void UGenGameLogic::IssueEnter(const TArray<FGenObjectID>& Units, FGenObjectID ContainerId)
{
	PendingCommands.Add([this, Units, ContainerId]()
	{
		FGenObject* C = FindObject(ContainerId);
		for (FGenObjectID Id : Units)
		{
			FGenObject* U = FindObject(Id);
			if (C && U && CanEnterObject(*U, *C, true))
			{
				U->OnPlayerCommand(false);
				U->CommandEnter(*C);
			}
		}
	});
}

void UGenGameLogic::IssueExit(FGenObjectID Rider)
{
	PendingCommands.Add([this, Rider]()
	{
		if (FGenObject* R = FindObject(Rider))
		{
			R->CommandExit();
		}
	});
}

void UGenGameLogic::IssueEvacuate(FGenObjectID ContainerId)
{
	PendingCommands.Add([this, ContainerId]()
	{
		const FGenObject* C = FindObject(ContainerId);
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
				R->ContainedBy = ContainerId; // (out of this tunnel, whichever one they went in by)
				R->CommandExit();
			}
		}
	});
}

// ---- the units --------------------------------------------------------------------------------------

void FGenObject::InitContain()
{
	if (Template->Contain.Type != EGenContainType::None)
	{
		Contain = MakeUnique<FGenContain>();
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
	Container.Contain->Wanters.AddUnique(ID);
	NextRepathFrame = 0;
	CommandStop();
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
	if (bMayFire)
	{
		UpdateCombat(Frame);
	}
	else if (TargetID != GenInvalidObjectID)
	{
		ClearAttack();
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
}
