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
			else if (Is(TEXT("GroupMovementPriority")))
			{
				const FString P = E.Value();
				L.MovePriority = P.Equals(TEXT("MOVES_BACK"), ESearchCase::IgnoreCase) ? 0 : P.Equals(TEXT("MOVES_FRONT"), ESearchCase::IgnoreCase) ? 2 : 1;
			}
			else if (Is(TEXT("StickToGround"))) { L.bStickToGround = E.AsBool(); }
			else if (Is(TEXT("CanMoveBackwards"))) { L.bCanMoveBackwards = E.AsBool(); }
			else if (Is(TEXT("ZAxisBehavior"))) { L.ZAxisBehavior = E.Value(); }
			else if (Is(TEXT("Lift"))) { L.Lift = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("LiftDamaged"))) { L.LiftDamaged = GenLogic::AccelPerFrame(E.AsFloat()); }
			else if (Is(TEXT("SpeedLimitZ"))) { L.SpeedLimitZ = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("PreferredHeightDamping"))) { L.PreferredHeightDamping = E.AsFloat(); }
			else if (Is(TEXT("MaxThrustAngle"))) { L.MaxThrustAngle = FMath::DegreesToRadians(E.AsFloat()); }
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
			else if (Is(TEXT("AccelerationPitchLimit"))) { L.AccelPitchLimit = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("DecelerationPitchLimit"))) { L.DecelPitchLimit = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("BounceAmount"))) { L.BounceKick = FMath::DegreesToRadians(E.AsFloat()) / 30.f; }
			else if (Is(TEXT("ForwardAccelerationPitchFactor"))) { L.ForwardAccelCoef = E.AsFloat(); }
			else if (Is(TEXT("LateralAccelerationRollFactor"))) { L.LateralAccelCoef = E.AsFloat(); }
			else if (Is(TEXT("HasSuspension"))) { L.bHasSuspension = E.AsBool(); }
			else if (Is(TEXT("FrontWheelTurnAngle"))) { L.WheelTurnAngle = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("MaximumWheelExtension"))) { L.MaxWheelExtension = E.AsFloat(); }
			else if (Is(TEXT("CirclingRadius"))) { L.CirclingRadius = E.AsFloat(); }
			else if (Is(TEXT("SlideIntoPlaceTime"))) { L.SlideIntoPlaceFrames = E.AsFloat() * float(GenLogic::GetLogicFPS()) / 1000.f; }
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

	/** INI::parseVeterancyLevelFlags: "ALL -REGULAR", "ELITE HEROIC", ... as a bit per level. */
	/** DumbProjectileBehavior / MissileAIUpdate: GarrisonHitKillRequiredKindOf ... GarrisonHitKillFX. */
	void ParseGarrisonHitKill(const FGenIniBlock& B, FGenDumbProjectile& P)
	{
		P.GarrisonHitKillCount = B.GetInt(TEXT("GarrisonHitKillCount"), 0);
		P.GarrisonHitKillFX = B.GetString(TEXT("GarrisonHitKillFX"));
		auto Kinds = [&B](const TCHAR* Key, TArray<FString>& Out)
		{
			Out.Reset();
			if (const FGenIniEntry* K = B.Find(Key))
			{
				for (const FString& V : K->Values) { if (!V.Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { Out.Add(V.ToUpper()); } }
			}
		};
		Kinds(TEXT("GarrisonHitKillRequiredKindOf"), P.GarrisonHitKillRequired);
		Kinds(TEXT("GarrisonHitKillForbiddenKindOf"), P.GarrisonHitKillForbidden);
	}

	uint8 ParseVeterancyMask(const TArray<FString>& Values)
	{
		static const TCHAR* Names[4] = { TEXT("REGULAR"), TEXT("VETERAN"), TEXT("ELITE"), TEXT("HEROIC") };
		uint8 Mask = 0xF;
		for (const FString& V : Values)
		{
			if (V.Equals(TEXT("ALL"), ESearchCase::IgnoreCase)) { Mask = 0xF; continue; }
			if (V.Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { Mask = 0; continue; }
			const bool bClear = V.StartsWith(TEXT("-"));
			const FString Name = (bClear || V.StartsWith(TEXT("+"))) ? V.Mid(1) : V;
			for (int32 i = 0; i < 4; ++i)
			{
				if (Name.Equals(Names[i], ESearchCase::IgnoreCase))
				{
					Mask = bClear ? (Mask & ~(1 << i)) : (Mask | (1 << i));
				}
			}
		}
		return Mask;
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
			else if (Is(TEXT("ScatterTargetScalar"))) { W.ScatterTargetScalar = E.AsFloat(); }
			else if (Is(TEXT("ShotsPerBarrel"))) { W.ShotsPerBarrel = FMath::Max(1, E.AsInt()); }
			else if (Is(TEXT("ScatterTarget")))
			{
				// "ScatterTarget = X: 0.133 Y:-0.200"
				TArray<FString> Tokens;
				GenIni::Tokenize(E.Raw, Tokens, TEXT(" \t=:"));
				FVector2f V = FVector2f::ZeroVector;
				for (int32 i = 0; i + 1 < Tokens.Num(); i += 2)
				{
					if (Tokens[i].Equals(TEXT("X"), ESearchCase::IgnoreCase)) { V.X = FCString::Atof(*Tokens[i + 1]); }
					else if (Tokens[i].Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { V.Y = FCString::Atof(*Tokens[i + 1]); }
				}
				W.ScatterTargets.Add(V);
			}
			else if (Is(TEXT("ScatterRadiusVsInfantry"))) { W.ScatterRadiusVsInfantry = E.AsFloat(); }
			else if (Is(TEXT("DamageType"))) { W.DamageType = E.Value().ToUpper(); }
			else if (Is(TEXT("DeathType"))) { W.DeathType = E.Value().ToUpper(); }
			else if (Is(TEXT("WeaponSpeed"))) { W.WeaponSpeed = GenLogic::VelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("WeaponRecoil"))) { W.WeaponRecoil = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("ProjectileObject"))) { W.ProjectileObject = E.Value().Equals(TEXT("NONE"), ESearchCase::IgnoreCase) ? FString() : E.Value(); }
			else if (Is(TEXT("ProjectileStreamName"))) { W.ProjectileStreamName = E.Value(); }
			else if (Is(TEXT("AllowAttackGarrisonedBldgs"))) { W.bAllowAttackGarrisonedBldgs = E.AsBool(); }
			else if (Is(TEXT("FireFX"))) { W.FireFX = E.Value(); }
			else if (Is(TEXT("ProjectileDetonationFX"))) { W.ProjectileDetonationFX = E.Value(); }
			else if (Is(TEXT("FireOCL"))) { W.FireOCL = E.Value(); }
			else if (Is(TEXT("ProjectileDetonationOCL"))) { W.ProjectileDetonationOCL = E.Value(); }
			else if (Is(TEXT("ProjectileExhaust"))) { W.ProjectileExhaust = E.Value(); }
			else if (Is(TEXT("LaserName"))) { W.LaserName = E.Value(); }
			else if (Is(TEXT("RequestAssistRange"))) { W.RequestAssistRange = E.AsFloat(); }
			else if (Is(TEXT("LaserBoneName"))) { W.LaserBoneName = E.Value(); }
			else if (Is(TEXT("FireSound"))) { W.FireSound = E.Value(); }
			else if (Is(TEXT("FireSoundLoopTime"))) { W.FireSoundLoopTime = GenLogic::MsecToFrames(E.AsFloat()); }
			else if (Is(TEXT("ClipSize"))) { W.ClipSize = E.AsInt(); }
			else if (Is(TEXT("ClipReloadTime"))) { W.ClipReloadTime = GenLogic::MsecToFrames(E.AsFloat()); }
			else if (Is(TEXT("ContinuousFireOne"))) { W.ContinuousFireOne = E.AsInt(); }
			else if (Is(TEXT("ContinuousFireTwo"))) { W.ContinuousFireTwo = E.AsInt(); }
			else if (Is(TEXT("ContinuousFireCoast"))) { W.ContinuousFireCoast = GenLogic::MsecToFrames(E.AsFloat()); }
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
			else if (Is(TEXT("PreAttackType")))
			{
				W.PrefireType = E.Value().Equals(TEXT("PER_ATTACK"), ESearchCase::IgnoreCase) ? FGenWeaponTemplate::EPrefire::PerAttack
					: E.Value().Equals(TEXT("PER_CLIP"), ESearchCase::IgnoreCase) ? FGenWeaponTemplate::EPrefire::PerClip : FGenWeaponTemplate::EPrefire::PerShot;
			}
			else if (Is(TEXT("MinTargetPitch"))) { W.MinTargetPitch = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("MaxTargetPitch"))) { W.MaxTargetPitch = FMath::DegreesToRadians(E.AsFloat()); }
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
			else if (Is(TEXT("AntiMine"))) { W.bAntiMine = E.AsBool(); }
			else if (Is(TEXT("ContinueAttackRange"))) { W.ContinueAttackRange = E.AsFloat(); }
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
		T.bAllowsPitch = B.GetBool(TEXT("AllowsPitch"));
		if (B.Find(TEXT("TurretPitchRate"))) { T.PitchRate = GenLogic::AngularVelocityPerFrame(B.GetFloat(TEXT("TurretPitchRate"), 0.f)); }
		T.NaturalPitch = FMath::DegreesToRadians(B.GetFloat(TEXT("NaturalTurretPitch"), 0.f));
		T.FirePitch = FMath::DegreesToRadians(B.GetFloat(TEXT("FirePitch"), 0.f));
		T.MinPitch = FMath::DegreesToRadians(B.GetFloat(TEXT("MinPhysicalPitch"), 0.f));
		T.GroundUnitPitch = FMath::DegreesToRadians(B.GetFloat(TEXT("GroundUnitPitch"), 0.f));
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
			else if (Is(TEXT("Flags"))) { S.Flags = GenStateFlags::Parse(E.Values); }
			else if (Is(TEXT("TransitionKey"))) { S.TransitionKey = E.Value(0).ToLower(); }
			else if (Is(TEXT("WaitForStateToFinishIfPossible"))) { S.AllowToFinishKey = E.Value(0).ToLower(); }
			else if (Is(TEXT("AnimationSpeedFactorRange")) && E.Values.Num() >= 2)
			{
				S.AnimSpeedMin = E.AsFloat(0, 1.f);
				S.AnimSpeedMax = E.AsFloat(1, 1.f);
			}
			else if (Is(TEXT("HideSubObject")) || Is(TEXT("ShowSubObject")))
			{
				// parseShowHideSubObject: a name the state has already (its own, or the default state's it
				// started from) keeps its place in the list and changes its flag - the order matters, because
				// showing a part shows what hangs on its bone as well (a gun's muzzle flash, hidden further
				// down the list). "None" empties the list.
				const bool bHide = Is(TEXT("HideSubObject"));
				for (const FString& V : E.Values)
				{
					if (V.Equals(TEXT("None"), ESearchCase::IgnoreCase))
					{
						S.HideShow.Reset();
						break;
					}
					bool bFound = false;
					for (TPair<FString, bool>& Have : S.HideShow)
					{
						if (Have.Key.Equals(V, ESearchCase::IgnoreCase))
						{
							Have.Value = bHide;
							bFound = true;
						}
					}
					if (!bFound)
					{
						S.HideShow.Add({ V, bHide });
					}
				}
			}
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

uint32 GenStateFlags::Parse(const TArray<FString>& Names)
{
	static const TCHAR* const Known[] = { TEXT("RANDOMSTART"), TEXT("START_FRAME_FIRST"), TEXT("START_FRAME_LAST"), TEXT("ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT"),
		TEXT("PRISTINE_BONE_POS_IN_FINAL_FRAME"), TEXT("MAINTAIN_FRAME_ACROSS_STATES"), TEXT("MAINTAIN_FRAME_ACROSS_STATES2"), TEXT("MAINTAIN_FRAME_ACROSS_STATES3"),
		TEXT("MAINTAIN_FRAME_ACROSS_STATES4"), TEXT("RESTART_ANIM_WHEN_COMPLETE") };
	uint32 Flags = 0;
	for (const FString& Name : Names)
	{
		for (int32 i = 0; i < UE_ARRAY_COUNT(Known); ++i)
		{
			if (Name.Equals(Known[i], ESearchCase::IgnoreCase))
			{
				Flags |= 1u << i;
			}
		}
	}
	return Flags;
}

const FGenModelConditionState* FGenDrawModule::FindTransition(const FString& FromKey, const FString& ToKey) const
{
	return Transitions.Num() > 0 ? Transitions.Find(FromKey + TEXT(" ") + ToKey) : nullptr;
}

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
			{
				static const TCHAR* Kinds[7] = { TEXT("AutoFireParticleSmall"), TEXT("AutoFireParticleMedium"), TEXT("AutoFireParticleLarge"),
					TEXT("AutoSmokeParticleSmall"), TEXT("AutoSmokeParticleMedium"), TEXT("AutoSmokeParticleLarge"), TEXT("AutoAflameParticle") };
				for (int32 i = 0; i < 7; ++i)
				{
					const FString Kind(Kinds[i]);
					AutoParticles[i].Prefix = B->GetString(*(Kind + TEXT("Prefix")), *AutoParticles[i].Prefix);
					AutoParticles[i].System = B->GetString(*(Kind + TEXT("System")), *AutoParticles[i].System);
					AutoParticles[i].Max = B->GetInt(*(Kind + TEXT("Max")), AutoParticles[i].Max);
				}
			}
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
			if (const FGenIniEntry* R = B->Find(TEXT("BaseRegenHealthPercentPerSecond"))) { BaseRegenHealthPercentPerSecond = ParsePercent(R->Value()); }
			if (const FGenIniEntry* R = B->Find(TEXT("BaseRegenDelay"))) { BaseRegenDelay = GenLogic::MsecToFrames(R->AsFloat()); }
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
			MultipleFactory = B->GetFloat(TEXT("MultipleFactory"), MultipleFactory);
			AllowedHeightVariationForBuilding = B->GetFloat(TEXT("AllowedHeightVariationForBuilding"), AllowedHeightVariationForBuilding);
			ValuePerSupplyBox = B->GetInt(TEXT("ValuePerSupplyBox"), ValuePerSupplyBox);
			SupplyBuildBorder = B->GetFloat(TEXT("SupplyBuildBorder"), SupplyBuildBorder);
			GroupMoveClickToGatherFactor = B->GetFloat(TEXT("GroupMoveClickToGatherAreaFactor"), GroupMoveClickToGatherFactor);
			bDrawSkyBox = B->GetBool(TEXT("DrawSkyBox"), bDrawSkyBox);
			bUseCloudMap = B->GetBool(TEXT("UseCloudMap"), bUseCloudMap);
			bUseLightMap = B->GetBool(TEXT("UseLightMap"), bUseLightMap);
			SkyBoxPositionZ = B->GetFloat(TEXT("SkyBoxPositionZ"), SkyBoxPositionZ);
			SkyBoxScale = B->GetFloat(TEXT("SkyBoxScale"), SkyBoxScale);
			StandardMinefieldDensity = B->GetFloat(TEXT("StandardMinefieldDensity"), StandardMinefieldDensity);
			StandardMinefieldDistance = B->GetFloat(TEXT("StandardMinefieldDistance"), StandardMinefieldDistance);
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

	// DamageFX
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\DamageFX.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\DamageFX.ini"), Blocks);
		static const TCHAR* LevelNames[4] = { TEXT("REGULAR"), TEXT("VETERAN"), TEXT("ELITE"), TEXT("HEROIC") };
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("DamageFX"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FGenDamageFX& D = DamageFXs.FindOrAdd(B->Name().ToLower());
			D = FGenDamageFX();
			D.Name = B->Name();
			for (const FGenIniEntry& E : B->Entries)
			{
				// "MajorFX = <damage type> <fx>", "VeterancyMajorFX = <level> <damage type> <fx>"; the damage
				// type "Default" stands for all of them, including those given lines of their own before.
				FString Field = E.Key;
				const bool bVeterancy = Field.RemoveFromStart(TEXT("Veterancy"), ESearchCase::IgnoreCase);
				int32 Arg = 0, First = 0, Last = 3;
				if (bVeterancy && E.Values.Num() > 0)
				{
					for (int32 i = 0; i < 4; ++i)
					{
						if (E.Values[0].Equals(LevelNames[i], ESearchCase::IgnoreCase)) { First = Last = i; }
					}
					Arg = 1;
				}
				if (E.Values.Num() < Arg + 2)
				{
					continue;
				}
				const FString& Type = E.Values[Arg];
				const FString& Value = E.Values[Arg + 1];
				auto Apply = [&](FGenDamageFX::FLevels& Levels)
				{
					for (int32 L = First; L <= Last; ++L)
					{
						FGenDamageFX::FEntry& Entry = Levels.L[L];
						if (Field.Equals(TEXT("AmountForMajorFX"), ESearchCase::IgnoreCase)) { Entry.AmountForMajor = FCString::Atof(*Value); }
						else if (Field.Equals(TEXT("MajorFX"), ESearchCase::IgnoreCase)) { Entry.MajorFX = Value.Equals(TEXT("None"), ESearchCase::IgnoreCase) ? FString() : Value; }
						else if (Field.Equals(TEXT("MinorFX"), ESearchCase::IgnoreCase)) { Entry.MinorFX = Value.Equals(TEXT("None"), ESearchCase::IgnoreCase) ? FString() : Value; }
						else if (Field.Equals(TEXT("ThrottleTime"), ESearchCase::IgnoreCase)) { Entry.ThrottleFrames = GenLogic::MsecToFrames(FCString::Atof(*Value)); }
					}
				};
				if (Type.Equals(TEXT("DEFAULT"), ESearchCase::IgnoreCase))
				{
					Apply(D.Default);
					for (TPair<FString, FGenDamageFX::FLevels>& It : D.ByType) { Apply(It.Value); }
				}
				else
				{
					FGenDamageFX::FLevels* Levels = D.ByType.Find(Type);
					if (!Levels)
					{
						Levels = &D.ByType.Add(Type.ToUpper(), D.Default);
					}
					Apply(*Levels);
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
			P.SpecialPowerShortcutWinName = B->GetString(TEXT("SpecialPowerShortcutWinName"));
			P.SpecialPowerShortcutButtonCount = B->GetInt(TEXT("SpecialPowerShortcutButtonCount"), 0);
			P.SideIcon = B->GetString(TEXT("SideIconImage"));
			P.ScoreScreenImage = B->GetString(TEXT("ScoreScreenImage"));
			P.ScoreScreenMusic = B->GetString(TEXT("ScoreScreenMusic"));
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
	// (The crate system's files define objects too: the crates themselves. The engine reads them later,
	// with TheCrateSystem.)
	ObjectFiles.Add(TEXT("Data\\INI\\Default\\Crate.ini"));
	ObjectFiles.Add(TEXT("Data\\INI\\Crate.ini"));
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
			else if (B->Keyword.Equals(TEXT("CrateData"), ESearchCase::IgnoreCase))
			{
				// CrateSystem::parseCrateTemplateDefinition
				static const TCHAR* Levels[4] = { TEXT("REGULAR"), TEXT("VETERAN"), TEXT("ELITE"), TEXT("HEROIC") };
				FGenCrateData& C = CrateData.FindOrAdd(B->Name().ToLower());
				C = FGenCrateData();
				C.Name = B->Name();
				for (const FGenIniEntry& CE : B->Entries)
				{
					if (CE.Key.Equals(TEXT("CreationChance"), ESearchCase::IgnoreCase)) { C.CreationChance = CE.AsFloat(); }
					else if (CE.Key.Equals(TEXT("VeterancyLevel"), ESearchCase::IgnoreCase))
					{
						for (int32 i = 0; i < 4; ++i) { if (CE.Value().Equals(Levels[i], ESearchCase::IgnoreCase)) { C.VeterancyLevel = i; } }
					}
					else if (CE.Key.Equals(TEXT("KilledByType"), ESearchCase::IgnoreCase)) { for (const FString& V : CE.Values) { C.KilledByKindOf.Add(V.ToUpper()); } }
					else if (CE.Key.Equals(TEXT("KillerScience"), ESearchCase::IgnoreCase)) { C.KillerScience = CE.Value(); }
					else if (CE.Key.Equals(TEXT("OwnedByMaker"), ESearchCase::IgnoreCase)) { C.bOwnedByMaker = CE.AsBool(); }
					else if (CE.Key.Equals(TEXT("CrateObject"), ESearchCase::IgnoreCase) && CE.Values.Num() >= 2)
					{
						C.CrateObjects.Emplace(CE.Values[0], FCString::Atof(*CE.Values[1]));
					}
				}
			}
		}
	}

	// ThingTemplate inheritance from DefaultThingTemplate: its InheritableModule and OverrideableByLikeKind
	// blocks hold modules that every object gets.
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\Object.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("Object"), ESearchCase::IgnoreCase) || !B->Name().Equals(TEXT("DefaultThingTemplate"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			TSharedPtr<FGenIniBlock> Modules = MakeShared<FGenIniBlock>();
			Modules->Keyword = B->Keyword;
			Modules->Args = B->Args;
			for (const FGenIniEntry& E : B->Entries)
			{
				if (E.Block && (E.Key.Equals(TEXT("InheritableModule"), ESearchCase::IgnoreCase) || E.Key.Equals(TEXT("OverrideableByLikeKind"), ESearchCase::IgnoreCase)))
				{
					Modules->Entries.Append(E.Block->Entries);
				}
			}
			DefaultThing = BuildTemplate(B->Name(), Modules);
		}
	}

	for (const TPair<FString, TSharedPtr<FGenIniBlock>>& It : ObjectBlocks)
	{
		TSharedPtr<FGenThingTemplate> T = BuildTemplate(It.Value->Name(), It.Value);
		if (DefaultThing)
		{
			// OverrideableByLikeKind: only where the object has no module of the kind itself.
			if (!T->Stealth.bValid)
			{
				T->Stealth = DefaultThing->Stealth;
			}
			// InheritableModule (elite and heroic units mend themselves). One self-repair per object here,
			// so an object with an AutoHealBehavior of its own keeps just that.
			if (!T->AutoHeal.bValid && DefaultThing->AutoHeal.bValid)
			{
				T->AutoHeal = DefaultThing->AutoHeal;
				for (const FGenUpgradeModule& M : DefaultThing->UpgradeModules)
				{
					if (M.Effect == EGenUpgradeEffect::AutoHeal) { T->UpgradeModules.Add(M); }
				}
			}
		}
		Templates.Add(It.Key, T);
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
			T->bKindInfantry = T->KindOf.Contains(TEXT("INFANTRY"));
			T->bKindNoCollide = T->KindOf.Contains(TEXT("NO_COLLIDE"));
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
		else if (E.Key.Equals(TEXT("SelectPortrait"), ESearchCase::IgnoreCase)) { T->SelectPortrait = E.Value(0); }
		else if (E.Key.StartsWith(TEXT("UpgradeCameo"), ESearchCase::IgnoreCase) && E.Key.Len() == 13 && FChar::IsDigit(E.Key[12]))
		{
			const int32 Index = E.Key[12] - TEXT('1');
			if (Index >= 0 && Index < 5) { T->UpgradeCameos[Index] = E.Value(0); }
		}
		else if (E.Key.Equals(TEXT("BuildTime"), ESearchCase::IgnoreCase)) { T->BuildTime = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("GeometryIsSmall"), ESearchCase::IgnoreCase)) { T->bGeometryIsSmall = E.AsBool(); }
		else if (E.Key.Equals(TEXT("FenceWidth"), ESearchCase::IgnoreCase)) { T->FenceWidth = E.AsFloat(); }
		else if (E.Key.Equals(TEXT("FenceXOffset"), ESearchCase::IgnoreCase)) { T->FenceXOffset = E.AsFloat(); }
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
		else if (E.Key.Equals(TEXT("Buildable"), ESearchCase::IgnoreCase))
		{
			const FString V = E.Value(0);
			T->Buildable = V.Equals(TEXT("No"), ESearchCase::IgnoreCase) ? 1 : V.Equals(TEXT("Only_By_AI"), ESearchCase::IgnoreCase) ? 2
				: V.Equals(TEXT("Ignore_Prerequisites"), ESearchCase::IgnoreCase) ? 3 : 0;
		}
		else if (E.Key.Equals(TEXT("MaxSimultaneousOfType"), ESearchCase::IgnoreCase))
		{
			// A number, or the keyword for the lobby option (which this game has no screen for): unlimited.
			T->MaxSimultaneousOfType = E.Value(0).Equals(TEXT("DeterminedBySuperweaponRestriction"), ESearchCase::IgnoreCase) ? 0 : E.AsInt();
		}
		else if (E.Key.Equals(TEXT("MaxSimultaneousLinkKey"), ESearchCase::IgnoreCase)) { T->MaxSimultaneousLinkKey = E.Value(0); }
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
			// (ThingTemplate: a module of the object's own with the die interface takes the place of the
			// DestroyDie every object has from the default template.)
			if (Type.EndsWith(TEXT("Die"), ESearchCase::IgnoreCase) || Type.Contains(TEXT("SlowDeath"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("InstantDeathBehavior"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("StructureCollapseUpdate"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("StructureToppleUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHasOwnDieModule = true;
			}
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
			else if (Type.Equals(TEXT("FlammableUpdate"), ESearchCase::IgnoreCase))
			{
				FGenFlammable& F = T->Flammable;
				const FGenIniBlock& B = *E.Block;
				F.bValid = true;
				F.BurnedDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("BurnedDelay"), 0.f));
				F.AflameDuration = GenLogic::MsecToFrames(B.GetFloat(TEXT("AflameDuration"), 0.f));
				F.AflameDamageDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("AflameDamageDelay"), 0.f));
				F.AflameDamageAmount = B.GetFloat(TEXT("AflameDamageAmount"), 0.f);
				F.BurningSound = B.GetString(TEXT("BurningSoundName"));
				F.FlameDamageLimit = B.GetFloat(TEXT("FlameDamageLimit"), 20.f);
				F.FlameDamageExpiration = B.Find(TEXT("FlameDamageExpiration")) ? GenLogic::MsecToFrames(B.GetFloat(TEXT("FlameDamageExpiration"), 0.f))
					: GenLogic::GetLogicFPS() * 2;
			}
			else if (Type.Equals(TEXT("CrushDie"), ESearchCase::IgnoreCase))
			{
				T->bCrushDie = true;
				static const TCHAR* Which[3] = { TEXT("TotalCrushSound"), TEXT("BackEndCrushSound"), TEXT("FrontEndCrushSound") };
				for (int32 i = 0; i < 3; ++i)
				{
					T->CrushSounds[i] = E.Block->GetString(Which[i]);
					T->CrushSoundPercent[i] = E.Block->GetInt(*(FString(Which[i]) + TEXT("Percent")), 100);
				}
			}
			else if (Type.Equals(TEXT("StructureCollapseUpdate"), ESearchCase::IgnoreCase))
			{
				FGenStructureCollapse& C = T->Collapse;
				const FGenIniBlock& B = *E.Block;
				C = FGenStructureCollapse();
				C.bValid = true;
				C.MinCollapseDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MinCollapseDelay"), 0.f));
				C.MaxCollapseDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MaxCollapseDelay"), 0.f));
				if (B.Find(TEXT("MinBurstDelay"))) { C.MinBurstDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MinBurstDelay"), 0.f)); }
				if (B.Find(TEXT("MaxBurstDelay"))) { C.MaxBurstDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MaxBurstDelay"), 0.f)); }
				C.CollapseDamping = B.GetFloat(TEXT("CollapseDamping"), 0.f);
				C.MaxShudder = B.GetFloat(TEXT("MaxShudder"), 0.f);
				C.BigBurstFrequency = B.GetInt(TEXT("BigBurstFrequency"), 0);
				static const TCHAR* Phases[4] = { TEXT("INITIAL"), TEXT("DELAY"), TEXT("BURST"), TEXT("FINAL") };
				for (const FGenIniEntry& PE : B.Entries)
				{
					const bool bFX = PE.Key.Equals(TEXT("FXList"), ESearchCase::IgnoreCase), bOCL = PE.Key.Equals(TEXT("OCL"), ESearchCase::IgnoreCase);
					if (PE.Key.Equals(TEXT("DeathTypes"), ESearchCase::IgnoreCase)) { C.DeathTypes = PE.Values; }
					for (int32 Phase = 0; (bFX || bOCL) && Phase < 4 && PE.Values.Num() >= 2; ++Phase)
					{
						if (PE.Values[0].Equals(Phases[Phase], ESearchCase::IgnoreCase))
						{
							for (int32 v = 1; v < PE.Values.Num(); ++v) { (bFX ? C.FX[Phase] : C.OCL[Phase]).Add(PE.Values[v]); }
						}
					}
				}
			}
			else if (Type.Equals(TEXT("StructureToppleUpdate"), ESearchCase::IgnoreCase))
			{
				FGenStructureTopple& S = T->StructureTopple;
				const FGenIniBlock& B = *E.Block;
				S = FGenStructureTopple();
				S.bValid = true;
				S.MinToppleDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MinToppleDelay"), 0.f));
				S.MaxToppleDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MaxToppleDelay"), 0.f));
				S.MinBurstDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MinToppleBurstDelay"), 0.f));
				S.MaxBurstDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MaxToppleBurstDelay"), 0.f));
				S.StructuralIntegrity = B.GetFloat(TEXT("StructuralIntegrity"), 0.1f);
				S.StructuralDecay = B.GetFloat(TEXT("StructuralDecay"), 0.f);
				S.ToppleStartFX = B.GetString(TEXT("ToppleStartFX"));
				S.ToppleDelayFX = B.GetString(TEXT("ToppleDelayFX"));
				S.ToppleDoneFX = B.GetString(TEXT("ToppleDoneFX"));
				S.CrushingFX = B.GetString(TEXT("CrushingFX"));
				S.CrushingWeapon = B.GetString(TEXT("CrushingWeaponName"));
				static const TCHAR* Phases[3] = { TEXT("INITIAL"), TEXT("DELAY"), TEXT("FINAL") };
				for (const FGenIniEntry& PE : B.Entries)
				{
					if (PE.Key.Equals(TEXT("DeathTypes"), ESearchCase::IgnoreCase)) { S.DeathTypes = PE.Values; }
					else if (PE.Key.Equals(TEXT("AngleFX"), ESearchCase::IgnoreCase) && PE.Values.Num() >= 2)
					{
						S.AngleFX.Emplace(FMath::DegreesToRadians(FCString::Atof(*PE.Values[0])), PE.Values[1]);
					}
					else if (PE.Key.Equals(TEXT("OCL"), ESearchCase::IgnoreCase) && PE.Values.Num() >= 2)
					{
						for (int32 Phase = 0; Phase < 3; ++Phase)
						{
							if (PE.Values[0].Equals(Phases[Phase], ESearchCase::IgnoreCase))
							{
								for (int32 v = 1; v < PE.Values.Num(); ++v) { S.OCL[Phase].Add(PE.Values[v]); }
							}
						}
					}
				}
			}
			else if (Type.Equals(TEXT("ToppleUpdate"), ESearchCase::IgnoreCase))
			{
				FGenTopple& P = T->Topple;
				const FGenIniBlock& B = *E.Block;
				P = FGenTopple();
				P.bValid = true;
				P.ToppleFX = B.GetString(TEXT("ToppleFX"));
				P.BounceFX = B.GetString(TEXT("BounceFX"));
				P.StumpName = B.GetString(TEXT("StumpName"));
				P.bKillWhenStartToppling = B.GetBool(TEXT("KillWhenStartToppling"));
				P.bKillWhenToppled = B.GetBool(TEXT("KillWhenFinishedToppling"), true);
				P.bKillStumpWhenToppled = B.GetBool(TEXT("KillStumpWhenToppled"));
				P.bLeftOrRightOnly = B.GetBool(TEXT("ToppleLeftOrRightOnly"));
				P.bReorientToppledRubble = B.GetBool(TEXT("ReorientToppledRubble"));
				P.InitialVelocityPercent = ParsePercent(B.GetString(TEXT("InitialVelocityPercent"), TEXT("20%")));
				P.InitialAccelPercent = ParsePercent(B.GetString(TEXT("InitialAccelPercent"), TEXT("1%")));
				P.BounceVelocityPercent = ParsePercent(B.GetString(TEXT("BounceVelocityPercent"), TEXT("30%")));
			}
			else if (Type.Equals(TEXT("FireSpreadUpdate"), ESearchCase::IgnoreCase))
			{
				FGenFireSpread& F = T->FireSpread;
				const FGenIniBlock& B = *E.Block;
				F.bValid = true;
				F.OCLEmbers = B.GetString(TEXT("OCLEmbers"));
				F.MinDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MinSpreadDelay"), 0.f));
				F.MaxDelay = GenLogic::MsecToFrames(B.GetFloat(TEXT("MaxSpreadDelay"), 0.f));
				F.Range = B.GetFloat(TEXT("SpreadTryRange"), 0.f);
			}
			else if (Type.Equals(TEXT("CountermeasuresBehavior"), ESearchCase::IgnoreCase))
			{
				FGenCountermeasures& C = T->Countermeasures;
				const FGenIniBlock& B = *E.Block;
				C.bValid = true;
				C.FlareTemplate = B.GetString(TEXT("FlareTemplateName"));
				C.VolleySize = B.GetInt(TEXT("VolleySize"), 0);
				C.VolleyArcAngle = FMath::DegreesToRadians(B.GetFloat(TEXT("VolleyArcAngle"), 0.f));
				C.VolleyVelocityFactor = B.GetFloat(TEXT("VolleyVelocityFactor"), 1.f);
				C.FramesBetweenVolleys = GenLogic::MsecToFrames(B.GetFloat(TEXT("DelayBetweenVolleys"), 0.f));
				C.NumberOfVolleys = B.GetInt(TEXT("NumberOfVolleys"), 0);
				C.ReloadFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("ReloadTime"), 0.f));
				C.EvasionRate = ParsePercent(B.GetString(TEXT("EvasionRate"), TEXT("0%")));
				C.ReactionFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("ReactionLaunchLatency"), 0.f));
				C.MissileDecoyFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("MissileDecoyDelay"), 0.f));
			}
			else if (Type.Equals(TEXT("EjectPilotDie"), ESearchCase::IgnoreCase))
			{
				FGenDieModule& M = T->DieModules.AddDefaulted_GetRef();
				M.Type = EGenDieModule::EjectPilot;
				M.GroundOCL = E.Block->GetString(TEXT("GroundCreationList"));
				M.AirOCL = E.Block->GetString(TEXT("AirCreationList"));
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { M.DeathTypes = DT->Values; }
				if (const FGenIniEntry* VL = E.Block->Find(TEXT("VeterancyLevels"))) { M.VeterancyMask = ParseVeterancyMask(VL->Values); }
				if (const FGenIniEntry* ES = E.Block->Find(TEXT("ExemptStatus"))) { M.ExemptStatus = ES->Values; }
				if (const FGenIniEntry* RS = E.Block->Find(TEXT("RequiredStatus"))) { M.RequiredStatus = RS->Values; }
			}
			else if (Type.Equals(TEXT("VeterancyCrateCollide"), ESearchCase::IgnoreCase))
			{
				// (Only the pilot's: the crates that lie about on a map aren't ported.)
				if (E.Block->GetBool(TEXT("IsPilot")))
				{
					T->bPilot = true;
					T->bPilotAddsOwnVeterancy = E.Block->GetBool(TEXT("AddsOwnerVeterancy"));
					if (const FGenIniEntry* K = E.Block->Find(TEXT("RequiredKindOf"))) { for (const FString& V : K->Values) { T->PilotRequiredKindOf.Add(V.ToUpper()); } }
					if (const FGenIniEntry* K = E.Block->Find(TEXT("ForbiddenKindOf"))) { for (const FString& V : K->Values) { T->PilotForbiddenKindOf.Add(V.ToUpper()); } }
				}
			}
			else if (Type.Equals(TEXT("PilotFindVehicleUpdate"), ESearchCase::IgnoreCase))
			{
				T->PilotScanFrames = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ScanRate"), 0.f)));
				T->PilotScanRange = E.Block->GetFloat(TEXT("ScanRange"), 0.f);
				T->PilotMinHealth = E.Block->GetFloat(TEXT("MinHealth"), 0.5f);
			}
			else if (Type.Equals(TEXT("GrantUpgradeCreate"), ESearchCase::IgnoreCase))
			{
				T->GrantUpgrades.Add(E.Block->GetString(TEXT("UpgradeToGrant")));
			}
			else if (Type.Equals(TEXT("SpecialAbilityUpdate"), ESearchCase::IgnoreCase))
			{
				FGenUpgradeData::ParseAbility(*E.Block, T->Abilities.AddDefaulted_GetRef());
			}
			else if (Type.Equals(TEXT("StickyBombUpdate"), ESearchCase::IgnoreCase))
			{
				T->StickyBomb.bValid = true;
				T->StickyBomb.OffsetZ = E.Block->GetFloat(TEXT("OffsetZ"), 10.f);
				T->StickyBomb.GeometryWeapon = E.Block->GetString(TEXT("GeometryBasedDamageWeapon"));
				T->StickyBomb.GeometryFX = E.Block->GetString(TEXT("GeometryBasedDamageFX"));
			}
			else if (Type.Equals(TEXT("AutoDepositUpdate"), ESearchCase::IgnoreCase))
			{
				FGenUpgradeData::ParseAutoDeposit(*E.Block, T->AutoDeposit);
			}
			else if (Type.Equals(TEXT("TechBuildingBehavior"), ESearchCase::IgnoreCase))
			{
				T->bTechBuilding = true;
			}
			else if (Type.Equals(TEXT("OCLUpdate"), ESearchCase::IgnoreCase))
			{
				FGenOCLUpdate& U = T->OCLUpdate;
				U = FGenOCLUpdate();
				U.bValid = true;
				U.OCL = E.Block->GetString(TEXT("OCL"));
				U.MinDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MinDelay"), 0.f));
				U.MaxDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MaxDelay"), 0.f));
				U.bCreateAtEdge = E.Block->GetBool(TEXT("CreateAtEdge"));
				U.bFactionTriggered = E.Block->GetBool(TEXT("FactionTriggered"));
				for (const FGenIniEntry& F : E.Block->Entries)
				{
					// "FactionOCL = Faction:America OCL:OCL_ReinforcementPadUSAVehicle"
					if (F.Key.Equals(TEXT("FactionOCL"), ESearchCase::IgnoreCase))
					{
						FString Faction, Ocl;
						for (const FString& V : F.Values)
						{
							FString Key, Value;
							if (V.Split(TEXT(":"), &Key, &Value))
							{
								if (Key.Equals(TEXT("Faction"), ESearchCase::IgnoreCase)) { Faction = Value; }
								else if (Key.Equals(TEXT("OCL"), ESearchCase::IgnoreCase)) { Ocl = Value; }
							}
						}
						if (!Faction.IsEmpty()) { U.FactionOCL.Emplace(Faction, Ocl); }
					}
				}
			}
			else if (Type.Equals(TEXT("MoneyCrateCollide"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("SalvageCrateCollide"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("UnitCrateCollide"), ESearchCase::IgnoreCase))
			{
				FGenCrateCollide& C = T->Crate;
				C = FGenCrateCollide();
				C.Type = Type.StartsWith(TEXT("Money"), ESearchCase::IgnoreCase) ? EGenCrate::Money
					: (Type.StartsWith(TEXT("Salvage"), ESearchCase::IgnoreCase) ? EGenCrate::Salvage : EGenCrate::Unit);
				C.Money = E.Block->GetInt(TEXT("MoneyProvided"), 0);
				C.bBuildingPickup = E.Block->GetBool(TEXT("BuildingPickup"));
				C.bForbidOwnerPlayer = E.Block->GetBool(TEXT("ForbidOwnerPlayer"));
				C.bHumanOnly = E.Block->GetBool(TEXT("HumanOnly"));
				C.PickupScience = E.Block->GetString(TEXT("PickupScience"));
				C.ExecuteFX = E.Block->GetString(TEXT("ExecuteFX"));
				C.WeaponChance = ParsePercent(E.Block->GetString(TEXT("WeaponChance"), TEXT("100%")));
				C.LevelChance = ParsePercent(E.Block->GetString(TEXT("LevelChance"), TEXT("25%")));
				C.MinMoney = E.Block->GetInt(TEXT("MinMoney"), 25);
				C.MaxMoney = E.Block->GetInt(TEXT("MaxMoney"), 75);
				C.UnitName = E.Block->GetString(TEXT("UnitName"));
				C.UnitCount = E.Block->GetInt(TEXT("UnitCount"), 0);
				for (const FGenIniEntry& F : E.Block->Entries)
				{
					if (F.Key.Equals(TEXT("RequiredKindOf"), ESearchCase::IgnoreCase)) { for (const FString& V : F.Values) { C.RequiredKindOf.Add(V.ToUpper()); } }
					else if (F.Key.Equals(TEXT("ForbiddenKindOf"), ESearchCase::IgnoreCase)) { for (const FString& V : F.Values) { C.ForbiddenKindOf.Add(V.ToUpper()); } }
					else if (F.Key.Equals(TEXT("UpgradedBoost"), ESearchCase::IgnoreCase))
					{
						FString Upgrade;
						int32 Boost = 0;
						for (const FString& V : F.Values)
						{
							FString Key, Value;
							if (V.Split(TEXT(":"), &Key, &Value))
							{
								if (Key.Equals(TEXT("UpgradeType"), ESearchCase::IgnoreCase)) { Upgrade = Value; }
								else if (Key.Equals(TEXT("Boost"), ESearchCase::IgnoreCase)) { Boost = FCString::Atoi(*Value); }
							}
						}
						if (!Upgrade.IsEmpty()) { C.UpgradeBoost.Emplace(Upgrade, Boost); }
					}
				}
			}
			else if (Type.Equals(TEXT("KeepObjectDie"), ESearchCase::IgnoreCase))
			{
				T->bKeepObjectDie = true;
			}
			else if (Type.Equals(TEXT("CommandButtonHuntUpdate"), ESearchCase::IgnoreCase))
			{
				T->bButtonHunt = true;
				if (E.Block->Find(TEXT("ScanRate"))) { T->HuntScanFrames = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ScanRate"), 1000.f))); }
				T->HuntScanRange = E.Block->GetFloat(TEXT("ScanRange"), 9999.f);
			}
			else if (Type.Equals(TEXT("NeutronMissileUpdate"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenNeutronMissile& N = T->NeutronMissile;
				N.bValid = true;
				N.InitialDist = B.GetFloat(TEXT("DistanceToTravelBeforeTurning"), 0.f);
				N.MaxTurnRate = GenLogic::AngularVelocityPerFrame(B.GetFloat(TEXT("MaxTurnRate"), 999.f * 30.f * 180.f / PI));
				N.ForwardDamping = B.GetFloat(TEXT("ForwardDamping"), 0.f);
				N.RelativeSpeed = B.GetFloat(TEXT("RelativeSpeed"), 1.f);
				N.TargetFromDirectlyAbove = B.GetFloat(TEXT("TargetFromDirectlyAbove"), 0.f);
				N.LaunchFX = B.GetString(TEXT("LaunchFX"));
				N.IgnitionFX = B.GetString(TEXT("IgnitionFX"));
				N.SpecialAccelFactor = B.GetFloat(TEXT("SpecialAccelFactor"), 1.f);
				N.SpecialSpeedTime = GenLogic::MsecToFrames(B.GetFloat(TEXT("SpecialSpeedTime"), 0.f));
				N.SpecialSpeedHeight = B.GetFloat(TEXT("SpecialSpeedHeight"), 0.f);
			}
			else if (Type.Equals(TEXT("SpectreGunshipUpdate"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenSpectreGunship& G = T->Gunship;
				G.bValid = true;
				G.Power = B.GetString(TEXT("SpecialPowerTemplate"));
				G.GattlingTemplate = B.GetString(TEXT("GattlingTemplateName"));
				G.HowitzerWeapon = B.GetString(TEXT("HowitzerWeaponTemplate"));
				G.GattlingStrafeFX = B.GetString(TEXT("GattlingStrafeFXParticleSystem"));
				G.RandomOffsetForHowitzer = B.GetFloat(TEXT("RandomOffsetForHowitzer"), 20.f);
				G.TargetingReticleRadius = B.GetFloat(TEXT("TargetingReticleRadius"), 25.f);
				G.OrbitInsertionSlope = B.GetFloat(TEXT("OrbitInsertionSlope"), 0.7f);
				G.OrbitRadius = B.GetFloat(TEXT("GunshipOrbitRadius"), 250.f);
				G.HowitzerFiringRate = B.Find(TEXT("HowitzerFiringRate")) ? FMath::Max(1, GenLogic::MsecToFrames(B.GetFloat(TEXT("HowitzerFiringRate"), 0.f))) : 10;
				G.HowitzerFollowLag = GenLogic::MsecToFrames(B.GetFloat(TEXT("HowitzerFollowLag"), 0.f));
				G.StrafingIncrement = B.GetFloat(TEXT("StrafingIncrement"), 20.f);
				G.AttackAreaRadius = B.GetFloat(TEXT("AttackAreaRadius"), 200.f);
				const FGenIniEntry* AreaDecal = B.Find(TEXT("AttackAreaDecal"));
				const FGenIniEntry* ReticleDecal = B.Find(TEXT("TargetingReticleDecal"));
				G.AttackAreaDecal = FGenRadiusDecalTemplate::Parse(AreaDecal ? AreaDecal->Block.Get() : nullptr);
				G.TargetingReticleDecal = FGenRadiusDecalTemplate::Parse(ReticleDecal ? ReticleDecal->Block.Get() : nullptr);
				G.OrbitFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("OrbitTime"), 0.f));
			}
			else if (Type.Equals(TEXT("SpectreGunshipDeploymentUpdate"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenGunshipDeployment& G = T->GunshipDeployments.AddDefaulted_GetRef();
				G.Power = B.GetString(TEXT("SpecialPowerTemplate"));
				G.GunshipTemplate = B.GetString(TEXT("GunshipTemplateName"));
				G.RequiredScience = B.GetString(TEXT("RequiredScience"));
				G.CreateLocation = B.GetString(TEXT("CreateLocation"), TEXT("CREATE_AT_EDGE_FARTHEST_FROM_TARGET"));
			}
			else if (Type.Equals(TEXT("ParticleUplinkCannonUpdate"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenParticleCannon& C = T->ParticleCannon;
				auto Frames = [&B](const TCHAR* Key) { return GenLogic::MsecToFrames(B.GetFloat(Key, 0.f)); };
				C.bValid = true;
				C.Power = B.GetString(TEXT("SpecialPowerTemplate"));
				C.BeginChargeFrames = Frames(TEXT("BeginChargeTime"));
				C.RaiseAntennaFrames = Frames(TEXT("RaiseAntennaTime"));
				C.ReadyDelayFrames = Frames(TEXT("ReadyDelayTime"));
				C.TotalFiringFrames = Frames(TEXT("TotalFiringTime"));
				C.WidthGrowFrames = Frames(TEXT("WidthGrowTime"));
				C.BeamTravelFrames = Frames(TEXT("BeamTravelTime"));
				C.DamagePerSecond = B.GetFloat(TEXT("DamagePerSecond"), 0.f);
				C.TotalDamagePulses = FMath::Max(1, B.GetInt(TEXT("TotalDamagePulses"), 1));
				C.DamageType = B.GetString(TEXT("DamageType"), TEXT("PARTICLE_BEAM"));
				C.DeathType = B.GetString(TEXT("DeathType"), TEXT("NORMAL"));
				C.DamageRadiusScalar = B.GetFloat(TEXT("DamageRadiusScalar"), 1.f);
				C.RevealRange = B.GetFloat(TEXT("RevealRange"), 0.f);
				C.OuterEffectBone = B.GetString(TEXT("OuterEffectBoneName"));
				C.OuterEffectNumBones = B.GetInt(TEXT("OuterEffectNumBones"), 0);
				C.ConnectorBone = B.GetString(TEXT("ConnectorBoneName"));
				C.FireBone = B.GetString(TEXT("FireBoneName"));
				C.OuterNodesLight = B.GetString(TEXT("OuterNodesLightFlareParticleSystem"));
				C.OuterNodesMedium = B.GetString(TEXT("OuterNodesMediumFlareParticleSystem"));
				C.OuterNodesIntense = B.GetString(TEXT("OuterNodesIntenseFlareParticleSystem"));
				C.ConnectorMediumLaser = B.GetString(TEXT("ConnectorMediumLaserName"));
				C.ConnectorIntenseLaser = B.GetString(TEXT("ConnectorIntenseLaserName"));
				C.ConnectorMediumFlare = B.GetString(TEXT("ConnectorMediumFlare"));
				C.ConnectorIntenseFlare = B.GetString(TEXT("ConnectorIntenseFlare"));
				C.LaserBaseLight = B.GetString(TEXT("LaserBaseLightFlareParticleSystemName"));
				C.LaserBaseMedium = B.GetString(TEXT("LaserBaseMediumFlareParticleSystemName"));
				C.LaserBaseIntense = B.GetString(TEXT("LaserBaseIntenseFlareParticleSystemName"));
				C.ParticleBeamLaser = B.GetString(TEXT("ParticleBeamLaserName"));
				C.GroundHitFX = B.GetString(TEXT("GroundHitFX"));
				C.BeamLaunchFX = B.GetString(TEXT("BeamLaunchFX"));
				C.FramesBetweenLaunchFX = FMath::Max(1, Frames(TEXT("DelayBetweenLaunchFX")));
				C.TotalScorchMarks = FMath::Max(1, B.GetInt(TEXT("TotalScorchMarks"), 1));
				C.ScorchMarkScalar = B.GetFloat(TEXT("ScorchMarkScalar"), 1.f);
				C.SwathDistance = B.GetFloat(TEXT("SwathOfDeathDistance"), 0.f);
				C.SwathAmplitude = B.GetFloat(TEXT("SwathOfDeathAmplitude"), 0.f);
				C.ManualDrivingSpeed = B.GetFloat(TEXT("ManualDrivingSpeed"), 0.f);
				C.ManualFastDrivingSpeed = B.GetFloat(TEXT("ManualFastDrivingSpeed"), 0.f);
				C.DoubleClickFrames = Frames(TEXT("DoubleClickToFastDriveDelay"));
				C.RemnantObject = B.GetString(TEXT("DamagePulseRemnantObjectName"));
			}
			else if (Type.Equals(TEXT("MissileLauncherBuildingUpdate"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenMissileLauncherBuilding& M = T->MissileLauncher;
				M.bValid = true;
				M.Power = B.GetString(TEXT("SpecialPowerTemplate"));
				M.DoorOpenTime = GenLogic::MsecToFrames(B.GetFloat(TEXT("DoorOpenTime"), 0.f));
				M.DoorWaitOpenTime = GenLogic::MsecToFrames(B.GetFloat(TEXT("DoorWaitOpenTime"), 0.f));
				M.DoorCloseTime = GenLogic::MsecToFrames(B.GetFloat(TEXT("DoorCloseTime"), 0.f));
				M.OpeningFX = B.GetString(TEXT("DoorOpeningFX"));
				M.OpenFX = B.GetString(TEXT("DoorOpenFX"));
				M.WaitingToCloseFX = B.GetString(TEXT("DoorWaitingToCloseFX"));
				M.ClosingFX = B.GetString(TEXT("DoorClosingFX"));
				M.ClosedFX = B.GetString(TEXT("DoorClosedFX"));
			}
			else if (Type.Equals(TEXT("HeightDieUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHeightDie = true;
				T->HeightDieTarget = E.Block->GetFloat(TEXT("TargetHeight"), 0.f);
				T->bHeightDieOnlyWhenMovingDown = E.Block->GetBool(TEXT("OnlyWhenMovingDown"));
				T->HeightDieInitialDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("InitialDelay"), 0.f));
				T->bHeightDieSnapToGround = E.Block->GetBool(TEXT("SnapToGroundOnDeath"));
			}
			else if (Type.Equals(TEXT("FireWeaponWhenDeadBehavior"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FDeathWeapon& D = T->DeathWeapons.AddDefaulted_GetRef();
				D.Weapon = E.Block->GetString(TEXT("DeathWeapon"));
				D.bStartsActive = E.Block->GetBool(TEXT("StartsActive"));
				D.bRequiresAllTriggers = E.Block->GetBool(TEXT("RequiresAllTriggers"));
				for (const FGenIniEntry& Sub : E.Block->Entries)
				{
					if (Sub.Key.Equals(TEXT("TriggeredBy"), ESearchCase::IgnoreCase)) { D.TriggeredBy.Append(Sub.Values); }
					else if (Sub.Key.Equals(TEXT("ConflictsWith"), ESearchCase::IgnoreCase)) { D.ConflictsWith.Append(Sub.Values); }
					else if (Sub.Key.Equals(TEXT("DeathTypes"), ESearchCase::IgnoreCase)) { D.DeathTypes = Sub.Values; }
					else if (Sub.Key.Equals(TEXT("ExemptStatus"), ESearchCase::IgnoreCase)) { D.ExemptStatus = Sub.Values; }
					else if (Sub.Key.Equals(TEXT("RequiredStatus"), ESearchCase::IgnoreCase)) { D.RequiredStatus = Sub.Values; }
				}
			}
			else if (Type.Equals(TEXT("AnimationSteeringUpdate"), ESearchCase::IgnoreCase))
			{
				T->AnimationSteeringFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MinTransitionTime"), 0.f));
			}
			else if (Type.Equals(TEXT("StealthUpdate"), ESearchCase::IgnoreCase))
			{
				FGenStealthData& S = T->Stealth;
				S.bValid = true;
				if (const FGenIniEntry* Delay = E.Block->Find(TEXT("StealthDelay")))
				{
					S.StealthDelay = GenLogic::MsecToFrames(Delay->AsFloat());
				}
				if (const FGenIniEntry* Forbidden = E.Block->Find(TEXT("StealthForbiddenConditions")))
				{
					static const TCHAR* Names[] = { TEXT("ATTACKING"), TEXT("MOVING"), TEXT("USING_ABILITY"), TEXT("FIRING_PRIMARY"),
						TEXT("FIRING_SECONDARY"), TEXT("FIRING_TERTIARY"), TEXT("NO_BLACK_MARKET"), TEXT("TAKING_DAMAGE"), TEXT("RIDERS_ATTACKING") };
					for (const FString& V : Forbidden->Values)
					{
						for (int32 n = 0; n < UE_ARRAY_COUNT(Names); ++n)
						{
							if (V.Equals(Names[n], ESearchCase::IgnoreCase)) { S.Forbidden |= 1u << n; }
						}
					}
				}
				S.MoveThresholdSpeed = GenLogic::VelocityPerFrame(E.Block->GetFloat(TEXT("MoveThresholdSpeed"), 0.f));
				S.bInnateStealth = E.Block->GetBool(TEXT("InnateStealth"), true);
				if (const FGenIniEntry* Opacity = E.Block->Find(TEXT("FriendlyOpacityMin")))
				{
					S.FriendlyOpacityMin = FMath::Clamp(ParsePercent(Opacity->Value()), 0.f, 1.f);
				}
				S.bGrantedBySpecialPower = E.Block->GetBool(TEXT("GrantedBySpecialPower"));
				S.bDisguisesAsTeam = E.Block->GetBool(TEXT("DisguisesAsTeam"));
				S.DisguiseFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DisguiseTransitionTime"), 0.f));
				S.DisguiseRevealFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DisguiseRevealTransitionTime"), 0.f));
				S.DisguiseFX = E.Block->GetString(TEXT("DisguiseFX"));
				S.DisguiseRevealFX = E.Block->GetString(TEXT("DisguiseRevealFX"));
				S.bOrderIdleEnemiesToAttackMeUponReveal = E.Block->GetBool(TEXT("OrderIdleEnemiesToAttackMeUponReveal"));
				S.BlackMarketCheckFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("BlackMarketCheckDelay"), 0.f));
			}
			else if (Type.Equals(TEXT("StealthDetectorUpdate"), ESearchCase::IgnoreCase))
			{
				FGenStealthDetector& D = T->Detector;
				D.bValid = true;
				D.Rate = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DetectionRate"), 0.f)));
				D.Range = E.Block->GetFloat(TEXT("DetectionRange"), 0.f);
				D.bInitiallyDisabled = E.Block->GetBool(TEXT("InitiallyDisabled"));
				D.bWhileGarrisoned = E.Block->GetBool(TEXT("CanDetectWhileGarrisoned"));
				D.bWhileContained = E.Block->GetBool(TEXT("CanDetectWhileContained"));
				D.PingSound = E.Block->GetString(TEXT("PingSound"));
				D.LoudPingSound = E.Block->GetString(TEXT("LoudPingSound"));
				D.IRParticleSys = E.Block->GetString(TEXT("IRParticleSysName"));
				D.IRBrightParticleSys = E.Block->GetString(TEXT("IRBrightParticleSysName"));
				D.IRBeaconParticleSys = E.Block->GetString(TEXT("IRBeaconParticleSysName"));
				D.IRParticleSysBone = E.Block->GetString(TEXT("IRParticleSysBone"));
				if (const FGenIniEntry* Kinds = E.Block->Find(TEXT("ExtraRequiredKindOf"))) { D.RequiredKindOf = Kinds->Values; }
				if (const FGenIniEntry* Kinds = E.Block->Find(TEXT("ExtraForbiddenKindOf"))) { D.ForbiddenKindOf = Kinds->Values; }
			}
			else if (Type.Equals(TEXT("PointDefenseLaserUpdate"), ESearchCase::IgnoreCase))
			{
				FGenPointDefenseLaser& P = T->PointDefenseLasers.AddDefaulted_GetRef();
				P.Weapon = E.Block->GetString(TEXT("WeaponTemplate"));
				if (const FGenIniEntry* K = E.Block->Find(TEXT("PrimaryTargetTypes"))) { P.PrimaryKinds = K->Values; }
				if (const FGenIniEntry* K = E.Block->Find(TEXT("SecondaryTargetTypes"))) { P.SecondaryKinds = K->Values; }
				P.ScanFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ScanRate"), 0.f));
				P.ScanRange = E.Block->GetFloat(TEXT("ScanRange"), 0.f);
				P.VelocityFactor = E.Block->GetFloat(TEXT("PredictTargetVelocityFactor"), 0.f);
			}
			else if (Type.Equals(TEXT("BattlePlanUpdate"), ESearchCase::IgnoreCase))
			{
				FGenBattlePlanData& P = T->BattlePlan;
				const FGenIniBlock& B = *E.Block;
				P.bValid = true;
				P.Power = B.GetString(TEXT("SpecialPowerTemplate"));
				static const TCHAR* const Plans[3] = { TEXT("Bombardment"), TEXT("HoldTheLine"), TEXT("SearchAndDestroy") };
				for (int32 i = 0; i < 3; ++i)
				{
					const FString Plan = Plans[i];
					P.PlanFrames[i] = GenLogic::MsecToFrames(B.GetFloat(*(Plan + TEXT("PlanAnimationTime")), 0.f));
					P.MessageLabel[i] = B.GetString(*(Plan + TEXT("MessageLabel")));
					P.UnpackSound[i] = B.GetString(*(Plan + TEXT("PlanUnpackSoundName")));
					P.PackSound[i] = B.GetString(*(Plan + TEXT("PlanPackSoundName")));
					P.Announcement[i] = B.GetString(*(Plan + TEXT("AnnouncementName")));
				}
				P.TransitionIdleFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("TransitionIdleTime"), 0.f));
				P.SearchAndDestroyIdleSound = B.GetString(TEXT("SearchAndDestroyPlanIdleLoopSoundName"));
				if (const FGenIniEntry* Kinds = B.Find(TEXT("ValidMemberKindOf"))) { P.ValidKindOf = Kinds->Values; }
				if (const FGenIniEntry* Kinds = B.Find(TEXT("InvalidMemberKindOf"))) { P.InvalidKindOf = Kinds->Values; }
				P.ParalyzeFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("BattlePlanChangeParalyzeTime"), 0.f));
				P.HoldTheLineArmorScalar = B.GetFloat(TEXT("HoldTheLinePlanArmorDamageScalar"), 1.f);
				P.SearchAndDestroySightScalar = B.GetFloat(TEXT("SearchAndDestroyPlanSightRangeScalar"), 1.f);
				P.CenterSightScalar = B.GetFloat(TEXT("StrategyCenterSearchAndDestroySightRangeScalar"), 1.f);
				P.bCenterDetectsStealth = B.GetBool(TEXT("StrategyCenterSearchAndDestroyDetectsStealth"));
				P.CenterMaxHealthScalar = B.GetFloat(TEXT("StrategyCenterHoldTheLineMaxHealthScalar"), 1.f);
				P.CenterMaxHealthChangeType = B.GetString(TEXT("StrategyCenterHoldTheLineMaxHealthChangeType"), TEXT("SAME_CURRENTHEALTH")).ToUpper();
			}
			else if (Type.Equals(TEXT("GrantStealthBehavior"), ESearchCase::IgnoreCase))
			{
				FGenGrantStealth& G = T->GrantStealth;
				G.bValid = true;
				G.StartRadius = E.Block->GetFloat(TEXT("StartRadius"), 0.f);
				G.FinalRadius = E.Block->GetFloat(TEXT("FinalRadius"), 0.f);
				G.GrowRate = E.Block->GetFloat(TEXT("RadiusGrowRate"), 0.f);
				G.RadiusParticleSystem = E.Block->GetString(TEXT("RadiusParticleSystemName"));
				if (const FGenIniEntry* Kinds = E.Block->Find(TEXT("KindOf"))) { G.KindOf = Kinds->Values; }
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
			else if (Type.Equals(TEXT("MinefieldBehavior"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenMinefield& M = T->Minefield;
				M.bValid = true;
				M.DetonationWeapon = B.GetString(TEXT("DetonationWeapon"));
				if (const FGenIniEntry* By = B.Find(TEXT("DetonatedBy")))
				{
					M.DetonatedByMask = 0;
					for (const FString& Who : By->Values)
					{
						M.DetonatedByMask |= Who.Equals(TEXT("ENEMIES"), ESearchCase::IgnoreCase) ? 1 : Who.Equals(TEXT("ALLIES"), ESearchCase::IgnoreCase) ? 2
							: Who.Equals(TEXT("NEUTRAL"), ESearchCase::IgnoreCase) ? 4 : 0;
					}
				}
				M.bStopsRegenAfterCreatorDies = B.GetBool(TEXT("StopsRegenAfterCreatorDies"), true);
				M.bRegenerates = B.GetBool(TEXT("Regenerates"));
				M.bWorkersDetonate = B.GetBool(TEXT("WorkersDetonate"));
				M.CreatorDeathCheckFrames = FMath::Max(1, GenLogic::MsecToFrames(B.GetFloat(TEXT("CreatorDeathCheckRate"), 1000.f)));
				M.ScootFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("ScootFromStartingPointTime"), 0.f));
				M.RepeatDetonateMoveThresh = B.GetFloat(TEXT("RepeatDetonateMoveThresh"), 1.f);
				M.NumVirtualMines = FMath::Max(1, B.GetInt(TEXT("NumVirtualMines"), 1));
				M.DrainPerSecond = B.GetFloat(TEXT("DegenPercentPerSecondAfterCreatorDies"), 0.f) / 100.f;
				M.CreationList = B.GetString(TEXT("CreationList"));
			}
			else if (Type.Equals(TEXT("GenerateMinefieldBehavior"), ESearchCase::IgnoreCase))
			{
				const FGenIniBlock& B = *E.Block;
				FGenGenerateMinefield& G = T->GenerateMinefield;
				G.bValid = true;
				G.MineName = B.GetString(TEXT("MineName"));
				G.UpgradedMineName = B.GetString(TEXT("UpgradedMineName"));
				G.UpgradedTriggeredBy = B.GetString(TEXT("UpgradedTriggeredBy"));
				G.GenerationFX = B.GetString(TEXT("GenerationFX"));
				G.DistanceAroundObject = B.GetFloat(TEXT("DistanceAroundObject"), -1.f);
				G.MinesPerSquareFoot = B.GetFloat(TEXT("MinesPerSquareFoot"), -1.f);
				G.bOnDeath = B.GetBool(TEXT("GenerateOnlyOnDeath"));
				G.bBorderOnly = B.GetBool(TEXT("BorderOnly"), true);
				G.bSmartBorder = B.GetBool(TEXT("SmartBorder"));
				G.bSmartBorderSkipInterior = B.GetBool(TEXT("SmartBorderSkipInterior"), true);
				G.bAlwaysCircular = B.GetBool(TEXT("AlwaysCircular"));
				G.bUpgradable = B.GetBool(TEXT("Upgradable"));
				G.RandomJitter = B.GetFloat(TEXT("RandomJitter"), 0.f) / 100.f;
				if (B.Find(TEXT("SkipIfThisMuchUnderStructure"))) { G.SkipIfThisMuchUnderStructure = B.GetFloat(TEXT("SkipIfThisMuchUnderStructure"), 33.f) / 100.f; }
			}
			else if (Type.Equals(TEXT("MobMemberSlavedUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FMobMember& D = T->MobMember;
				D.bValid = true;
				D.MustCatchUpRadius = float(E.Block->GetInt(TEXT("MustCatchUpRadius"), 0));
				D.NoNeedToCatchUpRadius = float(E.Block->GetInt(TEXT("NoNeedToCatchUpRadius"), 0));
				// onObjectCreated: "MIN(MAX_SQUIRRELLINESS, MAX(0, ...))"
				D.Squirrelliness = FMath::Clamp(E.Block->GetFloat(TEXT("Squirrelliness"), 0.f), 0.f, 1.f);
				D.CatchUpCrisisBailTime = E.Block->GetInt(TEXT("CatchUpCrisisBailTime"), 0);
			}
			else if (Type.Equals(TEXT("SlavedUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FSlaved& D = T->Slaved;
				const FGenIniBlock& B = *E.Block;
				D.bValid = true;
				D.GuardMaxRange = B.GetInt(TEXT("GuardMaxRange"), 0);
				D.GuardWanderRange = B.GetInt(TEXT("GuardWanderRange"), 0);
				D.AttackRange = B.GetInt(TEXT("AttackRange"), 0);
				D.AttackWanderRange = B.GetInt(TEXT("AttackWanderRange"), 0);
				D.ScoutRange = B.GetInt(TEXT("ScoutRange"), 0);
				D.ScoutWanderRange = B.GetInt(TEXT("ScoutWanderRange"), 0);
				D.RepairRange = B.GetInt(TEXT("RepairRange"), 0);
				D.DistToTargetToGrantRangeBonus = B.GetInt(TEXT("DistToTargetToGrantRangeBonus"), 0);
				D.RepairMinAltitude = B.GetFloat(TEXT("RepairMinAltitude"), 0.f);
				D.RepairMaxAltitude = B.GetFloat(TEXT("RepairMaxAltitude"), 0.f);
				D.RepairRatePerSecond = B.GetFloat(TEXT("RepairRatePerSecond"), 0.f);
				D.RepairWhenBelowPercent = B.GetInt(TEXT("RepairWhenBelowHealth%"), 0);
				D.MinReadyFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("RepairMinReadyTime"), 0.f));
				D.MaxReadyFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("RepairMaxReadyTime"), 0.f));
				D.MinWeldFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("RepairMinWeldTime"), 0.f));
				D.MaxWeldFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("RepairMaxWeldTime"), 0.f));
				D.WeldingSys = B.GetString(TEXT("RepairWeldingSys"));
				D.WeldingFXBone = B.GetString(TEXT("RepairWeldingFXBone"));
				D.bStayOnSameLayerAsMaster = B.GetBool(TEXT("StayOnSameLayerAsMaster"));
			}
			else if (Type.StartsWith(TEXT("Sabotage"), ESearchCase::IgnoreCase) && Type.EndsWith(TEXT("CrateCollide"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FSabotage& D = T->Sabotage;
				const FGenIniBlock& B = *E.Block;
				const FString Kind = Type.Mid(8, Type.Len() - 8 - 12);
				if (Kind.Equals(TEXT("PowerPlant"), ESearchCase::IgnoreCase)) { D.PowerFrames = FMath::Max(1, GenLogic::MsecToFrames(B.GetFloat(TEXT("SabotagePowerDuration"), 0.f))); }
				else if (Kind.Equals(TEXT("Superweapon"), ESearchCase::IgnoreCase)) { D.bSuperweapon = true; }
				else if (Kind.Equals(TEXT("CommandCenter"), ESearchCase::IgnoreCase)) { D.bCommandCenter = true; }
				else if (Kind.Equals(TEXT("SupplyCenter"), ESearchCase::IgnoreCase)) { D.StealCash = FMath::Max(1, B.GetInt(TEXT("StealCashAmount"), 0)); }
				else if (Kind.Equals(TEXT("MilitaryFactory"), ESearchCase::IgnoreCase)) { D.FactoryFrames = FMath::Max(1, GenLogic::MsecToFrames(B.GetFloat(TEXT("SabotageDuration"), 0.f))); }
				else if (Kind.Equals(TEXT("FakeBuilding"), ESearchCase::IgnoreCase)) { D.bFakeBuilding = true; }
				else if (Kind.Equals(TEXT("InternetCenter"), ESearchCase::IgnoreCase)) { D.InternetFrames = FMath::Max(1, GenLogic::MsecToFrames(B.GetFloat(TEXT("SabotageDuration"), 0.f))); }
			}
			else if (Type.Equals(TEXT("ConvertToHijackedVehicleCrateCollide"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("ConvertToCarBombCrateCollide"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FVehicleTakeover& D = T->VehicleTakeover;
				D = FGenThingTemplate::FVehicleTakeover();
				D.Kind = Type.StartsWith(TEXT("ConvertToHijacked"), ESearchCase::IgnoreCase) ? FGenThingTemplate::FVehicleTakeover::Hijack
					: FGenThingTemplate::FVehicleTakeover::CarBomb;
				for (const FGenIniEntry& CE : E.Block->Entries)
				{
					if (CE.Key.Equals(TEXT("RequiredKindOf"), ESearchCase::IgnoreCase)) { for (const FString& V : CE.Values) { D.RequiredKindOf.Add(V.ToUpper()); } }
					else if (CE.Key.Equals(TEXT("ForbiddenKindOf"), ESearchCase::IgnoreCase)) { for (const FString& V : CE.Values) { if (!V.Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { D.ForbiddenKindOf.Add(V.ToUpper()); } } }
					else if (CE.Key.Equals(TEXT("FXList"), ESearchCase::IgnoreCase)) { D.FX = CE.Value(); }
				}
			}
			else if (Type.Equals(TEXT("DemoTrapUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FDemoTrap& D = T->DemoTrap;
				const FGenIniBlock& B = *E.Block;
				D.bValid = true;
				D.bDefaultProximityMode = B.GetBool(TEXT("DefaultProximityMode"));
				D.DetonationSlot = WeaponSlotIndex(B.GetString(TEXT("DetonationWeaponSlot"), TEXT("PRIMARY")));
				D.ProximitySlot = WeaponSlotIndex(B.GetString(TEXT("ProximityModeWeaponSlot"), TEXT("SECONDARY")));
				D.ManualSlot = WeaponSlotIndex(B.GetString(TEXT("ManualModeWeaponSlot"), TEXT("TERTIARY")));
				D.TriggerRange = B.GetFloat(TEXT("TriggerDetonationRange"), 0.f);
				if (const FGenIniEntry* K = B.Find(TEXT("IgnoreTargetTypes"))) { for (const FString& V : K->Values) { D.IgnoreKindOf.Add(V.ToUpper()); } }
				D.ScanFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("ScanRate"), 0.f));
				D.bFriendlyDetonation = B.GetBool(TEXT("AutoDetonationWithFriendsInvolved"));
				D.DetonationWeapon = B.GetString(TEXT("DetonationWeapon"));
				D.bDetonateWhenKilled = B.GetBool(TEXT("DetonateWhenKilled"));
			}
			else if (Type.Equals(TEXT("HijackerUpdate"), ESearchCase::IgnoreCase))
			{
				T->HijackerParachute = E.Block->GetString(TEXT("ParachuteName"));
			}
			else if (Type.Equals(TEXT("UpgradeDie"), ESearchCase::IgnoreCase))
			{
				// ("UpgradeToRemove = Upgrade_AmericaBattleDrone ModuleTag_13": the upgrade, and the module it resets.)
				if (const FGenIniEntry* U = E.Block->Find(TEXT("UpgradeToRemove"))) { T->UpgradeDieRemove = U->Value(0); }
			}
			else if (Type.Equals(TEXT("PropagandaTowerBehavior"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FPropagandaTower& P = T->Propaganda;
				P.bValid = true;
				P.Radius = E.Block->GetFloat(TEXT("Radius"), 1.f);
				P.Delay = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DelayBetweenUpdates"), 100.f * 1000.f / 30.f)));
				P.HealPercent = ParsePercent(E.Block->GetString(TEXT("HealPercentEachSecond"), TEXT("1%")));
				P.UpgradedHealPercent = ParsePercent(E.Block->GetString(TEXT("UpgradedHealPercentEachSecond"), TEXT("2%")));
				P.PulseFX = E.Block->GetString(TEXT("PulseFX"));
				P.UpgradedPulseFX = E.Block->GetString(TEXT("UpgradedPulseFX"));
				P.UpgradeRequired = E.Block->GetString(TEXT("UpgradeRequired"));
				P.bAffectsSelf = E.Block->GetBool(TEXT("AffectsSelf"));
			}
			else if (Type.Equals(TEXT("WeaponBonusUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FWeaponBonusPulse& P = T->WeaponBonusPulse;
				P.bValid = true;
				if (const FGenIniEntry* K = E.Block->Find(TEXT("RequiredAffectKindOf"))) { P.RequiredKindOf = K->Values; }
				if (const FGenIniEntry* K = E.Block->Find(TEXT("ForbiddenAffectKindOf"))) { P.ForbiddenKindOf = K->Values; }
				P.Duration = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("BonusDuration"), 0.f));
				P.Delay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("BonusDelay"), 0.f));
				P.Range = E.Block->GetFloat(TEXT("BonusRange"), 0.f);
				P.Condition = E.Block->GetString(TEXT("BonusConditionType"));
			}
			else if (Type.Equals(TEXT("LeafletDropBehavior"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FLeafletDrop& L = T->LeafletDrop;
				L.bValid = true;
				L.DisabledDuration = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DisabledDuration"), 0.f));
				L.Delay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("Delay"), 0.f));
				L.Radius = E.Block->GetFloat(TEXT("AffectRadius"), 0.f);
				L.ParticleSystem = E.Block->GetString(TEXT("LeafletFXParticleSystem"));
			}
			else if (Type.Equals(TEXT("PoisonedBehavior"), ESearchCase::IgnoreCase))
			{
				T->Poisoned.bValid = true;
				T->Poisoned.IntervalFrames = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("PoisonDamageInterval"), 0.f)));
				T->Poisoned.DurationFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("PoisonDuration"), 0.f));
			}
			else if (Type.Equals(TEXT("RailroadBehavior"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FRailroad& R = T->Railroad;
				const FGenIniBlock& B = *E.Block;
				R.bValid = true;
				R.bIsLocomotive = B.GetBool(TEXT("IsLocomotive"));
				R.RunningGarrisonSpeedMax = B.GetFloat(TEXT("RunningGarrisonSpeedMax"), 1.f);
				R.KillSpeedMin = B.GetFloat(TEXT("KillSpeedMin"), 1.f);
				R.SpeedMax = B.GetFloat(TEXT("SpeedMax"), 4.f);
				R.Acceleration = B.GetFloat(TEXT("Acceleration"), 1.01f);
				R.Braking = B.GetFloat(TEXT("Braking"), 0.99f);
				R.Friction = B.GetFloat(TEXT("Friction"), 0.97f);
				R.WaitAtStationFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("WaitAtStationTime"), 5000.f));
				R.RunningSound = B.GetString(TEXT("RunningSound"));
				R.ClicketyClackSound = B.GetString(TEXT("ClicketyClackSound"));
				R.WhistleSound = B.GetString(TEXT("WhistleSound"));
				R.BigMetalBounceSound = B.GetString(TEXT("BigMetalBounceSound"));
				R.SmallMetalBounceSound = B.GetString(TEXT("SmallMetalBounceSound"));
				R.MeatyBounceSound = B.GetString(TEXT("MeatyBounceSound"));
				for (const FGenIniEntry& Sub : B.Entries)
				{
					if (Sub.Key.Equals(TEXT("CarriageTemplateName"), ESearchCase::IgnoreCase)) { R.CarriageTemplates.Append(Sub.Values); }
				}
			}
			else if (Type.Equals(TEXT("FireOCLAfterWeaponCooldownUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FFireOCLAfterCooldown& F = T->FireOCLAfterCooldown.AddDefaulted_GetRef();
				const FString SlotName = E.Block->GetString(TEXT("WeaponSlot"), TEXT("PRIMARY"));
				F.Slot = SlotName.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 1 : SlotName.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 2 : 0;
				F.OCL = E.Block->GetString(TEXT("OCL"));
				F.MinShots = E.Block->GetInt(TEXT("MinShotsToCreateOCL"), 1);
				F.LifetimeMsPerSecond = E.Block->GetFloat(TEXT("OCLLifetimePerSecond"), 1000.f);
				F.MaxFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("OCLLifetimeMaxCap"), 1000.f * 1000.f / 30.f));
				F.bRequiresAllTriggers = E.Block->GetBool(TEXT("RequiresAllTriggers"));
				for (const FGenIniEntry& Sub : E.Block->Entries)
				{
					if (Sub.Key.Equals(TEXT("TriggeredBy"), ESearchCase::IgnoreCase)) { F.TriggeredBy.Append(Sub.Values); }
					else if (Sub.Key.Equals(TEXT("ConflictsWith"), ESearchCase::IgnoreCase)) { F.ConflictsWith.Append(Sub.Values); }
				}
			}
			else if (Type.Equals(TEXT("AssistedTargetingUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FAssistedTargeting& A = T->AssistedTargeting;
				A.bValid = true;
				A.ClipSize = E.Block->GetInt(TEXT("AssistingClipSize"), 0);
				const FString SlotName = E.Block->GetString(TEXT("AssistingWeaponSlot"), TEXT("SECONDARY"));
				A.Slot = SlotName.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 1 : SlotName.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 2 : 0;
				A.LaserFromAssisted = E.Block->GetString(TEXT("LaserFromAssisted"));
				A.LaserToTarget = E.Block->GetString(TEXT("LaserToTarget"));
			}
			else if (Type.Equals(TEXT("ProjectileStreamUpdate"), ESearchCase::IgnoreCase))
			{
				T->bProjectileStream = true;
			}
			else if (Type.Equals(TEXT("FloatUpdate"), ESearchCase::IgnoreCase))
			{
				T->bFloat = true;
				T->bFloatLifts = E.Block->GetBool(TEXT("Enabled"), false);
			}
			else if (Type.Equals(TEXT("BoneFXUpdate"), ESearchCase::IgnoreCase))
			{
				// "<BodyDamageState><FXList|OCL|ParticleSystem><index> = Bone:<BoneName> OnlyOnce:<Yes|No> <Min delay>
				// <Max delay> <FXList|OCL|PSys>:<Name>". (Its DamageFXTypes and the like - "ALL -WATER" wherever they
				// are given - aren't looked at.)
				static const TCHAR* const States[] = { TEXT("ReallyDamaged"), TEXT("Damaged"), TEXT("Pristine"), TEXT("Rubble") };
				static const int32 StateIndex[] = { 2, 1, 0, 3 };
				for (const FGenIniEntry& BE : E.Block->Entries)
				{
					FGenThingTemplate::FBoneFX X;
					FString Rest;
					int32 s = 0;
					for (; s < 4; ++s)
					{
						if (BE.Key.StartsWith(States[s], ESearchCase::IgnoreCase))
						{
							X.State = StateIndex[s];
							Rest = BE.Key.Mid(FCString::Strlen(States[s]));
							break;
						}
					}
					if (s == 4) { continue; }
					if (Rest.StartsWith(TEXT("FXList"), ESearchCase::IgnoreCase)) { X.Kind = FGenThingTemplate::FBoneFX::FXList; }
					else if (Rest.StartsWith(TEXT("OCL"), ESearchCase::IgnoreCase)) { X.Kind = FGenThingTemplate::FBoneFX::OCL; }
					else if (Rest.StartsWith(TEXT("ParticleSystem"), ESearchCase::IgnoreCase)) { X.Kind = FGenThingTemplate::FBoneFX::ParticleSystem; }
					else { continue; }
					TArray<FString> Tok;
					GenIni::Tokenize(BE.Raw, Tok, TEXT(" \t=:"));
					if (Tok.Num() < 8 || !Tok[0].Equals(TEXT("bone"), ESearchCase::IgnoreCase))
					{
						continue;
					}
					X.Bone = Tok[1];
					X.bOnlyOnce = Tok[3].Equals(TEXT("Yes"), ESearchCase::IgnoreCase);
					X.MinDelay = GenLogic::MsecToFrames(FCString::Atof(*Tok[4]));
					X.MaxDelay = GenLogic::MsecToFrames(FCString::Atof(*Tok[5]));
					X.Name = Tok[7];
					T->BoneFX.Add(MoveTemp(X));
				}
			}
			else if (Type.Equals(TEXT("CleanupHazardUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FCleanupHazard& C = T->CleanupHazard;
				C.bValid = true;
				const FString Slot = E.Block->GetString(TEXT("WeaponSlot"), TEXT("PRIMARY"));
				C.WeaponSlot = Slot.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 1 : Slot.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 2 : 0;
				C.ScanFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ScanRate"), 0.f));
				C.ScanRange = E.Block->GetFloat(TEXT("ScanRange"), 0.f);
			}
			else if (Type.Equals(TEXT("AutoFindHealingUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FAutoFindHealing& H = T->AutoFindHealing;
				H.bValid = true;
				H.ScanFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ScanRate"), 0.f));
				H.ScanRange = E.Block->GetFloat(TEXT("ScanRange"), 0.f);
				H.NeverHeal = E.Block->GetFloat(TEXT("NeverHeal"), 0.95f);
				H.AlwaysHeal = E.Block->GetFloat(TEXT("AlwaysHeal"), 0.25f);
			}
			else if (Type.Equals(TEXT("LockWeaponCreate"), ESearchCase::IgnoreCase))
			{
				const FString SlotName = E.Block->GetString(TEXT("SlotToLock"), TEXT("PRIMARY"));
				T->LockWeaponSlot = SlotName.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 1 : SlotName.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 2 : 0;
			}
			else if (Type.Equals(TEXT("RebuildHoleExposeDie"), ESearchCase::IgnoreCase))
			{
				FGenDieModule& M = T->DieModules.AddDefaulted_GetRef();
				M.Type = EGenDieModule::RebuildHoleExpose;
				M.HoleName = E.Block->GetString(TEXT("HoleName"));
				M.HoleMaxHealth = E.Block->GetFloat(TEXT("HoleMaxHealth"), 0.f);
				M.bTransferAttackers = E.Block->GetBool(TEXT("TransferAttackers"), true);
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { M.DeathTypes = DT->Values; }
				if (const FGenIniEntry* VL = E.Block->Find(TEXT("VeterancyLevels"))) { M.VeterancyMask = ParseVeterancyMask(VL->Values); }
				if (const FGenIniEntry* ES = E.Block->Find(TEXT("ExemptStatus"))) { M.ExemptStatus = ES->Values; }
				if (const FGenIniEntry* RS = E.Block->Find(TEXT("RequiredStatus"))) { M.RequiredStatus = RS->Values; }
			}
			else if (Type.Equals(TEXT("RebuildHoleBehavior"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FRebuildHole& R = T->RebuildHole;
				R.bValid = true;
				R.WorkerName = E.Block->GetString(TEXT("WorkerObjectName"));
				R.WorkerRespawnFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("WorkerRespawnDelay"), 0.f));
				if (const FGenIniEntry* P = E.Block->Find(TEXT("HoleHealthRegen%PerSecond"))) { R.RegenPercentPerSecond = ParsePercent(P->Value()); }
			}
			else if (Type.Equals(TEXT("BaseRegenerateUpdate"), ESearchCase::IgnoreCase))
			{
				T->bBaseRegenerate = true;
			}
			else if (Type.Equals(TEXT("HordeUpdate"), ESearchCase::IgnoreCase))
			{
				FGenThingTemplate::FHorde& H = T->Horde;
				H.bValid = true;
				H.UpdateRate = FMath::Max(1, GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("UpdateRate"), 1000.f)));
				if (const FGenIniEntry* K = E.Block->Find(TEXT("KindOf")))
				{
					for (const FString& V : K->Values) { H.KindOf.Add(V.ToUpper()); }
				}
				H.Count = E.Block->GetInt(TEXT("Count"), 0);
				H.Radius = E.Block->GetFloat(TEXT("Radius"), 0.f);
				H.RubOffRadius = E.Block->GetFloat(TEXT("RubOffRadius"), 20.f);
				H.bAlliesOnly = E.Block->GetBool(TEXT("AlliesOnly"), true);
				H.bExactMatch = E.Block->GetBool(TEXT("ExactMatch"));
				H.bAllowedNationalism = E.Block->GetBool(TEXT("AllowedNationalism"), true);
			}
			else if (Type.Equals(TEXT("DeletionUpdate"), ESearchCase::IgnoreCase))
			{
				T->bHasDeletion = true;
				T->DeletionMin = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MinLifetime"), 0.f));
				T->DeletionMax = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("MaxLifetime"), 0.f));
			}
			else if (Type.Equals(TEXT("TransportContain"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("HelixContain"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("OverlordContain"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("RiderChangeContain"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("GarrisonContain"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("HealContain"), ESearchCase::IgnoreCase)
				|| Type.Equals(TEXT("TunnelContain"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("InternetHackContain"), ESearchCase::IgnoreCase))
			{
				// OpenContain and the modules built on it.
				FGenContainData& C = T->Contain;
				const FGenIniBlock& B = *E.Block;
				C.Type = Type.StartsWith(TEXT("Garrison"), ESearchCase::IgnoreCase) ? EGenContainType::Garrison
					: Type.StartsWith(TEXT("Heal"), ESearchCase::IgnoreCase) ? EGenContainType::Heal
					: Type.StartsWith(TEXT("Tunnel"), ESearchCase::IgnoreCase) ? EGenContainType::Tunnel : EGenContainType::Transport;
				C.ContainMax = B.GetInt(C.Type == EGenContainType::Transport ? TEXT("Slots") : TEXT("ContainMax"), 0);
				C.bKeepVelocityOnExit = B.GetBool(TEXT("KeepContainerVelocityOnExit"));
				C.bInternetHack = Type.Equals(TEXT("InternetHackContain"), ESearchCase::IgnoreCase); // (a TransportContain otherwise)
				C.bOverlord = Type.Equals(TEXT("OverlordContain"), ESearchCase::IgnoreCase);         // (these two as well)
				C.bHelix = Type.Equals(TEXT("HelixContain"), ESearchCase::IgnoreCase);
				C.bPassengersInTurret = B.GetBool(TEXT("PassengersInTurret"));
				C.bExperienceSinkForRider = C.bOverlord && B.GetBool(TEXT("ExperienceSinkForRider"), true);
				for (const FGenIniEntry& Line : B.Entries)
				{
					if (Line.Key.Equals(TEXT("PayloadTemplateName"), ESearchCase::IgnoreCase)) { C.PayloadTemplateNames.Append(Line.Values); }
				}
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
				C.bShouldDrawPips = B.GetBool(TEXT("ShouldDrawPips"), true);
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
				C.bImmuneToClearBuildingAttacks = B.GetBool(TEXT("ImmuneToClearBuildingAttacks"));
				C.EnterSound = B.GetString(TEXT("EnterSound"));
				C.ExitSound = B.GetString(TEXT("ExitSound"));
				if (Type.Equals(TEXT("RiderChangeContain"), ESearchCase::IgnoreCase))
				{
					C.bRiderChange = true;
					C.bShouldDrawPips = false; // getContainerPipsToShow: "always have one rider unless dead"
					for (int32 i = 1; i <= 8; ++i)
					{
						const FGenIniEntry* R = B.Find(*FString::Printf(TEXT("Rider%d"), i));
						if (R && R->Values.Num() >= 6)
						{
							C.RiderTypes.Add({ R->Values[0], R->Values[1], R->Values[2], R->Values[3], R->Values[4], R->Values[5].ToUpper() });
						}
					}
					C.ScuttleFrames = GenLogic::MsecToFrames(B.GetFloat(TEXT("ScuttleDelay"), 0.f));
					C.ScuttleCondition = B.GetString(TEXT("ScuttleStatus"), TEXT("TOPPLED"));
				}
			}
			else if (Type.Equals(TEXT("OverchargeBehavior"), ESearchCase::IgnoreCase))
			{
				T->bOvercharge = true;
				T->OverchargeDrainPerSecond = ParsePercent(E.Block->GetString(TEXT("HealthPercentToDrainPerSecond"), TEXT("0%")));
				T->OverchargeMinHealth = ParsePercent(E.Block->GetString(TEXT("NotAllowedWhenHealthBelowPercent"), TEXT("0%")));
			}
			else if (Type.Equals(TEXT("ParachuteContain"), ESearchCase::IgnoreCase))
			{
				T->bParachute = true;
				T->ParachuteOpenDist = E.Block->GetFloat(TEXT("ParachuteOpenDist"), 0.f);
				T->ParachuteOpenSound = E.Block->GetString(TEXT("ParachuteOpenSound"));
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
				if (Type.Equals(TEXT("UndeadBody"), ESearchCase::IgnoreCase))
				{
					T->SecondLifeMaxHealth = E.Block->GetFloat(TEXT("SecondLifeMaxHealth"), 1.f);
				}
				if (Type.Equals(TEXT("HiveStructureBody"), ESearchCase::IgnoreCase))
				{
					// "NONE +SMALL_ARMS +SNIPER ..."
					auto Types = [&E](const TCHAR* Key, TArray<FString>& Out)
					{
						if (const FGenIniEntry* L = E.Block->Find(Key))
						{
							for (const FString& V : L->Values)
							{
								const FString Name = V.StartsWith(TEXT("+")) ? V.Mid(1) : V;
								if (!Name.Equals(TEXT("NONE"), ESearchCase::IgnoreCase) && !Name.StartsWith(TEXT("-"))) { Out.Add(Name.ToUpper()); }
							}
						}
					};
					Types(TEXT("PropagateDamageTypesToSlavesWhenExisting"), T->HivePropagateDamage);
					Types(TEXT("SwallowDamageTypesIfSlavesNotExisting"), T->HiveSwallowDamage);
				}
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
				for (const FGenIniEntry& Q : E.Block->Entries)
				{
					// "QuantityModifier = ChinaInfantryRedguard 2"
					if (Q.Key.Equals(TEXT("QuantityModifier"), ESearchCase::IgnoreCase) && Q.Values.Num() >= 1)
					{
						T->QuantityModifiers.Emplace(Q.Values[0], Q.Values.Num() >= 2 ? FMath::Max(1, FCString::Atoi(*Q.Values[1])) : 1);
					}
				}
				T->DoorOpeningTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorOpeningTime"), 0.f));
				T->DoorCloseTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorCloseTime"), 0.f));
				T->NumDoorAnimations = E.Block->GetInt(TEXT("NumDoorAnimations"), 0);
				T->DoorWaitOpenTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorWaitOpenTime"), 0.f));
				T->ConstructionCompleteDuration = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ConstructionCompleteDuration"), 0.f));
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
				if (Type.Equals(TEXT("SpawnPointProductionExitUpdate"), ESearchCase::IgnoreCase))
				{
					T->SpawnPointBone = E.Block->GetString(TEXT("SpawnPointBoneName")); // (the stinger site's soldiers stand at these)
				}
				if (Type.Equals(TEXT("QueueProductionExitUpdate"), ESearchCase::IgnoreCase))
				{
					T->bQueueExit = true;
					T->ProductionExitDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("ExitDelay"), 0.f));
					T->ProductionInitialBurst = E.Block->GetInt(TEXT("InitialBurst"), 0);
				}
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
				// The one-shot kind: a building that comes with units. The other keeps them (GenSpawner.cpp).
				if (E.Block->GetBool(TEXT("OneShot")))
				{
					T->SpawnTemplate = E.Block->GetString(TEXT("SpawnTemplateName"));
					T->SpawnNumber = E.Block->GetInt(TEXT("SpawnNumber"), 0);
				}
				else
				{
					FGenThingTemplate::FSpawner& Sp = T->Spawner;
					Sp.Templates.Reset();
					for (const FGenIniEntry* N : E.Block->FindAll(TEXT("SpawnTemplateName")))
					{
						if (!N->Value().IsEmpty())
						{
							Sp.Templates.Add(N->Value()); // parseAsciiStringVectorAppend
						}
					}
					Sp.Number = E.Block->GetInt(TEXT("SpawnNumber"), 0);
					Sp.ReplaceFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("SpawnReplaceDelay"), 0.f));
					Sp.bRequireSpawner = E.Block->GetBool(TEXT("SpawnedRequireSpawner"));
					Sp.InitialBurst = E.Block->GetInt(TEXT("InitialBurst"), 0);
					Sp.bExitByBudding = E.Block->GetBool(TEXT("ExitByBudding"));
					Sp.bAggregateHealth = E.Block->GetBool(TEXT("AggregateHealth"));
					Sp.bSlavesHaveFreeWill = E.Block->GetBool(TEXT("SlavesHaveFreeWill"));
					Sp.bValid = Sp.Templates.Num() > 0 && Sp.Number > 0;
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
					else if (DE.Key.Equals(TEXT("VeterancyLevels"), ESearchCase::IgnoreCase)) { M.VeterancyMask = ParseVeterancyMask(DE.Values); }
					else if (DE.Key.Equals(TEXT("ExemptStatus"), ESearchCase::IgnoreCase)) { M.ExemptStatus = DE.Values; }
					else if (DE.Key.Equals(TEXT("RequiredStatus"), ESearchCase::IgnoreCase)) { M.RequiredStatus = DE.Values; }
					else if (DE.Key.Equals(TEXT("DeathFX"), ESearchCase::IgnoreCase) || DE.Key.Equals(TEXT("FX"), ESearchCase::IgnoreCase)) { M.FX.Append(DE.Values); }
					else if (DE.Key.Equals(TEXT("CreationList"), ESearchCase::IgnoreCase) || DE.Key.Equals(TEXT("OCL"), ESearchCase::IgnoreCase)) { M.OCL.Append(DE.Values); }
					else if (DE.Key.Equals(TEXT("TransferPreviousHealth"), ESearchCase::IgnoreCase)) { M.bTransferPreviousHealth = DE.AsBool(); }
				}
			}
			else if (Type.Equals(TEXT("CreateCrateDie"), ESearchCase::IgnoreCase))
			{
				FGenDieModule& M = T->DieModules.AddDefaulted_GetRef();
				M.Type = EGenDieModule::CreateCrate;
				for (const FGenIniEntry& DE : E.Block->Entries)
				{
					if (DE.Key.Equals(TEXT("DeathTypes"), ESearchCase::IgnoreCase)) { M.DeathTypes = DE.Values; }
					else if (DE.Key.Equals(TEXT("VeterancyLevels"), ESearchCase::IgnoreCase)) { M.VeterancyMask = ParseVeterancyMask(DE.Values); }
					else if (DE.Key.Equals(TEXT("ExemptStatus"), ESearchCase::IgnoreCase)) { M.ExemptStatus = DE.Values; }
					else if (DE.Key.Equals(TEXT("RequiredStatus"), ESearchCase::IgnoreCase)) { M.RequiredStatus = DE.Values; }
					else if (DE.Key.Equals(TEXT("CrateData"), ESearchCase::IgnoreCase)) { M.CrateData.Append(DE.Values); }
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
				H.OCLEjectPilot = NameOrNone(TEXT("OCLEjectPilot"));
				H.FXHitGround = NameOrNone(TEXT("FXHitGround"));
				H.OCLHitGround = NameOrNone(TEXT("OCLHitGround"));
				H.SoundDeathLoop = NameOrNone(TEXT("SoundDeathLoop"));
				H.FXFinalBlowUp = NameOrNone(TEXT("FXFinalBlowUp"));
				H.OCLFinalBlowUp = NameOrNone(TEXT("OCLFinalBlowUp"));
				H.DelayFromGroundToFinalDeath = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DelayFromGroundToFinalDeath"), 0.f));
				H.FinalRubbleObject = NameOrNone(TEXT("FinalRubbleObject"));
			}
			else if (Type.Equals(TEXT("SlowDeathBehavior"), ESearchCase::IgnoreCase) || Type.EndsWith(TEXT("SlowDeathBehavior"), ESearchCase::IgnoreCase))
			{
				if (Type.Equals(TEXT("NeutronMissileSlowDeathBehavior"), ESearchCase::IgnoreCase))
				{
					// (A slow death like any other - its DestructionDelay below - with the blasts on top.)
					const FGenIniBlock& B = *E.Block;
					FGenNeutronBlast& N = T->NeutronBlast;
					N.bValid = true;
					N.ScorchSize = B.GetFloat(TEXT("ScorchMarkSize"), 0.f);
					N.FXList = B.GetString(TEXT("FXList"));
					const float FramesPerMs = float(GenLogic::GetLogicFPS()) / 1000.f;
					for (int32 i = 0; i < UE_ARRAY_COUNT(N.Blasts); ++i)
					{
						auto Key = [i](const TCHAR* Suffix) { return FString::Printf(TEXT("Blast%d%s"), i + 1, Suffix); };
						FGenNeutronBlast::FBlast& Blast = N.Blasts[i];
						Blast.bEnabled = B.GetBool(*Key(TEXT("Enabled")));
						Blast.Delay = B.GetFloat(*Key(TEXT("Delay")), 0.f) * FramesPerMs;
						Blast.ScorchDelay = B.GetFloat(*Key(TEXT("ScorchDelay")), 0.f) * FramesPerMs;
						Blast.InnerRadius = B.GetFloat(*Key(TEXT("InnerRadius")), 0.f);
						Blast.OuterRadius = B.GetFloat(*Key(TEXT("OuterRadius")), 0.f);
						Blast.MaxDamage = B.GetFloat(*Key(TEXT("MaxDamage")), 0.f);
						Blast.MinDamage = B.GetFloat(*Key(TEXT("MinDamage")), 0.f);
						Blast.ToppleSpeed = B.GetFloat(*Key(TEXT("ToppleSpeed")), 0.f);
						Blast.PushForce = B.GetFloat(*Key(TEXT("PushForce")), 0.f);
					}
				}
				FGenSlowDeath D;
				if (Type.Equals(TEXT("BattleBusSlowDeathBehavior"), ESearchCase::IgnoreCase))
				{
					D.bBattleBus = true;
					D.FXStartUndeath = E.Block->GetString(TEXT("FXStartUndeath"));
					D.OCLStartUndeath = E.Block->GetString(TEXT("OCLStartUndeath"));
					D.FXHitGround = E.Block->GetString(TEXT("FXHitGround"));
					D.OCLHitGround = E.Block->GetString(TEXT("OCLHitGround"));
					D.ThrowForce = E.Block->GetFloat(TEXT("ThrowForce"), 1.f);
					D.PercentDamageToPassengers = ParsePercent(E.Block->GetString(TEXT("PercentDamageToPassengers"), TEXT("0%")));
					D.EmptyHulkDestructionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("EmptyHulkDestructionDelay"), 0.f));
				}
				D.DestructionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DestructionDelay"), 0.f));
				D.DestructionDelayVariance = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DestructionDelayVariance"), 0.f));
				D.SinkDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("SinkDelay"), 0.f));
				D.SinkRate = GenLogic::VelocityPerFrame(E.Block->GetFloat(TEXT("SinkRate"), 0.f));
				D.ProbabilityModifier = E.Block->GetInt(TEXT("ProbabilityModifier"), 10);
				if (const FGenIniEntry* DT = E.Block->Find(TEXT("DeathTypes"))) { D.DeathTypes = DT->Values; }
				if (const FGenIniEntry* ES = E.Block->Find(TEXT("ExemptStatus"))) { D.ExemptStatus = ES->Values; }
				if (const FGenIniEntry* RS = E.Block->Find(TEXT("RequiredStatus"))) { D.RequiredStatus = RS->Values; }
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
				if (const FGenIniEntry* VL = E.Block->Find(TEXT("VeterancyLevels"))) { M.VeterancyMask = ParseVeterancyMask(VL->Values); }
				if (const FGenIniEntry* ES = E.Block->Find(TEXT("ExemptStatus"))) { M.ExemptStatus = ES->Values; }
				if (const FGenIniEntry* RS = E.Block->Find(TEXT("RequiredStatus"))) { M.RequiredStatus = RS->Values; }
			}
			else if (Type.Equals(TEXT("DumbProjectileBehavior"), ESearchCase::IgnoreCase))
			{
				FGenDumbProjectile& P = T->DumbProjectile;
				P.bValid = true;
				P.FirstHeight = E.Block->GetFloat(TEXT("FirstHeight"), 0.f);
				P.SecondHeight = E.Block->GetFloat(TEXT("SecondHeight"), 0.f);
				if (const FGenIniEntry* F = E.Block->Find(TEXT("FirstPercentIndent"))) { P.FirstPercentIndent = ParsePercent(F->Value()); }
				if (const FGenIniEntry* F = E.Block->Find(TEXT("SecondPercentIndent"))) { P.SecondPercentIndent = ParsePercent(F->Value()); }
				ParseGarrisonHitKill(*E.Block, P);
			}
			else if (IsAIModule(Type))
			{
				T->bHasAI = true;
				T->AIUpdateType = Type;
				T->bWanderAI = Type.Equals(TEXT("WanderAIUpdate"), ESearchCase::IgnoreCase);
				if (Type.Equals(TEXT("DeployStyleAIUpdate"), ESearchCase::IgnoreCase))
				{
					FGenThingTemplate::FDeploy& D = T->Deploy;
					D.bValid = true;
					D.UnpackFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("UnpackTime"), 0.f));
					D.PackFrames = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("PackTime"), 0.f));
					D.bTurretsOnlyWhenDeployed = E.Block->GetBool(TEXT("TurretsFunctionOnlyWhenDeployed"));
					D.bTurretsMustCenter = E.Block->GetBool(TEXT("TurretsMustCenterBeforePacking"));
					D.bManualAnimations = E.Block->GetBool(TEXT("ManualDeployAnimations"));
				}
				else if (Type.Equals(TEXT("AssaultTransportAIUpdate"), ESearchCase::IgnoreCase))
				{
					T->AssaultTransport.bValid = true;
					T->AssaultTransport.HealRatio = E.Block->GetFloat(TEXT("MembersGetHealedAtLifeRatio"), 0.f);
				}
				if (Type.Equals(TEXT("HackInternetAIUpdate"), ESearchCase::IgnoreCase))
				{
					FGenUpgradeData::ParseHackInternet(*E.Block, T->HackInternet);
				}
				T->bIsMissile = Type.Equals(TEXT("MissileAIUpdate"), ESearchCase::IgnoreCase);
				if (T->bIsMissile)
				{
					ParseGarrisonHitKill(*E.Block, T->DumbProjectile); // (MissileAIUpdate has the same: the Dragon tank's flames)
					T->bMissileUseWeaponSpeed = E.Block->GetBool(TEXT("UseWeaponSpeed"));
					T->MissileInitialVelocity = GenLogic::VelocityPerFrame(E.Block->GetFloat(TEXT("InitialVelocity"), 0.f));
					T->MissileIgnitionDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("IgnitionDelay"), 0.f));
					T->bMissileTryToFollowTarget = E.Block->GetBool(TEXT("TryToFollowTarget"), true);
					T->MissileFuelLifetime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("FuelLifetime"), 0.f));
					T->MissileInitialDist = E.Block->GetFloat(TEXT("DistanceToTravelBeforeTurning"), 0.f);
					T->MissileDiveDistance = E.Block->GetFloat(TEXT("DistanceToTargetBeforeDiving"), 0.f);
					T->MissileLockDistance = E.Block->GetFloat(TEXT("DistanceToTargetForLock"), 75.f);
					T->MissileIgnitionFX = E.Block->GetString(TEXT("IgnitionFX"));
					T->bMissileDetonateOnNoFuel = E.Block->GetBool(TEXT("DetonateOnNoFuel"));
					T->bMissileDetonateCallsKill = E.Block->GetBool(TEXT("DetonateCallsKill"));
					if (E.Block->Find(TEXT("KillSelfDelay"))) { T->MissileKillSelfDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("KillSelfDelay"), 100.f)); }
				}
				T->bChinookAI = Type.Equals(TEXT("ChinookAIUpdate"), ESearchCase::IgnoreCase);
				if (T->bChinookAI)
				{
					const FGenIniBlock& B = *E.Block;
					T->ChinookNumRopes = B.GetInt(TEXT("NumRopes"), 4);
					T->ChinookRopeDelayMin = GenLogic::MsecToFrames(B.GetFloat(TEXT("PerRopeDelayMin"), 1000.f));
					T->ChinookRopeDelayMax = GenLogic::MsecToFrames(B.GetFloat(TEXT("PerRopeDelayMax"), 1000.f));
					T->ChinookRopeWidth = B.GetFloat(TEXT("RopeWidth"), 0.5f);
					T->ChinookRopeWobbleLen = B.GetFloat(TEXT("RopeWobbleLen"), 10.f);
					T->ChinookRopeWobbleAmp = B.GetFloat(TEXT("RopeWobbleAmplitude"), 0.f);
					T->ChinookRopeWobbleRate = GenLogic::AngularVelocityPerFrame(B.GetFloat(TEXT("RopeWobbleRate"), 0.f));
					T->ChinookRopeFinalHeight = B.GetFloat(TEXT("RopeFinalHeight"), 0.f);
					if (B.Find(TEXT("RopeDropSpeed"))) { T->ChinookRopeDropSpeed = GenLogic::VelocityPerFrame(B.GetFloat(TEXT("RopeDropSpeed"), 0.f)); }
					// (Unless the data says otherwise: half a second's worth of falling.)
					T->ChinookRappelSpeed = B.Find(TEXT("RappelSpeed")) ? GenLogic::VelocityPerFrame(B.GetFloat(TEXT("RappelSpeed"), 0.f))
						: FMath::Abs(FGenGameData::Get().Gravity) * float(GenLogic::GetLogicFPS()) * 0.5f;
					T->ChinookMinDropHeight = B.GetFloat(TEXT("MinDropHeight"), 30.f);
					T->bChinookWaitForRopes = B.GetBool(TEXT("WaitForRopesToDrop"), true);
				}
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
					T->JetSneakyOffset = E.Block->GetFloat(TEXT("SneakyOffsetWhenAttacking"), 0.f);
					T->JetAttackersMissPersistTime = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("AttackersMissPersistTime"), 0.f));
				}
				T->bDeliverPayloadAI = Type.Equals(TEXT("DeliverPayloadAIUpdate"), ESearchCase::IgnoreCase);
			T->bForbidPlayerCommands = E.Block->GetBool(TEXT("ForbidPlayerCommands"), false);
				if (T->bDeliverPayloadAI) { T->DeliverDoorDelay = GenLogic::MsecToFrames(E.Block->GetFloat(TEXT("DoorDelay"), 0.f)); }
				if (const FGenIniEntry* Boost = E.Block->Find(TEXT("UpgradedSupplyBoost"))) { T->UpgradedSupplyBoost = Boost->AsInt(); }
				T->bAutoAcquireEnemiesWhenIdle = E.Block->GetBool(TEXT("AutoAcquireEnemiesWhenIdle"), T->bAutoAcquireEnemiesWhenIdle);
				// (AIUpdateInterface::getNextMoodTarget. Said outright it holds for anything; left unsaid - which
				// in the original means No as well - it is taken to hold only for what takes no orders either:
				// a gunship, whose own module does its targeting.)
				{
					const FGenIniEntry* Said = E.Block->Find(TEXT("AutoAcquireEnemiesWhenIdle"));
					T->bNoIdleAutoAcquire = Said ? (Said->Values.Num() > 0 && Said->Values[0].Equals(TEXT("No"), ESearchCase::IgnoreCase)) : T->bForbidPlayerCommands;
				}
				if (const FGenIniEntry* Acquire = E.Block->Find(TEXT("AutoAcquireEnemiesWhenIdle")))
				{
					T->bAutoAcquireWhenStealthed = Acquire->Values.ContainsByPredicate([](const FString& V) { return V.Equals(TEXT("Stealthed"), ESearchCase::IgnoreCase); });
				T->bAutoAcquireBuildings = Acquire->Values.ContainsByPredicate([](const FString& V) { return V.Equals(TEXT("ATTACK_BUILDINGS"), ESearchCase::IgnoreCase); });
				}
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
					T->bHasMineClearingSet |= WS.Conditions.Contains(TEXT("MINE_CLEARING_DETAIL"));
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
				else if (WE.Key.Equals(TEXT("PreferredAgainst"), ESearchCase::IgnoreCase) && WE.Values.Num() >= 2)
				{
					const uint32 Slot = ParseWeaponSlots({ WE.Values[0] });
					const int32 Index = Slot == 1 ? 0 : (Slot == 2 ? 1 : (Slot == 4 ? 2 : -1));
					if (Index >= 0)
					{
						WS.PreferredAgainst[Index].Reset();
						for (int32 v = 1; v < WE.Values.Num(); ++v)
						{
							if (!WE.Values[v].Equals(TEXT("NONE"), ESearchCase::IgnoreCase)) { WS.PreferredAgainst[Index].Add(WE.Values[v].ToUpper()); }
						}
					}
				}
				else if (WE.Key.Equals(TEXT("AutoChooseSources"), ESearchCase::IgnoreCase) && WE.Values.Num() >= 2)
				{
					// "AutoChooseSources = SECONDARY NONE": nobody's attack order picks that weapon; a button does.
					const uint32 Slot = ParseWeaponSlots({ WE.Values[0] });
					const int32 Index = Slot == 1 ? 0 : (Slot == 2 ? 1 : (Slot == 4 ? 2 : -1));
					if (Index >= 0)
					{
						WS.bNoAutoChoose[Index] = WE.Values[1].Equals(TEXT("NONE"), ESearchCase::IgnoreCase);
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
				else if (AE.Key.Equals(TEXT("DamageFX"), ESearchCase::IgnoreCase))
				{
					AS.DamageFX = AE.Value();
				}
			}
		}
		else if (E.Key.Equals(TEXT("ClientUpdate"), ESearchCase::IgnoreCase) && E.Block && E.Value(0).Equals(TEXT("LaserUpdate"), ESearchCase::IgnoreCase))
		{
			// LaserUpdateModuleData: the flares at the two ends of a beam.
			T->LaserMuzzleParticleSystem = E.Block->GetString(TEXT("MuzzleParticleSystem"));
			T->LaserTargetParticleSystem = E.Block->GetString(TEXT("TargetParticleSystem"));
		}
		else if (E.Key.Equals(TEXT("ClientUpdate"), ESearchCase::IgnoreCase) && E.Value(0).Equals(TEXT("SwayClientUpdate"), ESearchCase::IgnoreCase))
		{
			T->Sway = FGenThingTemplate::ESway::ClientUpdate;
		}
		else if (E.Key.Equals(TEXT("ClientUpdate"), ESearchCase::IgnoreCase) && E.Value(0).Equals(TEXT("AnimatedParticleSysBoneClientUpdate"), ESearchCase::IgnoreCase))
		{
			T->bAnimatedParticleBones = true;
		}
		else if (E.Key.Equals(TEXT("Draw"), ESearchCase::IgnoreCase) && E.Block)
		{
			FGenDrawModule& D = T->Draws.AddDefaulted_GetRef();
			D.Type = E.Value(0);
			D.Tag = E.Value(1);
			D.Block = E.Block;
			D.bOkToChangeModelColor = E.Block->GetBool(TEXT("OkToChangeModelColor"));
			D.AttachToBoneInContainer = E.Block->GetString(TEXT("AttachToBoneInContainer"));
			D.RequiredScience = E.Block->GetString(TEXT("RequiredScience"));
			if (const FGenIniEntry* Feedback = E.Block->Find(TEXT("ProjectileBoneFeedbackEnabledSlots")))
			{
				for (const FString& SlotName : Feedback->Values)
				{
					D.ProjectileBoneFeedbackSlots |= SlotName.Equals(TEXT("PRIMARY"), ESearchCase::IgnoreCase) ? 1
						: SlotName.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 2 : SlotName.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 4 : 0;
				}
			}
			D.bAnimationsRequirePower = E.Block->GetBool(TEXT("AnimationsRequirePower"), true);
			D.bParticlesOnAnimatedBones = E.Block->GetBool(TEXT("ParticlesAttachedToAnimatedBones"), false);
			D.bPoliceLights = D.Type.Equals(TEXT("W3DPoliceCarDraw"), ESearchCase::IgnoreCase);
			if (D.Type.Equals(TEXT("W3DTreeDraw"), ESearchCase::IgnoreCase))
			{
				T->Sway = FGenThingTemplate::ESway::TreeBuffer; // (every tree in the buffer sways)
			}
			{
				FGenVehicleDraw& V = D.Vehicle;
				V.Kind = D.Type.Equals(TEXT("W3DTankDraw"), ESearchCase::IgnoreCase) ? FGenVehicleDraw::Tank
					: (D.Type.Equals(TEXT("W3DTruckDraw"), ESearchCase::IgnoreCase) || D.bPoliceLights) ? FGenVehicleDraw::Truck
					: D.Type.Equals(TEXT("W3DTankTruckDraw"), ESearchCase::IgnoreCase) ? FGenVehicleDraw::TankTruck : FGenVehicleDraw::None;
				const FGenIniBlock& B = *E.Block;
				if (V.HasTreads())
				{
					V.TreadDebrisLeft = B.GetString(TEXT("TreadDebrisLeft"), V.TreadDebrisLeft);
					V.TreadDebrisRight = B.GetString(TEXT("TreadDebrisRight"), V.TreadDebrisRight);
					V.TreadAnimationRate = B.GetFloat(TEXT("TreadAnimationRate"), 0.f) / 30.f; // (parseVelocityReal)
					V.TreadPivotSpeedFraction = B.GetFloat(TEXT("TreadPivotSpeedFraction"), V.TreadPivotSpeedFraction);
					V.TreadDriveSpeedFraction = B.GetFloat(TEXT("TreadDriveSpeedFraction"), V.TreadDriveSpeedFraction);
				}
				if (V.HasWheels())
				{
					static const TCHAR* TireKeys[FGenVehicleDraw::NumTires] = { TEXT("LeftFrontTireBone"), TEXT("RightFrontTireBone"),
						TEXT("LeftRearTireBone"), TEXT("RightRearTireBone"), TEXT("MidLeftFrontTireBone"), TEXT("MidRightFrontTireBone"),
						TEXT("MidLeftRearTireBone"), TEXT("MidRightRearTireBone"), TEXT("MidLeftMidTireBone"), TEXT("MidRightMidTireBone") };
					for (int32 i = 0; i < FGenVehicleDraw::NumTires; ++i)
					{
						V.Tires[i] = B.GetString(TireKeys[i]);
					}
					V.Dust = B.GetString(TEXT("Dust"));
					V.DirtSpray = B.GetString(TEXT("DirtSpray"));
					V.PowerslideSpray = B.GetString(TEXT("PowerslideSpray"));
					V.TireRotationMultiplier = B.GetFloat(TEXT("TireRotationMultiplier"), 0.f);
					V.PowerslideRotationAddition = B.GetFloat(TEXT("PowerslideRotationAddition"), 0.f);
					if (V.Kind == FGenVehicleDraw::Truck)
					{
						V.CabBone = B.GetString(TEXT("CabBone"));
						V.TrailerBone = B.GetString(TEXT("TrailerBone"));
						V.CabRotationMultiplier = B.GetFloat(TEXT("CabRotationMultiplier"), 0.f);
						V.TrailerRotationMultiplier = B.GetFloat(TEXT("TrailerRotationMultiplier"), 0.f);
						V.RotationDamping = B.GetFloat(TEXT("RotationDamping"), 0.f);
					}
				}
			}
			if (D.Type.Equals(TEXT("W3DLaserDraw"), ESearchCase::IgnoreCase))
			{
				FGenUpgradeData::ParseLaserDraw(*E.Block, T->LaserDraw);
			}
			if (D.Type.Equals(TEXT("W3DProjectileStreamDraw"), ESearchCase::IgnoreCase))
			{
				FGenStreamDraw& S = T->StreamDraw;
				S.bValid = true;
				S.Texture = E.Block->GetString(TEXT("Texture"));
				S.Width = E.Block->GetFloat(TEXT("Width"), 0.f);
				S.TileFactor = E.Block->GetFloat(TEXT("TileFactor"), 0.f);
				S.ScrollRate = E.Block->GetFloat(TEXT("ScrollRate"), 0.f);
				S.MaxSegments = E.Block->GetInt(TEXT("MaxSegments"), 0);
			}
			if (D.Type.Equals(TEXT("W3DTreeDraw"), ESearchCase::IgnoreCase) && E.Block->GetBool(TEXT("DoTopple")))
			{
				// W3DTreeDrawModuleData: the tree buffer's own toppling, with its own defaults.
				FGenTopple& P = T->Topple;
				const FGenIniBlock& B = *E.Block;
				P = FGenTopple();
				P.bValid = true;
				P.bTree = true;
				P.ToppleFX = B.GetString(TEXT("ToppleFX"));
				P.BounceFX = B.GetString(TEXT("BounceFX"));
				P.bKillWhenToppled = B.GetBool(TEXT("KillWhenFinishedToppling"), true);
				P.InitialVelocityPercent = ParsePercent(B.GetString(TEXT("InitialVelocityPercent"), TEXT("20%")));
				P.InitialAccelPercent = ParsePercent(B.GetString(TEXT("InitialAccelPercent"), TEXT("1%")));
				P.BounceVelocityPercent = ParsePercent(B.GetString(TEXT("BounceVelocityPercent"), TEXT("30%")));
				P.MinimumToppleSpeed = B.GetFloat(TEXT("MinimumToppleSpeed"), 0.5f);
				P.SinkFrames = FMath::Max(1, B.Find(TEXT("SinkTime")) ? GenLogic::MsecToFrames(B.GetFloat(TEXT("SinkTime"), 0.f)) : 10 * GenLogic::GetLogicFPS());
				P.SinkDistance = B.GetFloat(TEXT("SinkDistance"), 20.f);
			}
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
				else if (DE.Key.Equals(TEXT("TransitionState"), ESearchCase::IgnoreCase) && DE.Block && DE.Values.Num() >= 2)
				{
					// W3DModelDrawModuleData::parseConditionState, PARSE_TRANSITION: what is shown on the way
					// from a state with the first key to a state with the second. It starts out as the default
					// state, like any other, but has no keys of its own.
					FGenModelConditionState S;
					const bool bCopy = D.States.IsValidIndex(D.DefaultStateIndex);
					if (bCopy)
					{
						S = D.States[D.DefaultStateIndex];
						S.bIsDefault = false;
						S.ConditionSets.Reset();
						S.TransitionKey.Reset();
						S.AllowToFinishKey.Reset();
					}
					const FString From = DE.Values[0].ToLower(), To = DE.Values[1].ToLower();
					S.Description = TEXT("TRANSITION ") + From + TEXT(" ") + To;
					ParseConditionStateBlock(*DE.Block, S, bCopy);
					S.TransitionKey.Reset();
					S.AllowToFinishKey.Reset();
					D.Transitions.Add(From + TEXT(" ") + To, MoveTemp(S));
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

const FGenDamageFX* FGenGameData::FindDamageFX(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : DamageFXs.Find(Name.ToLower());
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

const FString& FGenGameData::BaseSideOf(const FString& Side) const
{
	// The faction an object's Side belongs to ("AmericaAirForceGeneral" -> "USA"), by the player template
	// of that side. Empty for sides no player plays (Civilian).
	static const FString None;
	const FGenPlayerTemplate* P = PlayerTemplates.FindByPredicate([&](const FGenPlayerTemplate& T) { return T.Side.Equals(Side, ESearchCase::IgnoreCase); });
	return P ? P->BaseSide : None;
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
