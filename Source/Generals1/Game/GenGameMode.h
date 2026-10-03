#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GenGameMode.generated.h"

class AGenMapActor;

/**
 * Skirmish game mode. Loads the map given by the URL option "?GenMap=Name" or the
 * command line "-GenMap=Name" (default: Tournament Desert) and starts a skirmish:
 * the local player (-GenFaction=, default FactionAmerica) against -GenEnemy= (default FactionChina).
 *
 * Until production exists, both sides start with a test battle group (-GenNoTestUnits turns it off;
 * runs that place their own units or take a screenshot skip it unless -GenTestUnits is given).
 *
 * Test automation (command line):
 *   -GenShot=<file.png>        take a screenshot after the map loads, then quit
 *   -GenShotDelay=<sec>        wait before the screenshot (default 4)
 *   -GenCam=X,Y,Yaw,Height     camera look-at in Generals units, yaw degrees, height
 *   -GenObject=T1,T2           spawn units for the local player at the camera target (or -GenObjectAt=X,Y)
 *   -GenTestMove=X,Y           after 1 s, order all local mobile units to move there
 *   -GenEnemyObject=T1,T2      spawn units for the enemy player (at -GenEnemyAt=X,Y)
 *   -GenTestAttack             after 1 s, order all local armed units to attack the first enemy unit
 *   -GenTestFX=Name[,Name]     after 1 s, play FX lists / particle systems at the camera target
 *   -GenTrace                  log unit state once per second
 *   -GenSeed=N                 fixed simulation random seed
 *   -GenPauseFrame=N           freeze the simulation at logic frame N (and take the -GenShot there)
 *   -GenModel=<name>           spawn a bare W3D model (preview)
 */
UCLASS()
class GENERALS1_API AGenGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGenGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	AGenMapActor* GetMapActor() const { return MapActor; }

private:
	void RunTestAutomation(float DeltaSeconds);
	/** Places a battle group for the local player in front of its base and one for the enemy beyond it. */
	void SpawnTestUnits(class UGenGameLogic* Logic);

	UPROPERTY(Transient)
	TObjectPtr<AGenMapActor> MapActor;

	FString ShotPath;
	float ShotDelay = 4.f;
	float Elapsed = 0.f;
	int32 ShotState = 0;
	bool bTestMovePending = false;
	FVector2f TestMoveGoal = FVector2f::ZeroVector;
	uint32 ShotFrame = 0;        // -GenPauseFrame: screenshot at this logic frame
	int32 FramesAtShotFrame = 0;
	bool bTestFXDone = false;
	bool bTestAttackPending = false;
	uint32 TestAttackTarget = 0; // first -GenEnemyObject unit
};
