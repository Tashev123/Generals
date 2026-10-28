#pragma once

#include "CoreMinimal.h"
#include <initializer_list>
#include "Subsystems/WorldSubsystem.h"
#include "Core/GenPerf.h"
#include "Engine/EngineBaseTypes.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenMatch.h"
#include "Logic/GenObject.h"
#include "Logic/GenPartition.h"
#include "Logic/GenPathfinder.h"
#include "Logic/GenShroud.h"
#include "Logic/GenSkirmishAI.h"
#include "Client/GenDrawable.h"
#include "GenGameLogic.generated.h"

class AGenMapActor;
class FGenMapData;

/** One rope to draw (W3DRopeDraw): straight down from Top, in segments that sway. */
struct FGenRopeDraw
{
	FVector3f Top = FVector3f::ZeroVector;
	float Len = 0.f;
	float LenMax = 1.f;
	float Width = 0.5f;
	float WobbleLen = 10.f;
	float WobbleAmp = 0.f;
	float Phase = 0.f;
};

/** A rope a helicopter has let go: it falls, and is gone a few seconds later. */
struct FGenFallingRope
{
	FVector3f Top = FVector3f::ZeroVector;
	float Len = 0.f;
	float Speed = 0.f;
	float Width = 0.5f;
	float WobbleLen = 10.f;
	float WobbleAmp = 0.f;
	float WobbleRate = 0.f;
	float Phase = 0.f;
	uint32 ExpireFrame = 0;
};
class FGenSimThread;
class UGenAudioManager;
class UGenFXManager;
class UGenModelComponent;
class UMaterialInstanceDynamic;
class UTexture2D;
struct FGenPlayerTemplate;
struct FGenThingTemplate;
struct FGenWeaponTemplate;
struct FGenDrawModule;

/** ScoreKeeper.cpp: what a player built, lost and destroyed, for the score screen. */
struct GENERALS1_API FGenScore
{
	static constexpr int32 MaxPlayers = 16;

	int32 MoneyEarned = 0;
	int32 MoneySpent = 0;                   // (as in the original: on buildings and upgrades)
	int32 UnitsBuilt = 0;
	int32 UnitsLost = 0;
	int32 BuildingsBuilt = 0;
	int32 BuildingsLost = 0;
	int32 UnitsDestroyed[MaxPlayers] = {};  // by whose they were
	int32 BuildingsDestroyed[MaxPlayers] = {};
	int32 TechBuildingsCaptured = 0;
	int32 FactionBuildingsCaptured = 0;

	int32 GetUnitsDestroyed() const;
	int32 GetBuildingsDestroyed() const;
	/** ScoreKeeper::calculateScore */
	int32 Calculate(int32 OwnIndex) const;
};

/** A participant (Player.cpp). Index 0 is the neutral/civilian player that owns map objects. */
/** Player::addKindOfProductionCostChange: things of these kinds cost the player this much less (or more). */
struct GENERALS1_API FGenCostModifier
{
	TArray<FString> KindOf;     // all of them (upper case)
	float Percent = 0.f;        // -0.1 = 10% cheaper
};

struct GENERALS1_API FGenPlayer
{
	int32 Index = 0;
	FString Name;
	const FGenPlayerTemplate* Template = nullptr;
	FLinearColor Color = FLinearColor::White;
	int32 Money = 0;
	int32 Team = -1;            // players on the same team (>= 0) are allies
	int32 StartPosition = -1;   // 0-based (Player_N_Start uses N = StartPosition + 1)
	bool bHuman = false;
	bool bNeutral = false;
	bool bSkirmishAI = false;   // played by the computer
	int32 Difficulty = -1;      // ... at this level (0 easy, 1 medium, 2 hard; -1: the default)
	bool bDefeated = false;     // has lost every building that counts: out of the game
	bool bDisconnected = false; // (a network game) its player has dropped out: its allies may command its things
	FGenScore Score;

	// Upgrades (lower-case names)
	TSet<FString> UpgradesComplete;
	TSet<FString> UpgradesInProduction;
	// The general: rank, skill points earned by destroying things, sciences bought with promotion points
	TSet<FString> Sciences;
	int32 RankLevel = 1;
	int32 SkillPoints = 0;
	int32 SciencePurchasePoints = 0;
	float CashBounty = 0.f;                 // share of each kill's cost paid to this player
	TMap<FString, uint32> PowerReadyFrame;  // timers of the powers all the player's buildings share (lower-case name)
	int32 PowersFired = 0;
	TArray<FGenObjectID> TunnelRiders;      // who is in the tunnel network (all of the player's tunnels share them)
	uint32 TunnelHealFrame = 0;
	TArray<FString> UpgradeEvents;          // finished upgrades / acquired sciences the scripts haven't asked about yet
	TArray<FString> ScienceEvents;
	// Fog of war and radar
	uint32 AlliesMask = 0;                  // the players who see what this one's units see (itself included)
	uint32 OthersMask = 0;                  // everybody else
	uint32 SpiedByMask = 0;                 // players who, for now, also see what this one's units see (spy vision)
	uint32 SpyVisionEndFrame = 0;           // ... and, for the one spying, when that ends (0: not spying)
	uint32 PowerSabotagedUntil = 0;         // Energy::setPowerSabotagedTillFrame: a saboteur cut the power
	FGenObjectID SpyVisionSource = GenInvalidObjectID; // ... and the building it comes from
	TArray<FGenCostModifier> CostModifiers; // from the oil refineries it holds (RefreshCostModifiers)
	int32 RadarCount = 0;                   // working radars (Player::m_radarCount)
	int32 DisableProofRadarCount = 0;       // ... of which these need no power
	bool bHasRadar = false;                 // Player::hasRadar
	bool bBrownOut = false;                 // uses more power than it makes (Energy::hasSufficientPower is false)
	// Battle plans (Player::m_bombardBattlePlans ... and m_battlePlanBonuses)
	int32 BattlePlans[3] = { 0, 0, 0 };     // strategy centers with bombardment / hold the line / search and destroy in force
	FGenBattlePlanBonuses BattlePlanBonuses; // what they give the army, all together (new units get it too)
	bool bHasBattlePlanBonuses = false;
	int32 NumBattlePlansActive() const { return BattlePlans[0] + BattlePlans[1] + BattlePlans[2]; }
};

/** Text that floats up from a spot in the world and fades ("+$600" over a supply center). */
struct GENERALS1_API FGenFloatingText
{
	FString Text;
	FVector3f Pos = FVector3f::ZeroVector;
	FLinearColor Color = FLinearColor::White;
	uint32 Frame = 0;           // logic frame it appeared
	bool bScreen = false;       // an announcement at the top of the screen rather than a spot in the world
};

/**
 * Where a network game's orders come from (Net/GenNetClient.h): a logic frame is worked out only once the
 * host has closed it, with exactly the orders the host gave it. Asked from the simulation's thread as well
 * as the game thread.
 */
class IGenOrderSource
{
public:
	virtual ~IGenOrderSource() = default;
	/** How many frames the host has closed: frames below this are ready to be worked out. */
	virtual uint32 GetReadyFrames() const = 0;
	bool IsFrameReady(uint32 InFrame) const { return InFrame < GetReadyFrames(); }
	/** The orders of that frame, in the host's order (each frame is taken once, in turn). */
	virtual void TakeFrame(uint32 InFrame, TArray<FGenCommand>& Out) = 0;
	/** An order of this computer's player, on its way to the host. */
	virtual void SendOrder(const FGenCommand& Command) = 0;
	/** What the simulation came to after that frame, for the host to compare with the other games'. */
	virtual void NoteChecksum(uint32 InFrame, uint32 Checksum) = 0;
};

/**
 * The simulation (GameLogic.cpp equivalent).
 *
 * Logic runs at a fixed rate (gen.LogicFPS) from an accumulator; rendering interpolates objects
 * between the previous and current logic frames, so the game renders at any frame rate. Player
 * orders (FGenCommand) are queued and applied at the start of a logic frame, like the original's
 * message stream: the same settings and the same orders at the same frames make the same game,
 * which is what replays and network games rest on.
 */
UCLASS()
class GENERALS1_API UGenGameLogic : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Starts the match on the map the map actor has loaded. InLocalPlayer: who plays on this computer (INDEX_NONE:
	 *  the first human). */
	bool StartSkirmish(AGenMapActor* InMapActor, const FGenMatchSettings& Settings, int32 InLocalPlayer = INDEX_NONE);
	bool IsRunning() const { return MapActor != nullptr; }
	const FGenMatchSettings& GetMatchSettings() const { return Match; }

	// --- orders (GenOrders.cpp) ---
	/** An order of this computer's player (or Command.Player's): applied at the start of the next logic frame. */
	void SubmitCommand(FGenCommand&& Command);
	/** An order the simulation gives itself (a computer player selling a captured building): applied at the start
	 *  of the next frame too, but neither recorded nor sent anywhere - every computer gives it again. */
	void QueueSimCommand(FGenCommand&& Command);
	/** May that player command that object? Its own things. */
	bool CanPlayerCommand(int32 Player, const FGenObject& Obj) const;
	/** A test switch that changes the game by hand, as an order (see ExecCheat): "money,5000", "kill,Type[,Player]". */
	void IssueCheat(const FString& Step);

	// --- replays and checks (GenOrders.cpp) ---
	/** A checksum of the simulation as it stands: the objects' places, headings, health and targets, the players'
	 *  money and ranks, the random numbers. Two computers with the same checksum at a frame have the same game. */
	uint32 ComputeChecksum() const;
	/** The simulation as text, an object to a line, for finding where two runs went apart. */
	bool DumpState(const FString& Path) const;
	/** The game so far: its settings, start, orders and checksums. */
	const FGenReplay* GetRecording() const { return Recording.Get(); }
	/** Writes it to that file (empty: LastReplay in the replay folder). */
	bool SaveRecording(const FString& Path = FString());
	/** The orders come from this replay instead of from this computer (before StartSkirmish, with its settings). */
	void SetPlayback(TUniquePtr<FGenReplay> Replay);
	bool IsPlayingBack() const { return Playback.IsValid(); }
	/** The first frame whose checksum differed from the replay's (0: none so far). */
	uint32 GetPlaybackMismatchFrame() const { return PlaybackMismatchFrame; }
	/** The replay's start: puts the objects the recorded game's start put on the map (after StartSkirmish). */
	void PlaceSetupObjects(const TArray<FGenSetupObject>& SetupObjects);

	// --- network games ---
	/** The orders come from the host (before StartSkirmish); null: from this computer. Not owned. */
	void SetOrderSource(IGenOrderSource* Source) { OrderSource = Source; }
	bool IsNetworkGame() const { return OrderSource != nullptr; }
	/** Far behind the host (coming back into a match): the steps are being run as fast as they go. */
	bool IsCatchingUp() const { return bCatchingUpNow; }

	// --- objects ---
	FGenObject* CreateObject(const FGenThingTemplate* Template, int32 OwnerPlayer, const FVector3f& Position, float Angle);
	void DestroyObject(FGenObjectID ID);
	FGenObject* FindObject(FGenObjectID ID) const;
	const TArray<FGenObjectID>& GetObjectIDs() const { return ObjectOrder; }

	// --- players ---
	const TArray<FGenPlayer>& GetPlayers() const { return Players; }
	int32 GetLocalPlayer() const { return LocalPlayer; }
	EGenRelationship GetRelationship(int32 A, int32 B) const;

	// --- commands (queued, applied next logic frame) ---
	void IssueMove(const TArray<FGenObjectID>& Units, const FVector3f& Goal);
	void IssueStop(const TArray<FGenObjectID>& Units);
	/** A FIRE_WEAPON button aimed at a spot (MSG_DO_WEAPON_AT_LOCATION): so many shots there, or clearing the
	 *  mines there (Command_DisarmMinesAtPosition). Pos.X FLT_MAX: where each one stands. */
	void IssueFireWeaponAt(const TArray<FGenObjectID>& Units, const FVector3f& Pos, int32 Slot, bool bMineClearing, int32 MaxShots = MAX_int32);
	/** MSG_DO_FORCE_ATTACK_OBJECT (Ctrl): attack it whoever it belongs to, and on until it is no more. */
	void IssueForceAttack(const TArray<FGenObjectID>& Units, FGenObjectID Target);
	/** MSG_DO_FORCE_ATTACK_GROUND (Ctrl): fire at that spot, on and on. */
	void IssueForceAttackGround(const TArray<FGenObjectID>& Units, const FVector3f& Pos);
	/** MSG_SWITCH_WEAPONS: setWeaponLockForGroup(slot, LOCKED_PERMANENTLY). */
	void IssueSwitchWeapon(const TArray<FGenObjectID>& Units, int32 Slot);
	/** AIGroup::groupCreateFormation (Ctrl+F): the units keep their places relative to one another from now on - or, if they do already, no longer. */
	void IssueCreateFormation(const TArray<FGenObjectID>& Units);
	/**
	 * AIGroup::groupMoveToPosition (GenGroupMove.cpp): every unit to its place in the group at the goal;
	 * three infantry or vehicles and more with some way to go as a column along one wide way; a formation
	 * in its shape and at its slowest member's pace; a click in the middle of the group gathers it in.
	 * Simulation side (a queued command, or the step itself).
	 */
	/** AIGroup::groupMoveToPosition; bAddWaypoint: MSG_ADD_WAYPOINT - the place each one would go to is added to its route instead. */
	void GroupMoveToPosition(const TArray<FGenObjectID>& Units, const FVector3f& Goal, bool bFromPlayer, bool bAddWaypoint = false);
	/** Attack-move: to the goal (spread out like a move), fighting what comes into view on the way. */
	void IssueAttackMove(const TArray<FGenObjectID>& Units, const FVector3f& Goal);
	/** Guard that spot, or (GuardObject) stay with that object. */
	void IssueGuard(const TArray<FGenObjectID>& Units, const FVector3f& Pos, FGenObjectID GuardObject, EGenGuardMode Mode);

	// --- veterancy and victory (GenVeterancy.cpp) ---
	void AnnounceVeterancy(const FGenObject& Obj, const TCHAR* LevelName);
	/** VictoryConditions: no building that counts is left. */
	bool HasPlayerBeenDefeated(int32 Player) const;
	bool IsPlayerDefeated(int32 Player) const { return Players.IsValidIndex(Player) && Players[Player].bDefeated; }
	/** Only one alliance is left. */
	bool IsGameOver() const { return bGameOver; }
	bool HasLocalPlayerWon() const;
	uint32 GetGameOverFrame() const { return GameOverFrame; }
	void IssueAttack(const TArray<FGenObjectID>& Units, FGenObjectID Target);
	/** MSG_ADD_WAYPOINT (Alt held): one more point on the way each of them is going. */
	void IssueAddWaypoint(const TArray<FGenObjectID>& Units, const FVector3f& Pos);
	/** Aircraft fly to that airfield, land and park there (planes also reload; helicopters get repaired). */
	void IssueLand(const TArray<FGenObjectID>& Units, FGenObjectID Airfield);

	// --- combat services (used by FGenObject) ---
	/** WeaponTemplate::dealDamageInternal: single victim or everything within the damage radius. */
	void DealDamage(const FGenWeaponTemplate& Weapon, FGenObjectID Source, FGenObjectID Victim, const FVector3f& Pos, bool bProjectileDetonation);
	/** WeaponStore::setDelayedDamage: instant-hit weapons land after their travel time. */
	void QueueDelayedDamage(const FGenWeaponTemplate& Weapon, FGenObjectID Source, FGenObjectID Victim, const FVector3f& Pos, uint32 WhenFrame);
	/** Creates the weapon's ProjectileObject and launches it. */
	FGenObject* SpawnProjectile(const FGenWeaponTemplate& Weapon, const FGenObject& Shooter, FGenObjectID Victim, const FVector3f& TargetPos,
		const FVector3f& LaunchPos, float Angle, const FQuat4f* LaunchRotation = nullptr);
	void OnObjectDied(const FGenObject& Obj);
	/** Deterministic random stream for the simulation (GameLogicRandomValue). */
	FRandomStream& GetRandom() { return Random; }

	// --- effects (FXList / ObjectCreationList), by INI name; unknown or empty names are ignored ---
	void DoFXObj(const FString& FXName, const FGenObject& Primary, const FGenObject* Secondary = nullptr);
	void DoFXPos(const FString& FXName, const FVector3f& Pos, const FTransform3f* Xf, float Speed, const FVector3f* Secondary, float Radius);
	/** Weapon FireFX at the shooter's WeaponFireFXBone (or at the shooter when the model has none). */
	void DoWeaponFireFX(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon, const FVector3f& TargetPos);
	/** ObjectCreationList::create: debris and objects (hulks) thrown out of the source object. */
	void DoOCL(const FString& OCLName, const FGenObject& Source, const FVector3f* AtPosition = nullptr, TArray<FGenObjectID>* OutCreated = nullptr);
	/** One beam to draw this frame (W3DLaserDraw + LaserUpdate): a laser pointer, a hacker's stream of numbers. */
	using FBeam = FGenBeam;
	/** The beams units hold on their targets right now, as far as the local player can see them. */
	void GetVisibleBeams(TArray<FBeam>& Out) const;
	/** Weapon::createLaser: the shot of a laser weapon - a beam from the shooter's bone to where it struck, for the laser object's lifetime. */
	void CreateWeaponLaser(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon, const FGenObject* Victim, const FVector3f& VictimPos);
	struct FWeaponBeam
	{
		const struct FGenLaserDraw* Draw = nullptr;
		FGenObjectID Shooter = GenInvalidObjectID;
		FString Bone;
		FVector3f Start = FVector3f::ZeroVector;
		FVector3f End = FVector3f::ZeroVector;
		uint32 ExpireFrame = 0;
	};
	TArray<FWeaponBeam> WeaponBeams;
	TArray<FGenObjectID> ParticleCannons;   // the objects with a ParticleUplinkCannonUpdate (their beams are drawn)
	/** One projectile stream to draw this frame (W3DProjectileStreamDraw): a run of its points, the newest last. */
	struct FStreamLine
	{
		const struct FGenStreamDraw* Draw = nullptr;
		TArray<FVector3f> Points;
	};
	/** The streams the local player can see, cut where they break (ProjectileStreamUpdate::getAllPoints). */
	void GetVisibleStreams(TArray<FStreamLine>& Out) const;
	/** Weapon::newProjectileFired: a shot of a weapon with a ProjectileStreamName joins its line. */
	void AddToProjectileStream(const FGenWeaponTemplate& Weapon, const FGenObject& Shooter, const FGenObject& Projectile, FGenObjectID Victim, const FVector3f& VictimPos);
	TArray<FGenObjectID> ProjectileStreams; // the stream objects there are (their lines are drawn)
	/** SpyVisionUpdate by upgrade: the buildings that may grant it, and - each shroud update - who sees through
	 *  whose eyes (the spying player, and the kinds of the enemy's things; none: all of them). */
	TArray<FGenObjectID> SpyVisionSources;
	struct FSpyGrant { int32 Player = INDEX_NONE; const TArray<FString>* Kinds = nullptr; };
	TArray<FSpyGrant> SpyGrants;
	uint32 SpyGrantMask(const FGenObject& O) const;
	void AddSpyVisionSource(FGenObjectID Source) { SpyVisionSources.AddUnique(Source); }
	// --- the Spectre gunship (GenGunship.cpp) ---
	/** SpectreGunshipDeploymentUpdate: the building's power makes a gunship beyond the map's edge and sends it in. */
	bool DeployGunship(FGenObject& Source, const FString& Power, const FVector3f& TargetPos);
	/** The player moves the reticle of gunships on their run. */
	void IssueSteerGunship(const TArray<FGenObjectID>& Units, const FVector3f& Pos);
	/** The nearest enemy within Range of that spot which the gunship may fire at. */
	FGenObject* FindGunshipVictim(const FGenObject& Ship, const FVector3f& Center, float Range) const;
	const TArray<FGenObjectID>& GetGunships() const { return Gunships; }
	TArray<FGenObjectID> Gunships;          // the gunships sent for (what they aim at is drawn for their owner)
	/** Aircraft on a delivery run with a DeliveryDecal (DeliverPayloadAIUpdate::m_deliveryDecal): drawn at the target for their owner. */
	const TArray<FGenObjectID>& GetDeliveryMarkers() const { return DeliveryMarkers; }
	TArray<FGenObjectID> DeliveryMarkers;
	/** GameLogic::selectObject: the logic wants the local player to have these selected (a gunship just sent for). */
	void TakeSelectRequests(TArray<FGenObjectID>& Out) { Out = MoveTemp(SelectRequests); SelectRequests.Reset(); }
	TArray<FGenObjectID> SelectRequests;
	/** A particle system of that name, by itself, at a spot. */
	void PlayParticleSystemAt(const FString& TemplateName, const FVector3f& Pos);
	/** A mine has been laid in this game (movers look for mines under them only then). */
	bool HasMines() const { return bAnyMines; }
	bool bAnyMines = false;
	/** The ropes of combat drops under way (W3DRopeDraw), and the ones let go that are still falling. */
	void GetVisibleRopes(TArray<FGenRopeDraw>& Out) const;
	// --- the Chinook's combat drop (GenContain.cpp) ---
	/** Soldiers may slide down into it: anything with room inside but a side's own base buildings. */
	bool CanCombatDropInto(const FGenObject& Building) const;
	/** Somebody on board that helicopter can slide down a rope (KINDOF_CAN_RAPPEL). */
	bool HasRappeller(const FGenObject& Container) const;
	void IssueCombatDrop(const TArray<FGenObjectID>& Units, FGenObjectID Target, const FVector3f& Pos);
	void UpdateFallingRopes();
	TArray<FGenFallingRope> FallingRopes;
	/** PartitionManager::findPositionAround, for the objects themselves: free ground near a spot. */
	FVector3f FindGroundSpotAround(const FVector3f& Center, float MinRadius, float MaxRadius) const { return FindPositionAround(Center, MinRadius, MaxRadius, GenSurface::Ground); }
	/** PartitionManager::findPositionAround with its default options: the nearest open ground with nothing on it
	 *  (where a helicopter sent to an airfield for repairs comes down), from a random angle. Center if none. */
	FVector3f FindFreeGroundAround(const FVector3f& Center, float MaxRadius);
	/** Where a named bone of the object's model is right now. */
	bool GetNamedBonePosition(const FGenObject& Obj, const FString& BoneName, FVector3f& OutWorld) const;
	void AttachParticleSystem(const FString& TemplateName, FGenObjectID Object);
	/** A particle system that stays with an object, at a spot in the object's own frame. Its id (0: none), for DestroyParticleSystem. */
	uint32 CreateObjectParticleSystem(const FString& TemplateName, FGenObjectID Object, const FVector3f& LocalOffset);
	void DestroyParticleSystem(uint32 SystemID);
	/** PartitionManager::doShroudReveal and undoShroudReveal at once: the player has seen that ground, and it is fog again. */
	void RevealAreaOnce(int32 Player, const FVector2f& Pos, float Range);
	/** ... and kept in view for so many frames (a special power's view object). */
	void RevealAreaFor(int32 Player, const FVector2f& Pos, float Range, uint32 Frames);
	/** GameClient::addScorch: a burn mark on the ground (Type: SCORCH_1 .. 4 as 0 .. 3). */
	void AddScorch(const FVector3f& Pos, float Radius, int32 Type);
	/** ... off its centre, and only for so many frames. */
	void AttachParticleSystemFor(const FString& TemplateName, FGenObjectID Object, const FVector3f& Offset, uint32 LifetimeFrames);
	/** TransitionDamageFX: FX lists and object creation lists for a body that just got worse and reached State. */
	void DoTransitionDamageFX(const FGenObject& Obj, int32 State);
	/** World transform (Generals space) of a weapon slot's fire-FX or launch bone on the object's model as it
	 *  is posed on the screen. For what is drawn only: the game thread's. */
	bool GetWeaponBoneTransform(const FGenObject& Obj, int32 Slot, bool bLaunchBone, FTransform3f& OutWorld) const;
	/** W3DModelDraw::getProjectileLaunchOffset: where a slot's projectiles leave from - its launch bone in
	 *  the model's base pose, turned with the turret. What the logic uses (it doesn't look at the screen). */
	bool GetWeaponLaunchTransform(const FGenObject& Obj, int32 Slot, FTransform3f& OutWorld, int32 Barrel = 0) const;
	/** W3DModelDraw::getBarrelCount: how many numbered launch bones the slot has in the look the object has now (0: one unnumbered, or none). */
	int32 GetWeaponBarrelCount(const FGenObject& Obj, int32 Slot) const;
	/** WeaponStore::createAndFireTempWeapon: that weapon fired from the object at a spot, without being one of its own. */
	void FireTempWeapon(const FGenWeaponTemplate& Weapon, FGenObject& Source, const FVector3f& TargetPos);

	// --- economy: money, power, production, construction (GenEconomy.cpp) ---
	int32 GetMoney(int32 Player) const { return Players.IsValidIndex(Player) ? Players[Player].Money : 0; }
	/** Power produced and consumed by the player's finished buildings. */
	void GetEnergy(int32 Player, int32& OutProduction, int32& OutConsumption) const;
	/** Frames a build takes for this player right now (ThingTemplate::calcTimeToBuild: slower when short on power). */
	float CalcTimeToBuild(const FGenThingTemplate& Template, int32 Player) const;
	/** Can the player build this now (prerequisites, money, the limit on its type)? If not, OutWhy says what is missing. */
	bool CanMake(int32 Player, const FGenThingTemplate& Template, FString* OutWhy = nullptr, bool bCheckMoney = true) const;
	/** Player::countExisting, for canBuildMoreOfType: how many of a type a player already has - what stands
	 *  in the world (with what its allies on the same team have, as the original's team iteration does),
	 *  plus what its factories are still making. */
	int32 CountOwnedOrQueued(int32 Player, const FGenThingTemplate& Template) const;
	/** BuildAssistant::isLocationLegalToBuild: clear, flat enough ground with nothing in the way. */
	/** BuildAssistant::isLocationLegalToBuild; OutWhy: the message for a refusal ("GUI:CantBuildNotFlatEnough"). */
	bool IsLocationLegalToBuild(const FGenThingTemplate& Template, const FVector3f& Pos, float Angle, const TCHAR** OutWhy = nullptr) const;
	// Orders (queued like the other commands)
	void IssueQueueUnit(FGenObjectID Factory, const FString& UnitTemplate);
	void IssueCancelUnit(FGenObjectID Factory, int32 QueueIndex);
	void IssueConstruct(FGenObjectID Dozer, const FString& BuildingTemplate, const FVector3f& Pos, float Angle);
	void IssueResumeConstruction(const TArray<FGenObjectID>& Dozers, FGenObjectID Building);
	// --- garrisons and transports (GenContain.cpp) ---
	/** ActionManager::canEnterObject: that unit may get into that transport, building or tunnel. */
	bool CanEnterObject(const FGenObject& Unit, const FGenObject& Container, bool bCheckCapacity = true) const;
	/** ActionManager::canEnterObject's special case for aircraft: that craft may come down at that airfield (a craft built on the helipad has no place there, so it may only be mended). */
	bool CanLandAtAirfield(const FGenObject& Unit, const FGenObject& Airfield) const;
	/** Sabotage...CrateCollide::isValidToExecute: the unit (a saboteur) has something to do to that building. */
	bool CanSabotage(const FGenObject& Unit, const FGenObject& Building) const;
	/** Sabotage...CrateCollide::executeCrateBehavior, every one that applies; the saboteur is gone afterwards. */
	void SabotageBuilding(FGenObject& Saboteur, FGenObject& Building);
	bool IsValidContainerFor(const FGenObject& Container, const FGenObject& Rider, bool bCheckCapacity) const;
	/** Who is inside (for a tunnel: everybody in the player's tunnel network). */
	const TArray<FGenObjectID>& GetRiders(const FGenObject& Container) const;
	/** OverlordContain::getRedirectedContain: an Overlord carrying a bunker is a transport of the bunker's
	 *  kind - whoever asks about its passengers is asking the bunker. Anything else is itself. */
	const FGenObject& ResolveContainer(const FGenObject& Container) const;
	/** The add-on riding in the open on a Helix or an Overlord (HelixContain::getPortableStructure). */
	FGenObject* GetPortableRider(const FGenObject& Container) const;
	/** getContainerPipsToShow: whether how full it is gets shown (an Overlord's own one slot never is). */
	bool ShowsContainPips(const FGenObject& Container) const;
	/** A garrisoned building with somebody inside that a flash-bang, a flame or a microwave can get at
	 *  (getContainCount > 0, isGarrisonable, !isImmuneToClearBuildingAttacks). */
	bool IsClearableGarrison(const FGenObject& Building) const;
	/** ActionManager::canSnipeVehicle: an enemy vehicle with a driver, on the ground, and no drone. */
	bool CanSnipeVehicle(const FGenObject& Sniper, const FGenObject& Target) const;
	/** ActionManager::canHijackVehicle / canConvertObjectToCarBomb: that soldier may take that vehicle over. */
	bool CanTakeOverVehicle(const FGenObject& Unit, const FGenObject& Vehicle) const;
	/** The player that owns what is nobody's (civilian buildings, crates, sniped vehicles), or INDEX_NONE. */
	int32 GetNeutralPlayer() const;
	/** Kills up to Count of those inside (of those kinds), in the order they went in; how many died. */
	int32 KillGarrisoned(FGenObject& Building, int32 Count, const TArray<FString>& RequiredKindOf, const TArray<FString>& ForbiddenKindOf, FGenObjectID Killer);
	/** AIGroup::groupAttackObject: "a garrisoned container capable of firing: order everyone inside to attack as well". */
	void OrderRidersToAttack(const FGenObject& Container, FGenObjectID Target, bool bForce = false);
	int32 GetContainMax(const FGenObject& Container) const;
	/** Slots in use (a vehicle in a transport takes several). */
	int32 GetContainCount(const FGenObject& Container) const;
	bool IsPassengerAllowedToFire(const FGenObject& Container, const FGenObject& Rider) const;
	void AddToContain(FGenObject& Container, FGenObject& Rider);
	/** OpenContain::removeFromContain: the rider is back in the world, where the container is. */
	void RemoveFromContain(FGenObject& Container, FGenObject& Rider);
	/** exitObjectViaDoor: out by the door and a few steps away. False while the door is busy (ExitDelay,
	    a helicopter that hasn't landed). */
	bool ExitViaDoor(FGenObject& Container, FGenObject& Rider);
	void UpdateContain(FGenObject& Container);
	/** OpenContain::onDie: the occupants are hurt or killed (DamagePercentToUnits) and the rest tumble out. */
	void OnContainerDied(FGenObject& Container);
	/** The occupant of a garrison takes the fire point nearest its target (GarrisonContain::trackTargets). */
	void PlaceAtGarrisonPoint(FGenObject& Container, FGenObject& Rider, const FGenObject* Victim);
	void IssueEnter(const TArray<FGenObjectID>& Units, FGenObjectID Container);
	void IssueExit(FGenObjectID Rider);
	void IssueEvacuate(FGenObjectID Container);
	/** A building about to be sold turns everybody out (the last tunnel too; otherwise they stay in the network). */
	void OnContainerSold(FGenObject& Container);

	// --- repair and rally points ---
	/** ActionManager::canRepairObject: this dozer may mend that building (damaged, not an enemy's). */
	bool CanRepairObject(const FGenObject& Dozer, const FGenObject& Target) const;
	/** ActionManager::canGetRepairedAt: this damaged vehicle may be mended at that repair pad. */
	bool CanGetRepairedAt(const FGenObject& Unit, const FGenObject& RepairDest) const;
	/** The first of the dozers that can goes to mend the building. */
	void IssueRepair(const TArray<FGenObjectID>& Dozers, FGenObjectID Building);
	void IssueGetRepaired(const TArray<FGenObjectID>& Units, FGenObjectID RepairDest);
	/** A building that sends what it produces to a rally point (KindOf AUTO_RALLYPOINT). */
	bool CanSetRallyPoint(const FGenObject& Obj) const;
	/** False (and nothing changes) when there is no way on foot from the building to the spot. */
	bool SetRallyPoint(FGenObject& Obj, const FVector3f& Pos);
	void IssueSetRallyPoint(const TArray<FGenObjectID>& Buildings, const FVector3f& Pos);
	void IssueSell(FGenObjectID Building);
	/** MSG_DOZER_CANCEL_CONSTRUCT: a building site of ours is given up - its price comes back, the site is destroyed. */
	void IssueCancelConstruction(FGenObjectID Building);
	/** MSG_TOGGLE_OVERCHARGE */
	void IssueToggleOvercharge(FGenObjectID Building);
	/** The strategy center's plan buttons (SPECIAL_POWER with OPTION_ONE / TWO / THREE): 1 bombardment, 2 hold
	 *  the line, 3 search and destroy. */
	void IssueBattlePlan(FGenObjectID Center, int32 Plan);
	/** Player::getBattlePlansActiveSpecific */
	int32 GetBattlePlansActive(int32 Player, int32 Plan) const;
	/** Player::changeBattlePlan: Bonus is in-out (taken back, it comes out inverted). */
	void ChangeBattlePlan(int32 Player, int32 Plan, int32 Delta, FGenBattlePlanBonuses& Bonus);
	/** localApplyBattlePlanBonusesToObject */
	void ApplyBattlePlanBonuses(FGenObject& Obj, const FGenBattlePlanBonuses& Bonus);
	void ApplyBattlePlanBonusesForPlayerObjects(int32 Player, const FGenBattlePlanBonuses& Bonus);
	/** Player::becomingTeamMember: what joins or leaves a player gains or loses the plans in force there. */
	void AddBattlePlanBonuses(FGenObject& Obj, int32 Player);
	void RemoveBattlePlanBonuses(FGenObject& Obj, int32 Player);
	/** One of an object's sounds that lasts as long as some state of its (an empty event ends it). */
	void SetObjectSound(const FGenObject& Obj, int32 Slot, const FString& Event);
	// Used by FGenObject
	void FinishSell(const FGenObject& Building);
	/** Player::onPowerBrownOutChange as its objects ask it: that player is short of power. */
	bool IsBrownOut(int32 Player) const { return Players.IsValidIndex(Player) && Players[Player].bBrownOut; }
	/**
	 * TerrainLogic::flattenTerrain: the ground under a building about to go up there is lowered to its
	 * mean height (never raised), a cell beyond its footprint all round.
	 */
	void FlattenTerrain(const FGenThingTemplate& Template, const FVector3f& Pos, float Angle);
	void SpawnProducedUnit(const FGenObject& Factory, const FGenThingTemplate& Unit, int32 ExitDoor = -1);
	/** DefaultProductionExitUpdate::exitObjectViaDoor: where what a building (without parking) lets out appears,
	 *  and the point a little beyond its door it heads for. */
	void GetFactoryExit(const FGenObject& Factory, FVector3f& OutCreate, FVector3f& OutRally);
	/** ... and aiFollowExitProductionPath: out by the door, then on to the building's rally point. */
	void FollowExitPath(const FGenObject& Factory, FGenObject& Unit, const FVector3f& Rally);
	void OnBuildingCompleted(FGenObject& Building);
	/** Object::setStatus, UNDER_CONSTRUCTION changing: the mines a building stands on - theirs go off, the others are taken away. */
	void ClearMinesUnder(const FGenObject& Building);

	// --- computer players (GenSkirmishAI.cpp); these act at once, they are called from inside the logic step ---
	/** BuildAssistant::buildObjectNow: pays for and places a building without checking the spot or the
	 *  prerequisites. With a dozer it is a construction site that dozer raises; without one it stands finished. */
	/** bRebuild: a GLA hole's building going up again - nothing is charged. */
	FGenObject* ConstructBuilding(FGenObject* Dozer, const FGenThingTemplate& Template, const FVector3f& Pos, float Angle, int32 Player, bool bRebuild = false);
	void ResumeConstruction(FGenObject& Dozer, FGenObject& Building);
	/** ProductionUpdate::queueCreateUnit: pays for the unit and adds it to the factory's queue. */
	bool QueueUnit(FGenObject& Factory, const FGenThingTemplate& Unit);
	/** BuildAssistant::isPossibleToMakeUnit: the factory offers the unit and its prerequisites are met (money aside). */
	bool CanFactoryMake(const FGenObject& Factory, const FGenThingTemplate& Unit) const;
	void AddMoney(int32 Player, int32 Amount);
	/** Money::deposit and Money::withdraw: what the player has, and the cash register's sound for the local player.
	 *  Withdraw takes no more than there is; it returns what it took. */
	void Deposit(int32 Player, int32 Amount, bool bPlaySound = true);
	int32 Withdraw(int32 Player, int32 Amount, bool bPlaySound = true);
	/** ThingTemplate::calcCostToBuild: what it costs this player (an oil refinery makes vehicles cheaper). */
	int32 CalcCostToBuild(int32 Player, const FGenThingTemplate& Template) const;
	void RefreshCostModifiers();

	// --- capture and tech buildings (GenAbilities.cpp) ---
	/** ActionManager::canCaptureBuilding */
	bool CanCaptureBuilding(const FGenObject& Unit, const FGenObject& Target, bool bCheckReady = true) const;
	/** Object::defect: the object goes over to another player (a captured building). */
	bool DefectObject(FGenObject& Obj, int32 NewOwner);
	/** Object::setTeam and onCapture: everything that follows from a change of owner. */
	void ChangeOwner(FGenObject& Obj, int32 NewOwner);
	bool IsFactionStructure(const FGenObject& Obj) const;
	/** Everything queued at a building is taken off and paid back. */
	void CancelAllProduction(FGenObject& Building);
	/** OpenContain::orderAllPassengersToExit */
	void EvacuateContainer(const FGenObject& Container);
	/** Every unit of the list that can capture buildings is sent to capture that one. */
	void IssueCapture(const TArray<FGenObjectID>& Units, FGenObjectID Target);
	/** A hacker's HACK_INTERNET button. */
	void IssueHackInternet(const TArray<FGenObjectID>& Units);
	/** ActionManager::canDoSpecialPowerAtObject (no target: canDoSpecialPower) for a unit's own ability. */
	bool CanUseAbilityOn(const FGenObject& Unit, int32 Module, const FGenObject* Target, bool bCheckReady = true) const;
	/** A FIRE_WEAPON button aimed at something: those of the units that have it attack with that weapon. */
	void IssueFireWeapon(const TArray<FGenObjectID>& Units, FGenObjectID Target, int32 Slot);
	void AddFloatingText(const FString& Text, const FVector3f& Pos, const FLinearColor& Color);
	/** A sound event by name, where the object is. */
	void PlaySoundAt(const FString& Event, const FGenObject& Obj);
	/** ... that belongs to the object (setObjectID): it goes where the object goes. */
	void PlayEventForObject(const FString& Event, const FGenObject& Obj);
	/** The sound a unit makes for as long as it prepares an ability (a hacker's typing); an empty event ends it. */
	void SetAbilityLoopSound(const FGenObject& Obj, const FString& Event);
	FGenSkirmishAI* GetAI(int32 Player) const;
	/** One line per computer player, for traces. */
	void DescribeAI(TArray<FString>& OutLines) const;

	// --- upgrades (GenUpgrades.cpp) ---
	bool HasUpgradeComplete(int32 Player, const FString& Upgrade) const;
	bool HasUpgradeInProduction(int32 Player, const FString& Upgrade) const;
	/** Can this building / unit start researching the upgrade now? If not, OutWhy says why. */
	bool CanQueueUpgrade(const FGenObject& Researcher, const FGenUpgradeTemplate& Upgrade, FString* OutWhy = nullptr) const;
	bool QueueUpgrade(FGenObject& Researcher, const FGenUpgradeTemplate& Upgrade);
	void IssueQueueUpgrade(FGenObjectID Researcher, const FString& Upgrade);
	/** The research is done: the object gets it, or the player and everything the player owns. */
	void FinishUpgrade(FGenObject& Researcher, const FGenUpgradeTemplate& Upgrade);
	void GrantPlayerUpgrade(int32 Player, const FGenUpgradeTemplate& Upgrade);
	void CancelUpgradeInProduction(int32 Player, const FGenUpgradeTemplate& Upgrade);

	// --- sciences and ranks (GenUpgrades.cpp) ---
	bool HasScience(int32 Player, const FString& Science) const;
	bool CanPurchaseScience(int32 Player, const FString& Science, FString* OutWhy = nullptr) const;
	bool PurchaseScience(int32 Player, const FString& Science);
	void IssuePurchaseScience(int32 Player, const FString& Science);
	/** Player::grantScience: given without spending points (an upgrade, a script). */
	bool GrantScience(int32 Player, const FString& Science);
	void AddSkillPoints(int32 Player, int32 Points);
	/** For the scripts: true once for each upgrade finished (or science acquired) under that name. */
	bool ConsumeScriptEvent(int32 Player, const FString& Name, bool bScience);
	/** Skill points and cash bounty for the killer's player. */
	void OnObjectKilled(const FGenObject& Victim, FGenObjectID Killer);

	// --- special powers (GenSpecialPowers.cpp) ---
	/** Index of the object's module for the power, or INDEX_NONE. */
	int32 FindPowerModule(const FGenObject& Obj, const FString& Power) const;
	/** Usable right now? OutReadyFrame: when its timer runs out; OutWhy: what is missing. */
	bool IsPowerReady(const FGenObject& Obj, int32 Module, uint32* OutReadyFrame = nullptr, FString* OutWhy = nullptr) const;
	/** One of the player's objects that has the power (a ready one if there is one). */
	FGenObject* FindPowerSource(int32 Player, const FString& Power, bool bMustBeReady, int32* OutModule = nullptr) const;
	void IssueSpecialPower(FGenObjectID Source, const FString& Power, const FVector3f& TargetPos, FGenObjectID TargetObj);
	bool FireSpecialPower(FGenObject& Source, int32 Module, const FVector3f& TargetPos, FGenObjectID TargetObj);
	/** ObjectCreationList::create for the lists powers and buildings use: things that appear, aircraft that deliver. */
	void RunPowerOCL(const FString& OCLName, const FGenObject& Source, const FVector3f& CreatePos, const FVector3f& TargetPos, const float* PlacedAngle = nullptr);
	/** The map's playable rectangle (world units), optionally with the border around it. */
	void GetMapExtent(FVector2f& OutLo, FVector2f& OutHi, bool bIncludeBorder) const;
	FVector3f FindEdgePoint(const FVector3f& Pos, bool bFarthest) const;

	// --- fog of war and radar (GenShroud.cpp, GenShroudView.cpp) ---
	const FGenShroud& GetShroud() const { return Shroud; }
	/** False with -GenNoShroud: the local player sees everything. */
	bool IsShroudOn() const { return bShroudOn; }
	/** Object::getShroudedStatus: what that player sees of the object. bInOutEverSeen is the player's memory of it. */
	EGenObjectShroud ComputeObjectShroud(const FGenObject& O, int32 Player, bool& bInOutEverSeen) const;
	/** The local player's view of it this instant (for things made since the last logic frame ended). */
	EGenObjectShroud GetLocalShroudNow(const FGenObject& O) const;
	/** Drawn for the local player: in view, a building remembered in the fog, or only just out of view. */
	bool IsVisibleToLocalPlayer(const FGenObject& O) const;
	/** A building in the fog: shown the way it was last seen, whatever has happened to it since. */
	bool IsFrozenInFog(const FGenObject& O) const;
	/** The local player is looking at that spot right now. */
	bool IsPositionClear(const FVector3f& Pos) const;
	bool HasRadar(int32 Player) const;
	/** Shows as a blip on the local player's radar. */
	bool IsOnRadar(const FGenObject& O) const;
	void CreateRadarEvent(const FVector3f& Pos, EGenRadarEvent Type, float SecondsToLive = 4.f);
	/** Radar::tryEvent: an event, unless there was one of the kind a moment ago. */
	bool TryRadarEvent(EGenRadarEvent Type, const FVector3f& Pos);
	void TryUnderAttackEvent(const FGenObject& Obj);
	bool GetLastRadarEventPos(FVector3f& OutPos) const;
	const TArray<FGenRadarEvent>& GetRadarEvents() const { return RadarEvents; }
	// --- sound (the audio manager does the playing; these are the game's occasions for it) ---
	UGenAudioManager* GetAudio() const { return Audio; }
	/** A weapon's FireSound, at the shooter (a loop for weapons that fire in a stream). */
	void DoWeaponFireSound(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon);
	/** One of MiscAudio.ini's notification sounds, for that player (heard only if it is the local one). */
	void PlayMiscSound(const TCHAR* MiscKey, int32 Player);
	/** The adviser has something to say to that player. */
	void EvaEvent(const TCHAR* EventName, int32 Player);
	/** InGameUI::message: a line of text at the top of the screen for the local player. */
	void AddScreenMessage(const FString& Text, const FLinearColor& Color = FLinearColor::White);
	/** ... from the game thread (the line is added with the next step's commands). */
	void IssueScreenMessage(const FString& Text);
	/** The adviser on a superweapon (Player::onStructureConstructionComplete, SpecialPowerModule::aboutToDoSpecialPower,
	 *  InGameUI's timers): Stage "Detected", "Launched" or "Ready"; Own / Ally / Enemy as the local player sees the
	 *  owner. Powers that have no such lines are passed over. */
	void EvaSuperweapon(const TCHAR* Stage, const FGenObject& Obj, const struct FGenSpecialPowerTemplate& Power);
	/** One of the object's own sounds or voices by its INI key ("VoiceCreated"); the first of the keys it has. */
	void PlayObjectSound(const FGenObject& Obj, std::initializer_list<const TCHAR*> Keys);

	/** One texel per fog-of-war cell (plus a border): brightness in RGB, darkness in alpha. Null without fog. */
	UTexture2D* GetShroudTexture() const { return ShroudTexture; }

	// --- stealth (GenStealth.cpp) ---
	/** How the local player sees it, as far as stealth goes. */
	EGenStealthLook GetStealthLook(const FGenObject& O) const;
	/** A detector looks round: what is stealthed within Range of it is shown up until its next look. Returns whether
	 *  anything stealthed was there at all ("foundSomeone": the loud ping). */
	bool DetectStealth(FGenObject& Detector, float Range, uint32 Rate);
	/** The GPS scrambler's circle: the grantor's side within Radius is stealthed. */
	void GrantStealth(const FGenObject& Grantor, float Radius);
	/** A stealthed unit has been shown up: idle enemies that can see the spot look for a target at once. */
	void WakeEnemiesNear(const FGenObject& Revealed);
	/** The player has a finished building of that kind. */
	bool HasFinishedKindOf(int32 Player, const TCHAR* Kind) const;
	bool IsPassengerAllowedToFireFrom(const FGenObject& Container) const;

	/** RiderChangeContain::onContaining / onRemoving: whoever is selected getting onto a combat cycle (or off it) hands
	 *  the selection over. Only the player controller reads these; they are kept for a second. */
	struct FSelectionHandoff
	{
		FGenObjectID From = GenInvalidObjectID;
		FGenObjectID To = GenInvalidObjectID;
		uint32 Frame = 0;
	};
	const TArray<FSelectionHandoff>& GetSelectionHandoffs() const { return SelectionHandoffs; }
	/** Pathfinder::createAWallFromMyFootprint / removeWallFromMyFootprint: a train at a station is in the way for a while. */
	void SetTemporaryObstacle(FGenObject& Obj, bool bOn);
	/** ReplaceObjectUpgrade::upgradeImplementation: the object is swapped for a new one of that template, finished. */
	void ReplaceWithObject(FGenObject& Obj, const FString& TemplateName);
	/** Weapon::processRequestAssistance / makeAssistanceRequest: those like the shooter, near it and free, join in. */
	void RequestAssistance(const FGenObject& Requester, FGenObject& Victim, float Range);
	/** AssistedTargetingUpdate::makeFeedbackLaser: a beam (an object with W3DLaserDraw) between two objects, for its lifetime. */
	void CreateFeedbackLaser(const FString& LaserTemplate, const FGenObject& From, const FGenObject& To);
	/** RebuildHoleExposeDie::onDie: the GLA building leaves a hole that will build it again. */
	void ExposeRebuildHole(FGenObject& Building, const FGenDieModule& Module);
	void AddSelectionHandoff(FGenObjectID From, FGenObjectID To);

	// --- supply gathering (GenSupply.cpp) ---
	/** ActionManager::canTransferSuppliesAt: can this gatherer load at / deliver to that dock right now? */
	bool CanTransferSuppliesAt(const FGenObject& Truck, const FGenObject& DockObj) const;
	/** Sends gatherers to a supply warehouse or supply center; they keep gathering from there on. */
	void IssueDock(const TArray<FGenObjectID>& Units, FGenObjectID DockId);
	// Used by FGenObject
	FGenObject* FindBestSupplyWarehouse(const FGenObject& Truck) const;
	FGenObject* FindBestSupplyCenter(const FGenObject& Truck) const;
	const FGenObject* FindClosestOwnedByKindOf(const FGenObject& From, const TCHAR* Kind) const;
	void DepositSupplies(int32 Player, int32 Value, const FGenObject& Docker);
	void SpawnBuildingUnits(const FGenObject& Building);
	/** Where an aircraft heading for InOutPos ends up: moved aside if another aircraft hovers or is going there. */
	void AdjustAirDestination(const FGenObject& Unit, FVector2f& InOutPos) const;
	const TArray<FGenFloatingText>& GetFloatingTexts() const { return FloatingTexts; }
	bool IsTracing() const { return bTrace; }

	// --- the path queue (Pathfinder::queueForPath / processPathfindQueue) ---
	/** The step has searched its share of path cells (gen.PathCellsPerStep; the original's 5000 a frame), or
	 *  units are already waiting their turn: a new request waits too. */
	bool ShouldQueuePath() const;
	void QueueForPath(FGenObjectID Unit);
	int32 GetPathQueueLength() const { return PathQueue.Num() - PathQueueHead; }

	// --- queries ---
	FGenPathfinder& GetPathfinder() { return Pathfinder; }
	/** Objects by where they are, as of the start of this logic step. */
	const FGenPartition& GetPartition() const { return Partition; }

	/**
	 * The objects a player owns, in the order they came to it (the dead and the projectiles among
	 * them). For those who ask about one player's things many times a step: the computer players'
	 * scripts count a player's units of a kind, add up its power and look for its things in an area,
	 * each of which was a walk through every object in the game. Not to be held on to across anything
	 * that makes, destroys or hands over an object.
	 */
	const TArray<FGenObject*>& GetObjectsOf(int32 Player) const;
	/** (FGenObject::SetOwner: the object has changed hands.) */
	void OwnerChanged(FGenObject& Obj, int32 OldOwner);
	const FGenMapData* GetMap() const;
	AGenMapActor* GetMapActor() const { return MapActor; }
	float GetGroundHeight(float X, float Y) const;
	/** The height an aircraft's preferred height counts from: the ground, or the water over it. */
	float GetFlightSurfaceHeight(float X, float Y) const;
	uint32 GetFrame() const { return Frame; }
	/** Logic time of the frame being rendered: the logic frame the screen blends towards plus how far it has got. */
	double GetRenderFrameTime() const { return double(PublishedFrame) + double(Alpha); }

	/** Render position/orientation of an object interpolated between logic frames (Generals units). */
	FVector3f GetRenderPosition(const FGenObject& Obj) const;
	/**
	 * RTS3DScene::castRay for one drawable: where the ray (world space) first meets the object's model as it
	 * is drawn now - all of its draw modules' models. A hit nearer than InOutT (in lengths of Dir) makes
	 * that less. Game thread.
	 */
	bool RaycastDrawable(FGenObjectID Id, const FVector& Origin, const FVector& Dir, double& InOutT) const;
	/** Drawable::onSelected -> flashAsSelected: it lights up white for a moment. Game thread. */
	void FlashSelected(FGenObjectID Id);
	float GetRenderOrientation(const FGenObject& Obj) const;
	/** ... and the whole of it, as GetTransform gives it for the logic. */
	FTransform3f GetRenderTransform(const FGenObject& Obj) const;

	/** Simulation speed multiplier (skirmish game speed). */
	void SetTimeScale(float Scale) { TimeScale = OrderSource ? 1.f : FMath::Clamp(Scale, 0.f, 8.f); }
	/** The game stands still (the in-game menu is up); sound and the picture carry on. (Not a network game: the
	 *  host's clock goes on.) */
	void SetPaused(bool bInPaused) { bUserPaused = bInPaused && !OrderSource; }
	bool IsPaused() const { return bUserPaused; }

	// --- the score (ScoreKeeper.cpp) ---
	/** Player::onUnitCreated / onStructureConstructionComplete */
	void ScoreObjectBuilt(const FGenObject& Obj);
	void ScoreMoneyEarned(int32 Player, int32 Amount);
	void ScoreMoneySpent(int32 Player, int32 Amount);

	static UGenGameLogic* Get(const UObject* WorldContext);

	// --- the simulation's thread (GenSimThread.h) ---
	/**
	 * Game-thread code that reads the logic and isn't called from the world's tick (the HUD's drawing):
	 * in the logic for as long as this lives. (Everything from the start of the world's tick to the end
	 * of the logic's own is in it anyway.)
	 */
	struct GENERALS1_API FGameScope
	{
		explicit FGameScope(UGenGameLogic* InLogic);
		~FGameScope();
		UGenGameLogic* Logic;
	};
	/** A safe point of the step: the game thread gets its turn if it is waiting for one. Where: for -GenPerf,
	 *  which tells where the step went longest without one. */
	void SimYield(const TCHAR* Where = TEXT("?"));
	/** Game thread: lets the step under way finish first. For what changes the simulation by hand, not
	 *  through the queue of orders (test hooks): that may only be done between two steps. */
	void WaitForStep();
	/** One step (on the simulation's thread, or on the game thread when there is none). */
	void RunStep();
	/** (The pathfinder's: see FGenPathfinder::SetDetach.) */
	bool BeginDetachedWork();
	void EndDetachedWork();
	/** How long the last steps took from being asked for to being done, at their worst lately (seconds). */
	double GetStepPeakSeconds() const { return StepPeakSeconds; }

private:
	void StepLogic();
	FGenSimThread* SimThread = nullptr;   // null: steps run on the game thread
	bool bGateHeldForFrame = false;       // the game thread is in the logic since this frame's world tick began
	double StepPeakSeconds = 0.0;
	double StepMeanSeconds = 0.0, StepDevSeconds = 0.0;   // what steps take from asked to done, lately: the usual, and how far off it they are
	uint32 StepStatsFrame = 0;
	double LastYieldTime = 0.0;           // (-GenPerf) when the step last was at a safe point
	double LongestStretch = 0.0;          // ... the longest it went without one, and what it was doing
	const TCHAR* LongestStretchWhere = TEXT("");
	FString StretchDetail, LongestStretchDetail;   // (the script being run, for one)
public:
	void SetStretchDetail(const FString& Detail) { if (GenPerf::bEnabled) { StretchDetail = Detail; } }
private:
	FDelegateHandle WorldTickStartHandle, WorldTearDownHandle;
	void OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void StartSimThread();
	void StopSimThread();
	/** Finds overlapping ground units and lets them react (block, step aside, crush); see GenCollision.cpp. */
	void UpdateCollisions();
	/** The bodies of the vehicles on the ground (GenChassis.cpp), once everything has moved and collided. */
	void UpdateChassis();
	FRandomStream ClientRandom{ 0x0BADCAFE }; // GameClientRandomValue: for what only the screen shows, apart from the logic's numbers
	void UpdateDrawables();
	// --- between the step and the screen ---
	// The step doesn't touch what is drawn or heard (it may be running on a thread of its own): it
	// leaves notes, and the game thread's drawing pass acts on them.
	struct FPendingRemoval
	{
		FGenObjectID ID = GenInvalidObjectID;
		bool bFrozenInFog = false;      // destroyed out of sight: what was last seen of it stays
		TArray<int32> Cells;            // ... until somebody looks at these
	};
	struct FPendingFireFX
	{
		FGenObjectID Shooter = GenInvalidObjectID;
		int32 Slot = 0;
		const FGenWeaponTemplate* Weapon = nullptr;
		FVector3f TargetPos = FVector3f::ZeroVector;
		FTransform3f ShooterXf = FTransform3f::Identity;
	};
	TArray<FGenObjectID> PendingVisuals;        // made, and no model yet
	TArray<FGenObjectID> RemakeVisuals;         // its look has changed to another template's (a disguise): made anew
	TMap<FGenObjectID, FString> SoundOnly;      // what has no model but a sound of its own (the map's Amb_ objects): the loop playing
	/** The ambient sound an object makes, the map's word on it included (Object::updateObjValuesFromMapProperties:
	 *  "Enable permanently looping sounds, disable one-shot sounds by default"). */
	FString GetAmbientSound(const FGenObject& O, const FString& TemplateSound) const;
	/** The player whose colours the local player sees it in (an enemy's disguise: the one it pretends to be). */
	int32 GetColorPlayer(const FGenObject& O) const;
	TArray<FPendingRemoval> PendingRemovals;    // gone, and the model still there
	TArray<FPendingFireFX> PendingFireFX;       // weapons fired: their flashes go on the model's bones as drawn
	void CreatePendingVisuals();
	/** How long the drawing pass may spend in one frame on building models, and again on changing models'
	 *  looks (seconds; 0: as long as it takes - the start of a game). The rest waits for the next frame. */
	double DrawBudgetSeconds = 0.0;
	void RemovePendingVisuals();
	void PlayPendingFireFX();
	/** The step is finished: what is drawn moves on to it. */
	void PublishFrame();
	void PlaceStartingObjects(const FGenPlayer& Player);
	void InitPlayerSciences(FGenPlayer& Player);
	bool AddScience(int32 Player, const FString& Science);
	void StartDelivery(const struct FGenDeliverPayload& Payload, const FGenObject& Source, const FVector3f& Primary, const FVector3f& Secondary);
	void SpawnMapObjects();
	FVector3f FindPositionAround(const FVector3f& Center, float MinRadius, float MaxRadius, uint32 Surfaces) const;
	void OnContainChanged(FGenObject& Container);
	void UpdateVictory();
	void InitShroud();
	void UpdateShroud();
	void UpdateObjectLook(FGenObject& O);
	void ForgetObjectLook(FGenObject& O);
	void GetObjectCells(const FGenThingTemplate& T, const FVector3f& P, float Angle, TArray<int32, TInlineAllocator<16>>& Out) const;
	void InitShroudView();
	void UpdateShroudView(float DeltaSeconds);
	void UpdateGhosts();
	void ClearGhosts();
	FGenShroud Shroud;
	bool bShroudOn = true;
	bool bUserPaused = false;
	/** Object::scoreTheKill's first half: the victim's player lost it, the killer's destroyed it. */
	void ScoreKill(const FGenObject& Victim, const FGenObject& Killer);
	/** OpenContain's m_loadSoundsEnabled: off while a transport is given the passengers it comes with. */
	bool bLoadSoundsEnabled = true;
	TArray<FSelectionHandoff> SelectionHandoffs;
	TArray<FGenRadarEvent> RadarEvents;
	bool bHasLastRadarEvent = false;
	FVector3f LastRadarEventPos = FVector3f::ZeroVector;
	// --- group moves (GenGroupMove.cpp) ---
	bool GroupGroundPath(const TArray<FGenObject*>& Members, const FVector2f& GroupMin, const FVector2f& GroupMax, const FVector2f& GroupCenter,
		const FVector3f& Goal, bool bForColumns, TArray<FVector2f>& OutPath);
	bool GroupMoveColumns(const TArray<FGenObject*>& Members, const FVector2f& GroupCenter, const TArray<FVector2f>& GroundPath, const FVector3f& Goal, bool bInfantry);
	void GroupMoveFormation(const TArray<FGenObject*>& Members, const TArray<FVector2f>& GroundPath, const FVector3f& Goal);
	void GroupTighten(const TArray<FGenObject*>& Members, const FVector3f& Goal);
	void GroupOrderMove(FGenObject& Unit, const FVector3f& Dest);
	void GroupOrderWaypoint(FGenObject& Unit, const FVector2f& Dest);
	void GroupOrderPath(FGenObject& Unit, const TArray<FVector2f>& Points, int32 EndLayer);
	uint32 NextFormationID = 1;
	/** The movers among the units, each with its own spot around the goal (so a group doesn't fight over one). */
	TArray<TPair<FGenObject*, FVector3f>> SpreadGroup(const TArray<FGenObjectID>& Units, const FVector3f& Goal) const;
	bool bGameOver = false;
	uint32 GameOverFrame = 0;
	int32 WinningPlayer = INDEX_NONE;
	FGenObject* FindOtherTunnel(const FGenObject& Tunnel) const;
	/** W3DDebrisDraw::setModelName: gives an object created without a model (GenericDebris) its visual. */
	void SetDebrisModel(FGenObject& Obj, const FString& Model, bool bUseHouseColor);

	UPROPERTY(Transient)
	TObjectPtr<AGenMapActor> MapActor;

	UPROPERTY(Transient)
	TObjectPtr<UGenFXManager> FX;

	UPROPERTY(Transient)
	TObjectPtr<UGenAudioManager> Audio;

	TArray<FGenPlayer> Players;
	int32 LocalPlayer = 1;
	TArray<TUniquePtr<FGenSkirmishAI>> AIPlayers;

	TMap<FGenObjectID, TUniquePtr<FGenObject>> Objects;
	TArray<FGenObjectID> ObjectOrder;
	FGenObjectID NextID = 1;

	UPROPERTY(Transient)
	TMap<uint32, TObjectPtr<UGenModelComponent>> Drawables;

	/**
	 * Models whose objects are gone - a shell that has landed, wreckage that has sunk into the ground -
	 * hidden and kept, by model name, for the next object that looks the same. A battle makes and
	 * unmakes hundreds of these a minute; making one is several engine objects created and registered,
	 * and unmaking it is garbage for the engine to collect.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UGenModelComponent>> PooledModels;
	TMap<FString, TArray<int32>> PoolByName;   // lower-case model name -> indices into PooledModels (null where taken)
	TArray<int32> PoolFree;
	UGenModelComponent* TakePooledModel(const FString& ModelName);
	bool PoolModel(UGenModelComponent* Model);

	/** A draw module besides the primary one (cargo slung under a helicopter, the boxes of a warehouse): its own model on the same object. */
	struct FExtraDraw
	{
		int32 DrawIndex = INDEX_NONE;     // index into the template's Draws
		TWeakObjectPtr<UGenModelComponent> Model;
		FGenDrawInstance State;           // the condition state it shows, and the one waiting behind a transition
		float AppliedRelZ = 0.f;          // how far below (or above) the object's first model it stands (ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT)
		const FGenModelConditionState* StateSystemsFor = nullptr; // the state the ParticleSysBone systems below belong to
		TArray<uint32> StateSystems;
	};

	/** Client-side state of one drawable: condition state shown, turret pose, muzzle flash timers. */
	struct FDrawState
	{
		const FGenObject* Object = nullptr;   // (both live until RemovePendingVisuals takes the state away)
		UGenModelComponent* Model = nullptr;
		const FGenThingTemplate* DrawTemplate = nullptr; // whose draw modules made it (a disguised bomb truck: the vehicle it looks like)
		bool bBusy = false;               // in the middle of something the screen follows frame by frame (recoil, shimmer)
		bool bFrozen = false;             // a building in the fog: it keeps the look it had
		bool bHasTurret = false;
		bool bRider = false;              // W3DDependencyModelDraw: drawn on a bone of what carries it (every frame)
		bool bAtRest = false;             // standing still, and its model is where it stands
		bool bStatePending = false;       // its look has changed and the frame it was noticed in had done enough: next frame
		TArray<FExtraDraw> Extras;
		uint32 SubObjectVersion = 0;      // upgrade show / hide overrides applied so far
		int32 SupplyBones = -1;           // W3DSupplyDraw: box bones on the model (-1 = not counted yet)
		int32 SupplyShown = -1;           // ... and how many of them currently show
		FGenDrawInstance State;           // the first draw module's condition state, and the one waiting behind a transition
		bool bStateBusy = false;          // one of its draw modules is in a transition (or has an animation that restarts itself)
		float AppliedAdjustZ = 0.f;       // ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT of the first model, as its place has it
		uint32 AnimDurationVersion = 0;   // the object's last "setAnimationLoopDuration" applied
		uint32 ManualFrameVersion = 0;    // ... and the last frame the logic put a MANUAL animation at
		uint32 AppliedVersion = 0;
		bool bPlaced = false;             // the drawable has been given the object's transform at least once
		FVector AppliedPos = FVector::ZeroVector; // ... and where that last put it (relative to the map)
		bool bHidden = false;             // inside a container, or out of the local player's sight
		float StealthFade = 0.f;          // the stealth look applied so far
		float StealthGlow = 0.f;
		double FlashStart = 0.0;          // when it was selected (flashAsSelected), while the flash lasts
		float AppliedFlash = 0.f;
		// The sounds it keeps making: its engine while it moves, its hum (by damage state), its building site.
		bool bSoundsResolved = false;
		FString MoveStartSound, MoveLoopSound, MoveStartDamagedSound, MoveLoopDamagedSound, ConstructionSound;
		FString AmbientSound[4];
		bool bSoundMoving = false;
		bool bSoundTurret = false;      // TurretMoveLoop playing (the turret turned or tilted in the last step)
		FString TurretLoopSound;
		int32 SoundAmbient = -1;          // which ambient plays: -1 none, 0..3 damage state, 4 building site
		int32 SoundDamageState = 0;
		int32 Owner = MIN_int32;          // whose colours it wears (a garrisoned building changes hands)
		const FGenModelConditionState* StateSystemsFor = nullptr; // condition state the ParticleSysBone systems below belong to
		TArray<uint32> StateSystems;
		bool bSystemsStopped = false;     // hidden: the ParticleSysBone systems (the extras' too) emit nothing
		int32 DamageState = 0;            // body damage state the TransitionDamageFX systems below were made for
		TArray<uint32> TransitionSystems;
		int32 DebrisAnimState = -1;       // W3DDebrisDraw: -1 not begun, 0 its first animation, 1 flying, 2 landed
		uint32 DebrisAnimFrame = 0;       // logic frame it began
		float AppliedTurretPitch = 0.f;
		int32 ClipSpent[3] = { 0, 0, 0 };  // shots of the clip whose launch bones' sub-objects are hidden
		const FGenModelConditionState* ClipSpentState = nullptr; // ... in this look of the model
		int32 BodySystemsKey = -1;        // damage state and "aflame" the fire and smoke below were made for
		TArray<uint32> BodySystems;
		float AppliedTurret = 1e9f;
		uint32 SeenFireCount[3] = { 0, 0, 0 };
		double MuzzleHideTime[3] = { 0.0, 0.0, 0.0 };
		// W3DModelDraw WeaponRecoilInfo per weapon slot (0 idle, 1 recoiling, 2 settling)
		uint8 RecoilState[3] = { 0, 0, 0 };
		float RecoilRate[3] = { 0.f, 0.f, 0.f };
		float RecoilShift[3] = { 0.f, 0.f, 0.f };
		float RecoilPrevShift[3] = { 0.f, 0.f, 0.f };
		float RecoilClock[3] = { 0.f, 0.f, 0.f };   // fraction of a logic frame since the last recoil step
		// W3DTankDraw / W3DTruckDraw (GenVehicleDraw.cpp): how far its wheels, cab and treads have turned, and the
		// dirt it throws up (tread debris left and right, dust, dirt spray, powerslide spray).
		float FrontWheelRotation = 0.f, RearWheelRotation = 0.f;
		float CabRotation = 0.f, TrailerRotation = 0.f;
		float TreadOffset[3] = { 0.f, 0.f, 0.f };   // left, right, middle
		FVector2f TreadLastDir = FVector2f(1.f, 0.f);
		bool bPowersliding = false;
		uint32 LandingFrame = 0;                     // the last landing (FWheelInfo::LandingSerial) reacted to
		uint32 VehicleSystems[5] = { 0, 0, 0, 0, 0 };
		uint32 VehicleTraceFrame = 0;
	};
	TMap<uint32, FDrawState> DrawStates;
public:
	/** (Test harness) the model an object is drawn with, its animation, the frame it is on. */
	FString DescribeObjectModel(FGenObjectID Id) const;
private:
	/** One drawn frame of a vehicle's wheels, treads and dirt (GenVehicleDraw.cpp); DeltaFrames: logic frames since the last. */
	void UpdateVehicleDraw(FDrawState& DS, const FGenObject& O, const FGenDrawModule& Draw, float DeltaFrames);
	/** Hidden, or out of sight in the fog: its dirt stops, its powerslide goes quiet. */
	void StopVehicleEffects(FDrawState& DS, const FGenObject& O);
	void DestroyVehicleEffects(FDrawState& DS);
	void TraceVehicleDraw(FDrawState& DS, const FGenObject& O);
	class FGenParticleSystem* GetVehicleSystem(FDrawState& DS, int32 Which, const FString& TemplateName, const FGenObject& O, bool bCreate);

	/** The fog of war as the local player's screen shows it (W3DShroud): levels fade towards what the logic says. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ShroudTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ShroudMaterial;

	TArray<uint8> ShroudCurrent;                    // m_currentFogData (border included)
	TArray<uint8> ShroudFinal;                      // m_finalFogData
	uint32 ShroudViewSerial = 0;
	float ShroudFadeCarry = 0.f;

	/** What is left on screen of a building that was destroyed out of sight: it stays until somebody looks (GhostObject). */
	struct FGhost
	{
		TWeakObjectPtr<UGenModelComponent> Model;
		TArray<TWeakObjectPtr<UGenModelComponent>> Extras;
		TArray<int32> Cells;
	};
	TArray<FGhost> Ghosts;
	bool bNight = false;
	double LastDrawFrameTime = 0.0;                 // logic frame + alpha at the previous drawable update

	struct FDelayedDamage
	{
		const FGenWeaponTemplate* Weapon = nullptr;
		FGenObjectID Source = GenInvalidObjectID;
		FGenObjectID Victim = GenInvalidObjectID;
		FVector3f Pos = FVector3f::ZeroVector;
		uint32 Frame = 0;
	};
	TArray<FDelayedDamage> DelayedDamage;
	TArray<FGenFloatingText> FloatingTexts;
	FRandomStream Random;

	FGenPathfinder Pathfinder;
	FGenPartition Partition;
	TArray<FGenObject*> StepObjects;     // the objects of this step, in update order (null: removed during it)
	mutable TArray<TArray<FGenObject*>> OwnedObjects; // by player; kept up as objects come, go and change hands
	mutable bool bOwnedDirty = true;                  // ... once it has been made (when first asked for)
	/** Objects whose footprint is currently registered with the pathfinder. */
	TSet<FGenObjectID> ObstacleOwners;

	// --- orders (GenOrders.cpp; each order's own part where the rest of its business is) ---
	TArray<FGenCommand> PendingCommands;       // this computer's orders, for the next logic frame
	TArray<FGenCommand> SimCommands;           // the simulation's own, for the next logic frame
	TArray<TFunction<void()>> PendingLocal;    // the interface's own (a line of text): nobody else's business
	TArray<uint32> DumpFrames;                 // -GenStateDump=F,...: the state is written out after these frames
	FString DumpTag;                           // -GenDumpTag=: tells the dumps of two runs apart
	FGenMatchSettings Match;
	TUniquePtr<FGenReplay> Recording;
	TUniquePtr<FGenReplay> Playback;
	int32 PlaybackNext = 0;                    // the next of its orders
	uint32 PlaybackMismatchFrame = 0;
	bool bSetupPhase = false;                  // between StartSkirmish and the first step: what is made belongs to the start
	bool bPowersReady = false;                 // (tests) special powers don't wait for their timers
	IGenOrderSource* OrderSource = nullptr;    // a network game's host
	bool bCatchingUpNow = false;
	/** Far behind the host: steps run here, one after another, for a few milliseconds of this frame. */
	void CatchUp();
	/** The host's word on a player: "drop" (gone from the game: its allies may command its army), "return". */
	void ExecSession(const FGenCommand& C);
	/** The orders of the frame about to be worked out: applied, and noted in the recording. */
	void ApplyCommands();
	/** After a step: its checksum is noted (and, played back, compared). */
	void CheckStep();
	void ExecuteCommand(const FGenCommand& C);
	void ExecMove(const FGenCommand& C);
	void ExecAddWaypoint(const FGenCommand& C);
	void ExecAttackMove(const FGenCommand& C);
	void ExecGuard(const FGenCommand& C);
	void ExecStop(const FGenCommand& C);
	void ExecFireWeaponAt(const FGenCommand& C);
	void ExecForceAttack(const FGenCommand& C);
	void ExecForceAttackGround(const FGenCommand& C);
	void ExecSwitchWeapon(const FGenCommand& C);
	void ExecCreateFormation(const FGenCommand& C);
	void ExecLand(const FGenCommand& C);
	void ExecAttack(const FGenCommand& C);
	void ExecSteerGunship(const FGenCommand& C);
	void ExecCombatDrop(const FGenCommand& C);
	void ExecEnter(const FGenCommand& C);
	void ExecExit(const FGenCommand& C);
	void ExecEvacuate(const FGenCommand& C);
	void ExecCapture(const FGenCommand& C);
	void ExecHackInternet(const FGenCommand& C);
	void ExecFireWeapon(const FGenCommand& C);
	void ExecSpecialPower(const FGenCommand& C);
	void ExecDock(const FGenCommand& C);
	void ExecQueueUnit(const FGenCommand& C);
	void ExecCancelUnit(const FGenCommand& C);
	void ExecConstruct(const FGenCommand& C);
	void ExecResumeConstruction(const FGenCommand& C);
	void ExecRepair(const FGenCommand& C);
	void ExecGetRepaired(const FGenCommand& C);
	void ExecSetRallyPoint(const FGenCommand& C);
	void ExecSell(const FGenCommand& C);
	void ExecCancelConstruction(const FGenCommand& C);
	void ExecToggleOvercharge(const FGenCommand& C);
	void ExecBattlePlan(const FGenCommand& C);
	void ExecQueueUpgrade(const FGenCommand& C);
	void ExecCheat(const FGenCommand& C);
	TArray<FIntRect> PendingTerrain;     // height map cells the step has changed, for the terrain's mesh (game thread: ApplyPendingTerrain)
	void ApplyPendingTerrain();
	TArray<FGenObjectID> PathQueue;      // units waiting for a path, first come first served
	int32 PathQueueHead = 0;
	uint32 LastMoveGroup = 0;   // the last group move order's number (FGenObject::MoveGroup)
	bool bProcessingPathQueue = false;
	void ProcessPathQueue();

	// Test automation: -GenTrace logs combat events, -GenPauseFrame=N freezes the simulation at logic frame N.
	bool bTrace = false;
	uint32 PauseAtFrame = 0;

	uint32 Frame = 0;                // the logic frame the step has got to (or is working on)
	uint32 CompletedFrame = 0;       // ... has finished
	uint32 PublishedFrame = 0;       // ... the screen is blending towards
	double SimClock = 0.0;           // game time in logic frames: frame N is due when it reaches N
	double RenderDelay = 0.0;        // how far behind that the screen runs (logic frames)
	float TimeScale = 1.f;
	/** Queues the models and animations of what the sides in this game can build (see UGenAssetSubsystem::QueuePreloadModel). */
	void QueuePreloads();
	// -GenPerf
	void ReportPerf(float DeltaTime);
	double PerfStartReal = 0.0;
	double PerfMaxFrame = 0.0;
	uint32 PerfNextFrame = 0;
	int32 PerfSteps = 0;
	int32 PerfFrames = 0;
	int32 PerfHitches33 = 0, PerfHitches50 = 0, PerfHitches100 = 0;
	int32 PerfOver42 = 0, PerfOver5 = 0, PerfOver83 = 0;   // frames that took longer than a 240 Hz / 200 Hz / 120 Hz screen gives them
	int32 PerfHolds = 0;                                   // frames in which a late step held the blend
	double PerfGameMs = 0.0, PerfRenderMs = 0.0, PerfGPUMs = 0.0, PerfDrawCalls = 0.0, PerfTriangles = 0.0;
	float Alpha = 0.f;
};
