#include "Logic/GenObject.h"
#include "Audio/GenAudioData.h"
#include "Logic/GenCommandData.h"
#include "Core/GenPerf.h"
#include "Client/GenFXData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPathfinder.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	constexpr float MaxBrakingFactor = 5.f;           // Locomotor.cpp MAX_BRAKING_FACTOR
	constexpr float CellSize = GenLogic::PathfindCellSize;
	enum { TurnNone = 0, TurnPositive = 1, TurnNegative = -1 };

	FORCEINLINE float Sqr(float V) { return V * V; }
}

FGenObject::FGenObject(FGenObjectID InID, const FGenThingTemplate* InTemplate, int32 InOwner, UGenGameLogic* InLogic)
	: ID(InID), Template(InTemplate), Owner(InOwner), Logic(InLogic)
{
	MaxHealth = Template->MaxHealth > 0.f ? Template->MaxHealth : 1.f;
	Health = Template->InitialHealth > 0.f ? Template->InitialHealth : MaxHealth;
	bHasAI = Template->bHasAI;
	bNoCollide = Template->bKindNoCollide;
	bImmobile = Template->IsKindOf(TEXT("IMMOBILE")) || Template->IsKindOf(TEXT("STRUCTURE"));
	Mass = FMath::Max(0.01f, Template->Mass);
	// The default frictions are per-frame values at 30 fps; rescale for the configured rate.
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS());
	ForwardFriction = Template->bHasPhysics ? Template->ForwardFriction : 0.15f * RateScale;
	LateralFriction = Template->bHasPhysics ? Template->LateralFriction : 0.15f * RateScale;

	if (const TArray<FString>* Set = Template->LocomotorSets.Find(TEXT("SET_NORMAL")))
	{
		// chooseGoodLocomotorFromCurrentSet for "a newly-created object": its ground locomotor (or the first there is).
		SetLocomotorSet(*Set);
		for (const FGenLocomotorTemplate* L : LocoSet)
		{
			if (L->Surfaces & GenSurface::Ground)
			{
				Locomotor = L;
				break;
			}
		}
		if (!Locomotor && LocoSet.Num() > 0)
		{
			Locomotor = LocoSet[0];
		}
	}
	bFlies = Locomotor && Locomotor->IsAirOnly();
	InitJet();
	bSpawnPending = !Template->SpawnTemplate.IsEmpty() && Template->SpawnNumber > 0;
	bPoweredKind = Template->IsKindOf(TEXT("POWERED"));
	ExitBurstLeft = Template->ProductionInitialBurst;
	SpawnInitialBurstLeft = Template->Spawner.InitialBurst;
	if (Template->bProjectileStream)
	{
		Stream = MakeUnique<FGenStream>();
	}
	if (Template->MobMember.bValid)
	{
		MobFramesToWait = Logic->GetRandom().RandRange(0, 20); // MobMemberSlavedUpdate's constructor
	}
	InitSupply();
	InitContain();
	bAllowBounce = Template->bAllowBouncing;
	CMAvailable = Template->Countermeasures.NumberOfVolleys * Template->Countermeasures.VolleySize;
	if (Template->bHasLifetime)
	{
		// LifetimeUpdate: dies after a random time in [Min, Max], at least one frame from now.
		const int32 Delay = Logic->GetRandom().RandRange(Template->MinLifetime, FMath::Max(Template->MinLifetime, Template->MaxLifetime));
		LifetimeDieFrame = Logic->GetFrame() + uint32(FMath::Max(1, Delay));
	}
	InitCombat();
	InitUpgrades();
	if (Template->Railroad.bValid)
	{
		Train = MakeUnique<FGenTrain>();
		Train->bLocomotive = Template->Railroad.bIsLocomotive;
		Train->bLead = Train->bLocomotive; // "for now, I am the lead, only if I am the locomotive"
		Train->State = Train->bLocomotive ? FGenTrain::EConductor::Accelerate : FGenTrain::EConductor::Coast;
	}
	if (Template->LockWeaponSlot != INDEX_NONE)
	{
		PermanentLockSlot = Template->LockWeaponSlot; // LockWeaponCreate::onBuildComplete: setWeaponLock( slot, LOCKED_PERMANENTLY )
	}
	// BattlePlanUpdate::onObjectCreated: "enableTurret( false )" - the gun only comes out with the bombardment plan.
	bTurretsDisabled = Template->BattlePlan.bValid;
	InitPayload();
	InitVeterancy();
	if (Template->Minefield.bValid)
	{
		InitMine();
	}
	InitShroud();
	InitStealth();
	if (Template->DemoTrap.bValid)
	{
		InitDemoTrap();
	}
}

bool FGenObject::IsDisabled() const
{
	return bUnderpowered || bUnmanned || bScriptHeld || Logic->GetFrame() < FMath::Max(DisabledUntilFrame, HackedUntilFrame);
}

bool FGenObject::ShouldAnimate(bool bConsiderPower) const
{
	// Drawable::getShouldAnimate. ("helicopters just look goofy if they stop animating, so keep animating them, anyway")
	if (Logic->GetFrame() < FMath::Max(DisabledUntilFrame, HackedUntilFrame) && !Template->IsKindOf(TEXT("PRODUCED_AT_HELIPAD")))
	{
		return false;
	}
	return !(bConsiderPower && bUnderpowered);
}

void FGenObject::SetRotation(const FQuat4f& Q)
{
	Rotation = Q.GetNormalized();
	Orientation = GetYawFromRotation();
}

float FGenObject::GetYawFromRotation() const
{
	// Matrix3D::Get_Z_Rotation: heading of the local X axis.
	const FVector3f X = Rotation.GetAxisX();
	return (FMath::Abs(X.X) < 1e-5f && FMath::Abs(X.Y) < 1e-5f) ? Orientation : FMath::Atan2(X.Y, X.X);
}

bool FGenObject::IsKindOf(const TCHAR* Kind) const
{
	return Template->IsKindOf(Kind);
}

bool FGenObject::IsSelectable() const
{
	// (BuildAssistant::sellObject: a building being sold is OBJECT_STATUS_UNSELECTABLE from that moment.)
	// (And a drone is nobody's to select: NO_SELECT, and OBJECT_STATUS_UNSELECTABLE once it has a master.)
	// (A salvage crate is SELECTABLE only "so the mouse can interact with it": "being dead makes them not actually selectable".)
	return !bDead && !bPendingDestroy && SellFrame == 0 && !bUnselectable && !bMasked && ContainedBy == GenInvalidObjectID && HijackedVehicle == GenInvalidObjectID && Template->IsKindOf(TEXT("SELECTABLE"))
		&& !Template->IsKindOf(TEXT("NO_SELECT")) && !Template->Crate.IsValid();
}

// ---- orders -------------------------------------------------------------------------------------

void FGenObject::CommandMoveTo(const FVector3f& InGoal)
{
	if (!CanMove() || bDead)
	{
		return;
	}
	if (bJetAI)
	{
		// A parked aircraft takes off first; one that is taking off or landing remembers the order.
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::Move;
		Cmd.Pos = InGoal;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	MoveOutOfWayOf = GenInvalidObjectID;
	StartPath(InGoal, false);
}

void FGenObject::CommandFollowPathExact(const TArray<FVector2f>& Points)
{
	// AIFollowWaypointPathExactState: one path through every point, straight from each to the next.
	if (bDead || !CanMove() || Points.Num() == 0)
	{
		return;
	}
	if (bFlies || bJetAI)
	{
		CommandFollowPath(Points);
		return;
	}
	StopDocking();
	ClearAttack();
	CommandStop();
	AIMode = EGenAIMode::None;
	const FVector2f& Last = Points.Last();
	StartDirectMove(FVector3f(Last.X, Last.Y, Logic->GetGroundHeight(Last.X, Last.Y)));
	Path = Points;
	PathIndex = 0;
	PathLayers.Reset();
}

void FGenObject::StartDirectMove(const FVector3f& InGoal)
{
	Path.Reset();
	PathLayers.Reset();
	Path.Add(FVector2f(InGoal.X, InGoal.Y));
	PathIndex = 0;
	PathStart = FVector2f(Position.X, Position.Y);
	Goal = FVector3f(InGoal.X, InGoal.Y, Logic->GetGroundHeight(InGoal.X, InGoal.Y));
	RequestedGoal = InGoal;
	PathExtraDistance = 0.f;
	bWaypointLeg = false;
	if (!bFlies)
	{
		Logic->GetPathfinder().UpdateUnitGoal(ID, Path[0], Template->GetBoundingCircleRadius());
	}
	bMoving = true;
	bDirectMove = true;
	StuckFrames = 0;
	WaitFrames = 0;
	ProgressPos = FVector2f(Position.X, Position.Y);
	ProgressAngle = Orientation;
	BlockedFrames = 0;
	bBlocked = false;
	bBlockedAndStuck = false;
	bSidestepPath = false;
	bLocoBraking = false;
	BrakingFactor = 1.f;
}

bool FGenObject::StartPath(const FVector3f& InGoal, bool bAroundUnits, const TFunction<bool(const FVector2f&)>* Accept, bool bIntoUnit, bool bWaypoint)
{
	GEN_PERF_SCOPE("Obj.StartPath");
	// AIInternalMoveToState::onEnter: "Target to stop at the end of this path. This value will be overriden
	// by the FollowWaypoint ai state" (which has set it already when this is a leg of its route).
	if (!bWaypoint)
	{
		PathExtraDistance = 0.f;
	}
	bWaypointLeg = bWaypoint;
	if (bFlies)
	{
		// Aircraft need no path: they fly straight there (Pathfinder::findPath for an AIR-only locomotor set),
		// ending where no other aircraft hovers or is headed.
		FVector2f Spot(InGoal.X, InGoal.Y);
		Logic->AdjustAirDestination(*this, Spot);
		StartDirectMove(FVector3f(Spot.X, Spot.Y, InGoal.Z));
		RequestedGoal = InGoal;
		return true;
	}
	// Pathfinder::queueForPath: the frame's pathfinding is spent (or others are waiting their turn) -
	// the order stands, its path comes in a frame or two. (A request that comes with a test of its own
	// is put off by whoever makes it, who can make it again: StartApproach, MoveToFreeGround.)
	if (!Accept && Logic->ShouldQueuePath())
	{
		FPendingPath Request;
		Request.Kind = FPendingPath::Plain;
		Request.Goal = InGoal;
		Request.bAroundUnits = bAroundUnits;
		Request.bIntoUnit = bIntoUnit;
		Request.bWaypoint = bWaypoint;
		return DeferPath(Request);
	}
	PendingPath.Kind = FPendingPath::None; // (this one is worked out now, whatever was waiting)
	FGenPathfinder& PF = Logic->GetPathfinder();
	const float Footprint = Template->GetBoundingCircleRadius();
	const FVector2f From(Position.X, Position.Y);

	// TerrainLogic::getLayerForDestination: a goal at the height of a bridge's deck is on that bridge.
	const int32 WantLayer = PF.GetLayerForDestination(InGoal, Logic->GetGroundHeight(InGoal.X, InGoal.Y));

	// Pathfinder::adjustDestination: end the move on ground no other unit has claimed or is parked on.
	FVector2f Dest(InGoal.X, InGoal.Y);
	if (bIntoUnit || bWaypoint)
	{
		// (setAdjustsDestination(false): "We want to run into the target"; "We are in the middle of a
		// path, so don't set the final goal location yet.")
	}
	else if (!Accept || !PF.AdjustUnitDestination(ID, Footprint, GetPathSurfaces(), Dest, *Accept, WantLayer, IgnoreObstacle))
	{
		Dest = FVector2f(InGoal.X, InGoal.Y);
		PF.AdjustUnitDestination(ID, Footprint, GetPathSurfaces(), Dest, WantLayer, IgnoreObstacle);
	}

	// (Pathfinder::queueForPath takes the id we are allowed to cross with the request: "setIgnoreObstacleID(
	// getIgnoredObstacleID() )" - the object we are going into, or the airfield it is parked on.)
	TArray<FVector2f> NewPath;
	TArray<uint8> NewLayers;
	if (bAroundUnits)
	{
		PF.FindPathAroundUnits(ID, Footprint, From, Dest, GetPathSurfaces(), NewPath, Layer, WantLayer, &NewLayers, IgnoreObstacle);
	}
	if (NewPath.Num() == 0)
	{
		PF.FindPath(From, Dest, GetPathSurfaces(), NewPath, Layer, WantLayer, &NewLayers, IgnoreObstacle);
	}
	if (NewPath.Num() == 0)
	{
		return false;
	}
	Path = MoveTemp(NewPath);
	PathLayers = MoveTemp(NewLayers);
	PathIndex = 0;
	PathStart = From;
	const FVector2f End = Path.Last();
	const int32 EndLayer = PathLayers.Num() == Path.Num() ? PathLayers.Last() : 0;
	Goal = FVector3f(End.X, End.Y, PF.GetLayerHeight(End.X, End.Y, EndLayer, Logic->GetGroundHeight(End.X, End.Y)));
	RequestedGoal = InGoal;
	if (!bWaypoint)
	{
		PF.UpdateUnitGoal(ID, End, Footprint, EndLayer); // (a point on the way is nobody's to keep)
	}
	bMoving = true;
	bDirectMove = false;
	StuckFrames = 0;
	WaitFrames = 0;
	ProgressPos = From;
	ProgressAngle = Orientation;
	BlockedFrames = 0;
	bBlocked = false;
	bBlockedAndStuck = false;
	bSidestepPath = false;
	// Locomotor::startMove
	bLocoBraking = false;
	BrakingFactor = 1.f;
	return true;
}

bool FGenObject::DeferPath(const FPendingPath& Request)
{
	PendingPath = Request;
	if (Request.Kind == FPendingPath::Plain)
	{
		RequestedGoal = Request.Goal;
	}
	// Busy from now on: on the way it was on if it had one, else waiting where it stands.
	bMoving = true;
	if (!bPathQueued)
	{
		bPathQueued = true;
		Logic->QueueForPath(ID);
	}
	return true;
}

bool FGenObject::DoPendingPath()
{
	bPathQueued = false;
	const FPendingPath Request = PendingPath;
	PendingPath.Kind = FPendingPath::None;
	// (Stopped, killed, picked up or given a path in the meantime: nothing left to work out.)
	if (Request.Kind == FPendingPath::None || bDead || bPendingDestroy || ContainedBy != GenInvalidObjectID || !bMoving || !CanMove())
	{
		return false;
	}
	bool bStarted = false;
	switch (Request.Kind)
	{
	case FPendingPath::Plain:
	{
		const uint32 KeepStuckFrames = StuckFrames, KeepWaitFrames = WaitFrames, KeepBlockedFrames = BlockedFrames;
		const FVector2f KeepProgressPos = ProgressPos;
		const float KeepProgressAngle = ProgressAngle;
		bStarted = StartPath(Request.Goal, Request.bAroundUnits, nullptr, Request.bIntoUnit, Request.bWaypoint);
		if (bStarted && Request.bAroundUnits)
		{
			// (As UpdateAI does for the way round a parked unit when it gets it at once.)
			StuckFrames = KeepStuckFrames;
			WaitFrames = KeepWaitFrames;
			ProgressPos = KeepProgressPos;
			ProgressAngle = KeepProgressAngle;
			BlockedFrames = KeepBlockedFrames;
			bSidestepPath = true;
		}
		break;
	}
	case FPendingPath::Approach:
	{
		const FGenObject* Target = Logic->FindObject(Request.Target);
		bStarted = Target && !Target->IsDead() && StartApproach(*Target, Request.Range, Request.MinRange);
		break;
	}
	case FPendingPath::ApproachPos:
		bStarted = StartApproachPos(Request.Goal, Request.Range, Request.MinRange);
		break;
	case FPendingPath::FreeGround:
		bMoving = Path.Num() > 0;
		MoveToFreeGround();
		bStarted = bMoving;
		break;
	case FPendingPath::MakeWay:
		// (Making way failed: on with the way it was on, if it had one.)
		bMoving = Path.Num() > 0;
		bStarted = StartMakeWay(Request.Target, Request.Along, Request.bResume, Request.ResumeGoal) || bMoving;
		break;
	default:
		break;
	}
	if (!bStarted && !IsWaitingForPath())
	{
		// No way there, or nothing to go to any more: it stays where it is.
		bMoving = false;
		Path.Reset();
	}
	return true;
}

void FGenObject::CommandStop()
{
	PendingPath.Kind = FPendingPath::None;
	IgnoreObstacle = GenInvalidObjectID;
	bMoving = false;
	Path.Reset();
	bLocoBraking = false;
	bMovingOutOfWay = false;
	bHasResumeGoal = false;
	if (Locomotor && !bDead && !bFlies)
	{
		// Where we stand is now our destination.
		Logic->GetPathfinder().UpdateUnitGoal(ID, FVector2f(Position.X, Position.Y), Template->GetBoundingCircleRadius(), Layer);
	}
}

// ---- update -------------------------------------------------------------------------------------

void FGenObject::Update(uint32 Frame)
{
	if (bPendingDestroy)
	{
		return;
	}
	if (Stream && UpdateProjectileStream())
	{
		bPendingDestroy = true;
		Logic->ProjectileStreams.Remove(ID);
		return;
	}
	if (Template->BoneFX.Num() > 0)
	{
		UpdateBoneFX(Frame); // (dead or flying too: a tower's rubble, a flame in the air)
	}
	if (Template->bFloatLifts)
	{
		// FloatUpdate::update, Enabled: "snap to the water surface". (Its rocking is the drawing's.)
		const FGenMapData* Map = Logic->GetMap();
		float WaterZ = 0.f;
		if (Map && Map->IsUnderwater(Position.X, Position.Y, &WaterZ))
		{
			SetPosition(FVector3f(Position.X, Position.Y, WaterZ));
		}
	}
	if (Projectile.bActive)
	{
		UpdateProjectile(Frame);
		return;
	}
	if (bDead)
	{
		UpdateDying(Frame);
		return;
	}
	if (HijackedVehicle != GenInvalidObjectID)
	{
		UpdateHijackerRide(Frame); // a hijacker driving what he took
		return;
	}
	// Object::setDisabledUntil / clearDisabled: a building or vehicle that loses its power (a power cut, an EMP,
	// a hacker) is heard going down - unless it already was for another of those reasons - and coming back.
	const bool bPowerDown = bUnderpowered || Frame < FMath::Max(EMPUntilFrame, HackedUntilFrame);
	if (bPowerDown != bPowerDownHeard)
	{
		bPowerDownHeard = bPowerDown;
		const TCHAR* Misc = IsKindOf(TEXT("STRUCTURE")) ? (bPowerDown ? TEXT("BuildingDisabled") : TEXT("BuildingReenabled"))
			: IsKindOf(TEXT("VEHICLE")) ? (bPowerDown ? TEXT("VehicleDisabled") : TEXT("VehicleReenabled")) : nullptr;
		if (Misc)
		{
			Logic->PlaySoundAt(FGenAudioData::Get().GetMisc(Misc), *this);
		}
	}
	if (Template->bIsDozer && Template->bHasMineClearingSet)
	{
		UpdateMineClearingDetail();
	}
	if (Template->DemoTrap.bValid)
	{
		UpdateDemoTrap(Frame);
		if (bDead || bPendingDestroy)
		{
			return;
		}
	}
	// TempWeaponBonusHelper: a bonus given for a while runs out.
	if (TempBonusEndFrame != 0 && Frame >= TempBonusEndFrame)
	{
		BonusConditions.Remove(TempBonus);
		TempBonus.Reset();
		TempBonusEndFrame = 0;
	}
	if (Template->WeaponBonusPulse.bValid && Frame >= NextBonusPulseFrame)
	{
		UpdateWeaponBonusPulse(Frame);
	}
	if (Template->Propaganda.bValid)
	{
		UpdatePropagandaTower(Frame); // (riding on an Overlord too)
	}
	if (MineState)
	{
		UpdateMine(Frame);
		if (bPendingDestroy || bDead)
		{
			return;
		}
	}
	if (Template->GenerateMinefield.bValid)
	{
		UpdateMinefieldUpgrade();
	}
	if (Template->LeafletDrop.bValid)
	{
		UpdateLeafletDrop(Frame);
	}
	// Stealth and detection go on whatever else the object is doing (inside a garrison too).
	if (Template->Stealth.bValid)
	{
		UpdateStealth(Frame);
	}
	if (Template->Detector.bValid)
	{
		UpdateStealthDetector(Frame);
	}
	if (Template->GrantStealth.bValid && !UpdateGrantStealth())
	{
		return;
	}
	if (bHeldByParachute)
	{
		if (bRappelling)
		{
			UpdateRappel(Frame); // ... or sliding down a rope
		}
		return; // hanging under a parachute: the parachute moves us
	}
	if (ContainedBy != GenInvalidObjectID)
	{
		if (HackState != EHackState::None)
		{
			UpdateHackInternet(Frame); // a hacker in an Internet Center works on
		}
		UpdateContained(Frame); // rides along; shoots out if the container lets it
		return;
	}
	if (LifetimeDieFrame != 0 && Frame >= LifetimeDieFrame)
	{
		Kill();
		return;
	}
	if (ToppleState == EToppleState::Falling || TreeSinkFramesLeft > 0)
	{
		UpdateTopple(Frame);
		if (bDead || bPendingDestroy)
		{
			return;
		}
	}
	if (bStickyActive)
	{
		UpdateStickyBomb(Frame);
		if (bPendingDestroy)
		{
			return;
		}
	}
	if (AbilityObjects.Num() > 0)
	{
		PruneAbilityObjects(); // SpecialAbilityUpdate::validateSpecialObjects
	}
	if (Template->DynamicShroud.bValid)
	{
		UpdateDynamicShroud(Frame);
	}
	if (RadarExtendDoneFrame != 0)
	{
		UpdateRadarDish(Frame);
	}
	if (SpyVisions.Num() > 0)
	{
		UpdateSpyVision(Frame);
	}
	if (SellFrame != 0)
	{
		// BuildAssistant::update, the sell list: the scaffolding gets a second and a half to go up, the
		// building sinks into the ground over three (its construction percent running backwards), the
		// scaffolding gets as long again to come down - then the owner has the money and it is gone.
		const float ScaffoldFrames = float(GenLogic::GetLogicFPS()) * 1.5f, SellFrames = float(GenLogic::GetLogicFPS()) * 3.f;
		if (float(Frame - SellFrame) >= ScaffoldFrames)
		{
			const float Was = ConstructionPercent;
			ConstructionPercent -= 100.f / SellFrames;
			if (Was > 0.f && ConstructionPercent <= 0.f)
			{
				static const int32 Partially = GenConditions::Index(TEXT("PARTIALLY_CONSTRUCTED")), Actively = GenConditions::Index(TEXT("ACTIVELY_BEING_CONSTRUCTED")),
					Sold = GenConditions::Index(TEXT("SOLD"));
				SetModelCondition(Partially, false);
				SetModelCondition(Actively, false);
				SetModelCondition(Sold, true);
				SetDrawAnimDuration(FMath::RoundToInt32(SellFrames * 0.5f));
			}
		}
		if (ConstructionPercent <= -50.f)
		{
			Logic->FinishSell(*this);
			bPendingDestroy = true;
		}
		return;
	}
	if (bUnderConstruction)
	{
		return; // a construction site does nothing by itself; without a dozer at work it just waits
	}
	if (bOverchargeActive)
	{
		// OverchargeBehavior::update: the overcharge eats a share of the reactor's health every second, and stops
		// below the allowed level ("GUI:OverchargeExhausted").
		AttemptDamage(MaxHealth * Template->OverchargeDrainPerSecond / float(GenLogic::GetLogicFPS()), TEXT("PENALTY"), TEXT("NORMAL"), ID);
		if (!bDead && Health < MaxHealth * Template->OverchargeMinHealth)
		{
			SetOvercharge(false);
			if (Owner == Logic->GetLocalPlayer())
			{
				FString Message = FGenCommandData::Get().GetText(TEXT("GUI:OverchargeExhausted"));
				Logic->AddScreenMessage(Message.IsEmpty() || Message.StartsWith(TEXT("GUI:")) ? FString(TEXT("Overcharge exhausted")) : Message);
				Logic->CreateRadarEvent(Position, EGenRadarEvent::Information);
			}
		}
	}
	if (Template->BattlePlan.bValid && !IsDisabled())
	{
		UpdateBattlePlan(Frame);
	}
	if (Template->Spawner.bValid)
	{
		UpdateSpawner(Frame);
	}
	if (bPoweredKind)
	{
		// Player::onPowerBrownOutChange: what needs power stands idle while its owner is short of it.
		const bool bShort = Logic->IsBrownOut(Owner);
		if (bShort != bUnderpowered)
		{
			bUnderpowered = bShort; // (heard at the top of the next update)
			if (bShort)
			{
				ClearAttack();
			}
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] #%u %s %s"), Frame, ID, *Template->Name, bShort ? TEXT("has no power") : TEXT("has power again"));
		}
	}
	if (bSpawnPending)
	{
		// SpawnBehavior: a finished building's own units come out (a supply center's first gatherer).
		bSpawnPending = false;
		Logic->SpawnBuildingUnits(*this);
	}
	if (Contain)
	{
		Logic->UpdateContain(*this);
	}
	if (bAutoHealActive)
	{
		UpdateAutoHeal(Frame);
	}
	if (PoisonStopFrame != 0)
	{
		UpdatePoison(Frame);
		if (bDead || bPendingDestroy)
		{
			return;
		}
	}
	if (Template->bBaseRegenerate && Frame >= BaseRegenFrame)
	{
		UpdateBaseRegenerate(Frame);
	}
	if (Template->Horde.bValid && Frame >= HordeNextFrame)
	{
		UpdateHorde(Frame);
	}
	if (Train)
	{
		UpdateTrain(Frame);
		if (bDead || bPendingDestroy)
		{
			return;
		}
	}
	if (Template->FireOCLAfterCooldown.Num() > 0)
	{
		UpdateFireOCLAfterCooldown(Frame); // (before this frame's shots: "getLastShotFrame() == now - 1")
	}
	if (Template->AutoFindHealing.bValid)
	{
		UpdateAutoFindHealing(Frame);
	}
	if (Template->CleanupHazard.bValid)
	{
		UpdateCleanupHazard(Frame);
	}
	if (Template->bWanderAI && CanMove() && !bMoving && IsAIIdle())
	{
		// WanderAIUpdate::update: "if (isIdle())" - off a little way, always the same way round.
		FRandomStream& Rand = Logic->GetRandom();
		const float DX = float(Rand.RandRange(5, 50)), DY = float(Rand.RandRange(5, 50));
		CommandMoveTo(FVector3f(Position.X + DX, Position.Y + DY, Position.Z));
	}
	if (BusState != 0)
	{
		UpdateBattleBus(Frame);
		if (BusState == 1 || bDead || bPendingDestroy)
		{
			return; // in the air: nothing else of it works until it is down
		}
	}
	if (Template->RebuildHole.bValid)
	{
		UpdateRebuildHole(Frame);
		if (bPendingDestroy)
		{
			return;
		}
	}
	if (Template->AssaultTransport.bValid)
	{
		UpdateAssaultTransport(Frame);
	}
	if (Template->Deploy.bValid)
	{
		UpdateDeploy(Frame);
	}
	if (bCountermeasuresActive)
	{
		UpdateCountermeasures(Frame);
	}
	if (FlameStatus == EFlameStatus::Aflame)
	{
		UpdateFlammable(Frame);
		if (bDead)
		{
			return;
		}
	}
	if (AreaWeapons.Num() > 0)
	{
		UpdateAreaWeapons(Frame);
	}
	if ((Template->bHasDeletion || Template->bHeightDie || bFalling || ParachuteRider != GenInvalidObjectID) && !UpdatePayload(Frame))
	{
		return; // deleted, gone off, or a parachute on its way down
	}
	if (CombatDrop && CombatDrop->bDropping)
	{
		// ChinookCombatDropState: DISABLED_HELD - it hangs where it is until the last soldier is down,
		// and whatever it is told in the meantime waits.
		UpdateCombatDrop(Frame);
		if (CombatDrop && CombatDrop->bDropping)
		{
			MaintainPosition(Frame);
			UpdateFlightPhysics(Frame);
			return;
		}
	}
	// Object::setDisabled / clearDisabled, "pauseAllSpecialPowers": while it is disabled its own powers'
	// clocks stand still (SpecialPowerModule::pauseCountdown) - they are ready that much later.
	if (PowerReady.Num() > 0)
	{
		const bool bDisabledNow = IsDisabled();
		if (bDisabledNow != bPowersPaused)
		{
			if (bDisabledNow)
			{
				PowersPausedFrame = Frame;
			}
			else
			{
				for (uint32& Ready : PowerReady)
				{
					Ready += Frame - PowersPausedFrame;
				}
			}
			bPowersPaused = bDisabledNow;
		}
	}
	if (Template->ParticleCannon.bValid)
	{
		UpdateParticleCannon(Frame); // (also without power: that is what cuts its beam short)
	}
	if (IsDisabled())
	{
		// EMP, hacked, or without the power it needs: nothing works. Whatever was rolling comes to rest.
		if (CanMove())
		{
			bMoving = false;
			Path.Reset();
			if (IsUsingAirLoco()) { UpdateFlightPhysics(Frame); } else { UpdatePhysics(Frame); }
			SetModelCondition(GenConditions::Moving(), false);
		}
		// A drone whose master is gone is "unmanned" and has no lift left: it is dead when it comes down
		// ("added special case code in physics falling to ensure death").
		if (Slave && Slave->CrashFrame != 0 && !bDead && (Frame >= Slave->CrashFrame || Position.Z - Logic->GetGroundHeight(Position.X, Position.Y) <= 1.f))
		{
			Kill();
		}
		return;
	}
	if (Dock)
	{
		UpdateDock(Frame);
	}
	if (Parking)
	{
		Parking->Update(Frame);
	}
	if (Template->bHasProduction)
	{
		UpdateProduction(Frame); // (with nothing in the queue too: a door may still be closing)
	}
	if (Template->MissileLauncher.bValid)
	{
		UpdateMissileLauncherBuilding(Frame);
	}
	bJetDriving = false;
	if (bJetAI)
	{
		UpdateJet(Frame); // taxiing, takeoff, landing, reloading; may take over this frame's movement
		if (bDead)
		{
			return; // ran out of fuel with nowhere to land
		}
	}
	if (Delivery && !UpdateDelivery(Frame))
	{
		return; // a power's aircraft that hasn't started yet, or has just left
	}
	if (Gunship && !UpdateGunship(Frame))
	{
		return; // off the map again
	}
	if (Slave)
	{
		UpdateSlaved(Frame); // a drone looks at what its master is doing
		if (bDead || bPendingDestroy)
		{
			return;
		}
	}
	if (Template->AutoDeposit.bValid)
	{
		UpdateAutoDeposit(Frame);
	}
	if (Template->OCLUpdate.bValid)
	{
		UpdateOCL(Frame);
	}
	if (Template->Crate.IsValid())
	{
		UpdateCrate(Frame);
		if (bPendingDestroy)
		{
			return;
		}
	}
	if (HackState != EHackState::None)
	{
		UpdateHackInternet(Frame);
	}
	// (A hacker packing up its laptop does nothing else until that is done.)
	const bool bPackingUp = HackState == EHackState::Packing;
	if (AbilityIndex != INDEX_NONE && !bPackingUp)
	{
		UpdateAbility(Frame);
	}
	if (!HuntButton.IsEmpty())
	{
		UpdateButtonHunt(Frame);
	}
	if (AIMode != EGenAIMode::None)
	{
		UpdateAIMode(Frame);
	}
	// (A weapon that does no damage of its own - the SCUD storm's - still has to be fired, and its
	// firing poses taken back afterwards.)
	if (Template->PointDefenseLasers.Num() > 0)
	{
		UpdatePointDefense();
	}
	if (HasWeapons() || bAttackingPosition || bCombatPosesSet)
	{
		UpdateCombat(Frame);
	}
	if (CanMove())
	{
		if (Template->bChinookAI && Contain)
		{
			UpdateChinookFlight(Frame);
		}
		if (Template->PilotScanFrames > 0 && Frame >= PilotNextScanFrame)
		{
			UpdatePilotFindVehicle(Frame);
		}
		// (MinefieldBehavior::onCollide, from the mover's side: only once there are mines in the game.)
		if (Logic->HasMines() && !bFlies && ((Frame + ID) & 1) == 1 && (FMath::Abs(Velocity.X) > 0.01f || FMath::Abs(Velocity.Y) > 0.01f))
		{
			DetonateMinesInTheWay(Frame);
			if (bDead || bPendingDestroy)
			{
				return;
			}
		}
		// (ToppleUpdate::onCollide / W3DTreeBuffer::unitMoved: "if( other->getCrusherLevel() > 1 )")
		if (Template->CrusherLevel > 1 && ((Frame + ID) & 1) == 0 && (FMath::Abs(Velocity.X) > 0.01f || FMath::Abs(Velocity.Y) > 0.01f))
		{
			ToppleWhatIsInTheWay();
		}
		if (EnterTarget != GenInvalidObjectID)
		{
			UpdateEnter(Frame);
		}
		if (bHasExitRally && !bMoving)
		{
			// Out of the factory door: now on to the rally point.
			bHasExitRally = false;
			CommandMoveTo(ExitRally);
		}
		if (BuildTarget != GenInvalidObjectID || RepairTarget != GenInvalidObjectID)
		{
			UpdateDozer(Frame);
		}
		else if (Template->bIsDozer)
		{
			UpdateDozerBored(Frame);
		}
		if (Template->bIsSupplyTruck)
		{
			UpdateSupplyTruck(Frame);
		}
		if (DockTarget != GenInvalidObjectID)
		{
			UpdateDocking(Frame);
		}
		if (!bJetDriving && !bPackingUp)
		{
			UpdateAI(Frame);
		}
		// (An aircraft on the ground at its airfield rolls on its taxi locomotor, like a ground unit.)
		if (IsUsingAirLoco())
		{
			UpdateFlightPhysics(Frame);
		}
		else
		{
			UpdatePhysics(Frame);
		}
		// A unit held up behind another one stops its move animation after a quarter second.
		SetModelCondition(GenConditions::Moving(), bMoving && !bPackingUp && BlockedFrames <= uint32(GenLogic::GetLogicFPS() / 4)
			&& !(Path.Num() == 0 && IsWaitingForPath()) && !IsDeployBusy());
		if (Template->AnimationSteeringFrames >= 0)
		{
			UpdateAnimationSteering(Frame);
		}
		if (LocoSet.Num() > 1)
		{
			// AIInternalMoveToState::update: on a cliff, CLIMBING - or RAPPELLING while backing down it ("to make col.
			// burton's monkey ass not slide down the cliffs backwards"). Both stay when it stops on the face.
			static const int32 ClimbingBit = GenConditions::Index(TEXT("CLIMBING"));
			static const int32 RappellingBit = GenConditions::Index(TEXT("RAPPELLING"));
			const bool bOnFace = bLocoClimbing && Locomotor->Appearance == EGenLocoAppearance::Climber;
			SetModelCondition(ClimbingBit, bOnFace && !bLocoBackwards);
			SetModelCondition(RappellingBit, bOnFace && bLocoBackwards);
		}
	}
	else if (bFreePhysics)
	{
		UpdateFreePhysics(Frame);
	}
	if (Template->MobMember.bValid && !bDead && !bPendingDestroy)
	{
		UpdateMobMember(Frame); // (MobMemberSlavedUpdate comes after the AI and its weapons)
	}
}

void FGenObject::SetModelCondition(int32 Bit, bool bOn)
{
	if (ModelConditions.Test(Bit) != bOn)
	{
		ModelConditions.Set(Bit, bOn);
		++ConditionVersion;
	}
}

float FGenObject::PathDistanceFrom(int32 Index) const
{
	float D = 0.f;
	FVector2f Prev(Position.X, Position.Y);
	for (int32 i = Index; i < Path.Num(); ++i)
	{
		D += FVector2f::Distance(Prev, Path[i]);
		Prev = Path[i];
	}
	return D;
}

void FGenObject::UpdateAI(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Move");
	// AIUpdateInterface::doLocomotor: collisions found after the last frame say whether we are blocked,
	// and how fast we may go without running into the unit ahead.
	BlockedFrames = bBlocked ? BlockedFrames + 1 : 0;
	bBlocked = false;
	const float MaxBlockedSpeed = CurMaxBlockedSpeed;
	CurMaxBlockedSpeed = 99999.f;
	const bool bStuck = bBlockedAndStuck;
	bBlockedAndStuck = false;

	SteerBlocker = GenInvalidObjectID;
	if (IsDeployBusy())
	{
		// DeployStyleAIUpdate: "setTemporaryState( AI_BUSY )" and "setLocomotorGoalNone" - the move waits, it isn't given up.
		Velocity.X = Velocity.Y = 0.f;
		return;
	}
	if (LocoSet.Num() > 1 && !bFlies)
	{
		ChooseGoodLocomotorFromCurrentSet(); // doLocomotor
	}
	if (!bMoving || Path.Num() == 0)
	{
		SteerSide = 0;
		SteerHold = 0;
		if (bFlies)
		{
			MaintainPosition(Frame); // hovering takes work even when going nowhere
		}
		return;
	}

	// AIInternalMoveToState: blocked by a unit that isn't going anywhere, or blocked for two seconds:
	// find a new path that goes around the parked units.
	const uint32 FPS = uint32(GenLogic::GetLogicFPS());
	if ((bStuck || BlockedFrames > 2 * FPS) && Frame >= NextBlockedRepathFrame)
	{
		NextBlockedRepathFrame = Frame + FPS;
		const FVector3f Dest = RequestedGoal;
		const uint32 KeepStuckFrames = StuckFrames, KeepWaitFrames = WaitFrames, KeepBlockedFrames = BlockedFrames;
		const FVector2f KeepProgressPos = ProgressPos;
		const float KeepProgressAngle = ProgressAngle;
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [steer f%u] #%u %s paths round parked units (%s, blocked %u)"), Frame, ID, *Template->Name,
			bStuck ? TEXT("held up by a parked unit") : TEXT("held up too long"), BlockedFrames);
		if (StartPath(Dest, true, nullptr, false, bWaypointLeg))
		{
			// Re-pathing is not progress, and we are still being held up: keep the counters running
			// (after a second of this, units that merely cross each other are let through).
			StuckFrames = KeepStuckFrames;
			WaitFrames = KeepWaitFrames;
			ProgressPos = KeepProgressPos;
			ProgressAngle = KeepProgressAngle;
			BlockedFrames = KeepBlockedFrames;
			bSidestepPath = true;
		}
	}
	const FVector2f Pos2(Position.X, Position.Y);

	// Advance through intermediate waypoints once we're close (the original projects ahead on the path).
	// The first waypoint of a path around a parked unit is the sidestep itself: really go there.
	// (A unit that steered round somebody may pass a point wide of it: once past it, with the next one in
	// plain sight, it doesn't turn back for it.)
	while (PathIndex < Path.Num() - 1
		&& (FVector2f::Distance(Pos2, Path[PathIndex]) < CellSize * ((bSidestepPath && PathIndex == 0) ? 0.5f : 1.5f)
			|| (!(bSidestepPath && PathIndex == 0) && FVector2f::DistSquared(Pos2, Path[PathIndex]) < Sqr(5.f * CellSize)
				&& FVector2f::DotProduct(Pos2 - Path[PathIndex], Path[PathIndex + 1] - Path[PathIndex]) > 0.f
				&& Logic->GetPathfinder().IsLineClear(Pos2, Path[PathIndex + 1], GetPathSurfaces(), Layer))))
	{
		++PathIndex;
	}

	// A plane measures what is left along its line of flight: it cannot stop on the goal, it flies past it.
	const bool bPlane = HasMinSpeed() && IsUsingAirLoco();
	// AIUpdateInterface::getTreatAsAircraftForLocoDistToGoal: "We are following a waypoint or other multiple
	// point path, so use the 'easy' success criteria" - a point on the way is reached once abeam of it.
	const bool bOnRoute = PathExtraDistance > 1.f;
	const float DistToGoal = (bPlane || bOnRoute) ? FlightDistToGoal() : PathDistanceFrom(PathIndex);
	const FVector2f Target = Path[PathIndex];
	const float CloseEnough = FMath::Max(Locomotor->CloseEnoughDist, 1.f);

	// Arrived: AIUpdate's move state ends when within CloseEnoughDist and stopped (or nearly).
	if (PathIndex == Path.Num() - 1 && DistToGoal <= CloseEnough)
	{
		if (bOnRoute && !bPlane && AIMode == EGenAIMode::FollowPath && TargetID == GenInvalidObjectID)
		{
			// AIFollowPathState::update: "if move to has finished, move to next point on path" - without stopping.
			Path.Reset();
			if (StartNextRouteLeg())
			{
				return;
			}
		}
		bMoving = false;
		Path.Reset();
		if (bPlane)
		{
			return; // keeps flying; with nowhere to go it circles here
		}
		Velocity.X = Velocity.Y = 0.f;
		bStatusBraking = false;
		if (IsWaitingForPath())
		{
			bMoving = true; // (the end of the old way; the new one is being worked out)
			return;
		}
		if (bMovingOutOfWay)
		{
			// Stepped aside: carry on with what we were doing before.
			bMovingOutOfWay = false;
			MoveOutOfWayOf = GenInvalidObjectID;
			if (bHasResumeGoal)
			{
				bHasResumeGoal = false;
				const FVector3f Resume = ResumeGoal;
				StartPath(Resume, false);
			}
		}
		return;
	}

	FVector3f TargetPos(Target.X, Target.Y, Logic->GetGroundHeight(Target.X, Target.Y));
	// doLocomotor: blocked, we go no faster than the unit ahead lets us - and, the locomotor's *blocked,
	// stop and turn on the spot if that is slower than we are going.
	const bool bCapped = BlockedFrames > 0 && MaxBlockedSpeed < Locomotor->Speed;
	float DesiredSpeed = bCapped ? MaxBlockedSpeed : Locomotor->Speed;
	if (FormationID != 0 && FormationSpeed > 0.f && TargetID == GenInvalidObjectID)
	{
		// AIMoveToState / AIFollowPathState::onEnter: a formation moves at its group's speed (its slowest member's).
		DesiredSpeed = FMath::Min(DesiredSpeed, FormationSpeed);
	}
	bool bWait = bCapped;
	// Steering (beyond the original): the way round whoever is near, or the pace to keep behind them. Not on
	// the last cell of the move - that ground is ours ("If we are about to arrive, don't get stuck").
	const float DistToTarget = FVector2f::Distance(Pos2, Target);
	if (UsesSteering() && !bPlane && !(PathIndex == Path.Num() - 1 && DistToTarget < CellSize))
	{
		const FSteerChoice Choice = Steer(Target, DistToTarget, DistToGoal + PathExtraDistance, DesiredSpeed, Frame);
		if (Choice.bSteered)
		{
			// Aim a little way out along the chosen way (no further than the path goes, or the locomotor brakes).
			const float Ahead = FMath::Min(FMath::Clamp(DistToTarget, 2.f * CellSize, 4.f * CellSize), FMath::Max(DistToGoal + PathExtraDistance, 1.f));
			const FVector2f Aim = Pos2 + Choice.Dir * Ahead;
			TargetPos = FVector3f(Aim.X, Aim.Y, Logic->GetGroundHeight(Aim.X, Aim.Y));
			DesiredSpeed = FMath::Min(DesiredSpeed, Choice.Speed);
		}
		bWait = Choice.bWait;
		if (bWait)
		{
			bBlocked = true;
		}
	}
	// (doLocomotor: "onPathDistToGoal + getPathExtraDistance()" - no slowing down for a point on the way.)
	LocoMoveTowardsPosition(TargetPos, DistToGoal + PathExtraDistance, DesiredSpeed, Frame, bWait);
	// (-GenSteerTrace=<id>: what that one unit's locomotor made of it, every frame.)
	static const uint32 SteerTraceID = []() { uint32 V = 0; FParse::Value(FCommandLine::Get(), TEXT("GenSteerTrace="), V); return V; }();
	UE_CLOG(SteerTraceID == ID, LogGenerals, Log, TEXT("  [steertrace f%u] #%u move: aim (%.1f, %.1f) speed %.3f wait %d dist %.1f -> pos (%.2f, %.2f) ang %.3f vel %.3f accel %.4f turning %d braking %d/%d"),
		Frame, ID, TargetPos.X, TargetPos.Y, DesiredSpeed, bWait ? 1 : 0, DistToGoal + PathExtraDistance, Position.X, Position.Y, Orientation,
		FVector2f(Velocity.X, Velocity.Y).Size(), FVector2f(Accel.X, Accel.Y).Size(), int32(Turning), bStatusBraking ? 1 : 0, bLocoBraking ? 1 : 0);

	// Give up when the move gets nowhere for a long time: 6 seconds with nothing in the way (caught on the
	// ground), 20 waiting on other units. (Not in the original. What counts is ground covered - a braking unit
	// slides with no speed of its own - or turning round, and time spent waiting is not held against a unit
	// once it is free.)
	if (FVector2f::DistSquared(FVector2f(Position.X, Position.Y), ProgressPos) > Sqr(0.5f * CellSize)
		|| FMath::Abs(GenLogic::AngleDiff(Orientation, ProgressAngle)) > 0.5f)
	{
		ProgressPos = FVector2f(Position.X, Position.Y);
		ProgressAngle = Orientation;
		StuckFrames = 0;
		WaitFrames = 0;
	}
	else if (bWait || BlockedFrames > 0)
	{
		++WaitFrames;
	}
	else
	{
		++StuckFrames;
	}
	if (StuckFrames > 6 * FPS || WaitFrames > 20 * FPS)
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [steer f%u] #%u p%d %s gives up its move at (%.0f, %.0f): %s"), Frame, ID, Owner, *Template->Name,
			Position.X, Position.Y, StuckFrames > 6 * FPS ? TEXT("caught on the ground") : TEXT("waited 20 s on other units"));
		CommandStop();
	}
}

// ---- Locomotor ----------------------------------------------------------------------------------

float FGenObject::GetBraking() const
{
	return FMath::Max(Locomotor->Braking, 1e-4f);
}

float FGenObject::GetForwardSpeed2D() const
{
	const FVector2f Dir = GetDirection2D();
	return Velocity.X * Dir.X + Velocity.Y * Dir.Y;
}

void FGenObject::LocoMoveTowardsPosition(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame, bool bHeldUp)
{
	const float MaxSpeed = Locomotor->Speed;
	DesiredSpeed = FMath::Min(DesiredSpeed, MaxSpeed);
	bMaintainPosValid = false;
	PrevTurning = Turning;

	// Locomotor::locoUpdate_moveTowardsPosition with *blocked: held up by another unit and going faster than
	// we may, "stop if we are about to run into the blocking object" and turn on the spot towards the way on -
	// whatever the locomotor, so a wheeled vehicle too can turn out from behind what blocks it. (Not into
	// somebody else, though: beyond the original, a steered unit whose turn would swing it into a unit waits.)
	const FVector2f Vel2D(Velocity.X, Velocity.Y);
	if (bHeldUp && DesiredSpeed <= Vel2D.Size() && !(bFlies && IsAirborneTarget()))
	{
		ApplyMotiveForce(FVector3f::ZeroVector, Frame);
		const float Speed2D = Vel2D.Size();
		if (Speed2D > DesiredSpeed && Speed2D > 0.f)
		{
			// PhysicsBehavior::scrubVelocity2D
			Velocity.X *= DesiredSpeed / Speed2D;
			Velocity.Y *= DesiredSpeed / Speed2D;
		}
		Turning = 0;
		const float Rel = GenLogic::AngleDiff(FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X), Orientation);
		if (FMath::Abs(Rel) > 0.001f && (!UsesSteering() || IsPivotClear(Orientation + FMath::Clamp(Rel, -Locomotor->TurnRate, Locomotor->TurnRate))))
		{
			Turning = int8(RotateAroundLocoPivot(GoalPos, Locomotor->TurnRate));
		}
		if (bFlies)
		{
			HandleBehaviorZ(Frame);
		}
		return;
	}

	const float DistToStopAtMaxSpeed = (MaxSpeed / GetBraking()) * MaxSpeed / 2.f;
	if (OnPathDistToGoal > CellSize && OnPathDistToGoal > DistToStopAtMaxSpeed)
	{
		bLocoBraking = false;
		BrakingFactor = 1.f;
	}

	const float DX = GoalPos.X - Position.X;
	const float DY = GoalPos.Y - Position.Y;
	float Dist = FMath::Sqrt(DX * DX + DY * DY);
	if (Dist > OnPathDistToGoal)
	{
		if (Dist > 2.f * OnPathDistToGoal)
		{
			bLocoBraking = true;
		}
		OnPathDistToGoal = Dist;
	}

	// A zero motive force marks the object as driven by its locomotor.
	ApplyMotiveForce(FVector3f::ZeroVector, Frame);

	if (Locomotor->Appearance == EGenLocoAppearance::Wings)
	{
		bLocoBraking = false; // wings never "brake into place"
	}
	const bool bWasBraking = bStatusBraking;
	Turning = 0; // (setTurning(TURN_NONE): what turns below says so)
	// Up in the air only locomotors that say so can still push themselves along.
	if (Locomotor->bAllowAirborneMotiveForce || !bFlies || !IsAirborneTarget())
	{
		switch (Locomotor->Appearance)
		{
		case EGenLocoAppearance::TwoLegs: MoveLegs(GoalPos, OnPathDistToGoal, DesiredSpeed); break;
		case EGenLocoAppearance::Climber: MoveClimb(GoalPos, OnPathDistToGoal, DesiredSpeed); break;
		case EGenLocoAppearance::FourWheels:
		case EGenLocoAppearance::Motorcycle: MoveWheels(GoalPos, OnPathDistToGoal, DesiredSpeed, Frame); break;
		case EGenLocoAppearance::Treads: MoveTreads(GoalPos, OnPathDistToGoal, DesiredSpeed); break;
		default: MoveOther(GoalPos, OnPathDistToGoal, DesiredSpeed, Frame); break; // hover, wings, other (and thrust for now)
		}
	}
	if (bFlies)
	{
		HandleBehaviorZ(Frame);
	}

	// Objects that are braking don't follow the normal physics, so they end up at their destination exactly.
	bStatusBraking = bLocoBraking;
	if (bWasBraking && Dist > 0.001f)
	{
		const float MinVel = CellSize / float(GenLogic::GetLogicFPS());
		float Vel = FMath::Abs(GetForwardSpeed2D());
		Vel = FMath::Clamp(Vel, MinVel, Dist);
		Position.X += DX / Dist * Vel;
		Position.Y += DY / Dist * Vel;
	}
}

int32 FGenObject::RotateAroundLocoPivot(const FVector3f& GoalPos, float MaxTurnRate, float* OutRelAngle)
{
	float Offset = Locomotor->TurnPivotOffset;
	if (bLocoBraking)
	{
		Offset = 0.f;
	}
	int32 Turn = TurnNone;
	if (Offset != 0.f)
	{
		const float Radius = Template->GetBoundingCircleRadius();
		const FVector2f Dir = GetDirection2D();
		const FVector2f TurnPos = FVector2f(Position.X, Position.Y) + Dir * (Offset * Radius);
		const float DX = GoalPos.X - TurnPos.X, DY = GoalPos.Y - TurnPos.Y;
		if (FMath::Abs(DX) < 0.1f && FMath::Abs(DY) < 0.1f)
		{
			return TurnNone;
		}
		float Amount = KeepTurningWay(GenLogic::AngleDiff(FMath::Atan2(DY, DX), Orientation));
		if (OutRelAngle) { *OutRelAngle = Amount; }
		if (Amount > MaxTurnRate) { Amount = MaxTurnRate; Turn = TurnPositive; }
		else if (Amount < -MaxTurnRate) { Amount = -MaxTurnRate; Turn = TurnNegative; }
		// Rotate the object around the pivot point.
		const float C = FMath::Cos(Amount), S = FMath::Sin(Amount);
		const FVector2f Rel = FVector2f(Position.X, Position.Y) - TurnPos;
		Position.X = TurnPos.X + Rel.X * C - Rel.Y * S;
		Position.Y = TurnPos.Y + Rel.X * S + Rel.Y * C;
		SetOrientation(Orientation + Amount);
	}
	else
	{
		const float DesiredAngle = FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X);
		float Amount = KeepTurningWay(GenLogic::AngleDiff(DesiredAngle, Orientation));
		if (OutRelAngle) { *OutRelAngle = Amount; }
		if (Amount > MaxTurnRate) { Amount = MaxTurnRate; Turn = TurnPositive; }
		else if (Amount < -MaxTurnRate) { Amount = -MaxTurnRate; Turn = TurnNegative; }
		SetOrientation(Orientation + Amount);
	}
	return Turn;
}

float FGenObject::KeepTurningWay(float Amount) const
{
	// Beyond the original: with the goal almost straight behind, the turn under way goes on round the way it
	// was going (hardly further) instead of changing its mind every frame as the goal shifts across the back.
	if (PrevTurning != 0 && FMath::Abs(Amount) > 0.85f * PI && (Amount > 0.f ? 1 : -1) != PrevTurning)
	{
		return Amount > 0.f ? Amount - 2.f * PI : Amount + 2.f * PI;
	}
	return Amount;
}

void FGenObject::ApplyGoalSpeed(float GoalSpeed, float ActualSpeed, float MaxAccel, float InBrakingFactor, bool bBackwards)
{
	float SpeedDelta = GoalSpeed - ActualSpeed;
	if (bBackwards)
	{
		SpeedDelta = -GoalSpeed + ActualSpeed;
	}
	if (SpeedDelta == 0.f)
	{
		return;
	}
	float Acceleration;
	if (bBackwards)
	{
		Acceleration = SpeedDelta < 0.f ? -MaxAccel : InBrakingFactor * GetBraking();
	}
	else
	{
		Acceleration = SpeedDelta > 0.f ? MaxAccel : -InBrakingFactor * GetBraking();
	}
	float AccelForce = Mass * Acceleration;
	// Don't accelerate/brake more than necessary to reach the goal speed.
	const float MaxForceNeeded = Mass * SpeedDelta;
	if (FMath::Abs(AccelForce) > FMath::Abs(MaxForceNeeded))
	{
		AccelForce = MaxForceNeeded;
	}
	const FVector2f Dir = GetDirection2D();
	ApplyForce(FVector3f(AccelForce * Dir.X, AccelForce * Dir.Y, 0.f));
}

void FGenObject::MoveTreads(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed)
{
	DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed);
	float RelAngle = 0.f;
	Turning = int8(RotateAroundLocoPivot(GoalPos, Locomotor->TurnRate, &RelAngle));

	// The more we have to turn, the slower we go.
	const float AngleCoeff = FMath::Min(FMath::Abs(RelAngle) / (PI / 4.f), 1.f);
	const float DX = Position.X - GoalPos.X, DY = Position.Y - GoalPos.Y;
	float GoalSpeed = (1.f - AngleCoeff) * DesiredSpeed;

	const float ActualSpeed = GetForwardSpeed2D();
	const float SlowDownTime = ActualSpeed / GetBraking();
	const float SlowDownDist = (ActualSpeed / 1.5f) * SlowDownTime;

	if (Sqr(DX) + Sqr(DY) < Sqr(2.f * CellSize) && AngleCoeff > 0.05f)
	{
		GoalSpeed = ActualSpeed * 0.6f;
	}
	if (OnPathDistToGoal < SlowDownDist && !bLocoBraking)
	{
		bLocoBraking = true;
		BrakingFactor = 1.1f;
	}
	if (OnPathDistToGoal > CellSize && OnPathDistToGoal > 2.f * SlowDownDist)
	{
		bLocoBraking = false;
	}
	if (bLocoBraking)
	{
		BrakingFactor = FMath::Min(Sqr(SlowDownDist / FMath::Max(OnPathDistToGoal, 0.001f)), MaxBrakingFactor);
		if (SlowDownDist > OnPathDistToGoal) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking(), 0.f); }
		else if (SlowDownDist > OnPathDistToGoal * 0.75f) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking() / 2.f, 0.f); }
		else { GoalSpeed = ActualSpeed; }
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, Locomotor->Acceleration, BrakingFactor);
}

void FGenObject::MoveWheels(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed, uint32 Frame)
{
	const float MaxSpeed = Locomotor->Speed;
	const float MaxTurnRate = Locomotor->TurnRate;
	const float MaxAccel = Locomotor->Acceleration;
	DesiredSpeed = FMath::Min(DesiredSpeed, MaxSpeed);

	const float TurnSpeed = FMath::Max(Locomotor->MinTurnSpeed, MaxSpeed / 4.f);
	float DesiredAngle = FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X);
	float RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);
	bool bMoveBackwards = false;
	float ActualSpeed = GetForwardSpeed2D();
	bool bThreePoint = false;

	if (ActualSpeed == 0.f)
	{
		bLocoBackwards = false;
		if (Locomotor->bCanMoveBackwards && FMath::Abs(RelAngle) > PI / 2.f)
		{
			bLocoBackwards = true;
			bLocoThreePointTurn = OnPathDistToGoal > 5.f * Template->GeometryMajorRadius;
		}
	}
	if (bLocoBackwards)
	{
		if (FMath::Abs(RelAngle) < PI / 2.f)
		{
			bLocoBackwards = false;
		}
		else
		{
			bMoveBackwards = true;
			bLocoThreePointTurn = OnPathDistToGoal > 5.f * Template->GeometryMajorRadius;
			bThreePoint = bLocoThreePointTurn;
			if (!bThreePoint)
			{
				DesiredAngle = GenLogic::AngleDiff(DesiredAngle, PI);
				RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);
			}
		}
	}

	if (FMath::Abs(RelAngle) > PI / 20.f && DesiredSpeed > TurnSpeed)
	{
		DesiredSpeed = TurnSpeed;
	}
	float GoalSpeed = DesiredSpeed;
	if (bMoveBackwards)
	{
		ActualSpeed = -ActualSpeed;
	}
	const float SlowDownTime = ActualSpeed / GetBraking() + 1.f;
	const float SlowDownDist = (ActualSpeed / 1.5f) * SlowDownTime + ActualSpeed;
	const float EffectiveSlowDownDist = FMath::Max(SlowDownDist, CellSize);

	// If a sharp turn would take us onto impassable terrain, just turn in place.
	if (FMath::Abs(RelAngle) > PI / 12.f)
	{
		const float ProjectFrames = float(GenLogic::GetLogicFPS()) / 2.f;
		const float Distance = ProjectFrames * (GoalSpeed + ActualSpeed) / 2.f;
		const float TurnFactor = FMath::Min(((GoalSpeed + ActualSpeed) / 2.f) / TurnSpeed, 1.f);
		const float TurnAmount = ProjectFrames * TurnFactor * MaxTurnRate / 4.f;
		const float TargetAngle = Orientation + (RelAngle < 0.f ? -TurnAmount : TurnAmount);
		const FVector2f Offset(FMath::Cos(TargetAngle) * Distance, FMath::Sin(TargetAngle) * Distance);
		const FVector2f P(Position.X, Position.Y);
		FGenPathfinder& PF = Logic->GetPathfinder();
		if (!PF.IsPassable(P + Offset * 0.5f, GetPathSurfaces(), Layer) || !PF.IsPassable(P + Offset, GetPathSurfaces(), Layer))
		{
			Turning = int8(RotateAroundLocoPivot(GoalPos, Locomotor->TurnRate));
			ApplyMotiveForce(FVector3f::ZeroVector, Frame);
			return;
		}
	}

	if (OnPathDistToGoal < EffectiveSlowDownDist && !bLocoBraking)
	{
		bLocoBraking = true;
		BrakingFactor = 1.1f;
	}
	if (OnPathDistToGoal > CellSize && OnPathDistToGoal > 2.f * SlowDownDist)
	{
		bLocoBraking = false;
	}
	const float DonutDistance = 4.f * CellSize;
	if (OnPathDistToGoal > DonutDistance)
	{
		DonutTimer = Frame + uint32(2.5f * GenLogic::GetLogicFPS());
	}
	else if (DonutTimer < Frame)
	{
		bLocoBraking = true; // stop circling around the goal ("donuts")
	}
	if (bLocoBraking)
	{
		BrakingFactor = 1.f;
		if (SlowDownDist > OnPathDistToGoal) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking(), 0.f); }
		else if (SlowDownDist > OnPathDistToGoal * 0.75f) { GoalSpeed = FMath::Max(ActualSpeed - GetBraking() / 2.f, 0.f); }
		else { GoalSpeed = ActualSpeed; }
	}

	const float TurnFactor = FMath::Min(FMath::Abs(ActualSpeed / TurnSpeed), 1.f);
	const float TurnAmount = TurnFactor * MaxTurnRate;
	if (bMoveBackwards && !bThreePoint)
	{
		const FVector3f BackwardPos(2.f * Position.X - GoalPos.X, 2.f * Position.Y - GoalPos.Y, Position.Z);
		Turning = int8(RotateAroundLocoPivot(BackwardPos, TurnAmount));
	}
	else
	{
		Turning = int8(RotateAroundLocoPivot(GoalPos, TurnAmount));
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, MaxAccel, BrakingFactor, bMoveBackwards);
}

void FGenObject::MoveLegs(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed)
{
	DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed);
	const float ActualSpeed = GetForwardSpeed2D();
	const float DesiredAngle = FMath::Atan2(GoalPos.Y - Position.Y, GoalPos.X - Position.X);
	const float RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);

	// locoUpdate_moveTowardsAngle: legs turn in place towards the goal.
	const float Turn = FMath::Clamp(RelAngle, -Locomotor->TurnRate, Locomotor->TurnRate);
	SetOrientation(Orientation + Turn);

	const float AngleCoeff = FMath::Min(FMath::Abs(RelAngle) / (PI / 4.f), 1.f);
	float GoalSpeed = (1.f - AngleCoeff) * DesiredSpeed;

	// calcSlowDownDist
	const float Delta = ActualSpeed - Locomotor->MinSpeed;
	const float SlowDownDist = Delta > 0.f ? (Sqr(Delta) / GetBraking()) * 0.5f * 1.05f : 0.f;
	if (OnPathDistToGoal < SlowDownDist)
	{
		GoalSpeed = Locomotor->MinSpeed;
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, Locomotor->Acceleration, 1.f);
}

void FGenObject::UpdateAnimationSteering(uint32 Frame)
{
	// AnimationSteeringUpdate::update: a combat cycle leans into a turn (CENTER_TO_LEFT / RIGHT), and back up
	// once the turn is over (LEFT / RIGHT_TO_CENTER), each stage held for at least MinTransitionTime.
	static const int32 CenterToLeft = GenConditions::Index(TEXT("CENTER_TO_LEFT"));
	static const int32 LeftToCenter = GenConditions::Index(TEXT("LEFT_TO_CENTER"));
	static const int32 CenterToRight = GenConditions::Index(TEXT("CENTER_TO_RIGHT"));
	static const int32 RightToCenter = GenConditions::Index(TEXT("RIGHT_TO_CENTER"));
	if (Frame < SteerAnimNextFrame)
	{
		return;
	}
	const int32 Turn = bMoving ? Turning : TurnNone; // (standing, it isn't turning)
	const uint32 Hold = uint32(Template->AnimationSteeringFrames);
	if (SteerAnim == 0)
	{
		if (Turn == TurnNegative || Turn == TurnPositive)
		{
			SteerAnim = Turn == TurnNegative ? CenterToRight : CenterToLeft; // "Initiate a right turn" / left
			SetModelCondition(SteerAnim, true);
			SteerAnimNextFrame = Frame + Hold;
		}
	}
	else if (SteerAnim == CenterToRight || SteerAnim == CenterToLeft)
	{
		if (Turn != (SteerAnim == CenterToRight ? TurnNegative : TurnPositive))
		{
			// "Recenter!"
			SetModelCondition(SteerAnim, false);
			SteerAnim = SteerAnim == CenterToRight ? RightToCenter : LeftToCenter;
			SetModelCondition(SteerAnim, true);
			SteerAnimNextFrame = Frame + Hold;
		}
	}
	else if (Turn == TurnNone)
	{
		// "Finish the turn"
		SetModelCondition(LeftToCenter, false);
		SetModelCondition(RightToCenter, false);
		SteerAnim = 0;
		SteerAnimNextFrame = Frame;
	}
}

void FGenObject::MoveClimb(const FVector3f& GoalPos, float OnPathDistToGoal, float DesiredSpeed)
{
	// Locomotor::moveTowardsPositionClimb: legs that go up a cliff facing it and back down it, slowed by how steep
	// the face is. What differs from the original (the user asked for it to be better than it was there):
	//  - It reads the ground over three units ahead, not one, and climbs only where the face is steep, with some
	//    give either way (climbing from 0.6 - about 31 degrees - walking again under 0.35): the original climbed
	//    whenever its waypoint was more than a cell above or below, flat or not, and turned to back down at any
	//    dip a tenth of a unit deep, so it flipped round and round on uneven ground.
	//  - Its pace on the face is a steady quarter of its speed upwards or downwards whatever the slope; the
	//    original's was full speed up to 45 degrees and then a quarter of it at once.
	DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed);
	FVector2f Dir(GoalPos.X - Position.X, GoalPos.Y - Position.Y);
	const float Dist = Dir.Size();
	Dir = Dist > 0.001f ? Dir / Dist : GetDirection2D();
	constexpr float Probe = 3.f;
	const float Here = Logic->GetGroundHeight(Position.X, Position.Y);
	const float Ahead = Logic->GetGroundHeight(Position.X + Dir.X * Probe, Position.Y + Dir.Y * Probe);
	const float Slope = (Ahead - Here) / Probe; // rising towards the goal: > 0
	const float Steep = FMath::Abs(Slope);
	if (Steep > 0.6f)
	{
		bLocoClimbing = true;
	}
	else if (Steep < 0.35f)
	{
		bLocoClimbing = false;
	}
	if (!bLocoClimbing || Slope > -0.35f)
	{
		bLocoBackwards = false;
	}
	else if (Slope < -0.6f)
	{
		bLocoBackwards = true; // "human climber - backs down cliffs"
	}
	if (bLocoClimbing && Steep > 0.25f)
	{
		DesiredSpeed = FMath::Min(DesiredSpeed, Locomotor->Speed / (4.f * Steep));
	}
	const uint8 ClimbState = uint8(bLocoClimbing) | (uint8(bLocoBackwards) << 1);
	UE_CLOG(Logic->IsTracing() && ClimbState != LastClimbState, LogGenerals, Log, TEXT("  [climb f%u] #%u %s %s at (%.0f, %.0f), slope %.2f"), Logic->GetFrame(), ID,
		*Template->Name, bLocoBackwards ? TEXT("backs down") : (bLocoClimbing ? TEXT("climbs") : TEXT("walks")), Position.X, Position.Y, Slope);
	LastClimbState = ClimbState;

	// Orient toward goal position (away from it, backing down).
	float DesiredAngle = FMath::Atan2(Dir.Y, Dir.X);
	if (bLocoBackwards)
	{
		DesiredAngle = GenLogic::AngleDiff(DesiredAngle, PI);
	}
	const float RelAngle = GenLogic::AngleDiff(DesiredAngle, Orientation);
	SetOrientation(Orientation + FMath::Clamp(RelAngle, -Locomotor->TurnRate, Locomotor->TurnRate)); // locoUpdate_moveTowardsAngle

	// "The more we have to turn, the slower we go"
	const float AngleCoeff = FMath::Min(FMath::Abs(RelAngle) / (PI / 4.f), 1.f);
	float GoalSpeed = (1.f - AngleCoeff) * DesiredSpeed;
	float ActualSpeed = GetForwardSpeed2D();
	if (bLocoBackwards)
	{
		ActualSpeed = -ActualSpeed;
	}
	const float Delta = ActualSpeed - Locomotor->MinSpeed;
	const float SlowDownDist = Delta > 0.f ? (Sqr(Delta) / GetBraking()) * 0.5f * 1.05f : 0.f; // calcSlowDownDist
	if (OnPathDistToGoal < SlowDownDist)
	{
		GoalSpeed = Locomotor->MinSpeed;
	}
	ApplyGoalSpeed(GoalSpeed, ActualSpeed, Locomotor->Acceleration, 1.f, bLocoBackwards);
}

// ---- PhysicsBehavior ----------------------------------------------------------------------------

void FGenObject::ApplyMotiveForce(const FVector3f& Force, uint32 Frame)
{
	ApplyForce(Force);
	MotiveForceExpires = Frame + uint32(GenLogic::GetLogicFPS() / 3); // MOTIVE_FRAMES
}

void FGenObject::UpdatePhysics(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Physics");
	// applyFrictionalForces (on the ground)
	const float Extra = (Locomotor ? Locomotor->Extra2DFriction : 0.f) + (bLocoUltraAccurate ? 0.5f * 30.f / float(GenLogic::GetLogicFPS()) : 0.f);
	if (Velocity.X != 0.f || Velocity.Y != 0.f)
	{
		const FVector2f Dir = GetDirection2D();
		const float LateralDot = Velocity.X * (-Dir.Y) + Velocity.Y * Dir.X;
		const float LF = FMath::Clamp(LateralFriction + Extra, 0.01f, 0.99f);
		FVector3f A(-(LF * LateralDot * -Dir.Y), -(LF * LateralDot * Dir.X), 0.f);
		if (Frame >= MotiveForceExpires)
		{
			const float ForwardDot = Velocity.X * Dir.X + Velocity.Y * Dir.Y;
			const float FF = FMath::Clamp(ForwardFriction + Extra, 0.01f, 0.99f);
			A.X += -(FF * ForwardDot * Dir.X);
			A.Y += -(FF * ForwardDot * Dir.Y);
		}
		Accel += A;
	}

	// integrate
	PrevAccel = Accel;
	PreviousOverlap = CurrentOverlap; // (the collisions after the objects' updates find this step's)
	CurrentOverlap = GenInvalidObjectID;
	Velocity += Accel;
	Accel = FVector3f::ZeroVector;
	if (FMath::Abs(Velocity.X) < 0.001f) { Velocity.X = 0.f; }
	if (FMath::Abs(Velocity.Y) < 0.001f) { Velocity.Y = 0.f; }
	Velocity.Z = 0.f;
	if (!bStatusBraking)
	{
		Position.X += Velocity.X;
		Position.Y += Velocity.Y;
	}

	// Ground units stay on the terrain - or on the bridge they are crossing.
	UpdateLayer();
	Position.Z = GetSurfaceHeight();
}

uint32 FGenObject::GetFlashNow() const
{
	return Logic->GetFrame();
}

uint32 FGenObject::GetPathSurfaces() const
{
	// (LocomotorSet::getValidSurfaces: a climber's paths go up cliffs, whichever locomotor it is on right now.)
	const uint32 Surfaces = LocoSet.Num() > 1 ? LocoSetSurfaces : (Locomotor ? Locomotor->Surfaces : uint32(GenSurface::Ground));
	return Surfaces | (Template->CrusherLevel > 0 ? uint32(GenSurface::Fence) : 0u);
}

void FGenObject::SetLocomotorSet(const TArray<FString>& Names)
{
	LocoSet.Reset();
	LocoSetSurfaces = 0;
	for (const FString& Name : Names)
	{
		if (const FGenLocomotorTemplate* L = FGenGameData::Get().FindLocomotor(Name))
		{
			LocoSet.Add(L);
			LocoSetSurfaces |= L->Surfaces;
		}
	}
}

const FGenLocomotorTemplate* FGenObject::FindLocomotorForPosition() const
{
	// LocomotorSet::findLocomotor: the first of the set that may move on what the ground there is.
	const uint32 Acceptable = Logic->GetPathfinder().LocomotorSurfacesAt(FVector2f(Position.X, Position.Y), Layer);
	for (const FGenLocomotorTemplate* L : LocoSet)
	{
		if (L->Surfaces & Acceptable)
		{
			return L;
		}
	}
	return nullptr;
}

void FGenObject::ChooseGoodLocomotorFromCurrentSet()
{
	// "due to physics, we might slide into a cell for which we have no loco (eg, cliff) and get stuck... as a
	// solution" the one in use is kept when none fits.
	const FGenLocomotorTemplate* New = FindLocomotorForPosition();
	if (!New || New == Locomotor)
	{
		return;
	}
	Locomotor = New;
	// "turn off precision-z-pos anytime the loco changes ... ditto for ultra-accuracy." (Each of the original's
	// locomotors has flags of its own: the one taken over starts with nothing of the other's.)
	bLocoPreciseZ = false;
	bLocoUltraAccurate = false;
	bLocoBackwards = false;
	if (Locomotor->Appearance != EGenLocoAppearance::Climber)
	{
		bLocoClimbing = false;
	}
}

float FGenObject::GetSurfaceHeight() const
{
	const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
	return Layer != 0 ? Logic->GetPathfinder().GetLayerHeight(Position.X, Position.Y, Layer, Ground) : Ground;
}

void FGenObject::UpdateLayer()
{
	const FGenPathfinder& PF = Logic->GetPathfinder();
	if (PF.GetNumLayers() == 0)
	{
		return;
	}
	if (Layer != 0)
	{
		const FGenBridgeInfo* B = PF.GetBridge(Layer);
		if (B && B->IsPointOnBridge(Position.X, Position.Y))
		{
			return; // still on the deck
		}
		Layer = 0;
		if (Logic->IsTracing())
		{
			UE_LOG(LogGenerals, Log, TEXT("  [bridge f%u] #%u p%d %s leaves the bridge at (%.0f, %.0f)"), Logic->GetFrame(), ID, Owner, *Template->Name, Position.X, Position.Y);
		}
	}
	// AIUpdateInterface::doLocomotor: "layer is a possible bridge in the path. Check & set the layer if
	// applicable" - Pathfinder::updateLayer takes it only if the object really is at the bridge
	// (TerrainLogic::objectInteractsWithBridgeLayer: on it, and at the deck's height).
	const int32 Want = (bMoving && PathLayers.Num() == Path.Num() && PathLayers.IsValidIndex(PathIndex)) ? PathLayers[PathIndex] : 0;
	if (Want != 0)
	{
		const FGenBridgeInfo* B = PF.GetBridge(Want);
		if (B && B->IsPointOnBridge(Position.X, Position.Y) && FMath::Abs(Position.Z - B->GetHeight(Position.X, Position.Y)) <= 10.f)
		{
			Layer = uint8(Want);
			if (Logic->IsTracing())
			{
				UE_LOG(LogGenerals, Log, TEXT("  [bridge f%u] #%u p%d %s drives onto bridge %d at (%.0f, %.0f)"), Logic->GetFrame(), ID, Owner, *Template->Name, Want, Position.X, Position.Y);
			}
		}
	}
}

void FGenObject::UpdateFreePhysics(uint32 Frame)
{
	// PhysicsBehavior::update for objects that are not stuck to the ground: debris and hulks thrown by
	// an ObjectCreationList fall under gravity, bounce, tumble and come to rest.
	constexpr float SignificantlyAboveTerrain = 9.f;  // SIGNIFICANTLY_ABOVE_TERRAIN_THRESHOLD
	const FGenGameData& GD = FGenGameData::Get();
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS());
	float GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	auto DampSpin = [this](float F) { YawRate *= F; PitchRate *= F; RollRate *= F; };

	// applyGravitationalForces
	Accel.Z += GD.Gravity;

	// applyFrictionalForces: ground friction near the terrain, air resistance above it
	if (Position.Z - GroundZ <= SignificantlyAboveTerrain)
	{
		DampSpin(1.f - 0.15f * RateScale);
		if (Velocity.X != 0.f || Velocity.Y != 0.f)
		{
			const float Yaw = GetYawFromRotation();
			const FVector2f Dir(FMath::Cos(Yaw), FMath::Sin(Yaw));
			const float LateralDot = Velocity.X * (-Dir.Y) + Velocity.Y * Dir.X;
			const float ForwardDot = Velocity.X * Dir.X + Velocity.Y * Dir.Y;
			const float LF = FMath::Clamp(LateralFriction, 0.01f, 0.99f);
			const float FF = FMath::Clamp(ForwardFriction, 0.01f, 0.99f);
			Accel.X += -(LF * LateralDot * -Dir.Y) - FF * ForwardDot * Dir.X;
			Accel.Y += -(LF * LateralDot * Dir.X) - FF * ForwardDot * Dir.Y;
		}
	}
	else
	{
		const float Aero = FMath::Clamp(Template->AerodynamicFriction + ExtraFriction, 0.f, 0.99f);
		Accel -= Velocity * Aero;
		DampSpin(1.f - Aero);
	}

	// integrate
	Velocity += Accel;
	Accel = FVector3f::ZeroVector;
	if (FMath::Abs(Velocity.X) < 0.001f) { Velocity.X = 0.f; }
	if (FMath::Abs(Velocity.Y) < 0.001f) { Velocity.Y = 0.f; }
	if (FMath::Abs(Velocity.Z) < 0.001f) { Velocity.Z = 0.f; }
	const float OldZ = Position.Z;
	Position += Velocity;

	if (YawRate != 0.f || PitchRate != 0.f || RollRate != 0.f)
	{
		// mtx.Rotate_X(roll); Rotate_Y(pitch); Rotate_Z(yaw): each turns about the object's own axes.
		const float F = Template->PitchRollYawFactor;
		Rotation = (Rotation * FQuat4f(FVector3f::ForwardVector, RollRate * F) * FQuat4f(FVector3f::RightVector, PitchRate * F)
			* FQuat4f(FVector3f::UpVector, YawRate * F)).GetNormalized();
		bFreeRotation = true;
	}

	// handleBounce
	GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	float BounceAccel = 0.f;
	if (bAllowBounce && Position.Z <= GroundZ)
	{
		const float Stiffness = FMath::Clamp(FGenFXData::Get().GroundStiffness, 0.01f, 0.99f);
		if (OldZ > GroundZ && Velocity.Z < 0.f)
		{
			BounceAccel = FMath::Abs(Velocity.Z) * Stiffness;
		}
		DampSpin(0.7f);
		if (BounceAccel <= 0.f)
		{
			bAllowBounce = Template->bAllowBouncing;
		}
	}

	// do not pass through the ground
	if (Position.Z <= GroundZ)
	{
		Velocity.Z += GroundZ - Position.Z; // remove the excess z velocity
		if (Velocity.Z > 0.f) { Velocity.Z = 0.f; }
		Position.Z = GroundZ;
	}

	// "it's not good enough to check for airborne being different between the start and end of this func":
	// down since the last frame, the bounce is heard (doBounceSound).
	const bool bAboveTerrain = Position.Z > GroundZ;
	if (bWasAboveTerrain && !bAboveTerrain && !BounceSound.IsEmpty())
	{
		Logic->PlayEventForObject(BounceSound, *this);
	}
	bWasAboveTerrain = bAboveTerrain;

	if (BounceAccel > 0.f)
	{
		// The object is righted on impact (pitch and roll may have been anything), then bounces back up.
		SetOrientation(GetYawFromRotation());
		Accel.Z += BounceAccel;
	}
	else
	{
		Orientation = GetYawFromRotation();
	}

	const bool bResting = Position.Z <= GroundZ && FMath::Abs(Velocity.X) < 0.01f && FMath::Abs(Velocity.Y) < 0.01f && FMath::Abs(Velocity.Z) < 0.01f;
	if (Template->bKillWhenRestingOnGround && bResting && !bDead)
	{
		Kill();
	}
}

void FGenObject::SetOwner(int32 NewOwner)
{
	const int32 OldOwner = Owner;
	Owner = NewOwner;
	Logic->OwnerChanged(*this, OldOwner);
	// HelixContain / OverlordContain::onCapture: its add-on changes hands with it.
	if (FGenObject* AddOn = Logic->GetPortableRider(*this))
	{
		AddOn->SetOwner(NewOwner);
	}
}
