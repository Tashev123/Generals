#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenObject.h"
#include "Logic/GenPathfinder.h"
#include "Logic/GenSkirmishAI.h"
#include "GenGameLogic.generated.h"

class AGenMapActor;
class FGenMapData;
class UGenFXManager;
class UGenModelComponent;
struct FGenPlayerTemplate;
struct FGenThingTemplate;
struct FGenWeaponTemplate;
struct FGenDrawModule;

/** A participant (Player.cpp). Index 0 is the neutral/civilian player that owns map objects. */
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
	bool bDefeated = false;     // has lost every building that counts: out of the game

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

/** One skirmish slot as chosen in the setup screen. */
struct GENERALS1_API FGenSkirmishSlot
{
	FString Faction = TEXT("FactionAmerica");   // PlayerTemplate name
	bool bHuman = false;
	int32 StartPosition = -1;                    // -1 = next free
	int32 Team = -1;
	FLinearColor Color = FLinearColor::Transparent; // transparent = faction preferred color
};

/**
 * The simulation (GameLogic.cpp equivalent).
 *
 * Logic runs at a fixed rate (gen.LogicFPS) from an accumulator; rendering interpolates objects
 * between the previous and current logic frames, so the game renders at any frame rate. Player
 * commands are queued and applied at the start of the next logic frame (like the original's
 * message stream), which keeps the door open for lockstep multiplayer and replays.
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

	bool StartSkirmish(AGenMapActor* InMapActor, const TArray<FGenSkirmishSlot>& Slots);
	bool IsRunning() const { return MapActor != nullptr; }

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
	/** Aircraft fly to that airfield, land and park there (planes also reload; helicopters get repaired). */
	void IssueLand(const TArray<FGenObjectID>& Units, FGenObjectID Airfield);

	// --- combat services (used by FGenObject) ---
	/** WeaponTemplate::dealDamageInternal: single victim or everything within the damage radius. */
	void DealDamage(const FGenWeaponTemplate& Weapon, FGenObjectID Source, FGenObjectID Victim, const FVector3f& Pos, bool bProjectileDetonation);
	/** WeaponStore::setDelayedDamage: instant-hit weapons land after their travel time. */
	void QueueDelayedDamage(const FGenWeaponTemplate& Weapon, FGenObjectID Source, FGenObjectID Victim, const FVector3f& Pos, uint32 WhenFrame);
	/** Creates the weapon's ProjectileObject and launches it. */
	void SpawnProjectile(const FGenWeaponTemplate& Weapon, const FGenObject& Shooter, FGenObjectID Victim, const FVector3f& TargetPos,
		const FVector3f& LaunchPos, float Angle);
	void OnObjectDied(const FGenObject& Obj);
	/** Deterministic random stream for the simulation (GameLogicRandomValue). */
	FRandomStream& GetRandom() { return Random; }

	// --- effects (FXList / ObjectCreationList), by INI name; unknown or empty names are ignored ---
	void DoFXObj(const FString& FXName, const FGenObject& Primary, const FGenObject* Secondary = nullptr);
	void DoFXPos(const FString& FXName, const FVector3f& Pos, const FTransform3f* Xf, float Speed, const FVector3f* Secondary, float Radius);
	/** Weapon FireFX at the shooter's WeaponFireFXBone (or at the shooter when the model has none). */
	void DoWeaponFireFX(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon, const FVector3f& TargetPos);
	/** ObjectCreationList::create: debris and objects (hulks) thrown out of the source object. */
	void DoOCL(const FString& OCLName, const FGenObject& Source);
	void AttachParticleSystem(const FString& TemplateName, FGenObjectID Object);
	/** TransitionDamageFX: FX lists and object creation lists for a body that just got worse and reached State. */
	void DoTransitionDamageFX(const FGenObject& Obj, int32 State);
	/** World transform (Generals space) of a weapon slot's fire-FX or launch bone on the object's current model. */
	bool GetWeaponBoneTransform(const FGenObject& Obj, int32 Slot, bool bLaunchBone, FTransform3f& OutWorld) const;

	// --- economy: money, power, production, construction (GenEconomy.cpp) ---
	int32 GetMoney(int32 Player) const { return Players.IsValidIndex(Player) ? Players[Player].Money : 0; }
	/** Power produced and consumed by the player's finished buildings. */
	void GetEnergy(int32 Player, int32& OutProduction, int32& OutConsumption) const;
	/** Frames a build takes for this player right now (ThingTemplate::calcTimeToBuild: slower when short on power). */
	float CalcTimeToBuild(const FGenThingTemplate& Template, int32 Player) const;
	/** Can the player build this now (prerequisites, money)? If not, OutWhy says what is missing. */
	bool CanMake(int32 Player, const FGenThingTemplate& Template, FString* OutWhy = nullptr, bool bCheckMoney = true) const;
	/** BuildAssistant::isLocationLegalToBuild: clear, flat enough ground with nothing in the way. */
	bool IsLocationLegalToBuild(const FGenThingTemplate& Template, const FVector3f& Pos, float Angle) const;
	// Orders (queued like the other commands)
	void IssueQueueUnit(FGenObjectID Factory, const FString& UnitTemplate);
	void IssueCancelUnit(FGenObjectID Factory, int32 QueueIndex);
	void IssueConstruct(FGenObjectID Dozer, const FString& BuildingTemplate, const FVector3f& Pos, float Angle);
	void IssueResumeConstruction(const TArray<FGenObjectID>& Dozers, FGenObjectID Building);
	// --- garrisons and transports (GenContain.cpp) ---
	/** ActionManager::canEnterObject: that unit may get into that transport, building or tunnel. */
	bool CanEnterObject(const FGenObject& Unit, const FGenObject& Container, bool bCheckCapacity = true) const;
	bool IsValidContainerFor(const FGenObject& Container, const FGenObject& Rider, bool bCheckCapacity) const;
	/** Who is inside (for a tunnel: everybody in the player's tunnel network). */
	const TArray<FGenObjectID>& GetRiders(const FGenObject& Container) const;
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
	// Used by FGenObject
	void FinishSell(const FGenObject& Building);
	void SpawnProducedUnit(const FGenObject& Factory, const FGenThingTemplate& Unit, int32 ExitDoor = -1);
	void OnBuildingCompleted(FGenObject& Building);

	// --- computer players (GenSkirmishAI.cpp); these act at once, they are called from inside the logic step ---
	/** BuildAssistant::buildObjectNow: pays for and places a building without checking the spot or the
	 *  prerequisites. With a dozer it is a construction site that dozer raises; without one it stands finished. */
	FGenObject* ConstructBuilding(FGenObject* Dozer, const FGenThingTemplate& Template, const FVector3f& Pos, float Angle, int32 Player);
	void ResumeConstruction(FGenObject& Dozer, FGenObject& Building);
	/** ProductionUpdate::queueCreateUnit: pays for the unit and adds it to the factory's queue. */
	bool QueueUnit(FGenObject& Factory, const FGenThingTemplate& Unit);
	/** BuildAssistant::isPossibleToMakeUnit: the factory offers the unit and its prerequisites are met (money aside). */
	bool CanFactoryMake(const FGenObject& Factory, const FGenThingTemplate& Unit) const;
	void AddMoney(int32 Player, int32 Amount);
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
	/** The map's playable rectangle (world units), optionally with the border around it. */
	void GetMapExtent(FVector2f& OutLo, FVector2f& OutHi, bool bIncludeBorder) const;
	FVector3f FindEdgePoint(const FVector3f& Pos, bool bFarthest) const;

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

	// --- queries ---
	FGenPathfinder& GetPathfinder() { return Pathfinder; }
	const FGenMapData* GetMap() const;
	AGenMapActor* GetMapActor() const { return MapActor; }
	float GetGroundHeight(float X, float Y) const;
	uint32 GetFrame() const { return Frame; }
	/** Logic time of the frame being rendered: the current logic frame plus the fraction of the next one. */
	double GetRenderFrameTime() const { return double(Frame) + double(Alpha); }

	/** Render position/orientation of an object interpolated between logic frames (Generals units). */
	FVector3f GetRenderPosition(const FGenObject& Obj) const;
	float GetRenderOrientation(const FGenObject& Obj) const;

	/** Simulation speed multiplier (skirmish game speed). */
	void SetTimeScale(float Scale) { TimeScale = FMath::Clamp(Scale, 0.f, 8.f); }

	static UGenGameLogic* Get(const UObject* WorldContext);

private:
	void StepLogic();
	/** Finds overlapping ground units and lets them react (block, step aside, crush); see GenCollision.cpp. */
	void UpdateCollisions();
	void UpdateDrawables();
	void PlaceStartingObjects(const FGenPlayer& Player);
	void InitPlayerSciences(FGenPlayer& Player);
	bool AddScience(int32 Player, const FString& Science);
	void RunPowerOCL(const FString& OCLName, const FGenObject& Source, const FVector3f& CreatePos, const FVector3f& TargetPos);
	void StartDelivery(const struct FGenDeliverPayload& Payload, const FGenObject& Source, const FVector3f& Primary, const FVector3f& Secondary);
	void SpawnMapObjects();
	FVector3f FindPositionAround(const FVector3f& Center, float MinRadius, float MaxRadius, uint32 Surfaces) const;
	void OnContainChanged(FGenObject& Container);
	void UpdateVictory();
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

	TArray<FGenPlayer> Players;
	int32 LocalPlayer = 1;
	TArray<TUniquePtr<FGenSkirmishAI>> AIPlayers;

	TMap<FGenObjectID, TUniquePtr<FGenObject>> Objects;
	TArray<FGenObjectID> ObjectOrder;
	FGenObjectID NextID = 1;

	UPROPERTY(Transient)
	TMap<uint32, TObjectPtr<UGenModelComponent>> Drawables;

	/** A draw module besides the primary one (cargo slung under a helicopter, the boxes of a warehouse): its own model on the same object. */
	struct FExtraDraw
	{
		int32 DrawIndex = INDEX_NONE;     // index into the template's Draws
		TWeakObjectPtr<UGenModelComponent> Model;
		int32 StateIndex = INDEX_NONE;
	};

	/** Client-side state of one drawable: condition state shown, turret pose, muzzle flash timers. */
	struct FDrawState
	{
		TArray<FExtraDraw> Extras;
		uint32 SubObjectVersion = 0;      // upgrade show / hide overrides applied so far
		int32 SupplyBones = -1;           // W3DSupplyDraw: box bones on the model (-1 = not counted yet)
		int32 SupplyShown = -1;           // ... and how many of them currently show
		int32 StateIndex = INDEX_NONE;
		uint32 AppliedVersion = 0;
		bool bPlaced = false;             // the drawable has been given the object's transform at least once
		bool bHidden = false;             // inside a container
		int32 Owner = MIN_int32;          // whose colours it wears (a garrisoned building changes hands)
		int32 StateSystemsFor = INDEX_NONE; // condition state the ParticleSysBone systems below belong to
		TArray<uint32> StateSystems;
		int32 DamageState = 0;            // body damage state the TransitionDamageFX systems below were made for
		TArray<uint32> TransitionSystems;
		float AppliedTurret = 1e9f;
		uint32 SeenFireCount[3] = { 0, 0, 0 };
		double MuzzleHideTime[3] = { 0.0, 0.0, 0.0 };
		// W3DModelDraw WeaponRecoilInfo per weapon slot (0 idle, 1 recoiling, 2 settling)
		uint8 RecoilState[3] = { 0, 0, 0 };
		float RecoilRate[3] = { 0.f, 0.f, 0.f };
		float RecoilShift[3] = { 0.f, 0.f, 0.f };
		float RecoilPrevShift[3] = { 0.f, 0.f, 0.f };
		float RecoilClock[3] = { 0.f, 0.f, 0.f };   // fraction of a logic frame since the last recoil step
	};
	TMap<uint32, FDrawState> DrawStates;
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
	/** Objects whose footprint is currently registered with the pathfinder. */
	TSet<FGenObjectID> ObstacleOwners;
	TArray<TFunction<void()>> PendingCommands;

	// Test automation: -GenTrace logs combat events, -GenPauseFrame=N freezes the simulation at logic frame N.
	bool bTrace = false;
	uint32 PauseAtFrame = 0;

	uint32 Frame = 0;
	double Accumulator = 0.0;
	float TimeScale = 1.f;
	float Alpha = 0.f;
};
