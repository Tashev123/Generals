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
		P.RequiredScience = B->GetString(TEXT("RequiredScience"));
		if (P.RequiredScience.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { P.RequiredScience.Reset(); }
		P.bPublicTimer = B->GetBool(TEXT("PublicTimer"));
		P.bSharedSyncedTimer = B->GetBool(TEXT("SharedSyncedTimer"));
		P.RadiusCursorRadius = B->GetFloat(TEXT("RadiusCursorRadius"), 0.f);
		// The powers whose effect exists so far. (Not yet: mines, tunnels, weapon bonuses for an area, anything
		// to do with vision, leaflets, the superweapon buildings, and the units' own abilities.)
		static const TCHAR* PortedEnums[] = { TEXT("SPECIAL_DAISY_CUTTER"), TEXT("SPECIAL_PARADROP_AMERICA"), TEXT("SPECIAL_CARPET_BOMB"),
			TEXT("SPECIAL_CHINA_CARPET_BOMB"), TEXT("SPECIAL_EMP_PULSE"), TEXT("SPECIAL_A10_THUNDERBOLT_STRIKE"), TEXT("SPECIAL_ARTILLERY_BARRAGE"),
			TEXT("SPECIAL_CASH_HACK"), TEXT("SPECIAL_AMBUSH"), TEXT("SPECIAL_ANTHRAX_BOMB"), TEXT("SPECIAL_REPAIR_VEHICLES"), TEXT("SPECIAL_CASH_BOUNTY") };
		// (The generals' own variants carry a prefix: EARLY_SPECIAL_..., AIRF_SPECIAL_...)
		const int32 EnumAt = P.Enum.Find(TEXT("SPECIAL_"), ESearchCase::IgnoreCase);
		const FString BaseEnum = EnumAt != INDEX_NONE ? P.Enum.Mid(EnumAt) : P.Enum;
		for (const TCHAR* Ported : PortedEnums)
		{
			P.bPorted |= BaseEnum.Equals(Ported, ESearchCase::IgnoreCase);
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
	return true;
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
