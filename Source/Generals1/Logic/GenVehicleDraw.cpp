// What a vehicle's model does as it drives, drawn frame by frame: wheels that roll, steer and ride their
// springs, a tractor's cab and trailer swinging into turns, treads that roll (and turn against each other
// when the tank turns on the spot), and the dust, dirt and tread debris thrown up. Follows W3DTankDraw,
// W3DTruckDraw and W3DTankTruckDraw (doDrawModule). The original steps all of this once per drawn 30 Hz
// frame; here every drawn frame takes its share of that, so it runs as smoothly as the screen does.

#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioManager.h"
#include "Client/GenFXManager.h"
#include "Client/GenParticles.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"

namespace
{
	enum EVehicleSystem { TreadDebrisLeft, TreadDebrisRight, Dust, Dirt, Powerslide, NumVehicleSystems };
	constexpr int32 PowerslideLoopSlot = 8; // (0 moving, 1 ambient, 2-4 weapons, 5 a helicopter going down, 6 abilities, 7 the turret)
}

FGenParticleSystem* UGenGameLogic::GetVehicleSystem(FDrawState& DS, int32 Which, const FString& TemplateName, const FGenObject& O, bool bCreate)
{
	uint32& Id = DS.VehicleSystems[Which];
	if (FGenParticleSystem* Sys = FX ? FX->FindSystem(Id) : nullptr)
	{
		return Sys;
	}
	Id = 0;
	if (!bCreate || !FX || TemplateName.IsEmpty())
	{
		return nullptr;
	}
	FGenParticleSystem* Sys = FX->CreateAttachedSystem(TemplateName, O.GetID());
	if (Sys)
	{
		Sys->Stop(); // ("they come into being stopped")
		Id = Sys->GetID();
	}
	return Sys;
}

void UGenGameLogic::StopVehicleEffects(FDrawState& DS, const FGenObject& O)
{
	// setHidden / setFullyObscuredByShroud: no more dirt from what can't be seen; its powerslide goes quiet.
	for (int32 i = 0; i < NumVehicleSystems; ++i)
	{
		if (FGenParticleSystem* Sys = FX ? FX->FindSystem(DS.VehicleSystems[i]) : nullptr)
		{
			Sys->Stop();
		}
	}
	if (DS.bPowersliding)
	{
		DS.bPowersliding = false;
		if (Audio)
		{
			Audio->SetObjectLoop(O.GetID(), PowerslideLoopSlot, FString());
		}
	}
}

void UGenGameLogic::DestroyVehicleEffects(FDrawState& DS)
{
	for (uint32& Id : DS.VehicleSystems)
	{
		if (FX && Id != 0)
		{
			FX->DestroySystem(Id);
		}
		Id = 0;
	}
}

void UGenGameLogic::UpdateVehicleDraw(FDrawState& DS, const FGenObject& O, const FGenDrawModule& Draw, float DeltaFrames)
{
	GEN_PERF_SCOPE("R.VehicleDraw");
	const FGenVehicleDraw& V = Draw.Vehicle;
	UGenModelComponent* MC = DS.Model;
	const FGenLocomotorTemplate* Loco = O.GetLocomotorTemplate();
	if (!MC || !Loco)
	{
		return;
	}
	// The original's figures are per 30 Hz frame; the logic's per logic frame.
	const float PerFrame30 = float(GenLogic::GetLogicFPS()) / 30.f;
	const float Steps = DeltaFrames / PerFrame30; // 30 Hz frames this drawn frame stands for
	const FVector3f Vel = O.GetVelocity() * PerFrame30;
	const float Speed = Vel.Size();
	const float MaxSpeed = Loco->Speed * PerFrame30;
	const bool bMotive = O.IsMotive();
	const bool bAirborne = O.IsSignificantlyAboveTerrain();
	const float Yaw = GetRenderOrientation(O);

	if (V.Kind == FGenVehicleDraw::Tank)
	{
		// "if tank is moving, kick up dust and debris" - higher and more of it the faster it goes.
		const float VelMag2 = Vel.X * Vel.X + Vel.Y * Vel.Y;
		const bool bDebris = VelMag2 > 0.00001f;
		const float VelMag = FMath::Sqrt(VelMag2);
		const float XY = FMath::Min(0.5f * VelMag + 0.1f, 1.f);
		const FVector3f VelMult(XY, XY, FMath::Min(VelMag + 0.1f, 1.f));
		for (int32 Side = TreadDebrisLeft; Side <= TreadDebrisRight; ++Side)
		{
			if (FGenParticleSystem* Sys = GetVehicleSystem(DS, Side, Side == TreadDebrisLeft ? V.TreadDebrisLeft : V.TreadDebrisRight, O, true))
			{
				if (bDebris) { Sys->Start(); } else { Sys->Stop(); }
				Sys->SetVelocityMultiplier(VelMult);
				Sys->SetBurstCountMultiplier(VelMult.Z);
			}
		}
	}

	if (V.HasTreads() && V.TreadAnimationRate != 0.f)
	{
		// The treads' textures: rolled by speed when driving straight on fast enough ("we stop scrolling when
		// tank slows down to reduce the appearance of sliding"); turning on the spot, the left one one way and
		// the right one the other - as long as it has turned since the last frame.
		const float Rate = V.TreadAnimationRate * Steps;
		const float SpeedFraction = MaxSpeed > 0.f ? Speed / MaxSpeed : 0.f;
		const FVector2f Dir(FMath::Cos(Yaw), FMath::Sin(Yaw));
		bool bChanged = false;
		if (V.Kind == FGenVehicleDraw::Tank && O.GetTurning() != 0 && SpeedFraction < V.TreadPivotSpeedFraction)
		{
			if (FMath::Abs(1.f - FVector2f::DotProduct(Dir, DS.TreadLastDir)) > 0.00001f)
			{
				const float Delta = O.GetTurning() < 0 ? -Rate : Rate;
				DS.TreadOffset[0] += Delta;
				DS.TreadOffset[1] -= Delta;
				bChanged = true;
			}
		}
		else if (bMotive && SpeedFraction >= V.TreadDriveSpeedFraction)
		{
			for (float& Offset : DS.TreadOffset)
			{
				Offset -= Rate;
			}
			bChanged = true;
		}
		DS.TreadLastDir = Dir;
		if (bChanged)
		{
			for (float& Offset : DS.TreadOffset)
			{
				Offset -= FMath::FloorToFloat(Offset); // "ensure coordinates of offset are in [0, 1] range"
			}
			MC->SetTreadOffsets(DS.TreadOffset[0], DS.TreadOffset[1], DS.TreadOffset[2]);
		}
	}

	if (!V.HasWheels())
	{
		TraceVehicleDraw(DS, O);
		return;
	}
	const FGenObject::FWheelInfo& WI = O.GetDrawWheels();
	const bool bTruck = V.Kind == FGenVehicleDraw::Truck;

	if (bTruck && !V.CabBone.IsEmpty())
	{
		// The cab swings with the steering (but not past the way to the next point on its path), the trailer
		// the other way, both easing towards it.
		float Desired = WI.Angle * V.CabRotationMultiplier;
		FVector3f Next;
		if (O.PeekNextPathPoint(Next))
		{
			const float ToGoal = GenLogic::AngleDiff(FMath::Atan2(Next.Y - O.GetPosition().Y, Next.X - O.GetPosition().X), O.GetOrientation());
			Desired = ToGoal < 0.f ? FMath::Clamp(Desired, ToGoal, 0.f) : FMath::Clamp(Desired, 0.f, ToGoal);
		}
		const float Ease = 1.f - FMath::Pow(1.f - FMath::Clamp(V.RotationDamping, 0.f, 1.f), Steps);
		DS.CabRotation += (Desired - DS.CabRotation) * Ease;
		MC->SetBoneControl(V.CabBone, FVector3f::ZeroVector, FQuat4f(FVector3f::UpVector, DS.CabRotation));
		if (!V.TrailerBone.IsEmpty())
		{
			DS.TrailerRotation += (-WI.Angle * V.TrailerRotationMultiplier - DS.TrailerRotation) * Ease;
			MC->SetBoneControl(V.TrailerBone, FVector3f::ZeroVector, FQuat4f(FVector3f::UpVector, DS.TrailerRotation));
		}
	}

	// The wheels roll with the speed (backwards when it reverses; a truck's rear wheels faster in a powerslide),
	// the front ones steer, and each rides its spring.
	float WheelSpeed = Speed;
	float PowerslideAddition = V.PowerslideRotationAddition;
	if (bTruck && O.IsMovingBackwards())
	{
		WheelSpeed = -WheelSpeed; // "rotate wheels backwards"
		PowerslideAddition = -PowerslideAddition;
	}
	DS.FrontWheelRotation = FMath::Fmod(DS.FrontWheelRotation + V.TireRotationMultiplier * WheelSpeed * Steps, 2.f * PI);
	DS.RearWheelRotation = FMath::Fmod(DS.RearWheelRotation + V.TireRotationMultiplier * (WheelSpeed + (DS.bPowersliding ? PowerslideAddition : 0.f)) * Steps, 2.f * PI);
	const FQuat4f Steer(FVector3f::UpVector, WI.Angle);
	const FQuat4f FrontRoll(FVector3f::RightVector, DS.FrontWheelRotation);
	const FQuat4f RearRoll(FVector3f::RightVector, DS.RearWheelRotation);
	auto Front = [&](int32 Tire, float Height)
	{
		MC->SetBoneControl(V.Tires[Tire], FVector3f(0.f, 0.f, Height), Steer * FrontRoll);
	};
	auto Rear = [&](int32 Tire, float Height)
	{
		MC->SetBoneControl(V.Tires[Tire], FVector3f(0.f, 0.f, Height), RearRoll);
	};
	Front(FGenVehicleDraw::LeftFront, WI.FrontLeft);
	Front(FGenVehicleDraw::RightFront, WI.FrontRight);
	Rear(FGenVehicleDraw::LeftRear, WI.RearLeft);
	Rear(FGenVehicleDraw::RightRear, WI.RearRight);
	Front(FGenVehicleDraw::MidLeftFront, WI.FrontLeft);
	Front(FGenVehicleDraw::MidRightFront, WI.FrontRight);
	Rear(FGenVehicleDraw::MidLeftRear, WI.RearLeft);
	Rear(FGenVehicleDraw::MidRightRear, WI.RearRight);
	if (bTruck)
	{
		Rear(FGenVehicleDraw::MidLeftMid, WI.RearLeft);
		Rear(FGenVehicleDraw::MidRightMid, WI.RearRight);
	}
	MC->UpdatePoseIfDirty();

	// Dust behind it, dirt sprayed while it gets going, spray from the wheels in a powerslide.
	const bool bWasPowersliding = DS.bPowersliding;
	DS.bPowersliding = false;
	FGenParticleSystem* DustSys = GetVehicleSystem(DS, Dust, V.Dust, O, true);
	FGenParticleSystem* DirtSys = GetVehicleSystem(DS, Dirt, V.DirtSpray, O, true);
	FGenParticleSystem* SlideSys = GetVehicleSystem(DS, Powerslide, V.PowerslideSpray, O, true);
	if (bMotive && !bAirborne)
	{
		if (DustSys) { DustSys->Start(); }
		if (DirtSys) { DirtSys->Start(); }
		FVector3f Accel = O.GetPrevAccel() * (PerFrame30 * PerFrame30);
		Accel.Z = 0.f; // "ignore gravitational force"
		bool bAccelerating = Accel.Size() > 0.01f;
		if (bAccelerating && Accel.X * Vel.X + Accel.Y * Vel.Y < 0.f)
		{
			bAccelerating = false; // "decelerating, actually"
		}
		if (DustSys)
		{
			DustSys->SetSizeMultiplier(FMath::Min(Speed, 2.f)); // "Need more dust the faster we go."
		}
		if (DirtSys)
		{
			if (WI.FramesAirborne > 3 && DS.LandingFrame != WI.LandingSerial)
			{
				// Down again after a jump: a cloud of dust, and the landing.
				DS.LandingFrame = WI.LandingSerial;
				const float Factor = FMath::Min(1.f + float(WI.FramesAirborne / 16), 2.f);
				if (DustSys)
				{
					DustSys->SetSizeMultiplier(Factor * 2.f);
					DustSys->Trigger();
				}
				if (Audio)
				{
					Audio->PlayAt(Audio->GetTemplateSound(*O.GetTemplate(), TEXT("TruckLandingSound")), O.GetPosition(), O.GetOwner());
				}
			}
			if (!bAccelerating || Speed > 2.f)
			{
				DirtSys->Stop();
			}
		}
		if (SlideSys)
		{
			if (O.GetTurning() == 0)
			{
				SlideSys->Stop();
			}
			else
			{
				DS.bPowersliding = true;
				SlideSys->Start();
			}
		}
	}
	else
	{
		if (DustSys) { DustSys->Stop(); }
		if (DirtSys) { DirtSys->Stop(); }
		if (SlideSys) { SlideSys->Stop(); }
	}
	if (DS.bPowersliding != bWasPowersliding && Audio)
	{
		Audio->SetObjectLoop(O.GetID(), PowerslideLoopSlot,
			DS.bPowersliding ? Audio->GetTemplateSound(*O.GetTemplate(), TEXT("TruckPowerslideSound")) : FString());
	}
	TraceVehicleDraw(DS, O);
}

void UGenGameLogic::TraceVehicleDraw(FDrawState& DS, const FGenObject& O)
{
	// Once a second of game time per vehicle while it is drawn moving.
	const uint32 Now = GetFrame();
	if (!bTrace || Now % uint32(GenLogic::GetLogicFPS()) != 0 || DS.VehicleTraceFrame == Now)
	{
		return;
	}
	DS.VehicleTraceFrame = Now;
	UE_LOG(LogGenerals, Log, TEXT("  [vehdraw f%u] #%u %s wheels front=%.2f rear=%.2f steer=%.1f cab=%.1f treads=%.2f/%.2f/%.2f slide=%d"), Now, O.GetID(),
		*O.GetTemplate()->Name, DS.FrontWheelRotation, DS.RearWheelRotation, FMath::RadiansToDegrees(O.GetDrawWheels().Angle), FMath::RadiansToDegrees(DS.CabRotation),
		DS.TreadOffset[0], DS.TreadOffset[1], DS.TreadOffset[2], DS.bPowersliding ? 1 : 0);
}
