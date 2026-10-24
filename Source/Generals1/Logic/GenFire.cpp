// What being hit looks like, and catching fire.
// Follows ActiveBody::doDamageFX with DamageFX.cpp (the sparks, puffs and thuds of a hit, by armor and
// damage type), FlammableUpdate.cpp and FireSpreadUpdate.cpp. The fire and smoke that sit on a damaged or
// burning model's bones (ActiveBody::updateBodyParticleSystems) are made with the drawable, in
// UGenGameLogic::UpdateDrawables.

#include "Logic/GenObject.h"
#include "Audio/GenAudioManager.h"
#include "Core/GenPerf.h"
#include "HAL/IConsoleManager.h"
#include "Logic/GenConditions.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Generals1.h"

static TAutoConsoleVariable<int32> CVarGenDamageFX(
	TEXT("gen.DamageFX"), 1,
	TEXT("1: bodies show the effect of being hit (DamageFX.ini). 0: they don't - to measure what the effects cost."));

namespace
{
	constexpr int32 BurningSoundSlot = 7; // (0 moving, 1 ambient, 2-4 weapons, 5 a helicopter going down, 6 a power's)

	int32 AflameBit() { static const int32 I = GenConditions::Index(TEXT("AFLAME")); return I; }
	int32 SmolderingBit() { static const int32 I = GenConditions::Index(TEXT("SMOLDERING")); return I; }
}

// ---- hits ---------------------------------------------------------------------------------------

void FGenObject::DoDamageFX(const FString& DamageType, float Amount, FGenObjectID Source)
{
	// ActiveBody::doDamageFX: one effect per kind of damage and throttle time, however many are shooting.
	const uint32 Now = Logic->GetFrame();
	if ((Now < NextDamageFXFrame && LastDamageFXType == DamageType) || CVarGenDamageFX.GetValueOnAnyThread() == 0)
	{
		return;
	}
	GEN_PERF_SCOPE("Obj.DamageFX");
	const FGenObject* Attacker = Logic->FindObject(Source);
	const FGenDamageFX::FEntry& Entry = DamageFX->Find(DamageType, Attacker ? Attacker->VeterancyLevel : 0);
	LastDamageFXType = DamageType;
	NextDamageFXFrame = Now + uint32(Entry.ThrottleFrames);
	// DamageFX::getDamageFXList ("if damage is zero, never do damage fx": the caller has seen to that)
	const FString& FXName = Amount >= Entry.AmountForMajor ? Entry.MajorFX : Entry.MinorFX;
	if (!FXName.IsEmpty())
	{
		// "since the victim is receiving the damage, it's the "primary" object."
		Logic->DoFXObj(FXName, *this, Attacker);
	}
}

// ---- fire ---------------------------------------------------------------------------------------

void FGenObject::OnFlameDamage(float ActualDamage, uint32 Frame)
{
	// FlammableUpdate::onDamage
	const FGenFlammable& F = Template->Flammable;
	if (Frame > LastFlameDamageFrame + uint32(F.FlameDamageExpiration))
	{
		// "If it has been a long time since our last flame damage, reset the threshold"
		FlameDamageLimit = F.FlameDamageLimit;
	}
	LastFlameDamageFrame = Frame;
	if (FlameStatus != EFlameStatus::Aflame && !bBurned)
	{
		// "If I'm not on fire, and I haven't burned up, see if I should try to catch fire."
		FlameDamageLimit -= ActualDamage;
		if (FlameDamageLimit <= 0.f)
		{
			TryToIgnite();
		}
	}
}

void FGenObject::TryToIgnite()
{
	// FlammableUpdate::tryToIgnite
	const FGenFlammable& F = Template->Flammable;
	if (!F.bValid || FlameStatus != EFlameStatus::Normal || bDead || bPendingDestroy)
	{
		return;
	}
	const uint32 Now = Logic->GetFrame();
	FlameStatus = EFlameStatus::Aflame;
	SetModelCondition(AflameBit(), true);
	if (UGenAudioManager* Audio = Logic->GetAudio())
	{
		Audio->SetObjectLoop(ID, BurningSoundSlot, F.BurningSound);
	}
	AflameEndFrame = Now + uint32(FMath::Max(1, F.AflameDuration));
	BurnedEndFrame = F.BurnedDelay > 0 ? Now + uint32(F.BurnedDelay) : 0;
	FlameDamageFrame = F.AflameDamageDelay > 0 ? Now + uint32(F.AflameDamageDelay) : 0;
	if (Template->FireSpread.bValid)
	{
		// FireSpreadUpdate::startFireSpreading
		const FGenFireSpread& S = Template->FireSpread;
		NextFireSpreadFrame = Now + uint32(FMath::Max(1, Logic->GetRandom().RandRange(S.MinDelay, FMath::Max(S.MinDelay, S.MaxDelay))));
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [fire f%u] #%u %s catches fire (burns for %d frames)"), Now, ID, *Template->Name, F.AflameDuration);
}

void FGenObject::UpdateFlammable(uint32 Frame)
{
	// FlammableUpdate::update, while aflame.
	const FGenFlammable& F = Template->Flammable;
	if (FlameDamageFrame != 0 && Frame >= FlameDamageFrame)
	{
		// doAflameDamage
		FlameDamageFrame = Frame + uint32(F.AflameDamageDelay);
		AttemptDamage(F.AflameDamageAmount, TEXT("FLAME"), TEXT("BURNED"), ID);
		if (bDead)
		{
			if (UGenAudioManager* Audio = Logic->GetAudio())
			{
				Audio->SetObjectLoop(ID, BurningSoundSlot, FString());
			}
			return;
		}
	}
	if (BurnedEndFrame != 0 && Frame >= BurnedEndFrame && !bBurned)
	{
		// "So this status is set, but I am still aflame on an independent timer."
		bBurned = true;
		SetModelCondition(SmolderingBit(), true);
	}
	if (Template->FireSpread.bValid && NextFireSpreadFrame != 0 && Frame >= NextFireSpreadFrame)
	{
		// FireSpreadUpdate::update: embers, and "the closest flammable thing will be set on fire automatically".
		const FGenFireSpread& S = Template->FireSpread;
		NextFireSpreadFrame = Frame + uint32(FMath::Max(1, Logic->GetRandom().RandRange(S.MinDelay, FMath::Max(S.MinDelay, S.MaxDelay))));
		if (!S.OCLEmbers.IsEmpty())
		{
			Logic->DoOCL(S.OCLEmbers, *this);
		}
		if (S.Range > 0.f)
		{
			FGenObject* Nearest = nullptr;
			float BestDistSq = FMath::Square(S.Range);
			Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), S.Range, [&](FGenObject& Other)
			{
				// PartitionFilterFlammable: "It must be burnable in general, and burnable now"
				if (&Other == this || !Other.Template->Flammable.bValid || Other.FlameStatus != EFlameStatus::Normal || Other.bDead || Other.bPendingDestroy)
				{
					return;
				}
				const float DistSq = FVector3f::DistSquared(Position, Other.Position);
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					Nearest = &Other;
				}
			});
			if (Nearest)
			{
				Nearest->TryToIgnite();
			}
		}
	}
	if (Frame >= AflameEndFrame)
	{
		// "This is the important one. I am no longer on fire." - and what has burned never burns again.
		FlameStatus = bBurned ? EFlameStatus::Burned : EFlameStatus::Normal;
		SetModelCondition(AflameBit(), false);
		NextFireSpreadFrame = 0;
		if (UGenAudioManager* Audio = Logic->GetAudio())
		{
			Audio->SetObjectLoop(ID, BurningSoundSlot, FString());
		}
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [fire f%u] #%u %s has stopped burning%s"), Frame, ID, *Template->Name, bBurned ? TEXT(" (burned)") : TEXT(""));
	}
}
