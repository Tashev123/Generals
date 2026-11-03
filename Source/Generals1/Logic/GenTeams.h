#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"
#include "Logic/GenScriptTypes.h"

class UGenGameLogic;
class FGenObject;

/**
 * Team instance (Team.cpp): an instance of one of the team definitions, of one player. A player's default team
 * holds whatever it has that is on no other team, so its members are worked out instead of listed.
 */
struct GENERALS1_API FGenTeam
{
	int32 Id = 0;
	int32 Proto = INDEX_NONE;                   // index into the team definitions
	int32 Player = INDEX_NONE;                  // whose team it is (the controlling player)
	bool bPlayerDefault = false;                // "team<PlayerName>": everything of the player's that is on no other team
	bool bActive = false;                       // false while it is being assembled
	bool bAssembling = false;                   // TeamFactory::createInactiveTeam: a computer player's, until checkReadyTeams says go
	bool bCreated = false;                      // activated this frame: the on-create script is due
	TArray<FGenObjectID> Members;               // (not for a player's default team: its members are worked out)
	TMap<int32, EGenRelationship> OverrideRelations; // Team::setOverrideTeamRelationship, by team id
	TMap<int32, EGenRelationship> OverridePlayerRelations; // Team::setOverridePlayerRelationship, by player
	bool bEnteredOrExited = false;              // TEAM_ENTERED_AREA / a team's enter-and-then-attack scripts
	bool bGenericPending[FGenTeamProto::MaxGenericScripts];
	int8 Recruitable = -1;                      // -1: as the definition says
	int32 CurUnits = 0;
	int32 DestroyThreshold = -1;
	bool bWasIdle = false;
	bool bCheckEnemySighted = false;            // the definition has an enemy-sighted or all-clear script: only then is it looked at
	bool bSeeEnemy = false, bPrevSeeEnemy = false;
	TArray<uint8> InsideFlags;                  // per watched trigger: was the whole team inside last frame

	FGenTeam() { for (bool& B : bGenericPending) { B = true; } }
};

/**
 * The game's one team registry (Common/RTS/Team.cpp's TeamFactory): the team definitions and their instances,
 * which player's they are, and which object is on which team. The map's script engine and a computer player's AI
 * both use this one, so that a team the AI builds is the team the scripts name.
 */
class GENERALS1_API FGenTeamFactory
{
public:
	explicit FGenTeamFactory(UGenGameLogic* InLogic);
	FGenTeamFactory(const FGenTeamFactory&) = delete;
	FGenTeamFactory& operator=(const FGenTeamFactory&) = delete;

	TArray<FGenTeamProto> Protos;
	TArray<int32> ProtoPlayers;                 // the player each definition's teams belong to
	TArray<TUniquePtr<FGenTeam>> Teams;         // pointers stay valid as the list grows
	TMap<int32, int32> DefaultTeams;            // player -> its default team's id
	TMap<FGenObjectID, int32> ObjectTeams;      // objects on a team that isn't their owner's default one
	int32 NextTeamId = 1;

	/** Team definitions of one player (TeamFactory::initFromSides, or a skirmish script library's teams), so that
	 *  a definition is known to belong to its player when the registry holds every side's. */
	void AddProtos(const TArray<FGenTeamProto>& In, int32 Player);
	int32 FindProto(const FString& Name) const;             // TeamFactory::findTeamPrototype
	FGenTeam* FindTeam(int32 TeamId) const;                 // TeamFactory::findTeamByID
	/** TeamFactory::getTeamNamed: the definition's (latest) instance, only when it is active. */
	FGenTeam* FindTeamNamed(const FString& Name) const;
	/** TeamFactory::findTeam: the instance, made if there is none yet. */
	FGenTeam* GetOrCreateTeamNamed(const FString& Name);
	/** TeamFactory::createInactiveTeam (the script side of it, running the definition's production condition, is left to the engine that owns the scripts). */
	FGenTeam& CreateTeam(int32 ProtoIndex, int32 Player);
	/** Player::setDefaultTeam: a player's default team, made if it has none. */
	FGenTeam& EnsureDefaultTeam(int32 Player);
	int32 CountTeamInstances(int32 ProtoIndex) const;
	/** Object::setTeam: on another player's team, the object becomes that player's. */
	void SetObjectTeam(FGenObject& Obj, FGenTeam& Team);
	/** Object::getTeam: the team an object is on (its owner's default team if no other). */
	int32 GetObjectTeam(FGenObjectID Object) const;
	void DeleteTeam(int32 TeamId);
	/** The team's members that are still there (a default team's are worked out from what the player has). */
	void GetTeamMembers(const FGenTeam& Team, TArray<FGenObject*>& Out, bool bIncludeDead = false) const;
	/** The same, one object at a time, for the two engines' own idea of a member worth counting. */
	void ForEachMember(const FGenTeam& Team, TFunctionRef<bool(const FGenObject&)> Wanted, TFunctionRef<void(FGenObject&)> Fn) const;
	bool GetTeamCenter(const FGenTeam& Team, FVector2f& OutCenter) const;

	/**
	 * Team::updateState for every team instance, in the order the teams were made: the on-create script once, the
	 * destroyed threshold, the enemy-sighted / all-clear check, the on-idle script; a team that has nobody left is
	 * gone (singletons, a player's default team and teams still being assembled stay). Called from the script
	 * engine's update (ScriptEngine::update -> PlayerList::updateTeamStates), so the flags the previous frame's
	 * objects set are read first and then cleared. RunTeamScript is the engine that owns the scripts - a map's, or
	 * a computer player's own copy - and is given the team's id, because a script can disband the team it runs on.
	 */
	void UpdateTeamStates(TFunctionRef<void(int32 TeamId, const FString& Name)> RunTeamScript, int32 OnlyPlayer = INDEX_NONE);
	/** Team::notifyTeamOfObjectDeath: one of a team's members died, so its definition's on-unit-destroyed script runs. */
	void NotifyTeamOfObjectDeath(FGenObject& Obj, TFunctionRef<void(int32 TeamId, const FString& Name)> RunTeamScript);

private:
	/** Team::getRelationship: this team's override for the team the object is on, else what the players' relation says. */
	EGenRelationship RelationTo(const FGenTeam& From, const FGenObject& To) const;
	/** Team::updateState's enemy check: does any living member have an enemy within its vision. */
	bool TeamSeesEnemy(const FGenTeam& Team) const;

	UGenGameLogic* Logic = nullptr;
	int32 DeathNotifyDepth = 0;
};
