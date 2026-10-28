#pragma once

#include "CoreMinimal.h"
#include "Logic/GenConditions.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenShroud.h"

struct FGenThingTemplate;
struct FGenRadiusDecalTemplate;
struct FGenPolygonTrigger;
struct FGenLocomotorTemplate;
struct FGenWeaponTemplate;
struct FGenWeaponSetEntry;
struct FGenArmorTemplate;
struct FGenSlowDeath;
struct FGenUpgradeTemplate;
struct FGenUpgradeModule;
struct FGenDieModule;
struct FGenDamageFX;
struct FGenDeliverPayload;
enum class EGenAbilityKind : uint8;
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
	uint32 LastShotFrame = 0;
	uint32 PreAttackDoneFrame = 0;  // PRE_ATTACK: the shot comes when this frame is reached (0: none under way)
	TArray<int32, TInlineAllocator<12>> ScatterUnused; // ScatterTarget entries not yet used this clip (Weapon::m_scatterTargetsUnused)
	int32 CurBarrel = 0;            // which launch bone the next shot leaves from
	int32 ShotsForCurBarrel = 0;
};

/** Projectile flight state (DumbProjectileBehavior / MissileAIUpdate). */
/** MissileAIUpdate's states. */
enum class EGenMissileState : uint8 { Prelaunch, Launch, Ignition, AttackNoTurn, Attack, Kill, KillSelf, Dead };

struct FGenProjectileState
{
	bool bActive = false;
	bool bMissile = false;
	// A missile on a THRUST locomotor (GenMissile.cpp):
	bool bThrust = false;
	EGenMissileState State = EGenMissileState::Prelaunch;
	uint32 StateFrame = 0;
	bool bArmed = false;            // the engine has lit: it goes off on what it hits
	bool bTracking = false;         // fired at something it follows (otherwise at a spot)
	bool bBraking = false;          // locked: moved straight at the target from here
	bool bPreciseZ = false;         // near enough to dive: the preferred height no longer counts
	uint32 FuelExpires = MAX_uint32;
	float NoTurnDistLeft = 0.f;
	float MaxAccel = 99999.f;
	float MaxSpeed = 0.f;
	FVector3f PrevPos = FVector3f::ZeroVector;
	FVector3f OriginalTarget = FVector3f::ZeroVector;
	// The nuclear missile (GenNuke.cpp: NeutronMissileUpdate):
	bool bNeutron = false;
	bool bNeutronLaunched = false;
	bool bReachedIntermediate = true;
	FVector3f Intermediate = FVector3f::ZeroVector; // the spot over the target it makes for first
	float HeightAtLaunch = 0.f;
	uint32 FrameAtLaunch = 0;
	const FGenWeaponTemplate* Weapon = nullptr;
	FGenObjectID Launcher = GenInvalidObjectID;
	FGenObjectID Victim = GenInvalidObjectID;
	FGenObjectID Aimed = GenInvalidObjectID;  // what the shooter aimed at, when the shot was scattered to a spot near it
	FVector3f Start = FVector3f::ZeroVector;
	FVector3f End = FVector3f::ZeroVector;
	int32 Segments = 1;
	int32 Step = 0;
	uint32 DecoyFrame = 0;          // a missile fooled by countermeasures turns for a flare then
	bool bNoDamage = false;         // ... and does no harm where it goes off
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
	FGenObjectID Portable = GenInvalidObjectID; // HelixContain / OverlordContain: the add-on riding in the open (not one of the Riders)
	bool bPassengersFire = false;              // m_passengerAllowedToFire: the data's say, until a PassengersFireUpgrade
	uint32 LastLoadSoundFrame = 0;             // OpenContain::doLoadSound / doUnloadSound: not more than one a frame
	uint32 LastUnloadSoundFrame = 0;
	bool bRiderSwap = false;                   // RiderChangeContain's m_containing: a new rider is pushing the old one off
};

/** RailroadBehavior's TrainTrack: the chain of waypoints a train runs on, shared by all its cars. */
struct FGenTrainTrack
{
	struct FPoint
	{
		FVector3f Pos = FVector3f::ZeroVector;
		float DistFromFirst = 0.f;
		int32 Handle = -1;              // the waypoint the stretch from here starts at (TrackPoint::m_handle)
		bool bTunnel = false;           // a waypoint named ...Tunnel: the train keeps its height (tunnels, bridges)
		bool bStation = false;          // ...Station: it stops here a while
		bool bDisembark = false;        // ...Disembark: it stops and lets its passengers out
		bool bPingPong = false;         // ...PingPong: it stops and goes back the way it came
	};
	TArray<FPoint> Points;
	float Length = 0.f;
	bool bLooping = false;
};

/** RailroadBehavior: one car's part in a train (the locomotive's, or a car's that it pulls). */
struct FGenTrain
{
	struct FPull
	{
		float Direction = 1.f;           // 1 forward, -1 backward
		float Speed = 0.f;               // along the track, per 30 Hz frame (the data's)
		float TrackDistance = 0.f;
		FVector3f TowHitch = FVector3f::ZeroVector;
		int32 MostRecentSpecial = -1;
	};
	enum class EConductor : uint8 { ApplyBrakes, WaitAtStation, Accelerate, Coast };
	TSharedPtr<FGenTrainTrack> Track;
	FPull Conductor;                     // conductorPullInfo: what pulls it (its own, the lead car's)
	FPull Mine;                          // m_pullInfo: what it hands on to the car behind
	EConductor State = EConductor::Coast;
	FGenObjectID Trailer = GenInvalidObjectID;
	bool bLocomotive = false;
	bool bLead = false;
	int32 WantsToBeLead = 0;
	bool bTrackLoaded = false;
	bool bWaitingInWings = true;         // not on the track yet (an open track: still behind its start)
	bool bEndOfLine = false;             // run off its end
	bool bInTunnel = false;
	bool bDisembark = false;
	bool bEverHitched = false;
	bool bWall = false;                  // standing at a station: an obstacle to paths
	bool bPlaced = false;
	int32 WaitTimer = 0;
	int32 CurrentPointHandle = -1;
	uint32 WhistleFrame = 0;
};

/** SlavedUpdate: a drone's ties to its master. */
struct FGenSlaveState
{
	FGenObjectID Slaver = GenInvalidObjectID;
	FVector3f GuardOffset = FVector3f::ZeroVector; // where it keeps itself, from the master (or from the spot it was sent to)
	int32 FramesToWait = 0;
	uint8 RepairState = 0;                         // RepairStates
	bool bRepairing = false;                       // the first sparks have flown: the master is being mended
	bool bFastLoco = false;                        // on its SET_PANIC locomotor, zipping between welds
	uint32 CrashFrame = 0;                         // its master gone, it falls: dead by then at the latest
};

/** ChinookFlightStatus */
enum class EGenChinookFlight : uint8 { Flying, Landing, Landed, TakingOff, CombatDrop };

/** ChinookCombatDropState: a helicopter on its way to, or hanging over, the spot its soldiers slide down to. */
struct FGenCombatDrop
{
	/** RopeInfo */
	struct FRope
	{
		FVector3f Top = FVector3f::ZeroVector;        // the RopeStart bone
		FVector3f DropStart = FVector3f::ZeroVector;  // the RopeEnd bone: where a soldier takes hold
		FQuat4f DropRotation = FQuat4f::Identity;     // ... and how it hangs there (the bone's own attitude)
		float Speed = 0.f;
		float Len = 1.f;
		float LenMax = 1.f;
		uint32 NextDropFrame = 0;
		TArray<FGenObjectID> Rappellers;              // on their way down this rope
	};
	FGenObjectID Target = GenInvalidObjectID;         // the building to drop into, if any
	FVector3f Pos = FVector3f::ZeroVector;
	float DestZ = 0.f;                                // the height to hover at
	bool bDropping = false;                           // over the spot: the ropes are out
	float WobblePhase = 0.f;
	TArray<FRope> Ropes;
};

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
	/** Whether one of this kind gets a door when it is queued at the airfield (the helipad craft never do). */
	bool ShouldReserveDoorWhenQueued(const FGenThingTemplate& Thing) const;
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
	/** Object::getLayer: what the object stands on - 0 the ground, n the deck of the n-th bridge. */
	int32 GetLayer() const { return Layer; }
	/** The height of that at the object's spot (TerrainLogic::getLayerHeight). */
	float GetSurfaceHeight() const;
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

	/**
	 * What the screen blends between: where the object was at the last two logic frames the logic has
	 * published (UGenGameLogic::PublishFrame) - not where the step under way has got it to. Prev: the
	 * earlier of the two, Draw: the later.
	 */
	const FVector3f& GetPrevPosition() const { return PrevPosition; }
	float GetPrevOrientation() const { return PrevOrientation; }
	const FQuat4f& GetPrevRotation() const { return PrevRotation; }
	const FVector3f& GetDrawPosition() const { return DrawPosition; }
	float GetDrawOrientation() const { return DrawOrientation; }
	const FQuat4f& GetDrawRotation() const { return DrawRotation; }
	/** What the screen adds to a ground vehicle's place and heading: the body's pitch and roll and a lift (PhysicsXformInfo). */
	struct FChassisPose
	{
		float Pitch = 0.f;
		float Roll = 0.f;
		float Z = 0.f;
		bool operator==(const FChassisPose& O) const { return Pitch == O.Pitch && Roll == O.Roll && Z == O.Z; }
	};
	/** TWheelInfo: the wheels' springs (height offsets) and the front wheels' steering. */
	struct FWheelInfo
	{
		float FrontLeft = 0.f, FrontRight = 0.f, RearLeft = 0.f, RearRight = 0.f;
		float Angle = 0.f;               // 0 straight, > 0 left
		int32 FramesAirborneCounter = 0;
		int32 FramesAirborne = 0;        // how long it was in the air before its last landing
		uint32 LandingSerial = 0;        // counts the landings (the screen reacts to each once)
	};
	/** A ground vehicle whose body the screen tilts and rocks (TREADS, FOUR_WHEELS, MOTORCYCLE locomotors). */
	bool HasChassis() const;
	const FChassisPose& GetPrevChassis() const { return PrevChassis; }
	const FChassisPose& GetDrawChassis() const { return DrawChassis; }
	const FWheelInfo& GetDrawWheels() const { return DrawWheels; }
	/** PhysicsBehavior::getTurning: which way the locomotor last turned it (-1, 0, 1). */
	int32 GetTurning() const { return Turning; }
	/** PhysicsBehavior::isMotive: its locomotor has pushed it lately. */
	bool IsMotive() const;
	/** Thing::isSignificantlyAboveTerrain: "more than 3 frames to return to the ground". */
	bool IsSignificantlyAboveTerrain() const;
	/** Locomotor::isMovingBackwards */
	bool IsMovingBackwards() const { return bLocoBackwards; }
	const FGenLocomotorTemplate* GetLocomotorTemplate() const { return Locomotor; }
	/** Path::peekCachedPointOnPath: the next point of the path it is driving along. */
	bool PeekNextPathPoint(FVector3f& Out) const;
	/** PhysicsBehavior::getAcceleration: the last step's acceleration. */
	const FVector3f& GetPrevAccel() const { return PrevAccel; }

	/** Put somewhere in one go (made, set down out of a transport): the screen blends on from here, not from where it was. */
	void SnapshotPrevious()
	{
		SnapPosition = Position;
		SnapOrientation = Orientation;
		SnapRotation = Rotation;
		bDrawSnap = true;
		if (!bDrawPublished)
		{
			// (Not shown yet: until its first logic frame is published it is drawn where it was made.)
			PrevPosition = DrawPosition = Position;
			PrevOrientation = DrawOrientation = Orientation;
			PrevRotation = DrawRotation = Rotation;
		}
	}
	/** The logic frame is finished: the pair moves on. */
	void PublishDraw()
	{
		PrevPosition = bDrawSnap ? SnapPosition : DrawPosition;
		PrevOrientation = bDrawSnap ? SnapOrientation : DrawOrientation;
		PrevRotation = bDrawSnap ? SnapRotation : DrawRotation;
		DrawPosition = Position;
		DrawOrientation = Orientation;
		DrawRotation = Rotation;
		for (int32 t = 0; t < 2; ++t)
		{
			PrevTurretAngle[t] = bDrawPublished ? DrawTurretAngle[t] : TurretAngle[t];
			DrawTurretAngle[t] = TurretAngle[t];
			PrevTurretPitch[t] = bDrawPublished ? DrawTurretPitch[t] : TurretPitch[t];
			DrawTurretPitch[t] = TurretPitch[t];
		}
		PrevChassis = bDrawPublished ? DrawChassis : Chassis;
		DrawChassis = Chassis;
		DrawWheels = Wheels;
		bDrawSnap = false;
		bDrawPublished = true;
	}

	/**
	 * AIUpdateInterface::isWaitingForPath: the unit has somewhere to go and its path hasn't been worked
	 * out yet, because the frame's pathfinding was spent when it asked (UGenGameLogic::ShouldQueuePath).
	 * It carries on along the way it was on, or waits where it stands; the path comes a frame or
	 * two later (DoPendingPath, from the logic's queue).
	 */
	struct FPendingPath
	{
		enum EKind : uint8 { None, Plain, Approach, ApproachPos, FreeGround, MakeWay };
		EKind Kind = None;
		FVector3f Goal = FVector3f::ZeroVector;
		bool bAroundUnits = false;
		bool bIntoUnit = false;
		bool bWaypoint = false;
		FGenObjectID Target = GenInvalidObjectID;   // Approach: whom, and to within what range (ApproachPos: the spot is Goal; MakeWay: for whom)
		float Range = 0.f;
		float MinRange = 0.f;                       // ... and no nearer than this
		FVector2f Along = FVector2f::ZeroVector;    // MakeWay: the way the other unit goes
		bool bResume = false;                       // ... and whether we go on to ResumeGoal afterwards
		FVector3f ResumeGoal = FVector3f::ZeroVector;
	};
	bool IsWaitingForPath() const { return PendingPath.Kind != FPendingPath::None; }
	/** Its turn in the queue. False: there was nothing (left) to work out. */
	bool DoPendingPath();

	/** A living, locomotor-driven unit on the ground: takes part in unit-vs-unit collision. */
	bool IsGroundUnit() const
	{
		return CanMove() && !bFlies && !bDead && !bPendingDestroy && !Projectile.bActive && !bHeldByParachute && ContainedBy == GenInvalidObjectID
			&& !bNoCollide;
	}
	/** Moves through the air only (helicopters, planes): flies straight to its goal above everything. */
	bool IsFlying() const { return bFlies; }
	/** OBJECT_STATUS_AIRBORNE_TARGET: high enough above the terrain that only anti-air weapons reach it. */
	bool IsAirborneTarget() const;
	/** Radius for unit-vs-unit overlap tests (boxes: between their half-width and half-length). */
	float GetCollisionRadius() const;
	uint32 GetBlockedFrames() const { return BlockedFrames; }

	/** The drawable has to follow this object every frame (it moves, tumbles, flies or sinks). */
	bool IsDynamic() const { return CanMove() || bDead || Projectile.bActive || bFreePhysics || ToppleState == EToppleState::Falling || TreeSinkFramesLeft > 0 || Train.IsValid(); }
	/** ToppleUpdate::applyTopplingForce: pushed over that way (a 2D direction) at that speed. */
	void ApplyTopplingForce(const FVector2f& Direction, float Speed, uint32 Options);
	/** ToppleUpdate::isAbleToBeToppled */
	bool IsAbleToBeToppled() const;

	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }
	/** BodyDamageType: 0 pristine, 1 damaged, 2 really damaged, 3 rubble (no health left). */
	int32 GetDamageState() const;

	/** Has an AI module and a locomotor: can receive move orders. */
	bool CanMove() const { return Locomotor != nullptr && bHasAI && !bImmobile; }
	bool IsMoving() const { return bMoving; }
	/** AIUpdateInterface::getLocomotorDistanceToGoal: how far along its path it still has to go (0: not moving). */
	float GetDistanceToGoal() const { return bMoving ? PathDistanceFrom(PathIndex) : 0.f; }
	bool IsSelectable() const;
	/** The template whose geometry its pathfinding footprint has (a GLA hole: its building's). */
	const FGenThingTemplate* GetFootprintTemplate() const { return FootprintTemplate ? FootprintTemplate : Template; }
	float GetDrawManualFrame() const { return DrawManualFrame; }
	uint32 GetDrawManualFrameVersion() const { return DrawManualFrameVersion; }
	/** RiderChangeContain: the STATUS_RIDERn of whoever rides it (empty: nobody, or not a combat cycle). */
	const FString& GetRiderStatus() const { return RiderStatus; }

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
	/** Attack an object (explicit order). bForce: a force attack (Ctrl) - whoever it belongs to, and kept at
	 *  for as long as it is there (ATTACK_CONTINUED_TARGET_FORCED). */
	void CommandAttack(FGenObjectID Target, bool bForce = false);
	bool IsForceAttacking() const { return bForceAttack; }
	/** AIUpdateInterface::aiAttackPosition with the weapon of that slot locked: so many shots at the spot (the SCUD storm). */
	void CommandAttackPosition(const FVector3f& Pos, int32 MaxShots, int32 Slot);
	/** A player's attack on a spot: it goes within range, aims and fires (AIAttackState on a position). Slot
	 *  INDEX_NONE is a force attack on the ground - the weapon it holds, on and on; a slot is a button's
	 *  weapon (MSG_DO_WEAPON_AT_LOCATION), locked till its clip is empty or MaxShots are fired. */
	void CommandFireAtPosition(const FVector3f& Pos, int32 MaxShots, int32 Slot);
	/** The particle cannon's power fired at a spot (false: this is no particle cannon with that power). */
	bool BeginParticleCannon(const FString& PowerName, const FVector3f& TargetPos, uint32 Frame);
	/** ... and, while its beam is burning, sent somewhere else (false: there is no beam to send). */
	bool SteerParticleCannon(const FString& PowerName, const FVector3f& TargetPos, uint32 Frame);
	/** doesSpecialPowerHaveOverridableDestinationActive: its beam is burning and can be sent elsewhere. */
	bool IsParticleCannonSteerable() const;
	/** Its beams as they are this frame: the lasers of its nodes, the beam going up and the beam coming down. */
	void GetParticleCannonBeams(uint32 Frame, TArray<struct FGenBeam>& Out) const;
	/** SpectreGunshipUpdate::initiateIntentToDoSpecialPower: from where it was made, in to circle that spot. */
	void BeginGunshipRun(const FVector3f& Target, uint32 Frame);
	/** ... setSpecialPowerOverridableDestination: where its guns point from now on (kept inside its attack area). */
	bool SteerGunship(const FVector3f& Pos);
	/** A gunship on its run, which can be told where to fire. */
	bool IsGunshipSteerable() const;
	/** The area it attacks and the spot its guns are on (false: it is not on a run). */
	bool GetGunshipTargets(FVector3f& OutArea, float& OutAreaRadius, FVector3f& OutReticle, float& OutReticleRadius) const;
	/** MissileLauncherBuildingUpdate::initiateIntentToDoSpecialPower: the silo's power has been fired. */
	void OnLauncherPowerFired(const FString& PowerName, uint32 Frame);
	bool IsAttackingPosition() const { return bAttackingPosition; }
	/** Shots of the slot's clip that are gone (W3DModelDraw::updateProjectileClipStatus hides as many launch bones' sub-objects). */
	int32 GetClipSpent(int32 Slot) const;
	FGenObjectID GetTargetID() const { return TargetID; }
	/** Body::attemptDamage: armor, health, damage state, death. */
	/** bInFlight: a missile in the air may be hit too (a point defense laser shooting it down). */
	/** DamageFXType: what is shown for it instead of DamageType's effects (m_damageFXOverride - a dose of poison is unresistable damage). */
	void AttemptDamage(float Amount, const FString& DamageType, const FString& DeathType, FGenObjectID Source, bool bInFlight = false, const TCHAR* DamageFXType = nullptr);
	/** Body::attemptHealing: health back, up to the maximum; the damage state follows. */
	void AttemptHealing(float Amount);
	/** Turret angle relative to the body (radians) for turret slot 0/1, and previous frame value. */
	float GetTurretAngle(int32 Turret) const { return TurretAngle[Turret]; }
	float GetPrevTurretAngle(int32 Turret) const { return PrevTurretAngle[Turret]; }
	float GetDrawTurretAngle(int32 Turret) const { return DrawTurretAngle[Turret]; }
	float GetPrevTurretPitch(int32 Turret) const { return PrevTurretPitch[Turret]; }
	float GetDrawTurretPitch(int32 Turret) const { return DrawTurretPitch[Turret]; }
	/** W3DDebrisDraw: a model given to an object whose template has none (a chunk of wreckage). */
	const FString& GetDebrisModel() const { return DebrisModel; }
	// Object::updateObjValuesFromMapProperties: what the map says about its ambient sound.
	bool bFromMap = false;                // placed by the map (its one-shot ambient sounds start switched off)
	bool bMapAmbientOverride = false;     // "objectSoundAmbient": this sound instead of the template's (empty: none)
	FString MapAmbientSound;
	int8 MapAmbientEnabled = -1;          // "objectSoundAmbientEnabled": 0 off, 1 on, -1 not said
	/** W3DDebrisDraw::setAnimNames: what the chunk plays first (0), in flight (1) and once it has come down (2), and the effect of landing. */
	const FString& GetDebrisAnim(int32 Which) const { return DebrisAnims[Which]; }
	const FString& GetDebrisFXFinal() const { return DebrisFXFinal; }
	bool HasDebrisAnims() const { return !DebrisAnims[0].IsEmpty() || !DebrisAnims[1].IsEmpty() || !DebrisAnims[2].IsEmpty(); }
	bool IsDebrisHouseColored() const { return bDebrisHouseColor; }
	const FGenWeaponInstance& GetWeapon(int32 Slot) const { return Weapons[Slot]; }
	void LaunchProjectile(const FGenWeaponTemplate* Weapon, FGenObjectID Launcher, FGenObjectID Victim, const FVector3f& TargetPos, const FQuat4f* LaunchRotation = nullptr);
	float GetDeathSink() const { return SinkOffset; }
	/** Object::kill: dies regardless of armor or health (lifetime expiry, debris coming to rest). */
	void Kill(const FString& DeathType = TEXT("NORMAL"), FGenObjectID Killer = GenInvalidObjectID);
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
		int32 QuantityRemaining = -1; // how many of it have still to come out, once it is finished (-1: not finished yet)
	};
	/** OBJECT_STATUS_UNDER_CONSTRUCTION: placed, but a dozer still has to finish it. */
	bool IsUnderConstruction() const { return bUnderConstruction; }
	/** A state a die module can be exempt from (the data's ExemptStatus names these). */
	bool IsHijacked() const { return bHijacked; }
	float GetConstructionPercent() const { return ConstructionPercent; }
	bool IsBeingSold() const { return SellFrame != 0; }
	/** The construction percent as the drawing reads it (W3DModelDraw::adjustTransformMtx): below zero unless the
	 *  building is going up - or coming down, sold. */
	float GetDrawConstructionPercent() const { return (bUnderConstruction || SellFrame != 0) ? ConstructionPercent : -1.f; }
	/** Drawable::setAnimationLoopDuration, asked for by the logic: whatever the object's models are playing is to take that many frames. */
	uint32 GetDrawAnimDurationVersion() const { return DrawAnimDurationVersion; }
	int32 GetDrawAnimDurationFrames() const { return DrawAnimDurationFrames; }
	/** Drawable::getShouldAnimate: nothing moves on what is shut down; on a building short of power only where the module says so. */
	bool ShouldAnimate(bool bConsiderPower) const;
	/** DISABLED_UNDERPOWERED: a thing of the POWERED kind whose owner hasn't the power for it. */
	bool IsUnderpowered() const { return bUnderpowered; }
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
	void SetOwner(int32 NewOwner);
	EGenChinookFlight GetChinookFlight() const { return ChinookFlight; }
	/** The combat drop this helicopter is flying, if any. */
	const FGenCombatDrop* GetCombatDrop() const { return CombatDrop.Get(); }
	/** AIRappelState: on its way down a rope. */
	bool IsRappelling() const { return bRappelling; }
	/** The weapon's range for this object (its bonuses included: a garrisoned soldier sees further). */
	float GetAttackRange(const FGenWeaponTemplate& Weapon) const;

	// --- fog of war and radar (GenShroud.cpp) ---
	/** How far it clears the fog of war around itself (a construction site: no further than its own walls). */
	float GetShroudClearingRange() const;
	/** What the local player sees of it (refreshed at the end of every logic frame). */
	EGenObjectShroud GetLocalShroud() const { return LocalShroud; }
	/** RadarUpgrade has arrived: its owner has a radar while this stands. */
	bool GivesRadar() const { return bRadarUpgraded; }

	// --- stealth (GenStealth.cpp) ---
	/** OBJECT_STATUS_STEALTHED: its enemies can't see or target it, unless it is detected. */
	bool IsStealthed() const { return bStealthed; }
	/** OBJECT_STATUS_DETECTED: a detector nearby is showing it up. */
	bool IsDetected() const { return bDetected; }
	bool IsStealthedAndUndetected() const { return bStealthed && !bDetected && !bDead; }
	/** OBJECT_STATUS_DISGUISED: a bomb truck that looks like another vehicle. */
	bool IsDisguised() const { return DisguiseTemplate != nullptr; }
	/** Can be picked as a target by its enemies: targetable, and not hidden by stealth. */
	bool IsAttackable() const { return IsTargetable() && !IsStealthedAndUndetected(); }
	/** StealthUpdate::markAsDetected: shown up for that many frames (0: for as long as its stealth delay). */
	void MarkAsDetected(uint32 NumFrames);
	/** StealthUpdate::receiveGrant: stealth from a power (the GPS scrambler). */
	void ReceiveStealthGrant();

	// --- flares, shots that miss, pilots (GenCountermeasures.cpp) ---
	/** CountermeasuresBehavior, once its upgrade is there: the aircraft carries flares. */
	bool HasCountermeasures() const { return bCountermeasuresActive; }
	/** JetAIUpdate::getSneakyTargetingOffset: while a plane like the Aurora attacks, whoever shoots at it aims beside it. */
	bool GetSneakyTargetingOffset(FVector3f* OutOffset) const;
	/** VeterancyCrateCollide::isValidToExecute for a pilot: he can climb into that vehicle and give it his rank. */
	bool CanPilotEnter(const FGenObject& Vehicle) const;
	/** ParachuteContain: this parachute carries that rider down - to that spot, or (null) to below where it opens. */
	void CarryUnderParachute(FGenObject& Rider, const FVector3f* LandAt, const FVector3f& StartVelocity);

	/** OBJECT_STATUS_AFLAME: burning right now. */
	bool IsAflame() const { return FlameStatus == EFlameStatus::Aflame; }

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
	/** OCLUpdate::getRemainingFrames / getCountdownPercent: the supply drop zone's countdown (0..1 done). */
	uint32 GetOCLRemainingFrames(uint32 Now) const { return OCLNextFrame > Now ? OCLNextFrame - Now : 0; }
	float GetOCLCountdownPercent(uint32 Now) const
	{
		return OCLNextFrame > OCLStartFrame ? FMath::Clamp(1.f - float(GetOCLRemainingFrames(Now)) / float(OCLNextFrame - OCLStartFrame), 0.f, 1.f) : 0.f;
	}
	/** (Test harness) what keeps it from IsAIIdle, or "idle". */
	FString DescribeBusy() const;

	// --- standing orders (GenObjectAI.cpp) ---
	EGenAIMode GetAIMode() const { return AIMode; }
	void CommandHunt();
	/** Guards a spot; Radius is how far out enemies are engaged (0 = this unit's own guard range), Area limits it to a map area. */
	void CommandGuard(const FVector2f& Pos, float Radius, const FGenPolygonTrigger* Area = nullptr);
	void CommandFollowPath(const TArray<FVector2f>& Points);
	/** AIStateMachine::addToGoalPath: already on its way along a route - one more point at its end (false: it isn't). */
	bool AppendToRoute(const FVector2f& Point);
	/** The route it is following (AIFollowPathState's goal path) and the next point of it; none: an empty list. */
	const TArray<FVector2f>& GetRoutePoints() const { return RoutePoints; }
	int32 GetRouteIndex() const { return AIMode == EGenAIMode::FollowPath ? RouteIndex : RoutePoints.Num(); }

	// --- groups (Object::m_formationID / m_formationOffset; AIGroup::getSpeed) ---
	uint32 GetFormationID() const { return FormationID; }
	/** AIAttackMoveToState: go there, fighting whatever comes into view on the way. */
	void CommandAttackMove(const FVector3f& Goal);
	/** A player's guard order (GuardInnerModifierHuman / GuardOuterModifierHuman): hold that spot, or stay with that object. */
	void CommandGuardPosition(const FVector2f& Pos, EGenGuardMode Mode, FGenObjectID GuardObject = GenInvalidObjectID);

	// --- veterancy (GenVeterancy.cpp) ---
	/** 0 regular, 1 veteran, 2 elite, 3 heroic. */
	int32 GetVeterancyLevel() const { return VeterancyLevel; }
	int32 GetExperience() const { return Experience; }
	/** ExperienceTracker::addExperiencePoints */
	void AddExperience(int32 Points, bool bCanScaleForBonus = true);
	/** PoisonedBehavior: still taking poison damage. */
	bool IsPoisoned() const { return PoisonStopFrame != 0; }
	/** HordeUpdate: in a horde (its HORDE bonus). */
	bool IsInHorde() const { return bInHorde; }
	void SetVeterancyLevel(int32 NewLevel, bool bProvideFeedback = true);
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
	/** OverchargeBehavior: on, it adds the template's EnergyBonus and eats into the reactor's health. */
	bool IsOverchargeActive() const { return bOverchargeActive; }
	bool CanOvercharge() const;
	void SetOvercharge(bool bOn);
	/** BattlePlanUpdate::initiateIntentToDoSpecialPower: the plan the strategy center is to unpack next
	 *  (1 bombardment, 2 hold the line, 3 search and destroy). */
	void RequestBattlePlan(int32 Plan);
	/** BattlePlanUpdate::getCommandOption: the plan asked for last (its button is lit). */
	int32 GetDesiredBattlePlan() const { return BattlePlanState.Desired; }
	/** BattlePlanUpdate::getActiveBattlePlan: the plan in force, or 0 while none is unpacked. */
	int32 GetActiveBattlePlan() const;
	/** onDelete / onCapture: the plan stops working for the owner, and the building is packed up again. */
	void StopBattlePlan();
	/** Object::getVisionRange: how far it looks for enemies (a battle plan may stretch it). */
	float GetVisionRange() const;
	/** SpawnBehavior: the units it keeps (the stinger site's soldiers). */
	const TArray<FGenObjectID>& GetSpawns() const { return Spawns; }
	/** CleanupHazardUpdate::setCleanupAreaParameters: off to clean up round there until it is clean. */
	void SetCleanupAreaParameters(const FVector3f& Pos, float Range);
	/** The angry mob's nexus a member belongs to (MobMemberSlavedUpdate::getSlaverID). */
	FGenObjectID GetMobSlaver() const { return MobSlaver; }
	/** Object::getHealthBoxPosition's m_healthBoxOffset (the nexus: the middle of its mob). */
	const FVector3f& GetHealthBoxOffset() const { return HealthBoxOffset; }
	/** SpawnBehavior::getCanAnySlavesAttackSpecificTarget (SPAWNS_ARE_THE_WEAPONS: its soldiers do its fighting). */
	bool SpawnsCanAttack(const FGenObject& Target) const;
	/** Factor a weapon gains from this object's bonus conditions (WeaponBonus lines; 1 = none). */
	float GetWeaponBonus(const FGenWeaponTemplate& Weapon, int32 Field) const;
	/** Sub-objects an upgrade shows or hides on the model, applied on top of the condition state. */
	const TArray<TPair<FString, bool>>& GetSubObjectOverrides() const { return SubObjectOverrides; }
	uint32 GetSubObjectVersion() const { return SubObjectVersion; }

	// --- special powers and payload deliveries (GenSpecialPowers.cpp) ---
	/** Frame the object's own (not player-shared) power in that module slot is ready again. */
	uint32 GetPowerReadyFrame(int32 ModuleIndex) const { return PowerReady.IsValidIndex(ModuleIndex) ? PowerReady[ModuleIndex] : 0; }
	bool IsPowerPaused(int32 ModuleIndex) const { return PowerPaused.IsValidIndex(ModuleIndex) && PowerPaused[ModuleIndex]; }
	bool IsUpgradeModuleDone(int32 ModuleIndex) const { return UpgradeModuleDone.IsValidIndex(ModuleIndex) && UpgradeModuleDone[ModuleIndex]; }

	// --- special abilities (SpecialAbilityUpdate; GenAbilities.cpp) ---
	/** Index of the SpecialAbilityUpdate that carries out the power, or INDEX_NONE. */
	int32 FindAbility(const FString& Power) const;
	/** The unit sets about the ability: walks up to the target, unpacks, prepares, does it, packs. */
	bool StartAbility(int32 InAbility, FGenObjectID Target, const FVector3f* TargetPos);
	void ExitAbility(bool bCleanup);
	bool IsAbilityActive() const { return AbilityIndex != INDEX_NONE; }
	/** OBJECT_STATUS_IS_USING_ABILITY: in the middle of it (does not look for enemies meanwhile). */
	bool IsUsingAbility() const { return bUsingAbility; }
	FGenObjectID GetAbilityTarget() const { return AbilityIndex != INDEX_NONE ? AbilityTarget : GenInvalidObjectID; }
	/** CommandButtonHuntUpdate::setCommandButton: use that button's ability on whatever is nearest, again and again. */
	void SetHuntButton(const FString& ButtonName);
	/** AutoDepositUpdate::awardInitialCaptureBonus */
	void AwardCaptureBonus();
	/** Drawable::flashAsSelected: lights up for a moment. */
	void FlashAs(const FLinearColor& Color) { FlashColor = Color; FlashFrame = Logic ? GetFlashNow() : 0; }
	uint32 GetFlashFrame() const { return FlashFrame; }
	/** Flying a delivery for a special power (DeliverPayloadAIUpdate). */
	bool IsDelivering() const { return Delivery.IsValid(); }
	/** Knocked out for the time being (an EMP, or somebody hacking it). */
	bool IsDisabled() const;
	void DisableUntil(uint32 Frame) { DisabledUntilFrame = FMath::Max(DisabledUntilFrame, Frame); }
	/** DISABLED_EMP: as DisableUntil, and with the sound of a building or vehicle losing its power. */
	void EMPUntil(uint32 Frame) { DisableUntil(Frame); EMPUntilFrame = FMath::Max(EMPUntilFrame, Frame); }
	/** OBJECT_STATUS_IS_CARBOMB: a terrorist is driving it. */
	bool IsCarBomb() const { return bCarBomb; }

	// --- hackers, charges and the other unit abilities (GenHackers.cpp, GenAbilities.cpp) ---
	/** HackInternetAIUpdate::hackInternet: unpack on the spot and make money until told otherwise. */
	void CommandHackInternet();
	bool IsHackingInternet() const { return HackState != EHackState::None; }
	/** DISABLED_HACKED: dead for the time being because somebody is hacking it. */
	bool IsHacked() const;
	void HackUntil(uint32 Frame) { HackedUntilFrame = FMath::Max(HackedUntilFrame, Frame); }
	/** OBJECT_STATUS_BOOBY_TRAPPED */
	bool IsBoobyTrapped() const { return bBoobyTrapped; }
	/** Object::checkAndDetonateBoobyTrap: the trap on this goes off under whoever touches it (unless a friend of the trapper's). */
	bool CheckAndDetonateBoobyTrap(const FGenObject* Victim);
	/** StickyBombUpdate::initStickyBomb: the charge is on that target from now on. */
	void InitStickyBomb(FGenObject* Target, const FGenObject* Bomber);
	/** StickyBombUpdate::detonate */
	void DetonateStickyBomb();
	FGenObjectID GetStickyTarget() const { return StickyTarget; }
	/** How many charges (or whatever it makes) this ability has out there. */
	int32 GetAbilityObjectCount(int32 InAbility) const;
	/** SpecialAbilityUpdate::findSpecialObjectWithProducerID: one of them sits on that target. */
	bool HasAbilityObjectOn(int32 InAbility, FGenObjectID Target) const;
	/** The beam an ability holds on its target right now (a laser pointer, a stream of numbers): what to draw, to whom, from which bone. */
	const FGenThingTemplate* GetAbilityBeam(FGenObjectID& OutTarget, FString& OutBone) const;
	/** WeaponSet::setWeaponLock: the slot an ability or a button has locked in, or INDEX_NONE. */
	int32 GetWeaponLock() const { return WeaponLockSlot != INDEX_NONE ? WeaponLockSlot : PermanentLockSlot; }
	/** WeaponSet::getCurrentWeapon's slot, as the switch buttons show it: the locked one, else the primary. */
	int32 GetCurrentWeaponSlot() const { return GetWeaponLock() != INDEX_NONE ? GetWeaponLock() : 0; }
	/** WeaponSet::getAbleToAttackSpecificObject, as far as weapons go: one of them may be used on that. */
	bool HasWeaponFor(const FGenObject& Target) const { return ChooseWeapon(Target) != INDEX_NONE; }
	/** GUI_COMMAND_SWITCH_WEAPON: setWeaponLock(slot, LOCKED_PERMANENTLY) - "switches until un-switched, or
	 *  switched to something else". False when it has nothing in that slot. */
	bool SwitchWeapon(int32 Slot);
	/** A FIRE_WEAPON button: attack that with the weapon of this slot. */
	void CommandAttackWithSlot(FGenObjectID Target, int32 Slot);
	/**
	 * PartitionFilterRejectBuildings: "never target buildings (unless they can attack)" - what a unit goes
	 * for by itself. The computer's units take any enemy building; a player's only base defences and
	 * buildings somebody shoots out of.
	 */
	bool IgnoresAsAutoTarget(const FGenObject& Other) const;

	// --- jets and airfields (GenJet.cpp) ---
	/** Parking spaces and runways of an airfield; null for everything else. */
	FGenParkingPlace* GetParking() const { return Parking.Get(); }
	EGenJetState GetJetState() const { return JetState; }
	/** On the ground at its airfield (or anywhere, for a helicopter that has landed): may not fly until it takes off. */
	bool IsParked() const { return bJetAI && !bJetAllowAirLoco; }
	/** JetAIUpdate's m_enginesOn: a plane standing at its airfield makes no engine noise (a helicopter always does). */
	bool AreEnginesOn() const;
	/** Every weapon that only reloads at an airfield is empty. */
	bool IsOutOfSpecialReloadAmmo() const;
	/** The weapon in this slot has a clip that is empty and doesn't refill by itself. */
	bool IsWeaponOutOfAmmo(int32 Slot) const;
	/** JetAIUpdate::doLandingCommand: fly to this airfield and park there (it becomes home). */
	void CommandLandAt(FGenObjectID AirfieldId);
	/** ChinookAIUpdate::privateGetRepaired: down on open ground beside that airfield, mended there until whole. */
	void CommandChinookGetRepaired(const FGenObject& Airfield);
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
	FString RiderStatus;                   // RiderChangeContain: STATUS_RIDERn of the rider aboard
	float XPScalar = 1.f;                  // ExperienceTracker's m_experienceScalar (ExperienceScalarUpgrade adds to it)
	// PoisonedBehavior
	uint32 PoisonDamageFrame = 0;          // the next dose
	uint32 PoisonStopFrame = 0;            // poisoned until then (0: not)
	float PoisonAmount = 0.f;              // what each dose does (what the poison did when it hit)
	FString PoisonDeathType;
	void StartPoison(float Amount, const FString& DeathType);
	void UpdatePoison(uint32 Frame);
	// BaseRegenerateUpdate
	uint32 BaseRegenFrame = 0;             // when it next looks at its health (MAX_uint32: whole, asleep until hurt)
	void UpdateBaseRegenerate(uint32 Frame);
	// FireOCLAfterWeaponCooldownUpdate: per module, the run of shots under way
	struct FCooldownOCLState
	{
		int32 Shots = 0;
		uint32 StartFrame = 0;
		bool bValid = false;
	};
	TArray<FCooldownOCLState> CooldownOCL;
	void UpdateFireOCLAfterCooldown(uint32 Frame);
	int32 AttackShotsLeft = -1;            // aiAttackObject's maxShotsToFire (an assisting Patriot's clip); < 0: no limit
	uint32 HealScanFrame = 0;              // AutoFindHealingUpdate
	void UpdateAutoFindHealing(uint32 Frame);
	// ProjectileStreamUpdate and BoneFXUpdate (GenStreams.cpp)
	struct FGenStream
	{
		static constexpr int32 Max = 20;   // MAX_PROJECTILE_STREAM
		FGenObjectID Ids[Max] = {};        // a ring: the shots in flight, oldest first, holes where the aim changed
		int32 NextFree = 0;
		int32 FirstValid = 0;
		FGenObjectID Owner = GenInvalidObjectID;
		FGenObjectID TargetObject = GenInvalidObjectID;
		FVector3f TargetPos = FVector3f::ZeroVector;
	};
	TUniquePtr<FGenStream> Stream;
	FGenObjectID ProjectileStreams[3] = { GenInvalidObjectID, GenInvalidObjectID, GenInvalidObjectID }; // Weapon::m_projectileStreamID, per slot
	void AddStreamProjectile(FGenObjectID Source, FGenObjectID Shot, FGenObjectID Victim, const FVector3f* VictimPos);
	bool UpdateProjectileStream();
	void UpdateBoneFX(uint32 Frame);
	TArray<int32> BoneFXNext;              // the frame each of the template's BoneFX entries is due (-1: not)
	TArray<FVector3f> BoneFXBones;         // ... and where on the model it is
	TArray<uint32> BoneFXSystems;          // the particle systems it has started (killed when the damage state changes)
	int32 BoneFXState = -1;                // the body damage state BoneFXNext was set up for
	// CleanupHazardUpdate (GenCleanup.cpp)
	void UpdateCleanupHazard(uint32 Frame);
	void CleanupFireWhenReady(uint32 Frame);
	FGenObject* CleanupScanClosestTarget();
	FGenObjectID CleanupTarget = GenInvalidObjectID; // m_bestTargetID
	int32 CleanupNextScanFrames = 0;
	bool bCleanupInRange = false;
	float CleanupMoveRange = 0.f;          // > 0: cleaning up an area round CleanupPos, not just what is in reach
	FVector3f CleanupPos = FVector3f::ZeroVector;
	// DeployStyleAIUpdate
	enum class EDeploy : uint8 { ReadyToMove, Deploy, ReadyToAttack, Undeploy, AligningTurrets };
	EDeploy DeployState = EDeploy::ReadyToMove;
	uint32 DeployDoneFrame = 0;            // m_frameToWaitForDeploy
	void UpdateDeploy(uint32 Frame);
	void SetDeployState(EDeploy State, bool bReverse = false);
	/** Packing, unpacking or lining its turret up for that: AI_BUSY, it neither moves nor fires. */
	bool IsDeployBusy() const;
	/** Its weapons may fire (a deploying unit only once it has unpacked). */
	bool IsDeployedToFire() const;
	// AssaultTransportAIUpdate
	struct FAssaultMember
	{
		FGenObjectID ID = GenInvalidObjectID;
		bool bNew = false;                 // came aboard during this assault: stays in
		uint32 JoinFrame = 0;
	};
	TArray<FAssaultMember> AssaultMembers;
	FGenObjectID AssaultTarget = GenInvalidObjectID; // m_designatedTarget: what the DEPLOY weapon was last fired at
	FVector3f AssaultMoveGoal = FVector3f::ZeroVector;
	bool bAssaultAttackMove = false;
	bool bAssaultAttackObject = false;
	bool bAssaultNewOccupantsAreNew = false;
	uint32 LastPlayerOrderFrame = 0;       // (getLastCommandSource: when the player last told it anything)
	void UpdateAssaultTransport(uint32 Frame);
	void AssaultReset();
	void AssaultRetrieveMembers();
	void AssaultGiveFinalOrders();
	// UndeadBody + BattleBusSlowDeathBehavior
	bool bSecondLife = false;
	uint8 BusState = 0;                    // 1: thrown into the air by its first death, 2: down again, a hulk
	const FGenSlowDeath* BusDeath = nullptr;
	uint32 BusGroundCheckFrame = 0;
	uint32 BusPenaltyFrame = 0;
	uint32 BusNextHulkCheck = 0;
	float BusVelZ = 0.f;
	void StartSecondLife(const FString& DeathType);
	void UpdateBattleBus(uint32 Frame);
	// RebuildHoleBehavior (on the hole) and RebuildHoleExposeDie (on the building)
	FGenObjectID HoleWorker = GenInvalidObjectID;
	FGenObjectID HoleReconstructing = GenInvalidObjectID;
	int32 HoleWorkerWait = 0;
	const FGenThingTemplate* HoleRebuildTemplate = nullptr;
	const FGenThingTemplate* FootprintTemplate = nullptr; // a hole stands on its building's footprint (setGeometryInfo)
	bool bMasked = false;                  // OBJECT_STATUS_MASKED: neither selected nor targeted (a hole whose building is going up again)
	void UpdateRebuildHole(uint32 Frame);
	void HoleNewWorkerProcess(FGenObject* ExistingWorker);
	// manual animation frames (ManualDeployAnimations): the frame (at 30 a second) the logic puts the model's animation at
	float DrawManualFrame = -1.f;
	uint32 DrawManualFrameVersion = 0;
	// HordeUpdate
	bool bInHorde = false;
	bool bTrueHordeMember = false;         // in the thick of one (not just rubbing off on one)
	uint32 HordeNextFrame = 0;
	void UpdateHorde(uint32 Frame);
	/** AIUpdateInterface::evaluateMoraleBonus: the HORDE, NATIONALISM and FANATICISM weapon bonuses. */
	void EvaluateMoraleBonus();
	uint32 ScuttleFrame = 0;               // RiderChangeContain's m_scuttledOnFrame: its rider got off, it falls over
	bool bUnselectable = false;            // OBJECT_STATUS_UNSELECTABLE (a scuttled combat cycle)
	int32 SteerAnim = 0;                   // AnimationSteeringUpdate: the lean under way (a condition bit, 0: upright)
	uint32 SteerAnimNextFrame = 0;
	void UpdateAnimationSteering(uint32 Frame);
	TArray<TPair<FString, bool>> SubObjectOverrides;
	uint32 SubObjectVersion = 0;
	bool bAutoHealActive = false;
	bool bAutoHealDone = false;            // SingleBurst has fired
	uint32 NextAutoHealFrame = 0;

	// --- special powers ---
	TArray<uint32> PowerReady;             // parallel to the template's PowerModules
	TArray<bool> PowerPaused;
	bool bPowersPaused = false;        // disabled: its powers' clocks stand still since PowersPausedFrame
	uint32 PowersPausedFrame = 0;

	// --- special abilities ---
	enum class EAbilityPacking : uint8 { None, Packed, Unpacking, Unpacked, Packing };
	enum class EHackState : uint8 { None, Unpacking, Hacking, Packing };
	void UpdateAbility(uint32 Frame);
	void FinishAbility();
	void TriggerAbility(const struct FGenAbilityData& D, EGenAbilityKind Kind, FGenObject* Target);
	/** False if the preparation can't even begin (the target has changed hands, a trap went off ...). */
	bool StartAbilityPreparation(const struct FGenAbilityData& D, EGenAbilityKind Kind, FGenObject* Target, uint32 Frame);
	bool ContinueAbilityPreparation(const struct FGenAbilityData& D, EGenAbilityKind Kind, FGenObject* Target, uint32 Frame);
	void EndAbilityPreparation();
	FGenObject* CreateAbilityObject(int32 InAbility);
	void KillAbilityObjects(int32 InAbility);
	void PruneAbilityObjects();
	void UpdateHackInternet(uint32 Frame);
	void EndHackInternet();
	void UpdateStickyBomb(uint32 Frame);
	EHackState HackState = EHackState::None;   // HackInternetAIUpdate
	int32 HackFrames = 0;
	bool bHackWasContained = false;
	bool bHackAgain = false;               // told to hack while still packing up: it unpacks again when done
	bool bHackInterrupt = false;           // an order has come in (even one to stop)
	uint32 HackedUntilFrame = 0;
	bool bBoobyTrapped = false;
	FGenObjectID StickyTarget = GenInvalidObjectID; // StickyBombUpdate: what the charge sits on
	uint32 StickyNextPingFrame = 0;
	bool bStickyActive = false;
	TArray<TArray<FGenObjectID>> AbilityObjects; // per SpecialAbilityUpdate: the special objects it has out there
	const FGenThingTemplate* AbilityBeam = nullptr; // the beam held on the target while preparing
	bool bAbilityAttack = false;           // the ability itself has the unit shooting at its target
	bool bAbilityLoopSound = false;        // its preparation loop is playing
	bool bDisableFXToggle = true;          // m_doDisableFXParticles
	int32 WeaponLockSlot = INDEX_NONE;    // LOCKED_TEMPORARILY: till the attack is over
	int32 PermanentLockSlot = INDEX_NONE; // LOCKED_PERMANENTLY: a switch button's choice
	FGenObjectID XPSink = GenInvalidObjectID; // ExperienceTracker::setExperienceSink: who learns from what this kills (a charge's planter)
	bool bNoAutoChoose[3] = { false, false, false };
	const FGenWeaponSetEntry* WeaponSetEntry = nullptr; // the weapon set in use (its PreferredAgainst)
	void UpdateButtonHunt(uint32 Frame);
	void UpdateAutoDeposit(uint32 Frame);
	void UpdateOCL(uint32 Frame);
	void UpdateCrate(uint32 Frame);
	/** WeaponTemplate::estimateWeaponTemplateDamage / ActiveBody::estimateDamage: what the weapon would do to
	 *  the target, for choosing a weapon - a token 1 for what does no damage but is meant for it. */
	float EstimateWeaponDamage(const FGenWeaponTemplate& Weapon, const FGenObject& Target) const;
	/** DumbProjectileBehavior / MissileAIUpdate::projectileHandleCollision with a garrisoned building: this
	 *  shot (a flash-bang, a flame, a toxin stream) kills some of those inside instead of going off. True if
	 *  it did - the shot is gone then. */
	bool ProjectileClearsGarrison(FGenObjectID BuildingId);
	bool ExecuteCrate(FGenObject& Other, uint32 Frame);
	/** CreateCrateDie::onDie: what its death leaves behind for its killer's side. */
	void CreateCrates(const struct FGenDieModule& Module, FGenObjectID KillerId);
	/** The armour for its upgrade and the salvage it has bolted on. */
	void RefreshArmorSet();
	uint8 CrateWeaponLevel = 0;            // WEAPONSET_CRATEUPGRADE_ONE / _TWO: salvage has bettered its weapon this often
	uint8 CrateArmorLevel = 0;             // ARMORSET_CRATE_UPGRADE_ONE / _TWO
	uint32 GetFlashNow() const;
	uint32 OCLNextFrame = 0;               // OCLUpdate: the next delivery
	uint32 OCLStartFrame = 0;              // ... and when its countdown began (m_timerStartedFrame)
	bool bOCLFactionNeutral = true;        // ... nobody (who plays) owns it
	int32 OCLOwner = INDEX_NONE;           // ... whose it was when the timer was started
	int32 AbilityIndex = INDEX_NONE;       // the ability in progress (index into the template's Abilities)
	FGenObjectID AbilityTarget = GenInvalidObjectID;
	FVector3f AbilityTargetPos = FVector3f::ZeroVector;
	bool bAbilityHasPos = false;
	bool bAbilityNoTarget = false;
	int32 AbilityPrepFrames = 0;           // preparation left (the flag going up)
	int32 AbilityAnimFrames = 0;           // unpacking / packing left
	EAbilityPacking AbilityPacking = EAbilityPacking::None;
	bool bAbilityInRange = false;          // "Only get within range once."
	bool bAbilityFacingStarted = false;
	bool bAbilityFacingDone = false;
	bool bUsingAbility = false;
	float CaptureFlashPhase = 0.f;
	uint32 NextAbilityPathFrame = 0;
	FString HuntButton;                    // CommandButtonHuntUpdate: the button being hunted with
	uint32 NextHuntScanFrame = 0;
	uint32 DepositFrame = 0;               // AutoDepositUpdate: the next payment
	bool bDepositInitialized = false;
	bool bAwardCaptureBonus = false;
	uint32 FlashFrame = 0;
	FLinearColor FlashColor = FLinearColor::White;
	// --- payload: things dropped from aircraft, and markers that delete themselves ---
	void InitPayload();
	/** False when the object was dealt with here (deleted, went off, hangs under a parachute). */
	bool UpdatePayload(uint32 Frame);
	void DropPayloadItem(uint32 Frame);
	void FireVisiblePayload(uint32 Frame);
	uint32 DisabledUntilFrame = 0;         // EMP: no moving, shooting or building until then
	uint32 EMPUntilFrame = 0;              // ... of that, what an EMP did (leaflets and a lost master make no sound)
	bool bPowerDownHeard = false;          // Object::setDisabledUntil / clearDisabled: "power down" was heard, "power up" is next
	bool bOverchargeActive = false;
	// BattlePlanUpdate (GenBattlePlans.cpp)
	struct FBattlePlanState
	{
		uint8 Current = 0;                  // m_currentPlan: the one being unpacked, in force or packed
		uint8 Desired = 0;                  // m_desiredPlan
		uint8 AffectingArmy = 0;            // m_planAffectingArmy
		uint8 Status = 0;                   // TransitionStatus: 0 idle, 1 unpacking, 2 active, 3 packing
		uint32 NextReadyFrame = 0;
		FGenBattlePlanBonuses Bonuses;      // what this building's plan gives the army
	};
	FBattlePlanState BattlePlanState;
	void UpdateBattlePlan(uint32 Frame);
	void SetBattlePlanStatus(uint8 NewStatus);
	void SetBattlePlan(uint8 Plan);
	void TakeBackBattlePlan();
	/** ActiveBody::setMaxHealth with its MaxHealthChangeType. */
	void ChangeMaxHealth(float NewMax, const FString& ChangeType);
	/** TurretAI::recenterTurret, one frame of it; true once every turret rests at its natural angle and pitch. */
	bool RecenterTurrets();
	float DamageScalar = 1.f;              // ActiveBody::m_damageScalar (hold the line)
	float VisionScalar = 1.f;              // Object::setVisionRange (search and destroy)
	bool bTurretsDisabled = false;         // TurretAI::setTurretEnabled(false): the strategy center's gun outside the bombardment plan
	FString BounceSound;                   // an ObjectCreationList's BounceSound: heard each time it lands
	bool bWasAboveTerrain = false;         // WAS_AIRBORNE_LAST_FRAME
	uint32 DeletionFrame = 0;              // DeletionUpdate
	uint32 EMPFrame = 0;                   // EMPUpdate: when the pulse takes effect
	bool bEMPDone = false;
	void DoEMP(uint32 Frame);
	uint32 HeightDieArmedFrame = 0;
	bool bFalling = false;                 // dropped: falls freely until HeightDieUpdate ends it
	FGenObjectID ParachuteRider = GenInvalidObjectID; // ParachuteContain: who hangs under this parachute
	bool bHeldByParachute = false;         // ... and the rider: hidden from orders until it lands
	bool bChuteOpen = false;               // the free fall is over
	float ChuteRiderDrop = 0.f;            // ParachuteContain: how far below the canopy's PARA_ATTCH the rider's PARA_MAN hangs
	bool bChuteLandsWhereItOpens = false;  // no landing spot given (ParachuteContain's m_isLandingOverrideSet is false)
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
public:
	/** DeliverPayloadAIUpdate's delivery decal: where, how big and what - until the aircraft heads off the map
	 *  (HeadOffMapState::onEnter: killDeliveryDecal). False: none. */
	bool GetDeliveryMarker(FVector3f& OutPos, float& OutRadius, const FGenRadiusDecalTemplate*& OutDecal) const;
private:

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
	// --- fog of war and radar (GenShroud.cpp) ---
	/** DynamicShroudClearingRangeUpdate: the range grows from nothing, holds, and shrinks to FinalVision. */
	struct FDynShroudState
	{
		enum EState : uint8 { NotStarted, Growing, Sustaining, Shrinking, DoneForever, Sleeping };
		int32 StateCountDown = 0;
		int32 ShrinkStartDeadline = 0;
		int32 GrowStartDeadline = 0;
		int32 SustainDeadline = 0;
		uint32 DoneForeverFrame = 0;
		int32 ChangeIntervalCountdown = 0;
		float NativeRange = 0.f;
		float CurrentRange = 0.f;
		EState State = NotStarted;
	};
	void InitShroud();
	void UpdateDynamicShroud(uint32 Frame);
	void StartRadar(bool bDisableProof);
	void UpdateRadarDish(uint32 Frame);
	float ShroudClearingRange = 0.f;       // Object::m_shroudClearingRange
	FDynShroudState DynShroud;
	FGenSighting LastLook;                 // m_partitionLastLook: where it is looking from, and for whom
	FGenSighting LastRevealAll;            // m_partitionRevealAllLastLook: what it shows of itself to everybody else
	EGenObjectShroud LocalShroud = EGenObjectShroud::Clear;
	bool bEverSeenByLocal = false;         // PartitionData::m_everSeenByPlayer, for the local player
	uint32 ShroudClearFrame = 0;           // Drawable::m_shroudClearFrame: when the local player last saw it in the clear
	bool bRadarUpgraded = false;
	bool bRadarDisableProof = false;
	/** SpyVisionUpdate switched on by an upgrade (GenShroud.cpp): on for good, or on and off by turns. */
	struct FSpyVision
	{
		const FGenUpgradeModule* Module = nullptr;
		bool bActive = false;
		uint32 OffFrame = 0;               // m_deactivateFrame
		uint32 OnFrame = 0;                // (self powered: back on then)
	};
	TArray<FSpyVision> SpyVisions;
	void UpdateSpyVision(uint32 Frame);
	// PointDefenseLaserUpdate (GenPointDefense.cpp)
	struct FPointDefenseState
	{
		FGenObjectID Best = GenInvalidObjectID; // m_bestTargetID
		int32 NextScanFrames = 0;
		int32 NextShotFrames = 0;               // m_nextShotAvailableInFrames
		bool bInRange = false;
	};
	TArray<FPointDefenseState> PointDefense;
	void UpdatePointDefense();
	const FGenObject* ScanPointDefense(int32 Index);
	void FirePointDefenseWhenReady(int32 Index);
	uint32 RadarExtendDoneFrame = 0;       // RadarUpdate: the dish is unfolding until then

	// --- stealth (GenStealth.cpp) ---
	void InitStealth();
	void UpdateStealth(uint32 Frame);
	bool AllowedToStealth(uint32 Frame);
	/** SpawnBehavior::areAllSlavesStealthed: the units it keeps are stealthed as one group with it, so when one
	 *  of them may not be, all of them are shown up and none of them may. */
	bool AllSpawnsAllowedToStealth(uint32 Frame);
	void UpdateStealthDetector(uint32 Frame);
	/** False once the marker has done its last scan and removed itself. */
	bool UpdateGrantStealth();
	bool IsFiringWeaponStatus() const;
	bool bStealthEnabled = false;          // StealthUpdate::m_enabled
	bool bCanStealth = false;              // OBJECT_STATUS_CAN_STEALTH
	bool bStealthed = false;
	bool bDetected = false;
	uint32 StealthAllowedFrame = 0;
	uint32 DetectionExpiresFrame = 0;
	uint32 NextBlackMarketCheckFrame = 0;
	bool bDetectorEnabled = false;
	uint32 NextDetectFrame = 0;
	float GrantStealthRadius = 0.f;

	// --- veterancy ---
	void InitVeterancy();
	void OnVeterancyLevelChanged(int32 OldLevel, int32 NewLevel, bool bProvideFeedback = true);
	int32 VeterancyLevel = 0;
	int32 Experience = 0;
	TArray<FVector2f> RoutePoints;
	int32 RouteIndex = 0;
	/** AIFollowPathState: the next leg of the route, if there is one left (points within a cell of here are skipped). */
	bool StartNextRouteLeg();
	float PathExtraDistance = 0.f;     // AIUpdateInterface::m_pathExtraDistance: this much of the way lies beyond the path's end
	bool bWaypointLeg = false;         // the path under way ends at a point on the way, not where the move ends
	uint32 FormationID = 0;            // 0: none (NO_FORMATION_ID)
	uint32 MoveGroup = 0;              // the move order it last went with others on (steering: group mates are eased past, not sent away)
	FVector2f FormationOffset = FVector2f::ZeroVector;
	float FormationSpeed = 0.f;        // the formation's speed (its slowest member's) while FormationID is set
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
	/** Locomotor::calcLiftToUseAtPt: the upward acceleration the locomotor asks for this frame to hold its height. */
	float CalcLiftToUse() const;
	void UpdateBodyTilt(uint32 Frame);
	// --- the body of a vehicle on the ground (GenChassis.cpp) ---
	/** Drawable::calcPhysicsXformTreads / Wheels / Motorcycle, once a logic step after collisions. */
	void UpdateChassis(FRandomStream& ClientRandom);
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
	bool bJetLandingSoundPlayed = false;   // JetTakeoffOrLandingState: the wheels' screech on touching down, once
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
	uint32 JetAttackersMissExpireFrame = 0;
	bool bLocoSupersonic = false;          // flying on its SET_SUPERSONIC locomotor
	// CountermeasuresBehavior
	void ReportMissileForCountermeasures(FGenObject& Missile);
	FGenObjectID CalculateCountermeasureToDivertTo() const;
	void UpdateCountermeasures(uint32 Frame);
	void LaunchCountermeasureVolley(uint32 Frame);
	void ReloadCountermeasures();
	bool bCountermeasuresActive = false;
	int32 CMAvailable = 0;                 // flares left on board
	uint32 CMReactionFrame = 0;            // shot at: the first volley goes out then
	uint32 CMNextVolleyFrame = 0;
	uint32 CMReloadFrame = 0;
	TArray<FGenObjectID> CMFlares;         // the ones in the air, oldest first
	// EjectPilotDie, and the pilot himself (VeterancyCrateCollide, PilotFindVehicleUpdate)
	void EjectPilot(const FGenDieModule& Module);
	void EjectPilotBy(const FString& OCL);
	void PilotEnterVehicle(FGenObject& Vehicle);
	void UpdatePilotFindVehicle(uint32 Frame);
	uint32 PilotNextScanFrame = 0;
	bool bPilotMovedToBase = false;
	uint32 InvulnerableUntilFrame = 0;     // ObjectCreationList InvulnerableTime: nothing hurts it before
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
	void UpdateFactoryDoors(uint32 Frame);
	/** ProductionUpdate's DoorInfo: frame marks of a door on its way round (opening, standing open, closing). */
	struct FFactoryDoor
	{
		uint32 OpenedFrame = 0, WaitOpenFrame = 0, ClosedFrame = 0;
	};
	FFactoryDoor FactoryDoors[4];
	uint32 ConstructionCompleteFrame = 0;  // a unit has just been finished: CONSTRUCTION_COMPLETE shows from here
	int32 ExitDelayLeft = 0;               // QueueProductionExitUpdate: frames until the next may leave
	int32 ExitBurstLeft = 0;               // ... which the first so many needn't wait for
	uint32 DrawAnimDurationVersion = 0;
	int32 DrawAnimDurationFrames = 0;
	void SetDrawAnimDuration(int32 Frames) { DrawAnimDurationFrames = Frames; ++DrawAnimDurationVersion; }
	bool bPoweredKind = false;             // KINDOF_POWERED
	bool bUnderpowered = false;
	void UpdateDozer(uint32 Frame);
	void UpdateDozerBored(uint32 Frame);
	void StopConstructing();

	bool bUnderConstruction = false;
	float ConstructionPercent = 100.f;
	uint32 LastBuiltFrame = 0;         // last frame a dozer worked on this building
	uint32 SellFrame = 0;              // being sold, since this frame
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
	// ChinookAIUpdate's combat drop, and the soldier's side of it (AIRappelState)
	void CommandCombatDrop(FGenObject* Target, const FVector3f& Pos);
	void UpdateCombatDropApproach(uint32 Frame);
	bool BeginCombatDrop(uint32 Frame);
	void UpdateCombatDrop(uint32 Frame);
	void EndCombatDrop();
	void BeginRappel(FGenObject* Target, float Speed);
	void UpdateRappel(uint32 Frame);
	TUniquePtr<FGenCombatDrop> CombatDrop;
	bool bRappelling = false;
	FGenObjectID RappelTarget = GenInvalidObjectID; // the building it will land on
	float RappelRate = 0.f;
	float RappelDestZ = 0.f;
	TUniquePtr<FGenContain> Contain;
	TUniquePtr<FGenTrain> Train;          // RailroadBehavior (GenTrains.cpp)
	void UpdateTrain(uint32 Frame);
	void TrainLoadTrack();
	void TrainCreateCarriages();
	void TrainHitch(FGenObjectID Loco, int32 TemplateIndex, bool bByProximity);
	FGenObject* TrainFindCarBehind() const;
	void TrainGetPulled(FGenTrain::FPull& Info);
	void TrainPlace(const FGenTrain::FPull& Puller, FGenTrain::FPull& Mine);
	FVector3f TrainFindPos(float Dist, bool bSetState);
	void TrainCollide(uint32 Frame);
	void TrainOnCollide(FGenObject& Other);
	void TrainSetWall(bool bOn);
	void TrainDisembark();
	void TrainImpactSound(const FGenObject& Victim);
	/** RailroadBehavior::isRailroad: a car that is on the line and either leads the train or ends it. */
	bool IsDangerousRailroad() const;
	FGenObjectID ContainedBy = GenInvalidObjectID;
	uint32 ContainedFrame = 0;
	FGenObjectID EnterTarget = GenInvalidObjectID;
	bool bWantsExit = false;
	bool bHidden = false;
	int32 GarrisonPoint = INDEX_NONE;  // the fire point this occupant shoots from
	EGenChinookFlight ChinookFlight = EGenChinookFlight::Flying;
	FGenObjectID ChinookHealAirfield = GenInvalidObjectID; // m_airfieldForHealing: where it was sent for repairs
	void SetChinookHealAirfield(FGenObjectID Airfield);
	bool bHasExitRally = false;       // units: second leg of the way out of the factory (aiFollowExitProductionPath)
	FVector3f ExitRally = FVector3f::ZeroVector;
	// --- free physics (PhysicsBehavior for objects without a locomotor: debris, hulks) ---
	void UpdateFreePhysics(uint32 Frame);
	float GetYawFromRotation() const;
	void DoDeathPhase(int32 Phase);
	// --- combat (GenObjectCombat.cpp) ---
	void InitCombat();
	void UpdateCombat(uint32 Frame);
	void UpdateProjectile(uint32 Frame);
	// --- missiles (GenMissile.cpp: MissileAIUpdate on a THRUST locomotor) ---
	void LaunchMissile(uint32 Frame, const FQuat4f* LaunchRotation);
	void UpdateMissile(uint32 Frame, const FGenObject* Victim);
	void DetonateMissile(uint32 Frame);
	// --- weapon bonuses for a while, and leaflets (GenUpgrades.cpp: TempWeaponBonusHelper, WeaponBonusUpdate, LeafletDropBehavior) ---
	/** Object::doTempWeaponBonus: that weapon bonus condition for so many frames (a new one takes the old one's place). */
	void DoTempWeaponBonus(const FString& Condition, int32 DurationFrames);
	void UpdateWeaponBonusPulse(uint32 Frame);
	// --- the disguise (GenStealth.cpp: StealthUpdate's DisguisesAsTeam, the bomb truck) ---
	/** The look it is drawn with: the vehicle it is disguised as, or its own. */
	const FGenThingTemplate* GetDrawTemplate() const { return DisguiseTemplate ? DisguiseTemplate : Template; }
	/** Whose colours an enemy sees a disguised unit in. */
	int32 GetDisguisePlayer() const { return DisguisePlayer; }
	/** StealthUpdate::disguiseAsObject: take that one's look (or, with nothing, drop the disguise). */
	void DisguiseAs(const FGenObject* Target);
	void UpdateDisguise(uint32 Frame);
	const FGenThingTemplate* DisguiseTemplate = nullptr;
	int32 DisguisePlayer = INDEX_NONE;
	const FGenThingTemplate* NextDisguise = nullptr;   // the look it is changing to (nothing: its own)
	int32 NextDisguisePlayer = INDEX_NONE;
	int32 DisguiseFramesLeft = 0;
	int32 DisguiseFramesTotal = 0;
	bool bDisguiseSwapped = true;

	// --- the demo trap (GenDemoTrap.cpp) ---
	void InitDemoTrap();
	void UpdateDemoTrap(uint32 Frame);
	void DetonateDemoTrap();
	bool bDemoTrapDetonated = false;
	int32 DemoTrapScanWait = 0;

	// --- vehicles taken over (GenHijack.cpp) ---
	/** ConvertToHijackedVehicleCrateCollide / ConvertToCarBombCrateCollide::executeCrateBehavior. */
	void TakeOverVehicle(FGenObject& Vehicle);
	/** HijackerUpdate: riding in the vehicle he took, and out again when it is gone. */
	void UpdateHijackerRide(uint32 Frame);
	bool bHijacked = false;                // OBJECT_STATUS_HIJACKED: taken by a hijacker (nobody takes it twice)
	bool bCarBomb = false;                 // OBJECT_STATUS_IS_CARBOMB
	FGenObjectID HijackedVehicle = GenInvalidObjectID; // the hijacker: the vehicle he is driving
	FVector3f HijackEjectPos = FVector3f::ZeroVector;
	bool bHijackVehicleAirborne = false;

	// --- a driver sniped (GenCountermeasures.cpp, with the pilots) ---
	/** DISABLED_UNMANNED: its driver was shot (Jarmen Kell): it stands, nobody's, until infantry climb in. */
	bool IsUnmanned() const { return bUnmanned; }
	/** ActiveBody::attemptDamage, DAMAGE_KILLPILOT: the driver dies, the vehicle stays - empty and nobody's. */
	void MakeUnmanned(FGenObjectID Sniper);
	/** PhysicsBehavior::onCollide: "an infantry colliding with an unmanned vehicle ... becomes its new pilot". */
	void ClaimUnmanned(FGenObject& NewDriver);
	bool bUnmanned = false;

	// --- drones (GenDrones.cpp) ---
	void Enslave(const FGenObject& Slaver);
	void UpdateSlaved(uint32 Frame);
	void SlaveMoveNear(const FGenObject& Master, const FVector3f& Spot, int32 Range, int32 Wander, float DistToSpot);
	void SlaveGuard(FVector3f Pinned);
	void SlaveRepair(FGenObject& Master);
	void SlaveEndRepair();
	void SetSlaveRepairState(uint8 State, const FGenObject& Master);
	void SetSlaveRepairCondition(int32 Bit);
	/** Object::removeUpgrade: "not removing anything in the sense of undoing the effects: just resetting the upgrade so it may be run again". */
	void RemoveObjectUpgrade(const FString& UpgradeName);
	TUniquePtr<FGenSlaveState> Slave;
	// SpawnBehavior / SpawnPointProductionExitUpdate (GenSpawner.cpp)
	void UpdateSpawner(uint32 Frame);
	bool CreateSpawn();
	/** onDie / onDelete with SpawnedRequireSpawner: they go with it. */
	void EndSpawns(bool bDestroy);
	/** SpawnBehavior::getClosestSlave */
	FGenObject* GetClosestSpawn(const FVector3f& Pos) const;
	/** SpawnBehavior::onSpawnDeath: one of ours died (or went), to be replaced SpawnReplaceDelay later. With
	 *  AggregateHealth the spawner goes with the last of them, the killer scoring it. */
	void OnSpawnDeath(FGenObjectID Spawn, FGenObjectID Killer);
	/** SpawnBehavior::computeAggregateStates (AggregateHealth: the angry mob's nexus): their health and veterancy
	 *  are its own, its health bar over the middle of them. */
	void ComputeAggregateStates();
	/** The new spawn's place: by the barracks' door, budded from the one nearest us, or where we stand. */
	void ExitSpawn(FGenObject& Spawn);
	/** SpawnBehavior::maySpawnSelfTaskAI */
	bool MaySpawnSelfTask(float MaxSelfTaskersRatio) const;
	/** MobMemberSlavedUpdate::update: keep up with the nexus, take up its target. */
	void UpdateMobMember(uint32 Frame);
	TArray<FGenObjectID> Spawns;
	TArray<uint32> SpawnReplaceFrames;     // m_replacementTimes: when the next ones may come
	int32 SpawnTemplateIndex = 0;          // m_templateNameIterator: which of the names comes next
	int32 SpawnInitialBurstLeft = 0;       // m_initialBurstCountdown: still to leave by the barracks' door
	int32 SpawnCount = -1;                 // m_spawnCount (-1: NONE_SPAWNED_YET)
	int32 SelfTaskingSpawns = 0;           // m_selfTaskingSpawnCount
	FVector3f HealthBoxOffset = FVector3f::ZeroVector; // Object::m_healthBoxOffset: the mob's health bar over the mob
	// MobMemberSlavedUpdate
	FGenObjectID MobSlaver = GenInvalidObjectID;
	FGenObjectID MobPrimaryVictim = GenInvalidObjectID; // the nexus's last target, remembered
	int32 MobFramesToWait = 0;
	int32 MobCatchUpCrisisTimer = 0;
	bool bMobSelfTasking = false;
	uint32 LastAICommandFrame = 0;         // getLastCommandSource is CMD_FROM_AI while this is no earlier than LastPlayerOrderFrame
	TArray<FVector3f> SpawnPoints;         // the spawn point bones in the world (found once)
	TArray<float> SpawnPointAngles;
	TArray<FGenObjectID> SpawnPointUsers;  // m_spawnPointOccupier
	bool bSpawnsInited = false;
	uint32 NextSpawnUpdate = 0;
	uint32 FaerieFireUntilFrame = 0;         // OBJECT_STATUS_FAERIE_FIRE: marked by a target designator - whoever shoots at it shoots faster
	void UpdatePropagandaTower(uint32 Frame);
	void DoPropagandaScan();
	void RemovePropagandaInfluence();
	TArray<FGenObjectID> PropagandaInside;   // PropagandaTowerBehavior: who has our bonus now
	uint32 PropagandaScanFrame = 0;
	void UpdateLeafletDrop(uint32 Frame);
	void DoLeafletDisable(uint32 Frame);
	FString TempBonus;
	uint32 TempBonusEndFrame = 0;
	uint32 NextBonusPulseFrame = 0;
	uint32 LeafletStartFrame = 0;
	// --- mines (GenMines.cpp: MinefieldBehavior, GenerateMinefieldBehavior) ---
	void InitMine();
	void UpdateMine(uint32 Frame);
	void SetMineScoot(const FVector3f& Start, const FVector3f& End);
	void MineDetonateOnce(const FVector3f& At);
	void OnMineCollide(FGenObject& Other, uint32 Frame);
	void OnMineHealthChanged(bool bHealing, FGenObjectID Source, const FString& DamageType);
	/** A mover asks what mines its shape is on, and sets them off. */
	void DetonateMinesInTheWay(uint32 Frame);
	void PlaceMines();
	FGenObject* PlaceMineAt(const FVector3f& Pt, const FGenThingTemplate& MineT);
	void UpdateMinefieldUpgrade();
	struct FGenMineDeleter { void operator()(struct FGenMineState* State) const; };
	TUniquePtr<struct FGenMineState, FGenMineDeleter> MineState;
	bool bMinesGenerated = false;
	bool bMinesUpgraded = false;
	// Clearing mines (GenMines.cpp):
	/** OBJECT_STATUS_MASKED on a mine: all of it gone off or disarmed, it waits to grow back - no target meanwhile. */
	bool IsMineMasked() const;
	/** MinefieldBehavior::disarm (DAMAGE_DISARM): gone, or - one that grows back - empty for now. */
	void DisarmMine();
	/** AIUpdateInterface::isClearingMines: attacking a mine with an anti-mine weapon. */
	bool IsClearingMines() const;
	/** WEAPONSET_MINE_CLEARING_DETAIL: a dozer or worker with nothing to build or mend has its disarming tool out. */
	void UpdateMineClearingDetail();
	void SetMineClearingDetail(bool bOn);
	/** AIAttackState with a ContinueAttackRange: the nearest mine (or like thing) to clear around a spot. */
	FGenObject* FindContinueTarget(const FVector3f& Around, float Range, int32 VictimOwner, bool bIgnoreStealth) const;
	/** Command_DisarmMinesAtPosition: "attack" the spot - the nearest mine within the weapon's continue range of it. */
	void CommandClearMinesAt(const FVector3f& Pos, int32 Slot);
	bool bMineClearingDetail = false;
	FVector3f ContinueOrigin = FVector3f::ZeroVector; // where the first victim of a continuing attack stood
	bool bHasContinueOrigin = false;
	bool bIgnoringStealth = false;         // OBJECT_STATUS_IGNORING_STEALTH: sent to clear mines it can't see
	TArray<FGenObjectID> GeneratedMines;
	// --- the particle cannon (GenParticleCannon.cpp: ParticleUplinkCannonUpdate) ---
	void UpdateParticleCannon(uint32 Frame);
	void SetParticleCannonStatus(uint8 NewStatus, uint32 Frame);
	struct FGenParticleCannonDeleter { void operator()(struct FGenParticleCannonState* State) const; };
	TUniquePtr<struct FGenParticleCannonState, FGenParticleCannonDeleter> ParticleCannon;
	// --- the Spectre gunship (GenGunship.cpp: SpectreGunshipUpdate) ---
	/** False when it has just left the map. */
	bool UpdateGunship(uint32 Frame);
	void GunshipDepart(uint32 Frame);
	struct FGenGunshipDeleter { void operator()(struct FGenGunshipState* State) const; };
	TUniquePtr<struct FGenGunshipState, FGenGunshipDeleter> Gunship;
	// --- the nuclear missile, its blast and its silo (GenNuke.cpp) ---
	void LaunchNeutronMissile(uint32 Frame);
	void UpdateNeutronMissile(uint32 Frame);
	void UpdateNeutronBlast(uint32 Frame);
	void UpdateMissileLauncherBuilding(uint32 Frame);
	void SwitchLauncherDoor(uint8 NewState, uint32 Frame);
	uint32 NeutronBlastFrame = 0;      // when its death began (0: not yet)
	uint16 NeutronBlastsDone = 0, NeutronScorchDone = 0;
	bool bNeutronScorchPlaced = false;
	uint8 LauncherDoorState = 0;       // closed, opening, open, open after the launch, closing
	uint8 LauncherTimeoutState = 0;
	uint32 LauncherTimeoutFrame = 0;
	void UpdateDying(uint32 Frame);
	void Die(const FString& DeathType, FGenObjectID Killer);
	FGenObject* FindAutoTarget() const;
	int32 ChooseWeapon(const FGenObject& Target) const;
	bool CanWeaponTarget(const FGenWeaponTemplate& W, const FGenObject& Target) const;
	float AttackDistance(const FGenObject& Target) const;
	bool AimAt(const FGenObject& Target, int32 Slot);
	/** The turret (or the whole unit) turned towards a point; true once it bears. bGroundTarget: nothing in the air there. */
	bool AimAtPoint(const FVector3f& Point, bool bGroundTarget, int32 Slot);
	/** A player's attack on a spot this frame: out of range it heads for it, in range it aims. True: fire. */
	bool UpdatePositionApproach(int32 Slot, uint32 Frame);
	void FireWeapon(int32 Slot, FGenObject& Target, uint32 Frame);
	void FireWeaponAtPosition(int32 Slot, const FVector3f& Pos, uint32 Frame);
	/** Weapon::privateFireWeapon: "If I have a set scatter pattern, I need to offset the target by a random pick from that pattern." */
	bool PickScatterTarget(int32 Slot, FVector3f& InOutPos);
	/** Where the slot's next shot leaves from (the launch bones in turn), or false: no bone. */
	bool NextLaunchTransform(int32 Slot, FTransform3f& OutBone);
	/** The books after a shot: counts, the wait to the next one, the clip. */
	void FinishShot(int32 Slot, FGenObjectID Victim, uint32 Frame);
	/** DumbProjectileBehavior / MissileAIUpdate::update: the layer a shot is over; true when it has just come down
	 *  through the deck of a bridge it was over (and bMayDetonate) - it is then put on the deck to go off there. */
	bool ProjectileHitsBridgeDeck(bool bMayDetonate);
	bool bAttackingPosition = false;
	bool bCombatPosesSet = false;      // a firing or attacking model condition is set: the combat update has to run to take it back
	FVector3f AttackPosition = FVector3f::ZeroVector;
	int32 AttackPositionShots = 0;
	int32 AttackPositionSlot = 0;
	bool bAttackPositionApproach = false; // a player's: walked up to and aimed at
	bool bAttackPositionLocked = false;   // ... with a button's weapon, LOCKED_TEMPORARILY: "just till the weapon is empty"
	bool bForceAttack = false;
	void ClearAttack();
	void SetFiringConditions(int32 Slot, int32 State);
	int32 TurretForSlot(int32 Slot) const;
	// --- AI ---
	void UpdateAI(uint32 Frame);
	float PathDistanceFrom(int32 Index) const;
	/** Picks a free destination near InGoal (one that also satisfies Accept, if given), reserves it and starts following a path to it. */
	bool StartPath(const FVector3f& InGoal, bool bAroundUnits, const TFunction<bool(const FVector2f&)>* Accept = nullptr, bool bIntoUnit = false, bool bWaypoint = false);
	/** Heads straight for InGoal, ignoring obstacles and without looking for a free spot (docking lanes run through buildings). */
	void StartDirectMove(const FVector3f& InGoal);
	/** A spot from which the weapon reaches Target, as close to where we are coming from as possible. */
	bool StartApproach(const FGenObject& Target, float Range, float MinRange = 0.f);
	/** ... and from which it reaches a spot on the ground. */
	bool StartApproachPos(const FVector3f& Spot, float Range, float MinRange);
	/** What it can move over: its locomotor's surfaces, and fences if it can crush ("isCrusher = obj->getCrusherLevel() > 0"). */
	uint32 GetPathSurfaces() const;
	// CrushDie
	FGenObjectID CrushedBy = GenInvalidObjectID;
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
	void TryCrush(FGenObject& Other);
	void MoveAwayFromUnit(const FGenObject& Unit);
	void MoveToFreeGround();
	/** Separating-axis overlap of the two footprints: how far, and which way, this one has to go to be clear of Other. */
	bool GeometryPenetration(const FGenObject& Other, FVector2f& OutPush) const;

	// --- steering (GenSteering.cpp; beyond the original, the user's "move like in modern games") ---
	struct FSteerChoice
	{
		FVector2f Dir = FVector2f::ZeroVector; // which way to drive
		float Speed = 0.f;                     // ... no faster than this
		bool bSteered = false;                 // something near changed the way or the pace
		bool bWait = false;                    // no way on for now: stand, turning on the spot if that is free
	};
	/** Moves with local steering (a vehicle; infantry around vehicles) rather than the original's stop and wait. */
	bool UsesSteering() const;
	/** Whether Other is something our steering keeps clear of (not what we drive over, what we are going for, what steps aside for us). */
	bool SteersAround(const FGenObject& Other) const;
	/** The way and the pace for this frame, towards Toward (DistToWaypoint away; DistToGoal left of the whole move) at PreferredSpeed at the most. */
	FSteerChoice Steer(const FVector2f& Toward, float DistToWaypoint, float DistToGoal, float PreferredSpeed, uint32 Frame);
	/** Standing idle and free to move: may be asked to make way. */
	bool CanMakeWay() const;
	/** Not held to its place (a docking lane, a factory's door, packing up, building, an ability): the collision
	 *  pass may push it over. */
	bool CanBePushed() const;
	/** Pathfinder::moveAllies / getMoveAwayFromPath: go to the nearest free spot off Mover's way (Along). */
	void MakeWayFor(const FGenObject& Mover, const FVector2f& Along);
	bool StartMakeWay(FGenObjectID Mover, const FVector2f& Along, bool bResume, const FVector3f& Resume);
	/** Turning on the spot to NewOrientation runs into nobody. */
	bool IsPivotClear(float NewOrientation) const;

	// --- Locomotor (Locomotor.cpp) ---
	/** bHeldUp: held up by another unit - Locomotor's *blocked: "stop if we are about to run into the blocking object", turning on the spot. */
	void LocoMoveTowardsPosition(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame, bool bHeldUp = false);
	void MoveTreads(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed);
	void MoveWheels(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame);
	void MoveLegs(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed);
	/** Locomotor::moveTowardsPositionClimb: up a cliff facing it, down it backwards. */
	void MoveClimb(const FVector3f& Goal, float OnPathDistToGoal, float DesiredSpeed);
	/** LocomotorSet::findLocomotor for the ground under the unit (Pathfinder::chooseBestLocomotorForPosition); null: none fits. */
	const FGenLocomotorTemplate* FindLocomotorForPosition() const;
	/** AIUpdateInterface::chooseGoodLocomotorFromCurrentSet: every frame, the set's locomotor for where the unit is. */
	void ChooseGoodLocomotorFromCurrentSet();
	/** Fills LocoSet / LocoSetSurfaces from a set's locomotor names. */
	void SetLocomotorSet(const TArray<FString>& Names);
	int32 RotateAroundLocoPivot(const FVector3f& Goal, float MaxTurnRate, float* OutRelAngle = nullptr);
	float KeepTurningWay(float Amount) const;
	void ApplyGoalSpeed(float GoalSpeed, float ActualSpeed, float MaxAccel, float BrakingFactor, bool bBackwards = false);
	/** A push that keeps GoalSpeed along Dir whichever way the nose points (sliding into place), braking to stop at the goal. */
	void ApplyGoalSpeedAlong(float GoalSpeed, const FVector2f& Dir, float OnPathDistToGoal);
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
	FVector3f PrevPosition = FVector3f::ZeroVector;    // drawn: the earlier logic frame of the pair
	float PrevOrientation = 0.f;
	FQuat4f Rotation = FQuat4f::Identity;
	FQuat4f PrevRotation = FQuat4f::Identity;
	FVector3f DrawPosition = FVector3f::ZeroVector;    // drawn: the later one
	float DrawOrientation = 0.f;
	FQuat4f DrawRotation = FQuat4f::Identity;
	FVector3f SnapPosition = FVector3f::ZeroVector;    // where it was put in one go since the last publish
	float SnapOrientation = 0.f;
	FQuat4f SnapRotation = FQuat4f::Identity;
	bool bDrawSnap = true;
	bool bDrawPublished = false;
	FString DebrisModel;
	bool bDebrisHouseColor = false;
	FString DebrisAnims[3];
	FString DebrisFXFinal;
	bool bFreeRotation = false;        // pitch / roll in use: the drawable follows Rotation instead of the heading

	float Health = 1.f;
	float MaxHealth = 1.f;
	bool bHasAI = false;
	bool bNoCollide = false;           // KindOf NO_COLLIDE (the angry mob's nexus): no part in unit collisions
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
	FVector3f PrevAccel = FVector3f::ZeroVector; // m_prevAccel
	int8 Turning = 0;                // m_turning: TURN_NEGATIVE -1, TURN_NONE 0, TURN_POSITIVE 1
	int8 PrevTurning = 0;            // ... last frame (a goal straight behind doesn't flip the turn)
	FGenObjectID CurrentOverlap = GenInvalidObjectID;  // m_currentOverlap: what it is driving over (crushing) this step
	FGenObjectID PreviousOverlap = GenInvalidObjectID;
	// DrawableLocoInfo: the body's springs (pitch and roll towards the ground's, and the extra from speeding up,
	// braking and turning), the fake height over what it crushes, and the wheels.
	float ChassisPitch = 0.f, ChassisPitchRate = 0.f, ChassisRoll = 0.f, ChassisRollRate = 0.f;
	float AccelPitch = 0.f, AccelPitchRate = 0.f, AccelRoll = 0.f, AccelRollRate = 0.f;
	float OverlapZ = 0.f, OverlapZVel = 0.f;
	FWheelInfo Wheels, DrawWheels;
	FChassisPose Chassis, PrevChassis, DrawChassis;
	float ExtraFriction = 0.f;
	float YawRate = 0.f, PitchRate = 0.f, RollRate = 0.f;

	// LifetimeUpdate / ObjectCreationList
	uint32 LifetimeDieFrame = 0;
	FGenObjectID Producer = GenInvalidObjectID;

	// Locomotor
	const FGenLocomotorTemplate* Locomotor = nullptr;
	/** LocomotorSet: every locomotor of the set in use (Burton's ground and cliff ones); Locomotor is the one for
	 *  the ground under the unit. LocoSetSurfaces: all of their surfaces (getValidSurfaces), what paths are found on. */
	TArray<const FGenLocomotorTemplate*, TInlineAllocator<2>> LocoSet;
	uint32 LocoSetSurfaces = 0;
	bool bLocoBraking = false;       // IS_BRAKING
	bool bLocoBackwards = false;     // MOVING_BACKWARDS (a climber backing down a cliff, too)
	bool bLocoClimbing = false;      // CLIMBING: on a cliff face, with the climbing animation
	uint8 LastClimbState = 0;        // (what the [climb] trace said last)
	bool bLocoThreePointTurn = false;
	float BrakingFactor = 1.f;
	uint32 DonutTimer = 0;

	// AI move state
	bool bMoving = false;
	TArray<FVector2f> Path;
	TArray<uint8> PathLayers;           // the layer of each point of Path (empty: all on the ground)
	int32 PathIndex = 0;
	uint8 Layer = 0;
	void UpdateLayer();
	FVector3f Goal = FVector3f::ZeroVector;
	FVector3f RequestedGoal = FVector3f::ZeroVector; // where the move order asked to go (re-paths aim here)
	uint32 StuckFrames = 0;            // frames without getting anywhere with nothing in the way
	uint32 WaitFrames = 0;             // ... and spent waiting on other units, since ProgressPos
	FVector2f ProgressPos = FVector2f::ZeroVector; // where the move last got somewhere
	float ProgressAngle = 0.f;         // ... and which way it faced then (turning round is progress too)
	bool bDirectMove = false;          // StartDirectMove: straight through whatever is there (docking lanes, factory doors) - while bMoving

	// Unit collision
	bool bBlocked = false;             // another unit is in the way (set by the collision pass, consumed next frame)
	bool bBlockedAndStuck = false;     // ... and it is not going to move: path around it
	uint32 BlockedFrames = 0;
	float CurMaxBlockedSpeed = 99999.f; // fastest we may go without running into the unit ahead
	uint32 IgnoreCollisionsUntil = 0;
	FGenObjectID IgnoreObstacle = GenInvalidObjectID; // AIUpdateInterface::ignoreObstacle: what a contact weapon is being carried up to
	FGenObjectID ContactTarget = GenInvalidObjectID;  // ... and what it has been carried up to
	uint32 NextBlockedRepathFrame = 0;
	bool bSidestepPath = false;        // the current path was made to get around a parked unit
	bool bMovingOutOfWay = false;      // AI_MOVE_OUT_OF_THE_WAY: stepping aside for MoveOutOfWayOf
	FGenObjectID MoveOutOfWayOf = GenInvalidObjectID;
	bool bHasResumeGoal = false;       // continue to ResumeGoal after stepping aside
	FVector3f ResumeGoal = FVector3f::ZeroVector;
	// Steering (GenSteering.cpp)
	int8 SteerSide = 0;                // the side the last way round went (+1 left, -1 right, 0 none)
	float SteerHeading = 0.f;          // ... and its heading (while SteerSide is set)
	uint8 SteerHold = 0;               // frames of a clear way still to see before heading back to it
	FGenObjectID SteerBlocker = GenInvalidObjectID; // the unit we are waiting on, while we wait

	// Combat
	FGenWeaponInstance Weapons[3];
	const FGenArmorTemplate* Armor = nullptr;
	// ActiveBody::doDamageFX
	void DoDamageFX(const FString& DamageType, float Amount, FGenObjectID Source);
	const FGenDamageFX* DamageFX = nullptr;
	FString LastDamageFXType;
	uint32 NextDamageFXFrame = 0;
	// ToppleUpdate and W3DTreeBuffer's toppling (GenTopple.cpp)
	enum class EToppleState : uint8 { Upright, Falling, Down };
	void UpdateTopple(uint32 Frame);
	void ToppleWhatIsInTheWay();
	void DieByToppling();
	EToppleState ToppleState = EToppleState::Upright;
	FVector2f ToppleDirection = FVector2f::ZeroVector;
	float ToppleVelocity = 0.f;            // radians per frame
	float ToppleAcceleration = 0.f;
	float ToppleAccumulation = 0.f;        // how far over it is
	uint32 ToppleOptions = 0;
	int32 ToppleNumAngleDeltaX = 0;        // frames left of turning to fall flat
	float ToppleAngleDeltaX = 0.f;
	FGenObjectID ToppleStump = GenInvalidObjectID;
	int32 TreeSinkFramesLeft = 0;          // a fallen tree sinking away
	// StructureCollapseUpdate / StructureToppleUpdate (GenCollapse.cpp)
	enum class ECollapseState : uint8 { Standing, Waiting, Falling, WaitingForDone, Done };
	void BeginStructureCollapse(uint32 Frame);
	void BeginStructureTopple(uint32 Frame, FGenObjectID Killer);
	void UpdateStructureCollapse(uint32 Frame);
	void UpdateStructureTopple(uint32 Frame);
	void DoCollapsePhase(int32 Phase, const FVector3f& At);
	void DoStructureTopplePhase(int32 Phase, const FVector3f& At);
	void ApplyStructureCrushingDamage(float Theta);
	ECollapseState CollapseState = ECollapseState::Standing;
	bool bStructureToppling = false;       // falling over, not sinking
	uint32 CollapseFrame = 0;
	uint32 CollapseBurstFrame = 0;
	float CollapseVelocity = 0.f;
	float CollapseHeight = 0.f;            // how far it has sunk (negative)
	FVector3f CollapseBasePos = FVector3f::ZeroVector;
	FVector2f StructureToppleDirection = FVector2f::ZeroVector;
	float StructureToppleAngle = 0.001f;   // "Need to give it a little nudge in the right direction"
	float StructureIntegrity = 0.f;
	float LastCrushedLocation = 0.f;
	FVector3f DelayBurstLocation = FVector3f::ZeroVector;
	// FlammableUpdate / FireSpreadUpdate (GenFire.cpp)
	enum class EFlameStatus : uint8 { Normal, Aflame, Burned };
	void OnFlameDamage(float ActualDamage, uint32 Frame);
	void TryToIgnite();
	void UpdateFlammable(uint32 Frame);
	EFlameStatus FlameStatus = EFlameStatus::Normal;
	bool bBurned = false;                  // OBJECT_STATUS_BURNED
	float FlameDamageLimit = 0.f;
	uint32 LastFlameDamageFrame = 0;
	uint32 AflameEndFrame = 0;
	uint32 BurnedEndFrame = 0;
	uint32 FlameDamageFrame = 0;
	uint32 NextFireSpreadFrame = 0;
	FGenObjectID TargetID = GenInvalidObjectID;
	bool bExplicitAttack = false;      // player ordered this attack (pursue)
	uint32 NextAutoAcquireFrame = 0;
	uint32 NextRepathFrame = 0;
	float TurretAngle[2] = { 0.f, 0.f };
	float TurretPitch[2] = { 0.f, 0.f };
	float PrevTurretPitch[2] = { 0.f, 0.f };
	float DrawTurretPitch[2] = { 0.f, 0.f };
	/** Weapon::isWithinTargetPitch: the weapon can be brought to bear on something that much higher or lower. */
	bool IsWithinTargetPitch(const FGenWeaponTemplate& W, const FGenObject& Target) const;
	/** Weapon::getPreAttackDelay: how long the weapon winds up before this shot at that target. */
	int32 GetPreAttackDelay(int32 Slot, const FGenObject& Target) const;
	/** TurretAI::friend_turnTowardsPitch */
	bool TurnTurretTowardsPitch(int32 Turret, float Desired, float RateModifier);
	FGenObjectID ConsecutiveVictim = GenInvalidObjectID; // FiringTracker: whom the last shots went at ...
	int32 ConsecutiveShots = 0;                           // ... and how many of them in a row
	// FiringTracker's speeding up (a new attack order doesn't start it over; only going without firing does):
	void NoteContinuousFire(const FGenWeaponTemplate& W, const FGenWeaponInstance& WI, FGenObjectID Victim, uint32 Frame);
	void SetContinuousFireLevel(int32 Level, bool bSlow);
	void ContinuousFireCoolDown();
	float PreviousHealth = 0.f;                           // BodyModule::getPreviousHealth: what it had before the last thing that hurt (or killed) it
	FGenObjectID ContinuousVictim = GenInvalidObjectID;
	int32 ContinuousShots = 0;
	uint32 ContinuousCooldownFrame = 0;                   // when it starts to slow down unless it fires again (0: nothing to slow)
	uint8 ContinuousLevel = 0;                            // 0 none, 1 CONTINUOUS_FIRE_MEAN, 2 CONTINUOUS_FIRE_FAST
	FPendingPath PendingPath;
	bool bPathQueued = false;                  // in the logic's path queue
	bool DeferPath(const FPendingPath& Request);
	float PrevTurretAngle[2] = { 0.f, 0.f };   // drawn (see PublishDraw)
	float DrawTurretAngle[2] = { 0.f, 0.f };
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

	int32 PartitionSlot = INDEX_NONE;   // FGenPartition: its place in this step's index (INDEX_NONE: made since)
	friend class FGenPartition;
	friend class UGenGameLogic;
	friend class FGenSkirmishAI;
	friend class FGenParkingPlace;
};
