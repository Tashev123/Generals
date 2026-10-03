#pragma once

#include "CoreMinimal.h"
#include "Logic/GenConditions.h"
#include "Logic/GenLogicTypes.h"

struct FGenThingTemplate;
struct FGenLocomotorTemplate;
struct FGenWeaponTemplate;
struct FGenArmorTemplate;
struct FGenSlowDeath;
class UGenGameLogic;

/** Runtime state of one weapon slot (Weapon.cpp). */
struct FGenWeaponInstance
{
	const FGenWeaponTemplate* Template = nullptr;
	int32 Slot = 0;                 // 0 PRIMARY, 1 SECONDARY, 2 TERTIARY
	int32 AmmoInClip = 0;
	uint32 ReadyFrame = 0;          // earliest frame for the next shot
	bool bReloading = false;
	uint32 FireCount = 0;           // incremented per shot (client muzzle flash / recoil)
};

/** Projectile flight state (DumbProjectileBehavior / MissileAIUpdate). */
struct FGenProjectileState
{
	bool bActive = false;
	bool bMissile = false;
	const FGenWeaponTemplate* Weapon = nullptr;
	FGenObjectID Launcher = GenInvalidObjectID;
	FGenObjectID Victim = GenInvalidObjectID;
	FVector3f Start = FVector3f::ZeroVector;
	FVector3f End = FVector3f::ZeroVector;
	int32 Segments = 1;
	int32 Step = 0;
};

/**
 * A simulation object (Object.cpp equivalent). Plain C++ for speed; updated once per logic frame.
 * Holds the parts of the original modules implemented so far:
 *   - transform + previous-frame transform (for render interpolation)
 *   - PhysicsBehavior (forces, friction, ground clamp)
 *   - Locomotor (treads / wheels / legs / other) moving towards a goal
 *   - a minimal AIUpdate move state following a pathfinder path
 */
class GENERALS1_API FGenObject
{
public:
	FGenObject(FGenObjectID InID, const FGenThingTemplate* InTemplate, int32 InOwner, UGenGameLogic* InLogic);

	FGenObjectID GetID() const { return ID; }
	const FGenThingTemplate* GetTemplate() const { return Template; }
	int32 GetOwner() const { return Owner; }
	bool IsKindOf(const TCHAR* Kind) const;

	const FVector3f& GetPosition() const { return Position; }
	float GetOrientation() const { return Orientation; }
	void SetPosition(const FVector3f& P) { Position = P; }
	/** Sets the heading; pitch and roll are reset (the object stands upright). */
	void SetOrientation(float A) { Orientation = GenLogic::NormalizeAngle(A); Rotation = FQuat4f(FVector3f::UpVector, Orientation); }
	FVector2f GetDirection2D() const { return FVector2f(FMath::Cos(Orientation), FMath::Sin(Orientation)); }

	/** Full orientation in Generals space (heading only for ground units; tumbling debris and projectiles use all axes). */
	const FQuat4f& GetRotation() const { return Rotation; }
	void SetRotation(const FQuat4f& Q);
	FTransform3f GetTransform() const { return FTransform3f(Rotation, Position); }
	bool HasFreeRotation() const { return bFreeRotation; }
	const FVector3f& GetVelocity() const { return Velocity; }

	/** Transform at the previous logic frame (render interpolation). */
	const FVector3f& GetPrevPosition() const { return PrevPosition; }
	float GetPrevOrientation() const { return PrevOrientation; }
	const FQuat4f& GetPrevRotation() const { return PrevRotation; }
	void SnapshotPrevious() { PrevPosition = Position; PrevOrientation = Orientation; PrevRotation = Rotation; }

	/** A living, locomotor-driven unit on the ground: takes part in unit-vs-unit collision. */
	bool IsGroundUnit() const { return CanMove() && !bDead && !bPendingDestroy && !Projectile.bActive; }
	/** Radius for unit-vs-unit overlap tests (boxes: between their half-width and half-length). */
	float GetCollisionRadius() const;
	uint32 GetBlockedFrames() const { return BlockedFrames; }

	/** The drawable has to follow this object every frame (it moves, tumbles, flies or sinks). */
	bool IsDynamic() const { return CanMove() || bDead || Projectile.bActive || bFreePhysics; }

	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }

	/** Has an AI module and a locomotor: can receive move orders. */
	bool CanMove() const { return Locomotor != nullptr && bHasAI && !bImmobile; }
	bool IsMoving() const { return bMoving; }
	bool IsSelectable() const;

	void CommandMoveTo(const FVector3f& Goal);
	void CommandStop();

	void Update(uint32 Frame);

	/** Model condition flags drive the drawable's condition state (W3DModelDraw). */
	const FGenConditionBits& GetModelConditions() const { return ModelConditions; }
	uint32 GetConditionVersion() const { return ConditionVersion; }
	void SetModelCondition(int32 Bit, bool bOn);

	// --- combat ---
	bool HasWeapons() const;
	bool IsDead() const { return bDead; }
	bool IsDestroyed() const { return bPendingDestroy; }
	bool IsProjectile() const { return Projectile.bActive; }
	bool IsTargetable() const;
	/** True if the point lies inside the object's collision geometry (box or cylinder). */
	bool ContainsPoint(const FVector3f& P) const;
	/** Attack an object (explicit order). */
	void CommandAttack(FGenObjectID Target);
	FGenObjectID GetTargetID() const { return TargetID; }
	/** Body::attemptDamage: armor, health, damage state, death. */
	void AttemptDamage(float Amount, const FString& DamageType, const FString& DeathType, FGenObjectID Source);
	/** Turret angle relative to the body (radians) for turret slot 0/1, and previous frame value. */
	float GetTurretAngle(int32 Turret) const { return TurretAngle[Turret]; }
	float GetPrevTurretAngle(int32 Turret) const { return PrevTurretAngle[Turret]; }
	const FGenWeaponInstance& GetWeapon(int32 Slot) const { return Weapons[Slot]; }
	void LaunchProjectile(const FGenWeaponTemplate* Weapon, FGenObjectID Launcher, FGenObjectID Victim, const FVector3f& TargetPos);
	float GetDeathSink() const { return SinkOffset; }
	/** Object::kill: dies regardless of armor or health (lifetime expiry, debris coming to rest). */
	void Kill(const FString& DeathType = TEXT("NORMAL"));
	/** The object that created this one through an ObjectCreationList. */
	FGenObjectID GetProducer() const { return Producer; }

private:
	// --- free physics (PhysicsBehavior for objects without a locomotor: debris, hulks) ---
	void UpdateFreePhysics(uint32 Frame);
	float GetYawFromRotation() const;
	void DoDeathPhase(int32 Phase);
	// --- combat (GenObjectCombat.cpp) ---
	void InitCombat();
	void UpdateCombat(uint32 Frame);
	void UpdateProjectile(uint32 Frame);
	void UpdateDying(uint32 Frame);
	void Die(const FString& DeathType, FGenObjectID Killer);
	FGenObject* FindAutoTarget() const;
	int32 ChooseWeapon(const FGenObject& Target) const;
	bool CanWeaponTarget(const FGenWeaponTemplate& W, const FGenObject& Target) const;
	float AttackDistance(const FGenObject& Target) const;
	bool AimAt(const FGenObject& Target, int32 Slot);
	void FireWeapon(int32 Slot, FGenObject& Target, uint32 Frame);
	void ClearAttack();
	void SetFiringConditions(int32 Slot, int32 State);
	int32 TurretForSlot(int32 Slot) const;
	// --- AI ---
	void UpdateAI(uint32 Frame);
	float PathDistanceFrom(int32 Index) const;
	/** Picks a free destination near InGoal (one that also satisfies Accept, if given), reserves it and starts following a path to it. */
	bool StartPath(const FVector3f& InGoal, bool bAroundUnits, const TFunction<bool(const FVector2f&)>* Accept = nullptr);
	/** A spot from which the weapon reaches Target, as close to where we are coming from as possible. */
	bool StartApproach(const FGenObject& Target, float Range);
	bool IsWithinRangeFrom(const FVector2f& From, const FGenObject& Target, float Range) const;
	/** Oriented box / circle overlap of the two units' geometry, shrunk a little so touching doesn't count. */
	bool GeometryOverlaps(const FGenObject& Other, float Shrink) const;

	// --- unit collision (GenCollision.cpp; AIUpdateInterface::processCollision and friends) ---
	void ProcessCollision(FGenObject& Other);
	bool BlockedBy(const FGenObject& Other) const;
	bool HasHigherPathPriority(const FGenObject& Other) const;
	float CalculateMaxBlockedSpeed(const FGenObject& Other) const;
	bool NeedToRotate() const;
	bool CanCrushOrSquish(const FGenObject& Other) const;
	void TryCrush(FGenObject& Other) const;
	void MoveAwayFromUnit(const FGenObject& Unit);
	void MoveToFreeGround();

	// --- Locomotor (Locomotor.cpp) ---
	void LocoMoveTowardsPosition(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame);
	void MoveTreads(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed);
	void MoveWheels(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame);
	void MoveLegs(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed);
	int32 RotateAroundLocoPivot(const FVector3f& Goal, float MaxTurnRate, float* OutRelAngle = nullptr);
	void ApplyGoalSpeed(float GoalSpeed, float ActualSpeed, float MaxAccel, float BrakingFactor, bool bBackwards = false);
	float GetBraking() const;

	// --- PhysicsBehavior ---
	void UpdatePhysics(uint32 Frame);
	void ApplyForce(const FVector3f& Force) { Accel += Force / Mass; }
	void ApplyMotiveForce(const FVector3f& Force, uint32 Frame);
	float GetForwardSpeed2D() const;

	FGenObjectID ID;
	const FGenThingTemplate* Template;
	int32 Owner;
	UGenGameLogic* Logic;

	FVector3f Position = FVector3f::ZeroVector;
	float Orientation = 0.f;
	FVector3f PrevPosition = FVector3f::ZeroVector;
	float PrevOrientation = 0.f;
	FQuat4f Rotation = FQuat4f::Identity;
	FQuat4f PrevRotation = FQuat4f::Identity;
	bool bFreeRotation = false;        // pitch / roll in use: the drawable follows Rotation instead of the heading

	float Health = 1.f;
	float MaxHealth = 1.f;
	bool bHasAI = false;
	bool bImmobile = false;

	FGenConditionBits ModelConditions;
	uint32 ConditionVersion = 1;

	// Physics
	FVector3f Velocity = FVector3f::ZeroVector;
	FVector3f Accel = FVector3f::ZeroVector;
	float Mass = 1.f;
	float ForwardFriction = 0.15f;
	float LateralFriction = 0.15f;
	uint32 MotiveForceExpires = 0;
	bool bStatusBraking = false;     // OBJECT_STATUS_BRAKING
	bool bFreePhysics = false;       // gravity / bounce / tumble simulation is running
	bool bAllowBounce = false;       // ALLOW_BOUNCE
	float ExtraFriction = 0.f;
	float YawRate = 0.f, PitchRate = 0.f, RollRate = 0.f;

	// LifetimeUpdate / ObjectCreationList
	uint32 LifetimeDieFrame = 0;
	FGenObjectID Producer = GenInvalidObjectID;

	// Locomotor
	const FGenLocomotorTemplate* Locomotor = nullptr;
	bool bLocoBraking = false;       // IS_BRAKING
	bool bLocoBackwards = false;     // MOVING_BACKWARDS
	bool bLocoThreePointTurn = false;
	float BrakingFactor = 1.f;
	uint32 DonutTimer = 0;

	// AI move state
	bool bMoving = false;
	TArray<FVector2f> Path;
	int32 PathIndex = 0;
	FVector3f Goal = FVector3f::ZeroVector;
	FVector3f RequestedGoal = FVector3f::ZeroVector; // where the move order asked to go (re-paths aim here)
	uint32 StuckFrames = 0;

	// Unit collision
	bool bBlocked = false;             // another unit is in the way (set by the collision pass, consumed next frame)
	bool bBlockedAndStuck = false;     // ... and it is not going to move: path around it
	uint32 BlockedFrames = 0;
	float CurMaxBlockedSpeed = 99999.f; // fastest we may go without running into the unit ahead
	uint32 IgnoreCollisionsUntil = 0;
	uint32 NextBlockedRepathFrame = 0;
	bool bSidestepPath = false;        // the current path was made to get around a parked unit
	bool bMovingOutOfWay = false;      // AI_MOVE_OUT_OF_THE_WAY: stepping aside for MoveOutOfWayOf
	FGenObjectID MoveOutOfWayOf = GenInvalidObjectID;
	bool bHasResumeGoal = false;       // continue to ResumeGoal after stepping aside
	FVector3f ResumeGoal = FVector3f::ZeroVector;

	// Combat
	FGenWeaponInstance Weapons[3];
	const FGenArmorTemplate* Armor = nullptr;
	FGenObjectID TargetID = GenInvalidObjectID;
	bool bExplicitAttack = false;      // player ordered this attack (pursue)
	uint32 NextAutoAcquireFrame = 0;
	uint32 NextRepathFrame = 0;
	float TurretAngle[2] = { 0.f, 0.f };
	float PrevTurretAngle[2] = { 0.f, 0.f };
	uint32 TurretIdleSince[2] = { 0, 0 };
	int32 FiringState[3] = { 0, 0, 0 }; // 0 none, 1 firing, 2 between shots, 3 reloading
	uint32 FiringConditionEnd[3] = { 0, 0, 0 };

	// Death
	bool bDead = false;
	bool bPendingDestroy = false;
	uint32 DestroyFrame = 0;
	uint32 SinkStartFrame = 0;
	float SinkRate = 0.f;
	float SinkOffset = 0.f;
	const FGenSlowDeath* SlowDeath = nullptr; // the chosen SlowDeathBehavior
	uint32 MidpointFrame = 0;
	bool bMidpointDone = false;

	// Projectile
	FGenProjectileState Projectile;

	friend class UGenGameLogic;
};
