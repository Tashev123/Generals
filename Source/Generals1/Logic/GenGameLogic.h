#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenObject.h"
#include "Logic/GenPathfinder.h"
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
	void IssueAttack(const TArray<FGenObjectID>& Units, FGenObjectID Target);

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
	/** World transform (Generals space) of a weapon slot's fire-FX or launch bone on the object's current model. */
	bool GetWeaponBoneTransform(const FGenObject& Obj, int32 Slot, bool bLaunchBone, FTransform3f& OutWorld) const;

	// --- queries ---
	FGenPathfinder& GetPathfinder() { return Pathfinder; }
	const FGenMapData* GetMap() const;
	AGenMapActor* GetMapActor() const { return MapActor; }
	float GetGroundHeight(float X, float Y) const;
	uint32 GetFrame() const { return Frame; }

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
	void SpawnMapObjects();
	FVector3f FindPositionAround(const FVector3f& Center, float MinRadius, float MaxRadius, uint32 Surfaces) const;
	/** W3DDebrisDraw::setModelName: gives an object created without a model (GenericDebris) its visual. */
	void SetDebrisModel(FGenObject& Obj, const FString& Model, bool bUseHouseColor);

	UPROPERTY(Transient)
	TObjectPtr<AGenMapActor> MapActor;

	UPROPERTY(Transient)
	TObjectPtr<UGenFXManager> FX;

	TArray<FGenPlayer> Players;
	int32 LocalPlayer = 1;

	TMap<FGenObjectID, TUniquePtr<FGenObject>> Objects;
	TArray<FGenObjectID> ObjectOrder;
	FGenObjectID NextID = 1;

	UPROPERTY(Transient)
	TMap<uint32, TObjectPtr<UGenModelComponent>> Drawables;

	/** Client-side state of one drawable: condition state shown, turret pose, muzzle flash timers. */
	struct FDrawState
	{
		int32 StateIndex = INDEX_NONE;
		uint32 AppliedVersion = 0;
		bool bPlaced = false;             // the drawable has been given the object's transform at least once
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
