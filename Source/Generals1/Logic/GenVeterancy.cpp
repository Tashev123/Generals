// Veterancy (experience levels of units) and winning or losing the game.
// Follows ExperienceTracker.cpp, Object::onVeterancyLevelChanged, ActiveBody::onVeterancyLevelChanged,
// VeterancyGainCreate.cpp and VictoryConditions.cpp (with Player::killPlayer).

#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenSkirmishAI.h"
#include "Logic/GenUpgrades.h"
#include "Generals1.h"

namespace
{
	// Weapon set flags / weapon bonus conditions by level, and the upgrade each level grants.
	const TCHAR* VeterancyFlags[4] = { TEXT(""), TEXT("VETERAN"), TEXT("ELITE"), TEXT("HERO") };
	const TCHAR* VeterancyUpgrades[4] = { TEXT(""), TEXT("Upgrade_Veterancy_VETERAN"), TEXT("Upgrade_Veterancy_ELITE"), TEXT("Upgrade_Veterancy_HEROIC") };
	const TCHAR* VeterancyNames[4] = { TEXT("Regular"), TEXT("Veteran"), TEXT("Elite"), TEXT("Heroic") };
}

// ---- veterancy ----------------------------------------------------------------------------------

void FGenObject::InitVeterancy()
{
	// VeterancyGainCreate: some units are made at a higher level once their player has the science
	// for it (Red Guard training, artillery training, ...).
	if (Template->VeterancyStartLevel > 0
		&& (Template->VeterancyStartScience.IsEmpty() || Logic->HasScience(Owner, Template->VeterancyStartScience)))
	{
		// (Born at that level, it isn't "promoted": no sound, no notice - the user asked for none.)
		SetVeterancyLevel(Template->VeterancyStartLevel, false);
	}
}

void FGenObject::SetVeterancyLevel(int32 NewLevel, bool bProvideFeedback)
{
	// ExperienceTracker::setVeterancyLevel: at that level, with the least experience it takes.
	NewLevel = FMath::Clamp(NewLevel, 0, 3);
	if (NewLevel != VeterancyLevel)
	{
		const int32 Old = VeterancyLevel;
		VeterancyLevel = NewLevel;
		Experience = Template->ExperienceRequired[NewLevel];
		OnVeterancyLevelChanged(Old, NewLevel, bProvideFeedback);
	}
}

void FGenObject::AddExperience(int32 Points, bool bCanScaleForBonus)
{
	// ExperienceTracker::addExperiencePoints
	if (!Template->bIsTrainable || Points <= 0 || bDead)
	{
		return;
	}
	// ("if ( canScaleForBonus ) amountToGain *= m_experienceScalar" - Advanced Training doubles it.)
	Experience += bCanScaleForBonus ? FMath::TruncToInt32(float(Points) * XPScalar) : Points;
	int32 Level = 0;
	while (Level + 1 < 4 && Experience >= Template->ExperienceRequired[Level + 1])
	{
		++Level;
	}
	if (Level != VeterancyLevel)
	{
		const int32 Old = VeterancyLevel;
		VeterancyLevel = Level;
		OnVeterancyLevelChanged(Old, Level);
	}
}

void FGenObject::OnVeterancyLevelChanged(int32 OldLevel, int32 NewLevel, bool bProvideFeedback)
{
	// Object::onVeterancyLevelChanged: the level's weapon set and weapon bonuses replace the old level's.
	for (int32 i = 1; i < 4; ++i)
	{
		SetFlags.Remove(VeterancyFlags[i]);
		BonusConditions.Remove(VeterancyFlags[i]);
	}
	if (NewLevel > 0)
	{
		SetFlags.AddUnique(VeterancyFlags[NewLevel]);
		BonusConditions.AddUnique(VeterancyFlags[NewLevel]);
	}
	RefreshWeaponSet();

	// ActiveBody::onVeterancyLevelChanged: more health at each level, the damage taken so far kept in proportion.
	const FGenGameData& GD = FGenGameData::Get();
	const float Mult = GD.HealthBonus[NewLevel] / FMath::Max(GD.HealthBonus[OldLevel], 0.01f);
	const float Ratio = MaxHealth > 0.f ? Health / MaxHealth : 1.f;
	MaxHealth *= Mult;
	Health = bDead ? 0.f : FMath::Max(Ratio * MaxHealth, 1.f);

	// The level's upgrade (Upgrade_Veterancy_ELITE switches on the self-repair of many units).
	if (NewLevel > 0)
	{
		if (const FGenUpgradeTemplate* Upgrade = FGenUpgradeData::Get().FindUpgrade(VeterancyUpgrades[NewLevel]))
		{
			GiveObjectUpgrade(*Upgrade);
		}
	}
	UpdateUpgradeModules();

	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [vet f%u] #%u %s is now %s (experience %d, health %.0f / %.0f)"), Logic->GetFrame(), ID, *Template->Name,
		VeterancyNames[NewLevel], Experience, Health, MaxHealth);
	if (NewLevel > OldLevel && Logic->GetFrame() > 1 && bProvideFeedback)
	{
		Logic->AnnounceVeterancy(*this, VeterancyNames[NewLevel]);
	}
}

void UGenGameLogic::AnnounceVeterancy(const FGenObject& Obj, const TCHAR* LevelName)
{
	// (In place of the original's level-gain animation over the unit.)
	if (Obj.GetOwner() != LocalPlayer || Obj.IsKindOf(TEXT("IGNORED_IN_GUI")))
	{
		return;
	}
	// Object::onVeterancyLevelChanged: the unit's own promotion sound, and the notification.
	static const TCHAR* PromotedKeys[4] = { TEXT(""), TEXT("SoundPromotedVeteran"), TEXT("SoundPromotedElite"), TEXT("SoundPromotedHero") };
	PlayObjectSound(Obj, { PromotedKeys[FMath::Clamp(Obj.GetVeterancyLevel(), 1, 3)] });
	PlayMiscSound(TEXT("UnitPromoted"), Obj.GetOwner());
	FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
	Text.Text = LevelName;
	Text.Pos = Obj.GetPosition();
	Text.Color = FLinearColor(1.f, 0.85f, 0.2f);
	Text.Frame = Frame;
}

// ---- victory ------------------------------------------------------------------------------------

bool UGenGameLogic::HasPlayerBeenDefeated(int32 Player) const
{
	// VictoryConditions::hasSinglePlayerBeenDefeated with VICTORY_NOBUILDINGS (what a skirmish is played
	// to): no building left that counts (KindOf MP_COUNT_FOR_VICTORY).
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (O->GetOwner() == Player && !O->IsDead() && !O->IsDestroyed() && O->IsKindOf(TEXT("STRUCTURE")) && O->IsKindOf(TEXT("MP_COUNT_FOR_VICTORY")))
		{
			return false;
		}
	}
	return true;
}

void UGenGameLogic::UpdateVictory()
{
	GEN_PERF_SCOPE("Victory");
	if (Frame < 2 || Frame % 15 != 0)
	{
		return;
	}
	// The players in the game: everybody with a start position.
	auto Plays = [](const FGenPlayer& P) { return !P.bNeutral && P.StartPosition >= 0 && P.Template != nullptr; };
	for (FGenPlayer& P : Players)
	{
		if (!Plays(P) || P.bDefeated || !HasPlayerBeenDefeated(P.Index))
		{
			continue;
		}
		P.bDefeated = true;
		if (Frame > 1 && Shroud.IsValid())
		{
			// VictoryConditions::update: whoever is out of the game gets to watch all of it.
			Shroud.RevealMapForPlayerPermanently(P.Index);
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [victory f%u] p%d (%s) has been defeated"), Frame, P.Index, *P.Name);
		FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
		Text.Text = P.Index == LocalPlayer ? FString(TEXT("You have been defeated")) : FString::Printf(TEXT("%s has been defeated"), *P.Name);
		Text.Color = P.Color;
		Text.Frame = Frame;
		Text.bScreen = true;
		// Player::killPlayer: containers empty, then everything the player still has dies.
		TArray<FGenObjectID> Owned;
		for (FGenObjectID Id : ObjectOrder)
		{
			const FGenObject* O = Objects[Id].Get();
			if (O->GetOwner() == P.Index && !O->IsDead() && !O->IsDestroyed() && !O->IsProjectile())
			{
				Owned.Add(Id);
			}
		}
		for (FGenObjectID Id : Owned)
		{
			FGenObject* O = FindObject(Id);
			if (O && !O->IsDead() && !O->IsContained())
			{
				O->Kill();
			}
		}
	}

	// When only one alliance is left the game is decided.
	if (!bGameOver)
	{
		int32 Alive = INDEX_NONE;
		bool bSeveral = false;
		int32 NumPlaying = 0;
		for (const FGenPlayer& P : Players)
		{
			if (!Plays(P))
			{
				continue;
			}
			++NumPlaying;
			if (P.bDefeated)
			{
				continue;
			}
			if (Alive == INDEX_NONE) { Alive = P.Index; }
			else if (GetRelationship(Alive, P.Index) != EGenRelationship::Allies) { bSeveral = true; }
		}
		if (NumPlaying >= 2 && !bSeveral)
		{
			bGameOver = true;
			GameOverFrame = Frame;
			WinningPlayer = Alive;
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [victory f%u] the game is over: %s"), Frame,
				Alive == INDEX_NONE ? TEXT("nobody is left") : *FString::Printf(TEXT("p%d (%s) has won"), Alive, *Players[Alive].Name));
		}
	}
}

bool UGenGameLogic::HasLocalPlayerWon() const
{
	return bGameOver && WinningPlayer != INDEX_NONE
		&& (WinningPlayer == LocalPlayer || GetRelationship(WinningPlayer, LocalPlayer) == EGenRelationship::Allies);
}
