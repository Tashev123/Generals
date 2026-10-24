// Upgrade.ini, Science.ini, Rank.ini, SpecialPower.ini, and the object modules that use them.

#include "Logic/GenUpgrades.h"
#include "Core/GenIni.h"
#include "Generals1.h"

namespace
{
	float UpgPercent(const FString& V)
	{
		FString S = V;
		const bool bPct = S.RemoveFromEnd(TEXT("%"));
		const float F = FCString::Atof(*S);
		return bPct ? F / 100.f : F;
	}

	void LoadIni(const TCHAR* File, TArray<TSharedPtr<FGenIniBlock>>& Blocks)
	{
		GenIni::ParseFile(FString(TEXT("Data\\INI\\Default\\")) + File, Blocks);
		GenIni::ParseFile(FString(TEXT("Data\\INI\\")) + File, Blocks);
	}

	void AppendNames(const FGenIniEntry& E, TArray<FString>& Out)
	{
		for (const FString& V : E.Values)
		{
			if (!V.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { Out.Add(V); }
		}
	}
}

FGenUpgradeData& FGenUpgradeData::Get()
{
	static FGenUpgradeData Instance;
	return Instance;
}

bool FGenUpgradeData::Initialize()
{
	if (bInitialized)
	{
		return true;
	}
	bInitialized = true;
	const double Start = FPlatformTime::Seconds();
	const int32 FPS = GenLogic::GetLogicFPS();

	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	LoadIni(TEXT("Upgrade.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("Upgrade"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FGenUpgradeTemplate& U = Upgrades.FindOrAdd(B->Name().ToLower());
		U.Name = B->Name();
		U.DisplayName = B->GetString(TEXT("DisplayName"));
		U.bObjectUpgrade = B->GetString(TEXT("Type")).Equals(TEXT("OBJECT"), ESearchCase::IgnoreCase);
		U.BuildCost = B->GetInt(TEXT("BuildCost"), 0);
		U.BuildTime = B->GetFloat(TEXT("BuildTime"), 0.f);
		U.ButtonImage = B->GetString(TEXT("ButtonImage"));
		U.ResearchSound = B->GetString(TEXT("ResearchSound"));
	}
	// UpgradeCenter::init / friend_makeVeterancyUpgrade: the three veterancy "upgrades" are not in any INI.
	// The game makes them itself: "always per-object, not per-player", never shown.
	for (const TCHAR* VetName : { TEXT("Upgrade_Veterancy_VETERAN"), TEXT("Upgrade_Veterancy_ELITE"), TEXT("Upgrade_Veterancy_HEROIC") })
	{
		FGenUpgradeTemplate& U = Upgrades.FindOrAdd(FString(VetName).ToLower());
		U.Name = VetName;
		U.bObjectUpgrade = true;
	}

	Blocks.Reset();
	LoadIni(TEXT("Science.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("Science"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FGenScience& S = Sciences.FindOrAdd(B->Name().ToLower());
		S = FGenScience();
		S.Name = B->Name();
		if (const FGenIniEntry* P = B->Find(TEXT("PrerequisiteSciences"))) { AppendNames(*P, S.Prerequisites); }
		S.PurchaseCost = B->GetInt(TEXT("SciencePurchasePointCost"), 0);
		S.bGrantable = B->GetBool(TEXT("IsGrantable"), true);
		S.DisplayName = B->GetString(TEXT("DisplayName"));
		S.Description = B->GetString(TEXT("Description"));
	}

	Blocks.Reset();
	LoadIni(TEXT("Rank.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("Rank"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 Level = FCString::Atoi(*B->Name());
		if (Level < 1 || Level > 64)
		{
			continue;
		}
		if (Ranks.Num() < Level) { Ranks.SetNum(Level); }
		FGenRank& R = Ranks[Level - 1];
		R = FGenRank();
		R.SkillPointsNeeded = B->GetInt(TEXT("SkillPointsNeeded"), 0);
		R.PurchasePointsGranted = B->GetInt(TEXT("SciencePurchasePointsGranted"), 0);
		if (const FGenIniEntry* G = B->Find(TEXT("SciencesGranted"))) { AppendNames(*G, R.SciencesGranted); }
	}

	Blocks.Reset();
	LoadIni(TEXT("SpecialPower.ini"), Blocks);
	// House rule: the Air Force General's carpet bomb recharges in 2:30, not the 4:00 its
	// ReloadTime in SpecialPower.ini gives it.
	static const TMap<FString, int32> ReloadMsOverride = { { TEXT("airf_superweaponcarpetbomb"), 150000 } };
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("SpecialPower"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FGenSpecialPowerTemplate& P = Powers.FindOrAdd(B->Name().ToLower());
		P = FGenSpecialPowerTemplate();
		P.Name = B->Name();
		P.Enum = B->GetString(TEXT("Enum"));
		P.ReloadTime = FMath::CeilToInt32(B->GetFloat(TEXT("ReloadTime"), 0.f) * float(FPS) / 1000.f);
		if (const int32* Override = ReloadMsOverride.Find(B->Name().ToLower()))
		{
			P.ReloadTime = FMath::CeilToInt32(float(*Override) * float(FPS) / 1000.f);
		}
		P.ViewObjectFrames = FMath::CeilToInt32(B->GetFloat(TEXT("ViewObjectDuration"), 0.f) * float(FPS) / 1000.f);
		P.ViewObjectRange = B->GetFloat(TEXT("ViewObjectRange"), 0.f);
		P.RequiredScience = B->GetString(TEXT("RequiredScience"));
		if (P.RequiredScience.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { P.RequiredScience.Reset(); }
		P.bPublicTimer = B->GetBool(TEXT("PublicTimer"));
		P.bSharedSyncedTimer = B->GetBool(TEXT("SharedSyncedTimer"));
		P.RadiusCursorRadius = B->GetFloat(TEXT("RadiusCursorRadius"), 0.f);
		P.InitiateSound = B->GetString(TEXT("InitiateSound"));
		P.InitiateAtLocationSound = B->GetString(TEXT("InitiateAtLocationSound"));
		// The powers whose effect exists so far. (Not yet: mines, tunnels, weapon bonuses for an area,
		// leaflets, the superweapon buildings, and the units' own abilities.)
		static const TCHAR* PortedEnums[] = { TEXT("SPECIAL_DAISY_CUTTER"), TEXT("SPECIAL_PARADROP_AMERICA"), TEXT("SPECIAL_CARPET_BOMB"),
			TEXT("SPECIAL_CHINA_CARPET_BOMB"), TEXT("SPECIAL_EMP_PULSE"), TEXT("SPECIAL_A10_THUNDERBOLT_STRIKE"), TEXT("SPECIAL_ARTILLERY_BARRAGE"),
			TEXT("SPECIAL_CASH_HACK"), TEXT("SPECIAL_AMBUSH"), TEXT("SPECIAL_ANTHRAX_BOMB"), TEXT("SPECIAL_REPAIR_VEHICLES"), TEXT("SPECIAL_CASH_BOUNTY"),
			TEXT("SPECIAL_SPY_SATELLITE"), TEXT("SPECIAL_RADAR_VAN_SCAN"), TEXT("SPECIAL_SPY_DRONE"), TEXT("SPECIAL_CIA_INTELLIGENCE"),
			TEXT("SPECIAL_GPS_SCRAMBLER"), TEXT("SPECIAL_INFANTRY_CAPTURE_BUILDING"), TEXT("SPECIAL_BLACKLOTUS_CAPTURE_BUILDING"),
			TEXT("SPECIAL_BLACKLOTUS_DISABLE_VEHICLE_HACK"), TEXT("SPECIAL_BLACKLOTUS_STEAL_CASH_HACK"), TEXT("SPECIAL_HACKER_DISABLE_BUILDING"),
			TEXT("SPECIAL_REMOTE_CHARGES"), TEXT("SPECIAL_TIMED_CHARGES"), TEXT("SPECIAL_TANKHUNTER_TNT_ATTACK"), TEXT("SPECIAL_BOOBY_TRAP"),
			TEXT("SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES"), TEXT("SPECIAL_SCUD_STORM"), TEXT("SPECIAL_NEUTRON_MISSILE"), TEXT("SPECIAL_PARTICLE_UPLINK_CANNON"), TEXT("SPECIAL_FRENZY"), TEXT("SPECIAL_LEAFLET_DROP"), TEXT("SPECIAL_CLUSTER_MINES"), TEXT("SPECIAL_SPECTRE_GUNSHIP"), TEXT("SPECIAL_SNEAK_ATTACK"), TEXT("SPECIAL_DISGUISE_AS_VEHICLE"), TEXT("SPECIAL_CHANGE_BATTLE_PLANS"), TEXT("SPECIAL_HELIX_NAPALM_BOMB") };
		// (The generals' own variants carry a prefix: EARLY_SPECIAL_..., AIRF_SPECIAL_...)
		const int32 EnumAt = P.Enum.Find(TEXT("SPECIAL_"), ESearchCase::IgnoreCase);
		const FString BaseEnum = EnumAt != INDEX_NONE ? P.Enum.Mid(EnumAt) : P.Enum;
		for (const TCHAR* Ported : PortedEnums)
		{
			P.bPorted |= BaseEnum.Equals(Ported, ESearchCase::IgnoreCase);
		}
		static const TPair<const TCHAR*, EGenAbilityKind> Kinds[] = {
			{ TEXT("SPECIAL_INFANTRY_CAPTURE_BUILDING"), EGenAbilityKind::CaptureBuilding },
			{ TEXT("SPECIAL_BLACKLOTUS_CAPTURE_BUILDING"), EGenAbilityKind::LotusCapture },
			{ TEXT("SPECIAL_BLACKLOTUS_DISABLE_VEHICLE_HACK"), EGenAbilityKind::LotusVehicleHack },
			{ TEXT("SPECIAL_BLACKLOTUS_STEAL_CASH_HACK"), EGenAbilityKind::LotusCashHack },
			{ TEXT("SPECIAL_HACKER_DISABLE_BUILDING"), EGenAbilityKind::HackerDisableBuilding },
			{ TEXT("SPECIAL_REMOTE_CHARGES"), EGenAbilityKind::RemoteCharges },
			{ TEXT("SPECIAL_TIMED_CHARGES"), EGenAbilityKind::TimedCharges },
			{ TEXT("SPECIAL_TANKHUNTER_TNT_ATTACK"), EGenAbilityKind::TankHunterTNT },
			{ TEXT("SPECIAL_BOOBY_TRAP"), EGenAbilityKind::BoobyTrap },
			{ TEXT("SPECIAL_MISSILE_DEFENDER_LASER_GUIDED_MISSILES"), EGenAbilityKind::LaserGuidedMissiles },
			{ TEXT("SPECIAL_HELIX_NAPALM_BOMB"), EGenAbilityKind::HelixNapalmBomb },
			{ TEXT("SPECIAL_DISGUISE_AS_VEHICLE"), EGenAbilityKind::Disguise } };
		for (const TPair<const TCHAR*, EGenAbilityKind>& K : Kinds)
		{
			if (BaseEnum.Equals(K.Key, ESearchCase::IgnoreCase)) { P.AbilityKind = K.Value; }
		}
	}

	UE_LOG(LogGenerals, Log, TEXT("Upgrade data: %d upgrades, %d sciences, %d ranks, %d special powers (%.2fs)"), Upgrades.Num(), Sciences.Num(),
		Ranks.Num(), Powers.Num(), FPlatformTime::Seconds() - Start);
	return true;
}

const FGenUpgradeTemplate* FGenUpgradeData::FindUpgrade(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Upgrades.Find(Name.ToLower());
}

const FGenScience* FGenUpgradeData::FindScience(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Sciences.Find(Name.ToLower());
}

const FGenSpecialPowerTemplate* FGenUpgradeData::FindPower(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Powers.Find(Name.ToLower());
}

// ---- object modules -----------------------------------------------------------------------------

bool FGenUpgradeData::ParseUpgradeModule(const FString& Type, const FGenIniBlock& Block, FGenUpgradeModule& Out)
{
	const FGenIniEntry* Trigger = Block.Find(TEXT("TriggeredBy"));
	if (!Trigger)
	{
		return false;
	}
	Out = FGenUpgradeModule();
	Out.ModuleType = Type;
	AppendNames(*Trigger, Out.TriggeredBy);
	if (const FGenIniEntry* C = Block.Find(TEXT("ConflictsWith"))) { AppendNames(*C, Out.ConflictsWith); }
	Out.bRequiresAllTriggers = Block.GetBool(TEXT("RequiresAllTriggers"));
	auto Is = [&Type](const TCHAR* Name) { return Type.Equals(Name, ESearchCase::IgnoreCase); };

	if (Is(TEXT("WeaponSetUpgrade"))) { Out.Effect = EGenUpgradeEffect::WeaponSet; }
	else if (Is(TEXT("ArmorUpgrade"))) { Out.Effect = EGenUpgradeEffect::Armor; }
	else if (Is(TEXT("MaxHealthUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::MaxHealth;
		Out.AddMaxHealth = Block.GetFloat(TEXT("AddMaxHealth"), 0.f);
		Out.ChangeType = Block.GetString(TEXT("ChangeType"), TEXT("SAME_CURRENTHEALTH")).ToUpper();
	}
	else if (Is(TEXT("LocomotorSetUpgrade"))) { Out.Effect = EGenUpgradeEffect::LocomotorSet; }
	else if (Is(TEXT("PowerPlantUpgrade"))) { Out.Effect = EGenUpgradeEffect::PowerPlant; }
	else if (Is(TEXT("CommandSetUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::CommandSet;
		Out.CommandSet = Block.GetString(TEXT("CommandSet"));
		Out.CommandSetAlt = Block.GetString(TEXT("CommandSetAlt"));
		Out.TriggerAlt = Block.GetString(TEXT("TriggerAlt"));
	}
	else if (Is(TEXT("ModelConditionUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::ModelCondition;
		Out.ConditionFlag = Block.GetString(TEXT("ConditionFlag"));
	}
	else if (Is(TEXT("SubObjectsUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::SubObjects;
		for (const FGenIniEntry& E : Block.Entries)
		{
			if (E.Key.Equals(TEXT("ShowSubObjects"), ESearchCase::IgnoreCase)) { AppendNames(E, Out.ShowSubObjects); }
			else if (E.Key.Equals(TEXT("HideSubObjects"), ESearchCase::IgnoreCase)) { AppendNames(E, Out.HideSubObjects); }
		}
	}
	else if (Is(TEXT("GrantScienceUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::GrantScience;
		Out.Science = Block.GetString(TEXT("GrantScience"));
	}
	else if (Is(TEXT("WeaponBonusUpgrade"))) { Out.Effect = EGenUpgradeEffect::WeaponBonus; }
	else if (Is(TEXT("AutoHealBehavior"))) { Out.Effect = EGenUpgradeEffect::AutoHeal; }
	else if (Is(TEXT("UnpauseSpecialPowerUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::UnpauseSpecialPower;
		Out.SpecialPower = Block.GetString(TEXT("SpecialPowerTemplate"));
	}
	else if (Is(TEXT("StealthUpgrade"))) { Out.Effect = EGenUpgradeEffect::Stealth; }
	else if (Is(TEXT("CountermeasuresBehavior"))) { Out.Effect = EGenUpgradeEffect::Countermeasures; }
	else if (Is(TEXT("GenerateMinefieldBehavior"))) { Out.Effect = EGenUpgradeEffect::GenerateMinefield; }
	else if (Is(TEXT("ObjectCreationUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::ObjectCreation;
		Out.OCL = Block.GetString(TEXT("UpgradeObject"));
	}
	else if (Is(TEXT("PassengersFireUpgrade"))) { Out.Effect = EGenUpgradeEffect::PassengersFire; }
	else if (Is(TEXT("CostModifierUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::CostModifier;
		if (const FGenIniEntry* K = Block.Find(TEXT("EffectKindOf"))) { for (const FString& V : K->Values) { Out.EffectKindOf.Add(V.ToUpper()); } }
		Out.Percentage = UpgPercent(Block.GetString(TEXT("Percentage"), TEXT("0")));
	}
	else if (Is(TEXT("SpyVisionUpdate")))
	{
		// (Only the kind an upgrade switches on; the CIA's is a special power's.)
		if (!Block.GetBool(TEXT("NeedsUpgrade")))
		{
			return false;
		}
		Out.Effect = EGenUpgradeEffect::SpyVision;
		if (const FGenIniEntry* K = Block.Find(TEXT("SpyOnKindof"))) { AppendNames(*K, Out.SpyOnKindOf); }
		Out.SpyDuration = GenLogic::MsecToFrames(Block.GetFloat(TEXT("SelfPoweredDuration"), 0.f));
		Out.SpyInterval = GenLogic::MsecToFrames(Block.GetFloat(TEXT("SelfPoweredInterval"), 0.f));
	}
	else if (Is(TEXT("RadarUpgrade")))
	{
		Out.Effect = EGenUpgradeEffect::Radar;
		Out.bDisableProof = Block.GetBool(TEXT("DisableProof"));
	}
	return true;
}

bool FGenUpgradeData::ParseSpecialPowerModule(const FString& Type, const FGenIniBlock& Block, FGenSpecialPowerModule& Out)
{
	const FGenIniEntry* Power = Block.Find(TEXT("SpecialPowerTemplate"));
	// (Updates and die modules that merely name a power are not power modules.)
	if (!Power || Type.EndsWith(TEXT("Update"), ESearchCase::IgnoreCase) || Type.EndsWith(TEXT("Die"), ESearchCase::IgnoreCase)
		|| Type.EndsWith(TEXT("Upgrade"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	Out = FGenSpecialPowerModule();
	Out.ModuleType = Type;
	Out.Power = Power->Value();
	Out.bScriptedOnly = Block.GetBool(TEXT("ScriptedSpecialPowerOnly"));
	Out.bStartsPaused = Block.GetBool(TEXT("StartsPaused"));
	if (Type.Equals(TEXT("OCLSpecialPower"), ESearchCase::IgnoreCase))
	{
		Out.Type = EGenPowerModule::OCL;
		Out.OCL = Block.GetString(TEXT("OCL"));
		Out.CreateLocation = Block.GetString(TEXT("CreateLocation"), TEXT("CREATE_AT_EDGE_NEAR_SOURCE")).ToUpper();
		Out.bAdjustToPassable = Block.GetBool(TEXT("OCLAdjustPositionToPassable"));
		Out.ReferenceObject = Block.GetString(TEXT("ReferenceObject"));
		for (const FGenIniEntry& E : Block.Entries)
		{
			// "UpgradeOCL = SCIENCE_Paradrop3 SUPERWEAPON_Paradrop3"
			if (E.Key.Equals(TEXT("UpgradeOCL"), ESearchCase::IgnoreCase) && E.Values.Num() >= 2)
			{
				Out.UpgradeOCLs.Emplace(E.Values[0], E.Values[1]);
			}
		}
	}
	else if (Type.Equals(TEXT("CashHackSpecialPower"), ESearchCase::IgnoreCase))
	{
		Out.Type = EGenPowerModule::CashHack;
		Out.MoneyAmount = Block.GetInt(TEXT("MoneyAmount"), 0);
		for (const FGenIniEntry& E : Block.Entries)
		{
			if (E.Key.Equals(TEXT("UpgradeMoneyAmount"), ESearchCase::IgnoreCase) && E.Values.Num() >= 2)
			{
				Out.UpgradeMoney.Emplace(E.Values[0], FCString::Atoi(*E.Values[1]));
			}
		}
	}
	else if (Type.Equals(TEXT("CashBountyPower"), ESearchCase::IgnoreCase))
	{
		Out.Type = EGenPowerModule::CashBounty;
		Out.Bounty = UpgPercent(Block.GetString(TEXT("Bounty"), TEXT("0")));
	}
	else if (Type.Equals(TEXT("SpyVisionSpecialPower"), ESearchCase::IgnoreCase))
	{
		Out.Type = EGenPowerModule::SpyVision;
		Out.Duration = GenLogic::MsecToFrames(Block.GetFloat(TEXT("BaseDuration"), 0.f));
	}
	else if (Type.Equals(TEXT("SpecialAbility"), ESearchCase::IgnoreCase))
	{
		Out.Type = EGenPowerModule::SpecialAbility;
		Out.InitiateSound = Block.GetString(TEXT("InitiateSound"));
	}
	return true;
}

void FGenUpgradeData::ParseAbility(const FGenIniBlock& Block, FGenAbilityData& Out)
{
	Out = FGenAbilityData();
	Out.Power = Block.GetString(TEXT("SpecialPowerTemplate"));
	Out.StartAbilityRange = Block.GetFloat(TEXT("StartAbilityRange"), Out.StartAbilityRange);
	Out.AbortRange = Block.GetFloat(TEXT("AbilityAbortRange"), Out.AbortRange);
	Out.UnpackFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("UnpackTime"), 0.f));
	Out.PreparationFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("PreparationTime"), 0.f));
	Out.PackFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("PackTime"), 0.f));
	Out.PackUnpackVariation = Block.GetFloat(TEXT("PackUnpackVariationFactor"), 0.f);
	Out.bSkipPackingWithNoTarget = Block.GetBool(TEXT("SkipPackingWithNoTarget"));
	Out.bNeedToFaceTarget = Block.GetBool(TEXT("NeedToFaceTarget"), true);
	Out.bDoCaptureFX = Block.GetBool(TEXT("DoCaptureFX"));
	Out.AwardXP = Block.GetInt(TEXT("AwardXPForTriggering"), 0);
	Out.SkillPoints = Block.GetInt(TEXT("SkillPointsForTriggering"), -1);
	Out.UnpackSound = Block.GetString(TEXT("UnpackSound"));
	Out.PackSound = Block.GetString(TEXT("PackSound"));
	Out.PrepSoundLoop = Block.GetString(TEXT("PrepSoundLoop"));
	Out.TriggerSound = Block.GetString(TEXT("TriggerSound"));
	Out.PersistentPrepFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("PersistentPrepTime"), 0.f));
	Out.bPersistenceRequiresRecharge = Block.GetBool(TEXT("PersistenceRequiresRecharge"));
	Out.EffectDuration = GenLogic::MsecToFrames(Block.GetFloat(TEXT("EffectDuration"), 0.f));
	Out.EffectValue = Block.GetInt(TEXT("EffectValue"), 1);
	Out.SpecialObject = Block.GetString(TEXT("SpecialObject"));
	Out.SpecialObjectBone = Block.GetString(TEXT("SpecialObjectAttachToBone"));
	Out.MaxSpecialObjects = Block.GetInt(TEXT("MaxSpecialObjects"), 1);
	Out.bSpecialObjectsPersistent = Block.GetBool(TEXT("SpecialObjectsPersistent"));
	Out.bSpecialObjectsPersistWhenOwnerDies = Block.GetBool(TEXT("SpecialObjectsPersistWhenOwnerDies"));
	Out.bUniqueSpecialObjectTargets = Block.GetBool(TEXT("UniqueSpecialObjectTargets"));
	Out.bFlipAfterUnpacking = Block.GetBool(TEXT("FlipOwnerAfterUnpacking"));
	Out.bFlipAfterPacking = Block.GetBool(TEXT("FlipOwnerAfterPacking"));
	Out.FleeRange = Block.GetFloat(TEXT("FleeRangeAfterCompletion"), 0.f);
	Out.bLoseStealthOnTrigger = Block.GetBool(TEXT("LoseStealthOnTrigger"));
	Out.PreTriggerUnstealthFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("PreTriggerUnstealthTime"), 0.f));
	Out.bApproachRequiresLOS = Block.GetBool(TEXT("ApproachRequiresLOS"), true);
	Out.DisableFXParticleSystem = Block.GetString(TEXT("DisableFXParticleSystem"));
}

void FGenUpgradeData::ParseHackInternet(const FGenIniBlock& Block, FGenHackInternet& Out)
{
	Out = FGenHackInternet();
	Out.bValid = true;
	Out.UnpackFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("UnpackTime"), 0.f));
	Out.PackFrames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("PackTime"), 0.f));
	Out.CashDelay = GenLogic::MsecToFrames(Block.GetFloat(TEXT("CashUpdateDelay"), 0.f));
	Out.CashDelayFast = GenLogic::MsecToFrames(Block.GetFloat(TEXT("CashUpdateDelayFast"), 0.f));
	Out.Cash[0] = Block.GetInt(TEXT("RegularCashAmount"), 0);
	Out.Cash[1] = Block.GetInt(TEXT("VeteranCashAmount"), 0);
	Out.Cash[2] = Block.GetInt(TEXT("EliteCashAmount"), 0);
	Out.Cash[3] = Block.GetInt(TEXT("HeroicCashAmount"), 0);
	Out.XpPerCash = Block.GetInt(TEXT("XpPerCashUpdate"), 0);
	Out.PackUnpackVariation = Block.GetFloat(TEXT("PackUnpackVariationFactor"), 0.f);
}

void FGenUpgradeData::ParseLaserDraw(const FGenIniBlock& Block, FGenLaserDraw& Out)
{
	// W3DLaserDrawModuleData
	auto Color = [&Block](const TCHAR* Key, FLinearColor& C)
	{
		// "InnerColor = R:0 G:255 B:0 A:180"
		if (const FGenIniEntry* E = Block.Find(Key))
		{
			float V[4] = { 255.f, 255.f, 255.f, 255.f };
			for (const FString& Part : E->Values)
			{
				FString K, Val;
				if (Part.Split(TEXT(":"), &K, &Val))
				{
					const int32 i = K.Equals(TEXT("R"), ESearchCase::IgnoreCase) ? 0 : K.Equals(TEXT("G"), ESearchCase::IgnoreCase) ? 1
						: K.Equals(TEXT("B"), ESearchCase::IgnoreCase) ? 2 : K.Equals(TEXT("A"), ESearchCase::IgnoreCase) ? 3 : -1;
					if (i >= 0) { V[i] = FCString::Atof(*Val); }
				}
			}
			C = FLinearColor(V[0] / 255.f, V[1] / 255.f, V[2] / 255.f, V[3] / 255.f);
		}
	};
	Out = FGenLaserDraw();
	Out.bValid = true;
	Out.Texture = Block.GetString(TEXT("Texture"));
	Out.NumBeams = FMath::Max(1, Block.GetInt(TEXT("NumBeams"), 1));
	Out.InnerBeamWidth = Block.GetFloat(TEXT("InnerBeamWidth"), 0.f);
	Out.OuterBeamWidth = Block.GetFloat(TEXT("OuterBeamWidth"), 1.f);
	Color(TEXT("InnerColor"), Out.InnerColor);
	Color(TEXT("OuterColor"), Out.OuterColor);
	Out.bTile = Block.GetBool(TEXT("Tile"));
	Out.ScrollRate = Block.GetFloat(TEXT("ScrollRate"), 0.f);
	Out.Segments = FMath::Max(1, Block.GetInt(TEXT("Segments"), 1));
	Out.ArcHeight = Block.GetFloat(TEXT("ArcHeight"), 0.f);
	Out.SegmentOverlapRatio = Block.GetFloat(TEXT("SegmentOverlapRatio"), 0.f);
	Out.TilingScalar = Block.GetFloat(TEXT("TilingScalar"), 1.f);
}

void FGenUpgradeData::ParseAutoDeposit(const FGenIniBlock& Block, FGenAutoDeposit& Out)
{
	Out = FGenAutoDeposit();
	Out.bValid = true;
	Out.Frames = GenLogic::MsecToFrames(Block.GetFloat(TEXT("DepositTiming"), 0.f));
	Out.Amount = Block.GetInt(TEXT("DepositAmount"), 0);
	Out.CaptureBonus = Block.GetInt(TEXT("InitialCaptureBonus"), 0);
	Out.bActualMoney = Block.GetBool(TEXT("ActualMoney"), true);
	for (const FGenIniEntry& E : Block.Entries)
	{
		// "UpgradedBoost = UpgradeType:Upgrade_AmericaSupplyLines Boost:20"
		if (E.Key.Equals(TEXT("UpgradedBoost"), ESearchCase::IgnoreCase))
		{
			FString Upgrade;
			int32 Boost = 0;
			for (const FString& V : E.Values)
			{
				FString Key, Value;
				if (V.Split(TEXT(":"), &Key, &Value))
				{
					if (Key.Equals(TEXT("UpgradeType"), ESearchCase::IgnoreCase)) { Upgrade = Value; }
					else if (Key.Equals(TEXT("Boost"), ESearchCase::IgnoreCase)) { Boost = FCString::Atoi(*Value); }
				}
			}
			if (!Upgrade.IsEmpty()) { Out.UpgradeBoost.Emplace(Upgrade, Boost); }
		}
	}
}

void FGenUpgradeData::ParseAutoHeal(const FGenIniBlock& Block, FGenAutoHeal& Out)
{
	Out = FGenAutoHeal();
	Out.bValid = true;
	Out.Amount = Block.GetFloat(TEXT("HealingAmount"), 0.f);
	Out.Delay = FMath::Max(1, GenLogic::MsecToFrames(Block.GetFloat(TEXT("HealingDelay"), 0.f)));
	Out.StartDelay = GenLogic::MsecToFrames(Block.GetFloat(TEXT("StartHealingDelay"), 0.f));
	Out.Radius = FCString::Atof(*Block.GetString(TEXT("Radius"), TEXT("0"))); // (the data writes "100.0f")
	Out.bStartsActive = Block.GetBool(TEXT("StartsActive"));
	Out.bSingleBurst = Block.GetBool(TEXT("SingleBurst"));
	Out.bAffectsWholePlayer = Block.GetBool(TEXT("AffectsWholePlayer"));
	for (const FGenIniEntry& E : Block.Entries)
	{
		if (E.Key.Equals(TEXT("KindOf"), ESearchCase::IgnoreCase)) { for (const FString& V : E.Values) { Out.KindOf.Add(V.ToUpper()); } }
		else if (E.Key.Equals(TEXT("ForbiddenKindOf"), ESearchCase::IgnoreCase)) { for (const FString& V : E.Values) { Out.ForbiddenKindOf.Add(V.ToUpper()); } }
	}
}
