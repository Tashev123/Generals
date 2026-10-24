#include "Logic/GenCommand.h"
#include "Serialization/Archive.h"

void FGenCommand::Serialize(FArchive& Ar)
{
	uint8 RawType = uint8(Type);
	Ar << RawType;
	int8 RawPlayer = int8(Player);
	Ar << RawPlayer;
	Ar << Frame;
	uint16 NumUnits = uint16(FMath::Min(Units.Num(), int32(MAX_uint16)));
	Ar << NumUnits;
	if (Ar.IsLoading())
	{
		Type = RawType < uint8(EGenCommandType::Count) ? EGenCommandType(RawType) : EGenCommandType::None;
		Player = RawPlayer;
		Units.SetNumUninitialized(NumUnits);
	}
	for (int32 i = 0; i < NumUnits; ++i)
	{
		Ar << Units[i];
	}
	Ar << Target;
	Ar << Pos.X << Pos.Y << Pos.Z;
	Ar << A << B << F;
	uint8 RawFlag = bFlag ? 1 : 0;
	Ar << RawFlag;
	bFlag = RawFlag != 0;
	Ar << Name;
}

const TCHAR* FGenCommand::TypeName(EGenCommandType Type)
{
	switch (Type)
	{
	case EGenCommandType::Move: return TEXT("Move");
	case EGenCommandType::AddWaypoint: return TEXT("AddWaypoint");
	case EGenCommandType::AttackMove: return TEXT("AttackMove");
	case EGenCommandType::Guard: return TEXT("Guard");
	case EGenCommandType::Stop: return TEXT("Stop");
	case EGenCommandType::FireWeaponAt: return TEXT("FireWeaponAt");
	case EGenCommandType::ForceAttack: return TEXT("ForceAttack");
	case EGenCommandType::ForceAttackGround: return TEXT("ForceAttackGround");
	case EGenCommandType::SwitchWeapon: return TEXT("SwitchWeapon");
	case EGenCommandType::CreateFormation: return TEXT("CreateFormation");
	case EGenCommandType::Land: return TEXT("Land");
	case EGenCommandType::Attack: return TEXT("Attack");
	case EGenCommandType::SteerGunship: return TEXT("SteerGunship");
	case EGenCommandType::CombatDrop: return TEXT("CombatDrop");
	case EGenCommandType::Enter: return TEXT("Enter");
	case EGenCommandType::Exit: return TEXT("Exit");
	case EGenCommandType::Evacuate: return TEXT("Evacuate");
	case EGenCommandType::Capture: return TEXT("Capture");
	case EGenCommandType::HackInternet: return TEXT("HackInternet");
	case EGenCommandType::FireWeapon: return TEXT("FireWeapon");
	case EGenCommandType::SpecialPower: return TEXT("SpecialPower");
	case EGenCommandType::Dock: return TEXT("Dock");
	case EGenCommandType::QueueUnit: return TEXT("QueueUnit");
	case EGenCommandType::CancelUnit: return TEXT("CancelUnit");
	case EGenCommandType::Construct: return TEXT("Construct");
	case EGenCommandType::ResumeConstruction: return TEXT("ResumeConstruction");
	case EGenCommandType::Repair: return TEXT("Repair");
	case EGenCommandType::GetRepaired: return TEXT("GetRepaired");
	case EGenCommandType::SetRallyPoint: return TEXT("SetRallyPoint");
	case EGenCommandType::Sell: return TEXT("Sell");
	case EGenCommandType::CancelConstruction: return TEXT("CancelConstruction");
	case EGenCommandType::ToggleOvercharge: return TEXT("ToggleOvercharge");
	case EGenCommandType::BattlePlan: return TEXT("BattlePlan");
	case EGenCommandType::QueueUpgrade: return TEXT("QueueUpgrade");
	case EGenCommandType::PurchaseScience: return TEXT("PurchaseScience");
	case EGenCommandType::Cheat: return TEXT("Cheat");
	case EGenCommandType::Session: return TEXT("Session");
	default: return TEXT("None");
	}
}

FString FGenCommand::Describe() const
{
	FString Out = FString::Printf(TEXT("f%u p%d %s"), Frame, Player, TypeName(Type));
	if (Units.Num() == 1)
	{
		Out += FString::Printf(TEXT(" #%u"), Units[0]);
	}
	else if (Units.Num() > 1)
	{
		Out += FString::Printf(TEXT(" %d units"), Units.Num());
	}
	if (Target != GenInvalidObjectID)
	{
		Out += FString::Printf(TEXT(" -> #%u"), Target);
	}
	if (!Pos.IsZero())
	{
		Out += FString::Printf(TEXT(" at (%.0f, %.0f)"), Pos.X, Pos.Y);
	}
	if (A != 0 || B != 0)
	{
		Out += FString::Printf(TEXT(" [%d %d]"), A, B);
	}
	if (!Name.IsEmpty())
	{
		Out += TEXT(" ") + Name;
	}
	return Out;
}
