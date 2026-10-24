// Port of GameClient/System/ParticleSys.cpp: emission volumes and velocities, per-frame particle
// integration, key-framed alpha/color, wind, slave systems and per-particle attached systems.

#include "Client/GenParticles.h"

namespace
{
	constexpr int32 MaxKeys = FGenParticleSystemTemplate::MaxKeyframes;
	constexpr float MaxSizeBonus = 50.f; // MAX_SIZE_BONUS

	FVector3f PointOnUnitSphere(FRandomStream& R)
	{
		FVector3f P;
		do
		{
			P = FVector3f(R.FRandRange(-1.f, 1.f), R.FRandRange(-1.f, 1.f), R.FRandRange(-1.f, 1.f));
		} while (P.IsZero());
		return P.GetSafeNormal();
	}
}

// ---- system setup -------------------------------------------------------------------------------

void FGenParticleSystem::SetPosition(const FVector3f& P)
{
	LocalTransform.SetTranslation(P);
	bLocalIdentity = false;
}

void FGenParticleSystem::SetLocalTransform(const FTransform3f& T)
{
	LocalTransform = T;
	bLocalIdentity = false;
}

void FGenParticleSystem::RotateLocal(const FVector3f& Axis, float Radians)
{
	// Matrix3D::Rotate_X/Y/Z post-multiply: the rotation happens in the system's own frame.
	LocalTransform.SetRotation((LocalTransform.GetRotation() * FQuat4f(Axis, Radians)).GetNormalized());
	bLocalIdentity = false;
}

void FGenParticleSystem::SetEmissionRadius(float Radius)
{
	if (Template->VolumeType == EGenEmissionVolume::Sphere) { SphereRadius = Radius; }
	else if (Template->VolumeType == EGenEmissionVolume::Cylinder) { CylRadius = Radius; }
}

// ---- emission -----------------------------------------------------------------------------------

FVector3f FGenParticleSystem::ComputePosition(FRandomStream& R) const
{
	const FGenParticleSystemTemplate& T = *Template;
	switch (T.VolumeType)
	{
	case EGenEmissionVolume::Cylinder:
	{
		const float Angle = R.FRandRange(0.f, 2.f * PI);
		const float Radius = T.bHollow ? CylRadius : R.FRandRange(0.f, CylRadius);
		const float HalfLength = T.CylLength / 2.f;
		return FVector3f(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), R.FRandRange(-HalfLength, HalfLength));
	}
	case EGenEmissionVolume::Sphere:
	{
		const float Radius = T.bHollow ? SphereRadius : R.FRandRange(0.f, SphereRadius);
		return PointOnUnitSphere(R) * Radius;
	}
	case EGenEmissionVolume::Box:
	{
		const FVector3f& H = T.BoxHalfSize;
		FVector3f P(R.FRandRange(-H.X, H.X), R.FRandRange(-H.Y, H.Y), R.FRandRange(-H.Z, H.Z));
		if (T.bHollow)
		{
			// Snap to one of the six faces.
			const int32 Side = R.RandRange(0, 5);
			const float Sign = Side < 3 ? -1.f : 1.f;
			if (Side % 3 == 0) { P.Z = Sign * H.Z; }
			else if (Side % 3 == 1) { P.X = Sign * H.X; }
			else { P.Y = Sign * H.Y; }
		}
		return P;
	}
	case EGenEmissionVolume::Line:
		return FMath::Lerp(T.LineStart, T.LineEnd, R.FRandRange(0.f, 1.f));
	default:
		return FVector3f::ZeroVector;
	}
}

FVector3f FGenParticleSystem::ComputeVelocity(const FVector3f& InPos, FRandomStream& R) const
{
	const FGenParticleSystemTemplate& T = *Template;
	switch (T.VelocityType)
	{
	case EGenEmissionVelocity::Ortho:
		return FVector3f(T.VelOrtho[0].Get(R), T.VelOrtho[1].Get(R), T.VelOrtho[2].Get(R));
	case EGenEmissionVelocity::Cylindrical:
	{
		const float Radial = T.VelCylRadial.Get(R);
		const float Angle = R.FRandRange(0.f, 2.f * PI);
		return FVector3f(Radial * FMath::Cos(Angle), Radial * FMath::Sin(Angle), T.VelCylNormal.Get(R));
	}
	case EGenEmissionVelocity::Outward:
	{
		// Directed along the surface normal of the emission volume.
		const float Speed = T.VelOutward.Get(R);
		const float Other = T.VelOutwardOther.Get(R);
		switch (T.VolumeType)
		{
		case EGenEmissionVolume::Cylinder:
		{
			const FVector2f Disk = FVector2f(InPos.X, InPos.Y).GetSafeNormal();
			return FVector3f(Speed * Disk.X, Speed * Disk.Y, Other);
		}
		case EGenEmissionVolume::Box:
		case EGenEmissionVolume::Sphere:
			return InPos.GetSafeNormal() * Speed;
		case EGenEmissionVolume::Line:
		{
			const FVector3f Along = (T.LineEnd - T.LineStart).GetSafeNormal();
			const FVector3f Perp = FVector3f::CrossProduct(FVector3f::UpVector, Along);
			const FVector3f Up = FVector3f::CrossProduct(Along, Perp);
			return Perp * Speed + Up * Other;
		}
		case EGenEmissionVolume::Point:
			return PointOnUnitSphere(R) * Speed;
		default:
			return FVector3f::ZeroVector;
		}
	}
	case EGenEmissionVelocity::Spherical:
		return PointOnUnitSphere(R) * T.VelSpherical.Get(R);
	case EGenEmissionVelocity::Hemispherical:
	{
		FVector3f V;
		do
		{
			V = FVector3f(R.FRandRange(-1.f, 1.f), R.FRandRange(-1.f, 1.f), R.FRandRange(0.f, 1.f));
		} while (V.IsZero());
		return V.GetSafeNormal() * T.VelHemispherical.Get(R);
	}
	default:
		return FVector3f::ZeroVector;
	}
}

void FGenParticleSystem::ComputeAlphaRate(FGenParticle& P) const
{
	const int32 K = P.AlphaTargetKey;
	P.AlphaRate = 0.f;
	if (K >= 1 && K < MaxKeys && Template->AlphaKeys[K].Frame != 0)
	{
		const int32 Time = int32(Template->AlphaKeys[K].Frame) - int32(Template->AlphaKeys[K - 1].Frame);
		if (Time > 0)
		{
			P.AlphaRate = (P.AlphaKeyValue[K] - P.AlphaKeyValue[K - 1]) / float(Time);
		}
	}
}

void FGenParticleSystem::ComputeColorRate(FGenParticle& P) const
{
	const int32 K = P.ColorTargetKey;
	P.ColorRate = FVector3f::ZeroVector;
	if (K >= 1 && K < MaxKeys && Template->ColorKeys[K].Frame != 0)
	{
		const int32 Time = int32(Template->ColorKeys[K].Frame) - int32(Template->ColorKeys[K - 1].Frame);
		if (Time > 0)
		{
			P.ColorRate = (Template->ColorKeys[K].Color - Template->ColorKeys[K - 1].Color) / float(Time);
		}
	}
}

FGenParticle FGenParticleSystem::GenerateParticle(int32 Num, int32 Count, FGenParticleManager& Mgr)
{
	const FGenParticleSystemTemplate& T = *Template;
	FRandomStream& R = Mgr.Random;
	FGenParticle P;

	// Position first: OUTWARD velocity depends on where the particle is in the volume.
	P.Pos = ComputePosition(R);
	P.Vel = ComputeVelocity(P.Pos, R) * VelocityMultiplier;
	if (!bIdentity)
	{
		if (bFirstPos)
		{
			LastPos = Pos;
			bFirstPos = false;
		}
		// Spread a burst along the emitter's movement since the previous frame.
		const FVector3f Adjust = (Pos - LastPos) * (1.f - float(Num) / float(FMath::Max(1, Count)));
		P.Pos = Transform.TransformPositionNoScale(P.Pos) - Adjust;
		P.Vel = Transform.TransformVectorNoScale(P.Vel);
		P.EmitterPos = Transform.GetTranslation();
	}

	P.VelDamping = T.VelDamping.Get(R);
	P.AngularDamping = T.AngularDamping.Get(R);
	P.AngleZ = T.AngleZ.Get(R);
	P.AngularRateZ = T.AngularRateZ.Get(R);
	P.LifetimeLeft = uint32(FMath::Max(0.f, T.Lifetime.Get(R)));
	P.Size = T.StartSize.Get(R) * SizeMultiplier;
	P.SizeRate = T.SizeRate.Get(R) * SizeMultiplier;
	P.SizeRateDamping = T.SizeRateDamping.Get(R);

	// Each successive particle starts a bit bigger (or smaller).
	P.Size += AccumulatedSizeBonus;
	AccumulatedSizeBonus = FMath::Min(AccumulatedSizeBonus + T.StartSizeRate.Get(R), MaxSizeBonus);

	for (int32 i = 0; i < MaxKeys; ++i)
	{
		P.AlphaKeyValue[i] = T.AlphaKeys[i].Var.Get(R);
	}
	P.ColorScale = T.ColorScale.Get(R);
	P.bUpTowardsEmitter = T.bParticleUpTowardsEmitter;
	P.WindRandomness = R.FRandRange(0.7f, 1.3f);

	// Particle::Particle
	P.Alpha = P.AlphaKeyValue[0];
	P.AlphaTargetKey = 1;
	ComputeAlphaRate(P);
	P.Color = T.ColorKeys[0].Color;
	P.ColorTargetKey = 1;
	ComputeColorRate(P);

	P.LastPos = P.Pos;
	P.LastAngleZ = P.AngleZ;
	P.LastSize = P.Size;
	P.LastAlpha = P.Alpha;
	P.LastColor = P.Color;
	return P;
}

// ---- per-frame update ---------------------------------------------------------------------------

bool FGenParticleSystem::IsInvisible(const FGenParticle& P) const
{
	const bool bColorSettled = P.ColorTargetKey >= MaxKeys || Template->ColorKeys[P.ColorTargetKey].Frame == 0;
	switch (Template->Shader)
	{
	case EGenParticleShader::Additive:
		return bColorSettled && (P.Color.X + P.Color.Y + P.Color.Z) <= 0.06f;   // faded to black
	case EGenParticleShader::Alpha:
		return P.Alpha < 0.02f;
	case EGenParticleShader::AlphaTest:
		return false;
	case EGenParticleShader::Multiply:
		return bColorSettled && (P.Color.X * P.Color.Y * P.Color.Z) > 0.95f;    // faded to white
	default:
		return true;
	}
}

void FGenParticleSystem::DoWindMotion(FGenParticle& P, FGenParticleManager& Mgr) const
{
	FVector3f SystemPos = LocalTransform.GetTranslation();
	FTransform3f Parent;
	if (AttachedObject != GenInvalidObjectID && Mgr.ObjectTransform && Mgr.ObjectTransform(AttachedObject, Parent))
	{
		SystemPos += Parent.GetTranslation();
	}
	// Full force within 75 units of the system, fading out to none at 200.
	constexpr float FullForceDistance = 75.f, NoForceDistance = 200.f;
	const float Dist = (P.Pos - SystemPos).Size();
	if (Dist < NoForceDistance)
	{
		float Strength = 2.f * P.WindRandomness;
		if (Dist > FullForceDistance)
		{
			Strength *= 1.f - (Dist - FullForceDistance) / (NoForceDistance - FullForceDistance);
		}
		P.Pos.X += FMath::Cos(WindAngle) * Strength;
		P.Pos.Y += FMath::Sin(WindAngle) * Strength;
	}
}

bool FGenParticleSystem::UpdateParticle(FGenParticle& P, FGenParticleManager& Mgr)
{
	const FGenParticleSystemTemplate& T = *Template;
	P.LastPos = P.Pos;
	P.LastAngleZ = P.AngleZ;
	P.LastSize = P.Size;
	P.LastAlpha = P.Alpha;
	P.LastColor = P.Color;

	P.Vel = (P.Vel + P.Accel) * P.VelDamping;
	P.Pos += P.Vel + T.DriftVelocity;
	if (T.WindMotion != EGenWindMotion::NotUsed)
	{
		DoWindMotion(P, Mgr);
	}

	P.AngleZ += P.AngularRateZ;
	P.AngularRateZ *= P.AngularDamping;
	if (P.bUpTowardsEmitter)
	{
		// The particle's "up" points back at the emitter.
		const FVector2f Dir(P.Pos.X - P.EmitterPos.X, P.Pos.Y - P.EmitterPos.Y);
		if (!Dir.IsNearlyZero())
		{
			P.AngleZ = FMath::Atan2(Dir.Y, Dir.X) - PI / 2.f + PI;
			P.LastAngleZ = P.AngleZ;
		}
	}

	P.Size += P.SizeRate;
	P.SizeRate *= P.SizeRateDamping;

	if (T.Shader != EGenParticleShader::Additive)
	{
		P.Alpha += P.AlphaRate;
		if (P.AlphaTargetKey < MaxKeys && T.AlphaKeys[P.AlphaTargetKey].Frame != 0)
		{
			if (P.Age >= T.AlphaKeys[P.AlphaTargetKey].Frame)
			{
				P.Alpha = P.AlphaKeyValue[P.AlphaTargetKey];
				++P.AlphaTargetKey;
				ComputeAlphaRate(P);
			}
		}
		else
		{
			P.AlphaRate = 0.f;
		}
		P.Alpha = FMath::Clamp(P.Alpha, 0.f, 1.f);
	}

	P.Color += P.ColorRate;
	if (P.ColorTargetKey < MaxKeys && T.ColorKeys[P.ColorTargetKey].Frame != 0)
	{
		if (P.Age >= T.ColorKeys[P.ColorTargetKey].Frame)
		{
			++P.ColorTargetKey;
			ComputeColorRate(P);
		}
	}
	else
	{
		P.ColorRate = FVector3f::ZeroVector;
	}
	P.Color += FVector3f(P.ColorScale);
	P.Color = FVector3f(FMath::Clamp(P.Color.X, 0.f, 1.f), FMath::Clamp(P.Color.Y, 0.f, 1.f), FMath::Clamp(P.Color.Z, 0.f, 1.f));

	P.Accel = FVector3f::ZeroVector;
	++P.Age;

	if (P.LifetimeLeft != 0 && --P.LifetimeLeft == 0)
	{
		return false;
	}
	return !IsInvisible(P);
}

void FGenParticleSystem::UpdateWindMotion(FGenParticleManager& Mgr)
{
	const FGenParticleSystemTemplate& T = *Template;
	FRandomStream& R = Mgr.Random;
	if (T.WindMotion == EGenWindMotion::PingPong)
	{
		// Swing between the start and end angle, faster near the middle of the span.
		const float HalfSpan = FMath::Max((WindEndAngle - WindStartAngle) / 2.f, 1e-4f);
		const float DiffFromCenter = FMath::Abs(HalfSpan - WindAngle + WindStartAngle);
		const float Change = FMath::Max((1.f - DiffFromCenter / HalfSpan) * WindAngleChange, 0.005f);
		WindAngle += bWindMovingToEnd ? Change : -Change;
		if (bWindMovingToEnd ? WindAngle >= WindEndAngle : WindAngle <= WindStartAngle)
		{
			bWindMovingToEnd = !bWindMovingToEnd;
			WindAngleChange = R.FRandRange(T.WindAngleChangeMin, T.WindAngleChangeMax);
			WindStartAngle = R.FRandRange(T.WindStartAngleMin, T.WindStartAngleMax);
			WindEndAngle = R.FRandRange(T.WindEndAngleMin, T.WindEndAngleMax);
		}
	}
	else if (T.WindMotion == EGenWindMotion::Circular)
	{
		if (WindAngleChange == 0.f)
		{
			WindAngleChange = R.FRandRange(T.WindAngleChangeMin, T.WindAngleChangeMax);
		}
		WindAngle = FMath::Fmod(WindAngle + WindAngleChange + 2.f * PI, 2.f * PI);
	}
}

bool FGenParticleSystem::Update(FGenParticleManager& Mgr)
{
	const FGenParticleSystemTemplate& T = *Template;
	FRandomStream& R = Mgr.Random;

	if (bDestroyed && SlaveID != 0)
	{
		// We are solely responsible for our slave: it stops with us.
		if (FGenParticleSystem* Slave = Mgr.Find(SlaveID)) { Slave->bDestroyed = true; }
	}

	if (DelayLeft > 0)
	{
		--DelayLeft;
		return true;
	}
	if (T.WindMotion != EGenWindMotion::NotUsed)
	{
		UpdateWindMotion(Mgr);
	}

	// Follow the object we are attached to: particles are emitted relative to its position and orientation.
	bool bTransformSet = false;
	bool bShrouded = false;
	if (AttachedObject != GenInvalidObjectID)
	{
		FTransform3f Parent;
		if (Mgr.ObjectTransform && Mgr.ObjectTransform(AttachedObject, Parent))
		{
			bShrouded = Mgr.ObjectShrouded && Mgr.ObjectShrouded(AttachedObject);
			LastPos = Pos;
			Pos = Parent.GetTranslation();
			Transform = bLocalIdentity ? Parent : LocalTransform * Parent;
			bIdentity = false;
			bTransformSet = true;
		}
		else
		{
			AttachedObject = GenInvalidObjectID; // the object is gone: finish and remove ourselves
			bDestroyed = true;
		}
	}
	if (!bTransformSet)
	{
		Transform = LocalTransform;
		bIdentity = bLocalIdentity;
	}
	if (bControlled)
	{
		// Riding on a particle: its position is our origin.
		Transform.SetTranslation(ControlPos);
		bIdentity = false;
		LastPos = Pos;
		Pos = ControlPos;
	}

	// Emit (slave systems only emit when their master tells them to).
	if (!bDestroyed && (bForever || SystemLifetimeLeft > 0) && !bStopped && !bShrouded && MasterID == 0)
	{
		if (BurstDelayLeft == 0)
		{
			const int32 Count = int32(float(int32(T.BurstCount.Get(R))) * BurstCountMultiplier); // ("count *= m_countCoeff")
			FGenParticleSystem* Slave = SlaveID != 0 ? Mgr.Find(SlaveID) : nullptr;
			for (int32 i = 0; i < Count; ++i)
			{
				FGenParticle P = GenerateParticle(i, Count, Mgr);
				if (T.bEmitAboveGroundOnly && Mgr.GroundHeight && P.Pos.Z < Mgr.GroundHeight(P.Pos.X, P.Pos.Y))
				{
					continue;
				}
				if (Mgr.ParticleCount >= FGenParticleManager::MaxParticles)
				{
					continue;
				}
				if (!T.AttachedSystem.IsEmpty())
				{
					if (FGenParticleSystem* Rider = Mgr.Create(T.AttachedSystem))
					{
						Rider->bControlled = true;
						Rider->ControlPos = P.Pos;
						Rider->Pos = Rider->LastPos = P.Pos;
						P.ControlledSystem = Rider->ID;
					}
				}
				Particles.Add(P);
				++Mgr.ParticleCount;

				if (Slave)
				{
					// mergeRelatedParticleSystems: the slave particle moves like a master particle but looks
					// like a slave one, its size being a scale factor of the master's.
					FGenParticle M = GenerateParticle(1, 1, Mgr);
					const FGenParticle S = Slave->GenerateParticle(1, 1, Mgr);
					M.LifetimeLeft = S.LifetimeLeft;
					M.Size *= S.Size;
					M.SizeRate *= S.SizeRate;
					M.SizeRateDamping *= S.SizeRateDamping;
					M.AngleZ = S.AngleZ;
					M.AngularRateZ = S.AngularRateZ;
					M.AngularDamping = S.AngularDamping;
					FMemory::Memcpy(M.AlphaKeyValue, S.AlphaKeyValue, sizeof(M.AlphaKeyValue));
					M.Alpha = S.Alpha;
					M.AlphaRate = S.AlphaRate;
					M.Color = S.Color;
					M.ColorRate = S.ColorRate;
					M.ColorScale = S.ColorScale;
					M.Pos += Slave->Template->SlavePosOffset.IsZero() ? T.SlavePosOffset : Slave->Template->SlavePosOffset;
					M.LastPos = M.Pos;
					M.LastAngleZ = M.AngleZ;
					M.LastSize = M.Size;
					M.LastAlpha = M.Alpha;
					M.LastColor = M.Color;
					Slave->Particles.Add(M);
					++Mgr.ParticleCount;
				}
			}
			BurstDelayLeft = uint32(FMath::Max(0.f, T.BurstDelay.Get(R)));
		}
		else
		{
			--BurstDelayLeft;
		}
	}

	// Update the particles, dropping the dead ones (order is kept: streaks connect them in sequence).
	int32 Write = 0;
	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		FGenParticle& P = Particles[i];
		if (T.Gravity != 0.f)
		{
			P.Accel.Z += T.Gravity;
		}
		const bool bAlive = UpdateParticle(P, Mgr);
		if (P.ControlledSystem != 0)
		{
			if (FGenParticleSystem* Rider = Mgr.Find(P.ControlledSystem))
			{
				Rider->ControlPos = P.Pos;
				if (!bAlive) { Rider->bDestroyed = true; }
			}
		}
		if (bAlive)
		{
			if (Write != i) { Particles[Write] = P; }
			++Write;
		}
		else
		{
			--Mgr.ParticleCount;
		}
	}
	Particles.SetNum(Write, EAllowShrinking::No);

	// A destroyed system lingers until its particles have died off.
	if (bDestroyed && Particles.Num() == 0)
	{
		return false;
	}
	if (!bForever)
	{
		if (SystemLifetimeLeft > 0) { --SystemLifetimeLeft; }
		if (Particles.Num() > 0) { return true; }
		if (SystemLifetimeLeft == 0) { return false; }
	}
	return true;
}

// ---- manager ------------------------------------------------------------------------------------

FGenParticleSystem* FGenParticleManager::Create(const FString& TemplateName)
{
	return Create(FGenFXData::Get().FindParticleSystem(TemplateName));
}

FGenParticleSystem* FGenParticleManager::Create(const FGenParticleSystemTemplate* Template, bool bCreateSlaves)
{
	// Drawable particles (one template in the whole game) and smudges are not supported. (W3DParticleSystemManager:
	// "if texture name starts with "SMUD" ... we can assume it's a smudge type" - the heat haze over a
	// microwave tank's dish, drawn by distorting the picture behind it, and not at all without the
	// hardware for it or with heat effects off; drawn as particles it is a cloud of white squares.)
	if (!Template || Template->Shader == EGenParticleShader::None
		|| (Template->Type != EGenParticleType::Particle && Template->Type != EGenParticleType::Streak && Template->Type != EGenParticleType::VolumeParticle)
		|| Template->ParticleName.StartsWith(TEXT("SMUD"), ESearchCase::IgnoreCase))
	{
		return nullptr;
	}
	TUniquePtr<FGenParticleSystem> Sys = MakeUnique<FGenParticleSystem>();
	FGenParticleSystem* S = Sys.Get();
	S->ID = NextID++;
	S->Template = Template;
	S->DelayLeft = uint32(FMath::Max(0.f, Template->InitialDelay.Get(Random)));
	S->SystemLifetimeLeft = Template->SystemLifetime;
	S->bForever = Template->SystemLifetime == 0;
	S->SphereRadius = Template->SphereRadius;
	S->CylRadius = Template->CylRadius;
	S->WindAngleChange = Template->WindAngleChangeMin;
	S->WindStartAngle = Random.FRandRange(Template->WindStartAngleMin, Template->WindStartAngleMax);
	S->WindEndAngle = Random.FRandRange(Template->WindEndAngleMin, Template->WindEndAngleMax);
	S->WindAngle = Random.FRandRange(FMath::Min(S->WindStartAngle, S->WindEndAngle), FMath::Max(S->WindStartAngle, S->WindEndAngle));
	ByID.Add(S->ID, S);
	Systems.Add(MoveTemp(Sys));

	if (bCreateSlaves && !Template->SlaveSystem.IsEmpty())
	{
		if (FGenParticleSystem* Slave = Create(FGenFXData::Get().FindParticleSystem(Template->SlaveSystem), true))
		{
			S->SlaveID = Slave->ID;
			Slave->MasterID = S->ID;
		}
	}
	return S;
}

FGenParticleSystem* FGenParticleManager::Find(uint32 ID) const
{
	FGenParticleSystem* const* Found = ByID.Find(ID);
	return Found ? *Found : nullptr;
}

void FGenParticleManager::DestroyAttachedTo(FGenObjectID Object)
{
	for (const TUniquePtr<FGenParticleSystem>& S : Systems)
	{
		if (S->AttachedObject == Object)
		{
			S->AttachedObject = GenInvalidObjectID;
			S->bDestroyed = true;
		}
	}
}

void FGenParticleManager::Reset()
{
	Systems.Reset();
	ByID.Reset();
	ParticleCount = 0;
}

void FGenParticleManager::Step()
{
	// Systems created during the pass (per-particle attached systems) are appended and updated too.
	for (int32 i = 0; i < Systems.Num(); ++i)
	{
		FGenParticleSystem* S = Systems[i].Get();
		if (S->Update(*this))
		{
			if (S->Particles.Num() > 0)
			{
				FVector3f Min(MAX_flt), Max(-MAX_flt);
				float Size = 0.f;
				for (const FGenParticle& P : S->Particles)
				{
					Min = FVector3f::Min(Min, FVector3f::Min(P.Pos, P.LastPos));
					Max = FVector3f::Max(Max, FVector3f::Max(P.Pos, P.LastPos));
					Size = FMath::Max3(Size, P.Size, P.LastSize);
				}
				S->BoundsCenter = (Min + Max) * 0.5f;
				S->BoundsRadius = (Max - Min).Size() * 0.5f + Size;
			}
			continue;
		}
		// Going away: release our slave / master links, drop the remaining particles.
		if (FGenParticleSystem* Slave = S->SlaveID != 0 ? Find(S->SlaveID) : nullptr) { Slave->MasterID = 0; }
		if (FGenParticleSystem* Master = S->MasterID != 0 ? Find(S->MasterID) : nullptr) { Master->SlaveID = 0; }
		for (const FGenParticle& P : S->Particles)
		{
			if (FGenParticleSystem* Rider = P.ControlledSystem != 0 ? Find(P.ControlledSystem) : nullptr) { Rider->bDestroyed = true; }
		}
		ParticleCount -= S->Particles.Num();
		ByID.Remove(S->ID);
		Systems.RemoveAt(i--);
	}
}
