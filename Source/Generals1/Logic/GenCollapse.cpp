// How civilian buildings come down: StructureCollapseUpdate.cpp (a shudder, then down into the ground in
// bursts of debris) and StructureToppleUpdate.cpp (a tower falls over, away from what killed it, and
// what it falls on is crushed). Either way the ruin stays where the building stood.

#include "Logic/GenObject.h"
#include "Logic/GenConditions.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Generals1.h"

namespace
{
	int32 PostCollapseBit() { static const int32 I = GenConditions::Index(TEXT("POST_COLLAPSE")); return I; }
}

// ---- StructureCollapseUpdate --------------------------------------------------------------------

void FGenObject::DoCollapsePhase(int32 Phase, const FVector3f& At)
{
	// StructureCollapseUpdate::doPhaseStuff: one of the phase's effects and one of its creation lists.
	const FGenStructureCollapse& C = Template->Collapse;
	FRandomStream& Rand = Logic->GetRandom();
	if (C.FX[Phase].Num() > 0)
	{
		Logic->DoFXPos(C.FX[Phase][Rand.RandRange(0, C.FX[Phase].Num() - 1)], At, nullptr, 0.f, nullptr, 0.f);
	}
	if (C.OCL[Phase].Num() > 0)
	{
		Logic->DoOCL(C.OCL[Phase][Rand.RandRange(0, C.OCL[Phase].Num() - 1)], *this, &At);
	}
}

void FGenObject::BeginStructureCollapse(uint32 Frame)
{
	// StructureCollapseUpdate::beginStructureCollapse
	const FGenStructureCollapse& C = Template->Collapse;
	CollapseBasePos = Position;
	CollapseFrame = Frame + uint32(Logic->GetRandom().RandRange(C.MinCollapseDelay, FMath::Max(C.MinCollapseDelay, C.MaxCollapseDelay)));
	CollapseState = ECollapseState::Waiting;
	bStructureToppling = false;
	CollapseHeight = 0.f;
	CollapseVelocity = 0.f;
	DestroyFrame = MAX_uint32;
	DoCollapsePhase(FGenStructureCollapse::Initial, CollapseBasePos);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [collapse f%u] #%u %s will come down in %u frames"), Frame, ID, *Template->Name, CollapseFrame - Frame);
}

void FGenObject::UpdateStructureCollapse(uint32 Frame)
{
	// StructureCollapseUpdate::update
	const FGenStructureCollapse& C = Template->Collapse;
	if (CollapseState == ECollapseState::Done)
	{
		return;
	}
	FRandomStream& Rand = Logic->GetRandom();
	const FVector3f Shudder(Rand.FRandRange(-C.MaxShudder, C.MaxShudder), Rand.FRandRange(-C.MaxShudder, C.MaxShudder), 0.f);
	if (CollapseState == ECollapseState::Waiting)
	{
		// "We are in the dramatic pause between when the building has lost all its hit points and when it starts toppling over."
		Position = CollapseBasePos + Shudder;
		if (Frame >= CollapseFrame)
		{
			CollapseState = ECollapseState::Falling;
			DoCollapsePhase(FGenStructureCollapse::Burst, CollapseBasePos);
			CollapseBurstFrame = Frame + uint32(Rand.RandRange(C.MinBurstDelay, FMath::Max(C.MinBurstDelay, C.MaxBurstDelay)));
		}
	}
	if (CollapseState == ECollapseState::Falling)
	{
		CollapseHeight -= CollapseVelocity;
		CollapseVelocity -= FGenGameData::Get().Gravity * (1.f - C.CollapseDamping);
		Position = CollapseBasePos + Shudder + FVector3f(0.f, 0.f, CollapseHeight);
		if (Frame >= CollapseBurstFrame)
		{
			const bool bBig = C.BigBurstFrequency >= 1 && Rand.RandRange(1, C.BigBurstFrequency) == 1;
			DoCollapsePhase(bBig ? FGenStructureCollapse::Burst : FGenStructureCollapse::Delay, CollapseBasePos);
			CollapseBurstFrame += uint32(FMath::Max(1, Rand.RandRange(C.MinBurstDelay, FMath::Max(C.MinBurstDelay, C.MaxBurstDelay))));
		}
		if (CollapseHeight + Template->GetMaxHeightAbovePosition() <= 0.f)
		{
			// Down: what shows from now on is the heap it leaves.
			CollapseState = ECollapseState::Done;
			DoCollapsePhase(FGenStructureCollapse::Final, CollapseBasePos);
			SetModelCondition(GenConditions::Rubble(), false);
			SetModelCondition(PostCollapseBit(), true);
			Position = CollapseBasePos;
			SetOrientation(Orientation);
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [collapse f%u] #%u %s is down"), Frame, ID, *Template->Name);
		}
	}
}

// ---- StructureToppleUpdate ----------------------------------------------------------------------

void FGenObject::DoStructureTopplePhase(int32 Phase, const FVector3f& At)
{
	const TArray<FString>& List = Template->StructureTopple.OCL[Phase];
	if (List.Num() > 0)
	{
		Logic->DoOCL(List[Logic->GetRandom().RandRange(0, List.Num() - 1)], *this, &At);
	}
}

void FGenObject::BeginStructureTopple(uint32 Frame, FGenObjectID Killer)
{
	// StructureToppleUpdate::beginStructureTopple: away from whoever did it, give or take a little.
	const FGenStructureTopple& S = Template->StructureTopple;
	FRandomStream& Rand = Logic->GetRandom();
	CollapseBasePos = Position;
	CollapseFrame = Frame + uint32(Rand.RandRange(S.MinToppleDelay, FMath::Max(S.MinToppleDelay, S.MaxToppleDelay)));
	float Angle;
	const FGenObject* Attacker = Logic->FindObject(Killer);
	if (Attacker && Attacker != this)
	{
		Angle = FMath::Atan2(Position.Y - Attacker->Position.Y, Position.X - Attacker->Position.X) + Rand.FRandRange(-PI / 8.f, PI / 8.f);
	}
	else
	{
		Angle = Rand.FRandRange(0.f, 2.f * PI);
	}
	StructureToppleDirection = FVector2f(FMath::Cos(Angle), FMath::Sin(Angle));
	const float BurstRadius = (Template->GeometryMajorRadius + Template->GeometryMinorRadius) * 0.5f * 0.9f;
	DelayBurstLocation = FVector3f(Position.X + BurstRadius * StructureToppleDirection.X, Position.Y + BurstRadius * StructureToppleDirection.Y, 0.f);
	DelayBurstLocation.Z = Logic->GetGroundHeight(DelayBurstLocation.X, DelayBurstLocation.Y);
	// doToppleStartFX
	Logic->DoFXPos(S.ToppleStartFX, Position, nullptr, 0.f, nullptr, 0.f);
	DoStructureTopplePhase(FGenStructureTopple::Initial, Position);
	CollapseBurstFrame = Frame + uint32(Rand.RandRange(S.MinBurstDelay, FMath::Max(S.MinBurstDelay, S.MaxBurstDelay)));
	CollapseState = ECollapseState::Waiting;
	bStructureToppling = true;
	CollapseVelocity = 0.f;
	StructureToppleAngle = 0.001f;
	LastCrushedLocation = 0.f;
	DestroyFrame = MAX_uint32;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [collapse f%u] #%u %s will fall towards (%.2f, %.2f) in %u frames"), Frame, ID, *Template->Name,
		StructureToppleDirection.X, StructureToppleDirection.Y, CollapseFrame - Frame);
}

void FGenObject::UpdateStructureTopple(uint32 Frame)
{
	// StructureToppleUpdate::update
	static constexpr float ToppleAccelerationFactor = 0.02f;
	const FGenStructureTopple& S = Template->StructureTopple;
	FRandomStream& Rand = Logic->GetRandom();
	auto DelayBurst = [&]()
	{
		// doToppleDelayBurstFX
		Logic->DoFXPos(S.ToppleDelayFX, DelayBurstLocation, nullptr, 0.f, nullptr, 0.f);
		DoStructureTopplePhase(FGenStructureTopple::Delay, DelayBurstLocation);
		CollapseBurstFrame = Frame + uint32(FMath::Max(1, Rand.RandRange(S.MinBurstDelay, FMath::Max(S.MinBurstDelay, S.MaxBurstDelay))));
	};

	if (CollapseState == ECollapseState::Waiting)
	{
		if (Frame >= CollapseBurstFrame)
		{
			DelayBurst();
		}
		if (Frame >= CollapseFrame)
		{
			CollapseState = ECollapseState::Falling;
			StructureIntegrity = S.StructuralIntegrity;
			bFreeRotation = true;
		}
	}
	if (CollapseState == ECollapseState::Falling)
	{
		// The further over it is, the faster it goes - held back at first by what is left of its structure.
		CollapseVelocity += ToppleAccelerationFactor * (FMath::Sin(StructureToppleAngle) * (1.f - StructureIntegrity));
		if (StructureIntegrity > 0.f)
		{
			StructureIntegrity = FMath::Max(0.f, StructureIntegrity * S.StructuralDecay);
		}
		// doAngleFX
		for (const TPair<float, FString>& A : S.AngleFX)
		{
			if (A.Key > StructureToppleAngle && A.Key <= StructureToppleAngle + CollapseVelocity)
			{
				Logic->DoFXObj(A.Value, *this, nullptr);
			}
		}
		StructureToppleAngle += CollapseVelocity;
		ApplyStructureCrushingDamage(PI / 2.f - StructureToppleAngle);
		if (StructureToppleAngle >= PI / 2.f)
		{
			CollapseVelocity -= StructureToppleAngle - PI / 2.f;
			StructureToppleAngle = PI / 2.f;
			CollapseState = ECollapseState::WaitingForDone;
			ApplyStructureCrushingDamage(0.f);
			DoStructureTopplePhase(FGenStructureTopple::Final, Position);
			Logic->DoFXObj(S.ToppleDoneFX, *this, nullptr);
		}
		if (Frame >= CollapseBurstFrame)
		{
			DelayBurst();
		}
		const FVector3f Axis(-StructureToppleDirection.Y, StructureToppleDirection.X, 0.f);
		Rotation = (FQuat4f(Axis, CollapseVelocity) * Rotation).GetNormalized();
	}
	else if (CollapseState == ECollapseState::WaitingForDone)
	{
		// doToppleDoneStuff: the heap lies the way it fell.
		SetModelCondition(GenConditions::Rubble(), false);
		SetModelCondition(PostCollapseBit(), true);
		SetOrientation(FMath::Atan2(StructureToppleDirection.Y, StructureToppleDirection.X));
		bFreeRotation = false;
		CollapseState = ECollapseState::Done;
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [collapse f%u] #%u %s is down"), Frame, ID, *Template->Name);
	}
}

void FGenObject::ApplyStructureCrushingDamage(float Theta)
{
	// StructureToppleUpdate::applyCrushingDamage: "Fire explosions at regular intervals across the area
	// that the building is currently crushing. The explosions occur across the face and along the length
	// of the building."
	static constexpr float ThetaCeiling = PI / 6.f;       // "This weapon won't do any damage until theta is less than this value."
	static constexpr float WeaponSpacing = 25.f;
	const FGenStructureTopple& S = Template->StructureTopple;
	const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(S.CrushingWeapon);
	if (Theta > ThetaCeiling || !W)
	{
		return;
	}
	const float ToppleAngle = FMath::Atan2(StructureToppleDirection.Y, StructureToppleDirection.X);
	const float Angle = Orientation - ToppleAngle;
	const float FacingWidth = FVector2f(Template->GeometryMajorRadius * FMath::Sin(Angle), Template->GeometryMinorRadius * FMath::Cos(Angle)).Size() * 0.5f;
	const float MaxDistance = Template->GetMaxHeightAbovePosition() * (1.f - FMath::Sin(Theta));
	const float CosT = FMath::Cos(ToppleAngle), SinT = FMath::Sin(ToppleAngle);
	auto Strike = [&](float X, float Y)
	{
		const FVector3f Target(X, Y, Logic->GetGroundHeight(X, Y));
		Logic->DealDamage(*W, ID, GenInvalidObjectID, Target, true);
		Logic->DoFXPos(S.CrushingFX, Target, nullptr, 0.f, nullptr, 0.f);
	};
	auto DamageLine = [&](float Along)
	{
		// doDamageLine
		const float BX = CollapseBasePos.X + Along * CosT, BY = CollapseBasePos.Y + Along * SinT;
		for (float i = -FacingWidth; i < FacingWidth; i += WeaponSpacing)
		{
			Strike(BX + i * SinT, BY + i * CosT);
		}
		Strike(BX + FacingWidth * SinT, BY + FacingWidth * CosT);
		DoStructureTopplePhase(FGenStructureTopple::Final, FVector3f(BX, BY, Logic->GetGroundHeight(BX, BY)));
	};
	float j = LastCrushedLocation;
	for (; j < MaxDistance; j += WeaponSpacing)
	{
		DamageLine(j);
	}
	DamageLine(MaxDistance);
	LastCrushedLocation = j;
}
