// The particle cannon: ParticleUplinkCannonUpdate.
//
// By the power's timer the building charges (its outer nodes glow), raises its antenna, links the
// nodes to the antenna with lasers and stands ready. Fired, a beam goes up from the antenna and,
// BeamTravelTime later, another comes down on the target - widening for WidthGrowTime, staying for
// TotalFiringTime, narrowing again. While it is down it burns: TotalDamagePulses pulses of damage to
// whatever is within the beam's width times DamageRadiusScalar, TotalScorchMarks scorch marks with the
// ground-hit effect, a burning remnant left at every pulse. The player steers it: every new target given
// while it burns is where the beam now wanders to, at ManualDrivingSpeed (twice as fast on a double
// click). Losing power or being disabled cuts the beam short.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Logic/GenUpgrades.h"
#include "W3D/GenAssets.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

namespace
{
	enum ECannonStatus : uint8 { StatusIdle, StatusCharging, StatusPreparing, StatusAlmostReady, StatusReadyToFire, StatusPrefire, StatusFiring, StatusPostfire, StatusPacking };
	enum ECannonLaser : uint8 { LaserNone, LaserBorn, LaserDecaying, LaserDead };
	enum ECannonIntensity : uint8 { Light, Medium, Intense };
	/** The beam comes down from this far above the target, and goes up this far above the antenna. */
	constexpr float OrbitHeight = 500.f;

	const TCHAR* StatusName(uint8 Status)
	{
		static const TCHAR* Names[] = { TEXT("idle"), TEXT("charging"), TEXT("raising its antenna"), TEXT("almost ready"), TEXT("ready to fire"),
			TEXT("about to fire"), TEXT("firing"), TEXT("done firing"), TEXT("packing up") };
		return Status < UE_ARRAY_COUNT(Names) ? Names[Status] : TEXT("?");
	}

	/** The point a set of bones is aimed at: where their own X axes cross. For the ring's nodes that is the
	 * centre of the ring at their own height, so the beams from them stay level. */
	FVector3f AimPoint(const TArray<FTransform3f>& Bones)
	{
		FVector3f Sum = FVector3f::ZeroVector;
		int32 Crosses = 0;
		for (int32 A = 0; A < Bones.Num(); ++A)
		{
			for (int32 B = A + 1; B < Bones.Num(); ++B)
			{
				const FVector3f Pa = Bones[A].GetTranslation(), Pb = Bones[B].GetTranslation();
				const FVector3f Da = Bones[A].GetRotation().RotateVector(FVector3f::RightVector);
				const FVector3f Db = Bones[B].GetRotation().RotateVector(FVector3f::RightVector);
				const float Planar = Da.X * Db.Y - Da.Y * Db.X;
				if (FMath::Abs(Planar) < KINDA_SMALL_NUMBER)
				{
					continue;
				}
				const float T = ((Pb.X - Pa.X) * Db.Y - (Pb.Y - Pa.Y) * Db.X) / Planar;
				if (T <= 0.f)
				{
					continue; // (the crossing is behind the node, so not where its beam points)
				}
				Sum += Pa + Da * T;
				++Crosses;
			}
		}
		return Crosses > 0 ? Sum / float(Crosses) : FVector3f::ZeroVector;
	}
}

/** What a particle cannon is doing (ParticleUplinkCannonUpdate's members). */
struct FGenParticleCannonState
{
	uint8 Status = StatusIdle;
	uint8 LaserStatus = LaserNone;
	uint32 StartAttackFrame = 0;
	uint32 StartDecayFrame = 0;
	FVector3f InitialTarget = FVector3f::ZeroVector;
	FVector3f CurrentTarget = FVector3f::ZeroVector;
	FVector3f OverrideDestination = FVector3f::ZeroVector;
	bool bManualTargetMode = false;
	int32 ScorchMarksMade = 0;
	int32 DamagePulsesMade = 0;
	uint32 NextScorchFrame = 0;
	uint32 NextDamagePulseFrame = 0;
	uint32 NextLaunchFXFrame = 0;
	uint32 LastClickFrame = 0;
	uint32 SecondLastClickFrame = 0;

	// What shows (ParticleUplinkCannonUpdate::setClientStatus):
	bool bBonesCached = false;
	FVector3f OuterNodes[5];                 // in the building's own frame
	FVector3f Connector = FVector3f::ZeroVector;
	FVector3f Origin = FVector3f::ZeroVector;
	int32 NumOuterNodes = 0;
	TArray<uint32, TInlineAllocator<8>> Systems; // the particle systems of the status it is in
	const FGenLaserDraw* ConnectorLaser = nullptr; // the lasers from the nodes to the antenna, if any
	// The beam up from the antenna and the beam down on the target (LaserUpdate: growing, then decaying).
	struct FBeamLife
	{
		bool bOn = false;
		uint32 BornFrame = 0;
		uint32 GrowFrames = 0;
		uint32 DecayStartFrame = 0; // 0: not decaying
		uint32 DecayFrames = 0;
		float Scale(uint32 Frame) const
		{
			if (!bOn)
			{
				return 0.f;
			}
			float S = GrowFrames > 0 ? FMath::Clamp(float(Frame - BornFrame) / float(GrowFrames), 0.f, 1.f) : 1.f;
			if (DecayStartFrame != 0 && Frame >= DecayStartFrame)
			{
				S *= DecayFrames > 0 ? FMath::Clamp(1.f - float(Frame - DecayStartFrame) / float(DecayFrames), 0.f, 1.f) : 0.f;
			}
			return S;
		}
	};
	FBeamLife GroundBeam, OrbitBeam;
};

void FGenObject::FGenParticleCannonDeleter::operator()(FGenParticleCannonState* State) const
{
	delete State;
}

void FGenObject::SetParticleCannonStatus(uint8 NewStatus, uint32 Frame)
{
	// ParticleUplinkCannonUpdate::setLogicalStatus and setClientStatus
	FGenParticleCannonState& C = *ParticleCannon;
	if (C.Status == NewStatus)
	{
		return;
	}
	const FGenParticleCannon& D = Template->ParticleCannon;
	static const int32 Packing = GenConditions::Index(TEXT("PACKING"));
	static const int32 Unpacking = GenConditions::Index(TEXT("UNPACKING"));
	static const int32 Deployed = GenConditions::Index(TEXT("DEPLOYED"));
	switch (NewStatus)
	{
	case StatusIdle:
		SetModelCondition(Packing, false);
		SetModelCondition(Unpacking, false);
		SetModelCondition(Deployed, false);
		break;
	case StatusCharging:
		C.LaserStatus = LaserNone;
		break;
	case StatusPreparing:
		SetModelCondition(Packing, false);
		SetModelCondition(Deployed, false);
		SetModelCondition(Unpacking, true);
		SetDrawAnimDuration(D.RaiseAntennaFrames);
		C.LaserStatus = LaserNone;
		break;
	case StatusAlmostReady:
	case StatusReadyToFire:
		SetModelCondition(Packing, false);
		SetModelCondition(Unpacking, false);
		SetModelCondition(Deployed, true);
		C.LaserStatus = LaserNone;
		break;
	case StatusFiring:
		SetModelCondition(Packing, false);
		SetModelCondition(Unpacking, false);
		SetModelCondition(Deployed, true);
		C.NextLaunchFXFrame = 0;
		break;
	case StatusPacking:
		SetModelCondition(Unpacking, false);
		SetModelCondition(Deployed, false);
		SetModelCondition(Packing, true);
		break;
	default:
		break;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [cannon f%u] #%u %s: %s"), Frame, ID, *Template->Name, StatusName(NewStatus));
	C.Status = NewStatus;

	// Where its nodes, its antenna's tip and the dish are: the bones of the look with the antenna up.
	if (!C.bBonesCached)
	{
		C.bBonesCached = true;
		UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
		// (The model's hierarchy has the five node bones but no connector or fire bone, so the point the beams meet
		// at comes from the nodes themselves further down: they were authored pointing at the centre of the ring they
		// sit on, at their own height. A model that does have those bones still wins.)
		C.Connector = C.Origin = FVector3f::ZeroVector;
		if (Assets)
		{
			FGenConditionBits Up;
			Up.Set(Deployed, true);
			TArray<FTransform3f> Bones;
			bool bGotNodes = false, bGotConnector = false, bGotFire = false;
			// (The ring is on the building's own model, the dish and the emitter on the antenna's: the bones come
			// from whichever draw module's model has them, so each is taken from the first module that answers.)
			for (const FGenDrawModule& Draw : Template->Draws)
			{
				const int32 StateIndex = Draw.FindBestState(Up);
				if (!Draw.States.IsValidIndex(StateIndex) || Draw.States[StateIndex].Model.IsEmpty())
				{
					continue;
				}
				const FGenModelConditionState& State = Draw.States[StateIndex];
				const bool bLast = (State.Flags & GenStateFlags::PristineBonePosInFinalFrame) != 0;
				// (calculateUpBonePositions takes the antenna's tip and the dish where the model shows them now -
				// getCurrentClientBonePositions - and the look with the antenna up is a MANUAL animation held on its
				// last frame, START_FRAME_LAST. Its first frame has the antenna folded: the beams aimed at the wrong dish.)
				const bool bShownLast = bLast || (State.Flags & GenStateFlags::StartFrameLast) != 0;
				const FString Anim = State.Animations.Num() > 0 ? State.Animations[0] : FString();
				if (!bGotNodes)
				{
					const int32 Found = FMath::Min(Assets->GetPristineBones(State.Model, D.OuterEffectBone, 1, FMath::Min(D.OuterEffectNumBones, 5), Bones, Anim, bLast), 5);
					if (Found > 0)
					{
						bGotNodes = true;
						C.NumOuterNodes = Found;
						for (int32 i = 0; i < Found; ++i)
						{
							C.OuterNodes[i] = Bones[i].GetTranslation();
						}
						const FVector3f Centre = AimPoint(Bones);
						if (!bGotConnector)
						{
							C.Connector = Centre;
						}
						if (!bGotFire)
						{
							C.Origin = Centre;
						}
					}
				}
				if (!bGotConnector && Assets->GetPristineBones(State.Model, D.ConnectorBone, 0, 1, Bones, Anim, bShownLast) > 0)
				{
					bGotConnector = true;
					C.Connector = Bones[0].GetTranslation();
				}
				if (!bGotFire && Assets->GetPristineBones(State.Model, D.FireBone, 0, 1, Bones, Anim, bShownLast) > 0)
				{
					bGotFire = true;
					C.Origin = Bones[0].GetTranslation();
				}
			}
		}
	}

	// removeAllEffects, then what the new status shows.
	for (uint32 SystemID : C.Systems)
	{
		Logic->DestroyParticleSystem(SystemID);
	}
	C.Systems.Reset();
	C.ConnectorLaser = nullptr;
	const bool bKeepGroundBeam = NewStatus == StatusFiring || NewStatus == StatusPostfire;
	if (!bKeepGroundBeam)
	{
		C.GroundBeam.bOn = false;
	}
	auto System = [&](const FString& Name, const FVector3f& At)
	{
		if (const uint32 SystemID = Name.IsEmpty() ? 0 : Logic->CreateObjectParticleSystem(Name, ID, At))
		{
			C.Systems.Add(SystemID);
		}
	};
	auto OuterNodes = [&](ECannonIntensity I)
	{
		const FString& Name = I == Light ? D.OuterNodesLight : I == Medium ? D.OuterNodesMedium : D.OuterNodesIntense;
		for (int32 i = 0; i < C.NumOuterNodes; ++i)
		{
			System(Name, C.OuterNodes[i]);
		}
	};
	auto Connector = [&](ECannonIntensity I)
	{
		const FGenThingTemplate* Laser = FGenGameData::Get().FindTemplate(I == Intense ? D.ConnectorIntenseLaser : D.ConnectorMediumLaser);
		C.ConnectorLaser = (Laser && Laser->LaserDraw.bValid) ? &Laser->LaserDraw : nullptr;
		System(I == Intense ? D.ConnectorIntenseFlare : D.ConnectorMediumFlare, C.Connector);
	};
	auto LaserBase = [&](ECannonIntensity I)
	{
		System(I == Light ? D.LaserBaseLight : I == Medium ? D.LaserBaseMedium : D.LaserBaseIntense, C.Origin);
	};
	switch (NewStatus)
	{
	case StatusCharging:
		OuterNodes(Light);
		break;
	case StatusPreparing:
		OuterNodes(Medium);
		break;
	case StatusAlmostReady:
		OuterNodes(Medium);
		Connector(Medium);
		break;
	case StatusReadyToFire:
		OuterNodes(Medium);
		Connector(Medium);
		LaserBase(Light);
		break;
	case StatusFiring:
		// createGroundToOrbitLaser: the beam up from the antenna, widening.
		C.GroundBeam.bOn = true;
		C.GroundBeam.BornFrame = Frame;
		C.GroundBeam.GrowFrames = uint32(D.WidthGrowFrames);
		C.GroundBeam.DecayStartFrame = 0;
		OuterNodes(Intense);
		Connector(Intense);
		LaserBase(Intense);
		break;
	case StatusPostfire:
		OuterNodes(Medium);
		Connector(Medium);
		LaserBase(Medium);
		// ... narrowing again.
		C.GroundBeam.bOn = true;
		C.GroundBeam.DecayStartFrame = Frame;
		C.GroundBeam.DecayFrames = uint32(D.WidthGrowFrames);
		break;
	default:
		break;
	}
}

bool FGenObject::BeginParticleCannon(const FString& PowerName, const FVector3f& TargetPos, uint32 Frame)
{
	// ParticleUplinkCannonUpdate::initiateIntentToDoSpecialPower (the player's own order)
	if (!Template->ParticleCannon.bValid || !Template->ParticleCannon.Power.Equals(PowerName, ESearchCase::IgnoreCase))
	{
		return false;
	}
	if (!ParticleCannon)
	{
		ParticleCannon.Reset(new FGenParticleCannonState());
	}
	FGenParticleCannonState& C = *ParticleCannon;
	C.StartAttackFrame = Frame;
	C.LaserStatus = LaserNone;
	C.bManualTargetMode = true;
	C.InitialTarget = C.OverrideDestination = C.CurrentTarget = TargetPos;
	C.StartDecayFrame = C.StartAttackFrame + uint32(Template->ParticleCannon.TotalFiringFrames);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [cannon f%u] #%u %s fires at (%.0f, %.0f)"), Frame, ID, *Template->Name, TargetPos.X, TargetPos.Y);
	return true;
}

bool FGenObject::IsParticleCannonSteerable() const
{
	return ParticleCannon && !bDead && (ParticleCannon->Status == StatusPrefire || ParticleCannon->Status == StatusFiring
		|| ParticleCannon->Status == StatusPostfire);
}

bool FGenObject::SteerParticleCannon(const FString& PowerName, const FVector3f& TargetPos, uint32 Frame)
{
	// ParticleUplinkCannonUpdate::setSpecialPowerOverridableDestination, while
	// doesSpecialPowerHaveOverridableDestinationActive: the beam is told where to go next.
	if (!ParticleCannon || !Template->ParticleCannon.Power.Equals(PowerName, ESearchCase::IgnoreCase) || IsDisabled())
	{
		return false;
	}
	FGenParticleCannonState& C = *ParticleCannon;
	if (C.Status != StatusPrefire && C.Status != StatusFiring && C.Status != StatusPostfire)
	{
		return false;
	}
	C.OverrideDestination = TargetPos;
	C.bManualTargetMode = true;
	C.SecondLastClickFrame = C.LastClickFrame;
	C.LastClickFrame = Frame;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [cannon f%u] #%u %s: the beam is sent to (%.0f, %.0f)"), Frame, ID, *Template->Name, TargetPos.X, TargetPos.Y);
	return true;
}

void FGenObject::UpdateParticleCannon(uint32 Frame)
{
	// ParticleUplinkCannonUpdate::update
	GEN_PERF_SCOPE("Obj.ParticleCannon");
	const FGenParticleCannon& D = Template->ParticleCannon;
	if (!ParticleCannon)
	{
		ParticleCannon.Reset(new FGenParticleCannonState());
	}
	FGenParticleCannonState& C = *ParticleCannon;
	if (IsBeingSold() || bDead)
	{
		if (C.Status != StatusIdle)
		{
			SetParticleCannonStatus(StatusIdle, Frame);
			C.OrbitBeam.bOn = false;
			C.StartAttackFrame = 0;
		}
		return;
	}
	if (IsUnderConstruction())
	{
		return;
	}
	const int32 Module = Logic->FindPowerModule(*this, D.Power);
	if (Module == INDEX_NONE)
	{
		return;
	}
	const int32 Now = int32(Frame);
	const int32 ReadyToFireFrame = Logic->IsPowerReady(*this, Module) ? Now : int32(GetPowerReadyFrame(Module));
	const int32 AlmostReadyFrame = ReadyToFireFrame - D.ReadyDelayFrames;
	const int32 RaiseAntennaFrame = AlmostReadyFrame - D.RaiseAntennaFrames;
	const int32 BeginChargeFrame = RaiseAntennaFrame - D.BeginChargeFrames;

	if (C.StartAttackFrame != 0 && C.StartAttackFrame <= Frame)
	{
		if (C.StartDecayFrame > Frame && IsDisabled())
		{
			C.StartDecayFrame = Frame; // "We must end the special power early" - no power, EMP, hacked
		}
		const uint32 EndDecayFrame = C.StartDecayFrame + uint32(D.WidthGrowFrames);
		const uint32 OrbitalBirthFrame = C.StartAttackFrame + uint32(D.BeamTravelFrames);
		const uint32 OrbitalDecayStart = C.StartDecayFrame + uint32(D.BeamTravelFrames);
		const uint32 OrbitalDeathFrame = OrbitalDecayStart + uint32(D.WidthGrowFrames);
		const FGenThingTemplate* BeamThing = FGenGameData::Get().FindTemplate(D.ParticleBeamLaser);
		const float BeamHalfWidth = (BeamThing && BeamThing->LaserDraw.bValid) ? BeamThing->LaserDraw.OuterBeamWidth * 0.5f : 0.f;

		switch (C.LaserStatus)
		{
		case LaserNone:
			if (OrbitalBirthFrame <= Frame)
			{
				// createOrbitToTargetLaser
				C.OrbitBeam.bOn = true;
				C.OrbitBeam.BornFrame = Frame;
				C.OrbitBeam.GrowFrames = uint32(D.WidthGrowFrames);
				C.OrbitBeam.DecayStartFrame = 0;
				C.CurrentTarget = C.InitialTarget;
				C.LaserStatus = LaserBorn;
				C.ScorchMarksMade = 0;
				C.NextScorchFrame = Frame;
				C.DamagePulsesMade = 0;
				C.NextDamagePulseFrame = Frame;
			}
			break;
		case LaserBorn:
			if (OrbitalDecayStart <= Frame)
			{
				C.OrbitBeam.DecayStartFrame = Frame;
				C.OrbitBeam.DecayFrames = uint32(D.WidthGrowFrames);
				C.LaserStatus = LaserDecaying;
			}
			break;
		case LaserDecaying:
			if (OrbitalDeathFrame <= Frame)
			{
				C.OrbitBeam.bOn = false;
				C.LaserStatus = LaserDead;
				C.StartAttackFrame = 0;
				SetParticleCannonStatus(StatusIdle, Frame);
			}
			break;
		default:
			break;
		}

		if (C.OrbitBeam.bOn && OrbitalBirthFrame <= Frame && Frame <= OrbitalDeathFrame)
		{
			if (!C.bManualTargetMode)
			{
				// The "swath of death": a sine across the line from the building to the target.
				const float Factor = float(Frame - OrbitalBirthFrame) / float(FMath::Max(1u, OrbitalDeathFrame - OrbitalBirthFrame));
				const float Along = Factor * D.SwathDistance - D.SwathDistance * 0.5f;
				const float Across = FMath::Sin(Factor * 2.f * PI - PI) * D.SwathAmplitude;
				const FVector2f ToTarget(C.InitialTarget.X - Position.X, C.InitialTarget.Y - Position.Y);
				const FVector2f Dir = ToTarget.GetSafeNormal();
				const FVector2f Spot = FVector2f(Position.X, Position.Y) + Dir * (ToTarget.Size() + Along) + FVector2f(-Dir.Y, Dir.X) * Across;
				C.CurrentTarget.X = Spot.X;
				C.CurrentTarget.Y = Spot.Y;
			}
			else
			{
				// Steered: towards where it was last sent, faster after a double click.
				float Speed = D.ManualDrivingSpeed;
				if (int32(C.LastClickFrame - C.SecondLastClickFrame) < D.DoubleClickFrames)
				{
					Speed = D.ManualFastDrivingSpeed;
				}
				Speed /= float(GenLogic::GetLogicFPS());
				FVector2f Way(C.OverrideDestination.X - C.CurrentTarget.X, C.OverrideDestination.Y - C.CurrentTarget.Y);
				const float Distance = Way.Size();
				Way = Way.GetSafeNormal() * FMath::Min(Speed, Distance);
				C.CurrentTarget.X += Way.X;
				C.CurrentTarget.Y += Way.Y;
			}
			C.CurrentTarget.Z = Logic->GetGroundHeight(C.CurrentTarget.X, C.CurrentTarget.Y);

			const float Radius = BeamHalfWidth * C.OrbitBeam.Scale(Frame);
			const float Span = float(FMath::Max(1u, OrbitalDeathFrame - OrbitalBirthFrame));
			if (C.NextScorchFrame <= Frame)
			{
				++C.ScorchMarksMade;
				Logic->AddScorch(C.CurrentTarget, Radius * D.ScorchMarkScalar, Logic->GetRandom().RandRange(0, 3));
				C.NextScorchFrame = OrbitalBirthFrame + uint32(float(C.ScorchMarksMade) / float(FMath::Max(1, D.TotalScorchMarks)) * Span);
				Logic->DoFXPos(D.GroundHitFX, C.CurrentTarget, nullptr, 0.f, nullptr, 0.f);
				Logic->RevealAreaOnce(Owner, FVector2f(C.CurrentTarget.X, C.CurrentTarget.Y), D.RevealRange);
			}
			if (C.NextDamagePulseFrame <= Frame)
			{
				++C.DamagePulsesMade;
				const float FiringSeconds = float(D.TotalFiringFrames) / float(GenLogic::GetLogicFPS());
				const float DamagePerPulse = FiringSeconds * D.DamagePerSecond / float(FMath::Max(1, D.TotalDamagePulses));
				// (Who is in reach is taken down first: what dies leaves wrecks behind.)
				TArray<FGenObjectID, TInlineAllocator<32>> InRange;
				Logic->GetPartition().ForEachInRange(FVector2f(C.CurrentTarget.X, C.CurrentTarget.Y), Radius * D.DamageRadiusScalar, [&](FGenObject& Other)
				{
					if (!Other.bDead && !Other.bPendingDestroy)
					{
						InRange.Add(Other.GetID());
					}
				});
				for (FGenObjectID OtherId : InRange)
				{
					if (FGenObject* Other = Logic->FindObject(OtherId))
					{
						Other->AttemptDamage(DamagePerPulse, D.DamageType, D.DeathType, ID);
					}
				}
				if (const FGenThingTemplate* Remnant = D.RemnantObject.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(D.RemnantObject))
				{
					Logic->CreateObject(Remnant, Owner, C.CurrentTarget, 0.f);
				}
				C.NextDamagePulseFrame = OrbitalBirthFrame + uint32(float(C.DamagePulsesMade) / float(FMath::Max(1, D.TotalDamagePulses)) * Span);
				UE_CLOG(Logic->IsTracing() && (C.DamagePulsesMade % 8) == 1, LogGenerals, Log,
					TEXT("  [cannon f%u] #%u pulse %d of %d at (%.0f, %.0f): %.0f damage to %d things within %.0f"), Frame, ID, C.DamagePulsesMade,
					D.TotalDamagePulses, C.CurrentTarget.X, C.CurrentTarget.Y, DamagePerPulse, InRange.Num(), Radius * D.DamageRadiusScalar);
			}
		}

		if (C.StartAttackFrame != 0)
		{
			SetParticleCannonStatus(EndDecayFrame <= Frame ? StatusPacking : C.StartDecayFrame <= Frame ? StatusPostfire : StatusFiring, Frame);
		}
	}
	else if (ReadyToFireFrame <= Now)
	{
		SetParticleCannonStatus(StatusReadyToFire, Frame);
	}
	else if (AlmostReadyFrame <= Now)
	{
		SetParticleCannonStatus(StatusAlmostReady, Frame);
	}
	else if (RaiseAntennaFrame <= Now)
	{
		SetParticleCannonStatus(StatusPreparing, Frame);
	}
	else if (BeginChargeFrame <= Now)
	{
		SetParticleCannonStatus(StatusCharging, Frame);
	}
	else if (C.Status == StatusReadyToFire)
	{
		SetParticleCannonStatus(StatusPacking, Frame);
	}

	if (C.Status == StatusFiring && C.NextLaunchFXFrame <= Frame)
	{
		const FVector3f OriginWorld = GetTransform().TransformPosition(C.Origin);
		Logic->DoFXPos(D.BeamLaunchFX, OriginWorld, nullptr, 0.f, nullptr, 0.f);
		C.NextLaunchFXFrame = Frame + uint32(FMath::Max(1, D.FramesBetweenLaunchFX));
	}
}

void FGenObject::GetParticleCannonBeams(uint32 Frame, TArray<UGenGameLogic::FBeam>& Out) const
{
	if (!ParticleCannon)
	{
		return;
	}
	const FGenParticleCannonState& C = *ParticleCannon;
	const FGenParticleCannon& D = Template->ParticleCannon;
	const FTransform3f Xf = GetTransform();
	// The lasers from the outer nodes to the antenna (seen with the building).
	const bool bSeen = GetLocalShroud() != EGenObjectShroud::Fogged && GetLocalShroud() != EGenObjectShroud::Shrouded;
	if (C.ConnectorLaser && bSeen)
	{
		for (int32 i = 0; i < C.NumOuterNodes; ++i)
		{
			UGenGameLogic::FBeam& B = Out.AddDefaulted_GetRef();
			B.Draw = C.ConnectorLaser;
			B.Start = Xf.TransformPosition(C.OuterNodes[i]);
			B.End = Xf.TransformPosition(C.Connector);
		}
	}
	const FGenThingTemplate* BeamThing = FGenGameData::Get().FindTemplate(D.ParticleBeamLaser);
	if (!BeamThing || !BeamThing->LaserDraw.bValid)
	{
		return;
	}
	if (C.GroundBeam.bOn && bSeen && C.GroundBeam.Scale(Frame) > 0.f)
	{
		UGenGameLogic::FBeam& B = Out.AddDefaulted_GetRef();
		B.Draw = &BeamThing->LaserDraw;
		B.Start = Xf.TransformPosition(C.Origin);
		B.End = B.Start + FVector3f(0.f, 0.f, OrbitHeight);
		B.WidthScale = C.GroundBeam.Scale(Frame);
	}
	if (C.OrbitBeam.bOn && C.OrbitBeam.Scale(Frame) > 0.f)
	{
		UGenGameLogic::FBeam& B = Out.AddDefaulted_GetRef();
		B.Draw = &BeamThing->LaserDraw;
		B.Start = C.CurrentTarget + FVector3f(0.f, 0.f, OrbitHeight);
		B.End = C.CurrentTarget;
		B.WidthScale = C.OrbitBeam.Scale(Frame);
	}
}
