// The strategy center's battle plans: bombardment, hold the line, search and destroy.
// Follows BattlePlanUpdate.cpp and the Player's side of it in Player.cpp (changeBattlePlan,
// applyBattlePlanBonusesForPlayerObjects, localApplyBattlePlanBonusesToObject, becomingTeamMember).

#include "Logic/GenObject.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenConditions.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Generals1.h"

namespace
{
	enum : uint8 { PlanNone = 0, PlanBombardment = 1, PlanHoldTheLine = 2, PlanSearchAndDestroy = 3 };
	enum : uint8 { StatusIdle = 0, StatusUnpacking = 1, StatusActive = 2, StatusPacking = 3 };

	// (The unpacking and packing sounds share one slot of the object's sounds, the search and destroy hum another.)
	constexpr int32 TransitionSoundSlot = 9; // (0 moving, 1 ambient, 2-4 weapons, 5 a helicopter going down, 6 abilities, 7 the turret, 8 a powerslide)
	constexpr int32 IdleSoundSlot = 10;

	const TCHAR* PlanName(uint8 Plan)
	{
		static const TCHAR* const Names[] = { TEXT("none"), TEXT("bombardment"), TEXT("hold the line"), TEXT("search and destroy") };
		return Names[FMath::Min<uint8>(Plan, 3)];
	}

	/** DOOR_n_OPENING / DOOR_n_WAITING_TO_CLOSE / DOOR_n_CLOSING: door n is the plan's. */
	int32 DoorCondition(uint8 Plan, uint8 Status)
	{
		static const int32 Bits[3][3] = {
			{ GenConditions::Index(TEXT("DOOR_1_OPENING")), GenConditions::Index(TEXT("DOOR_1_WAITING_TO_CLOSE")), GenConditions::Index(TEXT("DOOR_1_CLOSING")) },
			{ GenConditions::Index(TEXT("DOOR_2_OPENING")), GenConditions::Index(TEXT("DOOR_2_WAITING_TO_CLOSE")), GenConditions::Index(TEXT("DOOR_2_CLOSING")) },
			{ GenConditions::Index(TEXT("DOOR_3_OPENING")), GenConditions::Index(TEXT("DOOR_3_WAITING_TO_CLOSE")), GenConditions::Index(TEXT("DOOR_3_CLOSING")) } };
		return (Plan >= PlanBombardment && Plan <= PlanSearchAndDestroy && Status >= StatusUnpacking && Status <= StatusPacking)
			? Bits[Plan - 1][Status - 1] : INDEX_NONE;
	}

	bool IsAnyKindOf(const FGenObject& Obj, const TArray<FString>& Kinds)
	{
		for (const FString& Kind : Kinds)
		{
			if (Obj.IsKindOf(*Kind))
			{
				return true;
			}
		}
		return false;
	}

	const FString BombardmentBonus(TEXT("BATTLEPLAN_BOMBARDMENT"));
	const FString HoldTheLineBonus(TEXT("BATTLEPLAN_HOLDTHELINE"));
	const FString SearchAndDestroyBonus(TEXT("BATTLEPLAN_SEARCHANDDESTROY"));
}

// ---- the strategy center --------------------------------------------------------------------------

void FGenObject::RequestBattlePlan(int32 Plan)
{
	// BattlePlanUpdate::initiateIntentToDoSpecialPower: OPTION_ONE bombardment, OPTION_TWO hold the line,
	// OPTION_THREE search and destroy. What is in force now is packed up first (update).
	if (Plan < PlanBombardment || Plan > PlanSearchAndDestroy || !Template->BattlePlan.bValid)
	{
		return;
	}
	BattlePlanState.Desired = uint8(Plan);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [plan f%u] #%u p%d %s: %s wanted (now %s, status %d)"), Logic->GetFrame(), ID, Owner,
		*Template->Name, PlanName(uint8(Plan)), PlanName(BattlePlanState.Current), BattlePlanState.Status);
}

int32 FGenObject::GetActiveBattlePlan() const
{
	return BattlePlanState.Status == StatusActive ? BattlePlanState.AffectingArmy : PlanNone;
}

float FGenObject::GetVisionRange() const
{
	return Template->VisionRange * VisionScalar;
}

void FGenObject::UpdateBattlePlan(uint32 Frame)
{
	// BattlePlanUpdate::update: idle -> unpacking -> active -> packing -> idle, each step when the last is done.
	FBattlePlanState& S = BattlePlanState;
	if (S.NextReadyFrame > Frame)
	{
		return;
	}
	switch (S.Status)
	{
	case StatusIdle:
		if (S.Desired != PlanNone)
		{
			S.Current = S.Desired;
			SetBattlePlanStatus(StatusUnpacking);
		}
		break;
	case StatusUnpacking:
		SetBattlePlanStatus(StatusActive);
		if (S.Current == PlanBombardment)
		{
			bTurretsDisabled = false; // the gun is out
		}
		break;
	case StatusActive:
		if (S.Current != S.Desired)
		{
			if (S.Current == PlanBombardment)
			{
				// "aiIdle ... recenterTurret": the gun stops, swings back to where it rests, and only then is packed
				// away.
				if (!bTurretsDisabled)
				{
					bTurretsDisabled = true;
					ClearAttack();
				}
				if (RecenterTurrets())
				{
					SetBattlePlanStatus(StatusPacking);
				}
			}
			else
			{
				SetBattlePlanStatus(StatusPacking);
			}
		}
		break;
	case StatusPacking:
		SetBattlePlanStatus(StatusIdle);
		break;
	}
}

void FGenObject::SetBattlePlanStatus(uint8 NewStatus)
{
	// BattlePlanUpdate::setStatus
	FBattlePlanState& S = BattlePlanState;
	if (S.Status == NewStatus)
	{
		return;
	}
	const FGenBattlePlanData& D = Template->BattlePlan;
	const uint32 Now = Logic->GetFrame();
	const bool bLocal = Owner == Logic->GetLocalPlayer();
	const int32 P = FMath::Clamp<int32>(S.Current, PlanBombardment, PlanSearchAndDestroy) - 1;

	// What the old state showed and played stops.
	const int32 OldDoor = DoorCondition(S.Current, S.Status);
	if (OldDoor != INDEX_NONE)
	{
		SetModelCondition(OldDoor, false);
	}
	if (S.Status == StatusUnpacking || S.Status == StatusPacking)
	{
		Logic->SetObjectSound(*this, TransitionSoundSlot, FString());
	}
	else if (S.Status == StatusActive && S.Current == PlanSearchAndDestroy)
	{
		Logic->SetObjectSound(*this, IdleSoundSlot, FString());
	}

	switch (NewStatus)
	{
	case StatusIdle:
		S.Current = PlanNone;
		S.NextReadyFrame = Now + uint32(D.TransitionIdleFrames);
		break;
	case StatusUnpacking:
		// The doors open on the plan's part of the building, its unpacking sound plays, everybody is told.
		// (The message and the radar mark are the owner's here; the original shows them on every machine.)
		SetModelCondition(DoorCondition(S.Current, StatusUnpacking), true);
		SetDrawAnimDuration(D.PlanFrames[P]);
		S.NextReadyFrame = Now + uint32(D.PlanFrames[P]);
		Logic->SetObjectSound(*this, TransitionSoundSlot, D.UnpackSound[P]);
		if (bLocal)
		{
			Logic->CreateRadarEvent(Position, EGenRadarEvent::Information);
			const FString Message = FGenCommandData::Get().GetText(D.MessageLabel[P]);
			if (!Message.IsEmpty())
			{
				Logic->AddScreenMessage(Message);
			}
		}
		Logic->PlaySoundAt(D.Announcement[P], *this);
		break;
	case StatusActive:
		SetBattlePlan(S.Current);
		SetModelCondition(DoorCondition(S.Current, StatusActive), true);
		if (S.Current == PlanSearchAndDestroy)
		{
			Logic->SetObjectSound(*this, IdleSoundSlot, D.SearchAndDestroyIdleSound);
		}
		break;
	case StatusPacking:
		SetBattlePlan(PlanNone);
		SetModelCondition(DoorCondition(S.Current, StatusPacking), true);
		SetDrawAnimDuration(D.PlanFrames[P]);
		S.NextReadyFrame = Now + uint32(D.PlanFrames[P]);
		Logic->SetObjectSound(*this, TransitionSoundSlot, D.PackSound[P]);
		break;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [plan f%u] #%u p%d %s: status %d -> %d (%s)"), Now, ID, Owner, *Template->Name, S.Status, NewStatus,
		PlanName(NewStatus == StatusIdle ? PlanNone : S.Current));
	S.Status = NewStatus;
}

void FGenObject::TakeBackBattlePlan()
{
	// BattlePlanUpdate::setBattlePlan, first half: the plan in force stops working for the army, and what it did
	// to the building itself is undone.
	const FGenBattlePlanData& D = Template->BattlePlan;
	FBattlePlanState& S = BattlePlanState;
	switch (S.AffectingArmy)
	{
	case PlanBombardment:
		Logic->ChangeBattlePlan(Owner, PlanBombardment, -1, S.Bonuses);
		break;
	case PlanHoldTheLine:
		Logic->ChangeBattlePlan(Owner, PlanHoldTheLine, -1, S.Bonuses);
		if (D.CenterMaxHealthScalar != 1.f && D.CenterMaxHealthScalar > 0.f)
		{
			ChangeMaxHealth(MaxHealth / D.CenterMaxHealthScalar, D.CenterMaxHealthChangeType);
		}
		break;
	case PlanSearchAndDestroy:
		Logic->ChangeBattlePlan(Owner, PlanSearchAndDestroy, -1, S.Bonuses);
		if (D.CenterSightScalar != 1.f && D.CenterSightScalar > 0.f)
		{
			VisionScalar /= D.CenterSightScalar;
			ShroudClearingRange /= D.CenterSightScalar;
		}
		if (D.bCenterDetectsStealth && Template->Detector.bValid)
		{
			bDetectorEnabled = false; // StealthDetectorUpdate::setSDEnabled(false)
		}
		break;
	}
	S.AffectingArmy = PlanNone;
	S.Bonuses = FGenBattlePlanBonuses();
	S.Bonuses.Kinds = &D;
}

void FGenObject::SetBattlePlan(uint8 Plan)
{
	// BattlePlanUpdate::setBattlePlan
	const FGenBattlePlanData& D = Template->BattlePlan;
	FBattlePlanState& S = BattlePlanState;
	if (!Logic->GetPlayers().IsValidIndex(Owner))
	{
		return;
	}
	TakeBackBattlePlan();
	switch (Plan)
	{
	case PlanNone:
	{
		// paralyzeTroop: while the plans change, the army stands still.
		const uint32 Until = Logic->GetFrame() + uint32(D.ParalyzeFrames);
		int32 Num = 0;
		for (FGenObject* O : Logic->GetObjectsOf(Owner))
		{
			if (O && !O->bDead && IsAnyKindOf(*O, D.ValidKindOf) && !IsAnyKindOf(*O, D.InvalidKindOf))
			{
				O->DisableUntil(Until);
				++Num;
			}
		}
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [plan f%u] p%d: %d units paralyzed for %d frames"), Logic->GetFrame(), Owner, Num, D.ParalyzeFrames);
		break;
	}
	case PlanBombardment:
		S.Bonuses.Bombardment = 1; // (the weapon bonus)
		Logic->ChangeBattlePlan(Owner, PlanBombardment, 1, S.Bonuses);
		break;
	case PlanHoldTheLine:
		if (D.CenterMaxHealthScalar != 1.f && D.CenterMaxHealthScalar > 0.f)
		{
			ChangeMaxHealth(MaxHealth * D.CenterMaxHealthScalar, D.CenterMaxHealthChangeType);
		}
		S.Bonuses.ArmorScalar = D.HoldTheLineArmorScalar;
		S.Bonuses.HoldTheLine = 1;
		Logic->ChangeBattlePlan(Owner, PlanHoldTheLine, 1, S.Bonuses);
		break;
	case PlanSearchAndDestroy:
		if (D.CenterSightScalar != 1.f && D.CenterSightScalar > 0.f)
		{
			VisionScalar *= D.CenterSightScalar;
			ShroudClearingRange *= D.CenterSightScalar;
		}
		if (D.bCenterDetectsStealth && Template->Detector.bValid)
		{
			bDetectorEnabled = true;
		}
		S.Bonuses.SearchAndDestroy = 1;
		S.Bonuses.SightScalar = D.SearchAndDestroySightScalar;
		Logic->ChangeBattlePlan(Owner, PlanSearchAndDestroy, 1, S.Bonuses);
		break;
	}
	S.AffectingArmy = Plan;
}

void FGenObject::StopBattlePlan()
{
	FBattlePlanState& S = BattlePlanState;
	if (Logic->GetPlayers().IsValidIndex(Owner))
	{
		TakeBackBattlePlan();
	}
	const int32 Door = DoorCondition(S.Current, S.Status);
	if (Door != INDEX_NONE)
	{
		SetModelCondition(Door, false);
	}
	Logic->SetObjectSound(*this, TransitionSoundSlot, FString());
	Logic->SetObjectSound(*this, IdleSoundSlot, FString());
	S.Current = S.Desired = PlanNone;
	S.Status = StatusIdle;
	S.NextReadyFrame = 0;
	bTurretsDisabled = true;
}

void FGenObject::ChangeMaxHealth(float NewMax, const FString& ChangeType)
{
	// ActiveBody::setMaxHealth
	const float OldMax = FMath::Max(MaxHealth, 1.f);
	MaxHealth = FMath::Max(1.f, NewMax);
	if (ChangeType == TEXT("PRESERVE_RATIO")) { Health = Health / OldMax * MaxHealth; }
	else if (ChangeType == TEXT("ADD_CURRENT_HEALTH_TOO")) { Health += MaxHealth - OldMax; }
	else if (ChangeType == TEXT("FULLY_HEAL")) { Health = MaxHealth; }
	Health = FMath::Clamp(Health, bDead ? 0.f : 1.f, MaxHealth);
}

bool FGenObject::RecenterTurrets()
{
	// TurretAI::recenterTurret / isTurretInNaturalPosition
	bool bNatural = true;
	for (int32 t = 0; t < 2; ++t)
	{
		const FGenTurretData& TD = Template->Turrets[t];
		if (!TD.bValid)
		{
			continue;
		}
		const float Diff = GenLogic::AngleDiff(TD.NaturalAngle, TurretAngle[t]);
		TurretAngle[t] = GenLogic::NormalizeAngle(TurretAngle[t] + FMath::Clamp(Diff, -TD.TurnRate, TD.TurnRate));
		const bool bPitched = TurnTurretTowardsPitch(t, TD.NaturalPitch, 1.f);
		bNatural &= bPitched && FMath::Abs(GenLogic::AngleDiff(TD.NaturalAngle, TurretAngle[t])) < 0.001f;
	}
	return bNatural;
}

// ---- the player's side --------------------------------------------------------------------------

void UGenGameLogic::IssueBattlePlan(FGenObjectID Center, int32 Plan)
{
	FGenCommand C(EGenCommandType::BattlePlan);
	C.Units = { Center };
	C.A = Plan;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecBattlePlan(const FGenCommand& Cmd)
{
	const FGenObjectID Center = Cmd.Units.Num() > 0 ? Cmd.Units[0] : GenInvalidObjectID;
	FGenObject* C = FindObject(Center);
	const int32 Module = C ? FindPowerModule(*C, C->GetTemplate()->BattlePlan.Power) : INDEX_NONE;
	if (!C || Module == INDEX_NONE || !IsPowerReady(*C, Module))
	{
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [plan f%u] #%u: no plan change (not ready)"), Frame, Center);
		return;
	}
	C->RequestBattlePlan(Cmd.A);
}

int32 UGenGameLogic::GetBattlePlansActive(int32 Player, int32 Plan) const
{
	return Players.IsValidIndex(Player) && Plan >= PlanBombardment && Plan <= PlanSearchAndDestroy ? Players[Player].BattlePlans[Plan - 1] : 0;
}

void UGenGameLogic::ChangeBattlePlan(int32 Player, int32 Plan, int32 Delta, FGenBattlePlanBonuses& Bonus)
{
	// Player::changeBattlePlan: the first strategy center with a plan gives the army its bonuses, the last one
	// that stops takes them back (inverted).
	if (!Players.IsValidIndex(Player) || Plan < PlanBombardment || Plan > PlanSearchAndDestroy)
	{
		return;
	}
	int32& Count = Players[Player].BattlePlans[Plan - 1];
	Count += Delta;
	if (Count == 1 && Delta == 1)
	{
		ApplyBattlePlanBonusesForPlayerObjects(Player, Bonus);
	}
	else if (Count == 0 && Delta == -1)
	{
		Bonus.ArmorScalar = 1.f / FMath::Max(Bonus.ArmorScalar, 0.01f);
		Bonus.SightScalar = 1.f / FMath::Max(Bonus.SightScalar, 0.01f);
		if (Bonus.Bombardment > 0) { Bonus.Bombardment = -1; }
		if (Bonus.HoldTheLine > 0) { Bonus.HoldTheLine = -1; }
		if (Bonus.SearchAndDestroy > 0) { Bonus.SearchAndDestroy = -1; }
		ApplyBattlePlanBonusesForPlayerObjects(Player, Bonus);
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [plan f%u] p%d: %s %s (%d in force)"), Frame, Player, PlanName(uint8(Plan)), Delta > 0 ? TEXT("starts") : TEXT("stops"), Count);
}

void UGenGameLogic::ApplyBattlePlanBonusesForPlayerObjects(int32 Player, const FGenBattlePlanBonuses& Bonus)
{
	// Player::applyBattlePlanBonusesForPlayerObjects: kept, all together, for what the player gets later.
	FGenPlayer& P = Players[Player];
	if (!P.bHasBattlePlanBonuses)
	{
		P.BattlePlanBonuses = Bonus;
		P.bHasBattlePlanBonuses = true;
	}
	else
	{
		FGenBattlePlanBonuses& All = P.BattlePlanBonuses;
		All.ArmorScalar *= Bonus.ArmorScalar;
		All.SightScalar *= Bonus.SightScalar;
		All.Bombardment = FMath::Max(0, All.Bombardment + Bonus.Bombardment);
		All.HoldTheLine = FMath::Max(0, All.HoldTheLine + Bonus.HoldTheLine);
		All.SearchAndDestroy = FMath::Max(0, All.SearchAndDestroy + Bonus.SearchAndDestroy);
		if (!All.Kinds) { All.Kinds = Bonus.Kinds; }
	}
	for (FGenObject* O : GetObjectsOf(Player))
	{
		if (O)
		{
			ApplyBattlePlanBonuses(*O, Bonus);
		}
	}
}

void UGenGameLogic::ApplyBattlePlanBonuses(FGenObject& Obj, const FGenBattlePlanBonuses& Bonus)
{
	// localApplyBattlePlanBonusesToObject. (A projectile is judged there by whoever fired it, and gets the weapon
	// bonus; here a shot's damage bonus is its firer's anyway.)
	if (!Bonus.Kinds || Obj.IsKindOf(TEXT("PROJECTILE")) || !IsAnyKindOf(Obj, Bonus.Kinds->ValidKindOf) || IsAnyKindOf(Obj, Bonus.Kinds->InvalidKindOf))
	{
		return;
	}
	if (Bonus.ArmorScalar != 1.f)
	{
		Obj.DamageScalar *= Bonus.ArmorScalar; // ActiveBody::applyDamageScalar
	}
	if (Bonus.SightScalar != 1.f)
	{
		Obj.VisionScalar *= Bonus.SightScalar;
		Obj.ShroudClearingRange *= Bonus.SightScalar;
	}
	auto SetBonus = [&Obj](const FString& Condition, bool bOn)
	{
		if (bOn) { Obj.BonusConditions.AddUnique(Condition); } else { Obj.BonusConditions.Remove(Condition); }
	};
	SetBonus(BombardmentBonus, Bonus.Bombardment > 0);
	SetBonus(HoldTheLineBonus, Bonus.HoldTheLine > 0);
	SetBonus(SearchAndDestroyBonus, Bonus.SearchAndDestroy > 0);
}

void UGenGameLogic::AddBattlePlanBonuses(FGenObject& Obj, int32 Player)
{
	// Player::applyBattlePlanBonusesForObject: a new or converted object gains the plans in force.
	if (Players.IsValidIndex(Player) && Players[Player].NumBattlePlansActive() > 0 && Players[Player].bHasBattlePlanBonuses)
	{
		ApplyBattlePlanBonuses(Obj, Players[Player].BattlePlanBonuses);
	}
}

void UGenGameLogic::RemoveBattlePlanBonuses(FGenObject& Obj, int32 Player)
{
	// Player::removeBattlePlanBonusesForObject: "Copy bonuses, and invert them."
	if (!Players.IsValidIndex(Player) || Players[Player].NumBattlePlansActive() == 0 || !Players[Player].bHasBattlePlanBonuses)
	{
		return;
	}
	FGenBattlePlanBonuses Bonus = Players[Player].BattlePlanBonuses;
	Bonus.ArmorScalar = 1.f / FMath::Max(Bonus.ArmorScalar, 0.01f);
	Bonus.SightScalar = 1.f / FMath::Max(Bonus.SightScalar, 0.01f);
	Bonus.Bombardment = Bonus.HoldTheLine = Bonus.SearchAndDestroy = -3; // (-ALL_PLANS: clears the weapon bonus conditions)
	ApplyBattlePlanBonuses(Obj, Bonus);
}

void UGenGameLogic::SetObjectSound(const FGenObject& Obj, int32 Slot, const FString& Event)
{
	if (Audio)
	{
		Audio->SetObjectLoop(Obj.GetID(), Slot, Event);
	}
}
