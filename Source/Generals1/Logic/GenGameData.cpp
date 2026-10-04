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
			else if (Is(TEXT("Lift"))) { L.Lift = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("LiftDamaged"))) { L.LiftDamaged = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("SpeedLimitZ"))) { L.SpeedLimitZ = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("PreferredHeightDamping"))) { L.PreferredHeightDamping = E.AsFloat(); }
			else if (Is(TEXT("Apply2DFrictionWhenAirborne"))) { L.bApply2DFrictionWhenAirborne = E.AsBool(); }
			else if (Is(TEXT("AllowAirborneMotiveForce"))) { L.bAllowAirborneMotiveForce = E.AsBool(); }
			else if (Is(TEXT("PitchStiffness"))) { L.PitchStiffness = E.AsFloat(); }
			else if (Is(TEXT("RollStiffness"))) { L.RollStiffness = E.AsFloat(); }
			else if (Is(TEXT("PitchDamping"))) { L.PitchDamping = E.AsFloat(); }
			else if (Is(TEXT("RollDamping"))) { L.RollDamping = E.AsFloat(); }
			else if (Is(TEXT("ForwardVelocityPitchFactor"))) { L.ForwardVelocityPitchFactor = E.AsFloat(); }
			else if (Is(TEXT("LateralVelocityRollFactor"))) { L.LateralVelocityRollFactor = E.AsFloat(); }
			else if (Is(TEXT("UniformAxialDamping"))) { L.UniformAxialDamping = E.AsFloat(); }
			else if (Is(TEXT("PitchInDirectionOfZVelFactor"))) { L.PitchInDirectionOfZVelFactor = E.AsFloat(); }
			else if (Is(TEXT("CirclingRadius"))) { L.CirclingRadius = E.AsFloat(); }
			else if (Is(TEXT("AirborneTargetingHeight"))) { L.AirborneTargetingHeight = E.AsInt(); }
		}
		// LocomotorTemplate::validate: damaged values default to the normal ones.
		if (L.SpeedDamaged < 0.f) { L.SpeedDamaged = L.Speed; }
		if (L.TurnRateDamaged < 0.f) { L.TurnRateDamaged = L.TurnRate; }
		if (L.AccelerationDamaged < 0.f) { L.AccelerationDamaged = L.Acceleration; }
		if (L.LiftDamaged < 0.f) { L.LiftDamaged = L.Lift; }
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
			else if (Is(TEXT("WeaponBonus")) && E.Values.Num() >= 3)
			{
				// "WeaponBonus = PLAYER_UPGRADE DAMAGE 125%"
				static const TCHAR* Fields[] = { TEXT("DAMAGE"), TEXT("RADIUS"), TEXT("RANGE"), TEXT("RATE_OF_FIRE"), TEXT("PRE_ATTACK") };
				for (int32 f = 0; f < UE_ARRAY_COUNT(Fields); ++f)
				{
					if (E.Values[1].Equals(Fields[f], ESearchCase::IgnoreCase))
					{
						FGenWeaponBonus& Bonus = W.Bonuses.AddDefaulted_GetRef();
						Bonus.Condition = E.Values[0].ToUpper();
						Bonus.Field = FGenWeaponBonus::EField(f);
						Bonus.Value = ParsePercent(E.Values[2]);
					}
				}
			}
			else if (Is(TEXT("AutoReloadsClip")))
			{
				W.ReloadType = E.Value().Equals(TEXT("RETURN_TO_BASE"), ESearchCase::IgnoreCase) ? EGenReloadType::ReturnToBase
					: (E.AsBool() ? EGenReloadType::Auto : EGenReloadType::None);
			}
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
			else if (Is(TEXT("Flags"))) { S.bStartFrameLast = E.Values.ContainsByPredicate([](const FString& V) { return V.Equals(TEXT("START_FRAME_LAST"), ESearchCase::IgnoreCase); }); }
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
	FGenUpgradeData::Get().Initialize();

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
			MaxTunnelCapacity = B->GetInt(TEXT("MaxTunnelCapacity"), MaxTunnelCapacity);
			// Fog of war
			PartitionCellSize = FMath::Max(1.f, B->GetFloat(TEXT("PartitionCellSize"), PartitionCellSize));
			if (const FGenIniEntry* Unlook = B->Find(TEXT("UnlookPersistDuration")))
			{
				UnlookPersistFrames = GenLogic::MsecToFrames(Unlook->AsFloat());
			}
			ClearAlpha = uint8(FMath::Clamp(B->GetInt(TEXT("ClearAlpha"), ClearAlpha), 0, 255));
			FogAlpha = uint8(FMath::Clamp(B->GetInt(TEXT("FogAlpha"), FogAlpha), 0, 255));
			ShroudAlpha = uint8(FMath::Clamp(B->GetInt(TEXT("ShroudAlpha"), ShroudAlpha), 0, 255));
			if (const FGenIniEntry* Color = B->Find(TEXT("ShroudColor")))
			{
				ShroudColor = ParseIniColor(Color->Raw);
			}
			for (const FGenIniEntry& E : B->Entries)
			{
				// "WeaponBonus = GARRISONED RANGE 133%"
				if (!E.Key.Equals(TEXT("WeaponBonus"), ESearchCase::IgnoreCase) || E.Values.Num() < 3)
				{
					continue;
				}
				static const TCHAR* Fields[] = { TEXT("DAMAGE"), TEXT("RADIUS"), TEXT("RANGE"), TEXT("RATE_OF_FIRE"), TEXT("PRE_ATTACK") };
				for (int32 f = 0; f < UE_ARRAY_COUNT(Fields); ++f)
				{
					if (E.Values[1].Equals(Fields[f], ESearchCase::IgnoreCase))
					{
						FGenWeaponBonus& Bonus = GlobalWeaponBonuses.AddDefaulted_GetRef();
						Bonus.Condition = E.Values[0].ToUpper();
						Bonus.Field = FGenWeaponBonus::EField(f);
						Bonus.Value = ParsePercent(E.Values[2]);
					}
				}
			}
			UnitReallyDamagedThreshold = B->GetFloat(TEXT("UnitReallyDamagedThreshold"), UnitReallyDamagedThreshold);
			auto Percent = [&B](const TCHAR* Key, float& Out)
			{
				if (const FGenIniEntry* E = B->Find(Key)) { Out = ParsePercent(E->Value()); }
			};
			Percent(TEXT("SellPercentage"), SellPercentage);
			Percent(TEXT("HealthBonus_Veteran"), HealthBonus[1]);
			Percent(TEXT("HealthBonus_Elite"), HealthBonus[2]);
			Percent(TEXT("HealthBonus_Heroic"), HealthBonus[3]);
			Percent(TEXT("LowEnergyPenaltyModifier"), LowEnergyPenaltyModifier);
			Percent(TEXT("MinLowEnergyProductionSpeed"), MinLowEnergyProductionSpeed);
			Percent(TEXT("MaxLowEnergyProductionSpeed"), MaxLowEnergyProductionSpeed);
			AllowedHeightVariationForBuilding = B->GetFloat(TEXT("AllowedHeightVariationForBuilding"), AllowedHeightVariationForBuilding);
			ValuePerSupplyBox = B->GetInt(TEXT("ValuePerSupplyBox"), ValuePerSupplyBox);
			SupplyBuildBorder = B->GetFloat(TEXT("SupplyBuildBorder"), SupplyBuildBorder);
		}
	}

	// Water.ini (the colour of water on the radar) and Multiplayer.ini (whether maps start unexplored)
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Water.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\Multiplayer.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (const FGenIniEntry* Color = B->Find(TEXT("RadarWaterColor")))
			{
				RadarWaterColor = ParseIniColor(Color->Raw);
			}
			if (B->Find(TEXT("UseShroud")))
			{
				bShroudInMultiplayer = B->GetBool(TEXT("UseShroud"));
			}
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
			if (const FGenIniEntry* S = B->Find(TEXT("IntrinsicSciences")))
			{
				for (const FString& V : S->Values) { if (!V.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { P.IntrinsicSciences.Add(V); } }
			}
			P.PurchaseScienceSets[0] = B->GetString(TEXT("PurchaseScienceCommandSetRank1"));
			P.PurchaseScienceSets[1] = B->GetString(TEXT("PurchaseScienceCommandSetRank3"));
			P.PurchaseScienceSets[2] = B->GetString(TEXT("PurchaseScienceCommandSetRank8"));
			P.SpecialPowerShortcutSet = B->GetString(TEXT("SpecialPowerShortcutCommandSet"));
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
	// Build variations know which template they stand in for, so that type checks treat them alike.
	for (const TPair<FString, TSharedPtr<FGenThingTemplate>>& It : Templates)
	{
		for (const FString& Variation : It.Value->BuildVariations)
		{
			if (TSharedPtr<FGenThingTemplate>* V = Templates.Find(Variation.ToLower()))
			{
				(*V)->VariationOf = It.Value.Get();
			}
		}
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
		else if (E.Key.Equals(TEXT("TransportSlotCount"), ESearchCase::IgnoreCase)) { T->TransportSlotCount = E.AsInt(); }
		else if (E.Key.Equals(TEXT("BuildVariations"), ESearchCase::IgnoreCase)) { T->BuildVariations = E.Values; }
		else if (E.Key.Equals(TEXT("ButtonImage"), ESearchCase::IgnoreCase)) { T->ButtonImage = E.Value(0); }
		else if (E.Key.Equals(TEXT("BuildTime"), ESearchCase::IgnoreCase)) { T->BuildTime = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("GeometryIsSmall"), ESearchCase::IgnoreCase)) { T->bGeometryIsSmall = E.AsBool(); }
		else if (E.Key.Equals(TEXT("CommandSet"), ESearchCase::IgnoreCase)) { T->CommandSet = E.Value(0); }
		else if (E.Key.Equals(TEXT("DisplayName"), ESearchCase::IgnoreCase)) { T->DisplayName = E.Value(0); }
		else if (E.Key.Equals(TEXT("EnergyProduction"), ESearchCase::IgnoreCase)) { T->EnergyProduction = E.AsInt(); }
		else if (E.Key.Equals(TEXT("EnergyBonus"), ESearchCase::IgnoreCase)) { T->EnergyBonus = E.AsInt(); }
		else if (E.Key.Equals(TEXT("ExperienceValue"), ESearchCase::IgnoreCase))
		{
			// "ExperienceValue = 15 15 30 40": by the victim's level (one value: the same at every level)
			T->ExperienceValue = E.AsInt();
			for (int32 i = 0; i < 4 && E.Values.Num() > 0; ++i) { T->ExperienceValues[i] = FCString::Atoi(*E.Values[FMath::Min(i, E.Values.Num() - 1)]); }
		}
		else if (E.Key.Equals(TEXT("SkillPointValue"), ESearchCase::IgnoreCase))
		{
			T->SkillPointValue = E.AsInt();
			for (int32 i = 0; i < 4 && E.Values.Num() > 0; ++i) { T->SkillPointValues[i] = FCString::Atoi(*E.Values[FMath::Min(i, E.Values.Num() - 1)]); }
		}
		else if (E.Key.Equals(TEXT("ExperienceRequired"), ESearchCase::IgnoreCase))
		{
			for (int32 i = 0; i < 4 && i < E.Values.Num(); ++i) { T->ExperienceRequired[i] = FCString::Atoi(*E.Values[i]); }
		}
		else if (E.Key.Equals(TEXT("IsTrainable"), ESearchCase::IgnoreCase)) { T->bIsTrainable = E.AsBool(); }
		else if (E.Key.Equals(TEXT("Prerequisites"), ESearchCase::IgnoreCase) && E.Block)
		{
			// "Object = A B" lines: the player needs one of A, B. "Science = S" lines: the player needs S.
			T->Prerequisites.Reset();
			T->PrerequisiteSciences.Reset();
			for (const FGenIniEntry& PE : E.Block->Entries)
			{
				if (PE.Key.Equals(TEXT("Object"), ESearchCase::IgnoreCase) && PE.Values.Num() > 0) { T->Prerequisites.Add(PE.Values); }
				else if (PE.Key.Equals(TEXT("Science"), ESearchCase::IgnoreCase)) { T->PrerequisiteSciences.Append(PE.Values); }
			}
		}
		else if (E.Key.Equals(TEXT("CrusherLevel"), ESearchCase::IgnoreCase)) { T->CrusherLevel = E.AsInt(); }
		else if (E.Key.Equals(TEXT("CrushableLevel"), ESearchCase::IgnoreCase)) { T->CrushableLevel = E.AsInt(); }
		else if (E.Key.Equals(TEXT("VisionRange"), ESearchCase::IgnoreCase)) { T->VisionRange = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("ShroudClearingRange"), ESearchCase::IgnoreCase)) { T->ShroudClearingRange = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("ShroudRevealToAllRange"), ESearchCase::IgnoreCase)) { T->ShroudRevealToAllRange = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("RadarPriority"), ESearchCase::IgnoreCase))
		{
			const FString& V = E.Value();
			T->RadarPriority = V.Equals(TEXT("STRUCTURE"), ESearchCase::IgnoreCase) ? EGenRadarPriority::Structure
				: V.Equals(TEXT("UNIT"), ESearchCase::IgnoreCase) ? EGenRadarPriority::Unit
				: V.Equals(TEXT("LOCAL_UNIT_ONLY"), ESearchCase::IgnoreCase) ? EGenRadarPriority::LocalUnitOnly
				: V.Equals(TEXT("NOT_ON_RADAR"), ESearchCase::IgnoreCase) ? EGenRadarPriority::NotOnRadar : EGenRadarPriority::Invalid;
		}
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
			// Modules that wait for an upgrade, and modules that are special powers (whatever else they are).
			{
				FGenUpgradeModule UM;
				if (FGenUpgradeData::ParseUpgradeModule(Type, *E.Block, UM)) { T->UpgradeModules.Add(MoveTemp(UM)); }
				FGenSpecialPowerModule PM;
				if (FGenUpgradeData::ParseSpecialPowerModule(Type, *E.Block, PM)) { T->PowerModules.Add(MoveTemp(PM)); }
			}
			if (Type.Equals(TEXT("AutoHealBehavior"), ESearchCase::IgnoreCase))
			{
				FGenUpgradeData::ParseAutoHeal(*E.Block, T->AutoHeal);
			}
			else if (Type.Equals(TEXT("GrantUpgradeCreate"), ESearchCase::IgnoreCase))
			{
				T->GrantUpgrades.Add(E.Block->GetString(TEXT("UpgradeToGrant")));
			}
			else if (Type.Equals(TEXT("HeightDieUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHeightDie = true;
				T->HeightDieTarget = E.Block->GetFloat(TEXT("TargetHeight"), 0.f);
				T->bHeightDieOnlyWhenMovingDown = E.Block->GetBool(TEXT("OnlyWhenMovingDown"));
				T->HeightDieInitialDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("InitialDelay"), 0.f));
			}
			else if (Type.Equals(TEXT("FireWeaponWhenDeadBehavior"), ESearchCase::IgnoreCase))
			{
				if (E.Block->GetBool(TEXT("StartsActive"))) { T->DeathWeapons.Add(E.Block->GetString(TEXT("DeathWeapon"))); }
			}
			else if (Type.Equals(TEXT("RadarUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHasRadarUpdate = true;
				T->RadarExtendFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("RadarExtendTime"), 0.f));
			}
			else if (Type.Equals(TEXT("DynamicShroudClearingRangeUpdate"), ESearchCase::IgnoreCase))
			{
				FGenDynamicShroud& D = T->DynamicShroud;
				D.bValid = true;
				D.ChangeInterval = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ChangeInterval"), 0.f));
				D.GrowInterval = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("GrowInterval"), 0.f));
				D.ShrinkDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ShrinkDelay"), 0.f));
				D.ShrinkTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ShrinkTime"), 0.f));
				D.GrowDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("GrowDelay"), 0.f));
				D.GrowTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("GrowTime"), 0.f));
				D.FinalVision = E.Block->GetFloat(TEXT("FinalVision"), 0.f);
			}
			else if (Type.Equals(TEXT("DeletionUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHasDeletion = true;
				T->DeletionMin = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MinLifetime"), 0.f));
				T->DeletionMax = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MaxLifetime"), 0.f));
			}
			else if (Type.Equals(TEXT("TransportContain"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("HelixContain"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("GarrisonContain"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("HealContain"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("TunnelContain"), ESearchCase::IgnoreCase))
			{
				// OpenContain and the modules built on it.
				FGenContainData& C = T->Contain;
				const FGenIniBlock& B = *E.Block;
				C.Type = Type.StartsWith(TEXT("Garrison"), ESearchCase::IgnoreCase) ? EGenContainType::Garrison
					: Type.StartsWith(TEXT("Heal"), ESearchCase::IgnoreCase) ? EGenContainType::Heal
					: Type.StartsWith(TEXT("Tunnel"), ESearchCase::IgnoreCase) ? EGenContainType::Tunnel : EGenContainType::Transport;
				C.ContainMax = B.GetInt(C.Type == EGenContainType::Transport ? TEXT("Slots") : TEXT("ContainMax"), 0);
				auto Kinds = [&B](const TCHAR* Key, TArray<FString>& Out)
				{
					if (const FGenIniEntry* K = B.Find(Key))
					{
						Out.Reset();
						for (const FString& V : K->Values) { Out.Add(V.ToUpper()); }
					}
				};
				if (C.Type == EGenContainType::Transport || C.Type == EGenContainType::Garrison)
				{
					C.AllowInsideKindOf = { TEXT("INFANTRY") }; // (transports and garrisons take infantry unless the data says otherwise)
				}
				Kinds(TEXT("AllowInsideKindOf"), C.AllowInsideKindOf);
				Kinds(TEXT("ForbidInsideKindOf"), C.ForbidInsideKindOf);
				C.bAllowAllies = B.GetBool(TEXT("AllowAlliesInside"), true);
				C.bAllowEnemies = B.GetBool(TEXT("AllowEnemiesInside"), true);
				C.bAllowNeutral = B.GetBool(TEXT("AllowNeutralInside"), true);
				// (A garrison always lets its occupants shoot out.)
				C.bPassengersAllowedToFire = C.Type == EGenContainType::Garrison || B.GetBool(TEXT("PassengersAllowedToFire"));
				C.DamagePercentToUnits = ParsePercent(B.GetString(TEXT("DamagePercentToUnits"), TEXT("0%")));
				C.bBurnedDeathToUnits = B.GetBool(TEXT("BurnedDeathToUnits"), true);
				C.NumberOfExitPaths = B.GetInt(TEXT("NumberOfExitPaths"), 1);
				C.ExitDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("ExitDelay"), 0.f));
				C.bGoAggressiveOnExit = B.GetBool(TEXT("GoAggressiveOnExit"));
				C.HealthRegenPercentPerSec = B.GetFloat(TEXT("HealthRegen%PerSec"), 0.f);
				if (const FGenIniEntry* Payload = B.Find(TEXT("InitialPayload")))
				{
					C.InitialPayload = Payload->Value(0);
					C.InitialPayloadCount = Payload->Values.Num() >= 2 ? FCString::Atoi(*Payload->Values[1]) : 1;
				}
				C.FramesForFullHeal = GenLogic::MsecToFrames(B.GetFloat(TEXT("TimeForFullHeal"), 0.f));
				// (Barracks and tunnels heal; a garrison only when the data asks for it.)
				C.bHealObjects = C.Type == EGenContainType::Heal || C.Type == EGenContainType::Tunnel || B.GetBool(TEXT("HealObjects"));
				C.bEnclosing = B.GetBool(TEXT("IsEnclosingContainer"), true);
				C.bArmedRidersUpgradeWeaponSet = B.GetBool(TEXT("ArmedRidersUpgradeMyWeaponSet"));
				C.bDelayExitInAir = B.GetBool(TEXT("DelayExitInAir"));
			}
			else if (Type.Equals(TEXT("ParachuteContain"), ESearchCase::IgnoreCase))
			{
				T->bParachute = true;
				T->ParachuteOpenDist = E.Block->GetFloat(TEXT("ParachuteOpenDist"), 0.f);
			}
			else if (Type.Equals(TEXT("FireWeaponUpdate"), ESearchCase::IgnoreCase))
			{
				T->FireWeaponUpdates.Emplace(E.Block->GetString(TEXT("Weapon")), GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("InitialDelay"), 0.f)));
			}
			else if (Type.Equals(TEXT("SmartBombTargetHomingUpdate"), ESearchCase::IgnoreCase))
			{
				T->SmartBombCourseCorrection = E.Block->GetFloat(TEXT("CourseCorrectionScalar"), 0.99f);
			}
			else if (Type.Equals(TEXT("EMPUpdate"), ESearchCase::IgnoreCase))
			{
				// The pulse itself lasts Lifetime; what it catches stays disabled for DisabledDuration.
				T->EMPDisabledDuration = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DisabledDuration"), 0.f));
				T->EMPRadius = E.Block->GetFloat(TEXT("EffectRadius"), 200.f);
				T->EMPStartDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("StartFadeTime"), 0.f));
				T->bEMPSparesOwnBuildings = E.Block->GetBool(TEXT("DoesNotAffectMyOwnBuildings"));
				T->bHasDeletion = true;
				T->DeletionMin = T->DeletionMax = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("Lifetime"), 1000.f));
			}
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
			else if (Type.Equals(TEXT("ProductionUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHasProduction = true;
				T->MaxQueueEntries = E.Block->GetInt(TEXT("MaxQueueEntries"), 9);
				T->DoorOpeningTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorOpeningTime"), 0.f));
				T->DoorCloseTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorCloseTime"), 0.f));
			}
			else if (Type.EndsWith(TEXT("ProductionExitUpdate"), ESearchCase::IgnoreCase))
			{
				// "UnitCreatePoint = X:-10.0 Y:-30.0 Z:0.0", in the building's own frame
				auto Coord = [&E](const TCHAR* Key, FVector3f& Out)
				{
					const FGenIniEntry* C = E.Block->Find(Key);
					if (!C) { return; }
					TArray<FString> Tok;
					GenIni::Tokenize(C->Raw, Tok, TEXT(" \t=:"));
					for (int32 i = 0; i + 1 < Tok.Num(); i += 2)
					{
						const float V = FCString::Atof(*Tok[i + 1]);
						if (Tok[i].Equals(TEXT("X"), ESearchCase::IgnoreCase)) { Out.X = V; }
						else if (Tok[i].Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { Out.Y = V; }
						else if (Tok[i].Equals(TEXT("Z"), ESearchCase::IgnoreCase)) { Out.Z = V; }
					}
				};
				T->bHasExit = true;
				Coord(TEXT("UnitCreatePoint"), T->UnitCreatePoint);
				Coord(TEXT("NaturalRallyPoint"), T->NaturalRallyPoint);
			}
			else if (Type.Equals(TEXT("SupplyWarehouseDockUpdate"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("SupplyCenterDockUpdate"), ESearchCase::IgnoreCase))
			{
				T->bSupplyWarehouse = Type.StartsWith(TEXT("SupplyWarehouse"), ESearchCase::IgnoreCase);
				T->bSupplyCenter = !T->bSupplyWarehouse;
				T->StartingBoxes = E.Block->GetInt(TEXT("StartingBoxes"), 1);
				T->bDeleteWhenEmpty = E.Block->GetBool(TEXT("DeleteWhenEmpty"));
				T->NumberApproachPositions = E.Block->GetInt(TEXT("NumberApproachPositions"), 0);
				T->bDockAllowsPassthrough = E.Block->GetBool(TEXT("AllowsPassthrough"), true);
			}
			else if (Type.Equals(TEXT("VeterancyGainCreate"), ESearchCase::IgnoreCase))
			{
				// (Several of these may be listed; the highest level whose science is owned applies. The
				// vanilla units have one.)
				static const TCHAR* Levels[] = { TEXT("REGULAR"), TEXT("VETERAN"), TEXT("ELITE"), TEXT("HEROIC") };
				const FString Level = E.Block->GetString(TEXT("StartingLevel"));
				for (int32 i = 0; i < 4; ++i)
				{
					if (Level.Equals(Levels[i], ESearchCase::IgnoreCase) && i >= T->VeterancyStartLevel)
					{
						T->VeterancyStartLevel = i;
						T->VeterancyStartScience = E.Block->GetString(TEXT("ScienceRequired"));
					}
				}
			}
			else if (Type.Equals(TEXT("RepairDockUpdate"), ESearchCase::IgnoreCase))
			{
				T->bRepairDock = true;
				T->RepairDockFrames = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("TimeForFullHeal"), 0.f)));
				T->NumberApproachPositions = E.Block->GetInt(TEXT("NumberApproachPositions"), 0);
				T->bDockAllowsPassthrough = E.Block->GetBool(TEXT("AllowsPassthrough"), true);
			}
			else if (Type.Equals(TEXT("SpawnBehavior"), ESearchCase::IgnoreCase))
			{
				// Only the one-shot kind (a building that comes with units); replacement spawns need SlavedUpdate.
				if (E.Block->GetBool(TEXT("OneShot")))
				{
					T->SpawnTemplate = E.Block->GetString(TEXT("SpawnTemplateName"));
					T->SpawnNumber = E.Block->GetInt(TEXT("SpawnNumber"), 0);
				}
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
			else if (Type.Equals(TEXT("ParkingPlaceBehavior"), ESearchCase::IgnoreCase))
			{
				T->bParkingPlace = true;
				T->ParkingRows = E.Block->GetInt(TEXT("NumRows"), 0);
				T->ParkingCols = E.Block->GetInt(TEXT("NumCols"), 0);
				T->bParkingHasRunways = E.Block->GetBool(TEXT("HasRunways"));
				T->ParkingApproachHeight = E.Block->GetFloat(TEXT("ApproachHeight"), 0.f);
				T->bParkInHangars = E.Block->GetBool(TEXT("ParkInHangars"));
				T->ParkingHealPerSecond = E.Block->GetFloat(TEXT("HealAmountPerSecond"), 0.f);
			}
			else if (Type.Equals(TEXT("JetSlowDeathBehavior"), ESearchCase::IgnoreCase))
			{
				FGenJetDeath& J = T->JetDeath;
				J.bValid = true;
				auto NameOrNone = [&E](const TCHAR* Key)
				{
					const FString V = E.Block->GetString(Key);
					return V.Equals(TEXT("None"), ESearchCase::IgnoreCase) ? FString() : V;
				};
				auto Percent = [&E](const TCHAR* Key, float Default)
				{
					const FGenIniEntry* F = E.Block->Find(Key);
					return F ? ParsePercent(F->Value()) : Default;
				};
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { J.DeathTypes = DT->Values; }
				J.FXOnGroundDeath = NameOrNone(TEXT("FXOnGroundDeath"));
				J.OCLOnGroundDeath = NameOrNone(TEXT("OCLOnGroundDeath"));
				J.FXInitialDeath = NameOrNone(TEXT("FXInitialDeath"));
				J.OCLInitialDeath = NameOrNone(TEXT("OCLInitialDeath"));
				J.FXSecondary = NameOrNone(TEXT("FXSecondary"));
				J.OCLSecondary = NameOrNone(TEXT("OCLSecondary"));
				J.FXHitGround = NameOrNone(TEXT("FXHitGround"));
				J.OCLHitGround = NameOrNone(TEXT("OCLHitGround"));
				J.FXFinalBlowUp = NameOrNone(TEXT("FXFinalBlowUp"));
				J.OCLFinalBlowUp = NameOrNone(TEXT("OCLFinalBlowUp"));
				J.RollRate = E.Block->GetFloat(TEXT("RollRate"), 0.f);
				J.RollRateDelta = Percent(TEXT("RollRateDelta"), 1.f);
				J.PitchRate = E.Block->GetFloat(TEXT("PitchRate"), 0.f);
				J.FallHowFast = Percent(TEXT("FallHowFast"), 0.5f);
				J.DelaySecondaryFromInitialDeath = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DelaySecondaryFromInitialDeath"), 0.f));
				J.DelayFinalBlowUpFromHitGround = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DelayFinalBlowUpFromHitGround"), 0.f));
			}
			else if (Type.Equals(TEXT("HelicopterSlowDeathBehavior"), ESearchCase::IgnoreCase))
			{
				FGenHeliDeath& H = T->HeliDeath;
				H.bValid = true;
				auto NameOrNone = [&E](const TCHAR* Key)
				{
					const FString V = E.Block->GetString(Key);
					return V.Equals(TEXT("None"), ESearchCase::IgnoreCase) ? FString() : V;
				};
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { H.DeathTypes = DT->Values; }
				H.SpiralOrbitTurnRate = GenLogic::AngularVelocityPerFrame(E.Block->GetFloat(TEXT("SpiralOrbitTurnRate"), 0.f));
				H.SpiralOrbitForwardSpeed = GenLogic::VelocityPerFrame(E.Block->GetFloat(TEXT("SpiralOrbitForwardSpeed"), 0.f));
				H.SpiralOrbitForwardSpeedDamping = E.Block->GetFloat(TEXT("SpiralOrbitForwardSpeedDamping"), 1.f);
				H.MinSelfSpin = GenLogic::AngularVelocityPerFrame(E.Block->GetFloat(TEXT("MinSelfSpin"), 0.f));
				H.MaxSelfSpin = GenLogic::AngularVelocityPerFrame(E.Block->GetFloat(TEXT("MaxSelfSpin"), 0.f));
				H.SelfSpinUpdateDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("SelfSpinUpdateDelay"), 0.f));
				H.SelfSpinUpdateAmount = FMath::DegreesToRadians(E.Block->GetFloat(TEXT("SelfSpinUpdateAmount"), 0.f));
				if (const FGenIniEntry* F = E.Block->Find(TEXT("FallHowFast"))) { H.FallHowFast = ParsePercent(F->Value()); }
				H.MinBladeFlyOffDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MinBladeFlyOffDelay"), 0.f));
				H.MaxBladeFlyOffDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MaxBladeFlyOffDelay"), 0.f));
				H.AttachParticle = NameOrNone(TEXT("AttachParticle"));
				H.BladeBone = NameOrNone(TEXT("BladeBoneName"));
				H.FXBlade = NameOrNone(TEXT("FXBlade"));
				H.OCLBlade = NameOrNone(TEXT("OCLBlade"));
				H.FXHitGround = NameOrNone(TEXT("FXHitGround"));
				H.OCLHitGround = NameOrNone(TEXT("OCLHitGround"));
				H.FXFinalBlowUp = NameOrNone(TEXT("FXFinalBlowUp"));
				H.OCLFinalBlowUp = NameOrNone(TEXT("OCLFinalBlowUp"));
				H.DelayFromGroundToFinalDeath = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DelayFromGroundToFinalDeath"), 0.f));
				H.FinalRubbleObject = NameOrNone(TEXT("FinalRubbleObject"));
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
					else if (DE.Key.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase) && DE.Values.Num() >= 2)
					{
						const int32 Phase = DE.Values[0].Equals(TEXT("MIDPOINT"), ESearchCase::IgnoreCase) ? FGenSlowDeath::Midpoint
							: (DE.Values[0].Equals(TEXT("FINAL"), ESearchCase::IgnoreCase) ? FGenSlowDeath::Final : FGenSlowDeath::Initial);
						D.Weapons[Phase].Add(DE.Values[1]);
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
				if (T->bIsMissile)
				{
					T->bMissileUseWeaponSpeed = E.Block->GetBool(TEXT("UseWeaponSpeed"));
					T->MissileInitialVelocity = GenLogic::VelocityPerFrame(E.Block->GetFloat(TEXT("InitialVelocity"), 0.f));
					T->MissileIgnitionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("IgnitionDelay"), 0.f));
				}
				T->bChinookAI = Type.Equals(TEXT("ChinookAIUpdate"), ESearchCase::IgnoreCase);
				T->bIsDozer = Type.Equals(TEXT("DozerAIUpdate"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("WorkerAIUpdate"), ESearchCase::IgnoreCase);
				if (T->bIsDozer)
				{
					T->DozerRepairPerSecond = ParsePercent(E.Block->GetString(TEXT("RepairHealthPercentPerSecond"), TEXT("0%")));
					T->DozerBoredTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("BoredTime"), 0.f));
					T->DozerBoredRange = E.Block->GetFloat(TEXT("BoredRange"), 0.f);
				}
				if (E.Block->Find(TEXT("MaxBoxes")))
				{
					// SupplyTruckAIUpdate and the modules built on it (ChinookAIUpdate, WorkerAIUpdate)
					T->bIsSupplyTruck = true;
					T->MaxBoxes = E.Block->GetInt(TEXT("MaxBoxes"), 3);
					T->SupplyCenterActionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("SupplyCenterActionDelay"), 0.f));
					T->SupplyWarehouseActionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("SupplyWarehouseActionDelay"), 0.f));
					T->SupplyWarehouseScanDistance = E.Block->GetFloat(TEXT("SupplyWarehouseScanDistance"), 100.f);
				}
				if (Type.Equals(TEXT("JetAIUpdate"), ESearchCase::IgnoreCase))
				{
					T->bJetAI = true;
					T->bJetNeedsRunway = E.Block->GetBool(TEXT("NeedsRunway"), true);
					T->bJetKeepsParkingSpace = E.Block->GetBool(TEXT("KeepsParkingSpaceWhenAirborne"), true);
					if (const FGenIniEntry* F = E.Block->Find(TEXT("OutOfAmmoDamagePerSecond"))) { T->JetOutOfAmmoDamagePerSecond = ParsePercent(F->Value()); }
					T->JetTakeoffPause = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("TakeoffPause"), 0.f));
					T->JetMinHeight = E.Block->GetFloat(TEXT("MinHeight"), 0.f);
					T->JetParkingOffset = E.Block->GetFloat(TEXT("ParkingOffset"), 0.f);
					T->JetReturnToBaseIdleTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ReturnToBaseIdleTime"), 0.f));
					T->JetAttackLocoSet = E.Block->GetString(TEXT("AttackLocomotorType")).ToUpper();
					T->JetAttackLocoPersistTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("AttackLocomotorPersistTime"), 0.f));
					T->JetReturnLocoSet = E.Block->GetString(TEXT("ReturnForAmmoLocomotorType")).ToUpper();
				}
				T->bDeliverPayloadAI = Type.Equals(TEXT("DeliverPayloadAIUpdate"), ESearchCase::IgnoreCase);
				if (T->bDeliverPayloadAI) { T->DeliverDoorDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorDelay"), 0.f)); }
				if (const FGenIniEntry* Boost = E.Block->Find(TEXT("UpgradedSupplyBoost"))) { T->UpgradedSupplyBoost = Boost->AsInt(); }
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
			if (const FGenIniEntry* Prefix = E.Block->Find(TEXT("SupplyBonePrefix"))) { T->SupplyBonePrefix = Prefix->Value(); }
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

namespace
{
	/** SparseMatchFinder, simplified: every condition of the set must hold; the set naming most conditions wins. */
	template <typename TEntry>
	const TEntry* FindBestSet(const TArray<TEntry>& Sets, const TArray<FString>& Flags)
	{
		const TEntry* Best = nullptr;
		int32 BestCount = -1;
		for (const TEntry& S : Sets)
		{
			bool bAll = true;
			for (const FString& C : S.Conditions)
			{
				bAll &= Flags.Contains(C);
			}
			if (bAll && S.Conditions.Num() > BestCount)
			{
				BestCount = S.Conditions.Num();
				Best = &S;
			}
		}
		return Best ? Best : (Sets.Num() > 0 ? &Sets[0] : nullptr);
	}
}

const FGenWeaponSetEntry* FGenThingTemplate::FindWeaponSet(const TArray<FString>& Flags) const
{
	return FindBestSet(WeaponSets, Flags);
}

const FGenArmorSetEntry* FGenThingTemplate::FindArmorSet(const TArray<FString>& Flags) const
{
	return FindBestSet(ArmorSets, Flags);
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
