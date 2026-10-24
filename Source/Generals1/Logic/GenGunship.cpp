// The Spectre gunship: SpectreGunshipDeploymentUpdate (a building's power sends for one) and
// SpectreGunshipUpdate (its run).
//
// It is made beyond the edge of the map, facing the target, and flies in on its fast locomotor with
// its doors shut. Inside its orbit radius it changes to the slow one, opens its doors and circles the
// target anticlockwise for OrbitTime: it always makes for a point a little ahead of itself on the
// circle (the "satellite position"). While it circles, its gattling gun - a rider of its own, seen on
// its side - fires at whatever enemy is inside the targeting reticle, or strafes the ground towards
// the reticle a StrafingIncrement a frame; once the gattling has been on the spot for
// HowitzerFollowLag, the howitzer drops a shell near it every HowitzerFiringRate. The player moves
// the reticle about inside the attack area; a computer player's gunship looks for targets in the
// whole area by itself. Then it packs up and flies straight on until it is off the map.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Logic/GenUpgrades.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	enum EGunshipStatus : uint8 { GunshipIdle, GunshipInserting, GunshipOrbiting, GunshipDeparting };
	constexpr float OrbitInsertionSlopeMax = 0.8f, OrbitInsertionSlopeMin = 0.5f;
	constexpr int32 LotsOfShots = 9999;

	int32 Door1OpeningBit() { static const int32 I = GenConditions::Index(TEXT("DOOR_1_OPENING")); return I; }
	int32 Door1ClosingBit() { static const int32 I = GenConditions::Index(TEXT("DOOR_1_CLOSING")); return I; }
	int32 AfterburnerBit() { static const int32 I = GenConditions::Index(TEXT("JETAFTERBURNER")); return I; }
}

/** SpectreGunshipUpdate's members. */
struct FGenGunshipState
{
	uint8 Status = GunshipIdle;
	FVector3f InitialTarget = FVector3f::ZeroVector;   // the middle of the area it attacks
	FVector3f OverrideTarget = FVector3f::ZeroVector;  // where the player has the reticle
	FVector3f GattlingTarget = FVector3f::ZeroVector;  // where the gattling's bullets land now
	FVector3f ShootAt = FVector3f::ZeroVector;         // where they are making for
	FVector3f Satellite = FVector3f::ZeroVector;       // the point on its circle it flies to
	FVector2f ExitDir = FVector2f(1.f, 0.f);
	FGenObjectID Gattling = GenInvalidObjectID;
	uint32 OrbitEscapeFrame = 0;
	int32 OkToFireHowitzer = 0;
};

void FGenObject::FGenGunshipDeleter::operator()(FGenGunshipState* State) const
{
	delete State;
}

bool UGenGameLogic::DeployGunship(FGenObject& Source, const FString& Power, const FVector3f& TargetPos)
{
	// SpectreGunshipDeploymentUpdate::initiateIntentToDoSpecialPower. (A general with better gunships
	// has a module for each: the first whose science the player has.)
	const FGenGunshipDeployment* D = nullptr;
	for (const FGenGunshipDeployment& Try : Source.GetTemplate()->GunshipDeployments)
	{
		if (Try.Power.Equals(Power, ESearchCase::IgnoreCase) && (Try.RequiredScience.IsEmpty() || HasScience(Source.GetOwner(), Try.RequiredScience)))
		{
			D = &Try;
			break;
		}
	}
	const FGenThingTemplate* ShipT = D ? FGenGameData::Get().FindTemplate(D->GunshipTemplate) : nullptr;
	if (!ShipT || !ShipT->Gunship.bValid)
	{
		return false;
	}
	FVector3f Edge;
	if (D->CreateLocation.Equals(TEXT("CREATE_AT_EDGE_NEAR_SOURCE"), ESearchCase::IgnoreCase)) { Edge = FindEdgePoint(Source.GetPosition(), false); }
	else if (D->CreateLocation.Equals(TEXT("CREATE_AT_EDGE_FARTHEST_FROM_SOURCE"), ESearchCase::IgnoreCase)) { Edge = FindEdgePoint(Source.GetPosition(), true); }
	else if (D->CreateLocation.Equals(TEXT("CREATE_AT_EDGE_NEAR_TARGET"), ESearchCase::IgnoreCase)) { Edge = FindEdgePoint(TargetPos, false); }
	else { Edge = FindEdgePoint(TargetPos, true); }
	// "Here we need to create the point further off the map so we can't see the lame hover and accelerate behavior."
	const FVector2f Delta(TargetPos.X - Edge.X, TargetPos.Y - Edge.Y);
	const float Dist = Delta.Size();
	const FVector2f Dir = Dist > 1.f ? Delta / Dist : FVector2f(1.f, 0.f);
	const FVector2f Create = FVector2f(TargetPos.X, TargetPos.Y) - Dir * (Dist + ShipT->Gunship.OrbitRadius);
	const float Angle = FMath::Atan2(TargetPos.Y - Create.Y, TargetPos.X - Create.X);
	FGenObject* Ship = CreateObject(ShipT, Source.GetOwner(), FVector3f(Create.X, Create.Y, GetGroundHeight(Create.X, Create.Y)), Angle);
	if (!Ship)
	{
		return false;
	}
	Ship->Producer = Source.GetID();
	Ship->BeginGunshipRun(TargetPos, Frame);
	Gunships.Add(Ship->GetID());
	// "Make the gunship selected": its owner steers it.
	if (Source.GetOwner() == LocalPlayer)
	{
		SelectRequests.Add(Ship->GetID());
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [gunship f%u] #%u %s comes in from (%.0f, %.0f) for (%.0f, %.0f)"), Frame, Ship->GetID(), *ShipT->Name, Create.X, Create.Y, TargetPos.X, TargetPos.Y);
	return true;
}

void UGenGameLogic::IssueSteerGunship(const TArray<FGenObjectID>& Units, const FVector3f& Pos)
{
	FGenCommand C(EGenCommandType::SteerGunship);
	C.Units = Units;
	C.Pos = Pos;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecSteerGunship(const FGenCommand& C)
{
	const FVector3f Ground(C.Pos.X, C.Pos.Y, GetGroundHeight(C.Pos.X, C.Pos.Y));
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (U && !U->IsDead() && U->SteerGunship(Ground))
		{
			if (U->GetOwner() == LocalPlayer)
			{
				PlayObjectSound(*U, { TEXT("VoiceAttack") });
			}
		}
		else if (U && !U->IsDead() && U->GetTemplate()->ParticleCannon.bValid)
		{
			// (MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION for a selected particle cannon: its beam goes there.)
			U->SteerParticleCannon(U->GetTemplate()->ParticleCannon.Power, Ground, Frame);
		}
	}
}

FGenObject* UGenGameLogic::FindGunshipVictim(const FGenObject& Ship, const FVector3f& Center, float Range) const
{
	// PartitionFilterLiveMapEnemies, StealthedAndUndetected, PossibleToAttack, FreeOfFog; the nearest of
	// them (by where its edge is) that is a "fair distance from the ship": not almost under it.
	const float TooNear = Ship.GetTemplate()->Gunship.OrbitRadius * 0.75f;
	FGenObject* Best = nullptr;
	float BestDist = FLT_MAX;
	Partition.ForEachInRange(FVector2f(Center.X, Center.Y), Range + 60.f, [&](FGenObject& Them)
	{
		if (Them.IsDead() || !Them.IsAttackable() || Them.IsAirborneTarget() || GetRelationship(Ship.GetOwner(), Them.GetOwner()) != EGenRelationship::Enemies)
		{
			return;
		}
		const FVector3f P = Them.GetPosition();
		const float Dist = FVector2f::Distance(FVector2f(P.X, P.Y), FVector2f(Center.X, Center.Y)) - Them.GetTemplate()->GetBoundingCircleRadius();
		if (Dist > Range || Dist >= BestDist)
		{
			return;
		}
		if (FVector2f::Distance(FVector2f(P.X, P.Y), FVector2f(Ship.GetPosition().X, Ship.GetPosition().Y)) <= TooNear)
		{
			return;
		}
		if (bShroudOn && Shroud.IsValid() && Shroud.GetStatusAt(Ship.GetOwner(), P.X, P.Y) != EGenCellShroud::Clear)
		{
			return;
		}
		BestDist = Dist;
		Best = &Them;
	});
	return Best;
}

void FGenObject::BeginGunshipRun(const FVector3f& Target, uint32 Frame)
{
	// SpectreGunshipUpdate::initiateIntentToDoSpecialPower
	const FGenSpectreGunship& D = Template->Gunship;
	Gunship.Reset(new FGenGunshipState());
	FGenGunshipState& G = *Gunship;
	G.Status = GunshipInserting;
	G.InitialTarget = G.OverrideTarget = G.GattlingTarget = G.ShootAt = Target;
	// (It is made at the height its locomotor likes, and on its fast one climbs or sinks to that one's.)
	if (Locomotor)
	{
		Position.Z = Logic->GetGroundHeight(Position.X, Position.Y) + Locomotor->PreferredHeight;
	}
	ChooseLocomotorSet(TEXT("SET_PANIC"));
	if (Locomotor)
	{
		// (Off the map there is nothing to slow down for: it comes in at its speed.)
		const FVector2f Dir = GetDirection2D();
		Velocity = FVector3f(Dir.X, Dir.Y, 0.f) * Locomotor->Speed;
	}
	SnapshotPrevious();
	SetModelCondition(Door1OpeningBit(), false);
	SetModelCondition(Door1ClosingBit(), true);
	SetModelCondition(AfterburnerBit(), true);

	// "Tell the gunners aboard the gunship to hold their fire until orbit insertion": the gattling gun
	// rides along (it is given nothing to shoot at until then).
	const FGenThingTemplate* GunT = D.GattlingTemplate.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(D.GattlingTemplate);
	if (GunT && Contain)
	{
		if (FGenObject* Gun = Logic->CreateObject(GunT, Owner, Position, Orientation))
		{
			Gun->Producer = ID;
			Logic->AddToContain(*this, *Gun);
			G.Gattling = Gun->GetID();
		}
	}
}

bool FGenObject::IsGunshipSteerable() const
{
	return Gunship && !bDead && (Gunship->Status == GunshipInserting || Gunship->Status == GunshipOrbiting);
}

bool FGenObject::SteerGunship(const FVector3f& Pos)
{
	// SpectreGunshipUpdate::setSpecialPowerOverridableDestination (kept inside the attack area by the update)
	if (!IsGunshipSteerable() || IsDisabled())
	{
		return false;
	}
	Gunship->OverrideTarget = Pos;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [gunship f%u] #%u is told to fire at (%.0f, %.0f)"), Logic->GetFrame(), ID, Pos.X, Pos.Y);
	return true;
}

bool FGenObject::GetGunshipTargets(FVector3f& OutArea, float& OutAreaRadius, FVector3f& OutReticle, float& OutReticleRadius) const
{
	if (!IsGunshipSteerable())
	{
		return false;
	}
	OutArea = Gunship->InitialTarget;
	OutAreaRadius = Template->Gunship.AttackAreaRadius;
	OutReticle = Gunship->OverrideTarget;
	OutReticleRadius = Template->Gunship.TargetingReticleRadius;
	return true;
}

void FGenObject::GunshipDepart(uint32 Frame)
{
	// SpectreGunshipUpdate::disengageAndDepartAO: "head off the map in the direction you are facing" - cease
	// fire, doors shut, the fast locomotor again; cleanUp takes the gattling away.
	FGenGunshipState& G = *Gunship;
	G.Status = GunshipDeparting;
	G.ExitDir = GetDirection2D();
	if (FGenObject* Gun = Logic->FindObject(G.Gattling))
	{
		Gun->ClearAttack();
		Gun->bPendingDestroy = true;
	}
	G.Gattling = GenInvalidObjectID;
	ChooseLocomotorSet(TEXT("SET_PANIC"));
	SetModelCondition(Door1OpeningBit(), false);
	SetModelCondition(Door1ClosingBit(), true);
	SetModelCondition(AfterburnerBit(), true);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [gunship f%u] #%u %s leaves its orbit at (%.0f, %.0f)"), Frame, ID, *Template->Name, Position.X, Position.Y);
}

bool FGenObject::UpdateGunship(uint32 Frame)
{
	// SpectreGunshipUpdate::update
	GEN_PERF_SCOPE("Obj.Gunship");
	FGenGunshipState& G = *Gunship;
	const FGenSpectreGunship& D = Template->Gunship;
	if (G.Status == GunshipIdle || !Locomotor)
	{
		return true;
	}
	bJetDriving = true; // this module flies the aircraft, not a move order
	UE_CLOG(Logic->IsTracing() && Frame % 90 == 0, LogGenerals, Log, TEXT("  [gunship f%u] #%u status %d at (%.0f, %.0f) %.0f up, %.0f a second on %s, heading %.0f"), Frame, ID, int32(G.Status),
		Position.X, Position.Y, Position.Z - Logic->GetGroundHeight(Position.X, Position.Y), FVector2f(Velocity.X, Velocity.Y).Size() * float(GenLogic::GetLogicFPS()),
		*Locomotor->Name, FMath::RadiansToDegrees(Orientation));
	FGenObject* Gun = Logic->FindObject(G.Gattling);
	if (Gun)
	{
		Gun->SetOrientation(Orientation); // (it rides on the ship's side: its turret turns from there)
	}

	if (G.Status == GunshipInserting || G.Status == GunshipOrbiting)
	{
		// The perigee is where it is, seen from the target; the apogee a quarter turn on, anticlockwise.
		// The point it flies to lies between the two, by the insertion slope, on the orbit's circle.
		FVector2f Perigee(Position.X - G.InitialTarget.X, Position.Y - G.InitialTarget.Y);
		const float DistToTarget = Perigee.Size();
		Perigee = DistToTarget > 0.01f ? Perigee / DistToTarget : FVector2f(1.f, 0.f);
		const FVector2f Apogee(-Perigee.Y, Perigee.X);
		const float N1 = FMath::Clamp(D.OrbitInsertionSlope, OrbitInsertionSlopeMin, OrbitInsertionSlopeMax);
		const FVector2f Declination = (Perigee * N1 + Apogee * (1.f - N1)) * D.OrbitRadius;
		G.Satellite = FVector3f(G.InitialTarget.X + Declination.X, G.InitialTarget.Y + Declination.Y, Position.Z);
		LocoMoveTowardsPosition(G.Satellite, 99999.f, Locomotor->Speed, Frame);

		// "Constrain Target Override to the targeting radius."
		const float Constraint = FMath::Max(0.f, D.AttackAreaRadius - D.TargetingReticleRadius);
		FVector2f Over(G.InitialTarget.X - G.OverrideTarget.X, G.InitialTarget.Y - G.OverrideTarget.Y);
		if (Over.Size() > Constraint)
		{
			Over = Over.GetSafeNormal() * Constraint;
			G.OverrideTarget.X = G.InitialTarget.X - Over.X;
			G.OverrideTarget.Y = G.InitialTarget.Y - Over.Y;
			G.OverrideTarget.Z = Logic->GetGroundHeight(G.OverrideTarget.X, G.OverrideTarget.Y);
		}

		if (G.Status == GunshipInserting && DistToTarget < D.OrbitRadius)
		{
			// Close enough to shoot.
			G.Status = GunshipOrbiting;
			G.OrbitEscapeFrame = Frame + uint32(FMath::Max(1, D.OrbitFrames));
			ChooseLocomotorSet(TEXT("SET_NORMAL"));
			SetModelCondition(Door1ClosingBit(), false);
			SetModelCondition(Door1OpeningBit(), true);
			SetModelCondition(AfterburnerBit(), false);
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [gunship f%u] #%u %s is in its orbit at (%.0f, %.0f), %.0f up, until f%u"), Frame, ID, *Template->Name,
				Position.X, Position.Y, Position.Z - Logic->GetGroundHeight(Position.X, Position.Y), G.OrbitEscapeFrame);
		}
	}

	if (G.Status == GunshipOrbiting)
	{
		if (Frame >= G.OrbitEscapeFrame)
		{
			GunshipDepart(Frame);
			return true;
		}
		// "Only every few frames do we re-evaluate the target object."
		if (Frame % uint32(FMath::Max(1, D.HowitzerFiringRate)) == 0)
		{
			G.ShootAt = G.OverrideTarget; // "unless we get a hit, below"
			FGenObject* Victim = Logic->FindGunshipVictim(*this, G.OverrideTarget, D.TargetingReticleRadius);
			// "We want the wide-range autoacquire power disabled for human players so that the Spectre
			// gunship requires babysitting at all times."
			const TArray<FGenPlayer>& Players = Logic->GetPlayers();
			if (!Victim && Players.IsValidIndex(Owner) && !Players[Owner].bHuman)
			{
				Victim = Logic->FindGunshipVictim(*this, G.InitialTarget, D.AttackAreaRadius);
				if (Victim)
				{
					G.ShootAt = Victim->GetPosition();
				}
			}
			// "Let's keep a constant barrage of gattling bullets on the current aim location."
			if (Gun)
			{
				if (Victim)
				{
					if (Gun->GetTargetID() != Victim->GetID())
					{
						Gun->CommandAttack(Victim->GetID());
					}
				}
				else
				{
					Gun->CommandAttackPosition(G.GattlingTarget, LotsOfShots, 0);
				}
			}
			// "If the gattling gun has been plinking the target long enough, let's follow with a volley of howitzer fire."
			const FGenWeaponTemplate* Howitzer = G.OkToFireHowitzer > D.HowitzerFollowLag ? FGenGameData::Get().FindWeapon(D.HowitzerWeapon) : nullptr;
			if (Howitzer)
			{
				FRandomStream& Rand = Logic->GetRandom();
				FVector3f At = G.GattlingTarget;
				At.X += Rand.FRandRange(-D.RandomOffsetForHowitzer, D.RandomOffsetForHowitzer);
				At.Y += Rand.FRandRange(-D.RandomOffsetForHowitzer, D.RandomOffsetForHowitzer);
				Logic->FireTempWeapon(*Howitzer, *this, At);
				Logic->PlayObjectSound(*this, { TEXT("HowitzerFire") });
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [gunship f%u] #%u howitzer at (%.0f, %.0f)"), Frame, ID, At.X, At.Y);
			}
		}

		// The gattling's aim: "I am going to wind my gattling gun toward the next good spot, whether an
		// object's position or a cursor position" - while it is firing.
		const uint32 FPS = uint32(GenLogic::GetLogicFPS());
		const bool bGunFiring = Gun && Gun->GetWeapon(0).FireCount > 0 && Frame - Gun->GetWeapon(0).LastShotFrame <= FPS / 5;
		if (bGunFiring)
		{
			const float Increment = D.StrafingIncrement * 30.f / float(FPS);
			FVector3f Delta = G.ShootAt - G.GattlingTarget;
			Delta.Z = 0.f;
			if (Delta.Size() < Increment)
			{
				G.GattlingTarget = G.ShootAt;
				++G.OkToFireHowitzer;
			}
			else
			{
				G.OkToFireHowitzer = 0;
				G.GattlingTarget += Delta.GetSafeNormal() * Increment;
			}
			G.GattlingTarget.Z = Logic->GetGroundHeight(G.GattlingTarget.X, G.GattlingTarget.Y);
			// "The client smoke effects of the gattling cannon strafing the ground toward the attack position"
			// (as often as the original's frames come round, and only if the gunship can be seen).
			if (!D.GattlingStrafeFX.IsEmpty() && Frame % FMath::Max(1u, FPS / 30u) == 0 && (!Logic->IsShroudOn() || GetLocalShroud() <= EGenObjectShroud::PartialClear))
			{
				FVector3f Impact = G.GattlingTarget;
				Impact.X += FMath::FRandRange(-5.f, 5.f);
				Impact.Y += FMath::FRandRange(-5.f, 5.f);
				Impact.Z = Logic->GetGroundHeight(Impact.X, Impact.Y);
				Logic->PlayParticleSystemAt(D.GattlingStrafeFX, Impact);
			}
		}
	}
	else if (G.Status == GunshipDeparting)
	{
		LocoMoveTowardsPosition(FVector3f(Position.X + G.ExitDir.X * 1000.f, Position.Y + G.ExitDir.Y * 1000.f, Position.Z), 99999.f, Locomotor->Speed, Frame);
		FVector2f Lo, Hi;
		Logic->GetMapExtent(Lo, Hi, true);
		if (Position.X < Lo.X || Position.Y < Lo.Y || Position.X > Hi.X || Position.Y > Hi.Y)
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [gunship f%u] #%u %s is off the map: gone"), Frame, ID, *Template->Name);
			G.Status = GunshipIdle;
			bPendingDestroy = true;
			return false;
		}
	}
	return true;
}
