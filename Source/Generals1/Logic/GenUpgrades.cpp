// Upgrades, sciences (general's promotions) and ranks.
// Follows Upgrade.cpp, UpgradeModule.h (UpgradeMux), the *Upgrade modules, ProductionUpdate::queueUpgrade,
// Player.cpp (skill points, ranks, sciences, cash bounty) and AutoHealBehavior.cpp.

#include "Logic/GenUpgrades.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Generals1.h"

namespace
{
	const FString PlayerUpgradeFlag(TEXT("PLAYER_UPGRADE"));

	bool ContainsName(const TArray<FString>& Names, const FString& Name)
	{
		return Names.ContainsByPredicate([&Name](const FString& N) { return N.Equals(Name, ESearchCase::IgnoreCase); });
	}
}

// ---- objects ------------------------------------------------------------------------------------

void FGenObject::InitUpgrades()
{
	const uint32 Frame = Logic->GetFrame();
	UpgradeModuleDone.Init(false, Template->UpgradeModules.Num());
	PowerReady.Init(0, Template->PowerModules.Num());
	PowerPaused.Init(false, Template->PowerModules.Num());
	for (int32 i = 0; i < Template->PowerModules.Num(); ++i)
	{
		// SpecialPowerModule: a power with its own timer starts out recharging; one whose timer the
		// player's buildings share is ready as soon as its science is bought.
		const FGenSpecialPowerModule& M = Template->PowerModules[i];
		const FGenSpecialPowerTemplate* P = FGenUpgradeData::Get().FindPower(M.Power);
		PowerReady[i] = (P && !P->bSharedSyncedTimer) ? Frame + uint32(P->ReloadTime) : 0;
		PowerPaused[i] = M.bStartsPaused;
	}
	bAutoHealActive = Template->AutoHeal.bValid && Template->AutoHeal.bStartsActive;
	for (const FString& Granted : Template->GrantUpgrades)
	{
		ObjectUpgrades.AddUnique(Granted.ToLower());
	}
	UpdateUpgradeModules();
}

bool FGenObject::HasObjectUpgrade(const FString& UpgradeName) const
{
	return ObjectUpgrades.Contains(UpgradeName.ToLower());
}

bool FGenObject::HasUpgrade(const FString& UpgradeName) const
{
	return HasObjectUpgrade(UpgradeName) || Logic->HasUpgradeComplete(Owner, UpgradeName);
}

bool FGenObject::WouldUpgrade(const FGenUpgradeModule& Module, const TArray<FString>& ExtraUpgrades) const
{
	// UpgradeMux::wouldUpgrade: none of the conflicting upgrades, and any (or all) of the triggers.
	auto Has = [this, &ExtraUpgrades](const FString& Name) { return HasUpgrade(Name) || ContainsName(ExtraUpgrades, Name); };
	if (Module.TriggeredBy.Num() == 0)
	{
		return false;
	}
	for (const FString& Conflict : Module.ConflictsWith)
	{
		if (Has(Conflict))
		{
			return false;
		}
	}
	int32 Owned = 0;
	for (const FString& Trigger : Module.TriggeredBy)
	{
		Owned += Has(Trigger) ? 1 : 0;
	}
	return Module.bRequiresAllTriggers ? Owned == Module.TriggeredBy.Num() : Owned > 0;
}

bool FGenObject::AffectedByUpgrade(const FGenUpgradeTemplate& Upgrade) const
{
	const TArray<FString> Extra = { Upgrade.Name };
	for (int32 i = 0; i < Template->UpgradeModules.Num(); ++i)
	{
		const FGenUpgradeModule& M = Template->UpgradeModules[i];
		if (!UpgradeModuleDone[i] && ContainsName(M.TriggeredBy, Upgrade.Name) && WouldUpgrade(M, Extra))
		{
			return true;
		}
	}
	return false;
}

bool FGenObject::HasPortedUpgradeEffect(const FGenUpgradeTemplate& Upgrade) const
{
	// (A change of command set alone doesn't count when it comes with something that isn't there yet:
	// the mines upgrade swaps the buttons and lays mines.)
	bool bReal = false, bCommandSet = false, bMissing = false;
	for (const FGenUpgradeModule& M : Template->UpgradeModules)
	{
		if (ContainsName(M.TriggeredBy, Upgrade.Name))
		{
			bMissing |= M.Effect == EGenUpgradeEffect::Other;
			bCommandSet |= M.Effect == EGenUpgradeEffect::CommandSet;
			bReal |= M.Effect != EGenUpgradeEffect::Other && M.Effect != EGenUpgradeEffect::CommandSet;
		}
	}
	return bReal || (bCommandSet && !bMissing);
}

void FGenObject::GiveObjectUpgrade(const FGenUpgradeTemplate& Upgrade)
{
	ObjectUpgrades.AddUnique(Upgrade.Name.ToLower());
	UpdateUpgradeModules();
}

void FGenObject::RemoveObjectUpgrade(const FString& UpgradeName)
{
	if (ObjectUpgrades.Remove(UpgradeName.ToLower()) == 0)
	{
		return;
	}
	for (int32 i = 0; i < Template->UpgradeModules.Num(); ++i)
	{
		if (ContainsName(Template->UpgradeModules[i].TriggeredBy, UpgradeName))
		{
			UpgradeModuleDone[i] = false; // UpgradeMux::resetUpgrade
		}
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [upgrade f%u] #%u %s no longer has %s"), Logic->GetFrame(), ID, *Template->Name, *UpgradeName);
}

void FGenObject::UpdateUpgradeModules()
{
	static const TArray<FString> None;
	for (int32 i = 0; i < Template->UpgradeModules.Num(); ++i)
	{
		if (!UpgradeModuleDone[i] && WouldUpgrade(Template->UpgradeModules[i], None))
		{
			UpgradeModuleDone[i] = true;
			ExecuteUpgradeModule(Template->UpgradeModules[i]);
		}
	}
}

void FGenObject::ExecuteUpgradeModule(const FGenUpgradeModule& Module)
{
	const FGenGameData& GD = FGenGameData::Get();
	switch (Module.Effect)
	{
	case EGenUpgradeEffect::WeaponSet:
	{
		static const int32 WeaponSetUpgraded = GenConditions::Index(TEXT("WEAPONSET_PLAYER_UPGRADE"));
		SetFlags.AddUnique(PlayerUpgradeFlag);
		RefreshWeaponSet();
		SetModelCondition(WeaponSetUpgraded, true);
		break;
	}
	case EGenUpgradeEffect::Armor:
		bArmorUpgraded = true;
		RefreshArmorSet();
		break;
	case EGenUpgradeEffect::MaxHealth:
	{
		// ActiveBody::setMaxHealth
		const float OldMax = MaxHealth;
		MaxHealth = FMath::Max(1.f, MaxHealth + Module.AddMaxHealth);
		if (Module.ChangeType == TEXT("PRESERVE_RATIO")) { Health = Health / FMath::Max(OldMax, 1.f) * MaxHealth; }
		else if (Module.ChangeType == TEXT("ADD_CURRENT_HEALTH_TOO")) { Health += Module.AddMaxHealth; }
		Health = FMath::Clamp(Health, bDead ? 0.f : 1.f, MaxHealth);
		break;
	}
	case EGenUpgradeEffect::LocomotorSet:
		bLocoUpgraded = true;
		ChooseLocomotorSet(TEXT("SET_NORMAL"));
		break;
	case EGenUpgradeEffect::PowerPlant:
	{
		static const int32 Upgraded = GenConditions::Index(TEXT("POWER_PLANT_UPGRADED"));
		bPowerPlantUpgraded = true;
		SetModelCondition(Upgraded, true);
		break;
	}
	case EGenUpgradeEffect::CommandSet:
		CommandSetOverride = (!Module.TriggerAlt.IsEmpty() && !Module.CommandSetAlt.IsEmpty() && HasUpgrade(Module.TriggerAlt))
			? Module.CommandSetAlt : Module.CommandSet;
		break;
	case EGenUpgradeEffect::ModelCondition:
		if (!Module.ConditionFlag.IsEmpty())
		{
			SetModelCondition(GenConditions::Index(Module.ConditionFlag), true);
		}
		break;
	case EGenUpgradeEffect::SubObjects:
		for (const FString& Name : Module.ShowSubObjects) { SubObjectOverrides.Emplace(Name, true); }
		for (const FString& Name : Module.HideSubObjects) { SubObjectOverrides.Emplace(Name, false); }
		++SubObjectVersion;
		break;
	case EGenUpgradeEffect::GrantScience:
		Logic->GrantScience(Owner, Module.Science);
		break;
	case EGenUpgradeEffect::WeaponBonus:
		BonusConditions.AddUnique(PlayerUpgradeFlag);
		break;
	case EGenUpgradeEffect::AutoHeal:
		bAutoHealActive = Template->AutoHeal.bValid;
		break;
	case EGenUpgradeEffect::UnpauseSpecialPower:
		for (int32 i = 0; i < Template->PowerModules.Num(); ++i)
		{
			if (Template->PowerModules[i].Power.Equals(Module.SpecialPower, ESearchCase::IgnoreCase))
			{
				// SpecialPowerModule::pauseCountdown: a paused timer stood still, so a unit's ability that
				// waited for its upgrade has its whole recharge before it.
				if (PowerPaused[i] && Template->PowerModules[i].Type == EGenPowerModule::SpecialAbility)
				{
					const FGenSpecialPowerTemplate* P = FGenUpgradeData::Get().FindPower(Module.SpecialPower);
					PowerReady[i] = Logic->GetFrame() + uint32(P ? P->ReloadTime : 0);
				}
				PowerPaused[i] = false;
			}
		}
		break;
	case EGenUpgradeEffect::CostModifier:
		Logic->RefreshCostModifiers(); // (the module is marked done by now; the player's list is made from those)
		break;
	case EGenUpgradeEffect::Radar:
		StartRadar(Module.bDisableProof);
		break;
	case EGenUpgradeEffect::Stealth:
		bCanStealth = true; // StealthUpgrade::upgradeImplementation: OBJECT_STATUS_CAN_STEALTH
		break;
	case EGenUpgradeEffect::Countermeasures:
		bCountermeasuresActive = Template->Countermeasures.bValid;
		break;
	case EGenUpgradeEffect::SpyVision:
	{
		// SpyVisionUpdate::upgradeImplementation -> activateSpyVision(SelfPoweredDuration): "If zero, will turn on
		// permanently."
		FSpyVision& Spy = SpyVisions.AddDefaulted_GetRef();
		Spy.Module = &Module;
		Spy.bActive = true;
		Spy.OffFrame = Module.SpyDuration > 0 ? Logic->GetFrame() + uint32(Module.SpyDuration) : MAX_uint32;
		Logic->AddSpyVisionSource(ID);
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [spy f%u] #%u p%d %s: sees what the enemy's %s see%s"), Logic->GetFrame(), ID, Owner, *Template->Name,
			Module.SpyOnKindOf.Num() > 0 ? *FString::Join(Module.SpyOnKindOf, TEXT(" ")) : TEXT("everything"),
			Module.SpyDuration > 0 ? *FString::Printf(TEXT(" for %d frames every %d"), Module.SpyDuration, Module.SpyInterval) : TEXT(""));
		break;
	}
	case EGenUpgradeEffect::GenerateMinefield:
		PlaceMines(); // GenerateMinefieldBehavior::upgradeImplementation
		break;
	case EGenUpgradeEffect::ObjectCreation:
		Logic->DoOCL(Module.OCL, *this); // ObjectCreationUpgrade::upgradeImplementation: "spawn everything in the OCL"
		break;
	case EGenUpgradeEffect::ReplaceObject:
		Logic->ReplaceWithObject(*this, Module.ReplaceObject);
		break;
	case EGenUpgradeEffect::ExperienceScalar:
		XPScalar += Module.AddXPScalar; // ExperienceScalarUpgrade::upgradeImplementation
		break;
	case EGenUpgradeEffect::PassengersFire:
		if (Contain)
		{
			Contain->bPassengersFire = true; // PassengersFireUpgrade: OpenContain::setPassengerAllowedToFire
		}
		break;
	default:
		break; // experience: not ported
	}
}

void FGenObject::RefreshArmorSet()
{
	// Object::setArmorSetFlag: the set that fits what it has now.
	const FGenGameData& GD = FGenGameData::Get();
	TArray<FString> Flags;
	if (bArmorUpgraded) { Flags.Add(PlayerUpgradeFlag); }
	if (CrateArmorLevel == 1) { Flags.Add(TEXT("CRATE_UPGRADE_ONE")); }
	else if (CrateArmorLevel >= 2) { Flags.Add(TEXT("CRATE_UPGRADE_TWO")); }
	if (bSecondLife) { Flags.Add(TEXT("SECOND_LIFE")); } // UndeadBody::startSecondLife: setArmorSetFlag( ARMORSET_SECOND_LIFE )
	if (const FGenArmorSetEntry* AS = Template->FindArmorSet(Flags))
	{
		Armor = GD.FindArmor(AS->Armor);
		DamageFX = GD.FindDamageFX(AS->DamageFX);
	}
}

void FGenObject::RefreshWeaponSet()
{
	// WeaponSet::updateWeaponSet: slots whose weapon changes start fresh, the others keep their state.
	const FGenGameData& GD = FGenGameData::Get();
	const FGenWeaponSetEntry* WS = Template->FindWeaponSet(SetFlags);
	WeaponSetEntry = WS;
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenWeaponTemplate* New = WS ? GD.FindWeapon(WS->Weapons[s]) : nullptr;
		bNoAutoChoose[s] = WS && WS->bNoAutoChoose[s];
		if (New != Weapons[s].Template)
		{
			Weapons[s].Template = New;
			Weapons[s].AmmoInClip = New ? New->ClipSize : 0;
			Weapons[s].ReadyFrame = 0;
			Weapons[s].bReloading = false;
		}
	}
}

const FString& FGenObject::GetCommandSet() const
{
	return CommandSetOverride.IsEmpty() ? Template->CommandSet : CommandSetOverride;
}

int32 FGenObject::GetEnergyProduction() const
{
	// (Player::addPowerBonus: the rods upgrade and an overcharge each add the bonus.)
	return Template->EnergyProduction + (bPowerPlantUpgraded ? Template->EnergyBonus : 0) + (bOverchargeActive ? Template->EnergyBonus : 0);
}

bool FGenObject::CanOvercharge() const
{
	// ControlBar::getCommandAvailability: "not allowed when health below" - and not while it is being built or sold.
	return Template->bOvercharge && !bDead && !bUnderConstruction && SellFrame == 0 && Health >= MaxHealth * Template->OverchargeMinHealth;
}

void FGenObject::SetOvercharge(bool bOn)
{
	// OverchargeBehavior::enable: the rods come out (PowerPlantUpdate::extendRods) and the player gets the bonus.
	if (bOn == bOverchargeActive || (bOn && !CanOvercharge()))
	{
		return;
	}
	bOverchargeActive = bOn;
	static const int32 Upgraded = GenConditions::Index(TEXT("POWER_PLANT_UPGRADED"));
	SetModelCondition(Upgraded, bOn || bPowerPlantUpgraded);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] #%u %s overcharge %s"), Logic->GetFrame(), ID, *Template->Name, bOn ? TEXT("on") : TEXT("off"));
}

float FGenObject::GetWeaponBonus(const FGenWeaponTemplate& Weapon, int32 Field) const
{
	if (BonusConditions.Num() == 0)
	{
		return 1.f;
	}
	float Factor = 1.f;
	// The weapon's own bonuses, and the ones GameData.ini gives every weapon (GARRISONED, ...).
	const TArray<FGenWeaponBonus>* Lists[2] = { &Weapon.Bonuses, &FGenGameData::Get().GlobalWeaponBonuses };
	for (const TArray<FGenWeaponBonus>* List : Lists)
	{
		for (const FGenWeaponBonus& B : *List)
		{
			// (Bonuses of one field add up: 125% and 125% make 150%.)
			if (int32(B.Field) == Field && BonusConditions.Contains(B.Condition))
			{
				Factor += B.Value - 1.f;
			}
		}
	}
	return FMath::Max(Factor, 0.01f);
}

void FGenObject::DoTempWeaponBonus(const FString& Condition, int32 DurationFrames)
{
	// TempWeaponBonusHelper::doTempWeaponBonus
	if (!TempBonus.IsEmpty() && !TempBonus.Equals(Condition, ESearchCase::IgnoreCase))
	{
		BonusConditions.Remove(TempBonus);
	}
	TempBonus = Condition.ToUpper();
	BonusConditions.AddUnique(TempBonus);
	TempBonusEndFrame = Logic->GetFrame() + uint32(FMath::Max(1, DurationFrames));
}

void FGenObject::UpdateWeaponBonusPulse(uint32 Frame)
{
	// WeaponBonusUpdate::update: whatever of ours (and our allies') within range is of the right kind -
	// and whoever rides in it - gets the bonus for its duration.
	const FGenThingTemplate::FWeaponBonusPulse& P = Template->WeaponBonusPulse;
	NextBonusPulseFrame = Frame + uint32(FMath::Max(1, P.Delay));
	auto RightKind = [&P](const FGenObject& O)
	{
		for (const FString& K : P.RequiredKindOf) { if (!O.IsKindOf(*K)) { return false; } }
		for (const FString& K : P.ForbiddenKindOf) { if (O.IsKindOf(*K)) { return false; } }
		return true;
	};
	int32 Num = 0;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), P.Range, [&](FGenObject& Other)
	{
		if (Other.bDead || Other.bPendingDestroy || Logic->GetRelationship(Owner, Other.Owner) != EGenRelationship::Allies)
		{
			return;
		}
		if (RightKind(Other))
		{
			Other.DoTempWeaponBonus(P.Condition, P.Duration);
			++Num;
		}
		for (FGenObjectID RiderId : Other.GetContain() ? Other.GetContain()->Riders : TArray<FGenObjectID>())
		{
			FGenObject* Rider = Logic->FindObject(RiderId);
			if (Rider && RightKind(*Rider))
			{
				Rider->DoTempWeaponBonus(P.Condition, P.Duration);
				++Num;
			}
		}
	});
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] #%u %s: %s for %d frames to %d units within %.0f"), Frame, ID, *Template->Name, *P.Condition,
		P.Duration, Num, P.Range);
}

// ---- PropagandaTowerBehavior ---------------------------------------------------------------------

namespace
{
	const FString EnthusiasticBonus(TEXT("ENTHUSIASTIC"));
	const FString SubliminalBonus(TEXT("SUBLIMINAL"));
}

void FGenObject::RemovePropagandaInfluence()
{
	// PropagandaTowerBehavior::removeAllInfluence
	for (FGenObjectID Id : PropagandaInside)
	{
		if (FGenObject* O = Logic->FindObject(Id))
		{
			O->BonusConditions.Remove(EnthusiasticBonus);
			O->BonusConditions.Remove(SubliminalBonus);
		}
	}
	PropagandaInside.Reset();
}

void FGenObject::UpdatePropagandaTower(uint32 Frame)
{
	// PropagandaTowerBehavior::update
	const FGenThingTemplate::FPropagandaTower& P = Template->Propaganda;
	if (bUnderConstruction)
	{
		return;
	}
	const FGenObject* Carrier = IsContained() ? Logic->FindObject(ContainedBy) : nullptr;
	// Sold, or knocked out ("we need to let go of everyone if we are EMPd or underpowered"), or "our
	// container is contained: we work in a container, but not in a double container".
	if (IsBeingSold() || IsDisabled() || (Carrier && Carrier->IsContained()))
	{
		RemovePropagandaInfluence();
		return;
	}
	if (Frame - PropagandaScanFrame >= uint32(P.Delay))
	{
		DoPropagandaScan();
		PropagandaScanFrame = Frame;
	}

	// "Go through any objects in our area of influence and do the effect logic on them": every frame.
	const bool bUpgraded = !P.UpgradeRequired.IsEmpty() && Logic->HasUpgradeComplete(Owner, P.UpgradeRequired);
	const float Share = (bUpgraded ? P.UpgradedHealPercent : P.HealPercent) / float(GenLogic::GetLogicFPS());
	for (int32 i = PropagandaInside.Num() - 1; i >= 0; --i)
	{
		FGenObject* O = Logic->FindObject(PropagandaInside[i]);
		if (!O || O->bDead || O->bPendingDestroy
			|| !(O->IsKindOf(TEXT("SCORE")) || O->IsKindOf(TEXT("SCORE_CREATE")) || O->IsKindOf(TEXT("SCORE_DESTROY")) || O->IsKindOf(TEXT("MP_COUNT_FOR_VICTORY"))))
		{
			PropagandaInside.RemoveAtSwap(i);
			continue;
		}
		if (O->HasWeapons())
		{
			O->BonusConditions.AddUnique(EnthusiasticBonus);
			if (bUpgraded)
			{
				O->BonusConditions.AddUnique(SubliminalBonus);
			}
		}
		// "Dustin wants the healing effect not to stack from multiple propaganda towers."
		O->AttemptHealingFromSoleBenefactor(Share * O->MaxHealth, ID, uint32(P.Delay));
	}
}

void FGenObject::DoPropagandaScan()
{
	// PropagandaTowerBehavior::doScan: "the act of scanning is when we play our effect".
	const FGenThingTemplate::FPropagandaTower& P = Template->Propaganda;
	const bool bUpgradePresent = !P.UpgradeRequired.IsEmpty() && HasUpgrade(P.UpgradeRequired);
	bool bDoFX = true;
	if (const FGenObject* Carrier = IsContained() ? Logic->FindObject(ContainedBy) : nullptr)
	{
		if (Owner != Logic->GetLocalPlayer() && Carrier->IsStealthedAndUndetected())
		{
			bDoFX = false; // "so they don't give up their position"
		}
		if (IsKindOf(TEXT("VEHICLE")) && !IsKindOf(TEXT("PORTABLE_STRUCTURE")))
		{
			return; // "I am not a propaganda tower, I am an Emperor tank": riding in something, it does nothing
		}
	}
	if (Owner != Logic->GetLocalPlayer() && IsStealthedAndUndetected())
	{
		bDoFX = false;
	}
	const FString& Pulse = bUpgradePresent ? P.UpgradedPulseFX : P.PulseFX;
	if (bDoFX && !Pulse.IsEmpty())
	{
		Logic->DoFXObj(Pulse, *this);
	}

	// Our own and our allies', alive, not buildings, within the radius (centre to centre).
	TArray<FGenObjectID> NowInside;
	const float RadiusSq = P.Radius * P.Radius;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), P.Radius, [&](FGenObject& Other)
	{
		if (Other.bDead || Other.bPendingDestroy || (&Other == this && !P.bAffectsSelf) || Other.IsKindOf(TEXT("STRUCTURE"))
			|| (Other.IsContained() && Other.bHidden) || Logic->GetRelationship(Owner, Other.Owner) != EGenRelationship::Allies)
		{
			return;
		}
		if (FVector2f::DistSquared(FVector2f(Other.Position.X, Other.Position.Y), FVector2f(Position.X, Position.Y)) <= RadiusSq)
		{
			NowInside.Add(Other.ID);
		}
	});
	// Who has left loses the bonus ("if they are within the area of effect of another tower it's OK,
	// they'll get the bonus back again").
	for (FGenObjectID Id : PropagandaInside)
	{
		if (!NowInside.Contains(Id))
		{
			if (FGenObject* O = Logic->FindObject(Id))
			{
				O->BonusConditions.Remove(EnthusiasticBonus);
				O->BonusConditions.Remove(SubliminalBonus);
			}
		}
	}
	UE_CLOG(Logic->IsTracing() && NowInside.Num() != PropagandaInside.Num(), LogGenerals, Log, TEXT("  [propaganda f%u] #%u %s has %d unit(s) within %.0f%s"),
		Logic->GetFrame(), ID, *Template->Name, NowInside.Num(), P.Radius, bUpgradePresent ? TEXT(" (upgraded)") : TEXT(""));
	PropagandaInside = MoveTemp(NowInside);
}

void FGenObject::UpdateLeafletDrop(uint32 Frame)
{
	// LeafletDropBehavior::update
	const FGenThingTemplate::FLeafletDrop& L = Template->LeafletDrop;
	if (LeafletStartFrame == 0)
	{
		LeafletStartFrame = Frame + uint32(FMath::Max(1, L.Delay));
		if (!L.ParticleSystem.IsEmpty())
		{
			Logic->AttachParticleSystem(L.ParticleSystem, ID);
		}
	}
	if (Frame >= LeafletStartFrame)
	{
		DoLeafletDisable(Frame);
	}
}

void FGenObject::DoLeafletDisable(uint32 Frame)
{
	// LeafletDropBehavior::doDisableAttack: "both commuters and pedestrians", "only enemies".
	const FGenThingTemplate::FLeafletDrop& L = Template->LeafletDrop;
	int32 Num = 0;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), L.Radius + 40.f, [&](FGenObject& Other)
	{
		if (&Other == this || Other.bDead || !(Other.IsKindOf(TEXT("INFANTRY")) || Other.IsKindOf(TEXT("VEHICLE")))
			|| Logic->GetRelationship(Owner, Other.Owner) != EGenRelationship::Enemies)
		{
			return;
		}
		// (FROM_BOUNDINGSPHERE_3D)
		if ((Other.Position - Position).Size() - Other.Template->GetBoundingCircleRadius() <= L.Radius)
		{
			Num += Other.DisabledUntilFrame <= Frame ? 1 : 0;
			Other.DisableUntil(Frame + uint32(L.DisabledDuration));
		}
	});
	UE_CLOG(Logic->IsTracing() && Num > 0, LogGenerals, Log, TEXT("  [power f%u] #%u %s: leaflets disable %d more units for %d frames"), Frame, ID,
		*Template->Name, Num, L.DisabledDuration);
}

float FGenObject::GetAttackRange(const FGenWeaponTemplate& Weapon) const
{
	return Weapon.AttackRange * GetWeaponBonus(Weapon, FGenWeaponBonus::Range);
}

void FGenObject::UpdateAutoHeal(uint32 Frame)
{
	// AutoHealBehavior: itself, after it has been left alone for a while; or (Radius) whatever of ours stands near.
	const FGenAutoHeal& H = Template->AutoHeal;
	if (bAutoHealDone || Frame < NextAutoHealFrame)
	{
		return;
	}
	NextAutoHealFrame = Frame + uint32(H.Delay);
	if (H.Radius <= 0.f && !H.bAffectsWholePlayer)
	{
		if (Health < MaxHealth && (LastDamageFrame == 0 || Frame >= LastDamageFrame + uint32(H.StartDelay)))
		{
			AttemptHealing(H.Amount);
		}
		return;
	}
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		FGenObject* O = Logic->FindObject(Id);
		if (!O || O == this || O->bDead || O->bPendingDestroy || O->Projectile.bActive || Logic->GetRelationship(Owner, O->Owner) != EGenRelationship::Allies)
		{
			continue;
		}
		if (!H.bAffectsWholePlayer && FVector2f::DistSquared(FVector2f(O->Position.X, O->Position.Y), FVector2f(Position.X, Position.Y)) > H.Radius * H.Radius)
		{
			continue;
		}
		const bool bWanted = H.KindOf.Num() == 0 || H.KindOf.ContainsByPredicate([O](const FString& K) { return O->Template->KindOf.Contains(K); });
		const bool bForbidden = H.ForbiddenKindOf.ContainsByPredicate([O](const FString& K) { return O->Template->KindOf.Contains(K); });
		if (bWanted && !bForbidden)
		{
			O->AttemptHealing(H.Amount);
		}
	}
	bAutoHealDone = H.bSingleBurst;
}

// ---- upgrades: the player's side ----------------------------------------------------------------

bool UGenGameLogic::HasUpgradeComplete(int32 Player, const FString& Upgrade) const
{
	return Players.IsValidIndex(Player) && Players[Player].UpgradesComplete.Contains(Upgrade.ToLower());
}

bool UGenGameLogic::HasUpgradeInProduction(int32 Player, const FString& Upgrade) const
{
	return Players.IsValidIndex(Player) && Players[Player].UpgradesInProduction.Contains(Upgrade.ToLower());
}

bool UGenGameLogic::CanQueueUpgrade(const FGenObject& Researcher, const FGenUpgradeTemplate& Upgrade, FString* OutWhy) const
{
	auto No = [OutWhy](const TCHAR* Why) { if (OutWhy) { *OutWhy = Why; } return false; };
	const int32 Player = Researcher.GetOwner();
	if (!Players.IsValidIndex(Player) || Researcher.IsDead() || Researcher.IsUnderConstruction() || Researcher.IsBeingSold()
		|| !Researcher.GetTemplate()->bHasProduction)
	{
		return No(TEXT("Not available"));
	}
	// Object::canProduceUpgrade: one of its buttons has to offer it.
	const FGenCommandData& CD = FGenCommandData::Get();
	const FGenCommandSet* Set = CD.FindSet(Researcher.GetCommandSet());
	bool bOffered = false;
	for (int32 s = 0; Set && s < FGenCommandSet::NumSlots && !bOffered; ++s)
	{
		const FGenCommandButton* Button = CD.FindButton(Set->Slots[s]);
		bOffered = Button && (Button->Command == EGenCommand::PlayerUpgrade || Button->Command == EGenCommand::ObjectUpgrade)
			&& Button->Upgrade.Equals(Upgrade.Name, ESearchCase::IgnoreCase);
	}
	if (!bOffered)
	{
		return No(TEXT("Not available here"));
	}
	const bool bQueued = Researcher.GetProductionQueue().ContainsByPredicate([&Upgrade](const FGenObject::FProduction& P) { return P.Upgrade == &Upgrade; });
	if (Upgrade.bObjectUpgrade)
	{
		if (Researcher.HasObjectUpgrade(Upgrade.Name)) { return No(TEXT("Already installed")); }
		if (bQueued) { return No(TEXT("Being installed")); }
		if (!Researcher.AffectedByUpgrade(Upgrade)) { return No(TEXT("Not available (conflicts with another upgrade)")); }
	}
	else
	{
		if (HasUpgradeComplete(Player, Upgrade.Name)) { return No(TEXT("Already researched")); }
		if (bQueued || HasUpgradeInProduction(Player, Upgrade.Name)) { return No(TEXT("Being researched")); }
	}
	if (Researcher.GetProductionQueue().Num() >= Researcher.GetTemplate()->MaxQueueEntries)
	{
		return No(TEXT("Production queue is full"));
	}
	if (Players[Player].Money < Upgrade.BuildCost)
	{
		return No(TEXT("Not enough money"));
	}
	return true;
}

bool UGenGameLogic::QueueUpgrade(FGenObject& Researcher, const FGenUpgradeTemplate& Upgrade)
{
	// ProductionUpdate::queueUpgrade: paid for when queued, refunded if cancelled.
	if (!CanQueueUpgrade(Researcher, Upgrade))
	{
		return false;
	}
	FGenPlayer& P = Players[Researcher.GetOwner()];
	Withdraw(Researcher.GetOwner(), Upgrade.BuildCost);
	FGenObject::FProduction& Entry = Researcher.Production.AddDefaulted_GetRef();
	Entry.Upgrade = &Upgrade;
	Entry.Cost = Upgrade.BuildCost;
	if (!Upgrade.bObjectUpgrade)
	{
		P.UpgradesInProduction.Add(Upgrade.Name.ToLower());
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [upgrade f%u] p%d starts %s at #%u %s"), Frame, P.Index, *Upgrade.Name, Researcher.GetID(), *Researcher.GetTemplate()->Name);
	return true;
}

void UGenGameLogic::IssueQueueUpgrade(FGenObjectID Researcher, const FString& Upgrade)
{
	FGenCommand C(EGenCommandType::QueueUpgrade);
	C.Units = { Researcher };
	C.Name = Upgrade;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecQueueUpgrade(const FGenCommand& C)
{
	FGenObject* R = C.Units.Num() > 0 ? FindObject(C.Units[0]) : nullptr;
	const FGenUpgradeTemplate* U = FGenUpgradeData::Get().FindUpgrade(C.Name);
	if (R && U)
	{
		QueueUpgrade(*R, *U);
	}
}

void UGenGameLogic::FinishUpgrade(FGenObject& Researcher, const FGenUpgradeTemplate& Upgrade)
{
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [upgrade f%u] p%d finished %s"), Frame, Researcher.GetOwner(), *Upgrade.Name);
	ScoreMoneySpent(Researcher.GetOwner(), Upgrade.BuildCost);
	if (Players.IsValidIndex(Researcher.GetOwner()))
	{
		Players[Researcher.GetOwner()].UpgradeEvents.Add(Upgrade.Name.ToLower()); // ScriptEngine::notifyOfCompletedUpgrade
	}
	if (Upgrade.bObjectUpgrade)
	{
		Researcher.GiveObjectUpgrade(Upgrade);
	}
	else
	{
		GrantPlayerUpgrade(Researcher.GetOwner(), Upgrade);
	}
	if (Researcher.GetOwner() == LocalPlayer)
	{
		// ProductionUpdate: "upgrades are a more rare event, play a nifty radar event thingie"
		CreateRadarEvent(Researcher.GetPosition(), EGenRadarEvent::Upgrade);
		// "Play the sound for the upgrade, because we just built it!"
		if (Audio && !Upgrade.ResearchSound.IsEmpty())
		{
			Audio->PlayUI(Upgrade.ResearchSound, LocalPlayer);
		}
		EvaEvent(TEXT("UpgradeComplete"), LocalPlayer);
	}
}

void UGenGameLogic::GrantPlayerUpgrade(int32 Player, const FGenUpgradeTemplate& Upgrade)
{
	if (!Players.IsValidIndex(Player))
	{
		return;
	}
	const FString Key = Upgrade.Name.ToLower();
	Players[Player].UpgradesInProduction.Remove(Key);
	Players[Player].UpgradesComplete.Add(Key);
	// Player::onUpgradeCompleted: everything the player owns gets to react.
	for (int32 i = 0, Num = ObjectOrder.Num(); i < Num; ++i)
	{
		FGenObject* O = Objects[ObjectOrder[i]].Get();
		if (O->GetOwner() == Player && !O->IsDead() && !O->IsDestroyed())
		{
			O->UpdateUpgradeModules();
		}
	}
}

void UGenGameLogic::CancelUpgradeInProduction(int32 Player, const FGenUpgradeTemplate& Upgrade)
{
	if (Players.IsValidIndex(Player) && !Upgrade.bObjectUpgrade)
	{
		Players[Player].UpgradesInProduction.Remove(Upgrade.Name.ToLower());
	}
}

// ---- sciences and ranks -------------------------------------------------------------------------

void UGenGameLogic::InitPlayerSciences(FGenPlayer& P)
{
	// Player::resetRank + resetSciences: the faction's own sciences, and rank 1 with what it grants.
	const TArray<FGenRank>& Ranks = FGenUpgradeData::Get().GetRanks();
	P.Sciences.Reset();
	P.RankLevel = 1;
	P.SkillPoints = 0;
	P.SciencePurchasePoints = 0;
	if (P.Template)
	{
		for (const FString& S : P.Template->IntrinsicSciences)
		{
			P.Sciences.Add(S.ToLower());
		}
	}
	if (Ranks.Num() > 0)
	{
		P.SciencePurchasePoints = Ranks[0].PurchasePointsGranted;
		for (const FString& S : Ranks[0].SciencesGranted)
		{
			P.Sciences.Add(S.ToLower());
		}
	}
}

bool UGenGameLogic::HasScience(int32 Player, const FString& Science) const
{
	return Players.IsValidIndex(Player) && Players[Player].Sciences.Contains(Science.ToLower());
}

bool UGenGameLogic::CanPurchaseScience(int32 Player, const FString& Science, FString* OutWhy) const
{
	// Player::isCapableOfPurchasingScience
	auto No = [OutWhy](const FString& Why) { if (OutWhy) { *OutWhy = Why; } return false; };
	const FGenScience* S = FGenUpgradeData::Get().FindScience(Science);
	if (!S || !Players.IsValidIndex(Player))
	{
		return No(TEXT("Not available"));
	}
	if (HasScience(Player, Science))
	{
		return No(TEXT("Already acquired"));
	}
	for (const FString& Prereq : S->Prerequisites)
	{
		if (!HasScience(Player, Prereq))
		{
			if (Prereq.StartsWith(TEXT("SCIENCE_Rank"), ESearchCase::IgnoreCase))
			{
				return No(FString::Printf(TEXT("Requires rank %s"), *Prereq.Mid(12)));
			}
			const FGenScience* Needed = FGenUpgradeData::Get().FindScience(Prereq);
			return No(FString::Printf(TEXT("Requires %s"), Needed && !Needed->DisplayName.IsEmpty() ? *FGenCommandData::Get().GetText(Needed->DisplayName) : *Prereq));
		}
	}
	if (S->PurchaseCost <= 0)
	{
		return No(TEXT("Cannot be purchased"));
	}
	if (S->PurchaseCost > Players[Player].SciencePurchasePoints)
	{
		return No(FString::Printf(TEXT("Needs %d general's point%s"), S->PurchaseCost, S->PurchaseCost == 1 ? TEXT("") : TEXT("s")));
	}
	return true;
}

bool UGenGameLogic::PurchaseScience(int32 Player, const FString& Science)
{
	if (!CanPurchaseScience(Player, Science))
	{
		return false;
	}
	Players[Player].SciencePurchasePoints -= FGenUpgradeData::Get().FindScience(Science)->PurchaseCost;
	AddScience(Player, Science);
	return true;
}

void UGenGameLogic::IssuePurchaseScience(int32 Player, const FString& Science)
{
	FGenCommand C(EGenCommandType::PurchaseScience);
	C.Player = Player;
	C.Name = Science;
	SubmitCommand(MoveTemp(C));
}

bool UGenGameLogic::GrantScience(int32 Player, const FString& Science)
{
	const FGenScience* S = FGenUpgradeData::Get().FindScience(Science);
	return S && S->bGrantable && AddScience(Player, Science);
}

bool UGenGameLogic::AddScience(int32 Player, const FString& Science)
{
	if (!Players.IsValidIndex(Player) || HasScience(Player, Science))
	{
		return false;
	}
	FGenPlayer& P = Players[Player];
	P.Sciences.Add(Science.ToLower());
	P.ScienceEvents.Add(Science.ToLower()); // ScriptEngine::notifyOfAcquiredScience
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [science f%u] p%d acquires %s (%d point%s left)"), Frame, Player, *Science, P.SciencePurchasePoints,
		P.SciencePurchasePoints == 1 ? TEXT("") : TEXT("s"));

	// A power that needed this science is ready at once (Player::addScience -> setReadyFrame(now)).
	const FGenUpgradeData& UD = FGenUpgradeData::Get();
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (O->GetOwner() != Player)
		{
			continue;
		}
		for (const FGenSpecialPowerModule& M : O->GetTemplate()->PowerModules)
		{
			const FGenSpecialPowerTemplate* T = UD.FindPower(M.Power);
			if (!T || !T->RequiredScience.Equals(Science, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (T->bSharedSyncedTimer)
			{
				P.PowerReadyFrame.Add(T->Name.ToLower(), Frame);
			}
			// CashBountyPower: the share of every kill's cost this player now collects.
			if (M.Type == EGenPowerModule::CashBounty)
			{
				P.CashBounty = FMath::Max(P.CashBounty, M.Bounty);
			}
		}
	}
	return true;
}

bool UGenGameLogic::ConsumeScriptEvent(int32 Player, const FString& Name, bool bScience)
{
	// ScriptEngine::isUpgradeComplete / isScienceAcquired: the first script to ask takes the event off the list.
	if (!Players.IsValidIndex(Player))
	{
		return false;
	}
	TArray<FString>& Events = bScience ? Players[Player].ScienceEvents : Players[Player].UpgradeEvents;
	return Events.RemoveSingle(Name.ToLower()) > 0;
}

void UGenGameLogic::AddSkillPoints(int32 Player, int32 Points)
{
	// Player::addSkillPoints / setRankLevel
	if (!Players.IsValidIndex(Player) || Points <= 0)
	{
		return;
	}
	FGenPlayer& P = Players[Player];
	const TArray<FGenRank>& Ranks = FGenUpgradeData::Get().GetRanks();
	if (Ranks.Num() == 0)
	{
		return;
	}
	// (Capped at what the highest rank needs.)
	P.SkillPoints = FMath::Min(P.SkillPoints + Points, Ranks.Last().SkillPointsNeeded);
	while (P.RankLevel < Ranks.Num() && P.SkillPoints >= Ranks[P.RankLevel].SkillPointsNeeded)
	{
		const FGenRank& Next = Ranks[P.RankLevel];
		++P.RankLevel;
		P.SciencePurchasePoints = FMath::Max(0, P.SciencePurchasePoints + Next.PurchasePointsGranted);
		for (const FString& S : Next.SciencesGranted)
		{
			AddScience(Player, S);
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [science f%u] p%d promoted to rank %d (%d skill points, %d to spend)"), Frame, Player, P.RankLevel,
			P.SkillPoints, P.SciencePurchasePoints);
		if (Player == LocalPlayer)
		{
			EvaEvent(TEXT("GeneralLevelUp"), Player); // Player::setRankLevel
			FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
			Text.Text = FString::Printf(TEXT("Promoted: rank %d"), P.RankLevel);
			Text.Color = FLinearColor(1.f, 0.85f, 0.2f);
			Text.Frame = Frame;
			Text.bScreen = true;
		}
	}
}

void UGenGameLogic::OnObjectKilled(const FGenObject& Victim, FGenObjectID KillerId)
{
	// Object::scoreTheKill: the killer's player earns skill points towards the next rank, and (GLA's
	// Cash Bounty) a share of what the victim cost.
	FGenObject* Killer = FindObject(KillerId);
	if (Killer)
	{
		ScoreKill(Victim, *Killer);
	}
	if (!Killer || Victim.IsUnderConstruction() || !Players.IsValidIndex(Killer->GetOwner())
		|| GetRelationship(Killer->GetOwner(), Victim.GetOwner()) != EGenRelationship::Enemies
		|| Victim.IsKindOf(TEXT("IGNORED_IN_GUI")))
	{
		return;
	}
	// What the victim is worth depends on its own level (ThingTemplate::getExperienceValue / getSkillPointValue).
	const FGenThingTemplate* T = Victim.GetTemplate();
	const int32 Level = FMath::Clamp(Victim.GetVeterancyLevel(), 0, 3);
	AddSkillPoints(Killer->GetOwner(), T->SkillPointValues[Level] >= 0 ? T->SkillPointValues[Level] : T->ExperienceValues[Level]);
	// ... and the killer itself learns from it (a shot that outlived its launcher teaches nobody); what
	// a planted charge kills teaches whoever planted it.
	FGenObject* Learner = Killer->XPSink != GenInvalidObjectID ? FindObject(Killer->XPSink) : Killer;
	if (Learner && !Learner->IsDead())
	{
		// ("addExperiencePoints( experienceGain * m_experienceScalar )": what goes to a sink is scaled by the killer's too.)
		Learner->AddExperience(Learner != Killer ? FMath::TruncToInt32(float(T->ExperienceValues[Level]) * Killer->XPScalar) : T->ExperienceValues[Level]);
	}

	FGenPlayer& P = Players[Killer->GetOwner()];
	const int32 Bounty = FMath::CeilToInt32(T->BuildCost * P.CashBounty);
	if (Bounty > 0)
	{
		Deposit(Killer->GetOwner(), Bounty);
		P.Score.MoneyEarned += Bounty;
		FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
		Text.Text = FString::Printf(TEXT("+$%d"), Bounty);
		Text.Pos = Victim.GetPosition();
		Text.Color = P.Color;
		Text.Frame = Frame;
	}
}
