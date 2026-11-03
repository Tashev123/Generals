// Firestorms and the other things a blast leaves behind or sets off. After FirestormDynamicGeometryInfoUpdate.cpp,
// DynamicGeometryInfoUpdate.cpp, BunkerBusterBehavior.cpp, NeutonBlastBehavior.cpp and FireWeaponWhenDamagedBehavior.cpp.

#include "Logic/GenObject.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Generals1.h"

void FGenObject::UpdateFirestorm(uint32 Frame)
{
	const FGenFirestorm& D = Template->Firestorm;
	if (!Firestorm)
	{
		Firestorm = MakeUnique<FFirestormState>();
		Firestorm->DelayLeft = uint32(FMath::Max(D.InitialDelay, 1));
		Firestorm->bReverseAtEnd = D.bReverseAtTransitionTime;
		Firestorm->From = D.InitialMajorRadius;
		Firestorm->To = D.FinalMajorRadius;
	}
	FFirestormState& S = *Firestorm;

	// DynamicGeometryInfoUpdate::update
	if (!S.bFinished)
	{
		if (!S.bStarted && --S.DelayLeft > 0)
		{
			return; // ("get out of here if we haven't fired yet")
		}
		S.bStarted = true;
		const float Ratio = float(S.TimeActive) / float(FMath::Max(D.TransitionTime, 1));
		S.Radius = S.From + Ratio * (S.To - S.From);
		++S.TimeActive;
		if (S.TimeActive > uint32(D.TransitionTime))
		{
			if (S.bReverseAtEnd)
			{
				S.bSwitchedDirections = true;
				S.TimeActive = 0;
				S.bReverseAtEnd = false;
				Swap(S.From, S.To);
			}
			else
			{
				S.bFinished = true;
			}
		}
	}

	// FirestormDynamicGeometryInfoUpdate::update
	if (!S.bEffectsFired)
	{
		// "on the ground, but note that we add a small amount to it to avoid particles from the system (especially flat
		// XY particles) from popping up through the terrain"
		const FVector3f Pos(Position.X, Position.Y, D.ParticleOffsetZ + Logic->GetGroundHeight(Position.X, Position.Y));
		for (const FString& Name : D.ParticleSystems)
		{
			S.Systems.Add(Logic->CreateParticleSystemAt(Name, Pos));
		}
		Logic->DoFXObj(D.FXList, *this);
		S.bEffectsFired = true;
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [firestorm f%u] #%u %s lights at (%.0f, %.0f)"), Frame, ID, *Template->Name, Position.X, Position.Y);
	}
	for (const uint32 System : S.Systems)
	{
		Logic->SetParticleSystemRadius(System, S.Radius);
	}
	// "when we first start running backward ... make a scorch mark"
	if (S.bSwitchedDirections && !S.bScorchPlaced)
	{
		Logic->AddScorch(Position, D.ScorchSize, 0);
		S.bScorchPlaced = true;
	}
	// "scan and do some damage every once in a while"
	if (Frame - S.LastDamageFrame >= uint32(D.DelayBetweenDamageFrames) && S.Radius > 0.f)
	{
		S.LastDamageFrame = Frame;
		TArray<FGenObject*, TInlineAllocator<32>> Near;
		Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), S.Radius, [&Near](FGenObject& Other) { Near.Add(&Other); });
		Near.Sort([](const FGenObject& A, const FGenObject& B) { return A.GetID() < B.GetID(); });
		static const FString Flame(TEXT("FLAME")), Burned(TEXT("BURNED"));
		for (FGenObject* O : Near)
		{
			// (FROM_BOUNDINGSPHERE_2D. Friend and foe alike: "objects we have passed over".)
			if (O == this || O->GetPosition().Z > Position.Z + D.MaxHeightForDamage
				|| FVector2f::Distance(FVector2f(Position.X, Position.Y), FVector2f(O->GetPosition().X, O->GetPosition().Y)) - O->GetTemplate()->GetBoundingSphereRadius() > S.Radius)
			{
				continue;
			}
			O->AttemptDamage(D.DamageAmount, Flame, Burned, ID);
		}
	}
}

void FGenObject::OnBlastDeath()
{
	// BunkerBusterBehavior::onDie, NeutronBlastBehavior::onDie: what goes with it when the thing goes off.
	if (bBlastDone)
	{
		return;
	}
	bBlastDone = true;
	if (Template->BunkerBuster.bValid)
	{
		BustTheBunker();
	}
	if (!Template->NeutronShell.bValid)
	{
		return;
	}
	const FGenNeutronShell& D = Template->NeutronShell;
	TArray<FGenObject*, TInlineAllocator<32>> Near;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), D.BlastRadius, [&Near](FGenObject& Other) { Near.Add(&Other); });
	Near.Sort([](const FGenObject& A, const FGenObject& B) { return A.GetID() < B.GetID(); });
	const FVector2f Here(Position.X, Position.Y);
	for (FGenObject* O : Near)
	{
		if (O == this || O->bDead || O->bPendingDestroy || FVector2f::Distance(Here, FVector2f(O->GetPosition().X, O->GetPosition().Y)) > D.BlastRadius)
		{
			continue;
		}
		if (!D.bAffectAirborne && (O->IsKindOf(TEXT("AIRCRAFT")) || O->IsAirborneTarget()))
		{
			continue;
		}
		if (!D.bAffectAllies && Logic->GetRelationship(Owner, O->GetOwner()) == EGenRelationship::Allies)
		{
			continue;
		}
		if (O->IsKindOf(TEXT("INFANTRY")))
		{
			O->Kill();
		}
		// "Kill all contained if it is a container"
		if (O->GetContain())
		{
			const TArray<FGenObjectID> Riders = Logic->GetRiders(*O);
			for (const FGenObjectID Id : Riders)
			{
				if (FGenObject* R = Logic->FindObject(Id))
				{
					R->Kill();
				}
			}
		}
		// "Kill pilots of vehicles"; "If the vehicle is a combat bike, kill the whole thing"
		if (O->IsKindOf(TEXT("VEHICLE")) && !O->IsKindOf(TEXT("DRONE")))
		{
			if (O->IsKindOf(TEXT("CLIFF_JUMPER")))
			{
				O->Kill();
			}
			else
			{
				O->MakeUnmanned(ID);
			}
		}
	}
}

void FGenObject::BustTheBunker()
{
	// BunkerBusterBehavior::bustTheBunker
	const FGenBunkerBuster& D = Template->BunkerBuster;
	if (!D.UpgradeRequired.IsEmpty() && !Logic->HasUpgradeComplete(Owner, D.UpgradeRequired))
	{
		return;
	}
	FGenObject* Target = Logic->FindObject(Projectile.Aimed);
	FGenObject& ForFX = Target ? *Target : *this;
	if (Target && Target->GetContain())
	{
		const EGenContainType Type = Target->GetTemplate()->Contain.Type;
		if (Type == EGenContainType::Garrison || Type == EGenContainType::Tunnel) // (isBustable)
		{
			const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(D.OccupantDamageWeapon);
			const TArray<FGenObjectID> Riders = Logic->GetRiders(*Target);
			for (const FGenObjectID Id : Riders)
			{
				FGenObject* R = Logic->FindObject(Id);
				if (!R)
				{
					continue;
				}
				if (W)
				{
					// (OpenContain::harmAndForceExitAllContained: 100 of the weapon's kind of damage, out in the open.)
					Logic->RemoveFromContain(*Target, *R);
					R->AttemptDamage(100.f, W->DamageType, W->DeathType, ID);
				}
				else
				{
					R->Kill();
				}
			}
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [bunker f%u] #%u %s busts #%u %s: %d riders"), Logic->GetFrame(), ID, *Template->Name, Target->GetID(), *Target->GetTemplate()->Name, Riders.Num());
		}
	}
	Logic->DoFXObj(D.DetonationFX, ForFX);
	if (const FGenWeaponTemplate* Shock = FGenGameData::Get().FindWeapon(D.ShockwaveWeapon))
	{
		Logic->FireTempWeapon(*Shock, ForFX, ForFX.GetPosition());
	}
}

// ---- FireWeaponWhenDamagedBehavior ---------------------------------------------------------------

void FGenObject::FireWhenDamagedWeapon(int32 Index, uint32 Frame)
{
	// Weapon::forceFireWeapon at its own position, if it is READY_TO_FIRE; each of the module's weapons is one of its own.
	const FString& Name = Template->WhenDamaged.Weapons[Index];
	if (Name.IsEmpty())
	{
		return;
	}
	const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(Name);
	if (!W)
	{
		return;
	}
	if (WhenDamagedAmmo.Num() == 0)
	{
		WhenDamagedAmmo.Init(-1, 8);
		WhenDamagedReady.Init(0, 8);
	}
	int32& Ammo = WhenDamagedAmmo[Index];
	uint32& Ready = WhenDamagedReady[Index];
	if (Ammo < 0)
	{
		Ammo = W->ClipSize;
	}
	if (Frame < Ready || (W->ClipSize > 0 && Ammo == 0))
	{
		return;
	}
	Logic->FireTempWeapon(*W, *this, Position);
	uint32 Delay = uint32(FMath::Max(W->DelayBetweenShotsMin, 1));
	if (W->ClipSize > 0 && --Ammo <= 0)
	{
		if (W->ReloadType == EGenReloadType::Auto)
		{
			Ammo = W->ClipSize;
			Delay = uint32(FMath::Max(W->ClipReloadTime, 1));
		}
	}
	Ready = Frame + Delay;
}

void FGenObject::OnDamagedFireWeapon(const FString& DamageType, float ActualDamage)
{
	// FireWeaponWhenDamagedBehavior::onDamage: "right type?" "right amount? (use actual [post-armor] damage dealt)"
	const FGenWhenDamaged& D = Template->WhenDamaged;
	if (ActualDamage < D.DamageAmount)
	{
		return;
	}
	bool bApplies = false;
	for (const FString& T : D.DamageTypes)
	{
		if (T.Equals(TEXT("ALL"), ESearchCase::IgnoreCase))
		{
			bApplies = true;
		}
		else if (T.StartsWith(TEXT("-")))
		{
			bApplies = bApplies && !T.RightChop(1).Equals(DamageType, ESearchCase::IgnoreCase);
		}
		else if (T.Equals(DamageType, ESearchCase::IgnoreCase))
		{
			bApplies = true;
		}
	}
	if (bApplies)
	{
		FireWhenDamagedWeapon(GetDamageState(), Logic->GetFrame());
	}
}

void FGenObject::UpdateWhenDamagedWeapon(uint32 Frame)
{
	// FireWeaponWhenDamagedBehavior::update: "if object fires weapon constantly, figure out which one and do it"
	FireWhenDamagedWeapon(4 + GetDamageState(), Frame);
}
