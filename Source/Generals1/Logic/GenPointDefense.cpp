// PointDefenseLaserUpdate: a laser that shoots down the missiles coming for its owner - a Paladin's, and the Air
// Force General's jets'. It looks for the nearest missile of the kinds it is after (primary ones first) within
// its scan range, and fires its weapon - a beam, LaserName - at it whenever it is ready and the missile has come
// within the weapon's reach.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Generals1.h"

namespace
{
	bool IsAnyKind(const FGenObject& Obj, const TArray<FString>& Kinds)
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

	float Dist2D(const FGenObject& A, const FVector3f& B)
	{
		return FVector2f::Distance(FVector2f(A.GetPosition().X, A.GetPosition().Y), FVector2f(B.X, B.Y));
	}
}

void FGenObject::UpdatePointDefense()
{
	// PointDefenseLaserUpdate::update ("No more laser fo you" once dead).
	if (bDead)
	{
		return;
	}
	PointDefense.SetNum(Template->PointDefenseLasers.Num());
	for (int32 i = 0; i < PointDefense.Num(); ++i)
	{
		FPointDefenseState& S = PointDefense[i];
		if (S.NextScanFrames > 0)
		{
			--S.NextScanFrames;
			FirePointDefenseWhenReady(i); // "Only happens if something is tracked."
			continue;
		}
		S.NextScanFrames = Template->PointDefenseLasers[i].ScanFrames;
		if (ScanPointDefense(i))
		{
			FirePointDefenseWhenReady(i);
		}
	}
}

const FGenObject* FGenObject::ScanPointDefense(int32 Index)
{
	// PointDefenseLaserUpdate::scanClosestTarget: the nearest one in reach, or else the nearest coming - of the
	// primary kinds before the secondary ones.
	const FGenPointDefenseLaser& D = Template->PointDefenseLasers[Index];
	FPointDefenseState& S = PointDefense[Index];
	const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(D.Weapon);
	S.Best = GenInvalidObjectID;
	S.bInRange = false;
	if (!W)
	{
		return nullptr;
	}
	const float FireRange = W->AttackRange;
	const FGenObject* InRange[2] = { nullptr, nullptr };
	const FGenObject* OutOfRange[2] = { nullptr, nullptr };
	float ClosestIn[2] = { FLT_MAX, FLT_MAX };
	float ClosestOut[2] = { FLT_MAX, FLT_MAX };
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), D.ScanRange, [&](FGenObject& Other)
	{
		const int32 Kind = IsAnyKind(Other, D.PrimaryKinds) ? 0 : (IsAnyKind(Other, D.SecondaryKinds) ? 1 : -1);
		if (Kind < 0 || Other.IsDead() || Other.IsDestroyed())
		{
			return;
		}
		if (!Other.IsAirborneTarget() && !W->bAntiGround)
		{
			return;
		}
		if (Logic->GetRelationship(Owner, Other.GetOwner()) != EGenRelationship::Enemies || Other.IsStealthedAndUndetected())
		{
			return;
		}
		float Dist = Dist2D(*this, Other.GetPosition());
		if (Dist > D.ScanRange)
		{
			return;
		}
		if (Dist <= FireRange)
		{
			if (Dist < ClosestIn[Kind])
			{
				ClosestIn[Kind] = Dist;
				InRange[Kind] = &Other;
			}
		}
		else if (!InRange[Kind])
		{
			// (Where it will be a moment from now - "PredictTargetVelocityFactor".)
			if (D.VelocityFactor != 0.f && !Other.IsKindOf(TEXT("IMMOBILE")))
			{
				Dist = Dist2D(*this, Other.GetPosition() + Other.GetVelocity() * D.VelocityFactor);
			}
			if (Dist < ClosestOut[Kind])
			{
				ClosestOut[Kind] = Dist;
				OutOfRange[Kind] = &Other;
			}
		}
	});
	for (int32 k = 0; k < 2; ++k)
	{
		if (InRange[k])
		{
			S.Best = InRange[k]->GetID();
			S.bInRange = true;
			return InRange[k];
		}
	}
	for (int32 k = 0; k < 2; ++k)
	{
		if (OutOfRange[k])
		{
			S.Best = OutOfRange[k]->GetID();
			return OutOfRange[k];
		}
	}
	return nullptr;
}

void FGenObject::FirePointDefenseWhenReady(int32 Index)
{
	// PointDefenseLaserUpdate::fireWhenReady
	const FGenPointDefenseLaser& D = Template->PointDefenseLasers[Index];
	FPointDefenseState& S = PointDefense[Index];
	const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(D.Weapon);
	FGenObject* Target = S.Best != GenInvalidObjectID ? Logic->FindObject(S.Best) : nullptr;
	if (Target && W)
	{
		if (Dist2D(*this, Target->GetPosition()) < W->AttackRange)
		{
			S.bInRange = true;
		}
		else if (S.bInRange)
		{
			// It got away: look again soon.
			S.NextScanFrames = Logic->GetRandom().RandRange(0, 3);
			S.Best = GenInvalidObjectID;
			if (S.NextScanFrames == 0)
			{
				ScanPointDefense(Index);
				S.NextScanFrames = D.ScanFrames;
				Target = nullptr; // "so we don't shoot at it (might be out of range)"
			}
		}
	}
	if (S.NextShotFrames > 0)
	{
		--S.NextShotFrames;
		return;
	}
	if (!W || !Target || !S.bInRange)
	{
		return;
	}
	if (!Target->IsDead())
	{
		// "allocateNewWeapon ... fireWeapon( getObject(), target )": the beam, its flash and sound, the damage.
		Logic->DoWeaponFireFX(*this, 2, *W, Target->GetPosition());
		Logic->DoWeaponFireSound(*this, 2, *W);
		Logic->CreateWeaponLaser(*this, 2, *W, Target, Target->GetPosition());
		// (Weapon::fireWeapon on a missile in flight: the one it is aimed at, nothing around it.)
		Target->AttemptDamage(W->PrimaryDamage * GetWeaponBonus(*W, int32(FGenWeaponBonus::Damage)), W->DamageType, W->DeathType, ID, true);
		S.NextShotFrames = FMath::Max(0, W->DelayBetweenShotsMin);
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [laser f%u] #%u %s shoots at #%u %s%s"), Logic->GetFrame(), ID, *Template->Name, Target->GetID(),
			*Target->GetTemplate()->Name, Target->IsDead() ? TEXT(": down") : TEXT(""));
	}
	if (Target->IsDead())
	{
		S.NextScanFrames = Logic->GetRandom().RandRange(0, 3);
		S.Best = GenInvalidObjectID;
		if (S.NextScanFrames == 0)
		{
			ScanPointDefense(Index);
			S.NextScanFrames = D.ScanFrames;
		}
	}
}
