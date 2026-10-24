#pragma once

#include "CoreMinimal.h"
#include "Logic/GenLogicTypes.h"

/** What a player's order is (the original's GameMessage types, those the port has). */
enum class EGenCommandType : uint8
{
	None,
	Move,
	AddWaypoint,
	AttackMove,
	Guard,
	Stop,
	FireWeaponAt,
	ForceAttack,
	ForceAttackGround,
	SwitchWeapon,
	CreateFormation,
	Land,
	Attack,
	SteerGunship,
	CombatDrop,
	Enter,
	Exit,
	Evacuate,
	Capture,
	HackInternet,
	FireWeapon,
	SpecialPower,
	Dock,
	QueueUnit,
	CancelUnit,
	Construct,
	ResumeConstruction,
	Repair,
	GetRepaired,
	SetRallyPoint,
	Sell,
	CancelConstruction,
	ToggleOvercharge,
	BattlePlan,
	QueueUpgrade,
	PurchaseScience,
	/** A test switch that changes the game by hand (gen_test.ps1's -TestPowers "money", "kill", ...): an order
	 *  too, so that a replay of a test has it at the same frame. Name: the step ("kill,Type,2"). */
	Cheat,
	/** From the game's host, not a player: Name says what (see UGenGameLogic::ExecSession). */
	Session,
	Count
};

/**
 * A player's order as the simulation gets it (the original's GameMessage): who gave it, what it is and what it
 * is about. Orders are the only way into the simulation from outside: from this computer's interface, from a
 * replay, from the other players over the network. Every computer applies the same orders at the start of the
 * same logic frame, and so works out the same game.
 *
 * Units holds whoever is told - the building, factory, dozer or rider for the orders about one thing - and only
 * what the giving player may command is told (UGenGameLogic::CanPlayerCommand). Target is the object acted on.
 */
struct GENERALS1_API FGenCommand
{
	EGenCommandType Type = EGenCommandType::None;
	int32 Player = INDEX_NONE;                  // who gave it (INDEX_NONE: this computer's player)
	uint32 Frame = 0;                           // the logic frame it is applied at the start of (set when it is)
	TArray<FGenObjectID> Units;
	FGenObjectID Target = GenInvalidObjectID;
	FVector3f Pos = FVector3f::ZeroVector;
	int32 A = 0;                                // weapon slot, guard mode, battle plan, queue index
	int32 B = 0;                                // shots to fire
	float F = 0.f;                              // an angle
	bool bFlag = false;                         // mine clearing
	FString Name;                               // a template, an upgrade, a science, a power, a cheat

	FGenCommand() = default;
	explicit FGenCommand(EGenCommandType InType) : Type(InType) {}

	/** Writes or reads it, bit for bit (the floats too: every computer must get the same numbers). */
	void Serialize(FArchive& Ar);
	/** For logs: "f120 p1 Move 3 units -> (1005, 900)". */
	FString Describe() const;
	static const TCHAR* TypeName(EGenCommandType Type);
};
