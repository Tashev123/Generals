// Combat half of FGenObject: weapons, targeting, aiming, firing, projectiles, damage and death.
// Follows Weapon.cpp / WeaponSet.cpp / AIStates.cpp / ActiveBody.cpp / DumbProjectileBehavior.cpp.

#include "Logic/GenObject.h"
#include "Audio/GenAudioManager.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPathfinder.h"
#include "Generals1.h"

namespace
{
	/** AIAttackAimAtTargetState: extra tolerance added to the weapon's AcceptableAimDelta. */
	constexpr float AimThreshold = 0.035f;

	enum EFiringState { FS_None = 0, FS_Firing = 1, FS_Between = 2, FS_Reloading = 3, FS_PreAttack = 4 };

	int32 FiringConditionIndex(int32 Slot, int32 State)
	{
		// s_allWeaponFireFlags / WeaponSet::getModelConditionForWeaponSlot
		static const TCHAR* Names[5] = { nullptr, TEXT("FIRING_"), TEXT("BETWEEN_FIRING_SHOTS_"), TEXT("RELOADING_"), TEXT("PREATTACK_") };
		static int32 Cache[3][5] = { { -2, -2, -2, -2, -2 }, { -2, -2, -2, -2, -2 }, { -2, -2, -2, -2, -2 } };
		if (State <= 0 || State > 4) { return -1; }
		if (Cache[Slot][State] == -2)
		{
			Cache[Slot][State] = GenConditions::Index(FString(Names[State]) + TCHAR('A' + Slot));
		}
		return Cache[Slot][State];
	}

	int32 UsingWeaponIndex(int32 Slot)
	{
		static int32 Cache[3] = { -2, -2, -2 };
		if (Cache[Slot] == -2)
		{
			Cache[Slot] = GenConditions::Index(FString(TEXT("USING_WEAPON_")) + TCHAR('A' + Slot));
		}
		return Cache[Slot];
	}

	int32 DyingIndex() { static const int32 I = GenConditions::Index(TEXT("DYING")); return I; }

	/** DieMuxData::isDieApplicable: "ALL -CRUSHED -SPLATTED", "NONE +CRUSHED", "NORMAL", ... */
	bool DeathTypeApplies(const TArray<FString>& Tokens, const FString& DeathType)
	{
		if (Tokens.Num() == 0)
		{
			return true;
		}
		bool bApplies = false;
		for (const FString& T : Tokens)
		{
			if (T.Equals(TEXT("ALL"), ESearchCase::IgnoreCase)) { bApplies = true; }
			else if (T.Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { bApplies = false; }
			else if (T.StartsWith(TEXT("-"))) { if (T.Mid(1).Equals(DeathType, ESearchCase::IgnoreCase)) { bApplies = false; } }
			else if (T.StartsWith(TEXT("+"))) { if (T.Mid(1).Equals(DeathType, ESearchCase::IgnoreCase)) { bApplies = true; } }
			else if (T.Equals(DeathType, ESearchCase::IgnoreCase)) { bApplies = true; }
		}
		return bApplies;
	}

	/** Object::getStatus() for the states the game's data can name in ExemptStatus. Only the ones this code
	 *  keeps are answered; a rider slot (STATUS_RIDER1..7) is not modelled, so it never matches. */
	bool ObjectHasStatus(const FGenObject& Obj, const FString& Name)
	{
		if (Name.Equals(TEXT("UNDER_CONSTRUCTION"), ESearchCase::IgnoreCase)) { return Obj.IsUnderConstruction(); }
		if (Name.Equals(TEXT("HIJACKED"), ESearchCase::IgnoreCase)) { return Obj.IsHijacked(); }
		if (!Obj.GetRiderStatus().IsEmpty() && Name.Equals(Obj.GetRiderStatus(), ESearchCase::IgnoreCase)) { return true; } // STATUS_RIDERn
		return false;
	}

	/** DieMuxData::isDieApplicable: "if( m_requiredStatus.any() && !object->getStatusBits().testForAll( m_requiredStatus ) ) return FALSE". */
	bool StatusRequiredMet(const FGenObject& Obj, const TArray<FString>& Tokens)
	{
		for (const FString& T : Tokens)
		{
			if (!T.Equals(TEXT("NONE"), ESearchCase::IgnoreCase) && !ObjectHasStatus(Obj, T))
			{
				return false;
			}
		}
		return true;
	}

	/** DieMuxData::isDieApplicable's third check: "if( m_exemptStatus && (object->getStatus() & m_exemptStatus) ) return FALSE" —
	 *  a die module never runs for an object in a state it is exempt from, so a building site that is given
	 *  up does not do the death of a finished building. */
	bool StatusExemptApplies(const FGenObject& Obj, const TArray<FString>& Tokens)
	{
		bool bExempt = false;
		for (const FString& T : Tokens)
		{
			if (T.Equals(TEXT("ALL"), ESearchCase::IgnoreCase)) { bExempt = true; }
			else if (T.Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { bExempt = false; }
			else if (T.StartsWith(TEXT("-"))) { if (ObjectHasStatus(Obj, T.Mid(1))) { bExempt = false; } }
			else if (T.StartsWith(TEXT("+"))) { if (ObjectHasStatus(Obj, T.Mid(1))) { bExempt = true; } }
			else if (ObjectHasStatus(Obj, T)) { bExempt = true; }
		}
		return bExempt;
	}

	FVector3f Bezier(const FVector3f P[4], float T)
	{
		const float U = 1.f - T;
		return P[0] * (U * U * U) + P[1] * (3.f * U * U * T) + P[2] * (3.f * U * T * T) + P[3] * (T * T * T);
	}
}

// ---- setup / queries ----------------------------------------------------------------------------

void FGenObject::InitCombat()
{
	const FGenGameData& GD = FGenGameData::Get();
	if (const FGenWeaponSetEntry* WS = Template->FindWeaponSet())
	{
		WeaponSetEntry = WS;
		for (int32 s = 0; s < 3; ++s)
		{
			Weapons[s].Slot = s;
			Weapons[s].Template = GD.FindWeapon(WS->Weapons[s]);
			Weapons[s].AmmoInClip = Weapons[s].Template ? Weapons[s].Template->ClipSize : 0;
			bNoAutoChoose[s] = WS->bNoAutoChoose[s];
		}
	}
	if (const FGenArmorSetEntry* AS = Template->FindArmorSet())
	{
		Armor = GD.FindArmor(AS->Armor);
		DamageFX = GD.FindDamageFX(AS->DamageFX);
	}
	FlameDamageLimit = Template->Flammable.FlameDamageLimit;
	for (int32 t = 0; t < 2; ++t)
	{
		TurretAngle[t] = PrevTurretAngle[t] = DrawTurretAngle[t] = Template->Turrets[t].NaturalAngle;
	}
}

bool FGenObject::HasWeapons() const
{
	for (const FGenWeaponInstance& W : Weapons)
	{
		if (W.Template && W.Template->PrimaryDamage > 0.f)
		{
			return true;
		}
	}
	return false;
}

int32 FGenObject::GetDamageState() const
{
	if (ModelConditions.Test(GenConditions::Rubble())) { return 3; }
	if (ModelConditions.Test(GenConditions::ReallyDamaged())) { return 2; }
	return ModelConditions.Test(GenConditions::Damaged()) ? 1 : 0;
}

bool FGenObject::ContainsPoint(const FVector3f& P) const
{
	if (P.Z < Position.Z - 1.f || P.Z > Position.Z + FMath::Max(Template->GeometryHeight, 1.f) + 1.f)
	{
		return false;
	}
	const float Dx = P.X - Position.X, Dy = P.Y - Position.Y;
	if (Template->IsBox())
	{
		// Into the object's frame: major radius along its heading, minor radius across.
		const float C = FMath::Cos(Orientation), S = FMath::Sin(Orientation);
		return FMath::Abs(Dx * C + Dy * S) <= Template->GeometryMajorRadius && FMath::Abs(-Dx * S + Dy * C) <= Template->GeometryMinorRadius;
	}
	return Dx * Dx + Dy * Dy <= Template->GeometryMajorRadius * Template->GeometryMajorRadius;
}

bool FGenObject::IsTargetable() const
{
	if (bMasked || Template->Spawner.bAggregateHealth)
	{
		return false; // OBJECT_STATUS_MASKED (computeAggregateStates: "make sure no enemies are shooting at the nexus, since it doesn't 'exist'")
	}
	if (bDead || bPendingDestroy || Projectile.bActive || ContainedBy != GenInvalidObjectID || HijackedVehicle != GenInvalidObjectID || IsMineMasked())
	{
		return false; // (what is inside a container can only be got at through the container; a hijacker through his car)
	}
	// Objects without a real body (InactiveBody: trees, props, decorations) can't be damaged.
	if (Template->BodyType.IsEmpty() || Template->BodyType.Equals(TEXT("InactiveBody"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	return !Template->IsKindOf(TEXT("UNATTACKABLE")) && !Template->IsKindOf(TEXT("PROJECTILE"));
}

int32 FGenObject::TurretForSlot(int32 Slot) const
{
	for (int32 t = 0; t < 2; ++t)
	{
		if (Template->Turrets[t].bValid && (Template->Turrets[t].ControlledSlots & (1u << Slot)))
		{
			return t;
		}
	}
	return INDEX_NONE;
}

bool FGenObject::CanWeaponTarget(const FGenWeaponTemplate& W, const FGenObject& Target) const
{
	// WeaponTemplate anti-mask: aircraft up in the air need an anti-air weapon, everything else a ground one.
	if (W.PrimaryDamage <= 0.f)
	{
		return false;
	}
	if (Target.IsAirborneTarget())
	{
		return Target.IsKindOf(TEXT("INFANTRY")) ? W.bAntiAirborneInfantry : W.bAntiAirborneVehicle;
	}
	// getVictimAntiMask: a mine or a demo trap is ANTI_MINE | ANTI_GROUND; a disarming tool (AntiMine only)
	// is for nothing else.
	if (Target.IsKindOf(TEXT("MINE")) || Target.IsKindOf(TEXT("DEMOTRAP")))
	{
		return W.bAntiGround || W.bAntiMine;
	}
	return W.bAntiGround;
}

float FGenObject::AttackDistance(const FGenObject& Target) const
{
	// distCalcProc_BoundaryAndBoundary_2D: center distance minus both bounding circle radii.
	const float Center = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Target.Position.X, Target.Position.Y));
	return FMath::Max(0.f, Center - Template->GetBoundingCircleRadius() - Target.Template->GetBoundingCircleRadius());
}

bool FGenObject::IsWithinRangeFrom(const FVector2f& From, const FGenObject& Target, float Range) const
{
	const float Center = FVector2f::Distance(From, FVector2f(Target.Position.X, Target.Position.Y));
	return Center - Template->GetBoundingCircleRadius() - Target.Template->GetBoundingCircleRadius() <= Range;
}

bool FGenObject::StartApproach(const FGenObject& Target, float Range, float MinRange)
{
	// Pathfinder::adjustTargetDestination: aim for a point just inside weapon range on our side of the
	// target, then take the nearest free spot from which the weapon still reaches. Every attacker
	// reserves its own, so a group closes in side by side.
	const FVector2f TargetPos(Target.Position.X, Target.Position.Y);
	// WeaponTemplate::isContactWeapon ("like a car bomb", a knife): "let the attacker pathfind into the
	// target" - it goes for the target itself, and the target is no obstacle to it. (A building can only
	// be walked up to.)
	if (Range * 1.05f < GenLogic::PathfindCellSize)
	{
		bMovingOutOfWay = false;
		bHasResumeGoal = false;
		const FGenObjectID Victim = Target.ID;
		const bool bStarted = StartPath(Target.Position, false, nullptr, !Target.bImmobile);
		IgnoreObstacle = bStarted ? Victim : GenInvalidObjectID;
		ContactTarget = Victim;
		return bStarted;
	}
	if (!bFlies && Logic->ShouldQueuePath())
	{
		// (The frame's pathfinding is spent: asked again, target and all, when its turn comes.)
		FPendingPath Request;
		Request.Kind = FPendingPath::Approach;
		Request.Target = Target.ID;
		Request.Range = Range;
		Request.MinRange = MinRange;
		bMovingOutOfWay = false;
		bHasResumeGoal = false;
		return DeferPath(Request);
	}
	const FVector2f ToSelf = (FVector2f(Position.X, Position.Y) - TargetPos).GetSafeNormal();
	float Stand = Range * 0.9f + Template->GetBoundingCircleRadius() + Target.Template->GetBoundingCircleRadius();
	if (MinRange > 0.f && AttackDistance(Target) < MinRange + GenLogic::PathfindCellSize)
	{
		// (Pathfinder::findAttackPath looks for the nearest spot the weapon bears from: when the target is
		// too close that is just outside the minimum range, not out at the far end of its reach.)
		Stand = FMath::Min(Stand, MinRange + GenLogic::PathfindCellSize * 1.5f + Template->GetBoundingCircleRadius() + Target.Template->GetBoundingCircleRadius());
	}
	const FVector2f Dest = TargetPos + ToSelf * Stand;
	const FGenObject* TargetPtr = &Target;
	// (Weapon::isGoalPosWithinAttackRange: within reach, and not inside the weapon's minimum range.)
	const float Radii = Template->GetBoundingCircleRadius() + Target.Template->GetBoundingCircleRadius();
	const TFunction<bool(const FVector2f&)> InRange = [this, TargetPtr, Range, MinRange, Radii](const FVector2f& P)
	{
		return IsWithinRangeFrom(P, *TargetPtr, Range)
			&& (MinRange <= 0.f || FVector2f::Distance(P, FVector2f(TargetPtr->Position.X, TargetPtr->Position.Y)) - Radii >= MinRange + GenLogic::PathfindCellSize * 0.5f);
	};
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	return StartPath(FVector3f(Dest.X, Dest.Y, Position.Z), false, &InRange);
}

bool FGenObject::StartApproachPos(const FVector3f& Spot, float Range, float MinRange)
{
	// AIAttackApproachTargetState on a position: a free spot within range of it, on our side.
	if (!bFlies && Logic->ShouldQueuePath())
	{
		FPendingPath Request;
		Request.Kind = FPendingPath::ApproachPos;
		Request.Goal = Spot;
		Request.Range = Range;
		Request.MinRange = MinRange;
		bMovingOutOfWay = false;
		bHasResumeGoal = false;
		return DeferPath(Request);
	}
	const FVector2f SpotPos(Spot.X, Spot.Y);
	const float Radius = Template->GetBoundingCircleRadius();
	FVector2f ToSelf = (FVector2f(Position.X, Position.Y) - SpotPos).GetSafeNormal();
	if (ToSelf.IsNearlyZero())
	{
		ToSelf = FVector2f(-FMath::Cos(Orientation), -FMath::Sin(Orientation)); // (standing on it: back off the way it came)
	}
	float Stand = Range * 0.9f + Radius;
	if (MinRange > 0.f)
	{
		Stand = FMath::Min(Stand, MinRange + GenLogic::PathfindCellSize * 1.5f + Radius);
	}
	const FVector2f Dest = SpotPos + ToSelf * Stand;
	const TFunction<bool(const FVector2f&)> InRange = [SpotPos, Range, MinRange, Radius](const FVector2f& P)
	{
		const float D = FVector2f::Distance(P, SpotPos) - Radius;
		return D <= Range && (MinRange <= 0.f || D >= MinRange + GenLogic::PathfindCellSize * 0.5f);
	};
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	return StartPath(FVector3f(Dest.X, Dest.Y, Position.Z), false, &InRange);
}

bool FGenObject::ProjectileClearsGarrison(FGenObjectID BuildingId)
{
	const FGenDumbProjectile& D = Template->DumbProjectile;
	FGenObject* Building = Logic->FindObject(BuildingId);
	if (D.GarrisonHitKillCount <= 0 || Projectile.bNoDamage || !Building || !Logic->IsClearableGarrison(*Building))
	{
		return false;
	}
	const FGenObjectID Shooter = Logic->FindObject(Projectile.Launcher) ? Projectile.Launcher : ID;
	if (Logic->KillGarrisoned(*Building, D.GarrisonHitKillCount, D.GarrisonHitKillRequired, D.GarrisonHitKillForbidden, Shooter) == 0)
	{
		return false;
	}
	if (!D.GarrisonHitKillFX.IsEmpty())
	{
		Logic->DoFXObj(D.GarrisonHitKillFX, *Building); // "fx is played at center of building, not at grenade's location"
	}
	bPendingDestroy = true; // "don't do the normal explosion; just destroy ourselves"
	return true;
}

float FGenObject::EstimateWeaponDamage(const FGenWeaponTemplate& W, const FGenObject& Target) const
{
	const FString& Type = W.DamageType;
	if (Target.IsKindOf(TEXT("SHRUBBERY")))
	{
		// "a nonzero value, to ensure we can target shrubbery with flame weapons"
		return W.DeathType.Equals(TEXT("BURNED"), ESearchCase::IgnoreCase) ? 1.f : 0.f;
	}
	// "Must be shooting a firebase or such, if there is noone home to take the bullet, return 0!"
	if (Type.Equals(TEXT("SNIPER"), ESearchCase::IgnoreCase) && Target.IsKindOf(TEXT("STRUCTURE"))
		&& ((Target.Contain && Logic->GetContainCount(Target) == 0) || Target.IsUnderConstruction()))
	{
		return 0.f;
	}
	const bool bClearable = Logic->IsClearableGarrison(Target);
	if ((Type.Equals(TEXT("SURRENDER"), ESearchCase::IgnoreCase) || W.bAllowAttackGarrisonedBldgs) && bClearable)
	{
		return 1.f; // "to ensure we can target garrisoned things with surrender weapons, regardless"
	}
	if (Type.Equals(TEXT("DISARM"), ESearchCase::IgnoreCase))
	{
		return (Target.IsKindOf(TEXT("MINE")) || Target.IsKindOf(TEXT("BOOBY_TRAP")) || Target.IsKindOf(TEXT("DEMOTRAP"))) ? 1.f : 0.f;
	}
	if (Type.Equals(TEXT("DEPLOY"), ESearchCase::IgnoreCase) && !Target.IsAirborneTarget())
	{
		return 1.f;
	}
	if (Type.Equals(TEXT("KILL_GARRISONED"), ESearchCase::IgnoreCase))
	{
		return bClearable ? 1.f : 0.f;
	}
	const float Amount = W.PrimaryDamage * GetWeaponBonus(W, int32(FGenWeaponBonus::Damage));
	return Target.Armor ? Target.Armor->AdjustDamage(Type, Amount) : Amount;
}

bool FGenObject::SwitchWeapon(int32 Slot)
{
	if (Slot < 0 || Slot > 2 || !Weapons[Slot].Template)
	{
		return false;
	}
	PermanentLockSlot = Slot;
	WeaponLockSlot = INDEX_NONE;
	return true;
}

int32 FGenObject::ChooseWeapon(const FGenObject& Target) const
{
	// WeaponSet::chooseBestWeaponForTarget: "if we have a locked weapon, just use that" ...
	const int32 Lock = GetWeaponLock();
	if (Lock != INDEX_NONE)
	{
		const FGenWeaponTemplate* Locked = Weapons[Lock].Template;
		if (Locked && CanWeaponTarget(*Locked, Target) && !IsWeaponOutOfAmmo(Lock))
		{
			return Lock;
		}
	}
	// ... otherwise (PREFER_MOST_DAMAGE) the highest armor-adjusted damage, among the slots that may be
	// chosen without being asked for (AutoChooseSources). "If a weapon matches the magic 'preferred' bits, it's
	// ALWAYS preferred over other weapons" (the Comanche's cannon against infantry, its missiles against
	// vehicles), and "Readiness trumps Criteria": one that is reloading is only the fallback. (Backwards, "so
	// that in the event of ties, the primary weapon is preferred".)
	const uint32 Now = Logic->GetFrame();
	int32 Best = INDEX_NONE, Backup = INDEX_NONE, FillIn = INDEX_NONE;
	float BestDamage = 0.f, BackupDamage = 0.f, FillInDamage = 0.f;
	for (int32 s = 2; s >= 0; --s)
	{
		const FGenWeaponTemplate* W = Weapons[s].Template;
		if (!W || bNoAutoChoose[s] || !CanWeaponTarget(*W, Target) || IsWeaponOutOfAmmo(s) || !IsWithinTargetPitch(*W, Target))
		{
			continue; // (a plane's missiles are gone until it has been home)
		}
		float Damage = EstimateWeaponDamage(*W, Target);
		if (Damage <= 0.f && !W->DamageType.Equals(TEXT("UNRESISTABLE"), ESearchCase::IgnoreCase))
		{
			continue; // "weapon would do no damage against this target"
		}
		bool bReady = Now >= Weapons[s].ReadyFrame;
		if (WeaponSetEntry && WeaponSetEntry->PreferredAgainst[s].ContainsByPredicate([&Target](const FString& Kind) { return Target.IsKindOf(*Kind); }))
		{
			Damage = 1e10f;
			bReady = true; // ("preferred weapons are also kept if they are merely reloading")
		}
		else if (bReady && Damage >= FillInDamage && AttackDistance(Target) <= GetAttackRange(*W))
		{
			FillIn = s;
			FillInDamage = Damage;
		}
		if (!bReady)
		{
			if (Damage >= BackupDamage) { BackupDamage = Damage; Backup = s; }
		}
		else if (Damage >= BestDamage)
		{
			BestDamage = Damage;
			Best = s;
		}
	}
	if (Best == INDEX_NONE)
	{
		Best = Backup; // "No ready weapon was found, so return the most suitable unready one."
	}
	// (Not the original: while a helicopter's preferred weapon is reloading its clip, another that is ready and in
	// reach fires meanwhile - the user wanted the Comanche's cannon at work on vehicles too, between its missile
	// volleys. Only an empty clip steps a preferred weapon aside, not its time between shots: a weapon stays what
	// the data says it is preferred against, so the Comanche's cannon keeps infantry and its missiles keep
	// vehicles and buildings.)
	if (Best != INDEX_NONE && FillIn != INDEX_NONE && FillIn != Best && IsKindOf(TEXT("AIRCRAFT")) && Weapons[Best].bReloading)
	{
		Best = FillIn;
	}
	// TurretAI's idle scan: when chooseBestWeaponForTarget finds nothing it may pick by itself, "No weapon at all
	// was found, so we go back to primary" - and the turret fires that at what it has found (the strategy
	// center's gun, PRIMARY NONE).
	if (Best == INDEX_NONE && bNoAutoChoose[0] && TurretForSlot(0) != INDEX_NONE)
	{
		const FGenWeaponTemplate* W = Weapons[0].Template;
		if (W && CanWeaponTarget(*W, Target) && !IsWeaponOutOfAmmo(0) && IsWithinTargetPitch(*W, Target) && EstimateWeaponDamage(*W, Target) > 0.f)
		{
			Best = 0;
		}
	}
	return Best;
}

FGenObject* FGenObject::FindAutoTarget() const
{
	GEN_PERF_SCOPE("Obj.FindAutoTarget");
	FGenObject* Best = nullptr;
	float BestDist = FLT_MAX;
	// Nothing further off than the longest of our weapons reaches is of interest.
	float MaxRange = 0.f;
	bool bMineTool = false;
	for (int32 s = 0; s < 3; ++s)
	{
		if (Weapons[s].Template)
		{
			MaxRange = FMath::Max(MaxRange, GetAttackRange(*Weapons[s].Template));
			bMineTool |= Weapons[s].Template->bAntiMine && !Weapons[s].Template->bAntiGround;
		}
	}
	// AIUpdateInterface::getNextMoodTarget looks as far as the unit sees: for a disarming tool, which only
	// reaches what it touches, that is where the mines it may clear are.
	const float MineRange = bMineTool ? GetVisionRange() : 0.f;
	MaxRange = FMath::Max(MaxRange, MineRange);
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), MaxRange + Template->GetBoundingCircleRadius(), [&](FGenObject& Other)
	{
		FGenObject* O = &Other;
		// (PartitionFilterStealthedAndUndetected: what is stealthed and not shown up is no target.)
		if (O == this || !O->IsAttackable() || Logic->GetRelationship(Owner, O->Owner) != EGenRelationship::Enemies)
		{
			return;
		}
		const float Dist = AttackDistance(*O);
		if (Dist > MaxRange || Dist > BestDist || (Dist == BestDist && Best && Best->ID < O->ID))
		{
			return;
		}
		if (O->Template->IsKindOf(TEXT("HULK")) || IgnoresAsAutoTarget(*O))
		{
			return;
		}
		const int32 Slot = ChooseWeapon(*O);
		if (Slot != INDEX_NONE && Dist <= FMath::Max(GetAttackRange(*Weapons[Slot].Template), Weapons[Slot].Template->bAntiMine ? MineRange : 0.f))
		{
			BestDist = Dist;
			Best = O;
		}
	});
	return Best;
}

// ---- orders -------------------------------------------------------------------------------------

void FGenObject::CommandAttack(FGenObjectID Target, bool bForce)
{
	AttackShotsLeft = -1;
	if (bDead || !HasWeapons() || Target == ID)
	{
		return;
	}
	// WeaponSet::getAbleToAttackSpecificObject: "If an object is stealthed and hasn't been detected yet,
	// then it is not a valid target to fire on."
	if (const FGenObject* Victim = Logic->FindObject(Target))
	{
		// ("force-attack allows you to attack disguised things, which also happen to be stealthed")
		if (Victim->IsStealthedAndUndetected() && Logic->GetRelationship(Owner, Victim->Owner) != EGenRelationship::Allies && !(bForce && Victim->IsDisguised()))
		{
			return;
		}
	}
	if (bJetAI)
	{
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::Attack;
		Cmd.Target = Target;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	TargetID = Target;
	bExplicitAttack = true;
	bForceAttack = bForce;
	NextRepathFrame = 0;
}

bool FGenObject::IsWithinTargetPitch(const FGenWeaponTemplate& W, const FGenObject& Target) const
{
	// Weapon::isWithinTargetPitch
	if ((W.MinTargetPitch <= -PI && W.MaxTargetPitch >= PI) || W.AttackRange * 1.05f < GenLogic::PathfindCellSize)
	{
		return true; // not limited, or a contact weapon
	}
	// "always good enough if dz is small, regardless of pitch"
	if (FMath::Abs(Target.Position.Z - Position.Z) < 10.f)
	{
		return true;
	}
	// GeometryInfo::calcPitches: from our middle to the top and to the bottom of the target.
	const FGenThingTemplate& TT = *Target.Template;
	const bool bSphere = TT.GeometryType.Equals(TEXT("SPHERE"), ESearchCase::IgnoreCase);
	const float CenterZ = Position.Z + (Template->GeometryType.Equals(TEXT("SPHERE"), ESearchCase::IgnoreCase) ? 0.f : Template->GeometryHeight * 0.5f);
	const float Dxy = FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(Target.Position.X, Target.Position.Y));
	const float MaxPitch = FMath::Atan2(Target.Position.Z + TT.GetMaxHeightAbovePosition() - CenterZ, Dxy);
	const float MinPitch = FMath::Atan2(Target.Position.Z - (bSphere ? TT.GeometryMajorRadius : 0.f) - CenterZ, Dxy);
	// "if there's any intersection between the the two pitch ranges, we're good to go."
	return (MinPitch >= W.MinTargetPitch && MinPitch <= W.MaxTargetPitch) || (MaxPitch >= W.MinTargetPitch && MaxPitch <= W.MaxTargetPitch)
		|| (MinPitch <= W.MinTargetPitch && MaxPitch >= W.MaxTargetPitch);
}

int32 FGenObject::GetPreAttackDelay(int32 Slot, const FGenObject& Target) const
{
	// Weapon::getPreAttackDelay: "Look for a reason to return zero and have no delay."
	const FGenWeaponInstance& WI = Weapons[Slot];
	const FGenWeaponTemplate& W = *WI.Template;
	if (W.PreAttackDelay <= 0)
	{
		return 0;
	}
	if (W.PrefireType == FGenWeaponTemplate::EPrefire::PerClip)
	{
		if (W.ClipSize > 0 && WI.AmmoInClip < W.ClipSize)
		{
			return 0; // "I only delay once a clip, and this is not the first shot"
		}
	}
	else if (W.PrefireType == FGenWeaponTemplate::EPrefire::PerAttack)
	{
		if (ConsecutiveVictim == Target.ID && ConsecutiveShots > 0)
		{
			return 0; // "I only delay once an attack, and I have already shot this guy"
		}
	}
	return FMath::Max(1, FMath::RoundToInt32(float(W.PreAttackDelay) * GetWeaponBonus(W, FGenWeaponBonus::PreAttack)));
}

bool FGenObject::TurnTurretTowardsPitch(int32 Turret, float Desired, float RateModifier)
{
	// TurretAI::friend_turnTowardsPitch
	const FGenTurretData& TD = Template->Turrets[Turret];
	if (!TD.bAllowsPitch)
	{
		return true;
	}
	Desired = GenLogic::NormalizeAngle(Desired);
	const float Rate = TD.PitchRate * RateModifier;
	const float Diff = GenLogic::AngleDiff(Desired, TurretPitch[Turret]);
	if (FMath::Abs(Diff) < Rate)
	{
		TurretPitch[Turret] = Desired;
		return true;
	}
	TurretPitch[Turret] = GenLogic::NormalizeAngle(TurretPitch[Turret] + (Diff > 0.f ? Rate : -Rate));
	return false;
}

void FGenObject::ClearAttack()
{
	AttackShotsLeft = -1;
	// "this can occur if we start a preattack (eg, bayonet) and the target moves out range before we can
	// actually "fire"... cancel this state to avoid confusion."
	for (FGenWeaponInstance& WI : Weapons)
	{
		WI.PreAttackDoneFrame = 0;
	}
	ConsecutiveVictim = GenInvalidObjectID;
	ConsecutiveShots = 0;
	TargetID = GenInvalidObjectID;
	bExplicitAttack = false;
	bAttackingPosition = false;
	bAttackPositionApproach = false;
	bAttackPositionLocked = false;
	bForceAttack = false;
	WeaponLockSlot = INDEX_NONE; // (LOCKED_TEMPORARILY: "just till the weapon is empty or the attack is done")
	bHasContinueOrigin = false;
	bIgnoringStealth = false;
	ContactTarget = GenInvalidObjectID;
}

// ---- per-frame combat ---------------------------------------------------------------------------

void FGenObject::SetFiringConditions(int32 Slot, int32 State)
{
	if (FiringState[Slot] == State)
	{
		return;
	}
	FiringState[Slot] = State;
	for (int32 s = 1; s <= 4; ++s)
	{
		SetModelCondition(FiringConditionIndex(Slot, s), s == State);
	}
	SetModelCondition(UsingWeaponIndex(Slot), State != FS_None);
}

bool FGenObject::AimAt(const FGenObject& Target, int32 Slot)
{
	return AimAtPoint(Target.Position, Target.bImmobile || !Target.IsAirborneTarget(), Slot);
}

bool FGenObject::AimAtPoint(const FVector3f& Point, bool bGroundTarget, int32 Slot)
{
	const FGenWeaponTemplate& W = *Weapons[Slot].Template;
	const float Desired = FMath::Atan2(Point.Y - Position.Y, Point.X - Position.X);
	const float Tolerance = W.AcceptableAimDelta + AimThreshold;
	const int32 T = TurretForSlot(Slot);
	if (ContainedBy != GenInvalidObjectID && (T == INDEX_NONE || Template->Turrets[T].TurnRate <= 0.f))
	{
		// Shooting out of a window or a firing port: no turning on the spot in there. (An add-on riding in
		// the open turns its turret like anything else.)
		SetOrientation(Desired);
		return true;
	}

	// (A turret that cannot turn is only there to allow firing on the move, as on the Comanche: the body aims.)
	if (T != INDEX_NONE && Template->Turrets[T].TurnRate > 0.f)
	{
		// TurretAI: rotate the turret (relative to the body) towards the target.
		const float DesiredRel = GenLogic::AngleDiff(Desired, Orientation);
		const float Diff = GenLogic::AngleDiff(DesiredRel, TurretAngle[T]);
		const float Rate = Template->Turrets[T].TurnRate;
		TurretAngle[T] = GenLogic::NormalizeAngle(TurretAngle[T] + FMath::Clamp(Diff, -Rate, Rate));
		TurretIdleSince[T] = Logic->GetFrame();
		bool bPitchAligned = true;
		const FGenTurretData& TD = Template->Turrets[T];
		if (TD.bAllowsPitch)
		{
			// TurretAIAimTurretState: "Some turrets want to fire at a set pitch, and some want to fire straight to the target"
			float DesiredPitch = TD.FirePitch;
			if (TD.FirePitch <= 0.f)
			{
				// "we want the angle from our Weapon to the target, not us to the target. Raise our side"
				FVector3f V = Point - Position;
				V.Z -= Template->GetMaxHeightAbovePosition() * 0.5f;
				const float Length = V.Size();
				const float ActualPitch = Length > 0.f ? FMath::Asin(V.Z / Length) : 0.f;
				DesiredPitch = FMath::Max(ActualPitch, TD.MinPitch);
				if (TD.GroundUnitPitch > 0.f && bGroundTarget)
				{
					// "As the unit gets closer, reduce the pitch so we don't shoot over him."
					DesiredPitch = FMath::Max(ActualPitch + TD.GroundUnitPitch * (Length / FMath::Max(1.f, GetAttackRange(W))), TD.MinPitch);
				}
			}
			bPitchAligned = TurnTurretTowardsPitch(T, DesiredPitch, 1.f);
		}
		return bPitchAligned && FMath::Abs(GenLogic::AngleDiff(DesiredRel, TurretAngle[T])) <= Tolerance;
	}

	if (!CanMove())
	{
		return true; // fixed weapons without a turret fire in any direction
	}
	// No turret: turn the whole unit (locoUpdate_moveTowardsAngle).
	const float Rel = GenLogic::AngleDiff(Desired, Orientation);
	const float Rate = Locomotor->TurnRate;
	SetOrientation(Orientation + FMath::Clamp(Rel, -Rate, Rate));
	return FMath::Abs(GenLogic::AngleDiff(Desired, Orientation)) <= Tolerance;
}

void FGenObject::FireWeapon(int32 Slot, FGenObject& Target, uint32 Frame)
{
	FGenWeaponInstance& WI = Weapons[Slot];
	const FGenWeaponTemplate& W = *WI.Template;
	FRandomStream& Rand = Logic->GetRandom();

	// WeaponTemplate::fireWeaponTemplate
	FVector3f Destination = Target.Position;
	// "for a sneaky offset, we always target a position rather than an object"
	FVector3f Sneaky;
	const bool bSneaky = Target.GetSneakyTargetingOffset(&Sneaky);
	if (bSneaky)
	{
		Destination += Sneaky;
	}
	// (A shot from a scatter pattern is a shot at a spot.)
	const bool bPattern = PickScatterTarget(Slot, Destination);
	float Scatter = W.ScatterRadius;
	if (W.ScatterRadiusVsInfantry > 0.f && Target.IsKindOf(TEXT("INFANTRY")))
	{
		Scatter += W.ScatterRadiusVsInfantry;
	}
	if (Scatter > 0.f)
	{
		const float R = Rand.FRandRange(0.f, Scatter);
		const float A = Rand.FRandRange(0.f, 2.f * PI);
		Destination.X += R * FMath::Cos(A);
		Destination.Y += R * FMath::Sin(A);
		Destination.Z = Logic->GetGroundHeight(Destination.X, Destination.Y);
	}
	if (bPattern)
	{
		Scatter = FMath::Max(Scatter, 99999.f); // (treated like any scattered shot below: it flies to the spot)
	}

	// Weapon::processRequestAssistance: "tell everyone like us within our assistance range who is free to do so to
	// assist us on this shot" (the Air Force General's Patriots).
	if (W.RequestAssistRange > 0.f)
	{
		Logic->RequestAssistance(*this, Target, W.RequestAssistRange);
	}
	// Weapon::fireWeapon, DAMAGE_DEPLOY: "atInterface->beginAssault( victimObj )" - the Troop Crawler's riders get out at it.
	if (Template->AssaultTransport.bValid && W.DamageType.Equals(TEXT("DEPLOY"), ESearchCase::IgnoreCase))
	{
		if (AssaultTarget != Target.ID)
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [assault f%u] #%u %s begins an assault on #%u %s"), Frame, ID, *Template->Name, Target.ID, *Target.Template->Name);
		}
		AssaultTarget = Target.ID;
	}
	// Fire effects play at the weapon's FX bone (muzzle) when the model has one; tracers fly to the victim's center.
	const FVector3f VictimCenter = Target.Position + FVector3f(0.f, 0.f, Target.Template->GeometryHeight * 0.5f);
	Logic->DoWeaponFireFX(*this, Slot, W, (Scatter > 0.f || bSneaky) ? Destination : VictimCenter);
	Logic->DoWeaponFireSound(*this, Slot, W);
	if (!W.FireOCL.IsEmpty())
	{
		Logic->DoOCL(W.FireOCL, *this);
	}

	if (!W.IsProjectile() && W.IsLaser())
	{
		// WeaponTemplate::fireWeaponTemplate, "if( firingWeapon->isLaser() )": the beam strikes at once.
		if (!bSneaky && (Scatter <= W.PrimaryDamageRadius || Scatter <= W.SecondaryDamageRadius))
		{
			// "The laser is close enough to damage the object, so just hit it directly."
			Logic->CreateWeaponLaser(*this, Slot, W, &Target, Target.Position);
			Logic->DealDamage(W, ID, W.bDamageDealtAtSelfPosition ? GenInvalidObjectID : Target.ID, Target.Position, false);
		}
		else
		{
			// "We are missing our intended target, so now we want to aim at the ground at the projectile offset."
			Logic->CreateWeaponLaser(*this, Slot, W, nullptr, Destination);
			Logic->DealDamage(W, ID, GenInvalidObjectID, Destination, false);
		}
	}
	else if (!W.IsProjectile())
	{
		// Instant-hit weapons always damage the victim; the damage arrives after the travel time.
		const float Delay = (Target.Position - Position).Size() / FMath::Max(W.WeaponSpeed, 0.001f);
		const FGenObjectID DamageVictim = (W.bDamageDealtAtSelfPosition || bSneaky) ? GenInvalidObjectID : Target.ID;
		const FVector3f DamagePos = W.bDamageDealtAtSelfPosition ? Position : (bSneaky ? Destination : Target.Position);
		if (Delay < 1.f)
		{
			Logic->DealDamage(W, ID, DamageVictim, DamagePos, false);
		}
		else
		{
			Logic->QueueDelayedDamage(W, ID, DamageVictim, DamagePos, Frame + uint32(FMath::CeilToInt32(Delay)));
		}
	}
	else
	{
		// Launch from the model's WeaponLaunchBone; without one, from roughly where a muzzle would be.
		const int32 T = TurretForSlot(Slot);
		const float FireAngle = Orientation + (T != INDEX_NONE ? TurretAngle[T] : 0.f);
		const float Reach = Template->GeometryMajorRadius * 0.9f;
		FVector3f LaunchPos(Position.X + FMath::Cos(FireAngle) * Reach, Position.Y + FMath::Sin(FireAngle) * Reach,
			Position.Z + FMath::Max(Template->GeometryHeight * 0.75f, 3.f));
		FTransform3f LaunchBone;
		const bool bHasBone = NextLaunchTransform(Slot, LaunchBone);
		if (bHasBone)
		{
			LaunchPos = LaunchBone.GetTranslation();
		}
		// Weapon::positionProjectileForLaunch: the shot takes the launch bone's place and the way it points.
		const FQuat4f BoneRotation = LaunchBone.GetRotation();
		// A scattered shot flies to a position instead of tracking the victim (but it knows what it was
		// meant for: a flash-bang lobbed at a garrisoned building still clears it when it comes down on it).
		if (FGenObject* Shot = Logic->SpawnProjectile(W, *this, (Scatter > 0.f || bSneaky) ? GenInvalidObjectID : Target.ID, Destination, LaunchPos, FireAngle,
			bHasBone ? &BoneRotation : nullptr))
		{
			Shot->Projectile.Aimed = Target.ID;
		}
	}

	FinishShot(Slot, Target.ID, Frame);
}

int32 FGenObject::GetClipSpent(int32 Slot) const
{
	const FGenWeaponInstance& WI = Weapons[Slot];
	return (WI.Template && WI.Template->ClipSize >= 2) ? FMath::Clamp(WI.Template->ClipSize - WI.AmmoInClip, 0, WI.Template->ClipSize) : 0;
}

bool FGenObject::PickScatterTarget(int32 Slot, FVector3f& InOutPos)
{
	FGenWeaponInstance& WI = Weapons[Slot];
	const FGenWeaponTemplate& W = *WI.Template;
	if (W.ScatterTargets.Num() == 0)
	{
		return false;
	}
	if (WI.ScatterUnused.Num() == 0)
	{
		// Weapon::rebuildScatterTargets
		for (int32 i = 0; i < W.ScatterTargets.Num(); ++i)
		{
			WI.ScatterUnused.Add(i);
		}
	}
	// "essentially a radius, but operates only on this scatterTarget table"
	const int32 Pick = Logic->GetRandom().RandRange(0, WI.ScatterUnused.Num() - 1);
	const FVector2f Offset = W.ScatterTargets[WI.ScatterUnused[Pick]] * W.ScatterTargetScalar;
	WI.ScatterUnused.RemoveAtSwap(Pick);
	InOutPos.X += Offset.X;
	InOutPos.Y += Offset.Y;
	InOutPos.Z = Logic->GetGroundHeight(InOutPos.X, InOutPos.Y);
	return true;
}

bool FGenObject::NextLaunchTransform(int32 Slot, FTransform3f& OutBone)
{
	// Weapon::privateFireWeapon: the launch bones take turns, ShotsPerBarrel shots each.
	FGenWeaponInstance& WI = Weapons[Slot];
	const int32 Barrels = Logic->GetWeaponBarrelCount(*this, Slot);
	if (WI.CurBarrel >= Barrels)
	{
		WI.CurBarrel = 0;
		WI.ShotsForCurBarrel = WI.Template->ShotsPerBarrel;
	}
	if (WI.ShotsForCurBarrel <= 0)
	{
		WI.ShotsForCurBarrel = WI.Template->ShotsPerBarrel;
	}
	const bool bFound = Logic->GetWeaponLaunchTransform(*this, Slot, OutBone, WI.CurBarrel);
	if (--WI.ShotsForCurBarrel <= 0)
	{
		++WI.CurBarrel;
		WI.ShotsForCurBarrel = WI.Template->ShotsPerBarrel;
	}
	return bFound;
}

void FGenObject::FireWeaponAtPosition(int32 Slot, const FVector3f& InPos, uint32 Frame)
{
	// WeaponTemplate::fireWeaponTemplate at a spot (AIAttackPosition; the ObjectCreationList's Attack nugget).
	FGenWeaponInstance& WI = Weapons[Slot];
	const FGenWeaponTemplate& W = *WI.Template;
	FRandomStream& Rand = Logic->GetRandom();
	FVector3f Destination = InPos;
	PickScatterTarget(Slot, Destination);
	if (W.ScatterRadius > 0.f)
	{
		const float R = Rand.FRandRange(0.f, W.ScatterRadius);
		const float A = Rand.FRandRange(0.f, 2.f * PI);
		Destination.X += R * FMath::Cos(A);
		Destination.Y += R * FMath::Sin(A);
		Destination.Z = Logic->GetGroundHeight(Destination.X, Destination.Y);
	}
	Logic->DoWeaponFireFX(*this, Slot, W, Destination);
	Logic->DoWeaponFireSound(*this, Slot, W);
	if (!W.FireOCL.IsEmpty())
	{
		Logic->DoOCL(W.FireOCL, *this);
	}
	if (!W.IsProjectile())
	{
		const float Delay = (Destination - Position).Size() / FMath::Max(W.WeaponSpeed, 0.001f);
		const FVector3f DamagePos = W.bDamageDealtAtSelfPosition ? Position : Destination;
		if (Delay < 1.f)
		{
			Logic->DealDamage(W, ID, GenInvalidObjectID, DamagePos, false);
		}
		else
		{
			Logic->QueueDelayedDamage(W, ID, GenInvalidObjectID, DamagePos, Frame + uint32(FMath::CeilToInt32(Delay)));
		}
	}
	else
	{
		const int32 T = TurretForSlot(Slot);
		const float FireAngle = Orientation + (T != INDEX_NONE ? TurretAngle[T] : 0.f);
		FVector3f LaunchPos(Position.X, Position.Y, Position.Z + FMath::Max(Template->GeometryHeight * 0.75f, 3.f));
		FTransform3f LaunchBone;
		const bool bHasBone = NextLaunchTransform(Slot, LaunchBone);
		if (bHasBone)
		{
			LaunchPos = LaunchBone.GetTranslation();
		}
		const FQuat4f BoneRotation = LaunchBone.GetRotation();
		Logic->SpawnProjectile(W, *this, GenInvalidObjectID, Destination, LaunchPos, FireAngle, bHasBone ? &BoneRotation : nullptr);
	}
	FinishShot(Slot, GenInvalidObjectID, Frame);
}

void FGenObject::CommandAttackPosition(const FVector3f& Pos, int32 MaxShots, int32 Slot)
{
	if (bDead || Slot < 0 || Slot > 2 || !Weapons[Slot].Template)
	{
		return;
	}
	ClearAttack();
	bAttackingPosition = true;
	AttackPosition = Pos;
	AttackPositionShots = FMath::Max(1, MaxShots);
	AttackPositionSlot = Slot;
}

bool FGenObject::ProjectileHitsBridgeDeck(bool bMayDetonate)
{
	// "note that we want to use getHighestLayerForDestination() here, so that anything even slightly below
	// the bridge translates into GROUND."
	const FGenPathfinder& PF = Logic->GetPathfinder();
	const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
	const int32 OldLayer = Layer;
	const int32 NewLayer = PF.GetHighestLayerForDestination(Position, Ground);
	Layer = uint8(NewLayer);
	if (!bMayDetonate || OldLayer == 0 || NewLayer != 0)
	{
		return false;
	}
	// "see if we' still in the bridge's xy area"
	const FVector3f High(Position.X, Position.Y, 9999.f);
	if (PF.GetHighestLayerForDestination(High, Ground) != OldLayer)
	{
		return false;
	}
	// "ensure we are slightly above the bridge, to account for fudge & sloppy art"
	constexpr float Fudge = 2.f;
	Position.Z = PF.GetLayerHeight(Position.X, Position.Y, OldLayer, Ground) + Fudge;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [bridge f%u] #%u %s comes down on bridge %d's deck at (%.0f, %.0f)"), Logic->GetFrame(), ID,
		*Template->Name, OldLayer, Position.X, Position.Y);
	return true;
}

void FGenObject::CommandFireAtPosition(const FVector3f& Pos, int32 MaxShots, int32 Slot)
{
	// AIGroup::groupAttackPosition -> AIUpdateInterface::aiAttackPosition. Force attack on the ground:
	// "make only Primary attack the ground" unless a weapon is locked (WeaponSet::chooseBestWeaponForTarget,
	// no victim) - a switch button's choice stays.
	const int32 UseSlot = Slot != INDEX_NONE ? Slot : (PermanentLockSlot != INDEX_NONE ? PermanentLockSlot : 0);
	if (bDead || UseSlot < 0 || UseSlot > 2 || !Weapons[UseSlot].Template)
	{
		return;
	}
	ClearAttack();
	bAttackingPosition = true;
	AttackPosition = Pos;
	AttackPosition.Z = Logic->GetGroundHeight(Pos.X, Pos.Y);
	AttackPositionShots = FMath::Max(1, MaxShots);
	AttackPositionSlot = UseSlot;
	bAttackPositionApproach = true;
	bAttackPositionLocked = Slot != INDEX_NONE;
	NextRepathFrame = 0;
}

bool FGenObject::UpdatePositionApproach(int32 Slot, uint32 Frame)
{
	// AttackStateMachine on a position: outOfWeaponRangePosition -> CHASE_TARGET (AIAttackApproachTargetState),
	// otherwise AIAttackAimAtTargetState and then the shot.
	const FGenWeaponTemplate& W = *Weapons[Slot].Template;
	const float Range = GetAttackRange(W);
	const FVector2f Spot(AttackPosition.X, AttackPosition.Y);
	const float Radius = Template->GetBoundingCircleRadius();
	// (Weapon::isWithinAttackRange on a position: from the edge of us to the spot.)
	const float Dist = FMath::Max(0.f, FVector2f::Distance(FVector2f(Position.X, Position.Y), Spot) - Radius);
	const bool bTooClose = W.MinimumAttackRange > 0.f && Dist < W.MinimumAttackRange + GenLogic::PathfindCellSize * 0.25f;
	if (bTooClose || Dist > Range)
	{
		Weapons[Slot].PreAttackDoneFrame = 0;
		if (!CanMove() || ContainedBy != GenInvalidObjectID)
		{
			bAttackingPosition = false; // (what can't move there can't do it: ATTACKRESULT_INVALID_SHOT)
			return false;
		}
		const float GoalDist = FVector2f::Distance(FVector2f(Goal.X, Goal.Y), Spot) - Radius;
		if (!bMovingOutOfWay && Frame >= NextRepathFrame && (!bMoving || GoalDist > Range || (W.MinimumAttackRange > 0.f && GoalDist < W.MinimumAttackRange)))
		{
			StartApproachPos(AttackPosition, Range, bTooClose ? W.MinimumAttackRange : 0.f);
			NextRepathFrame = Frame + uint32(GenLogic::GetLogicFPS());
		}
		return false;
	}
	// In range: it stops (or rolls on onto the spot it reserved, which is in range too) and aims.
	const bool bRollingToSpot = bMoving && FVector2f::Distance(FVector2f(Goal.X, Goal.Y), Spot) - Radius <= Range;
	if (bMoving && !bMovingOutOfWay && !bRollingToSpot)
	{
		CommandStop();
		Velocity.X = Velocity.Y = 0.f;
		bStatusBraking = false;
	}
	if (bMoving && TurretForSlot(Slot) == INDEX_NONE)
	{
		return false; // (only a turret aims on the move)
	}
	return AimAtPoint(AttackPosition, true, Slot);
}

void FGenObject::FinishShot(int32 Slot, FGenObjectID Victim, uint32 Frame)
{
	FGenWeaponInstance& WI = Weapons[Slot];
	const FGenWeaponTemplate& W = *WI.Template;
	FRandomStream& Rand = Logic->GetRandom();
	++WI.FireCount;
	WI.LastShotFrame = Frame;
	WI.PreAttackDoneFrame = 0;
	// FiringTracker::shotFired
	ConsecutiveShots = ConsecutiveVictim == Victim ? ConsecutiveShots + 1 : 1;
	ConsecutiveVictim = Victim;
	SetFiringConditions(Slot, FS_Firing);
	FiringConditionEnd[Slot] = Frame + 1;

	// Rate of fire and clips (Weapon::privateFireWeapon / reloadWithBonus).
	// (The RATE_OF_FIRE bonuses -- veterancy -- shorten both the time between shots and the reload.)
	const float RateOfFire = GetWeaponBonus(W, FGenWeaponBonus::RateOfFire);
	const int32 ShotDelay = FMath::RoundToInt32(float(Rand.RandRange(W.DelayBetweenShotsMin, FMath::Max(W.DelayBetweenShotsMin, W.DelayBetweenShotsMax))) / RateOfFire);
	WI.bReloading = false;
	if (W.ClipSize > 0 && --WI.AmmoInClip <= 0)
	{
		if (W.ReloadType != EGenReloadType::Auto)
		{
			// OUT_OF_AMMO: the clip stays empty (a plane reloads at its airfield).
			WI.AmmoInClip = 0;
			WI.ReadyFrame = Frame + FMath::Max(1, ShotDelay);
		}
		else
		{
			WI.AmmoInClip = W.ClipSize;
			WI.ReadyFrame = Frame + FMath::Max(1, FMath::RoundToInt32(float(W.ClipReloadTime) / RateOfFire));
			WI.bReloading = true;
			WI.ScatterUnused.Reset(); // (Weapon::rebuildScatterTargets: a fresh pattern with the fresh clip)
		}
	}
	else
	{
		WI.ReadyFrame = Frame + FMath::Max(1, ShotDelay);
	}
	NoteContinuousFire(W, WI, Victim, Frame);
}

void FGenObject::NoteContinuousFire(const FGenWeaponTemplate& W, const FGenWeaponInstance& WI, FGenObjectID Victim, uint32 Frame)
{
	// FiringTracker::shotFired (after the weapon has worked out when it may fire next)
	{
		// "We shoot faster at guys marked thusly"; "a ground shot or the lack of the status on the target
		// will clear this".
		static const FString TargetFaerieFire(TEXT("TARGET_FAERIE_FIRE"));
		const FGenObject* Marked = Victim != GenInvalidObjectID ? Logic->FindObject(Victim) : nullptr;
		if (Marked && Frame < Marked->FaerieFireUntilFrame)
		{
			BonusConditions.AddUnique(TargetFaerieFire);
		}
		else if (BonusConditions.Num() > 0)
		{
			BonusConditions.Remove(TargetFaerieFire);
		}
	}
	if (W.ContinuousFireOne == MAX_int32 && ContinuousLevel == 0 && ContinuousCooldownFrame == 0)
	{
		return; // a weapon that doesn't speed up, on something that isn't sped up
	}
	if (Victim == ContinuousVictim)
	{
		++ContinuousShots;
	}
	else if (Frame < ContinuousCooldownFrame)
	{
		// "Switching targets within the coast time is valid, and we will not spin down"
		++ContinuousShots;
		ContinuousVictim = Victim;
	}
	else
	{
		ContinuousShots = 1;
		ContinuousVictim = Victim;
	}
	ContinuousCooldownFrame = W.ContinuousFireCoast > 0 ? WI.ReadyFrame + uint32(W.ContinuousFireCoast) : 0;
	if (ContinuousLevel == 1)
	{
		// "Can either go up or down from here."
		if (ContinuousShots < W.ContinuousFireOne) { ContinuousFireCoolDown(); }
		else if (ContinuousShots > W.ContinuousFireTwo) { SetContinuousFireLevel(2, false); }
	}
	else if (ContinuousLevel == 2)
	{
		if (ContinuousShots < W.ContinuousFireTwo) { ContinuousFireCoolDown(); }
	}
	else if (ContinuousShots > W.ContinuousFireOne)
	{
		SetContinuousFireLevel(1, false);
	}
}

void FGenObject::SetContinuousFireLevel(int32 Level, bool bSlow)
{
	// FiringTracker::speedUp / coolDown: "these flags are exclusive, not cumulative"
	static const int32 SlowBit = GenConditions::Index(TEXT("CONTINUOUS_FIRE_SLOW"));
	static const int32 MeanBit = GenConditions::Index(TEXT("CONTINUOUS_FIRE_MEAN"));
	static const int32 FastBit = GenConditions::Index(TEXT("CONTINUOUS_FIRE_FAST"));
	static const FString MeanBonus = TEXT("CONTINUOUS_FIRE_MEAN"), FastBonus = TEXT("CONTINUOUS_FIRE_FAST");
	if (Level == 2 && ContinuousLevel != 2)
	{
		Logic->PlayObjectSound(*this, { TEXT("VoiceRapidFire") });
	}
	ContinuousLevel = uint8(Level);
	BonusConditions.Remove(MeanBonus);
	BonusConditions.Remove(FastBonus);
	if (Level == 1) { BonusConditions.Add(MeanBonus); }
	if (Level == 2) { BonusConditions.Add(FastBonus); }
	SetModelCondition(MeanBit, Level == 1);
	SetModelCondition(FastBit, Level == 2);
	SetModelCondition(SlowBit, Level == 0 && bSlow);
}

void FGenObject::ContinuousFireCoolDown()
{
	// FiringTracker::coolDown: "straight to zero from wherever it is" - winding down first, and stopped the
	// next time round.
	if (ContinuousLevel != 0)
	{
		SetContinuousFireLevel(0, true);
	}
	else
	{
		SetContinuousFireLevel(0, false);
		ContinuousCooldownFrame = 0;
	}
	ContinuousShots = 0;
	ContinuousVictim = GenInvalidObjectID;
}

void FGenObject::UpdateCombat(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Combat");

	// FiringTracker::update: "If it has been too long since I fired. I have to start over."
	if (ContinuousCooldownFrame != 0 && Frame > ContinuousCooldownFrame)
	{
		ContinuousCooldownFrame = Frame + uint32(GenLogic::GetLogicFPS());
		ContinuousFireCoolDown();
	}
	if (Delivery)
	{
		return; // an aircraft on a delivery run has its own orders
	}
	if (bTurretsDisabled)
	{
		// TurretAI::setTurretEnabled(false): a gun that is packed away neither aims nor fires.
		if (TargetID != GenInvalidObjectID || bAttackingPosition)
		{
			ClearAttack();
		}
		return;
	}
	// An aircraft on the ground, or busy taking off, landing or flying home, does not fight.
	const bool bJetBusy = bJetAI && (JetState != EGenJetState::None || !bJetAllowAirLoco);
	// Planes (unlike helicopters) attack only what they are sent against.
	const bool bNoAutoAcquire = bJetBusy || (bJetAI && !Template->bAutoAcquireEnemiesWhenIdle) || Template->bNoIdleAutoAcquire;

	FGenObject* Target = (TargetID != GenInvalidObjectID && !bJetBusy) ? Logic->FindObject(TargetID) : nullptr;
	// (AIAttackApproachTargetState / PursueTargetState: "If obj is stealthed, can no longer approach.")
	// (... nor one that is no enemy any more: a building whose occupants are dead or gone is its owner's
	// again - there is no force attack in the port yet that would keep at it.)
	// (And one that has come over to our side - a vehicle whose driver was sniped and that one of ours
	// has climbed into - is no target at all.)
	if (TargetID != GenInvalidObjectID && bHasContinueOrigin && (!Target || !Target->IsTargetable()))
	{
		// AIAttackFireWeaponState: "continueRange > 0 && victim gone" - on to the nearest of its player's
		// within the weapon's ContinueAttackRange of where the first one stood (the next mine).
		const int32 VictimOwner = Target ? Target->Owner : INDEX_NONE;
		const int32 Slot = GetWeaponLock() != INDEX_NONE ? GetWeaponLock() : 0;
		const FGenWeaponTemplate* W = Weapons[Slot].Template;
		FGenObject* Next = (W && W->ContinueAttackRange > 0.f) ? FindContinueTarget(ContinueOrigin, W->ContinueAttackRange, VictimOwner, bIgnoringStealth) : nullptr;
		if (Next)
		{
			TargetID = Next->ID;
			Target = Next;
		}
	}
	// (A force attack keeps at it whoever it belongs to - ATTACK_CONTINUED_TARGET_FORCED - and sees through
	// our own stealth and a disguise.)
	const bool bForcedSees = bForceAttack && Target && Target->IsTargetable() && (Target->Owner == Owner || Target->IsDisguised());
	if (TargetID != GenInvalidObjectID && (!Target || !(Target->IsAttackable() || (bIgnoringStealth && Target->IsTargetable()) || bForcedSees)
		|| (!bForceAttack && (Target->Owner == Owner || Logic->GetRelationship(Owner, Target->Owner) == EGenRelationship::Allies || Target->IsUnmanned()
		|| (Logic->GetRelationship(Owner, Target->Owner) != EGenRelationship::Enemies && Target->GetContain() && Target->GetTemplate()->Contain.Type == EGenContainType::Garrison)))))
	{
		ClearAttack();
		Target = nullptr;
		if (bJetAI && Template->bJetNeedsRunway && !bJetBusy)
		{
			JetReturnToBaseFrame = Frame; // JetAIUpdate::notifyVictimIsDead: job done, home we go
		}
	}

	// AIUpdateInterface::getNextMoodTarget: "Sorry, stealthed and not allowed to idle fire when stealthed.
	// Being in a firing container is an exception to this veto." (So is stealth from the GPS scrambler.)
	bool bStealthVeto = false;
	if (bStealthed && !Template->bAutoAcquireWhenStealthed && !Template->Stealth.bGrantedBySpecialPower)
	{
		const FGenObject* Container = IsContained() ? Logic->FindObject(ContainedBy) : nullptr;
		bStealthVeto = !(Container && Logic->IsPassengerAllowedToFire(*Container, *this));
	}

	// Idle units look for enemies in weapon range (AutoAcquireEnemiesWhenIdle).
	// (OBJECT_STATUS_IS_USING_ABILITY: raising a flag, it has no eyes for anything else.)
	// (Nor while it unpacks or packs for one: aiBusy.)
	if (!Target && !bAttackingPosition && !bMoving && !bNoAutoAcquire && !bStealthVeto && !bUsingAbility && AbilityAnimFrames == 0 && Frame >= NextAutoAcquireFrame)
	{
		NextAutoAcquireFrame = Frame + uint32(GenLogic::GetLogicFPS() / 2) + (ID % 7);
		if (FGenObject* Found = FindAutoTarget())
		{
			TargetID = Found->ID;
			// (A mine an idle dozer or worker has spotted is gone after, as the original's mood target is - it
			// has to be touched to be disarmed.)
			const int32 Slot = ChooseWeapon(*Found);
			const FGenWeaponTemplate* W = Slot != INDEX_NONE ? Weapons[Slot].Template : nullptr;
			bExplicitAttack = W && W->bAntiMine && !W->bAntiGround;
			if (W && W->ContinueAttackRange > 0.f)
			{
				// AIAttackState::onEnter: with a ContinueAttackRange it ignores stealth while at it (a mine it has
				// seen doesn't vanish from it when the detector looks away), and goes on to the next.
				bIgnoringStealth = true;
				ContinueOrigin = Found->Position;
				bHasContinueOrigin = true;
			}
			Target = Found;
		}
	}

	int32 ActiveSlot = INDEX_NONE;
	bool bAiming = false;
	if (bAttackingPosition)
	{
		// AIAttackState on a spot, the weapon locked (the ObjectCreationList's Attack nugget): shot after
		// shot until the count is reached. (A player's goes within range and aims first.)
		Target = nullptr;
		ActiveSlot = AttackPositionSlot;
		bAiming = true;
		FGenWeaponInstance& Firing = Weapons[ActiveSlot];
		if (!Firing.Template)
		{
			bAttackingPosition = false;
			ActiveSlot = INDEX_NONE;
		}
		else if ((!bAttackPositionApproach || UpdatePositionApproach(ActiveSlot, Frame)) && bAttackingPosition && Frame >= Firing.ReadyFrame && !IsDisabled() && IsDeployedToFire())
		{
			if (Firing.PreAttackDoneFrame == 0)
			{
				const int32 Delay = GetPreAttackDelay(ActiveSlot, *this);
				if (Delay > 0)
				{
					Firing.PreAttackDoneFrame = Frame + uint32(Delay);
					SetDrawAnimDuration(Delay);
				}
			}
			if (Firing.PreAttackDoneFrame == 0 || Frame >= Firing.PreAttackDoneFrame)
			{
				FireWeaponAtPosition(ActiveSlot, AttackPosition, Frame);
				if (--AttackPositionShots <= 0)
				{
					bAttackingPosition = false; // "the attack is done"
				}
				else if (bAttackPositionLocked && Firing.Template->ClipSize > 0 && (Firing.bReloading || Firing.AmmoInClip <= 0))
				{
					bAttackingPosition = false; // (the clip is empty: the temporary lock goes, and with it the attack)
				}
			}
		}
	}
	// AIAttackState::onEnter / onExit: MODELCONDITION_ATTACKING for as long as an attack is under way.
	{
		static const int32 Attacking = GenConditions::Index(TEXT("ATTACKING"));
		SetModelCondition(Attacking, bAttackingPosition || Target != nullptr);
	}
	if (Target)
	{
		ActiveSlot = ChooseWeapon(*Target);
		if (ActiveSlot == INDEX_NONE)
		{
			ClearAttack();
			Target = nullptr;
		}
	}

	if (Target && HasMinSpeed() && IsUsingAirLoco() && (bExplicitAttack || AttackDistance(*Target) <= GetAttackRange(*Weapons[ActiveSlot].Template)))
	{
		// A plane cannot stop to aim: it flies attack passes.
		UpdateJetAttack(*Target, ActiveSlot, Frame);
		bAiming = true;
	}
	else if (Target)
	{
		const FGenWeaponInstance& WI = Weapons[ActiveSlot];
		const float Dist = AttackDistance(*Target);
		float Range = GetAttackRange(*WI.Template);
		// (Weapon::isWithinAttackRange wants a contact weapon to be touching a building; here, having
		// walked up to it as near as the ground allows is touching it.)
		if (Range * 1.05f < GenLogic::PathfindCellSize && Target->bImmobile && !bMoving && ContactTarget == Target->ID)
		{
			Range += 15.f;
		}
		// Weapon::isWithinAttackRange: nothing inside the minimum range (and a quarter cell, "otherwise if
		// it teters on the edge, attacks can fail") can be fired at.
		const float MinRange = WI.Template->MinimumAttackRange;
		const bool bTooClose = MinRange > 0.f && Dist < MinRange + GenLogic::PathfindCellSize * 0.25f;
		if (bTooClose)
		{
			Weapons[ActiveSlot].PreAttackDoneFrame = 0;
			if (bExplicitAttack && CanMove() && ContainedBy == GenInvalidObjectID)
			{
				// (Pathfinder::findAttackPath takes it to where the weapon bears: further off.)
				if (!bMovingOutOfWay && (!bMoving || Frame >= NextRepathFrame))
				{
					StartApproach(*Target, Range, MinRange);
					NextRepathFrame = Frame + uint32(GenLogic::GetLogicFPS());
				}
			}
			else
			{
				ClearAttack(); // it can't get at this one from where it is
				ActiveSlot = INDEX_NONE;
			}
		}
		else if (Dist > Range)
		{
			Weapons[ActiveSlot].PreAttackDoneFrame = 0;
			if (bExplicitAttack && CanMove() && ContainedBy == GenInvalidObjectID)
			{
				// Approach: head for a free spot within range - and again when the target has moved so far
				// that the spot we are making for no longer reaches it. (It used to be planned afresh every
				// second regardless: an army marching on a base asked for its way hundreds of times.)
				if (!bMovingOutOfWay && (!bMoving || (Frame >= NextRepathFrame && !IsWithinRangeFrom(FVector2f(Goal.X, Goal.Y), *Target, Range))))
				{
					StartApproach(*Target, Range);
					NextRepathFrame = Frame + uint32(GenLogic::GetLogicFPS());
				}
			}
			else
			{
				ClearAttack(); // acquired targets are not chased
				ActiveSlot = INDEX_NONE;
			}
		}
		else
		{
			// In range. While still rolling onto the spot we reserved for this attack (it lies within range
			// too) keep going, so attackers line up next to each other instead of stopping in a heap where
			// they first got in range. Any other movement ends here, except stepping out of a unit's way.
			const bool bRollingToSpot = bMoving && IsWithinRangeFrom(FVector2f(Goal.X, Goal.Y), *Target, Range);
			if (bMoving && !bMovingOutOfWay && !bRollingToSpot)
			{
				CommandStop();
				Velocity.X = Velocity.Y = 0.f;
				bStatusBraking = false;
			}
			bAiming = true;
			// A turret can already track and fire on the move; everything else has to stand still to aim. (One that
			// deploys - the Nuke Cannon - neither aims nor fires until it has unpacked: DeployStyleAIUpdate.)
			if (IsDeployedToFire() && (!bMoving || TurretForSlot(ActiveSlot) != INDEX_NONE) && AimAt(*Target, ActiveSlot) && Frame >= WI.ReadyFrame)
			{
				// Object::preFireCurrentWeapon / AIAttackFireWeaponState: a weapon that winds up first
				// (PRE_ATTACK) fires when its delay has run; the model shows the winding up, at the pace it takes.
				FGenWeaponInstance& Firing = Weapons[ActiveSlot];
				if (Firing.PreAttackDoneFrame == 0)
				{
					const int32 Delay = GetPreAttackDelay(ActiveSlot, *Target);
					if (Delay > 0)
					{
						Firing.PreAttackDoneFrame = Frame + uint32(Delay);
						SetDrawAnimDuration(Delay);
					}
				}
				if (Firing.PreAttackDoneFrame == 0 || Frame >= Firing.PreAttackDoneFrame)
				{
					FireWeapon(ActiveSlot, *Target, Frame);
					if (AttackShotsLeft > 0 && --AttackShotsLeft == 0)
					{
						ClearAttack(); // (an attack of so many shots - an assisting Patriot's clip - is done)
					}
				}
			}
			else if (Frame < WI.ReadyFrame)
			{
				Weapons[ActiveSlot].PreAttackDoneFrame = 0;
			}
		}
	}

	// Turrets drift back to their natural angle when idle (TurretAI recenter).
	for (int32 t = 0; t < 2; ++t)
	{
		const FGenTurretData& TD = Template->Turrets[t];
		if (!TD.bValid || (ActiveSlot != INDEX_NONE && TurretForSlot(ActiveSlot) == t))
		{
			continue;
		}
		if (Frame > TurretIdleSince[t] + uint32(FMath::Max(TD.RecenterTime, GenLogic::GetLogicFPS() * 2)))
		{
			const float Diff = GenLogic::AngleDiff(TD.NaturalAngle, TurretAngle[t]);
			TurretAngle[t] = GenLogic::NormalizeAngle(TurretAngle[t] + FMath::Clamp(Diff, -TD.TurnRate, TD.TurnRate));
			TurnTurretTowardsPitch(t, TD.NaturalPitch, 0.5f); // TurretAIRecenterTurretState
		}
	}

	// Object::adjustModelConditionForWeaponStatus
	bCombatPosesSet = bAttackingPosition || Target != nullptr;
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenWeaponInstance& WI = Weapons[s];
		int32 State = FS_None;
		if (WI.Template && s == ActiveSlot)
		{
			if (FiringState[s] == FS_Firing && Frame < FiringConditionEnd[s])
			{
				State = FS_Firing;
			}
			else if (WI.PreAttackDoneFrame != 0 && Frame < WI.PreAttackDoneFrame)
			{
				State = FS_PreAttack;
			}
			else if (Frame < WI.ReadyFrame)
			{
				State = WI.bReloading ? FS_Reloading : FS_Between;
			}
			else if (bAiming)
			{
				State = FS_Between; // keep the firing pose between bursts
			}
		}
		SetFiringConditions(s, State);
		bCombatPosesSet |= State != FS_None;
	}
}

static TAutoConsoleVariable<int32> CVarGenMissileFlight(
	TEXT("gen.MissileFlight"), 1,
	TEXT("1: missiles fly themselves on their thrust locomotors, as in the original. 0: straight at the target, as before."));

// ---- damage and death ---------------------------------------------------------------------------

void FGenObject::AttemptDamage(float Amount, const FString& DamageType, const FString& DeathType, FGenObjectID Source, bool bInFlight, const TCHAR* DamageFXType)
{
	const bool bShotDown = bInFlight && Projectile.bActive && !bDead && !bPendingDestroy;
	if ((!IsTargetable() && !bShotDown) || Logic->GetFrame() < InvulnerableUntilFrame || bIndestructible)
	{
		return;
	}
	if (Template->HivePropagateDamage.Num() > 0 && Template->HivePropagateDamage.Contains(DamageType.ToUpper()))
	{
		// HiveStructureBody::attemptDamage: the spawn nearest whoever shoots takes it instead - and damage of some
		// kinds (a sniper's) is lost on the building when there is nobody to take it.
		if (const FGenObject* Shooter = Logic->FindObject(Source))
		{
			if (FGenObject* Spawn = GetClosestSpawn(Shooter->Position))
			{
				Spawn->AttemptDamage(Amount, DamageType, DeathType, Source);
				return;
			}
			if (Template->HiveSwallowDamage.Contains(DamageType.ToUpper()))
			{
				return;
			}
		}
	}
	// UndeadBody::attemptDamage: the blow that would kill it the first time leaves it one point (before the armor
	// has its say), and its second life starts.
	bool bStartSecondLife = false;
	if (Template->SecondLifeMaxHealth > 0.f && !bSecondLife && !DamageType.Equals(TEXT("UNRESISTABLE"), ESearchCase::IgnoreCase) && Amount >= Health
		&& !DamageType.Equals(TEXT("STATUS"), ESearchCase::IgnoreCase) && !DamageType.StartsWith(TEXT("SUBDUAL"), ESearchCase::IgnoreCase)
		&& !DamageType.Equals(TEXT("KILL_PILOT"), ESearchCase::IgnoreCase) && !DamageType.Equals(TEXT("KILL_GARRISONED"), ESearchCase::IgnoreCase)
		&& !DamageType.Equals(TEXT("DISARM"), ESearchCase::IgnoreCase) && !DamageType.Equals(TEXT("DEPLOY"), ESearchCase::IgnoreCase)
		&& !DamageType.Equals(TEXT("HEALING"), ESearchCase::IgnoreCase))
	{
		Amount = FMath::Max(0.f, FMath::Min(Amount, Health - 1.f));
		bStartSecondLife = true;
		if (Amount <= 0.f)
		{
			StartSecondLife(DeathType);
			return;
		}
	}
	if (Armor)
	{
		Amount = Armor->AdjustDamage(DamageType, Amount);
	}
	// ActiveBody::attemptDamage: "Apply the damage scalar (extra bonuses -- like strategy center defensive battle
	// plan) ... not to adjust unresistable damage, just like the armor code can't."
	if (DamageScalar != 1.f && !DamageType.Equals(TEXT("UNRESISTABLE"), ESearchCase::IgnoreCase))
	{
		Amount *= DamageScalar;
	}
	if (DamageType.Equals(TEXT("KILL_GARRISONED"), ESearchCase::IgnoreCase))
	{
		// ActiveBody::attemptDamage: as many inside die as the damage says; the building itself is untouched.
		Logic->KillGarrisoned(*this, FMath::FloorToInt32(Amount), TArray<FString>(), TArray<FString>(), Source);
		return;
	}
	if (Amount <= 0.f)
	{
		return;
	}
	// PoisonedBehavior::onDamage: "if( damageInfo->in.m_damageType == DAMAGE_POISON ) startPoisonedEffects".
	if (Template->Poisoned.bValid && DamageType.Equals(TEXT("POISON"), ESearchCase::IgnoreCase))
	{
		StartPoison(Amount, DeathType);
	}
	// BaseRegenerateUpdate::onDamage: hurt, it mends itself only once it has been left alone for BaseRegenDelay.
	if (Template->bBaseRegenerate && !DamageType.Equals(TEXT("HEALING"), ESearchCase::IgnoreCase))
	{
		const FGenGameData& RegenData = FGenGameData::Get();
		BaseRegenFrame = RegenData.BaseRegenHealthPercentPerSecond > 0.f ? Logic->GetFrame() + uint32(RegenData.BaseRegenDelay) : MAX_uint32;
	}
	if (DamageType.Equals(TEXT("DISARM"), ESearchCase::IgnoreCase))
	{
		// Weapon::dealDamageInternal, DAMAGE_DISARM: a mine (a booby trap, a demo trap) is disarmed; nothing
		// else is touched.
		if (MineState || IsKindOf(TEXT("MINE")) || IsKindOf(TEXT("BOOBY_TRAP")) || IsKindOf(TEXT("DEMOTRAP")))
		{
			DisarmMine();
		}
		return;
	}
	if (DamageType.Equals(TEXT("KILL_PILOT"), ESearchCase::IgnoreCase))
	{
		// "This type of damage doesn't actually damage the unit, but it does kill its pilot, in the case of a vehicle."
		if (IsKindOf(TEXT("VEHICLE")))
		{
			MakeUnmanned(Source);
		}
		return;
	}
	if (DamageType.Equals(TEXT("STATUS"), ESearchCase::IgnoreCase))
	{
		// ActiveBody::attemptDamage: "damage amount is msec time we set the status given in
		// damageStatusType" - FAERIE_FIRE, the only one the data asks for. It doesn't hurt.
		FaerieFireUntilFrame = Logic->GetFrame() + uint32(FMath::Max(1, GenLogic::MsecToFrames(Amount)));
		return;
	}
	// (HighlanderBody: "Can take damage, but won't die. Can only die from ::kill() or other unresistable damage.")
	const bool bImmortal = Template->BodyType.Equals(TEXT("ImmortalBody"), ESearchCase::IgnoreCase)
		|| (Template->BodyType.Equals(TEXT("HighlanderBody"), ESearchCase::IgnoreCase) && !DamageType.Equals(TEXT("UNRESISTABLE"), ESearchCase::IgnoreCase));
	const int32 OldDamageState = GetDamageState();
	LastDamageFrame = Logic->GetFrame();
	// (DamageInfo's m_sourceID, m_sourcePlayerMask, m_sourceTemplate.)
	LastDamageSource = Source;
	if (const FGenObject* Damager = Logic->FindObject(Source))
	{
		LastDamageSourcePlayer = Damager->Owner;
		LastDamageSourceTemplate = Damager->Template;
	}
	PreviousHealth = Health;
	Health = FMath::Max(bImmortal ? 1.f : 0.f, Health - Amount);
	if (DamageFX)
	{
		DoDamageFX(DamageFXType ? FString(DamageFXType) : DamageType, Amount, Source);
	}
	if (Template->Flammable.bValid && (DamageType.Equals(TEXT("FLAME"), ESearchCase::IgnoreCase) || DamageType.Equals(TEXT("PARTICLE_BEAM"), ESearchCase::IgnoreCase)))
	{
		OnFlameDamage(Amount, LastDamageFrame);
	}
	if (MineState)
	{
		OnMineHealthChanged(false, Source, DamageType); // MinefieldBehavior::onDamage
		if (bPendingDestroy)
		{
			return;
		}
	}

	// ActiveBody::attemptDamage: "Should we play our fear sound?" - on falling below YELLOW_DAMAGE_PERCENT
	// (a quarter) of its health and still alive, "25% chance to play".
	if (MaxHealth > 0.f && PreviousHealth / MaxHealth > 0.25f && Health / MaxHealth < 0.25f && Health > 0.f
		&& Logic->GetRandom().RandRange(0, 99) < 25)
	{
		Logic->PlayObjectSound(*this, { TEXT("VoiceFear") });
	}

	// Object::attemptDamage: real damage to something of the local player's that shows on the radar,
	// done by somebody else, may raise the alarm there.
	if (Owner == Logic->GetLocalPlayer() && Template->RadarPriority != EGenRadarPriority::Invalid
		&& Template->RadarPriority != EGenRadarPriority::NotOnRadar && !DamageType.Equals(TEXT("PENALTY"), ESearchCase::IgnoreCase))
	{
		const FGenObject* Damager = Logic->FindObject(Source);
		if (!Damager || Damager->Owner != Owner)
		{
			Logic->TryUnderAttackEvent(*this);
		}
	}

	// ActiveBody::setCorrectDamageState -> Drawable::reactToBodyDamageStateChange: exactly one of
	// DAMAGED / REALLYDAMAGED / RUBBLE (zero health) is set.
	const FGenGameData& GD = FGenGameData::Get();
	const float Ratio = MaxHealth > 0.f ? Health / MaxHealth : 0.f;
	// (A construction site has little health by design: it only shows damage states once it is finished.)
	const bool bRubble = Ratio <= 0.f;
	const bool bReally = !bRubble && !bUnderConstruction && Ratio <= GD.UnitReallyDamagedThreshold;
	SetModelCondition(GenConditions::Rubble(), bRubble);
	SetModelCondition(GenConditions::ReallyDamaged(), bReally);
	SetModelCondition(GenConditions::Damaged(), !bRubble && !bReally && !bUnderConstruction && Ratio <= GD.UnitDamagedThreshold);

	// TransitionDamageFX: effects for crossing into a worse damage state.
	const int32 NewDamageState = GetDamageState();
	if (NewDamageState > OldDamageState)
	{
		Logic->DoTransitionDamageFX(*this, NewDamageState);
	}

	if (Health <= 0.f)
	{
		Die(DeathType, Source);
		return;
	}
	if (bStartSecondLife)
	{
		StartSecondLife(DeathType);
		return;
	}

	// Fight back when attacked while idle (parked aircraft and planes without orders don't).
	const bool bNoRetaliation = bJetAI && (JetState != EGenJetState::None || !bJetAllowAirLoco || !Template->bAutoAcquireEnemiesWhenIdle);
	// (ActiveBody::shouldRetaliate: "Stealthed units don't retaliate unless they're detected.")
	// (Nor does a unit in the middle of one of its abilities: it sees the job through.)
	// (... "Cannot retaliate objects dont", nor the immobile or drones.)
	if (TargetID == GenInvalidObjectID && !bMoving && !bAttackingPosition && HasWeapons() && !bNoRetaliation && !IsStealthedAndUndetected() && AbilityIndex == INDEX_NONE
		&& !Template->IsKindOf(TEXT("CANNOT_RETALIATE")) && !Template->IsKindOf(TEXT("IMMOBILE")) && !Template->IsKindOf(TEXT("DRONE")))
	{
		const FGenObject* Attacker = Logic->FindObject(Source);
		if (Attacker && Attacker->IsAttackable() && Logic->GetRelationship(Owner, Attacker->Owner) == EGenRelationship::Enemies
			&& ChooseWeapon(*Attacker) != INDEX_NONE)
		{
			TargetID = Source;
			bExplicitAttack = CanMove();
			NextRepathFrame = 0;
		}
	}
}

void FGenObject::AttemptHealing(float Amount)
{
	if (bDead || bPendingDestroy || Amount <= 0.f || Health >= MaxHealth)
	{
		return;
	}
	if (PoisonStopFrame != 0)
	{
		// PoisonedBehavior::onHealing: being healed cures the poison.
		PoisonDamageFrame = PoisonStopFrame = 0;
		PoisonAmount = 0.f;
	}
	Health = FMath::Min(MaxHealth, Health + Amount);
	if (MineState)
	{
		OnMineHealthChanged(true, ID, TEXT("HEALING")); // MinefieldBehavior::onHealing
	}
	// ActiveBody::setCorrectDamageState
	const FGenGameData& GD = FGenGameData::Get();
	const float Ratio = MaxHealth > 0.f ? Health / MaxHealth : 0.f;
	const bool bReally = !bUnderConstruction && Ratio <= GD.UnitReallyDamagedThreshold;
	SetModelCondition(GenConditions::Rubble(), false);
	SetModelCondition(GenConditions::ReallyDamaged(), bReally);
	SetModelCondition(GenConditions::Damaged(), !bReally && !bUnderConstruction && Ratio <= GD.UnitDamagedThreshold);
}

void FGenObject::StartSecondLife(const FString& DeathType)
{
	// UndeadBody::startSecondLife: a new body (SecondLifeMaxHealth, whole), the SECOND_LIFE armor, and one of its slow
	// deaths begun - the Battle Bus's, which isn't a death this time (BattleBusSlowDeathBehavior::beginSlowDeath).
	bSecondLife = true;
	MaxHealth = Template->SecondLifeMaxHealth;
	Health = MaxHealth;
	RefreshArmorSet();
	SetModelCondition(GenConditions::ReallyDamaged(), false);
	SetModelCondition(GenConditions::Damaged(), false);
	TArray<const FGenSlowDeath*> Candidates;
	int32 TotalWeight = 0;
	for (const FGenSlowDeath& D : Template->SlowDeaths)
	{
		if (DeathTypeApplies(D.DeathTypes, DeathType) && !StatusExemptApplies(*this, D.ExemptStatus) && StatusRequiredMet(*this, D.RequiredStatus))
		{
			Candidates.Add(&D);
			TotalWeight += FMath::Max(1, D.ProbabilityModifier);
		}
	}
	if (Candidates.Num() == 0)
	{
		return;
	}
	int32 Roll = Logic->GetRandom().RandRange(0, TotalWeight - 1);
	const FGenSlowDeath* Chosen = Candidates[0];
	for (const FGenSlowDeath* D : Candidates)
	{
		Roll -= FMath::Max(1, D->ProbabilityModifier);
		if (Roll < 0) { Chosen = D; break; }
	}
	if (!Chosen->bBattleBus)
	{
		return;
	}
	const uint32 Frame = Logic->GetFrame();
	BusDeath = Chosen;
	BusState = 1;
	BusGroundCheckFrame = Frame + 10; // GROUND_CHECK_DELAY: "to prevent hitting on the way up"
	if (!Chosen->FXStartUndeath.IsEmpty()) { Logic->DoFXObj(Chosen->FXStartUndeath, *this); }
	if (!Chosen->OCLStartUndeath.IsEmpty()) { Logic->DoOCL(Chosen->OCLStartUndeath, *this); }
	ClearAttack();
	CommandStop();
	// PhysicsBehavior: no more speed along the ground, a shock straight up (applyShock: the force over its mass, once),
	// and a tumble (applyRandomRotation).
	Velocity = FVector3f::ZeroVector;
	Accel = FVector3f::ZeroVector;
	BusVelZ = Chosen->ThrowForce / FMath::Max(Mass, 0.01f);
	FRandomStream& Rand = Logic->GetRandom();
	BodyPitchRate = Rand.FRandRange(-0.03f, 0.03f);
	BodyRollRate = Rand.FRandRange(-0.03f, 0.03f);
	// OpenContain::processDamageToContained: everyone inside takes that share of their health.
	if (Contain && Chosen->PercentDamageToPassengers > 0.f)
	{
		const TArray<FGenObjectID> Riders = Logic->GetRiders(*this);
		for (FGenObjectID RiderId : Riders)
		{
			if (FGenObject* R = Logic->FindObject(RiderId))
			{
				R->AttemptDamage(R->GetMaxHealth() * Chosen->PercentDamageToPassengers, TEXT("UNRESISTABLE"), TEXT("NORMAL"), ID);
			}
		}
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [bus f%u] #%u %s dies a first death: thrown up at %.2f a frame, %.0f health in its second life"),
		Frame, ID, *Template->Name, BusVelZ, MaxHealth);
}

void FGenObject::UpdateBattleBus(uint32 Frame)
{
	// BattleBusSlowDeathBehavior::update
	const FGenSlowDeath& D = *BusDeath;
	if (BusState == 1)
	{
		Position.Z += BusVelZ;
		BusVelZ += FGenGameData::Get().Gravity;
		const float Ground = GetSurfaceHeight();
		if (Position.Z <= Ground)
		{
			Position.Z = Ground;
			BusVelZ = 0.f;
		}
		BodyPitch += BodyPitchRate;
		BodyRoll += BodyRollRate;
		if (Frame > BusGroundCheckFrame && Position.Z <= Ground + 0.5f)
		{
			// "!me->isAboveTerrain()": down - a bunker on the spot from now on (DISABLED_HELD), SECOND_LIFE to look at.
			BusState = 2;
			BodyPitch = BodyRoll = BodyPitchRate = BodyRollRate = 0.f;
			if (!D.FXHitGround.IsEmpty()) { Logic->DoFXObj(D.FXHitGround, *this); }
			if (!D.OCLHitGround.IsEmpty()) { Logic->DoOCL(D.OCLHitGround, *this); }
			static const int32 SecondLifeBit = GenConditions::Index(TEXT("SECOND_LIFE"));
			SetModelCondition(SecondLifeBit, true);
			ClearAttack();
			CommandStop();
			Velocity = FVector3f::ZeroVector;
			bImmobile = true;
			BusNextHulkCheck = Frame;
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [bus f%u] #%u %s is down: a hulk with %d inside"), Frame, ID, *Template->Name, Logic->GetRiders(*this).Num());
		}
		return;
	}
	// The hulk: left empty for EmptyHulkDestructionDelay, it kills itself ("Penalty damage", DEATH_EXTRA_4).
	if (D.EmptyHulkDestructionDelay <= 0 || !Contain || Frame < BusNextHulkCheck)
	{
		return;
	}
	BusNextHulkCheck = Frame + 15; // EMPTY_HULK_CHECK_DELAY
	const int32 Count = Logic->GetRiders(*this).Num();
	if (BusPenaltyFrame != 0)
	{
		if (Count > 0)
		{
			BusPenaltyFrame = 0;
		}
		else if (Frame > BusPenaltyFrame)
		{
			Kill(TEXT("EXTRA_4"));
		}
	}
	else if (Count == 0)
	{
		BusPenaltyFrame = Frame + uint32(D.EmptyHulkDestructionDelay);
	}
}

void FGenObject::StartPoison(float Amount, const FString& DeathType)
{
	// PoisonedBehavior::startPoisonedEffects: every dose sets what the next ones do and starts the clock again.
	const FGenThingTemplate::FPoisoned& P = Template->Poisoned;
	const uint32 Now = Logic->GetFrame();
	PoisonAmount = Amount;
	PoisonStopFrame = Now + uint32(P.DurationFrames);
	PoisonDamageFrame = PoisonDamageFrame != 0 ? FMath::Min(PoisonDamageFrame, Now + uint32(P.IntervalFrames)) : Now + uint32(P.IntervalFrames);
	PoisonDeathType = DeathType;
}

void FGenObject::UpdatePoison(uint32 Frame)
{
	// PoisonedBehavior::update: "Not poison, as that will infect us again" - the dose is unresistable damage
	// (the poison's own effects are shown), and the poisoning ends once nothing has topped it up for a while.
	if (PoisonDamageFrame != 0 && Frame >= PoisonDamageFrame)
	{
		PoisonDamageFrame = Frame + uint32(Template->Poisoned.IntervalFrames);
		static const FString Unresistable(TEXT("UNRESISTABLE"));
		AttemptDamage(PoisonAmount, Unresistable, PoisonDeathType, GenInvalidObjectID, false, TEXT("POISON"));
	}
	if (PoisonStopFrame != 0 && Frame >= PoisonStopFrame && !bDead)
	{
		PoisonDamageFrame = PoisonStopFrame = 0;
		PoisonAmount = 0.f;
	}
}

void FGenObject::UpdateBaseRegenerate(uint32 Frame)
{
	// BaseRegenerateUpdate::update: a base building mends itself, a little every third frame, until whole.
	const FGenGameData& GD = FGenGameData::Get();
	if (GD.BaseRegenHealthPercentPerSecond <= 0.f || SellFrame != 0 || Health >= MaxHealth)
	{
		BaseRegenFrame = MAX_uint32; // "sleep till we get damaged again"
		return;
	}
	constexpr int32 HealRate = 3;
	AttemptHealing(float(HealRate) * MaxHealth * GD.BaseRegenHealthPercentPerSecond / float(GenLogic::GetLogicFPS()));
	BaseRegenFrame = Frame + HealRate;
}

void FGenObject::UpdateHorde(uint32 Frame)
{
	// HordeUpdate::update: enough of its kind close by (Count, itself included, within Radius between bounding
	// spheres) and it is a true member of a horde; fewer, but close to a true member, and it is in one too.
	const FGenThingTemplate::FHorde& H = Template->Horde;
	if (HordeNextFrame == 0)
	{
		// ("setWakeFrame(getObject(), UPDATE_SLEEP(GameLogicRandomValue(1, delay)))": not all at once.)
		HordeNextFrame = Frame + uint32(Logic->GetRandom().RandRange(1, H.UpdateRate));
		return;
	}
	HordeNextFrame = Frame + uint32(H.UpdateRate);
	int32 Count = 0;
	bool bRubOff = false;
	const float MyRadius = Template->GetBoundingCircleRadius();
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), H.Radius + MyRadius, [&](FGenObject& O)
	{
		// PartitionFilterHordeMember
		if (&O == this || O.bDead || O.bPendingDestroy || O.IsContained() || !O.Template->Horde.bValid
			|| (H.bExactMatch && O.Template != Template))
		{
			return;
		}
		for (const FString& K : H.KindOf)
		{
			if (!O.IsKindOf(*K))
			{
				return;
			}
		}
		if (H.bAlliesOnly && Logic->GetRelationship(Owner, O.Owner) != EGenRelationship::Allies)
		{
			return;
		}
		if (FVector3f::Distance(Position, O.Position) - MyRadius - O.Template->GetBoundingCircleRadius() > H.Radius)
		{
			return;
		}
		++Count;
		if (O.bTrueHordeMember && FVector2f::DistSquared(FVector2f(Position.X, Position.Y), FVector2f(O.Position.X, O.Position.Y)) <= FMath::Square(H.RubOffRadius))
		{
			bRubOff = true;
		}
	});
	bTrueHordeMember = Count >= H.Count - 1;
	const bool bWas = bInHorde;
	bInHorde = bTrueHordeMember || bRubOff;
	UE_CLOG(Logic->IsTracing() && bWas != bInHorde, LogGenerals, Log, TEXT("  [horde f%u] #%u p%d %s %s a horde (%d near)"), Frame, ID, Owner, *Template->Name,
		bInHorde ? TEXT("joins") : TEXT("leaves"), Count);
	EvaluateMoraleBonus();
}

void FGenObject::EvaluateMoraleBonus()
{
	// AIUpdateInterface::evaluateMoraleBonus: HORDE in a horde; NATIONALISM (and FANATICISM on top of it) with the
	// player's upgrade - unless it is in a horde of a kind that isn't "AllowedNationalism".
	static const FString HordeBonus(TEXT("HORDE")), NationalismBonus(TEXT("NATIONALISM")), FanaticismBonus(TEXT("FANATICISM"));
	bool bNationalism = Logic->HasUpgradeComplete(Owner, TEXT("Upgrade_Nationalism"));
	bool bFanaticism = Logic->HasUpgradeComplete(Owner, TEXT("Upgrade_Fanaticism"));
	if (bInHorde && !Template->Horde.bAllowedNationalism)
	{
		bNationalism = bFanaticism = false;
	}
	auto Set = [this](const FString& Bonus, bool bOn) { if (bOn) { BonusConditions.AddUnique(Bonus); } else { BonusConditions.Remove(Bonus); } };
	Set(HordeBonus, bInHorde);
	Set(NationalismBonus, bNationalism);
	Set(FanaticismBonus, bNationalism && bFanaticism);
}

void FGenObject::Die(const FString& DeathType, FGenObjectID Killer)
{
	if (bDead)
	{
		return;
	}
	const uint32 Frame = Logic->GetFrame();
	bDead = true;
	if (Template->AssaultTransport.bValid)
	{
		AssaultGiveFinalOrders(); // AssaultTransportAIUpdate::update: "isEffectivelyDead -> giveFinalOrders"
	}
	if (Template->RebuildHole.bValid)
	{
		// RebuildHoleBehavior::onDie: the worker goes with the hole.
		if (FGenObject* Worker = Logic->FindObject(HoleWorker))
		{
			Worker->bPendingDestroy = true;
		}
		HoleWorker = GenInvalidObjectID;
	}
	if (Template->BattlePlan.bValid)
	{
		StopBattlePlan(); // BattlePlanUpdate::onDelete (the strategy center is destroyed as it dies)
	}
	if (Template->Spawner.bValid)
	{
		EndSpawns(false); // SpawnBehavior::onDie
	}
	// Object::onDie: the adviser reports the loss, unless it was the player's own doing.
	{
		const FGenObject* KilledBy = Logic->FindObject(Killer);
		if (Owner == Logic->GetLocalPlayer() && (!KilledBy || KilledBy->Owner != Owner) && Killer != GenInvalidObjectID)
		{
			if (IsKindOf(TEXT("STRUCTURE")) && IsKindOf(TEXT("MP_COUNT_FOR_VICTORY")))
			{
				Logic->EvaEvent(TEXT("BuildingLost"), Owner);
			}
			else if (IsKindOf(TEXT("INFANTRY")) || IsKindOf(TEXT("VEHICLE")))
			{
				Logic->EvaEvent(TEXT("UnitLost"), Owner);
				Logic->TryRadarEvent(EGenRadarEvent::Fake, Position); // "so the user can use the spacebar to quickly jump to this"
			}
		}
	}
	if (Parking)
	{
		Parking->KillAllParkedUnits(); // ParkingPlaceBehavior::onDie: aircraft on the ground go with their airfield
	}
	if (JetState != EGenJetState::None)
	{
		SetJetState(EGenJetState::None, Frame); // gives back the runway
	}
	AIMode = EGenAIMode::None;
	if (BuildTarget != GenInvalidObjectID)
	{
		// DozerAIUpdate::onDelete: "If we were building something, kill the active-construction flag on it"
		if (FGenObject* Site = Logic->FindObject(BuildTarget))
		{
			static const int32 ActivelyBeingConstructed = GenConditions::Index(TEXT("ACTIVELY_BEING_CONSTRUCTED"));
			Site->SetModelCondition(ActivelyBeingConstructed, false);
		}
	}
	ExitAbility(true);
	// SpecialAbilityUpdate::onExit( TRUE ) for every one of them: what they left in the world goes with
	// the unit, unless it is of the kind that stays (a timed charge keeps ticking; remote ones vanish).
	for (int32 i = 0; i < AbilityObjects.Num(); ++i)
	{
		const FGenAbilityData& AD = Template->Abilities[i];
		if (!AD.bSpecialObjectsPersistent || !AD.bSpecialObjectsPersistWhenOwnerDies)
		{
			KillAbilityObjects(i);
		}
	}
	if (HackState != EHackState::None)
	{
		EndHackInternet();
	}
	CheckAndDetonateBoobyTrap(nullptr); // "Already dying, so no need to handle death case of explosion"
	HuntButton.Reset();
	ClearAttack();
	StopDocking(); // whoever was queueing behind us at a dock moves up
	CommandStop();
	if (bFlies && IsAirborneTarget())
	{
		// Shot out of the sky: the wreck falls (the helicopter and jet crash behaviours aren't ported yet).
		bFreePhysics = true;
		YawRate = Logic->GetRandom().FRandRange(-0.12f, 0.12f);
	}
	if (!bFreePhysics)
	{
		Velocity = FVector3f::ZeroVector; // thrown wrecks keep flying while they die
	}
	bStatusBraking = false;
	for (int32 s = 0; s < 3; ++s)
	{
		SetFiringConditions(s, FS_None);
	}
	SetModelCondition(GenConditions::Moving(), false);
	SetModelCondition(DyingIndex(), true);
	Logic->OnObjectDied(*this);
	Logic->OnObjectKilled(*this, Killer);
	// Object::onDie: "objects that were spawned from something, need to tell their spawner that they have died"
	if (FGenObject* Spawner = Logic->FindObject(Producer))
	{
		if (Spawner->Template->Spawner.bValid)
		{
			Spawner->OnSpawnDeath(ID, Killer);
		}
	}
	if (Template->bTechBuilding)
	{
		// TechBuildingBehavior::onDie: "put us on the team of the neutral player so no player has any bonus from us"
		SetModelCondition(GenConditions::Index(TEXT("CAPTURED")), false);
		SetOwner(0);
	}
	Logic->RefreshCostModifiers();
	Logic->OnContainerDied(*this);
	RemovePropagandaInfluence(); // PropagandaTowerBehavior::onDie
	if (Template->DemoTrap.bValid && Template->DemoTrap.bDetonateWhenKilled && !bDemoTrapDetonated)
	{
		// DemoTrapUpdate::update: "if( me->isEffectivelyDead() ) { if( data->m_detonateWhenKilled ) detonate(); }"
		bDemoTrapDetonated = true;
		if (const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(Template->DemoTrap.DetonationWeapon))
		{
			Logic->FireTempWeapon(*W, *this, Position);
		}
	}
	if (!Template->UpgradeDieRemove.IsEmpty())
	{
		// UpgradeDie::onDie: "used in cases where the producer builds an upgrade that can die... like
		// ranger building scout drones".
		if (FGenObject* Maker = Logic->FindObject(Producer))
		{
			Maker->RemoveObjectUpgrade(Template->UpgradeDieRemove);
		}
	}
	StopEntering();
	for (const FProduction& P : Production)
	{
		if (P.Upgrade) { Logic->CancelUpgradeInProduction(Owner, *P.Upgrade); }
	}
	// FireWeaponWhenDeadBehavior: a bomb's bang is the weapon it "fires" where it dies.
	for (const FGenThingTemplate::FDeathWeapon& DW : Template->DeathWeapons)
	{
		// FireWeaponWhenDeadBehavior::onDie: "if (!isUpgradeActive()) return" - it starts active or its
		// upgrades are in - and no upgrade it conflicts with is.
		bool bActive = DW.bStartsActive;
		if (!bActive && DW.TriggeredBy.Num() > 0)
		{
			int32 NumIn = 0;
			for (const FString& U : DW.TriggeredBy) { NumIn += HasUpgrade(U) ? 1 : 0; }
			bActive = DW.bRequiresAllTriggers ? NumIn == DW.TriggeredBy.Num() : NumIn > 0;
		}
		for (const FString& U : DW.ConflictsWith)
		{
			bActive = bActive && !HasUpgrade(U);
		}
		// "if (!d->m_dieMuxData.isDieApplicable(getObject(), damageInfo)) return;" - a combat cycle's bomb goes off
		// only with the terrorist aboard, and only at the deaths it names.
		if (!bActive || !DeathTypeApplies(DW.DeathTypes, DeathType) || StatusExemptApplies(*this, DW.ExemptStatus) || !StatusRequiredMet(*this, DW.RequiredStatus))
		{
			continue;
		}
		if (const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(DW.Weapon))
		{
			Logic->DoFXPos(W->FireFX, Position, nullptr, 0.f, nullptr, W->PrimaryDamageRadius);
			if (!W->FireOCL.IsEmpty()) { Logic->DoOCL(W->FireOCL, *this); }
			Logic->DealDamage(*W, ID, GenInvalidObjectID, Position, true);
		}
	}

	if (Template->LeafletDrop.bValid)
	{
		DoLeafletDisable(Frame); // LeafletDropBehavior::onDie
	}
	if (Template->GenerateMinefield.bValid && Template->GenerateMinefield.bOnDeath)
	{
		PlaceMines(); // GenerateMinefieldBehavior::onDie
	}
	if (Template->bCrushDie && (DeathType.Equals(TEXT("CRUSHED"), ESearchCase::IgnoreCase) || DeathType.Equals(TEXT("SPLATTED"), ESearchCase::IgnoreCase)))
	{
		// CrushDie with crushLocationCheck: "PhysicsCollide has already done the logic of which point to
		// smoosh ... so at this point we just need to know which crush point is closest." 0 the whole of it,
		// 1 the back end, 2 the front end.
		int32 Crush = 0;
		if (const FGenObject* Crusher = Logic->FindObject(CrushedBy))
		{
			const FVector2f Dir = GetDirection2D();
			const float Offset = Template->GeometryMajorRadius * 0.5f;
			const FVector2f To(Position.X - Crusher->Position.X, Position.Y - Crusher->Position.Y);
			float Best = To.SizeSquared();
			const float Front = (To + Dir * Offset).SizeSquared(), Back = (To - Dir * Offset).SizeSquared();
			if (Front < Best) { Best = Front; Crush = 2; }
			if (Back < Best) { Crush = 1; }
		}
		static const int32 FrontCrushed = GenConditions::Index(TEXT("FRONTCRUSHED")), BackCrushed = GenConditions::Index(TEXT("BACKCRUSHED"));
		SetModelCondition(FrontCrushed, Crush == 0 || Crush == 2);
		SetModelCondition(BackCrushed, Crush == 0 || Crush == 1);
		if (!Template->CrushSounds[Crush].IsEmpty() && Logic->GetRandom().RandRange(0, 99) < Template->CrushSoundPercent[Crush])
		{
			if (UGenAudioManager* Audio = Logic->GetAudio())
			{
				Audio->PlayForObject(Template->CrushSounds[Crush], *this);
			}
		}
	}

	// Die modules: every one whose DeathTypes apply runs (FXListDie, CreateObjectDie, DestroyDie, InstantDeathBehavior).
	FRandomStream& Rand = Logic->GetRandom();
	bool bDestroyNow = false;
	for (const FGenDieModule& M : Template->DieModules)
	{
		// DieMuxData::isDieApplicable: "wrong death type? punt", "wrong vet level? punt", "wrong status? punt"
		if (!DeathTypeApplies(M.DeathTypes, DeathType) || (M.VeterancyMask & (1 << VeterancyLevel)) == 0 || StatusExemptApplies(*this, M.ExemptStatus)
			|| !StatusRequiredMet(*this, M.RequiredStatus))
		{
			continue;
		}
		if (M.Type == EGenDieModule::RebuildHoleExpose)
		{
			Logic->ExposeRebuildHole(*this, M);
			continue;
		}
		if (M.Type == EGenDieModule::EjectPilot)
		{
			EjectPilot(M);
			continue;
		}
		if (M.Type == EGenDieModule::CreateCrate)
		{
			CreateCrates(M, Killer);
			continue;
		}
		if (M.FX.Num() > 0) { Logic->DoFXObj(M.FX[Rand.RandRange(0, M.FX.Num() - 1)], *this); }
		if (M.OCL.Num() > 0)
		{
			TArray<FGenObjectID> Made;
			Logic->DoOCL(M.OCL[Rand.RandRange(0, M.OCL.Num() - 1)], *this, nullptr, M.bTransferPreviousHealth ? &Made : nullptr);
			// CreateObjectDie::onDie, TransferPreviousHealth: "we're transferring the last known health before we
			// died. In the case of the sneak attack tunnel network, it is killed after the lifetime update
			// expires." And whoever was attacking this goes on with what has taken its place.
			if (FGenObject* New = Made.Num() > 0 ? Logic->FindObject(Made[0]) : nullptr)
			{
				const float Lost = MaxHealth - PreviousHealth;
				if (Lost > 0.f)
				{
					New->AttemptDamage(Lost, TEXT("UNRESISTABLE"), TEXT("NORMAL"), Killer);
				}
				for (FGenObjectID OtherId : Logic->GetObjectIDs())
				{
					FGenObject* Other = Logic->FindObject(OtherId);
					if (Other && Other->TargetID == ID && !New->IsDead())
					{
						Other->TargetID = New->GetID(); // AIUpdateInterface::transferAttack
					}
				}
			}
		}
		bDestroyNow |= M.Type == EGenDieModule::Destroy || M.Type == EGenDieModule::Instant;
	}
	if (bDestroyNow)
	{
		DestroyFrame = Frame + 1;
		return;
	}
	if (Template->JetDeath.bValid && DeathTypeApplies(Template->JetDeath.DeathTypes, DeathType) && BeginJetDeath(DeathType, Frame))
	{
		return; // JetSlowDeathBehavior: blown up on the ground, or rolling out of the sky
	}
	if (Template->HeliDeath.bValid && DeathTypeApplies(Template->HeliDeath.DeathTypes, DeathType) && BeginHeliDeath(Frame))
	{
		return; // HelicopterSlowDeathBehavior: spiralling down
	}
	if (Template->Collapse.bValid && DeathTypeApplies(Template->Collapse.DeathTypes, DeathType))
	{
		BeginStructureCollapse(Frame); // StructureCollapseUpdate: it sinks into the ground, and its rubble stays
		return;
	}
	if (Template->StructureTopple.bValid && DeathTypeApplies(Template->StructureTopple.DeathTypes, DeathType))
	{
		BeginStructureTopple(Frame, Killer); // StructureToppleUpdate: it falls over
		return;
	}

	// Pick one applicable SlowDeathBehavior, weighted by ProbabilityModifier.
	TArray<const FGenSlowDeath*> Candidates;
	int32 TotalWeight = 0;
	for (const FGenSlowDeath& D : Template->SlowDeaths)
	{
		if (DeathTypeApplies(D.DeathTypes, DeathType) && !StatusExemptApplies(*this, D.ExemptStatus) && StatusRequiredMet(*this, D.RequiredStatus))
		{
			Candidates.Add(&D);
			TotalWeight += FMath::Max(1, D.ProbabilityModifier);
		}
	}
	if (Candidates.Num() > 0)
	{
		int32 Roll = Logic->GetRandom().RandRange(0, TotalWeight - 1);
		const FGenSlowDeath* Chosen = Candidates[0];
		for (const FGenSlowDeath* D : Candidates)
		{
			Roll -= FMath::Max(1, D->ProbabilityModifier);
			if (Roll < 0) { Chosen = D; break; }
		}
		DestroyFrame = Frame + uint32(Chosen->DestructionDelay + Logic->GetRandom().RandRange(0, FMath::Max(0, Chosen->DestructionDelayVariance)));
		SinkStartFrame = Frame + uint32(Chosen->SinkDelay);
		SinkRate = Chosen->SinkRate;
		// SlowDeathBehavior::beginSlowDeath: INITIAL effects now, MIDPOINT somewhere in the middle third, FINAL at destruction.
		SlowDeath = Chosen;
		MidpointFrame = Frame + uint32(Rand.FRandRange(0.35f, 0.65f) * float(DestroyFrame - Frame));
		DoDeathPhase(FGenSlowDeath::Initial);
	}
	else if (Template->bKeepObjectDie)
	{
		// KeepObjectDie: the rubble stays where the building stood.
		SetModelCondition(GenConditions::Rubble(), true);
		DestroyFrame = MAX_uint32;
	}
	else if (Template->IsKindOf(TEXT("STRUCTURE")))
	{
		// "structures with "RUBBLE" states should not use DestroyDie; such buildings are never truly
		// destroyed, even when reduced to zero health": with a die module of its own (an effect, say) and
		// none that removes it, the ruin stays. Without any, the default DestroyDie is still its.
		SetModelCondition(GenConditions::Rubble(), true);
		DestroyFrame = Template->bHasOwnDieModule ? MAX_uint32 : Frame + 1;
	}
	else
	{
		DestroyFrame = Frame + 1; // DestroyDie
	}
}

void FGenObject::Kill(const FString& DeathType, FGenObjectID Killer)
{
	// (Object::kill is unresistable damage: an indestructible body takes none of it.)
	if (bDead || bPendingDestroy || bIndestructible)
	{
		return;
	}
	PreviousHealth = Health;
	Health = 0.f;
	SetModelCondition(GenConditions::ReallyDamaged(), false);
	SetModelCondition(GenConditions::Damaged(), false);
	SetModelCondition(GenConditions::Rubble(), true);
	Die(DeathType, Killer);
}

void FGenObject::DoDeathPhase(int32 Phase)
{
	// SlowDeathBehavior::doPhaseStuff: one random FX and one random OCL of the phase.
	if (!SlowDeath)
	{
		return;
	}
	FRandomStream& Rand = Logic->GetRandom();
	const TArray<FString>& FX = SlowDeath->FX[Phase];
	const TArray<FString>& OCL = SlowDeath->OCL[Phase];
	if (FX.Num() > 0) { Logic->DoFXObj(FX[Rand.RandRange(0, FX.Num() - 1)], *this); }
	if (OCL.Num() > 0) { Logic->DoOCL(OCL[Rand.RandRange(0, OCL.Num() - 1)], *this); }
	const TArray<FString>& PhaseWeapons = SlowDeath->Weapons[Phase];
	if (PhaseWeapons.Num() > 0)
	{
		if (const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(PhaseWeapons[Rand.RandRange(0, PhaseWeapons.Num() - 1)]))
		{
			Logic->DoFXPos(W->FireFX, Position, nullptr, 0.f, nullptr, W->PrimaryDamageRadius);
			Logic->DealDamage(*W, ID, GenInvalidObjectID, Position, true);
		}
	}
}

void FGenObject::UpdateDying(uint32 Frame)
{
	if (Template->NeutronBlast.bValid)
	{
		UpdateNeutronBlast(Frame);
	}
	if (bJetDying)
	{
		UpdateJetDying(Frame);
		return;
	}
	if (bHeliDying)
	{
		UpdateHeliDying(Frame);
		return;
	}
	if (CollapseState != ECollapseState::Standing)
	{
		if (bStructureToppling) { UpdateStructureTopple(Frame); } else { UpdateStructureCollapse(Frame); }
		return;
	}
	if (SlowDeath && !bMidpointDone && Frame >= MidpointFrame)
	{
		bMidpointDone = true;
		DoDeathPhase(FGenSlowDeath::Midpoint);
	}
	if (SinkRate > 0.f && Frame >= SinkStartFrame)
	{
		// Physics is held while the wreck sinks into the ground.
		SinkOffset += SinkRate;
		Position.Z -= SinkRate;
	}
	else if (bFreePhysics)
	{
		UpdateFreePhysics(Frame);
	}
	if (Frame >= DestroyFrame)
	{
		DoDeathPhase(FGenSlowDeath::Final);
		bPendingDestroy = true;
	}
}

// ---- projectiles --------------------------------------------------------------------------------

void FGenObject::LaunchProjectile(const FGenWeaponTemplate* Weapon, FGenObjectID Launcher, FGenObjectID Victim, const FVector3f& TargetPos, const FQuat4f* LaunchRotation)
{
	Projectile.bActive = true;
	Projectile.Weapon = Weapon;
	Projectile.Launcher = Launcher;
	Projectile.Victim = Victim;
	Projectile.Start = Position;
	Projectile.End = TargetPos;
	Projectile.Step = 0;
	// Missiles home in; everything else follows DumbProjectileBehavior's Bezier arc.
	Projectile.bMissile = Template->bIsMissile || !Template->DumbProjectile.bValid;
	if (Template->NeutronMissile.bValid)
	{
		LaunchNeutronMissile(Logic->GetFrame());
		return;
	}
	if (Template->bIsMissile && Locomotor && Locomotor->Appearance == EGenLocoAppearance::Thrust && CVarGenMissileFlight.GetValueOnAnyThread() != 0)
	{
		// A missile proper: it flies itself (GenMissile.cpp). Its exhaust comes with the ignition.
		LaunchMissile(Logic->GetFrame(), LaunchRotation);
		return;
	}
	// Point at the target from the start (the drawable and the exhaust trail follow Rotation).
	bFreeRotation = true;
	const FVector3f Aim = (TargetPos - Position).GetSafeNormal();
	if (!Aim.IsZero())
	{
		SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, Aim));
	}
	if (!Weapon->ProjectileExhaust.IsEmpty())
	{
		Logic->AttachParticleSystem(Weapon->ProjectileExhaust, ID);
	}

	const float Speed = FMath::Max(Weapon->WeaponSpeed, 0.01f);
	if (!Projectile.bMissile)
	{
		// calcFlightPath: number of steps = curve length / speed.
		const FGenDumbProjectile& D = Template->DumbProjectile;
		const float Top = FMath::Max(Projectile.Start.Z, Projectile.End.Z);
		FVector3f P[4] = { Projectile.Start, FMath::Lerp(Projectile.Start, Projectile.End, D.FirstPercentIndent),
			FMath::Lerp(Projectile.Start, Projectile.End, D.SecondPercentIndent), Projectile.End };
		P[1].Z = Top + D.FirstHeight;
		P[2].Z = Top + D.SecondHeight;
		float Length = 0.f;
		FVector3f Prev = P[0];
		for (int32 i = 1; i <= 16; ++i)
		{
			const FVector3f Cur = Bezier(P, i / 16.f);
			Length += (Cur - Prev).Size();
			Prev = Cur;
		}
		Projectile.Segments = FMath::Max(1, FMath::CeilToInt32(Length / Speed));
	}
}

void FGenObject::UpdateProjectile(uint32 Frame)
{
	if (Projectile.bNeutron)
	{
		UpdateNeutronMissile(Frame);
		return;
	}
	const FGenWeaponTemplate& W = *Projectile.Weapon;
	const FGenObject* Victim = Projectile.Victim != GenInvalidObjectID ? Logic->FindObject(Projectile.Victim) : nullptr;
	if (Projectile.DecoyFrame != 0 && Projectile.DecoyFrame <= Frame)
	{
		// MissileAIUpdate::update: "If this missile has been marked to divert to countermeasures, check when
		// that will occur, then do it when the timer expires." From here on it is harmless, flare or no flare.
		Projectile.DecoyFrame = 0;
		Projectile.bNoDamage = true;
		if (Victim)
		{
			const FGenObjectID Flare = Victim->CalculateCountermeasureToDivertTo();
			if (const FGenObject* F = Logic->FindObject(Flare))
			{
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [flare f%u] missile #%u leaves #%u %s for flare #%u"), Frame, ID, Victim->ID,
					*Victim->Template->Name, Flare);
				Projectile.Victim = Flare;
				Victim = F;
			}
		}
	}
	if (Victim && Projectile.bNoDamage)
	{
		// (A flare is no target for anything else, but a fooled missile follows it while it burns.)
		if (Victim->bDead || Victim->bPendingDestroy) { Victim = nullptr; }
	}
	else if (Victim && (!Victim->IsTargetable() || Victim->GetSneakyTargetingOffset(nullptr)))
	{
		// (WeaponTemplate::shouldProjectileCollideWith: "if something has a Sneaky Target offset, it is
		// momentarily immune to being hit": the shot flies on to where it last saw its victim.)
		if (Victim->IsTargetable()) { Projectile.Victim = GenInvalidObjectID; }
		Victim = nullptr;
	}
	if (Victim)
	{
		// Keep tracking a moving victim; projectiles aim at the center of its geometry, not its feet.
		Projectile.End = Victim->Position + FVector3f(0.f, 0.f, Victim->Template->GeometryHeight * 0.5f);
	}
	if (Projectile.bThrust)
	{
		UpdateMissile(Frame, Victim);
		return;
	}

	bool bDetonate = false;
	FVector3f NewPos = Position;
	if (Projectile.bMissile)
	{
		const FVector3f Aim = Projectile.End;
		const FVector3f Delta = Aim - Position;
		const float Dist = Delta.Size();
		float Speed = W.WeaponSpeed;
		if (Template->bIsMissile && !Template->bMissileUseWeaponSpeed && Locomotor)
		{
			// MissileAIUpdate: a missile flies on its own locomotor (the weapon's speed only counts with
			// UseWeaponSpeed); until its engine lights it coasts at the speed it was launched with.
			Speed = Projectile.Step++ < Template->MissileIgnitionDelay ? Template->MissileInitialVelocity : Locomotor->Speed;
		}
		Speed = FMath::Max(Speed, 0.01f);
		if (Dist <= Speed)
		{
			NewPos = Aim;
			bDetonate = true;
		}
		else
		{
			NewPos = Position + Delta * (Speed / Dist);
		}
	}
	else
	{
		const FGenDumbProjectile& D = Template->DumbProjectile;
		++Projectile.Step;
		const float Top = FMath::Max(Projectile.Start.Z, Projectile.End.Z);
		FVector3f P[4] = { Projectile.Start, FMath::Lerp(Projectile.Start, Projectile.End, D.FirstPercentIndent),
			FMath::Lerp(Projectile.Start, Projectile.End, D.SecondPercentIndent), Projectile.End };
		P[1].Z = Top + D.FirstHeight;
		P[2].Z = Top + D.SecondHeight;
		NewPos = Bezier(P, FMath::Min(1.f, float(Projectile.Step) / float(Projectile.Segments)));
		bDetonate = Projectile.Step >= Projectile.Segments;
	}

	bool bHitVictim = false;
	if (Victim)
	{
		// projectileHandleCollision: the shell goes off where it enters the victim's geometry, so the
		// explosion sits on the hull instead of inside it.
		for (int32 i = 1; i <= 4; ++i)
		{
			const FVector3f Probe = FMath::Lerp(Position, NewPos, float(i) / 4.f);
			if (Victim->ContainsPoint(Probe))
			{
				NewPos = Probe;
				bDetonate = true;
				bHitVictim = true;
				break;
			}
		}
		bHitVictim |= bDetonate; // (at the end of its flight it is in the victim's middle)
	}
	if (Template->DumbProjectile.GarrisonHitKillCount > 0 && !Projectile.bNoDamage)
	{
		// (Without a victim it is the building it was aimed at, when the shot comes down on that.)
		const FGenObject* Building = Victim ? Victim : Logic->FindObject(Projectile.Aimed);
		if (Building && !Victim)
		{
			for (int32 i = 1; i <= 4 && !bHitVictim; ++i)
			{
				bHitVictim = Building->ContainsPoint(FMath::Lerp(Position, NewPos, float(i) / 4.f));
			}
		}
		if (bHitVictim && Building && ProjectileClearsGarrison(Building->ID))
		{
			Position = NewPos;
			return;
		}
	}

	const FVector3f Move = NewPos - Position;
	if (Move.SizeSquared() > 1e-6f)
	{
		SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, Move.GetSafeNormal()));
	}
	Position = NewPos;
	// ("blow'd up!" - on a bridge's deck it has come down through.)
	if (!bDetonate && ProjectileHitsBridgeDeck(true))
	{
		bDetonate = true;
	}

	if (bDetonate)
	{
		// WeaponStore::handleProjectileDetonation: effects and damage are positional (radius around the impact).
		const FTransform3f Xf = GetTransform();
		Logic->DoFXPos(W.ProjectileDetonationFX, Position, &Xf, W.WeaponSpeed, &Position, W.PrimaryDamageRadius);
		if (!W.ProjectileDetonationOCL.IsEmpty())
		{
			Logic->DoOCL(W.ProjectileDetonationOCL, *this);
		}
		// (With the launcher gone the shot itself answers for the damage, so its player still gets the credit.)
		if (!Projectile.bNoDamage)
		{
			Logic->DealDamage(W, Logic->FindObject(Projectile.Launcher) ? Projectile.Launcher : ID, Projectile.Victim, Position, true);
		}
		bPendingDestroy = true;
	}
}
