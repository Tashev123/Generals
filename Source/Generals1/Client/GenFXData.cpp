#include "Client/GenFXData.h"
#include "Core/GenIni.h"
#include "Logic/GenLogicTypes.h"
#include "Generals1.h"

namespace
{
	/** Client effects tick at the original 30 frames per second regardless of the logic rate. */
	constexpr float ClientFPS = 30.f;

	FGenRandomVar Rand(const FGenIniEntry& E) { return FGenRandomVar(E.AsFloat(0), E.AsFloat(1, E.AsFloat(0))); }

	/** "X:1 Y:2 Z:3" */
	FVector3f Coord(const FGenIniEntry& E)
	{
		TArray<FString> T;
		GenIni::Tokenize(E.Raw, T, TEXT(" \t=:"));
		FVector3f V = FVector3f::ZeroVector;
		for (int32 i = 0; i + 1 < T.Num(); i += 2)
		{
			const float F = FCString::Atof(*T[i + 1]);
			if (T[i].Equals(TEXT("X"), ESearchCase::IgnoreCase)) { V.X = F; }
			else if (T[i].Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { V.Y = F; }
			else if (T[i].Equals(TEXT("Z"), ESearchCase::IgnoreCase)) { V.Z = F; }
		}
		return V;
	}

	/** "R:255 G:128 B:0 [frame]" -> color 0..1; returns the trailing token as the frame. */
	FVector3f ColorKey(const FGenIniEntry& E, uint32* OutFrame = nullptr)
	{
		TArray<FString> T;
		GenIni::Tokenize(E.Raw, T, TEXT(" \t=:"));
		FVector3f C = FVector3f::ZeroVector;
		int32 i = 0;
		for (; i + 1 < T.Num(); i += 2)
		{
			const float F = FCString::Atof(*T[i + 1]) / 255.f;
			if (T[i].Equals(TEXT("R"), ESearchCase::IgnoreCase)) { C.X = F; }
			else if (T[i].Equals(TEXT("G"), ESearchCase::IgnoreCase)) { C.Y = F; }
			else if (T[i].Equals(TEXT("B"), ESearchCase::IgnoreCase)) { C.Z = F; }
			else { break; }
		}
		if (OutFrame)
		{
			*OutFrame = i < T.Num() ? uint32(FMath::Max(0, FCString::Atoi(*T[i]))) : 0u;
		}
		return C;
	}

	float Percent(const FString& V)
	{
		FString S = V;
		const bool bPct = S.RemoveFromEnd(TEXT("%"));
		const float F = FCString::Atof(*S);
		return bPct ? F / 100.f : F;
	}

	int32 ClientFrames(float Msec) { return FMath::CeilToInt32(Msec * ClientFPS / 1000.f); }

	template <typename TEnum>
	TEnum Lookup(const FString& V, std::initializer_list<const TCHAR*> Names, TEnum Default)
	{
		int32 i = 0;
		for (const TCHAR* N : Names)
		{
			if (V.Equals(N, ESearchCase::IgnoreCase)) { return TEnum(i); }
			++i;
		}
		return Default;
	}

	bool IsNone(const FString& V) { return V.IsEmpty() || V.Equals(TEXT("NONE"), ESearchCase::IgnoreCase); }

	void ParseParticleSystem(const FGenIniBlock& B, FGenParticleSystemTemplate& T)
	{
		for (const FGenIniEntry& E : B.Entries)
		{
			const FString& K = E.Key;
			auto Is = [&](const TCHAR* N) { return K.Equals(N, ESearchCase::IgnoreCase); };
			if (K.Len() == 6 && K.StartsWith(TEXT("Alpha"), ESearchCase::IgnoreCase) && FChar::IsDigit(K[5]))
			{
				const int32 i = K[5] - TEXT('1');
				if (i >= 0 && i < FGenParticleSystemTemplate::MaxKeyframes)
				{
					T.AlphaKeys[i].Var = FGenRandomVar(E.AsFloat(0), E.AsFloat(1));
					T.AlphaKeys[i].Frame = uint32(FMath::Max(0, E.AsInt(2)));
				}
			}
			else if (K.Len() == 6 && K.StartsWith(TEXT("Color"), ESearchCase::IgnoreCase) && FChar::IsDigit(K[5]))
			{
				const int32 i = K[5] - TEXT('1');
				if (i >= 0 && i < FGenParticleSystemTemplate::MaxKeyframes)
				{
					T.ColorKeys[i].Color = ColorKey(E, &T.ColorKeys[i].Frame);
				}
			}
			else if (Is(TEXT("Priority")))
			{
				T.Priority = Lookup<int32>(E.Value(), { TEXT("NONE"), TEXT("WEAPON_EXPLOSION"), TEXT("SCORCHMARK"), TEXT("DUST_TRAIL"), TEXT("BUILDUP"),
					TEXT("DEBRIS_TRAIL"), TEXT("UNIT_DAMAGE_FX"), TEXT("DEATH_EXPLOSION"), TEXT("SEMI_CONSTANT"), TEXT("CONSTANT"), TEXT("WEAPON_TRAIL"),
					TEXT("AREA_EFFECT"), TEXT("CRITICAL"), TEXT("ALWAYS_RENDER") }, 0);
			}
			else if (Is(TEXT("Shader")))
			{
				T.Shader = Lookup(E.Value(), { TEXT("NONE"), TEXT("ADDITIVE"), TEXT("ALPHA"), TEXT("ALPHA_TEST"), TEXT("MULTIPLY") }, EGenParticleShader::None);
			}
			else if (Is(TEXT("Type")))
			{
				T.Type = Lookup(E.Value(), { TEXT("NONE"), TEXT("PARTICLE"), TEXT("DRAWABLE"), TEXT("STREAK"), TEXT("VOLUME_PARTICLE"), TEXT("SMUDGE") }, EGenParticleType::None);
			}
			else if (Is(TEXT("ParticleName"))) { T.ParticleName = E.Value(); }
			else if (Is(TEXT("AngleZ"))) { T.AngleZ = Rand(E); }
			else if (Is(TEXT("AngularRateZ"))) { T.AngularRateZ = Rand(E); }
			else if (Is(TEXT("AngularDamping"))) { T.AngularDamping = Rand(E); }
			else if (Is(TEXT("VelocityDamping"))) { T.VelDamping = Rand(E); }
			else if (Is(TEXT("Gravity"))) { T.Gravity = E.AsFloat(); }
			else if (Is(TEXT("SlaveSystem"))) { T.SlaveSystem = IsNone(E.Value()) ? FString() : E.Value(); }
			else if (Is(TEXT("SlavePosOffset"))) { T.SlavePosOffset = Coord(E); }
			else if (Is(TEXT("PerParticleAttachedSystem"))) { T.AttachedSystem = IsNone(E.Value()) ? FString() : E.Value(); }
			else if (Is(TEXT("Lifetime"))) { T.Lifetime = Rand(E); }
			else if (Is(TEXT("SystemLifetime"))) { T.SystemLifetime = uint32(FMath::Max(0, E.AsInt())); }
			else if (Is(TEXT("Size"))) { T.StartSize = Rand(E); }
			else if (Is(TEXT("StartSizeRate"))) { T.StartSizeRate = Rand(E); }
			else if (Is(TEXT("SizeRate"))) { T.SizeRate = Rand(E); }
			else if (Is(TEXT("SizeRateDamping"))) { T.SizeRateDamping = Rand(E); }
			else if (Is(TEXT("ColorScale"))) { T.ColorScale = FGenRandomVar(E.AsFloat(0) / 255.f, E.AsFloat(1) / 255.f); }
			else if (Is(TEXT("BurstDelay"))) { T.BurstDelay = Rand(E); }
			else if (Is(TEXT("BurstCount"))) { T.BurstCount = Rand(E); }
			else if (Is(TEXT("InitialDelay"))) { T.InitialDelay = Rand(E); }
			else if (Is(TEXT("DriftVelocity"))) { T.DriftVelocity = Coord(E); }
			else if (Is(TEXT("VelocityType")))
			{
				T.VelocityType = Lookup(E.Value(), { TEXT("NONE"), TEXT("ORTHO"), TEXT("SPHERICAL"), TEXT("HEMISPHERICAL"), TEXT("CYLINDRICAL"), TEXT("OUTWARD") },
					EGenEmissionVelocity::None);
			}
			else if (Is(TEXT("VelOrthoX"))) { T.VelOrtho[0] = Rand(E); }
			else if (Is(TEXT("VelOrthoY"))) { T.VelOrtho[1] = Rand(E); }
			else if (Is(TEXT("VelOrthoZ"))) { T.VelOrtho[2] = Rand(E); }
			else if (Is(TEXT("VelSpherical"))) { T.VelSpherical = Rand(E); }
			else if (Is(TEXT("VelHemispherical"))) { T.VelHemispherical = Rand(E); }
			else if (Is(TEXT("VelCylindricalRadial"))) { T.VelCylRadial = Rand(E); }
			else if (Is(TEXT("VelCylindricalNormal"))) { T.VelCylNormal = Rand(E); }
			else if (Is(TEXT("VelOutward"))) { T.VelOutward = Rand(E); }
			else if (Is(TEXT("VelOutwardOther"))) { T.VelOutwardOther = Rand(E); }
			else if (Is(TEXT("VolumeType")))
			{
				T.VolumeType = Lookup(E.Value(), { TEXT("NONE"), TEXT("POINT"), TEXT("LINE"), TEXT("BOX"), TEXT("SPHERE"), TEXT("CYLINDER") }, EGenEmissionVolume::None);
			}
			else if (Is(TEXT("VolLineStart"))) { T.LineStart = Coord(E); }
			else if (Is(TEXT("VolLineEnd"))) { T.LineEnd = Coord(E); }
			else if (Is(TEXT("VolBoxHalfSize"))) { T.BoxHalfSize = Coord(E); }
			else if (Is(TEXT("VolSphereRadius"))) { T.SphereRadius = E.AsFloat(); }
			else if (Is(TEXT("VolCylinderRadius"))) { T.CylRadius = E.AsFloat(); }
			else if (Is(TEXT("VolCylinderLength"))) { T.CylLength = E.AsFloat(); }
			else if (Is(TEXT("IsHollow"))) { T.bHollow = E.AsBool(); }
			else if (Is(TEXT("IsGroundAligned"))) { T.bGroundAligned = E.AsBool(); }
			else if (Is(TEXT("IsEmitAboveGroundOnly"))) { T.bEmitAboveGroundOnly = E.AsBool(); }
			else if (Is(TEXT("IsParticleUpTowardsEmitter"))) { T.bParticleUpTowardsEmitter = E.AsBool(); }
			else if (Is(TEXT("WindMotion")))
			{
				T.WindMotion = E.Value().Equals(TEXT("PingPong"), ESearchCase::IgnoreCase) ? EGenWindMotion::PingPong
					: (E.Value().Equals(TEXT("Circular"), ESearchCase::IgnoreCase) ? EGenWindMotion::Circular : EGenWindMotion::NotUsed);
			}
			else if (Is(TEXT("WindAngleChangeMin"))) { T.WindAngleChangeMin = E.AsFloat(); }
			else if (Is(TEXT("WindAngleChangeMax"))) { T.WindAngleChangeMax = E.AsFloat(); }
			else if (Is(TEXT("WindPingPongStartAngleMin"))) { T.WindStartAngleMin = E.AsFloat(); }
			else if (Is(TEXT("WindPingPongStartAngleMax"))) { T.WindStartAngleMax = E.AsFloat(); }
			else if (Is(TEXT("WindPingPongEndAngleMin"))) { T.WindEndAngleMin = E.AsFloat(); }
			else if (Is(TEXT("WindPingPongEndAngleMax"))) { T.WindEndAngleMax = E.AsFloat(); }
		}
	}

	void ParseFXNugget(const FString& Keyword, const FGenIniBlock& B, FGenFXList& List)
	{
		static const TCHAR* TypeNames[] = { TEXT("Sound"), TEXT("ParticleSystem"), TEXT("Tracer"), TEXT("LightPulse"), TEXT("ViewShake"),
			TEXT("TerrainScorch"), TEXT("RayEffect"), TEXT("FXListAtBonePos") };
		int32 TypeIndex = INDEX_NONE;
		for (int32 i = 0; i < UE_ARRAY_COUNT(TypeNames); ++i)
		{
			if (Keyword.Equals(TypeNames[i], ESearchCase::IgnoreCase)) { TypeIndex = i; break; }
		}
		if (TypeIndex == INDEX_NONE)
		{
			return;
		}
		FGenFXNugget& N = List.Nuggets.AddDefaulted_GetRef();
		N.Type = EGenFXNugget(TypeIndex);
		for (const FGenIniEntry& E : B.Entries)
		{
			auto Is = [&](const TCHAR* Key) { return E.Key.Equals(Key, ESearchCase::IgnoreCase); };
			if (Is(TEXT("Name")) || Is(TEXT("FX")) || Is(TEXT("TracerName"))) { N.Name = E.Value(); }
			else if (Is(TEXT("Count"))) { N.Count = E.AsInt(0, 1); }
			else if (Is(TEXT("Offset"))) { N.Offset = Coord(E); }
			else if (Is(TEXT("Radius")))
			{
				if (N.Type == EGenFXNugget::ParticleSystem) { N.Radius = Rand(E); }
				else { N.EffectRadius = E.AsFloat(); }
			}
			else if (Is(TEXT("Height"))) { N.Height = Rand(E); }
			else if (Is(TEXT("InitialDelay"))) { N.InitialDelayMs = Rand(E); }
			else if (Is(TEXT("RotateX"))) { N.RotateX = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("RotateY"))) { N.RotateY = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("RotateZ"))) { N.RotateZ = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("OrientToObject"))) { N.bOrientToObject = E.AsBool(); }
			else if (Is(TEXT("Ricochet"))) { N.bRicochet = E.AsBool(); }
			else if (Is(TEXT("AttachToObject"))) { N.bAttachToObject = E.AsBool(); }
			else if (Is(TEXT("CreateAtGroundHeight"))) { N.bCreateAtGroundHeight = E.AsBool(); }
			else if (Is(TEXT("UseCallersRadius"))) { N.bUseCallersRadius = E.AsBool(); }
			else if (Is(TEXT("Speed"))) { N.Speed = E.AsFloat() / ClientFPS; }
			else if (Is(TEXT("DecayAt"))) { N.DecayAt = E.AsFloat(); }
			else if (Is(TEXT("Length"))) { N.Length = E.AsFloat(); }
			else if (Is(TEXT("Width"))) { N.Width = E.AsFloat(); }
			else if (Is(TEXT("Probability"))) { N.Probability = E.AsFloat(); }
			else if (Is(TEXT("Color")))
			{
				const FVector3f C = ColorKey(E);
				N.Color = FLinearColor(C.X, C.Y, C.Z);
			}
			else if (Is(TEXT("RadiusAsPercentOfObjectSize"))) { N.RadiusPctOfObject = Percent(E.Value()); }
			else if (Is(TEXT("IncreaseTime"))) { N.IncreaseFrames = ClientFrames(E.AsFloat()); }
			else if (Is(TEXT("DecreaseTime"))) { N.DecreaseFrames = ClientFrames(E.AsFloat()); }
			else if (Is(TEXT("Type")))
			{
				if (N.Type == EGenFXNugget::ViewShake)
				{
					N.Variant = Lookup<int32>(E.Value(), { TEXT("SUBTLE"), TEXT("NORMAL"), TEXT("STRONG"), TEXT("SEVERE"), TEXT("CINE_EXTREME"), TEXT("CINE_INSANE") }, 1);
				}
				else
				{
					N.Variant = Lookup<int32>(E.Value(), { TEXT("SCORCH_1"), TEXT("SCORCH_2"), TEXT("SCORCH_3"), TEXT("SCORCH_4"), TEXT("SHADOW_SCORCH") }, -1);
				}
			}
			else if (Is(TEXT("BoneName"))) { N.BoneName = E.Value(); }
			else if (Is(TEXT("OrientToBone"))) { N.bOrientToBone = E.AsBool(); }
		}
	}

	void ParseOCLNugget(const FString& Keyword, const FGenIniBlock& B, FGenOCL& List)
	{
		const bool bDebris = Keyword.Equals(TEXT("CreateDebris"), ESearchCase::IgnoreCase);
		if (!bDebris && !Keyword.Equals(TEXT("CreateObject"), ESearchCase::IgnoreCase))
		{
			return; // DeliverPayload, FireWeapon, Attack, ApplyRandomForce: not ported yet
		}
		FGenOCLNugget& N = List.Nuggets.AddDefaulted_GetRef();
		N.bDebris = bDebris;
		for (const FGenIniEntry& E : B.Entries)
		{
			auto Is = [&](const TCHAR* Key) { return E.Key.Equals(Key, ESearchCase::IgnoreCase); };
			if (Is(TEXT("ModelNames")) || Is(TEXT("ObjectNames"))) { N.Names.Append(E.Values); }
			else if (Is(TEXT("Count"))) { N.Count = E.AsInt(0, 1); }
			else if (Is(TEXT("Mass"))) { N.Mass = E.AsFloat(); }
			else if (Is(TEXT("Offset"))) { N.Offset = Coord(E); }
			else if (Is(TEXT("Disposition")))
			{
				N.Disposition = 0;
				static const TCHAR* Names[] = { TEXT("LIKE_EXISTING"), TEXT("ON_GROUND_ALIGNED"), TEXT("SEND_IT_FLYING"), TEXT("SEND_IT_UP"), TEXT("SEND_IT_OUT"),
					TEXT("RANDOM_FORCE"), TEXT("FLOATING"), TEXT("INHERIT_VELOCITY"), TEXT("WHIRLING") };
				for (const FString& V : E.Values)
				{
					for (int32 i = 0; i < UE_ARRAY_COUNT(Names); ++i)
					{
						if (V.Equals(Names[i], ESearchCase::IgnoreCase)) { N.Disposition |= 1u << i; }
					}
				}
			}
			else if (Is(TEXT("DispositionIntensity"))) { N.DispositionIntensity = E.AsFloat(); }
			else if (Is(TEXT("SpinRate"))) { N.SpinRate = GenLogic::AngularVelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("YawRate"))) { N.YawRate = GenLogic::AngularVelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("RollRate"))) { N.RollRate = GenLogic::AngularVelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("PitchRate"))) { N.PitchRate = GenLogic::AngularVelocityPerFrame(E.AsFloat()); }
			else if (Is(TEXT("MinForceMagnitude"))) { N.MinMag = E.AsFloat(); }
			else if (Is(TEXT("MaxForceMagnitude"))) { N.MaxMag = E.AsFloat(); }
			else if (Is(TEXT("MinForcePitch"))) { N.MinPitch = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("MaxForcePitch"))) { N.MaxPitch = FMath::DegreesToRadians(E.AsFloat()); }
			else if (Is(TEXT("MinLifetime"))) { N.MinFrames = GenLogic::MsecToFrames(E.AsFloat()); }
			else if (Is(TEXT("MaxLifetime"))) { N.MaxFrames = GenLogic::MsecToFrames(E.AsFloat()); }
			else if (Is(TEXT("ExtraBounciness"))) { N.ExtraBounciness = E.AsFloat(); }
			else if (Is(TEXT("ExtraFriction"))) { N.ExtraFriction = GenLogic::FrictionPerFrame(E.AsFloat()); }
			else if (Is(TEXT("MinHealth"))) { N.MinHealth = Percent(E.Value()); }
			else if (Is(TEXT("MaxHealth"))) { N.MaxHealth = Percent(E.Value()); }
			else if (Is(TEXT("ParticleSystem"))) { N.ParticleSystem = E.Value(); }
			else if (Is(TEXT("FXFinal"))) { N.FXFinal = E.Value(); }
			else if (Is(TEXT("OkToChangeModelColor"))) { N.bOkToChangeModelColor = E.AsBool(); }
			else if (Is(TEXT("OrientInForceDirection"))) { N.bOrientInForceDirection = E.AsBool(); }
			else if (Is(TEXT("SkipIfSignificantlyAirborne"))) { N.bSkipIfSignificantlyAirborne = E.AsBool(); }
		}
	}

	void LoadBoth(const TCHAR* File, TArray<TSharedPtr<FGenIniBlock>>& Blocks)
	{
		GenIni::ParseFile(FString(TEXT("Data\\INI\\Default\\")) + File, Blocks);
		GenIni::ParseFile(FString(TEXT("Data\\INI\\")) + File, Blocks);
	}
}

FGenFXData& FGenFXData::Get()
{
	static FGenFXData Instance;
	return Instance;
}

bool FGenFXData::Initialize()
{
	if (bInitialized)
	{
		return true;
	}
	bInitialized = true;
	const double Start = FPlatformTime::Seconds();

	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	LoadBoth(TEXT("ParticleSystem.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (B->Keyword.Equals(TEXT("ParticleSystem"), ESearchCase::IgnoreCase))
		{
			FGenParticleSystemTemplate& T = ParticleSystems.FindOrAdd(B->Name().ToLower());
			T.Name = B->Name();
			ParseParticleSystem(*B, T);
		}
	}

	Blocks.Reset();
	LoadBoth(TEXT("FXList.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (B->Keyword.Equals(TEXT("FXList"), ESearchCase::IgnoreCase))
		{
			FGenFXList& L = FXLists.FindOrAdd(B->Name().ToLower());
			L.Name = B->Name();
			L.Nuggets.Reset();
			for (const FGenIniEntry& E : B->Entries)
			{
				if (E.Block) { ParseFXNugget(E.Key, *E.Block, L); }
			}
		}
	}

	Blocks.Reset();
	LoadBoth(TEXT("ObjectCreationList.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (B->Keyword.Equals(TEXT("ObjectCreationList"), ESearchCase::IgnoreCase))
		{
			FGenOCL& L = OCLs.FindOrAdd(B->Name().ToLower());
			L.Name = B->Name();
			L.Nuggets.Reset();
			for (const FGenIniEntry& E : B->Entries)
			{
				if (E.Block) { ParseOCLNugget(E.Key, *E.Block, L); }
			}
		}
	}

	Blocks.Reset();
	LoadBoth(TEXT("GameData.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("GameData"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		static const TCHAR* ShakeKeys[6] = { TEXT("ShakeSubtleIntensity"), TEXT("ShakeNormalIntensity"), TEXT("ShakeStrongIntensity"),
			TEXT("ShakeSevereIntensity"), TEXT("ShakeCineExtremeIntensity"), TEXT("ShakeCineInsaneIntensity") };
		for (int32 i = 0; i < 6; ++i)
		{
			ShakeIntensity[i] = B->GetFloat(ShakeKeys[i], ShakeIntensity[i]);
		}
		MaxShakeRange = B->GetFloat(TEXT("MaxShakeRange"), MaxShakeRange);
		MaxShakeIntensity = B->GetFloat(TEXT("MaxShakeIntensity"), MaxShakeIntensity);
		GroundStiffness = B->GetFloat(TEXT("GroundStiffness"), GroundStiffness);
	}

	UE_LOG(LogGenerals, Log, TEXT("FX data: %d particle systems, %d FX lists, %d object creation lists (%.2fs)"),
		ParticleSystems.Num(), FXLists.Num(), OCLs.Num(), FPlatformTime::Seconds() - Start);
	return true;
}

const FGenParticleSystemTemplate* FGenFXData::FindParticleSystem(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : ParticleSystems.Find(Name.ToLower());
}

const FGenFXList* FGenFXData::FindFXList(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : FXLists.Find(Name.ToLower());
}

const FGenOCL* FGenFXData::FindOCL(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : OCLs.Find(Name.ToLower());
}
