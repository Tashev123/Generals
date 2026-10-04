#pragma once

#include "CoreMinimal.h"
#include "Logic/GenConditions.h"
#include "Logic/GenLogicTypes.h"

struct FGenThingTemplate;
struct FGenPolygonTrigger;
struct FGenLocomotorTemplate;
struct FGenWeaponTemplate;
struct FGenArmorTemplate;
struct FGenSlowDeath;
struct FGenUpgradeTemplate;
struct FGenUpgradeModule;
struct FGenDeliverPayload;
class UGenGameLogic;
class FGenObject;

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

/** Standing orders given by scripts / the computer player to its teams (AI_HUNT, AI_GUARD, AI_FOLLOW_WAYPOINT_PATH ...). */
enum class EGenAIMode : uint8
{
	None,
	Hunt,         // seek out and attack the closest enemy, wherever it is, again and again
	Guard,        // hold a spot: attack what comes inside the guarded radius, then return
	FollowPath,   // move along a list of points, then be idle
	AttackMove,   // move to a spot, stopping to fight whatever comes into view on the way
};

/** GuardMode */
enum class EGenGuardMode : uint8
{
	Normal,           // attack what comes inside the inner radius, chase it to the outer one, then return
	WithoutPursuit,   // never leave the post: only what the weapons reach from there
	FlyingOnly,       // aircraft only
};

/** AIDockMachine states: how far a unit has got with docking at a building. */
enum class EGenDockStage : uint8
{
	None,
	Approach,           // heading for a waiting spot beside the dock
	WaitForClearance,   // queued until the dock lets us in
	AdvancePosition,    // moving up one place in the queue
	MoveToEntry,
	MoveToDock,
	ProcessDock,        // the dock's action runs until it says we are done
	MoveToExit,
};

/** SupplyTruckStateMachine states. */
enum class EGenSupplyState : uint8
{
	Busy,        // doing something the player ordered
	Idle,        // off autopilot: waits for an order
	Wanting,     // looks for the next warehouse (empty) or supply center (loaded)
	Regrouping,  // nothing to gather: goes home and tries again
	Docking,
};

/** DockUpdate state of a supply warehouse or supply center (SupplyWarehouseDockUpdate / SupplyCenterDockUpdate). */
struct FGenDock
{
	int32 Boxes = 0;                           // supply boxes left (warehouses)
	bool bPositionsLoaded = false;
	bool bHasDockBones = false;                // DockStart / DockAction / DockEnd exist on the model
	int32 NumApproachBones = 0;                // DockWaitingNN
	FVector3f EnterPos = FVector3f::ZeroVector; // in the building's frame
	FVector3f DockPos = FVector3f::ZeroVector;
	FVector3f ExitPos = FVector3f::ZeroVector;
	TArray<FVector3f> ApproachPositions;       // in the building's frame (zero without bones)
	TArray<FGenObjectID> ApproachOwners;       // who holds each waiting spot
	TArray<bool> ApproachReached;
	FGenObjectID ActiveDocker = GenInvalidObjectID; // the one unit allowed in
	bool bDockerInside = false;
	FGenObjectID LastRepair = GenInvalidObjectID;   // RepairDockUpdate: who the repair rate was worked out for
	float HealthPerFrame = 0.f;
};

/** A container's occupants and its doors (OpenContain and the modules built on it). */
struct FGenContain
{
	TArray<FGenObjectID> Riders;               // who is inside (a tunnel's are kept by the player: all tunnels share them)
	int32 ExtraSlotsInUse = 0;                 // slots taken by riders that need more than one
	uint32 ExitNotBusyFrame = 0;               // ExitDelay: nobody leaves before this
	int32 WhichExitPath = 1;                   // which ExitStartNN / ExitEndNN pair is next
	bool bPayloadCreated = false;
	int32 OriginalOwner = INDEX_NONE;          // garrison: whose the building was before it was occupied
	bool bFirePointsLoaded = false;
	uint32 FirePointsVersion = 0;              // the building's model condition version the fire points were read for
	TArray<FVector3f> FirePoints;              // FIREPOINTnn bones of the model now showing, in world space
	TArray<FGenObjectID> FirePointUsers;
	TArray<FGenObjectID> Wanters;              // units on their way in (a helicopter lands for them)
};

/** ChinookFlightStatus */
enum class EGenChinookFlight : uint8 { Flying, Landing, Landed, TakingOff };

/** JetAIStateType: where an aircraft is in its airfield routine (None: ordinary AI, in the air or parked). */
enum class EGenJetState : uint8
{
	None,
	TaxiFromHangar,          // just built: out of the hangar to its parking space
	TakeoffAwaitClearance,   // parked with something to do: waits for its runway
	TaxiToTakeoff,
	PauseBeforeTakeoff,      // lined up, afterburners on
	TakingOff,
	LandingAwaitClearance,   // circles until the runway is free
	Landing,
	TaxiFromLanding,
	OrientForParking,
	ReloadAmmo,
	ReturningForLanding,     // flying home
	ReturnToDeadAirfield,    // home is gone: back to where it was
	CirclingDeadAirfield,    // ... and circle there, losing health if out of ammo, until another airfield has room
};

/** ParkingPlaceBehaviorInterface::PPInfo: where one aircraft parks, taxis, takes off and lands (world space). */
struct FGenParkingInfo
{
	FVector3f ParkingSpace = FVector3f::ZeroVector;
	float ParkingOrientation = 0.f;
	FVector3f RunwayPrep = FVector3f::ZeroVector;
	FVector3f RunwayStart = FVector3f::ZeroVector;
	FVector3f RunwayEnd = FVector3f::ZeroVector;
	FVector3f RunwayApproach = FVector3f::ZeroVector;   // in the air beyond the far end: landings start here
	FVector3f RunwayExit = FVector3f::ZeroVector;       // ... and takeoffs end here
	FVector3f HangarInternal = FVector3f::ZeroVector;
	float HangarInternalOrient = 0.f;
	float RunwayTakeoffDist = 0.f;
};

/** ParkingPlaceBehavior: an airfield's parking spaces, hangars and runways, and who holds them. */
class GENERALS1_API FGenParkingPlace
{
public:
	enum { DoorNoneNeeded = -1, DoorNoneAvailable = -2 };

	struct FSpace
	{
		FVector3f HangarStart = FVector3f::ZeroVector;
		float HangarOrient = 0.f;
		FVector3f Location = FVector3f::ZeroVector;
		float Orient = 0.f;
		FVector3f Prep = FVector3f::ZeroVector;
		int32 Runway = 0;
		FGenObjectID Occupant = GenInvalidObjectID;
		bool bReservedForExit = false;      // an aircraft being built will get this space
		uint8 DoorState = 0;                // hangar door: 0 closed, 1 opening, 2 open, 3 closing
		uint32 DoorTimer = 0;               // frame the opening / closing ends
	};
	struct FRunway
	{
		FVector3f Start = FVector3f::ZeroVector;
		FVector3f End = FVector3f::ZeroVector;
		FGenObjectID InUseBy = GenInvalidObjectID;
		FGenObjectID NextInLineForTakeoff = GenInvalidObjectID;
		bool bWasInLine = false;
	};

	FGenParkingPlace(FGenObject& InAirfield, UGenGameLogic* InLogic) : Airfield(InAirfield), Logic(InLogic) {}

	/** Drops the dead from spaces and runways and heals the parked (every frame). */
	void Update(uint32 Frame);
	bool HasReservedSpace(FGenObjectID Id) const;
	/** A free space for one more aircraft of this kind (helicopters built on the helipad need none). */
	bool HasAvailableSpaceFor(const FGenThingTemplate& Thing) const;
	/** Keeps (or takes) a parking space for the aircraft; OutInfo, if given, says where everything is. */
	bool ReserveSpace(FGenObjectID Id, float ParkingOffset, FGenParkingInfo* OutInfo);
	bool CalcInfo(FGenObjectID Id, FGenParkingInfo& OutInfo);
	void ReleaseSpace(FGenObjectID Id);
	/** The runway of the aircraft's space, if nobody is on it; a second aircraft taking off queues behind the first. */
	bool ReserveRunway(FGenObjectID Id, bool bForLanding);
	void ReleaseRunway(FGenObjectID Id);
	void TransferRunwayReservationToNextInLineForTakeoff(FGenObjectID Id);
	int32 GetRunwayCount() const { return Runways.Num(); }
	FGenObjectID GetRunwayReservation(int32 Runway);
	void SetHealee(FGenObjectID Id, bool bAdd);
	/** The airfield died: aircraft on the ground go with it. */
	void KillAllParkedUnits();
	/** A space for an aircraft about to be built: its index, DoorNoneNeeded or DoorNoneAvailable. */
	int32 ReserveDoorForExit(const FGenThingTemplate& Thing);
	void UnreserveDoorForExit(int32 Door);
	/** The finished aircraft takes the space reserved for it. */
	bool ClaimDoor(FGenObjectID Id, int32 Door);
	/** HeliPark01: where helicopters built here appear. */
	bool GetHeliPark(FVector3f& OutPos, float& OutAngle);
	const TArray<FSpace>& GetSpaces() const { return Spaces; }
	const TArray<FRunway>& GetRunways() const { return Runways; }

private:
	void BuildInfo();
	void PurgeDead();
	void UpdateDoors(uint32 Frame);
	FSpace* FindSpace(FGenObjectID Id);
	FSpace* FindEmptySpace();
	bool IsGone(FGenObjectID Id) const;

	FGenObject& Airfield;
	UGenGameLogic* Logic;
	bool bGotInfo = false;
	TArray<FSpace> Spaces;
	TArray<FRunway> Runways;
	TArray<FGenObjectID> Healing;
	uint32 NextHealFrame = MAX_uint32;
	bool bHasHeliPark = false;
	FVector3f HeliPark = FVector3f::ZeroVector;
	float HeliParkOrient = 0.f;
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
	FGenObject(const FGenObject&) = delete;
	FGenObject& operator=(const FGenObject&) = delete;

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
	bool IsGroundUnit() const
	{
		return CanMove() && !bFlies && !bDead && !bPendingDestroy && !Projectile.bActive && !bHeldByParachute && ContainedBy == GenInvalidObjectID;
	}
	/** Moves through the air only (helicopters, planes): flies straight to its goal above everything. */
	bool IsFlying() const { return bFlies; }
	/** OBJECT_STATUS_AIRBORNE_TARGET: high enough above the terrain that only anti-air weapons reach it. */
	bool IsAirborneTarget() const;
	/** Radius for unit-vs-unit overlap tests (boxes: between their half-width and half-length). */
	float GetCollisionRadius() const;
	uint32 GetBlockedFrames() const { return BlockedFrames; }

	/** The drawable has to follow this object every frame (it moves, tumbles, flies or sinks). */
	bool IsDynamic() const { return CanMove() || bDead || Projectile.bActive || bFreePhysics; }

	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }
	/** BodyDamageType: 0 pristine, 1 damaged, 2 really damaged, 3 rubble (no health left). */
	int32 GetDamageState() const;

	/** Has an AI module and a locomotor: can receive move orders. */
	bool CanMove() const { return Locomotor != nullptr && bHasAI && !bImmobile; }
	bool IsMoving() const { return bMoving; }
	bool IsSelectable() const;

	void CommandMoveTo(const FVector3f& Goal);
	/** Halts the current move (also used internally; an order to stop is CommandIdle). */
	void CommandStop();
	/** The order to stop and do nothing (AICMD_IDLE): drops standing orders and the attack; a plane in the air goes home. */
	void CommandIdle();

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
	/** Body::attemptHealing: health back, up to the maximum; the damage state follows. */
	void AttemptHealing(float Amount);
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

	// --- construction and production (GenEconomy.cpp) ---
	/** One unit being built by a factory (ProductionUpdate queue entry). */
	struct FProduction
	{
		const FGenThingTemplate* Unit = nullptr;
		const FGenUpgradeTemplate* Upgrade = nullptr; // an upgrade being researched instead of a unit
		float FramesDone = 0.f;
		int32 Cost = 0;              // what was paid; refunded on cancel
		int32 ExitDoor = -1;         // airfields: the parking space kept for it (FGenParkingPlace::DoorNoneNeeded = none)
	};
	/** OBJECT_STATUS_UNDER_CONSTRUCTION: placed, but a dozer still has to finish it. */
	bool IsUnderConstruction() const { return bUnderConstruction; }
	float GetConstructionPercent() const { return ConstructionPercent; }
	bool IsBeingSold() const { return SellFrame != 0; }
	const TArray<FProduction>& GetProductionQueue() const { return Production; }
	/** The building this dozer is constructing (or on its way to). */
	FGenObjectID GetBuildTarget() const { return BuildTarget; }
	/** The building this dozer is mending (DOZER_TASK_REPAIR). */
	FGenObjectID GetRepairTarget() const { return RepairTarget; }
	/** DozerAIUpdate::isAnyTaskPending: building or repairing. */
	bool HasDozerTask() const { return BuildTarget != GenInvalidObjectID || RepairTarget != GenInvalidObjectID; }
	/** DozerAIUpdate::privateRepair: go and mend that building. False if it can't (or someone else already is). */
	bool StartRepair(FGenObject& Building);
	/** Object::getSoleHealingBenefactor: who is mending this object right now, if anybody. */
	FGenObjectID GetSoleHealingBenefactor() const;
	/** Object::attemptHealingFromSoleBenefactor: healing from one source at a time; others are turned away. */
	bool AttemptHealingFromSoleBenefactor(float Amount, FGenObjectID Source, uint32 Duration);

	// --- garrisons and transports (GenContain.cpp) ---
	/** What this object carries or shelters; null for everything that isn't a container. */
	const FGenContain* GetContain() const { return Contain.Get(); }
	FGenObjectID GetContainedBy() const { return ContainedBy; }
	bool IsContained() const { return ContainedBy != GenInvalidObjectID; }
	/** Inside a container: not drawn. */
	bool IsHidden() const { return bHidden; }
	/** The container this unit is on its way to (AI_ENTER). */
	FGenObjectID GetEnterTarget() const { return EnterTarget; }
	/** AIUpdateInterface::aiEnter: go to the container and get in. */
	void CommandEnter(FGenObject& Container);
	/** AIUpdateInterface::aiExit: leave the container as soon as its door is free. */
	void CommandExit() { bWantsExit = ContainedBy != GenInvalidObjectID; }
	bool WantsToExit() const { return bWantsExit; }
	void SetOwner(int32 NewOwner) { Owner = NewOwner; }
	EGenChinookFlight GetChinookFlight() const { return ChinookFlight; }
	/** The weapon's range for this object (its bonuses included: a garrisoned soldier sees further). */
	float GetAttackRange(const FGenWeaponTemplate& Weapon) const;

	// --- rally point (ExitInterface) ---
	bool HasRallyPoint() const { return bHasRallyPoint; }
	const FVector3f& GetRallyPoint() const { return RallyPoint; }
	void SetRallyPoint(const FVector3f& Pos) { RallyPoint = Pos; bHasRallyPoint = true; }
	/** Where units come out of this building (the template's NaturalRallyPoint; with bOffset a little beyond it). */
	FVector3f GetNaturalRallyPoint(bool bOffset) const;

	// --- supply gathering and docking (GenSupply.cpp) ---
	/** Dock state of a supply warehouse / supply center; null for everything else. */
	const FGenDock* GetDock() const { return Dock.Get(); }
	/** Supply boxes carried (gatherers) or left in stock (warehouses). */
	int32 GetBoxes() const { return Dock ? Dock->Boxes : Boxes; }
	EGenSupplyState GetSupplyState() const { return SupplyState; }
	EGenDockStage GetDockStage() const { return DockStage; }
	/** The building this unit is docking with (AI_DOCK goal object). */
	FGenObjectID GetDockTarget() const { return DockTarget; }
	/** Nothing ordered and nothing under way (AIUpdateInterface::isIdle). */
	bool IsAIIdle() const;

	// --- standing orders (GenObjectAI.cpp) ---
	EGenAIMode GetAIMode() const { return AIMode; }
	void CommandHunt();
	/** Guards a spot; Radius is how far out enemies are engaged (0 = this unit's own guard range), Area limits it to a map area. */
	void CommandGuard(const FVector2f& Pos, float Radius, const FGenPolygonTrigger* Area = nullptr);
	void CommandFollowPath(const TArray<FVector2f>& Points);
	/** AIAttackMoveToState: go there, fighting whatever comes into view on the way. */
	void CommandAttackMove(const FVector3f& Goal);
	/** A player's guard order (GuardInnerModifierHuman / GuardOuterModifierHuman): hold that spot, or stay with that object. */
	void CommandGuardPosition(const FVector2f& Pos, EGenGuardMode Mode, FGenObjectID GuardObject = GenInvalidObjectID);

	// --- veterancy (GenVeterancy.cpp) ---
	/** 0 regular, 1 veteran, 2 elite, 3 heroic. */
	int32 GetVeterancyLevel() const { return VeterancyLevel; }
	int32 GetExperience() const { return Experience; }
	/** ExperienceTracker::addExperiencePoints */
	void AddExperience(int32 Points);
	void SetVeterancyLevel(int32 NewLevel);
	void ClearAIMode() { AIMode = EGenAIMode::None; }
	/** Frame this object last took damage, and from whom. */
	uint32 GetLastDamageFrame() const { return LastDamageFrame; }

	// --- upgrades (GenUpgrades.cpp) ---
	/** The object itself has this (object) upgrade. */
	bool HasObjectUpgrade(const FString& UpgradeName) const;
	/** Has it, counting what its player has researched. */
	bool HasUpgrade(const FString& UpgradeName) const;
	/** Object::affectedByUpgrade: some module of this object would react to the upgrade. */
	bool AffectedByUpgrade(const FGenUpgradeTemplate& Upgrade) const;
	/** Some module the upgrade triggers on this object does something here (false: drones, add-ons and the like, not there yet). */
	bool HasPortedUpgradeEffect(const FGenUpgradeTemplate& Upgrade) const;
	void GiveObjectUpgrade(const FGenUpgradeTemplate& Upgrade);
	/** Object::updateUpgradeModules: runs every module whose upgrades have arrived (each runs once). */
	void UpdateUpgradeModules();
	/** The command set shown for this object (a CommandSetUpgrade may have replaced the template's). */
	const FString& GetCommandSet() const;
	/** Power produced (> 0) or used (< 0), with the control rods bonus. */
	int32 GetEnergyProduction() const;
	/** Factor a weapon gains from this object's bonus conditions (WeaponBonus lines; 1 = none). */
	float GetWeaponBonus(const FGenWeaponTemplate& Weapon, int32 Field) const;
	/** Sub-objects an upgrade shows or hides on the model, applied on top of the condition state. */
	const TArray<TPair<FString, bool>>& GetSubObjectOverrides() const { return SubObjectOverrides; }
	uint32 GetSubObjectVersion() const { return SubObjectVersion; }

	// --- special powers and payload deliveries (GenSpecialPowers.cpp) ---
	/** Frame the object's own (not player-shared) power in that module slot is ready again. */
	uint32 GetPowerReadyFrame(int32 ModuleIndex) const { return PowerReady.IsValidIndex(ModuleIndex) ? PowerReady[ModuleIndex] : 0; }
	bool IsPowerPaused(int32 ModuleIndex) const { return PowerPaused.IsValidIndex(ModuleIndex) && PowerPaused[ModuleIndex]; }
	/** Flying a delivery for a special power (DeliverPayloadAIUpdate). */
	bool IsDelivering() const { return Delivery.IsValid(); }
	/** Knocked out by an EMP for the time being. */
	bool IsDisabled() const;
	void DisableUntil(uint32 Frame) { DisabledUntilFrame = FMath::Max(DisabledUntilFrame, Frame); }

	// --- jets and airfields (GenJet.cpp) ---
	/** Parking spaces and runways of an airfield; null for everything else. */
	FGenParkingPlace* GetParking() const { return Parking.Get(); }
	EGenJetState GetJetState() const { return JetState; }
	/** On the ground at its airfield (or anywhere, for a helicopter that has landed): may not fly until it takes off. */
	bool IsParked() const { return bJetAI && !bJetAllowAirLoco; }
	/** Every weapon that only reloads at an airfield is empty. */
	bool IsOutOfSpecialReloadAmmo() const;
	/** The weapon in this slot has a clip that is empty and doesn't refill by itself. */
	bool IsWeaponOutOfAmmo(int32 Slot) const;
	/** JetAIUpdate::doLandingCommand: fly to this airfield and park there (it becomes home). */
	void CommandLandAt(FGenObjectID AirfieldId);
	/** How far the model is lifted so it is never drawn lower than MinHeight above the ground (wheels on the tarmac). */
	float GetDrawLift(const FVector3f& RenderPos) const;

private:
	// --- upgrades ---
	void InitUpgrades();
	bool WouldUpgrade(const FGenUpgradeModule& Module, const TArray<FString>& ExtraUpgrades) const;
	void ExecuteUpgradeModule(const FGenUpgradeModule& Module);
	void RefreshWeaponSet();
	void UpdateAutoHeal(uint32 Frame);

	TArray<FString> ObjectUpgrades;        // lower-case names of the object upgrades it has
	TArray<bool> UpgradeModuleDone;        // parallel to the template's UpgradeModules
	TArray<FString> SetFlags;              // weapon / armor set flags in force (PLAYER_UPGRADE)
	TArray<FString> BonusConditions;       // weapon bonus conditions in force (PLAYER_UPGRADE)
	bool bArmorUpgraded = false;
	bool bLocoUpgraded = false;            // SET_NORMAL_UPGRADED replaces SET_NORMAL
	bool bPowerPlantUpgraded = false;
	FString CommandSetOverride;
	TArray<TPair<FString, bool>> SubObjectOverrides;
	uint32 SubObjectVersion = 0;
	bool bAutoHealActive = false;
	bool bAutoHealDone = false;            // SingleBurst has fired
	uint32 NextAutoHealFrame = 0;

	// --- special powers ---
	TArray<uint32> PowerReady;             // parallel to the template's PowerModules
	TArray<bool> PowerPaused;
	// --- payload: things dropped from aircraft, and markers that delete themselves ---
	void InitPayload();
	/** False when the object was dealt with here (deleted, went off, hangs under a parachute). */
	bool UpdatePayload(uint32 Frame);
	void DropPayloadItem(uint32 Frame);
	void FireVisiblePayload(uint32 Frame);
	uint32 DisabledUntilFrame = 0;         // EMP: no moving, shooting or building until then
	uint32 DeletionFrame = 0;              // DeletionUpdate
	uint32 EMPFrame = 0;                   // EMPUpdate: when the pulse takes effect
	bool bEMPDone = false;
	void DoEMP(uint32 Frame);
	uint32 HeightDieArmedFrame = 0;
	bool bFalling = false;                 // dropped: falls freely until HeightDieUpdate ends it
	FGenObjectID ParachuteRider = GenInvalidObjectID; // ParachuteContain: who hangs under this parachute
	bool bHeldByParachute = false;         // ... and the rider: hidden from orders until it lands
	bool bChuteOpen = false;               // the free fall is over
	float ChuteStartZ = 0.f;
	// --- FireWeaponUpdate ---
	struct FAreaWeapon { uint32 ReadyFrame = 0; int32 Ammo = 0; };
	TArray<FAreaWeapon> AreaWeapons;       // parallel to the template's FireWeaponUpdates
	void UpdateAreaWeapons(uint32 Frame);
	// --- DeliverPayloadAIUpdate ---
	struct FDelivery
	{
		const FGenDeliverPayload* Data = nullptr;
		FVector3f Target = FVector3f::ZeroVector;
		FVector3f MoveTo = FVector3f::ZeroVector;    // where it is heading right now
		int32 State = 0;                // 0 flying in, 1 delivering, 2 leaving
		int32 ItemsLeft = 0;            // payload objects still on board
		int32 VisibleLeft = 0;          // visible-payload weapon shots still to fire
		uint32 NextDropFrame = 0;
		uint32 StartFrame = 0;          // DelayDeliveryMax: not before this
		int32 Attempts = 0;
		float PrevDist = FLT_MAX;       // distance to the target last frame (flying towards it, or away?)
		int32 DiveState = 0;            // 0 before the dive, 1 diving, 2 done
	};
	TUniquePtr<FDelivery> Delivery;
	/** False when the aircraft does not move this frame (not started yet, or just removed). */
	bool UpdateDelivery(uint32 Frame);

	void UpdateAIMode(uint32 Frame);
	/** Closest enemy this unit has a weapon for, within Radius of Center (Radius < 0: anywhere) and inside Area, if given. */
	FGenObject* FindEnemyNear(const FVector2f& Center, float Radius, const FGenPolygonTrigger* Area = nullptr) const;

	EGenAIMode AIMode = EGenAIMode::None;
	FVector2f GuardPos = FVector2f::ZeroVector;
	float GuardRadius = 0.f;
	float GuardOuterRadius = 0.f;      // how far a guard chases
	EGenGuardMode GuardMode = EGenGuardMode::Normal;
	FGenObjectID GuardObject = GenInvalidObjectID; // escorting: the post moves with this object
	const FGenPolygonTrigger* GuardArea = nullptr; // owned by the map
	FVector3f AttackMoveGoal = FVector3f::ZeroVector;
	// --- veterancy ---
	void InitVeterancy();
	void OnVeterancyLevelChanged(int32 OldLevel, int32 NewLevel);
	int32 VeterancyLevel = 0;
	int32 Experience = 0;
	TArray<FVector2f> RoutePoints;
	int32 RouteIndex = 0;
	uint32 NextAIModeFrame = 0;
	uint32 LastDamageFrame = 0;

	// --- docks ---
	void InitSupply();
	void LoadDockPositions();
	void UpdateDock(uint32 Frame);
	bool IsDockOpen() const;
	bool IsClearToApproach(FGenObjectID Docker) const;
	bool ReserveApproachPosition(const FGenObject& Docker, FVector3f& OutPos, int32& OutIndex);
	bool AdvanceApproachPosition(const FGenObject& Docker, FVector3f& OutPos, int32& InOutIndex);
	FVector3f ComputeApproachPosition(int32 Index, const FGenObject& Docker);
	FVector3f GetDockPoint(const FVector3f& LocalBone, const FGenObject& Docker, bool bEntry) const;
	void OnApproachReached(FGenObjectID Docker);
	void OnEnterReached(FGenObject& Docker);
	void OnExitReached(FGenObject& Docker);
	void CancelDocker(FGenObject& Docker);
	bool DockAction(FGenObject& Docker);
	// --- dockers (AIDockMachine) and gatherers (SupplyTruckAIUpdate) ---
	bool StartDock(FGenObject& DockObj);
	void StopDocking();
	void UpdateDocking(uint32 Frame);
	void UpdateSupplyTruck(uint32 Frame);
	bool StartRegroup();
	void SetBoxes(int32 NewBoxes);
	/** A player order takes a gatherer off autopilot (bDockOrder: the order was to dock somewhere). */
	void OnPlayerCommand(bool bDockOrder);

	TUniquePtr<FGenDock> Dock;
	FGenObjectID DockTarget = GenInvalidObjectID;
	EGenDockStage DockStage = EGenDockStage::None;
	int32 DockApproachIndex = -1;
	uint32 DockTimer = 0;              // frame the wait began / frame of the next dock action
	EGenSupplyState SupplyState = EGenSupplyState::Busy;
	int32 Boxes = 0;
	FGenObjectID PreferredDock = GenInvalidObjectID; // the dock the player sent us to; preferred over the nearest
	bool bForceWanting = false;        // go gathering at the next opportunity (just left a supply center)
	uint32 NextSupplyThinkFrame = 0;
	bool bSpawnPending = false;        // SpawnBehavior: the units this building comes with are still to appear

	// --- flight (GenFlight.cpp) ---
	void UpdateFlightPhysics(uint32 Frame);
	void MoveOther(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame);
	void MaintainPosition(uint32 Frame);
	void MaintainPositionWings(uint32 Frame);
	void HandleBehaviorZ(uint32 Frame);
	void UpdateBodyTilt(uint32 Frame);
	/** The locomotor in use moves through the air (an aircraft that is parked or taxiing uses a ground one). */
	bool IsUsingAirLoco() const;
	/** A plane: it cannot stop or turn on the spot, so it circles to wait and attacks in passes. */
	bool HasMinSpeed() const;
	/** Path::computeFlightDistToGoal: how far along the straight legs of the path the goal still lies ahead (0 once abeam of it). */
	float FlightDistToGoal();

	bool bFlies = false;               // an aircraft (its normal locomotor is air-only), wherever it is right now
	float BodyPitch = 0.f, BodyPitchRate = 0.f;
	float BodyRoll = 0.f, BodyRollRate = 0.f;
	bool bLocoPreciseZ = false;        // PRECISE_Z_POS: lift holds PreciseGoalZ instead of the preferred height
	bool bLocoUltraAccurate = false;   // ULTRA_ACCURATE (taxiing, takeoff, landing): more friction, lift may push down
	float PreciseGoalZ = 0.f;
	float LocoMaxLift = 99999.f;       // Locomotor::setMaxLift (a takeoff roll builds it up)
	bool bMaintainPosValid = false;    // MAINTAIN_POS_IS_VALID
	FVector3f MaintainPos = FVector3f::ZeroVector;
	FVector2f PathStart = FVector2f::ZeroVector; // where the current path began (flight legs are measured from it)

	// --- jets (GenJet.cpp; JetAIUpdate, JetSlowDeathBehavior) ---
	/** An order as JetAIUpdate remembers it: carried out after takeoff, or resumed after a trip home to reload. */
	struct FJetCommand
	{
		enum EType : uint8 { None, Stop, Move, Attack, Hunt, Guard, FollowPath, Land };
		EType Type = None;
		FVector3f Pos = FVector3f::ZeroVector;
		FGenObjectID Target = GenInvalidObjectID;
		float Radius = 0.f;
		const FGenPolygonTrigger* Area = nullptr;
		TArray<FVector2f> Points;
	};
	enum class EJetResult : uint8 { Continue, Success, Failure };

	void InitJet();
	/** JetAIUpdate::aiDoCommand: true if the order was taken care of here (stored for later, or turned into a takeoff / landing). */
	bool JetInterceptCommand(const FJetCommand& Cmd);
	void JetDoCommand(const FJetCommand& Cmd);
	void UpdateJet(uint32 Frame);
	void JetGetProducerLocation();
	void SetJetState(EGenJetState NewState, uint32 Frame);
	EJetResult JetEnterState(uint32 Frame);
	EJetResult JetUpdateState(uint32 Frame);
	void JetExitState();
	/** Follows JetPath; true once its end is reached. */
	bool JetFollowPath(uint32 Frame, float DesiredSpeed);
	void JetStartPath(bool bGround);
	/** Turns on the spot towards Angle; true when it faces that way. */
	bool JetFaceAngle(float Angle, float Tolerance);
	void ChooseLocomotorSet(const TCHAR* SetName);
	FGenParkingPlace* GetProducerParking(FGenObject** OutAirfield = nullptr) const;
	FGenObject* FindSuitableAirfield() const;
	bool IsParkedAt(FGenObjectID AirfieldId) const;
	void JetEnableAfterburners(bool bOn);
	/** A plane's attack: fly at the target, fire when it is ahead and in range, break away when too close. */
	void UpdateJetAttack(FGenObject& Target, int32 Slot, uint32 Frame);
	bool BeginJetDeath(const FString& DeathType, uint32 Frame);
	void UpdateJetDying(uint32 Frame);
	bool BeginHeliDeath(uint32 Frame);
	void UpdateHeliDying(uint32 Frame);

	TUniquePtr<FGenParkingPlace> Parking;
	bool bJetAI = false;
	EGenJetState JetState = EGenJetState::None;
	bool bJetAllowAirLoco = true;          // ALLOW_AIR_LOCO
	bool bJetPendingCommand = false;       // HAS_PENDING_COMMAND
	bool bJetTakeoffInProgress = false;
	bool bJetLandingInProgress = false;
	bool bJetTaxiInProgress = false;
	bool bJetAllowCircling = false;
	bool bJetUseReturnLoco = false;        // USE_SPECIAL_RETURN_LOCO
	bool bJetInterruptForReload = false;   // ALLOW_INTERRUPT_AND_RESUME_OF_CUR_STATE_FOR_RELOAD
	bool bJetHasProducerLocation = false;
	bool bJetDriving = false;              // this frame's movement came from the jet states or an attack pass, not the move order
	bool bJetBreakingOff = false;          // too close to the target: flying on to get room for another pass
	bool bJetAfterburners = false;
	FVector3f JetProducerLocation = FVector3f::ZeroVector;
	FVector3f JetHelipadLanding = FVector3f::ZeroVector;
	FJetCommand JetLastCommand;
	uint32 JetReturnToBaseFrame = 0;
	uint32 JetAttackLocoExpireFrame = 0;
	TArray<FVector3f> JetPath;             // the current state's route, exact in all three axes
	int32 JetPathIndex = 0;
	FVector3f JetPathFrom = FVector3f::ZeroVector;   // where the route began
	FVector3f JetLegFrom = FVector3f::ZeroVector;    // where the aircraft was when it turned for the current point
	bool bJetPathOnGround = false;
	// per-state scratch (JetPauseBeforeTakeoffState, JetOrHeliReloadAmmoState, JetOrHeliCirclingDeadAirfieldState ...)
	uint32 JetWhen = 0;
	uint32 JetWhenTransfer = 0;
	FGenObjectID JetWaitedForTaxi = GenInvalidObjectID;
	bool bJetResetTimer = false;
	uint32 JetReloadTime = 0;
	uint32 JetReloadDoneFrame = 0;
	int32 JetCheckAirfield = 0;
	float JetTakeoffLift = 0.f;
	float JetParkOrient = 0.f;
	// HelicopterSlowDeathBehavior
	bool bHeliDying = false;
	float HeliForwardAngle = 0.f;      // direction of travel round the downward spiral (not where the nose points)
	float HeliForwardSpeed = 0.f;
	float HeliSelfSpin = 0.f;
	bool bHeliSpinTowardsMax = true;
	uint32 HeliLastSpinFrame = 0;
	int32 HeliBladeFrames = 0;
	// JetSlowDeathBehavior
	bool bJetDying = false;
	uint32 JetDeathFrame = 0;
	uint32 JetGroundFrame = 0;
	float JetRollRate = 0.f;

	void UpdateProduction(uint32 Frame);
	void UpdateDozer(uint32 Frame);
	void UpdateDozerBored(uint32 Frame);
	void StopConstructing();

	bool bUnderConstruction = false;
	float ConstructionPercent = 100.f;
	uint32 LastBuiltFrame = 0;         // last frame a dozer worked on this building
	uint32 SellFrame = 0;              // being sold: removed and refunded at this frame
	TArray<FProduction> Production;
	FGenObjectID BuildTarget = GenInvalidObjectID;
	FGenObjectID RepairTarget = GenInvalidObjectID;
	uint32 DozerIdleSince = 0;         // DozerPrimaryIdleState: last frame the dozer had something to do
	FGenObjectID SoleHealer = GenInvalidObjectID;
	uint32 SoleHealerExpireFrame = 0;
	bool bHasRallyPoint = false;       // buildings: where finished units are sent
	FVector3f RallyPoint = FVector3f::ZeroVector;
	// --- containment ---
	void InitContain();
	void UpdateContained(uint32 Frame);
	void UpdateEnter(uint32 Frame);
	void StopEntering();
	void UpdateChinookFlight(uint32 Frame);
	TUniquePtr<FGenContain> Contain;
	FGenObjectID ContainedBy = GenInvalidObjectID;
	uint32 ContainedFrame = 0;
	FGenObjectID EnterTarget = GenInvalidObjectID;
	bool bWantsExit = false;
	bool bHidden = false;
	int32 GarrisonPoint = INDEX_NONE;  // the fire point this occupant shoots from
	EGenChinookFlight ChinookFlight = EGenChinookFlight::Flying;
	bool bHasExitRally = false;        // units: second leg of the way out of the factory (aiFollowExitProductionPath)
	FVector3f ExitRally = FVector3f::ZeroVector;
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
	/** Heads straight for InGoal, ignoring obstacles and without looking for a free spot (docking lanes run through buildings). */
	void StartDirectMove(const FVector3f& InGoal);
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
	friend class FGenSkirmishAI;
	friend class FGenParkingPlace;
};
