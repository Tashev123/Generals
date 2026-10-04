// Combat half of FGenObject: weapons, targeting, aiming, firing, projectiles, damage and death.
// Follows Weapon.cpp / WeaponSet.cpp / AIStates.cpp / ActiveBody.cpp / DumbProjectileBehavior.cpp.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPathfinder.h"

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
		for (int32 s = 0; s < 3; ++s)
		{
			Weapons[s].Slot = s;
			Weapons[s].Template = GD.FindWeapon(WS->Weapons[s]);
			Weapons[s].AmmoInClip = Weapons[s].Template ? Weapons[s].Template->ClipSize : 0;
		}
	}
	if (const FGenArmorSetEntry* AS = Template->FindArmorSet())
	{
		Armor = GD.FindArmor(AS->Armor);
	}
	for (int32 t = 0; t < 2; ++t)
	{
		TurretAngle[t] = PrevTurretAngle[t] = Template->Turrets[t].NaturalAngle;
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
	if (bDead || bPendingDestroy || Projectile.bActive || ContainedBy != GenInvalidObjectID)
	{
		return false; // (what is inside a container can only be got at through the container)
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

bool FGenObject::StartApproach(const FGenObject& Target, float Range)
{
	// Pathfinder::adjustTargetDestination: aim for a point just inside weapon range on our side of the
	// target, then take the nearest free spot from which the weapon still reaches. Every attacker
	// reserves its own, so a group closes in side by side.
	const FVector2f TargetPos(Target.Position.X, Target.Position.Y);
	const FVector2f ToSelf = (FVector2f(Position.X, Position.Y) - TargetPos).GetSafeNormal();
	const float Stand = Range * 0.9f + Template->GetBoundingCircleRadius() + Target.Template->GetBoundingCircleRadius();
	const FVector2f Dest = TargetPos + ToSelf * Stand;
	const FGenObject* TargetPtr = &Target;
	const TFunction<bool(const FVector2f&)> InRange = [this, TargetPtr, Range](const FVector2f& P) { return IsWithinRangeFrom(P, *TargetPtr, Range); };
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	return StartPath(FVector3f(Dest.X, Dest.Y, Position.Z), false, &InRange);
}

int32 FGenObject::ChooseWeapon(const FGenObject& Target) const
{
	// WeaponSet::chooseBestWeaponForTarget (PREFER_MOST_DAMAGE): highest armor-adjusted damage.
	int32 Best = INDEX_NONE;
	float BestDamage = 0.f;
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenWeaponTemplate* W = Weapons[s].Template;
		if (!W || !CanWeaponTarget(*W, Target) || IsWeaponOutOfAmmo(s))
		{
			continue; // (a plane's missiles are gone until it has been home)
		}
		const float Damage = Target.Armor ? Target.Armor->AdjustDamage(W->DamageType, W->PrimaryDamage) : W->PrimaryDamage;
		if (Damage > BestDamage)
		{
			BestDamage = Damage;
			Best = s;
		}
	}
	return Best;
}

FGenObject* FGenObject::FindAutoTarget() const
{
	FGenObject* Best = nullptr;
	float BestDist = FLT_MAX;
	for (FGenObjectID OtherId : Logic->GetObjectIDs())
	{
		FGenObject* O = Logic->FindObject(OtherId);
		if (!O || O == this || !O->IsTargetable() || Logic->GetRelationship(Owner, O->Owner) != EGenRelationship::Enemies
			|| O->Template->IsKindOf(TEXT("HULK")))
		{
			continue;
		}
		const int32 Slot = ChooseWeapon(*O);
		if (Slot == INDEX_NONE)
		{
			continue;
		}
		const float Dist = AttackDistance(*O);
		if (Dist <= GetAttackRange(*Weapons[Slot].Template) && Dist < BestDist)
		{
			BestDist = Dist;
			Best = O;
		}
	}
	return Best;
}

// ---- orders -------------------------------------------------------------------------------------

void FGenObject::CommandAttack(FGenObjectID Target)
{
	if (bDead || !HasWeapons() || Target == ID)
	{
		return;
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
	NextRepathFrame = 0;
}

void FGenObject::ClearAttack()
{
	TargetID = GenInvalidObjectID;
	bExplicitAttack = false;
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
	const FGenWeaponTemplate& W = *Weapons[Slot].Template;
	const float Desired = FMath::Atan2(Target.Position.Y - Position.Y, Target.Position.X - Position.X);
	const float Tolerance = W.AcceptableAimDelta + AimThreshold;
	if (ContainedBy != GenInvalidObjectID)
	{
		// Shooting out of a window or a firing port: no turning on the spot in there.
		SetOrientation(Desired);
		return true;
	}

	const int32 T = TurretForSlot(Slot);
	// (A turret that cannot turn is only there to allow firing on the move, as on the Comanche: the body aims.)
	if (T != INDEX_NONE && Template->Turrets[T].TurnRate > 0.f)
	{
		// TurretAI: rotate the turret (relative to the body) towards the target.
		const float DesiredRel = GenLogic::AngleDiff(Desired, Orientation);
		const float Diff = GenLogic::AngleDiff(DesiredRel, TurretAngle[T]);
		const float Rate = Template->Turrets[T].TurnRate;
		TurretAngle[T] = GenLogic::NormalizeAngle(TurretAngle[T] + FMath::Clamp(Diff, -Rate, Rate));
		TurretIdleSince[T] = Logic->GetFrame();
		return FMath::Abs(GenLogic::AngleDiff(DesiredRel, TurretAngle[T])) <= Tolerance;
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

	// Fire effects play at the weapon's FX bone (muzzle) when the model has one; tracers fly to the victim's center.
	const FVector3f VictimCenter = Target.Position + FVector3f(0.f, 0.f, Target.Template->GeometryHeight * 0.5f);
	Logic->DoWeaponFireFX(*this, Slot, W, Scatter > 0.f ? Destination : VictimCenter);
	if (!W.FireOCL.IsEmpty())
	{
		Logic->DoOCL(W.FireOCL, *this);
	}

	if (!W.IsProjectile())
	{
		// Instant-hit weapons always damage the victim; the damage arrives after the travel time.
		const float Delay = (Target.Position - Position).Size() / FMath::Max(W.WeaponSpeed, 0.001f);
		const FGenObjectID DamageVictim = W.bDamageDealtAtSelfPosition ? GenInvalidObjectID : Target.ID;
		const FVector3f DamagePos = W.bDamageDealtAtSelfPosition ? Position : Target.Position;
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
		if (Logic->GetWeaponBoneTransform(*this, Slot, true, LaunchBone))
		{
			LaunchPos = LaunchBone.GetTranslation();
		}
		// A scattered shot flies to a position instead of tracking the victim.
		Logic->SpawnProjectile(W, *this, Scatter > 0.f ? GenInvalidObjectID : Target.ID, Destination, LaunchPos, FireAngle);
	}

	++WI.FireCount;
	SetFiringConditions(Slot, FS_Firing);
	FiringConditionEnd[Slot] = Frame + 1;

	// Rate of fire and clips (Weapon::privateFireWeapon / reloadWithBonus).
	// (The RATE_OF_FIRE bonuses -- veterancy -- shorten both the time between shots and the reload.)
	const float RateOfFire = GetWeaponBonus(W, FGenWeaponBonus::RateOfFire);
	const int32 ShotDelay = FMath::RoundToInt32(float(Rand.RandRange(W.DelayBetweenShotsMin, FMath::Max(W.DelayBetweenShotsMin, W.DelayBetweenShotsMax))) / RateOfFire);
	WI.bReloading = false;
	if (W.ClipSize > 0)
	{
		if (--WI.AmmoInClip <= 0)
		{
			if (W.ReloadType != EGenReloadType::Auto)
			{
				// OUT_OF_AMMO: the clip stays empty (a plane reloads at its airfield).
				WI.AmmoInClip = 0;
				WI.ReadyFrame = Frame + FMath::Max(1, ShotDelay);
				return;
			}
			WI.AmmoInClip = W.ClipSize;
			WI.ReadyFrame = Frame + FMath::Max(1, FMath::RoundToInt32(float(W.ClipReloadTime) / RateOfFire));
			WI.bReloading = true;
			return;
		}
	}
	WI.ReadyFrame = Frame + FMath::Max(1, ShotDelay);
}

void FGenObject::UpdateCombat(uint32 Frame)
{
	PrevTurretAngle[0] = TurretAngle[0];
	PrevTurretAngle[1] = TurretAngle[1];

	if (Delivery)
	{
		return; // an aircraft on a delivery run has its own orders
	}
	// An aircraft on the ground, or busy taking off, landing or flying home, does not fight.
	const bool bJetBusy = bJetAI && (JetState != EGenJetState::None || !bJetAllowAirLoco);
	// Planes (unlike helicopters) attack only what they are sent against.
	const bool bNoAutoAcquire = bJetBusy || (bJetAI && !Template->bAutoAcquireEnemiesWhenIdle);

	FGenObject* Target = (TargetID != GenInvalidObjectID && !bJetBusy) ? Logic->FindObject(TargetID) : nullptr;
	if (TargetID != GenInvalidObjectID && (!Target || !Target->IsTargetable()))
	{
		ClearAttack();
		Target = nullptr;
		if (bJetAI && Template->bJetNeedsRunway && !bJetBusy)
		{
			JetReturnToBaseFrame = Frame; // JetAIUpdate::notifyVictimIsDead: job done, home we go
		}
	}

	// Idle units look for enemies in weapon range (AutoAcquireEnemiesWhenIdle).
	if (!Target && !bMoving && !bNoAutoAcquire && Frame >= NextAutoAcquireFrame)
	{
		NextAutoAcquireFrame = Frame + uint32(GenLogic::GetLogicFPS() / 2) + (ID % 7);
		if (FGenObject* Found = FindAutoTarget())
		{
			TargetID = Found->ID;
			bExplicitAttack = false;
			Target = Found;
		}
	}

	int32 ActiveSlot = INDEX_NONE;
	bool bAiming = false;
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
		const float Range = GetAttackRange(*WI.Template);
		if (Dist > Range)
		{
			if (bExplicitAttack && CanMove() && ContainedBy == GenInvalidObjectID)
			{
				// Approach: head for a free spot within range, refreshed while the target moves.
				if (!bMovingOutOfWay && (!bMoving || Frame >= NextRepathFrame))
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
			// A turret can already track and fire on the move; everything else has to stand still to aim.
			if ((!bMoving || TurretForSlot(ActiveSlot) != INDEX_NONE) && AimAt(*Target, ActiveSlot) && Frame >= WI.ReadyFrame)
			{
				FireWeapon(ActiveSlot, *Target, Frame);
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
		}
	}

	// Object::adjustModelConditionForWeaponStatus
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
	}
}

// ---- damage and death ---------------------------------------------------------------------------

void FGenObject::AttemptDamage(float Amount, const FString& DamageType, const FString& DeathType, FGenObjectID Source)
{
	if (!IsTargetable())
	{
		return;
	}
	if (Armor)
	{
		Amount = Armor->AdjustDamage(DamageType, Amount);
	}
	if (Amount <= 0.f)
	{
		return;
	}
	const bool bImmortal = Template->BodyType.Equals(TEXT("ImmortalBody"), ESearchCase::IgnoreCase);
	const int32 OldDamageState = GetDamageState();
	LastDamageFrame = Logic->GetFrame();
	Health = FMath::Max(bImmortal ? 1.f : 0.f, Health - Amount);

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

	// Fight back when attacked while idle (parked aircraft and planes without orders don't).
	const bool bNoRetaliation = bJetAI && (JetState != EGenJetState::None || !bJetAllowAirLoco || !Template->bAutoAcquireEnemiesWhenIdle);
	if (TargetID == GenInvalidObjectID && !bMoving && HasWeapons() && !bNoRetaliation)
	{
		const FGenObject* Attacker = Logic->FindObject(Source);
		if (Attacker && Attacker->IsTargetable() && Logic->GetRelationship(Owner, Attacker->Owner) == EGenRelationship::Enemies
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
	Health = FMath::Min(MaxHealth, Health + Amount);
	// ActiveBody::setCorrectDamageState
	const FGenGameData& GD = FGenGameData::Get();
	const float Ratio = MaxHealth > 0.f ? Health / MaxHealth : 0.f;
	const bool bReally = !bUnderConstruction && Ratio <= GD.UnitReallyDamagedThreshold;
	SetModelCondition(GenConditions::Rubble(), false);
	SetModelCondition(GenConditions::ReallyDamaged(), bReally);
	SetModelCondition(GenConditions::Damaged(), !bReally && !bUnderConstruction && Ratio <= GD.UnitDamagedThreshold);
}

void FGenObject::Die(const FString& DeathType, FGenObjectID Killer)
{
	if (bDead)
	{
		return;
	}
	const uint32 Frame = Logic->GetFrame();
	bDead = true;
	if (Parking)
	{
		Parking->KillAllParkedUnits(); // ParkingPlaceBehavior::onDie: aircraft on the ground go with their airfield
	}
	if (JetState != EGenJetState::None)
	{
		SetJetState(EGenJetState::None, Frame); // gives back the runway
	}
	AIMode = EGenAIMode::None;
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
	Logic->OnContainerDied(*this);
	StopEntering();
	for (const FProduction& P : Production)
	{
		if (P.Upgrade) { Logic->CancelUpgradeInProduction(Owner, *P.Upgrade); }
	}
	// FireWeaponWhenDeadBehavior: a bomb's bang is the weapon it "fires" where it dies.
	for (const FString& WeaponName : Template->DeathWeapons)
	{
		if (const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(WeaponName))
		{
			Logic->DoFXPos(W->FireFX, Position, nullptr, 0.f, nullptr, W->PrimaryDamageRadius);
			if (!W->FireOCL.IsEmpty()) { Logic->DoOCL(W->FireOCL, *this); }
			Logic->DealDamage(*W, ID, GenInvalidObjectID, Position, true);
		}
	}

	// Die modules: every one whose DeathTypes apply runs (FXListDie, CreateObjectDie, DestroyDie, InstantDeathBehavior).
	FRandomStream& Rand = Logic->GetRandom();
	bool bDestroyNow = false;
	for (const FGenDieModule& M : Template->DieModules)
	{
		if (!DeathTypeApplies(M.DeathTypes, DeathType))
		{
			continue;
		}
		if (M.FX.Num() > 0) { Logic->DoFXObj(M.FX[Rand.RandRange(0, M.FX.Num() - 1)], *this); }
		if (M.OCL.Num() > 0) { Logic->DoOCL(M.OCL[Rand.RandRange(0, M.OCL.Num() - 1)], *this); }
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

	// Pick one applicable SlowDeathBehavior, weighted by ProbabilityModifier.
	TArray<const FGenSlowDeath*> Candidates;
	int32 TotalWeight = 0;
	for (const FGenSlowDeath& D : Template->SlowDeaths)
	{
		if (DeathTypeApplies(D.DeathTypes, DeathType))
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
	else if (Template->IsKindOf(TEXT("STRUCTURE")))
	{
		// Structure collapse modules aren't ported yet: show the rubble state briefly, then remove.
		SetModelCondition(GenConditions::Rubble(), true);
		DestroyFrame = Frame + uint32(GenLogic::GetLogicFPS() * 4);
	}
	else
	{
		DestroyFrame = Frame + 1; // DestroyDie
	}
}

void FGenObject::Kill(const FString& DeathType)
{
	if (bDead || bPendingDestroy)
	{
		return;
	}
	Health = 0.f;
	SetModelCondition(GenConditions::ReallyDamaged(), false);
	SetModelCondition(GenConditions::Damaged(), false);
	SetModelCondition(GenConditions::Rubble(), true);
	Die(DeathType, GenInvalidObjectID);
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

void FGenObject::LaunchProjectile(const FGenWeaponTemplate* Weapon, FGenObjectID Launcher, FGenObjectID Victim, const FVector3f& TargetPos)
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
	const FGenWeaponTemplate& W = *Projectile.Weapon;
	const FGenObject* Victim = Projectile.Victim != GenInvalidObjectID ? Logic->FindObject(Projectile.Victim) : nullptr;
	if (Victim && !Victim->IsTargetable())
	{
		Victim = nullptr;
	}
	if (Victim)
	{
		// Keep tracking a moving victim; projectiles aim at the center of its geometry, not its feet.
		Projectile.End = Victim->Position + FVector3f(0.f, 0.f, Victim->Template->GeometryHeight * 0.5f);
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
				break;
			}
		}
	}

	const FVector3f Move = NewPos - Position;
	if (Move.SizeSquared() > 1e-6f)
	{
		SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, Move.GetSafeNormal()));
	}
	Position = NewPos;

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
		Logic->DealDamage(W, Logic->FindObject(Projectile.Launcher) ? Projectile.Launcher : ID, Projectile.Victim, Position, true);
		bPendingDestroy = true;
	}
}
