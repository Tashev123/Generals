#include "Logic/GenGameData.h"
#include "Core/GenFileSystem.h"
#include "Generals1.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarGenLogicFPS(
	TEXT("gen.LogicFPS"), 30,
	TEXT("Simulation (logic) frames per second. Rendering is independent and interpolated. Read when game data loads."),
	ECVF_Default);

int32 GenLogic::GetLogicFPS()
{
	static int32 Cached = 0;
	if (Cached == 0)
	{
		Cached = FMath::Clamp(CVarGenLogicFPS.GetValueOnGameThread(), 10, 240);
	}
	return Cached;
}

namespace
{
	FColor ParseIniColor(const FString& Raw)
	{
		// "R:255 G:0 B:0"
		FColor C(255, 255, 255, 255);
		TArray<FString> Tokens;
		GenIni::Tokenize(Raw, Tokens, TEXT(" \t=:"));
		for (int32 i = 0; i + 1 < Tokens.Num(); ++i)
		{
			const int32 V = FMath::Clamp(FCString::Atoi(*Tokens[i + 1]), 0, 255);
			if (Tokens[i].Equals(TEXT("R"), ESearchCase::IgnoreCase)) { C.R = uint8(V); }
			else if (Tokens[i].Equals(TEXT("G"), ESearchCase::IgnoreCase)) { C.G = uint8(V); }
			else if (Tokens[i].Equals(TEXT("B"), ESearchCase::IgnoreCase)) { C.B = uint8(V); }
		}
		return C;
	}

	uint32 ParseSurfaces(const TArray<FString>& Values)
	{
		uint32 S = 0;
		for (const FString& V : Values)
		{
			if (V.Equals(TEXT("GROUND"), ESearchCase::IgnoreCase)) { S |= GenSurface::Ground; }
			else if (V.Equals(TEXT("WATER"), ESearchCase::IgnoreCase)) { S |= GenSurface::Water; }
			else if (V.Equals(TEXT("CLIFF"), ESearchCase::IgnoreCase)) { S |= GenSurface::Cliff; }
			else if (V.Equals(TEXT("AIR"), ESearchCase::IgnoreCase)) { S |= GenSurface::Air; }
			else if (V.Equals(TEXT("RUBBLE"), ESearchCase::IgnoreCase)) { S |= GenSurface::Rubble; }
		}
		return S;
	}

	EGenLocoAppearance ParseAppearance(const FString& V)
	{
		static const TCHAR* Names[] = { TEXT("TWO_LEGS"), TEXT("FOUR_WHEELS"), TEXT("TREADS"), TEXT("HOVER"), TEXT("THRUST"),
			TEXT("WINGS"), TEXT("CLIMBER"), TEXT("OTHER"), TEXT("MOTORCYCLE") };
		for (int32 i = 0; i < UE_ARRAY_COUNT(Names); ++i)
		{
			if (V.Equals(Names[i], ESearchCase::IgnoreCase)) { return EGenLocoAppearance(i); }
		}
		return EGenLocoAppearance::Other;
	}

	void ParseLocomotor(const FGenIniBlock& B, FGenLocomotorTemplate& L)
	{
		for (const FGenIniEntry& E : B.Entries)
		{
			const FString& K = E.Key;
			auto Is = [&](const TCHAR* N) { return K.Equals(N, ESearchCase::IgnoreCase); };
			if (Is(TEXT("Surfaces"))) { L.Surfaces = ParseSurfaces(E.Values); }
			else if (Is(TEXT("Speed"))) { L.Speed = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("SpeedDamaged"))) { L.SpeedDamaged = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("TurnRate"))) { L.TurnRate = GenLogic::AngularVelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("TurnRateDamaged"))) { L.TurnRateDamaged = GenLogic::AngularVelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("Acceleration"))) { L.Acceleration = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("AccelerationDamaged"))) { L.AccelerationDamaged = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("Braking"))) { L.Braking = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("MinSpeed"))) { L.MinSpeed = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("MinTurnSpeed"))) { L.MinTurnSpeed = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("TurnPivotOffset"))) { L.TurnPivotOffset = E.AsFloat(); }
			else if (Is(TEXT("Extra2DFriction"))) { L.Extra2DFriction = GenLogic::FrictionPerFrame(E.AsFloat()); }
			else if (Is(TEXT("CloseEnoughDist"))) { L.CloseEnoughDist = E.AsFloat(); }
			else if (Is(TEXT("PreferredHeight"))) { L.PreferredHeight = E.AsFloat(); }
			else if (Is(TEXT("Appearance"))) { L.Appearance = ParseAppearance(E.Value()); }
			else if (Is(TEXT("StickToGround"))) { L.bStickToGround = E.AsBool(); }
			else if (Is(TEXT("CanMoveBackwards"))) { L.bCanMoveBackwards = E.AsBool(); }
			else if (Is(TEXT("ZAxisBehavior"))) { L.ZAxisBehavior = E.Value(); }
		}
		// LocomotorTemplate::validate: damaged values default to the normal ones.
		if (L.SpeedDamaged < 0.f) { L.SpeedDamaged = L.Speed; }
		if (L.TurnRateDamaged < 0.f) { L.TurnRateDamaged = L.TurnRate; }
		if (L.AccelerationDamaged < 0.f) { L.AccelerationDamaged = L.Acceleration; }
	}

	float ParsePercent(const FString& V)
	{
		FString S = V;
		const bool bPct = S.RemoveFromEnd(TEXT("%"));
		const float F = FCString::Atof(*S);
		return bPct ? F / 100.f : F;
	}

	void ParseWeapon(const FGenIniBlock& B, FGenWeaponTemplate& W)
	{
		for (const FGenIniEntry& E : B.Entries)
		{
			auto Is = [&](const TCHAR* N) { return E.Key.Equals(N, ESearchCase::IgnoreCase); };
			if (Is(TEXT("PrimaryDamage"))) { W.PrimaryDamage = E.AsFloat(); }
			else if (Is(TEXT("PrimaryDamageRadius"))) { W.PrimaryDamageRadius = E.AsFloat(); }
			else if (Is(TEXT("SecondaryDamage"))) { W.SecondaryDamage = E.AsFloat(); }
			else if (Is(TEXT("SecondaryDamageRadius"))) { W.SecondaryDamageRadius = E.AsFloat(); }
			else if (Is(TEXT("AttackRange"))) { W.AttackRange = E.AsFloat(); }
			else if (Is(TEXT("MinimumAttackRange"))) { W.MinimumAttackRange = E.AsFloat(); }
			else if (Is(TEXT("AcceptableAimDelta"))) { W.AcceptableAimDelta = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("ScatterRadius"))) { W.ScatterRadius = E.AsFloat(); }
			else if (Is(TEXT("ScatterRadiusVsInfantry"))) { W.ScatterRadiusVsInfantry = E.AsFloat(); }
			else if (Is(TEXT("DamageType"))) { W.DamageType = E.Value().ToUpper(); }
			else if (Is(TEXT("DeathType"))) { W.DeathType = E.Value().ToUpper(); }
			else if (Is(TEXT("WeaponSpeed"))) { W.WeaponSpeed = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("WeaponRecoil"))) { W.WeaponRecoil = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("ProjectileObject"))) { W.ProjectileObject = E.Value().Equals(TEXT("NONE"), ESearchCase::IgnoreCase) ? FString() : E.Value(); }
			else if (Is(TEXT("FireFX"))) { W.FireFX = E.Value(); }
			else if (Is(TEXT("ProjectileDetonationFX"))) { W.ProjectileDetonationFX = E.Value(); }
			else if (Is(TEXT("FireOCL"))) { W.FireOCL = E.Value(); }
			else if (Is(TEXT("ProjectileDetonationOCL"))) { W.ProjectileDetonationOCL = E.Value(); }
			else if (Is(TEXT("ProjectileExhaust"))) { W.ProjectileExhaust = E.Value(); }
			else if (Is(TEXT("FireSound"))) { W.FireSound = E.Value(); }
			else if (Is(TEXT("ClipSize"))) { W.ClipSize = E.AsInt(); }
			else if (Is(TEXT("ClipReloadTime"))) { W.ClipReloadTime = GenLogic::MsecToFrames(E.AsFloat()); }
			else if (Is(TEXT("PreAttackDelay"))) { W.PreAttackDelay = GenLogic::MsecToFrames(E.AsFloat()); }
			else if (Is(TEXT("DelayBetweenShots")))
			{
				// WeaponTemplate::parseShotDelay: "2000" or "Min:100 Max:200"
				TArray<FString> T;
				GenIni::Tokenize(E.Raw, T, TEXT(" \t=:"));
				if (T.Num() >= 4 && T[0].Equals(TEXT("Min"), ESearchCase::IgnoreCase))
				{
					W.DelayBetweenShotsMin = GenLogic::MsecToFrames(FCString::Atof(*T[1]));
					W.DelayBetweenShotsMax = GenLogic::MsecToFrames(FCString::Atof(*T[3]));
				}
				else
				{
					W.DelayBetweenShotsMin = W.DelayBetweenShotsMax = GenLogic::MsecToFrames(E.AsFloat());
				}
			}
			else if (Is(TEXT("RadiusDamageAffects")))
			{
				W.RadiusDamageAffects = 0;
				for (const FString& V : E.Values)
				{
					if (V.Equals(TEXT("SELF"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::Self; }
					else if (V.Equals(TEXT("ALLIES"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::Allies; }
					else if (V.Equals(TEXT("ENEMIES"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::Enemies; }
					else if (V.Equals(TEXT("NEUTRALS"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::Neutrals; }
					else if (V.Equals(TEXT("SUICIDE"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::KillsSelf; }
					else if (V.Equals(TEXT("NOT_SIMILAR"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::NotSimilar; }
					else if (V.Equals(TEXT("NOT_AIRBORNE"), ESearchCase::IgnoreCase)) { W.RadiusDamageAffects |= GenWeaponAffects::NotAirborne; }
				}
			}
			else if (Is(TEXT("AntiGround"))) { W.bAntiGround = E.AsBool(); }
			else if (Is(TEXT("AntiAirborneVehicle"))) { W.bAntiAirborneVehicle = E.AsBool(); }
			else if (Is(TEXT("AntiAirborneInfantry"))) { W.bAntiAirborneInfantry = E.AsBool(); }
			else if (Is(TEXT("DamageDealtAtSelfPosition"))) { W.bDamageDealtAtSelfPosition = E.AsBool(); }
		}
	}

	uint32 ParseWeaponSlots(const TArray<FString>& Values)
	{
		uint32 Slots = 0;
		for (const FString& V : Values)
		{
			if (V.Equals(TEXT("PRIMARY"), ESearchCase::IgnoreCase)) { Slots |= 1; }
			else if (V.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase)) { Slots |= 2; }
			else if (V.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase)) { Slots |= 4; }
		}
		return Slots;
	}

	void ParseTurret(const FGenIniBlock& B, FGenTurretData& T)
	{
		T.bValid = true;
		T.TurnRate = GenLogic::AngularVelocityPerFrame(B.GetFloat(TEXT("TurretTurnRate"), 0.f));
		T.NaturalAngle = FMath::DegreesToRadians(B.GetFloat(TEXT("NaturalTurretAngle"), 0.f));
		T.RecenterTime = GenLogic::MsecToFrames(B.GetFloat(TEXT("RecenterTime"), 0.f));
		if (const FGenIniEntry* E = B.Find(TEXT("ControlledWeaponSlots")))
		{
			T.ControlledSlots = ParseWeaponSlots(E->Values);
		}
	}

	bool IsAIModule(const FString& Type)
	{
		return Type.EndsWith(TEXT("AIUpdate"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("AIUpdateInterface"), ESearchCase::IgnoreCase);
	}

	bool IsBodyModule(const FString& Type)
	{
		return Type.EndsWith(TEXT("Body"), ESearchCase::IgnoreCase);
	}
	bool IsModuleKey(const FString& Key)
	{
		return Key.Equals(TEXT("Draw"), ESearchCase::IgnoreCase) || Key.Equals(TEXT("Behavior"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("Body"), ESearchCase::IgnoreCase) || Key.Equals(TEXT("ClientUpdate"), ESearchCase::IgnoreCase);
	}

	FString ModuleTag(const FGenIniEntry& E)
	{
		return E.Values.Num() > 1 ? E.Values[1] : FString();
	}

	/** W3DModelDrawModuleData::parseConditionState field parse (S may start as a copy of the default state). */
	int32 WeaponSlotIndex(const FString& SlotName)
	{
		return SlotName.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 1 : (SlotName.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 2 : 0);
	}

	void ParseConditionStateBlock(const FGenIniBlock& B, FGenModelConditionState& S, bool bCopiedFromDefault)
	{
		bool bAnimsReplaced = false;
		bool bFlashReplaced = false;
		bool bRecoilReplaced = false;
		bool bParticlesReplaced = false;
		for (const FGenIniEntry& E : B.Entries)
		{
			const FString& K = E.Key;
			auto Is = [&](const TCHAR* N) { return K.Equals(N, ESearchCase::IgnoreCase); };
			if (Is(TEXT("Model")))
			{
				S.Model = E.Value(0).Equals(TEXT("NONE"), ESearchCase::IgnoreCase) ? FString() : E.Value(0);
			}
			else if (Is(TEXT("Animation")) || Is(TEXT("IdleAnimation")))
			{
				// Animations copied from the default state are discarded once a state lists its own.
				if (bCopiedFromDefault && !bAnimsReplaced) { S.Animations.Reset(); }
				bAnimsReplaced = true;
				S.bIdleAnimations = Is(TEXT("IdleAnimation"));
				if (E.Values.Num() > 0 && !E.Values[0].Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { S.Animations.Add(E.Values[0]); }
			}
			else if (Is(TEXT("AnimationMode"))) { S.AnimationMode = E.Value(0); }
			else if (Is(TEXT("AnimationSpeedFactorRange")) && E.Values.Num() >= 2)
			{
				S.AnimSpeedMin = E.AsFloat(0, 1.f);
				S.AnimSpeedMax = E.AsFloat(1, 1.f);
			}
			else if (Is(TEXT("HideSubObject"))) { for (const FString& V : E.Values) { S.HideShow.Add({ V, true }); } }
			else if (Is(TEXT("ShowSubObject"))) { for (const FString& V : E.Values) { S.HideShow.Add({ V, false }); } }
			else if (Is(TEXT("Turret"))) { S.Turret = E.Value(0); }
			else if (Is(TEXT("TurretPitch"))) { S.TurretPitch = E.Value(0); }
			else if (Is(TEXT("WeaponMuzzleFlash")) && E.Values.Num() > 1)
			{
				if (bCopiedFromDefault && !bFlashReplaced) { S.MuzzleFlashBones.Reset(); S.MuzzleFlashSlots.Reset(); }
				bFlashReplaced = true;
				if (!S.MuzzleFlashBones.Contains(E.Values[1]))
				{
					S.MuzzleFlashBones.Add(E.Values[1]);
					S.MuzzleFlashSlots.Add(WeaponSlotIndex(E.Values[0]));
				}
			}
			else if (Is(TEXT("ParticleSysBone")) && E.Values.Num() > 1)
			{
				// A state that lists its own particle bones doesn't inherit the default state's.
				if (bCopiedFromDefault && !bParticlesReplaced) { S.ParticleBones.Reset(); }
				bParticlesReplaced = true;
				if (!E.Values[1].Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { S.ParticleBones.Add({ E.Values[0], E.Values[1] }); }
			}
			else if (Is(TEXT("WeaponFireFXBone")) && E.Values.Num() > 1) { S.FireFXBone[WeaponSlotIndex(E.Values[0])] = E.Values[1]; }
			else if (Is(TEXT("WeaponLaunchBone")) && E.Values.Num() > 1) { S.LaunchBone[WeaponSlotIndex(E.Values[0])] = E.Values[1]; }
			else if (Is(TEXT("WeaponRecoilBone")) && E.Values.Num() > 1)
			{
				if (bCopiedFromDefault && !bRecoilReplaced) { S.RecoilBones.Reset(); S.RecoilSlots.Reset(); }
				bRecoilReplaced = true;
				if (!S.RecoilBones.Contains(E.Values[1]))
				{
					S.RecoilBones.Add(E.Values[1]);
					S.RecoilSlots.Add(WeaponSlotIndex(E.Values[0]));
				}
			}
		}
	}
}

// ---- structs ------------------------------------------------------------------------------------

const FGenModelConditionState* FGenDrawModule::DefaultState() const
{
	if (States.IsValidIndex(DefaultStateIndex))
	{
		return &States[DefaultStateIndex];
	}
	return States.Num() > 0 ? &States[0] : nullptr;
}

int32 FGenDrawModule::FindBestState(const FGenConditionBits& InFlags) const
{
	const FGenConditionBits Flags = InFlags.Without(IgnoreConditions);
	int32 Best = INDEX_NONE;
	int32 BestYes = 0;
	int32 BestExtra = 999;
	for (int32 i = 0; i < States.Num(); ++i)
	{
		for (int32 c = States[i].ConditionSets.Num() - 1; c >= 0; --c)
		{
			const FGenConditionBits& Yes = States[i].ConditionSets[c];
			const int32 YesMatch = Flags.CountIntersection(Yes);
			const int32 Extra = Flags.CountMissing(Yes);
			if (YesMatch > BestYes || (YesMatch >= BestYes && Extra < BestExtra))
			{
				Best = i;
				BestYes = YesMatch;
				BestExtra = Extra;
			}
		}
	}
	return Best;
}

FString FGenDrawModule::DefaultModel() const
{
	if (const FGenModelConditionState* S = DefaultState())
	{
		if (!S->Model.IsEmpty())
		{
			return S->Model;
		}
	}
	if (Block)
	{
		// W3DTreeDraw / W3DPropDraw style
		FString Name = Block->GetString(TEXT("ModelName"));
		if (!Name.IsEmpty())
		{
			return Name;
		}
	}
	return FString();
}

float FGenThingTemplate::GetBoundingCircleRadius() const
{
	if (IsBox())
	{
		return FMath::Sqrt(GeometryMajorRadius * GeometryMajorRadius + GeometryMinorRadius * GeometryMinorRadius);
	}
	return GeometryMajorRadius;
}

float FGenThingTemplate::GetBoundingSphereRadius() const
{
	const float Circle = GetBoundingCircleRadius();
	const float HalfH = GeometryHeight * 0.5f;
	return GeometryType.Equals(TEXT("SPHERE"), ESearchCase::IgnoreCase) ? GeometryMajorRadius : FMath::Sqrt(Circle * Circle + HalfH * HalfH);
}

const FGenDrawModule* FGenThingTemplate::PrimaryDraw() const
{
	for (const FGenDrawModule& D : Draws)
	{
		if (!D.DefaultModel().IsEmpty())
		{
			return &D;
		}
	}
	return Draws.Num() > 0 ? &Draws[0] : nullptr;
}

// ---- registry -----------------------------------------------------------------------------------

FGenGameData& FGenGameData::Get()
{
	static FGenGameData Instance;
	return Instance;
}

bool FGenGameData::Initialize()
{
	if (bInitialized)
	{
		return true;
	}
	FGenFileSystem& FS = FGenFileSystem::Get();
	if (!FS.Initialize())
	{
		return false;
	}

	const double Start = FPlatformTime::Seconds();

	// Terrain types
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\Terrain.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\Terrain.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (B->Keyword.Equals(TEXT("Terrain"), ESearchCase::IgnoreCase))
			{
				FGenTerrainType& T = TerrainTypes.FindOrAdd(B->Name().ToLower());
				T.Name = B->Name();
				T.Texture = B->GetString(TEXT("Texture"), T.Texture);
				T.Class = B->GetString(TEXT("Class"), T.Class);
			}
		}
	}

	// GameData: gravity
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\GameData.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\GameData.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (B->Keyword.Equals(TEXT("GameData"), ESearchCase::IgnoreCase))
			{
				if (const FGenIniEntry* E = B->Find(TEXT("Gravity")))
				{
					Gravity = GenLogic::AccelPerFrame(E->AsFloat());
				}
			}
		}
	}

	// Damage thresholds (GameData)
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\GameData.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			UnitDamagedThreshold = B->GetFloat(TEXT("UnitDamagedThreshold"), UnitDamagedThreshold);
			UnitReallyDamagedThreshold = B->GetFloat(TEXT("UnitReallyDamagedThreshold"), UnitReallyDamagedThreshold);
		}
	}

	// Weapons
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\Weapon.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\Weapon.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (B->Keyword.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase))
			{
				FGenWeaponTemplate& W = Weapons.FindOrAdd(B->Name().ToLower());
				W.Name = B->Name();
				ParseWeapon(*B, W);
			}
		}
	}

	// Armor
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\Armor.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\Armor.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("Armor"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FGenArmorTemplate& A = Armors.FindOrAdd(B->Name().ToLower());
			A = FGenArmorTemplate();
			A.Name = B->Name();
			for (const FGenIniEntry& E : B->Entries)
			{
				if (E.Key.Equals(TEXT("Armor"), ESearchCase::IgnoreCase) && E.Values.Num() >= 2)
				{
					const float Pct = ParsePercent(E.Values[1]);
					if (E.Values[0].Equals(TEXT("DEFAULT"), ESearchCase::IgnoreCase))
					{
						// "Default" resets every damage type (ArmorTemplate::parseArmorCoefficients).
						A.DefaultCoefficient = Pct;
						A.Coefficients.Reset();
					}
					else
					{
						A.Coefficients.Add(E.Values[0].ToUpper(), Pct);
					}
				}
			}
		}
	}

	// Locomotors
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\Locomotor.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\Locomotor.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (B->Keyword.Equals(TEXT("Locomotor"), ESearchCase::IgnoreCase))
			{
				FGenLocomotorTemplate& L = Locomotors.FindOrAdd(B->Name().ToLower());
				L.Name = B->Name();
				ParseLocomotor(*B, L);
			}
		}
	}

	// Player templates (factions)
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\PlayerTemplate.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\PlayerTemplate.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("PlayerTemplate"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FGenPlayerTemplate P;
			P.Name = B->Name();
			P.Side = B->GetString(TEXT("Side"));
			P.BaseSide = B->GetString(TEXT("BaseSide"));
			P.bPlayable = B->GetBool(TEXT("PlayableSide"));
			P.bObserver = B->GetBool(TEXT("IsObserver"));
			P.StartingBuilding = B->GetString(TEXT("StartingBuilding"));
			P.StartMoney = B->GetInt(TEXT("StartMoney"));
			if (const FGenIniEntry* C = B->Find(TEXT("PreferredColor")))
			{
				P.PreferredColor = ParseIniColor(C->Raw);
			}
			for (int32 i = 0; i < 10; ++i)
			{
				const FString Unit = B->GetString(*FString::Printf(TEXT("StartingUnit%d"), i));
				if (!Unit.IsEmpty()) { P.StartingUnits.Add(Unit); }
			}
			PlayerTemplates.Add(MoveTemp(P));
		}
	}

	// Objects: INI::loadDirectory("Data\INI\Object") - files in the directory, then subdirectories.
	TArray<FString> ObjectFiles = FS.FindFiles(TEXT("Data\\INI\\Object\\"), TEXT(".ini"));
	ObjectFiles.StableSort([](const FString& A, const FString& B)
	{
		const int32 DA = A.Len() - A.Replace(TEXT("\\"), TEXT("")).Len();
		const int32 DB = B.Len() - B.Replace(TEXT("\\"), TEXT("")).Len();
		return DA < DB;
	});
	for (const FString& Path : ObjectFiles)
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(Path, Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (B->Keyword.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
			{
				AddObjectBlock(B, false);
			}
			else if (B->Keyword.Equals(TEXT("ObjectReskin"), ESearchCase::IgnoreCase))
			{
				AddObjectBlock(B, true);
			}
		}
	}

	for (const TPair<FString, TSharedPtr<FGenIniBlock>>& It : ObjectBlocks)
	{
		Templates.Add(It.Key, BuildTemplate(It.Value->Name(), It.Value));
	}

	bInitialized = true;
	UE_LOG(LogGenerals, Log, TEXT("Game data: %d object templates, %d terrain types (%.2fs)"),
		Templates.Num(), TerrainTypes.Num(), FPlatformTime::Seconds() - Start);
	return true;
}

void FGenGameData::AddObjectBlock(const TSharedPtr<FGenIniBlock>& Block, bool bReskin)
{
	const FString Key = Block->Name().ToLower();
	if (bReskin)
	{
		// ObjectReskin NewName BaseName
		const FString BaseKey = Block->Args.Num() > 1 ? Block->Args[1].ToLower() : FString();
		const TSharedPtr<FGenIniBlock>* Base = ObjectBlocks.Find(BaseKey);
		if (!Base)
		{
			UE_LOG(LogGenerals, Warning, TEXT("ObjectReskin %s: base '%s' not defined yet"), *Block->Name(), *BaseKey);
			return;
		}
		TSharedPtr<FGenIniBlock> Merged = MergeBlocks(Base->Get(), *Block);
		Merged->Keyword = TEXT("Object");
		Merged->Args = { Block->Name() };
		ObjectBlocks.Add(Key, Merged);
		return;
	}
	if (const TSharedPtr<FGenIniBlock>* Existing = ObjectBlocks.Find(Key))
	{
		// Redefinition (e.g. map.ini) overrides the existing template.
		ObjectBlocks.Add(Key, MergeBlocks(Existing->Get(), *Block));
		return;
	}
	ObjectBlocks.Add(Key, Block);
}

TSharedPtr<FGenIniBlock> FGenGameData::MergeBlocks(const FGenIniBlock* Base, const FGenIniBlock& Override)
{
	TSharedPtr<FGenIniBlock> Out = MakeShared<FGenIniBlock>(*Base);
	TSet<FString> ReplacedKeys;
	for (const FGenIniEntry& E : Override.Entries)
	{
		if (IsModuleKey(E.Key))
		{
			// Modules replace the base module with the same tag, otherwise are added.
			const FString Tag = ModuleTag(E);
			const int32 Existing = Out->Entries.IndexOfByPredicate([&](const FGenIniEntry& X)
			{
				return IsModuleKey(X.Key) && ModuleTag(X).Equals(Tag, ESearchCase::IgnoreCase);
			});
			if (Existing != INDEX_NONE)
			{
				Out->Entries[Existing] = E;
			}
			else
			{
				Out->Entries.Add(E);
			}
			continue;
		}
		// Plain fields: the override's occurrences replace all base occurrences of that key.
		const FString K = E.Key.ToLower();
		if (!ReplacedKeys.Contains(K))
		{
			ReplacedKeys.Add(K);
			Out->Entries.RemoveAll([&](const FGenIniEntry& X) { return X.Key.Equals(E.Key, ESearchCase::IgnoreCase); });
		}
		Out->Entries.Add(E);
	}
	return Out;
}

TSharedPtr<FGenThingTemplate> FGenGameData::BuildTemplate(const FString& Name, const TSharedPtr<FGenIniBlock>& Merged) const
{
	TSharedPtr<FGenThingTemplate> T = MakeShared<FGenThingTemplate>();
	T->Name = Name;
	T->Block = Merged;
	for (const FGenIniEntry& E : Merged->Entries)
	{
		if (E.Key.Equals(TEXT("KindOf"), ESearchCase::IgnoreCase))
		{
			for (const FString& V : E.Values) { T->KindOf.Add(V.ToUpper()); }
		}
		else if (E.Key.Equals(TEXT("Side"), ESearchCase::IgnoreCase)) { T->Side = E.Value(0); }
		else if (E.Key.Equals(TEXT("EditorSorting"), ESearchCase::IgnoreCase)) { T->EditorSorting = E.Value(0); }
		else if (E.Key.Equals(TEXT("Geometry"), ESearchCase::IgnoreCase)) { T->GeometryType = E.Value(0); }
		else if (E.Key.Equals(TEXT("GeometryMajorRadius"), ESearchCase::IgnoreCase)) { T->GeometryMajorRadius = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("GeometryMinorRadius"), ESearchCase::IgnoreCase)) { T->GeometryMinorRadius = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("GeometryHeight"), ESearchCase::IgnoreCase)) { T->GeometryHeight = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("BuildCost"), ESearchCase::IgnoreCase)) { T->BuildCost = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("BuildTime"), ESearchCase::IgnoreCase)) { T->BuildTime = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("GeometryIsSmall"), ESearchCase::IgnoreCase)) { T->bGeometryIsSmall = E.AsBool(); }
		else if (E.Key.Equals(TEXT("CrusherLevel"), ESearchCase::IgnoreCase)) { T->CrusherLevel = E.AsInt(); }
		else if (E.Key.Equals(TEXT("CrushableLevel"), ESearchCase::IgnoreCase)) { T->CrushableLevel = E.AsInt(); }
		else if (E.Key.Equals(TEXT("VisionRange"), ESearchCase::IgnoreCase)) { T->VisionRange = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("ShroudClearingRange"), ESearchCase::IgnoreCase)) { T->ShroudClearingRange = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("PlacementViewAngle"), ESearchCase::IgnoreCase)) { T->PlacementViewAngle = FMath::DegreesToRadians(E.AsFloat()); }
		else if (E.Key.Equals(TEXT("Locomotor"), ESearchCase::IgnoreCase) && E.Values.Num() >= 2)
		{
			// "Locomotor = SET_NORMAL Name [Name...]" (a set may be redefined; last wins)
			TArray<FString>& Set = T->LocomotorSets.FindOrAdd(E.Values[0].ToUpper());
			Set.Reset();
			for (int32 i = 1; i < E.Values.Num(); ++i)
			{
				if (!E.Values[i].Equals(TEXT("None"), ESearchCase::IgnoreCase)) { Set.Add(E.Values[i]); }
			}
		}
		else if ((E.Key.Equals(TEXT("Body"), ESearchCase::IgnoreCase) || E.Key.Equals(TEXT("Behavior"), ESearchCase::IgnoreCase)) && E.Block)
		{
			const FString& Type = E.Value(0);
			if (IsBodyModule(Type))
			{
				T->BodyType = Type;
				T->MaxHealth = E.Block->GetFloat(TEXT("MaxHealth"), T->MaxHealth);
				T->InitialHealth = E.Block->GetFloat(TEXT("InitialHealth"), T->MaxHealth);
			}
			else if (Type.Equals(TEXT("PhysicsBehavior"), ESearchCase::IgnoreCase))
			{
				T->bHasPhysics = true;
				T->Mass = E.Block->GetFloat(TEXT("Mass"), 1.f);
				if (const FGenIniEntry* F = E.Block->Find(TEXT("ForwardFriction"))) { T->ForwardFriction = GenLogic::FrictionPerFrame(F->AsFloat()); }
				if (const FGenIniEntry* F = E.Block->Find(TEXT("LateralFriction"))) { T->LateralFriction = GenLogic::FrictionPerFrame(F->AsFloat()); }
				if (const FGenIniEntry* F = E.Block->Find(TEXT("AerodynamicFriction"))) { T->AerodynamicFriction = GenLogic::FrictionPerFrame(F->AsFloat()); }
				T->PitchRollYawFactor = E.Block->GetFloat(TEXT("PitchRollYawFactor"), 2.f);
				T->bAllowBouncing = E.Block->GetBool(TEXT("AllowBouncing"));
				T->bKillWhenRestingOnGround = E.Block->GetBool(TEXT("KillWhenRestingOnGround"));
			}
			else if (Type.Equals(TEXT("SquishCollide"), ESearchCase::IgnoreCase))
			{
				T->bSquishable = true;
			}
			else if (Type.Equals(TEXT("TransitionDamageFX"), ESearchCase::IgnoreCase))
			{
				// "ReallyDamagedParticleSystem1 = Bone:Smoke RandomBone:Yes PSys:Name"
				// "DamagedFXList1 = Loc: X:0 Y:0 Z:0 FXList:Name", "RubbleOCL1 = OCL:Name"
				for (const FGenIniEntry& TE : E.Block->Entries)
				{
					FGenTransitionFX X;
					FString Rest;
					if (TE.Key.StartsWith(TEXT("ReallyDamaged"), ESearchCase::IgnoreCase)) { X.State = 2; Rest = TE.Key.Mid(13); }
					else if (TE.Key.StartsWith(TEXT("Damaged"), ESearchCase::IgnoreCase)) { X.State = 1; Rest = TE.Key.Mid(7); }
					else if (TE.Key.StartsWith(TEXT("Rubble"), ESearchCase::IgnoreCase)) { X.State = 3; Rest = TE.Key.Mid(6); }
					else { continue; }
					if (Rest.StartsWith(TEXT("FXList"), ESearchCase::IgnoreCase)) { X.Kind = FGenTransitionFX::FXList; }
					else if (Rest.StartsWith(TEXT("OCL"), ESearchCase::IgnoreCase)) { X.Kind = FGenTransitionFX::OCL; }
					else if (Rest.StartsWith(TEXT("ParticleSystem"), ESearchCase::IgnoreCase)) { X.Kind = FGenTransitionFX::ParticleSystem; }
					else { continue; }
					TArray<FString> Tok;
					GenIni::Tokenize(TE.Raw, Tok, TEXT(" \t=:"));
					for (int32 i = 0; i + 1 < Tok.Num(); ++i)
					{
						const FString& K = Tok[i];
						const FString& V = Tok[i + 1];
						if (K.Equals(TEXT("X"), ESearchCase::IgnoreCase)) { X.Loc.X = FCString::Atof(*V); }
						else if (K.Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { X.Loc.Y = FCString::Atof(*V); }
						else if (K.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) { X.Loc.Z = FCString::Atof(*V); }
						else if (K.Equals(TEXT("Bone"), ESearchCase::IgnoreCase)) { X.Bone = V.Equals(TEXT("None"), ESearchCase::IgnoreCase) ? FString() : V; }
						else if (K.Equals(TEXT("RandomBone"), ESearchCase::IgnoreCase)) { X.bRandomBone = V.Equals(TEXT("Yes"), ESearchCase::IgnoreCase); }
						else if (K.Equals(TEXT("PSys"), ESearchCase::IgnoreCase) || K.Equals(TEXT("FXList"), ESearchCase::IgnoreCase)
							|| K.Equals(TEXT("OCL"), ESearchCase::IgnoreCase)) { X.Name = V; }
					}
					if (!X.Name.IsEmpty())
					{
						T->TransitionFX.Add(MoveTemp(X));
					}
				}
			}
			else if (Type.Equals(TEXT("LifetimeUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHasLifetime = true;
				T->MinLifetime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MinLifetime"), 0.f));
				T->MaxLifetime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MaxLifetime"), 0.f));
			}
			else if (Type.Equals(TEXT("FXListDie"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("CreateObjectDie"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("InstantDeathBehavior"), ESearchCase::IgnoreCase))
			{
				FGenDieModule& M = T->DieModules.AddDefaulted_GetRef();
				M.Type = Type.Equals(TEXT("FXListDie"), ESearchCase::IgnoreCase) ? EGenDieModule::FXList
					: (Type.Equals(TEXT("CreateObjectDie"), ESearchCase::IgnoreCase) ? EGenDieModule::CreateObject : EGenDieModule::Instant);
				for (const FGenIniEntry& DE : E.Block->Entries)
				{
					if (DE.Key.Equals(TEXT("DeathTypes"), ESearchCase::IgnoreCase)) { M.DeathTypes = DE.Values; }
					else if (DE.Key.Equals(TEXT("DeathFX"), ESearchCase::IgnoreCase) || DE.Key.Equals(TEXT("FX"), ESearchCase::IgnoreCase)) { M.FX.Append(DE.Values); }
					else if (DE.Key.Equals(TEXT("CreationList"), ESearchCase::IgnoreCase) || DE.Key.Equals(TEXT("OCL"), ESearchCase::IgnoreCase)) { M.OCL.Append(DE.Values); }
				}
			}
			else if (Type.Equals(TEXT("SlowDeathBehavior"), ESearchCase::IgnoreCase) || Type.EndsWith(TEXT("SlowDeathBehavior"), ESearchCase::IgnoreCase))
			{
				FGenSlowDeath D;
				D.DestructionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DestructionDelay"), 0.f));
				D.DestructionDelayVariance = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DestructionDelayVariance"), 0.f));
				D.SinkDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("SinkDelay"), 0.f));
				D.SinkRate = GenLogic::VelocityPerFrame(E.Block->GetFloat(TEXT("SinkRate"), 0.f));
				D.ProbabilityModifier = E.Block->GetInt(TEXT("ProbabilityModifier"), 10);
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { D.DeathTypes = DT->Values; }
				for (const FGenIniEntry& DE : E.Block->Entries)
				{
					// "FX = FINAL FX_Name" / "OCL = INITIAL OCL_Name"
					const bool bFX = DE.Key.Equals(TEXT("FX"), ESearchCase::IgnoreCase);
					if ((bFX || DE.Key.Equals(TEXT("OCL"), ESearchCase::IgnoreCase)) && DE.Values.Num() >= 2)
					{
						const int32 Phase = DE.Values[0].Equals(TEXT("MIDPOINT"), ESearchCase::IgnoreCase) ? FGenSlowDeath::Midpoint
							: (DE.Values[0].Equals(TEXT("FINAL"), ESearchCase::IgnoreCase) ? FGenSlowDeath::Final : FGenSlowDeath::Initial);
						(bFX ? D.FX[Phase] : D.OCL[Phase]).Add(DE.Values[1]);
					}
				}
				T->SlowDeaths.Add(MoveTemp(D));
			}
			else if (Type.Equals(TEXT("DestroyDie"), ESearchCase::IgnoreCase))
			{
				FGenDieModule& M = T->DieModules.AddDefaulted_GetRef();
				M.Type = EGenDieModule::Destroy;
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { M.DeathTypes = DT->Values; }
			}
			else if (Type.Equals(TEXT("DumbProjectileBehavior"), ESearchCase::IgnoreCase))
			{
				FGenDumbProjectile& P = T->DumbProjectile;
				P.bValid = true;
				P.FirstHeight = E.Block->GetFloat(TEXT("FirstHeight"), 0.f);
				P.SecondHeight = E.Block->GetFloat(TEXT("SecondHeight"), 0.f);
				if (const FGenIniEntry* F = E.Block->Find(TEXT("FirstPercentIndent"))) { P.FirstPercentIndent = ParsePercent(F->Value()); }
				if (const FGenIniEntry* F = E.Block->Find(TEXT("SecondPercentIndent"))) { P.SecondPercentIndent = ParsePercent(F->Value()); }
			}
			else if (IsAIModule(Type))
			{
				T->bHasAI = true;
				T->AIUpdateType = Type;
				T->bIsMissile = Type.Equals(TEXT("MissileAIUpdate"), ESearchCase::IgnoreCase);
				T->bAutoAcquireEnemiesWhenIdle = E.Block->GetBool(TEXT("AutoAcquireEnemiesWhenIdle"), T->bAutoAcquireEnemiesWhenIdle);
				for (const FGenIniEntry& Sub : E.Block->Entries)
				{
					if (Sub.Block && Sub.Key.Equals(TEXT("Turret"), ESearchCase::IgnoreCase)) { ParseTurret(*Sub.Block, T->Turrets[0]); }
					else if (Sub.Block && Sub.Key.Equals(TEXT("AltTurret"), ESearchCase::IgnoreCase)) { ParseTurret(*Sub.Block, T->Turrets[1]); }
				}
			}
		}
		else if (E.Key.Equals(TEXT("WeaponSet"), ESearchCase::IgnoreCase) && E.Block)
		{
			FGenWeaponSetEntry& WS = T->WeaponSets.AddDefaulted_GetRef();
			for (const FGenIniEntry& WE : E.Block->Entries)
			{
				if (WE.Key.Equals(TEXT("Conditions"), ESearchCase::IgnoreCase))
				{
					for (const FString& C : WE.Values) { if (!C.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { WS.Conditions.Add(C.ToUpper()); } }
				}
				else if (WE.Key.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase) && WE.Values.Num() >= 2)
				{
					const uint32 Slot = ParseWeaponSlots({ WE.Values[0] });
					const int32 Index = Slot == 1 ? 0 : (Slot == 2 ? 1 : (Slot == 4 ? 2 : -1));
					if (Index >= 0)
					{
						WS.Weapons[Index] = WE.Values[1].Equals(TEXT("None"), ESearchCase::IgnoreCase) ? FString() : WE.Values[1];
					}
				}
			}
		}
		else if (E.Key.Equals(TEXT("ArmorSet"), ESearchCase::IgnoreCase) && E.Block)
		{
			FGenArmorSetEntry& AS = T->ArmorSets.AddDefaulted_GetRef();
			for (const FGenIniEntry& AE : E.Block->Entries)
			{
				if (AE.Key.Equals(TEXT("Conditions"), ESearchCase::IgnoreCase))
				{
					for (const FString& C : AE.Values) { if (!C.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { AS.Conditions.Add(C.ToUpper()); } }
				}
				else if (AE.Key.Equals(TEXT("Armor"), ESearchCase::IgnoreCase))
				{
					AS.Armor = AE.Value();
				}
			}
		}
		else if (E.Key.Equals(TEXT("Draw"), ESearchCase::IgnoreCase) && E.Block)
		{
			FGenDrawModule& D = T->Draws.AddDefaulted_GetRef();
			D.Type = E.Value(0);
			D.Tag = E.Value(1);
			D.Block = E.Block;
			D.bOkToChangeModelColor = E.Block->GetBool(TEXT("OkToChangeModelColor"));
			for (const FGenIniEntry& DE : E.Block->Entries)
			{
				const bool bDefault = DE.Key.Equals(TEXT("DefaultConditionState"), ESearchCase::IgnoreCase);
				const bool bState = DE.Key.Equals(TEXT("ConditionState"), ESearchCase::IgnoreCase);
				const bool bAlias = DE.Key.Equals(TEXT("AliasConditionState"), ESearchCase::IgnoreCase);
				if (DE.Key.Equals(TEXT("IgnoreConditionStates"), ESearchCase::IgnoreCase))
				{
					D.IgnoreConditions = GenConditions::Parse(DE.Values);
				}
				else if (bAlias)
				{
					// Adds another set of condition bits to the previous state.
					if (D.States.Num() > 0) { D.States.Last().ConditionSets.Add(GenConditions::Parse(DE.Values)); }
				}
				else if ((bDefault || bState) && DE.Block)
				{
					FGenModelConditionState S;
					const bool bCopy = bState && D.States.IsValidIndex(D.DefaultStateIndex);
					if (bCopy)
					{
						S = D.States[D.DefaultStateIndex];
						S.bIsDefault = false;
						S.ConditionSets.Reset();
					}
					S.ConditionSets.Add(bState ? GenConditions::Parse(DE.Values) : FGenConditionBits());
					S.Description = bDefault ? FString(TEXT("DEFAULT")) : FString::Join(DE.Values, TEXT(" "));
					ParseConditionStateBlock(*DE.Block, S, bCopy);
					if (bDefault)
					{
						S.bIsDefault = true;
						D.DefaultStateIndex = D.States.Num();
					}
					D.States.Add(MoveTemp(S));
				}
			}
		}
	}
	return T;
}

const FGenThingTemplate* FGenGameData::FindTemplate(const FString& Name) const
{
	const TSharedPtr<FGenThingTemplate>* Found = Templates.Find(Name.ToLower());
	return Found ? Found->Get() : nullptr;
}

const FGenTerrainType* FGenGameData::FindTerrain(const FString& Name) const
{
	return TerrainTypes.Find(Name.ToLower());
}

const FGenLocomotorTemplate* FGenGameData::FindLocomotor(const FString& Name) const
{
	return Locomotors.Find(Name.ToLower());
}

const FGenWeaponTemplate* FGenGameData::FindWeapon(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Weapons.Find(Name.ToLower());
}

const FGenArmorTemplate* FGenGameData::FindArmor(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Armors.Find(Name.ToLower());
}

float FGenArmorTemplate::AdjustDamage(const FString& DamageType, float Damage) const
{
	if (DamageType == TEXT("UNRESISTABLE") || DamageType == TEXT("SUBDUAL_UNRESISTABLE"))
	{
		return Damage;
	}
	const float* C = Coefficients.Find(DamageType);
	return FMath::Max(0.f, Damage * (C ? *C : DefaultCoefficient));
}

const FGenWeaponSetEntry* FGenThingTemplate::FindWeaponSet() const
{
	// Until upgrades/veterancy exist, the set without conditions applies.
	for (const FGenWeaponSetEntry& WS : WeaponSets)
	{
		if (WS.Conditions.Num() == 0) { return &WS; }
	}
	return WeaponSets.Num() > 0 ? &WeaponSets[0] : nullptr;
}

const FGenArmorSetEntry* FGenThingTemplate::FindArmorSet() const
{
	for (const FGenArmorSetEntry& AS : ArmorSets)
	{
		if (AS.Conditions.Num() == 0) { return &AS; }
	}
	return ArmorSets.Num() > 0 ? &ArmorSets[0] : nullptr;
}

const FGenPlayerTemplate* FGenGameData::FindPlayerTemplate(const FString& Name) const
{
	return PlayerTemplates.FindByPredicate([&](const FGenPlayerTemplate& P) { return P.Name.Equals(Name, ESearchCase::IgnoreCase); });
}

void FGenGameData::ApplyMapIni(const FString& MapFolder)
{
	const FString Path = MapFolder / TEXT("map.ini");
	if (!FGenFileSystem::Get().Exists(Path))
	{
		return;
	}
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(Path, Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		const bool bObject = B->Keyword.Equals(TEXT("Object"), ESearchCase::IgnoreCase);
		const bool bReskin = B->Keyword.Equals(TEXT("ObjectReskin"), ESearchCase::IgnoreCase);
		if (bObject || bReskin)
		{
			AddObjectBlock(B, bReskin);
			const FString Key = B->Name().ToLower();
			if (const TSharedPtr<FGenIniBlock>* Merged = ObjectBlocks.Find(Key))
			{
				Templates.Add(Key, BuildTemplate(B->Name(), *Merged));
			}
		}
	}
}
