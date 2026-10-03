// Effects side of the game logic: FX lists played for weapons and deaths, and ObjectCreationList
// (ObjectCreationList.cpp GenericObjectCreationNugget) spawning debris and hulks.

#include "Logic/GenGameLogic.h"
#include "Client/GenFXData.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapActor.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"

namespace
{
	/** calcRandomForce: a force of random magnitude and pitch in a random horizontal direction. */
	FVector3f RandomForce(FRandomStream& R, float MinMag, float MaxMag, float MinPitch, float MaxPitch)
	{
		const float Angle = R.FRandRange(0.f, 2.f * PI);
		const float Pitch = R.FRandRange(FMath::Min(MinPitch, MaxPitch), FMath::Max(MinPitch, MaxPitch));
		const float Mag = R.FRandRange(FMath::Min(MinMag, MaxMag), FMath::Max(MinMag, MaxMag));
		return FVector3f(FMath::Cos(Pitch) * FMath::Cos(Angle), FMath::Cos(Pitch) * FMath::Sin(Angle), FMath::Sin(Pitch)) * Mag;
	}
}

void UGenGameLogic::DoFXObj(const FString& FXName, const FGenObject& Primary, const FGenObject* Secondary)
{
	if (FX)
	{
		FX->DoFXObj(FGenFXData::Get().FindFXList(FXName), &Primary, Secondary);
	}
}

void UGenGameLogic::DoFXPos(const FString& FXName, const FVector3f& Pos, const FTransform3f* Xf, float Speed, const FVector3f* Secondary, float Radius)
{
	if (FX)
	{
		FX->DoFXPos(FGenFXData::Get().FindFXList(FXName), Pos, Xf, Speed, Secondary, Radius);
	}
}

void UGenGameLogic::AttachParticleSystem(const FString& TemplateName, FGenObjectID Object)
{
	if (FX)
	{
		FX->CreateAttachedSystem(TemplateName, Object);
	}
}

bool UGenGameLogic::GetWeaponBoneTransform(const FGenObject& Obj, int32 Slot, bool bLaunchBone, FTransform3f& OutWorld) const
{
	// The original also asks the drawable for bone positions (W3DModelDraw::handleWeaponFireFX,
	// getProjectileLaunchOffset): the pose is the one shown, turret angle included.
	const TObjectPtr<UGenModelComponent>* MC = Drawables.Find(Obj.GetID());
	const FDrawState* DS = DrawStates.Find(Obj.GetID());
	const FGenDrawModule* Draw = Obj.GetTemplate()->PrimaryDraw();
	if (!MC || !*MC || !DS || !Draw || !Draw->States.IsValidIndex(DS->StateIndex) || Slot < 0 || Slot > 2)
	{
		return false;
	}
	const FGenModelConditionState& State = Draw->States[DS->StateIndex];
	const FString& BoneName = bLaunchBone ? State.LaunchBone[Slot] : State.FireFXBone[Slot];
	FTransform3f Bone;
	if (BoneName.IsEmpty() || !(*MC)->GetBoneTransformW3D(BoneName, Bone))
	{
		return false;
	}
	OutWorld = Bone * Obj.GetTransform();
	return true;
}

void UGenGameLogic::DoWeaponFireFX(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon, const FVector3f& TargetPos)
{
	const FGenFXList* List = FX ? FGenFXData::Get().FindFXList(Weapon.FireFX) : nullptr;
	if (!List)
	{
		return;
	}
	FTransform3f Xf;
	if (!GetWeaponBoneTransform(Shooter, Slot, false, Xf))
	{
		Xf = Shooter.GetTransform();
	}
	FX->DoFXPos(List, Xf.GetTranslation(), &Xf, Weapon.WeaponSpeed, &TargetPos, Weapon.PrimaryDamageRadius);
}

void UGenGameLogic::SetDebrisModel(FGenObject& Obj, const FString& Model, bool bUseHouseColor)
{
	if (!MapActor)
	{
		return;
	}
	const int32 OwnerIndex = Obj.GetOwner();
	const FLinearColor Color = bUseHouseColor && Players.IsValidIndex(OwnerIndex) && !Players[OwnerIndex].bNeutral
		? Players[OwnerIndex].Color : FLinearColor(0.8f, 0.8f, 0.8f);
	if (const TObjectPtr<UGenModelComponent>* Existing = Drawables.Find(Obj.GetID()))
	{
		if (*Existing) { (*Existing)->SetModel(Model); }
		return;
	}
	if (UGenModelComponent* MC = MapActor->CreateModelVisual(Model, Color))
	{
		Drawables.Add(Obj.GetID(), MC);
		DrawStates.Add(Obj.GetID(), FDrawState());
	}
}

void UGenGameLogic::DoOCL(const FString& OCLName, const FGenObject& Source)
{
	const FGenOCL* OCL = FGenFXData::Get().FindOCL(OCLName);
	if (!OCL)
	{
		return;
	}
	const FGenGameData& GD = FGenGameData::Get();
	static const FString DebrisTemplateName(TEXT("GenericDebris"));
	const FGenThingTemplate* DebrisTemplate = GD.FindTemplate(DebrisTemplateName);

	// Forces and spin rates in the data are per 30 Hz frame.
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS());
	const FVector3f SourcePos = Source.GetPosition();
	const FTransform3f SourceXf = Source.GetTransform();
	const bool bSourceAirborne = SourcePos.Z > GetGroundHeight(SourcePos.X, SourcePos.Y) + 9.f;

	for (const FGenOCLNugget& N : OCL->Nuggets)
	{
		if (N.Names.Num() == 0 || (N.bSkipIfSignificantlyAirborne && bSourceAirborne))
		{
			continue;
		}
		for (int32 n = 0; n < N.Count; ++n)
		{
			const FString& Pick = N.Names[Random.RandRange(0, N.Names.Num() - 1)];
			const FGenThingTemplate* T = N.bDebris ? DebrisTemplate : GD.FindTemplate(Pick);
			FGenObject* O = T ? CreateObject(T, Source.GetOwner(), SourcePos, Source.GetOrientation()) : nullptr;
			if (!O)
			{
				continue;
			}
			// GenericObjectCreationNugget::doStuffToObj
			O->Producer = Source.GetID();
			if (T->bHasLifetime && N.MaxFrames > 0)
			{
				O->LifetimeDieFrame = Frame + uint32(FMath::Max(1, Random.RandRange(N.MinFrames, FMath::Max(N.MinFrames, N.MaxFrames))));
			}
			if (N.bDebris)
			{
				SetDebrisModel(*O, Pick, N.bOkToChangeModelColor);
				if (N.Mass > 0.f) { O->Mass = N.Mass; }
			}
			FVector3f ChunkPos = SourcePos + SourceXf.TransformVectorNoScale(N.Offset);
			if (!N.ParticleSystem.IsEmpty())
			{
				AttachParticleSystem(N.ParticleSystem, O->GetID());
			}
			if (N.MinHealth < 1.f || N.MaxHealth < 1.f)
			{
				O->Health = O->MaxHealth * Random.FRandRange(FMath::Min(N.MinHealth, N.MaxHealth), FMath::Max(N.MinHealth, N.MaxHealth));
			}

			const uint32 D = N.Disposition;
			const bool bPhysics = T->bHasPhysics;
			if ((D & GenDisposition::InheritVelocity) != 0 && bPhysics)
			{
				O->ApplyForce(Source.GetVelocity());
				O->bFreePhysics = true;
			}
			if ((D & GenDisposition::LikeExisting) != 0)
			{
				O->SetRotation(SourceXf.GetRotation());
				O->SetPosition(ChunkPos);
				O->bFreeRotation = Source.HasFreeRotation();
				if (bSourceAirborne && bPhysics) { O->bFreePhysics = true; } // setAllowToFall
			}
			if ((D & GenDisposition::OnGroundAligned) != 0)
			{
				O->SetOrientation(Random.FRandRange(0.f, 2.f * PI));
				ChunkPos.Z = GetGroundHeight(ChunkPos.X, ChunkPos.Y);
				O->SetPosition(ChunkPos);
			}
			if ((D & GenDisposition::SendItOut) != 0)
			{
				O->SetOrientation(Random.FRandRange(0.f, 2.f * PI));
				ChunkPos.Z = GetGroundHeight(ChunkPos.X, ChunkPos.Y);
				O->SetPosition(ChunkPos);
				if (bPhysics)
				{
					O->ExtraFriction = N.ExtraFriction;
					const float Horiz = 4.f * N.DispositionIntensity;
					const FVector3f Force(Random.FRandRange(-Horiz, Horiz), Random.FRandRange(-Horiz, Horiz), 0.f);
					O->ApplyForce(Force * RateScale);
					O->bFreePhysics = true;
				}
			}
			if ((D & (GenDisposition::SendItFlying | GenDisposition::SendItUp | GenDisposition::RandomForce)) != 0)
			{
				O->SetRotation(SourceXf.GetRotation());
				O->SetPosition(ChunkPos);
				if (bPhysics)
				{
					O->ExtraFriction = N.ExtraFriction;
					O->bAllowBounce = true;

					// Spin: an explicit rate, or one derived from the intensity; per-axis rates override it.
					const float Spin = N.SpinRate >= 0.f ? N.SpinRate : (PI / 32.f) * N.DispositionIntensity * RateScale;
					const float YawR = N.YawRate >= 0.f ? N.YawRate : Spin;
					const float RollR = N.RollRate >= 0.f ? N.RollRate : Spin;
					const float PitchR = N.PitchRate >= 0.f ? N.PitchRate : Spin;
					const float Yaw = Random.FRandRange(-YawR, YawR);
					const float Roll = Random.FRandRange(-RollR, RollR);
					const float Pitch = Random.FRandRange(-PitchR, PitchR);

					FVector3f Force;
					if ((D & GenDisposition::SendItFlying) != 0)
					{
						const float Horiz = 4.f * N.DispositionIntensity, Vert = 3.f * N.DispositionIntensity;
						Force = FVector3f(Random.FRandRange(-Horiz, Horiz), Random.FRandRange(-Horiz, Horiz), Random.FRandRange(Vert * 0.33f, Vert));
					}
					else if ((D & GenDisposition::SendItUp) != 0)
					{
						const float Horiz = 2.f * N.DispositionIntensity, Vert = 4.f * N.DispositionIntensity;
						Force = FVector3f(Random.FRandRange(-Horiz, Horiz), Random.FRandRange(-Horiz, Horiz), Random.FRandRange(Vert * 0.75f, Vert));
					}
					else
					{
						Force = RandomForce(Random, N.MinMag, N.MaxMag, N.MinPitch, N.MaxPitch);
					}
					O->ApplyForce(Force * RateScale);
					// setAngles(orientation, 0, 0): upright, facing the source's heading (or the throw direction).
					O->SetOrientation(N.bOrientInForceDirection ? FMath::Atan2(Force.Y, Force.X) : Source.GetOrientation());
					O->YawRate = Yaw;
					O->RollRate = Roll;
					O->PitchRate = Pitch;
					O->bFreePhysics = true;
					O->bFreeRotation = true;
				}
			}
			if ((D & GenDisposition::Whirling) != 0 && bPhysics)
			{
				const float I = N.DispositionIntensity * RateScale;
				O->YawRate = Random.FRandRange(-I, I);
				O->RollRate = Random.FRandRange(-I, I);
				O->PitchRate = Random.FRandRange(-I, I);
				O->bFreePhysics = true;
				O->bFreeRotation = true;
			}
			O->SnapshotPrevious();
		}
	}
}
