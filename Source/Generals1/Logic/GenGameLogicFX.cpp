// Effects side of the game logic: FX lists played for weapons and deaths, and ObjectCreationList
// (ObjectCreationList.cpp GenericObjectCreationNugget) spawning debris and hulks.

#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioManager.h"
#include "Client/GenFXData.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapActor.h"
#include "W3D/GenAssets.h"
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

uint32 UGenGameLogic::CreateObjectParticleSystem(const FString& TemplateName, FGenObjectID Object, const FVector3f& LocalOffset)
{
	if (FGenParticleSystem* Sys = FX ? FX->CreateAttachedSystem(TemplateName, Object) : nullptr)
	{
		Sys->SetPosition(LocalOffset);
		return Sys->GetID();
	}
	return 0;
}

void UGenGameLogic::PlayParticleSystemAt(const FString& TemplateName, const FVector3f& Pos)
{
	if (FX)
	{
		FX->CreateSystemAt(TemplateName, Pos);
	}
}

void UGenGameLogic::DestroyParticleSystem(uint32 SystemID)
{
	if (FX && SystemID != 0)
	{
		FX->DestroySystem(SystemID);
	}
}

void UGenGameLogic::RevealAreaOnce(int32 Player, const FVector2f& Pos, float Range)
{
	if (!Shroud.IsValid() || !Players.IsValidIndex(Player) || Range <= 0.f)
	{
		return;
	}
	FGenSighting S;
	Shroud.WorldToCell(Pos.X, Pos.Y, S.CellX, S.CellY);
	S.CellRadius = FMath::Max(1, Shroud.WorldToCellDist(Range));
	S.Mask = Players[Player].AlliesMask;
	Shroud.DoReveal(S);
	Shroud.UndoReveal(S);
}

void UGenGameLogic::RevealAreaFor(int32 Player, const FVector2f& Pos, float Range, uint32 Frames)
{
	if (!Shroud.IsValid() || !Players.IsValidIndex(Player) || Range <= 0.f || Frames == 0)
	{
		return;
	}
	FGenSighting S;
	Shroud.WorldToCell(Pos.X, Pos.Y, S.CellX, S.CellY);
	S.CellRadius = FMath::Max(1, Shroud.WorldToCellDist(Range));
	S.Mask = Players[Player].AlliesMask;
	Shroud.DoReveal(S);
	Shroud.QueueUndoReveal(S, Frame + Frames);
}

void UGenGameLogic::AddScorch(const FVector3f& Pos, float Radius, int32 Type)
{
	if (FX)
	{
		FX->AddScorch(Pos, Radius, Type);
	}
}

void UGenGameLogic::AttachParticleSystem(const FString& TemplateName, FGenObjectID Object)
{
	if (FX)
	{
		FX->CreateAttachedSystem(TemplateName, Object);
	}
}

void UGenGameLogic::AttachParticleSystemFor(const FString& TemplateName, FGenObjectID Object, const FVector3f& Offset, uint32 LifetimeFrames)
{
	if (FGenParticleSystem* Sys = FX ? FX->CreateAttachedSystem(TemplateName, Object) : nullptr)
	{
		Sys->SetPosition(Offset);
		Sys->SetSystemLifetime(LifetimeFrames);
	}
}

void UGenGameLogic::DoTransitionDamageFX(const FGenObject& Obj, int32 State)
{
	// The particle systems of the module sit on model bones and are handled with the drawable (UpdateDrawables).
	const FTransform3f Xf = Obj.GetTransform();
	for (const FGenTransitionFX& X : Obj.GetTemplate()->TransitionFX)
	{
		if (X.State != State)
		{
			continue;
		}
		if (X.Kind == FGenTransitionFX::FXList)
		{
			DoFXPos(X.Name, Xf.TransformPositionNoScale(X.Loc), &Xf, 0.f, nullptr, 0.f);
		}
		else if (X.Kind == FGenTransitionFX::OCL)
		{
			DoOCL(X.Name, Obj);
		}
	}
}

bool UGenGameLogic::GetWeaponBoneTransform(const FGenObject& Obj, int32 Slot, bool bLaunchBone, FTransform3f& OutWorld) const
{
	// The original also asks the drawable for bone positions (W3DModelDraw::handleWeaponFireFX,
	// getProjectileLaunchOffset): the pose is the one shown, turret angle included.
	const TObjectPtr<UGenModelComponent>* MC = Drawables.Find(Obj.GetID());
	const FDrawState* DS = DrawStates.Find(Obj.GetID());
	const FGenDrawModule* Draw = Obj.GetTemplate()->PrimaryDraw();
	if (!MC || !*MC || !DS || !Draw || !DS->State.Cur || Slot < 0 || Slot > 2)
	{
		return false;
	}
	const FGenModelConditionState& State = *DS->State.Cur;
	const FString& BoneName = bLaunchBone ? State.LaunchBone[Slot] : State.FireFXBone[Slot];
	FTransform3f Bone;
	if (BoneName.IsEmpty() || !(*MC)->GetBoneTransformW3D(BoneName, Bone))
	{
		return false;
	}
	OutWorld = Bone * Obj.GetTransform();
	return true;
}

int32 UGenGameLogic::GetWeaponBarrelCount(const FGenObject& Obj, int32 Slot) const
{
	const FGenDrawModule* Draw = Obj.GetTemplate()->PrimaryDraw();
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Draw || !Assets || Slot < 0 || Slot > 2)
	{
		return 0;
	}
	const int32 StateIndex = Draw->FindBestState(Obj.GetModelConditions());
	if (!Draw->States.IsValidIndex(StateIndex) || Draw->States[StateIndex].Model.IsEmpty() || Draw->States[StateIndex].LaunchBone[Slot].IsEmpty())
	{
		return 0;
	}
	TArray<FTransform3f> Bones;
	return Assets->GetPristineBones(Draw->States[StateIndex].Model, Draw->States[StateIndex].LaunchBone[Slot], 1, 32, Bones);
}

namespace
{
	/** The pose a state's bones are taken in: its first animation's first frame (or last: PRISTINE_BONE_POS_IN_FINAL_FRAME). */
	const FString& PristineAnimation(const FGenModelConditionState& State, bool& bOutLastFrame)
	{
		static const FString None;
		bOutLastFrame = (State.Flags & GenStateFlags::PristineBonePosInFinalFrame) != 0;
		return State.Animations.Num() > 0 ? State.Animations[0] : None;
	}
}

void UGenGameLogic::FireTempWeapon(const FGenWeaponTemplate& Weapon, FGenObject& Source, const FVector3f& TargetPos)
{
	DoFXPos(Weapon.FireFX, Source.GetPosition(), nullptr, 0.f, &TargetPos, Weapon.PrimaryDamageRadius);
	if (!Weapon.FireOCL.IsEmpty())
	{
		DoOCL(Weapon.FireOCL, Source);
	}
	if (Weapon.IsProjectile())
	{
		const FVector3f From = Source.GetPosition() + FVector3f(0.f, 0.f, Source.GetTemplate()->GeometryHeight * 0.5f);
		SpawnProjectile(Weapon, Source, GenInvalidObjectID, TargetPos, From, Source.GetOrientation());
	}
	else
	{
		DealDamage(Weapon, Source.GetID(), GenInvalidObjectID, TargetPos, false);
	}
}

void UGenGameLogic::FlashSelected(FGenObjectID Id)
{
	if (FDrawState* DS = DrawStates.Find(Id))
	{
		DS->FlashStart = FPlatformTime::Seconds();
		DS->bBusy = true; // (looked at every frame while it lasts)
	}
}

bool UGenGameLogic::RaycastDrawable(FGenObjectID Id, const FVector& Origin, const FVector& Dir, double& InOutT) const
{
	const FDrawState* DS = DrawStates.Find(Id);
	if (!DS || !DS->Model || DS->bHidden)
	{
		return false;
	}
	bool bHit = DS->Model->RaycastWorld(Origin, Dir, InOutT);
	for (const FExtraDraw& E : DS->Extras)
	{
		if (const UGenModelComponent* Extra = E.Model.Get())
		{
			bHit |= Extra->RaycastWorld(Origin, Dir, InOutT);
		}
	}
	return bHit;
}

bool UGenGameLogic::GetWeaponLaunchTransform(const FGenObject& Obj, int32 Slot, FTransform3f& OutWorld, int32 Barrel) const
{
	// The look the object's conditions ask for, whether or not the screen has caught up with them; the
	// bone where the model's base pose has it, turned with the turret. (Animation doesn't come into it,
	// in the original either: what the logic does must not depend on what a screen happens to show.)
	const FGenDrawModule* Draw = Obj.GetTemplate()->PrimaryDraw();
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (!Draw || !Assets || Slot < 0 || Slot > 2)
	{
		return false;
	}
	const int32 StateIndex = Draw->FindBestState(Obj.GetModelConditions());
	if (!Draw->States.IsValidIndex(StateIndex))
	{
		return false;
	}
	const FGenModelConditionState& State = Draw->States[StateIndex];
	FTransform3f Bone;
	if (State.Model.IsEmpty() || State.LaunchBone[Slot].IsEmpty())
	{
		return false;
	}
	// (The bones are numbered from 01; without a number the first of them is found.)
	bool bLastFrame = false;
	const FString& Anim = PristineAnimation(State, bLastFrame);
	if (!(Barrel > 0 && Assets->GetPristineBoneTurned(State.Model, State.LaunchBone[Slot] + FString::Printf(TEXT("%02d"), Barrel + 1), State.Turret, Obj.GetTurretAngle(0), Bone, Anim, bLastFrame))
		&& !Assets->GetPristineBoneTurned(State.Model, State.LaunchBone[Slot], State.Turret, Obj.GetTurretAngle(0), Bone, Anim, bLastFrame))
	{
		return false;
	}
	// Weapon::positionProjectileForLaunch: "pitchAdjustment.Rotate_Y(-pitch)" about the turret's pitch
	// bone, then the turn - the shot leaves the way the raised barrel points.
	const float Pitch = Obj.TurretPitch[0];
	FTransform3f PitchBone;
	if (FMath::Abs(Pitch) > 1e-4f && !State.TurretPitch.IsEmpty()
		&& Assets->GetPristineBoneTurned(State.Model, State.TurretPitch, State.Turret, Obj.GetTurretAngle(0), PitchBone, Anim, bLastFrame))
	{
		const float Turn = Obj.GetTurretAngle(0);
		const FVector3f Pivot = PitchBone.GetTranslation();
		const FQuat4f Raise(FVector3f(-FMath::Sin(Turn), FMath::Cos(Turn), 0.f), -Pitch); // (the model's y axis, turned with the turret)
		Bone = FTransform3f(Raise * Bone.GetRotation(), Pivot + Raise.RotateVector(Bone.GetTranslation() - Pivot), Bone.GetScale3D());
	}
	OutWorld = Bone * Obj.GetTransform();
	return true;
}

bool UGenGameLogic::GetNamedBonePosition(const FGenObject& Obj, const FString& BoneName, FVector3f& OutWorld) const
{
	const TObjectPtr<UGenModelComponent>* MC = Drawables.Find(Obj.GetID());
	FTransform3f Bone;
	if (BoneName.IsEmpty() || !MC || !*MC || !(*MC)->GetBoneTransformW3D(BoneName, Bone))
	{
		return false;
	}
	OutWorld = (Bone * Obj.GetTransform()).GetTranslation();
	return true;
}

void UGenGameLogic::CreateWeaponLaser(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon, const FGenObject* Victim, const FVector3f& VictimPos)
{
	const FGenThingTemplate* LaserT = FGenGameData::Get().FindTemplate(Weapon.LaserName);
	if (!LaserT || !LaserT->LaserDraw.bValid)
	{
		return;
	}
	// (The laser object's LifetimeUpdate or DeletionUpdate says how long the beam shows.)
	int32 Life = 3;
	if (LaserT->bHasLifetime) { Life = Random.RandRange(LaserT->MinLifetime, FMath::Max(LaserT->MinLifetime, LaserT->MaxLifetime)); }
	else if (LaserT->bHasDeletion) { Life = Random.RandRange(LaserT->DeletionMin, FMath::Max(LaserT->DeletionMin, LaserT->DeletionMax)); }
	Life = FMath::Max(1, Life);

	FWeaponBeam& B = WeaponBeams.AddDefaulted_GetRef();
	B.Draw = &LaserT->LaserDraw;
	B.Shooter = Shooter.GetID();
	B.Bone = Weapon.LaserBoneName;
	B.ExpireFrame = Frame + uint32(Life);
	B.End = VictimPos;
	// "Targets are positioned on the ground, so raise the beam up so we're not shooting their feet.
	// Projectiles are a different story, target their exact position."
	if (Victim && !Victim->IsKindOf(TEXT("PROJECTILE")) && !Victim->IsAirborneTarget())
	{
		B.End.Z += 10.f;
	}
	// (Where it starts is the bone as drawn, asked for when the beam is drawn; this is for the flare, and
	// for when the shooter is gone.)
	FTransform3f Launch;
	B.Start = GetWeaponLaunchTransform(Shooter, Slot, Launch) ? Launch.GetTranslation()
		: Shooter.GetPosition() + FVector3f(0.f, 0.f, Shooter.GetTemplate()->GeometryHeight * 0.5f);

	// LaserUpdate::initLaser: "Make sure the laser flare is visible to the player." The flares go with the beam.
	if (GetStealthLook(Shooter) != EGenStealthLook::Invisible)
	{
		if (!LaserT->LaserMuzzleParticleSystem.IsEmpty())
		{
			AttachParticleSystemFor(LaserT->LaserMuzzleParticleSystem, Shooter.GetID(), Shooter.GetTransform().InverseTransformPositionNoScale(B.Start), uint32(Life));
		}
		if (!LaserT->LaserTargetParticleSystem.IsEmpty() && Victim)
		{
			AttachParticleSystemFor(LaserT->LaserTargetParticleSystem, Victim->GetID(), Victim->GetTransform().InverseTransformPositionNoScale(B.End), uint32(Life));
		}
	}
}

void UGenGameLogic::GetVisibleBeams(TArray<FBeam>& Out) const
{
	// The particle cannons': the lasers of their nodes, the beam going up, the beam coming down.
	for (FGenObjectID Id : ParticleCannons)
	{
		if (const FGenObject* O = FindObject(Id))
		{
			O->GetParticleCannonBeams(Frame, Out);
		}
	}
	// The shots of laser weapons: from the shooter's bone (LaserUpdate::updateStartPos follows it) to where they struck.
	for (const FWeaponBeam& W : WeaponBeams)
	{
		const FGenObject* Shooter = FindObject(W.Shooter);
		if (Shooter && (Shooter->GetLocalShroud() == EGenObjectShroud::Fogged || Shooter->GetLocalShroud() == EGenObjectShroud::Shrouded
			|| GetStealthLook(*Shooter) == EGenStealthLook::Invisible))
		{
			continue;
		}
		FBeam& B = Out.AddDefaulted_GetRef();
		B.Draw = W.Draw;
		B.Start = W.Start;
		B.End = W.End;
		if (Shooter && !W.Bone.IsEmpty())
		{
			GetNamedBonePosition(*Shooter, W.Bone, B.Start);
		}
	}

	// SpecialAbilityUpdate::initLaser: from the unit's bone (failing that, from where it stands) to the
	// middle of its target.
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		FGenObjectID TargetId = GenInvalidObjectID;
		FString Bone;
		const FGenThingTemplate* Thing = O ? O->GetAbilityBeam(TargetId, Bone) : nullptr;
		if (!Thing || O->IsHidden() || O->GetLocalShroud() == EGenObjectShroud::Fogged || O->GetLocalShroud() == EGenObjectShroud::Shrouded
			|| GetStealthLook(*O) == EGenStealthLook::Invisible)
		{
			continue;
		}
		const FGenObject* Target = FindObject(TargetId);
		FBeam& B = Out.AddDefaulted_GetRef();
		B.Draw = &Thing->LaserDraw;
		if (!GetNamedBonePosition(*O, Bone, B.Start))
		{
			B.Start = O->GetPosition();
		}
		B.End = Target ? Target->GetPosition() + FVector3f(0.f, 0.f, Target->GetTemplate()->GeometryHeight * 0.5f) : B.Start;
	}
}

void UGenGameLogic::DoWeaponFireSound(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon)
{
	// WeaponTemplate::fireWeaponTemplate / FiringTracker::shotFired
	if (!Audio || Weapon.FireSound.IsEmpty() || GetStealthLook(Shooter) == EGenStealthLook::Invisible)
	{
		return;
	}
	if (Weapon.FireSoundLoopTime > 0)
	{
		// A weapon that fires in a stream: its sound loops until the shots stop coming.
		Audio->SetObjectLoop(Shooter.GetID(), 2 + FMath::Clamp(Slot, 0, 2), Weapon.FireSound, float(Weapon.FireSoundLoopTime) / float(GenLogic::GetLogicFPS()));
	}
	else
	{
		Audio->PlayForObject(Weapon.FireSound, Shooter);
	}
}

void UGenGameLogic::PlayMiscSound(const TCHAR* MiscKey, int32 Player)
{
	if (Audio && Player == LocalPlayer)
	{
		Audio->PlayUI(FGenAudioData::Get().GetMisc(MiscKey), Player);
	}
}

void UGenGameLogic::EvaEvent(const TCHAR* EventName, int32 Player)
{
	if (Audio && Player == LocalPlayer)
	{
		Audio->Eva(EventName);
	}
}

void UGenGameLogic::AddScreenMessage(const FString& Text, const FLinearColor& Color)
{
	FGenFloatingText& Line = FloatingTexts.AddDefaulted_GetRef();
	Line.Text = Text;
	Line.Color = Color;
	Line.Frame = Frame;
	Line.bScreen = true;
}

void UGenGameLogic::IssueScreenMessage(const FString& Text)
{
	// (This computer's own: not an order, nothing the other players' games hear of.)
	PendingLocal.Add([this, Text]()
	{
		AddScreenMessage(Text);
	});
}

void UGenGameLogic::EvaSuperweapon(const TCHAR* Stage, const FGenObject& Obj, const FGenSpecialPowerTemplate& Power)
{
	// (The kind by the power's Enum, the generals' variants included; "Detected" and "Ready" only for the
	// three superweapons, "Launched" also for the GPS scrambler and the sneak attack.)
	const FString& Enum = Power.Enum;
	const TCHAR* Kind = nullptr;
	if (Enum.EndsWith(TEXT("SPECIAL_PARTICLE_UPLINK_CANNON"), ESearchCase::IgnoreCase)) { Kind = TEXT("ParticleCannon"); }
	else if (Enum.EndsWith(TEXT("SPECIAL_NEUTRON_MISSILE"), ESearchCase::IgnoreCase)) { Kind = TEXT("Nuke"); }
	else if (Enum.Equals(TEXT("SPECIAL_SCUD_STORM"), ESearchCase::IgnoreCase)) { Kind = TEXT("ScudStorm"); }
	else if (FCString::Strcmp(Stage, TEXT("Launched")) == 0)
	{
		if (Enum.EndsWith(TEXT("SPECIAL_GPS_SCRAMBLER"), ESearchCase::IgnoreCase)) { Kind = TEXT("GPS_Scrambler"); }
		else if (Enum.Equals(TEXT("SPECIAL_SNEAK_ATTACK"), ESearchCase::IgnoreCase)) { Kind = TEXT("Sneak_Attack"); }
	}
	if (!Kind || !Players.IsValidIndex(LocalPlayer))
	{
		return;
	}
	// ("Note: treating NEUTRAL as ally.")
	const TCHAR* Who = Obj.GetOwner() == LocalPlayer ? TEXT("Own")
		: GetRelationship(LocalPlayer, Obj.GetOwner()) != EGenRelationship::Enemies ? TEXT("Ally") : TEXT("Enemy");
	EvaEvent(*FString::Printf(TEXT("Superweapon%s_%s_%s"), Stage, Who, Kind), LocalPlayer);
}

void UGenGameLogic::PlayObjectSound(const FGenObject& Obj, std::initializer_list<const TCHAR*> Keys)
{
	if (!Audio)
	{
		return;
	}
	for (const TCHAR* Key : Keys)
	{
		const FString& Sound = Audio->GetTemplateSound(*Obj.GetTemplate(), Key);
		if (!Sound.IsEmpty())
		{
			Audio->PlayForObject(Sound, Obj);
			return;
		}
	}
}

void UGenGameLogic::DoWeaponFireFX(const FGenObject& Shooter, int32 Slot, const FGenWeaponTemplate& Weapon, const FVector3f& TargetPos)
{
	const FGenFXList* List = FX ? FGenFXData::Get().FindFXList(Weapon.FireFX) : nullptr;
	if (!List)
	{
		return;
	}
	// Weapon::fireWeaponTemplate: a stealthed unit that isn't the watcher's own fires without giving itself away.
	if (GetStealthLook(Shooter) == EGenStealthLook::Invisible)
	{
		return;
	}
	// The flash goes on the bone as the model is posed on the screen (W3DModelDraw::handleWeaponFireFX
	// asks the render object): the drawing pass's business, a moment from now.
	FPendingFireFX& Pending = PendingFireFX.AddDefaulted_GetRef();
	Pending.Shooter = Shooter.GetID();
	Pending.Slot = Slot;
	Pending.Weapon = &Weapon;
	Pending.TargetPos = TargetPos;
	Pending.ShooterXf = Shooter.GetTransform();
}

void UGenGameLogic::PlayPendingFireFX()
{
	TArray<FPendingFireFX> Fired = MoveTemp(PendingFireFX);
	PendingFireFX.Reset();
	for (const FPendingFireFX& P : Fired)
	{
		const FGenFXList* List = FX && P.Weapon ? FGenFXData::Get().FindFXList(P.Weapon->FireFX) : nullptr;
		if (!List)
		{
			continue;
		}
		FTransform3f Xf = P.ShooterXf;
		if (const FGenObject* Shooter = FindObject(P.Shooter))
		{
			GetWeaponBoneTransform(*Shooter, P.Slot, false, Xf);
		}
		FX->DoFXPos(List, Xf.GetTranslation(), &Xf, P.Weapon->WeaponSpeed, &P.TargetPos, P.Weapon->PrimaryDamageRadius);
	}
}

void UGenGameLogic::SetDebrisModel(FGenObject& Obj, const FString& Model, bool bUseHouseColor)
{
	// (Called right after the object is made: the drawing pass, which makes its model, finds this.)
	Obj.DebrisModel = Model;
	Obj.bDebrisHouseColor = bUseHouseColor;
}

void UGenGameLogic::DoOCL(const FString& OCLName, const FGenObject& Source, const FVector3f* AtPosition, TArray<FGenObjectID>* OutCreated)
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
	// (ObjectCreationList::create with a position: what is made appears there instead of where the source is.)
	const FVector3f SourcePos = AtPosition ? *AtPosition : Source.GetPosition();
	const FTransform3f SourceXf = Source.GetTransform();
	const bool bSourceAirborne = !AtPosition && SourcePos.Z > Source.GetSurfaceHeight() + 9.f;

	for (const FGenOCLNugget& N : OCL->Nuggets)
	{
		if (N.bApplyRandomForce)
		{
			// ApplyRandomForceNugget: the source itself is thrown and set spinning - a wreck blown into the
			// air ("const_cast is evil", the original says of the same thing).
			FGenObject& Thrown = const_cast<FGenObject&>(Source);
			if (Thrown.GetTemplate()->bHasPhysics && Thrown.IsDead())
			{
				Thrown.ApplyForce(RandomForce(Random, N.MinMag, N.MaxMag, N.MinPitch, N.MaxPitch) * RateScale);
				const float Spin = FMath::Max(0.f, N.SpinRate);
				Thrown.YawRate = Random.FRandRange(-Spin, Spin);
				Thrown.RollRate = Random.FRandRange(-Spin, Spin);
				Thrown.PitchRate = Random.FRandRange(-Spin, Spin);
				Thrown.bFreePhysics = true;
				Thrown.bFreeRotation = true;
			}
			continue;
		}
		if (N.Names.Num() == 0 || (N.bSkipIfSignificantlyAirborne && bSourceAirborne))
		{
			continue;
		}
		if (N.bRequiresLivePlayer && HasPlayerBeenDefeated(Source.GetOwner()))
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
			if (OutCreated && !N.bDebris)
			{
				OutCreated->Add(O->GetID());
			}
			// GenericObjectCreationNugget::reallyCreate
			if (N.bInheritsVeterancy && T->bIsTrainable)
			{
				O->SetVeterancyLevel(Source.GetVeterancyLevel(), false); // (it carries its maker's rank over: no promotion)
			}
			if (N.InvulnerableFrames > 0)
			{
				O->InvulnerableUntilFrame = Frame + uint32(N.InvulnerableFrames);
			}
			const FGenThingTemplate* ChuteT = N.PutInContainer.IsEmpty() ? nullptr : GD.FindTemplate(N.PutInContainer);
			if (ChuteT && ChuteT->bParachute)
			{
				// "PutInContainer": it hangs under a parachute, and what the nugget does to its object is done
				// to the parachute (the push that throws a pilot clear of his aircraft).
				if (FGenObject* Chute = CreateObject(ChuteT, Source.GetOwner(), SourcePos, Source.GetOrientation()))
				{
					O->Producer = Source.GetID();
					Chute->Producer = Source.GetID();
					FVector3f Toss = FVector3f::ZeroVector;
					if ((N.Disposition & GenDisposition::RandomForce) != 0)
					{
						Toss = RandomForce(Random, N.MinMag, N.MaxMag, N.MinPitch, N.MaxPitch) * RateScale / FMath::Max(Chute->Mass, 0.01f);
					}
					Chute->CarryUnderParachute(*O, nullptr, Toss);
					Chute->SnapshotPrevious();
					O->SnapshotPrevious();
					UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [ocl f%u] %s (#%u) leaves #%u %s under a %s"), Frame, *T->Name, O->GetID(), Source.GetID(),
						*Source.GetTemplate()->Name, *ChuteT->Name);
					continue;
				}
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
				if (N.AnimSets.Num() >= 3)
				{
					// "AnimationSet": one of the sets, at random - a body's way of flying and landing.
					const int32 Which = Random.RandRange(0, N.AnimSets.Num() / 3 - 1) * 3;
					for (int32 a = 0; a < 3; ++a) { O->DebrisAnims[a] = N.AnimSets[Which + a]; }
					O->DebrisFXFinal = N.FXFinal;
				}
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
			if (T->Slaved.bValid)
			{
				O->Enslave(Source); // "If they have a SlavedUpdate, then I have to tell them who their daddy is from now on."
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
					O->BounceSound = N.BounceSound; // (setBounceSound)

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
			if (N.bContainInsideSourceObject)
			{
				// "The Obj has been totally made, so stuff it inside ourselves if desired" - and "if we fail
				// to contain it, we can't just leave it. Stillborn it."
				FGenObject& Carrier = const_cast<FGenObject&>(Source);
				if (IsValidContainerFor(Carrier, *O, true))
				{
					AddToContain(Carrier, *O);
				}
				else
				{
					O->bPendingDestroy = true;
				}
			}
			O->SnapshotPrevious();
		}
	}
}

// ---- ReplaceObjectUpgrade, AssistedTargetingUpdate -------------------------------------------------

void UGenGameLogic::ReplaceWithObject(FGenObject& Obj, const FString& TemplateName)
{
	// ReplaceObjectUpgrade::upgradeImplementation: the fake building goes, the real one stands in its place, finished.
	const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(TemplateName);
	if (!T || Obj.IsDead() || Obj.bPendingDestroy)
	{
		return;
	}
	FGenObject* New = CreateObject(T, Obj.GetOwner(), Obj.GetPosition(), Obj.GetOrientation());
	Obj.bPendingDestroy = true;
	if (New)
	{
		OnBuildingCompleted(*New); // (its create modules' onBuildComplete, and Player::onStructureConstructionComplete)
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [upgrade f%u] #%u %s is replaced by #%u %s"), Frame, Obj.GetID(), *Obj.GetTemplate()->Name, New->GetID(), *T->Name);
	}
}

void UGenGameLogic::RequestAssistance(const FGenObject& Requester, FGenObject& Victim, float Range)
{
	// makeAssistanceRequest: our player's things of the requester's kind, this close, with an AssistedTargetingUpdate
	// and free to (able to attack, the weapon in hand ready) fire a clip from the assisting slot, locked for it.
	if (Victim.IsDead() || !Players.IsValidIndex(Requester.GetOwner()))
	{
		return;
	}
	const FVector2f At(Requester.GetPosition().X, Requester.GetPosition().Y);
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* O = Objects[Id].Get();
		const FGenThingTemplate::FAssistedTargeting& A = O->GetTemplate()->AssistedTargeting;
		if (O == &Requester || !A.bValid || O->GetOwner() != Requester.GetOwner() || O->IsDead() || O->IsUnderConstruction() || O->IsDisabled()
			|| O->GetTemplate()->Canonical() != Requester.GetTemplate()->Canonical()
			|| FVector2f::DistSquared(At, FVector2f(O->GetPosition().X, O->GetPosition().Y)) > FMath::Square(Range))
		{
			continue;
		}
		const int32 Current = O->GetCurrentWeaponSlot();
		const FGenWeaponInstance& WI = O->Weapons[Current];
		if (!WI.Template || WI.bReloading || Frame < WI.ReadyFrame || !O->Weapons[A.Slot].Template)
		{
			continue; // "getStatus() == READY_TO_FIRE"
		}
		O->CommandAttack(Victim.GetID());
		O->WeaponLockSlot = A.Slot;               // setWeaponLock( m_weaponSlot, LOCKED_TEMPORARILY )
		O->AttackShotsLeft = FMath::Max(1, A.ClipSize);
		if (!A.LaserFromAssisted.IsEmpty())
		{
			CreateFeedbackLaser(A.LaserFromAssisted, Requester, *O);
			CreateFeedbackLaser(A.LaserFromAssisted, *O, Victim); // (sic: "m_laserToTarget = findTemplate( m_laserFromAssistedName )")
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [assist f%u] #%u %s fires %d at #%u %s for #%u"), Frame, O->GetID(), *O->GetTemplate()->Name, O->AttackShotsLeft,
			Victim.GetID(), *Victim.GetTemplate()->Name, Requester.GetID());
	}
}

void UGenGameLogic::CreateFeedbackLaser(const FString& LaserTemplate, const FGenObject& From, const FGenObject& To)
{
	const FGenThingTemplate* LaserT = FGenGameData::Get().FindTemplate(LaserTemplate);
	if (!LaserT || !LaserT->LaserDraw.bValid)
	{
		return;
	}
	int32 Life = 3;
	if (LaserT->bHasLifetime) { Life = Random.RandRange(LaserT->MinLifetime, FMath::Max(LaserT->MinLifetime, LaserT->MaxLifetime)); }
	else if (LaserT->bHasDeletion) { Life = Random.RandRange(LaserT->DeletionMin, FMath::Max(LaserT->DeletionMin, LaserT->DeletionMax)); }
	FWeaponBeam& B = WeaponBeams.AddDefaulted_GetRef();
	B.Draw = &LaserT->LaserDraw;
	B.Shooter = GenInvalidObjectID; // (from one spot to another: no bone)
	B.ExpireFrame = Frame + uint32(FMath::Max(1, Life));
	B.Start = From.GetPosition() + FVector3f(0.f, 0.f, From.GetTemplate()->GeometryHeight * 0.5f);
	B.End = To.GetPosition() + FVector3f(0.f, 0.f, To.IsAirborneTarget() ? 0.f : 10.f);
}
