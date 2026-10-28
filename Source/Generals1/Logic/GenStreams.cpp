// Projectile streams (ProjectileStreamUpdate, drawn by UGenFXManager as W3DProjectileStreamDraw does) and
// BoneFXUpdate.
//
// A weapon with a ProjectileStreamName - the toxin truck's spray, the ambulance's hose, the Dragon tank's flame -
// keeps an invisible object that remembers its shots in flight, oldest first; the draw puts a strip of scrolling
// texture through them, broken where the aim changed. The shots themselves are invisible: what shows besides the
// strip are the particle trails their BoneFXUpdate starts on them.
//
// BoneFXUpdate: FX lists, object creation lists and particle systems at model bones, a set for each body damage
// state, once or again and again after a while (a Mogadishu tower's rubble smoking).

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

void UGenGameLogic::AddToProjectileStream(const FGenWeaponTemplate& Weapon, const FGenObject& InShooter, const FGenObject& Projectile, FGenObjectID Victim,
	const FVector3f& VictimPos)
{
	// Weapon::newProjectileFired: "If I have a stream, I need to tell it about this new guy" - the weapon's own
	// stream, made anew if it has gone.
	FGenObject* Shooter = FindObject(InShooter.GetID());
	int32 Slot = INDEX_NONE;
	for (int32 s = 0; Shooter && s < 3; ++s)
	{
		if (Shooter->Weapons[s].Template == &Weapon)
		{
			Slot = s;
			break;
		}
	}
	if (Slot == INDEX_NONE)
	{
		return;
	}
	FGenObject* S = FindObject(Shooter->ProjectileStreams[Slot]);
	if (!S || S->IsDestroyed() || !S->Stream)
	{
		Shooter->ProjectileStreams[Slot] = GenInvalidObjectID;
		const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(Weapon.ProjectileStreamName);
		S = T ? CreateObject(T, Shooter->GetOwner(), Shooter->GetPosition(), 0.f) : nullptr;
		if (!S || !S->Stream)
		{
			return;
		}
		Shooter->ProjectileStreams[Slot] = S->GetID();
		ProjectileStreams.Add(S->GetID());
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [stream f%u] #%u %s: %s's shots are drawn by #%u %s"), Frame, Shooter->GetID(), *Shooter->GetTemplate()->Name, *Weapon.Name,
			S->GetID(), *T->Name);
	}
	S->SetPosition(Shooter->GetPosition());
	S->AddStreamProjectile(Shooter->GetID(), Projectile.GetID(), Victim, &VictimPos);
}

void FGenObject::AddStreamProjectile(FGenObjectID Source, FGenObjectID Shot, FGenObjectID Victim, const FVector3f* VictimPos)
{
	// ProjectileStreamUpdate::addProjectile: a change of target - another object, another spot - breaks the
	// stream with a hole.
	FGenStream& S = *Stream;
	if (S.Owner == GenInvalidObjectID)
	{
		S.Owner = Source;
	}
	if (Victim != GenInvalidObjectID)
	{
		if (Victim != S.TargetObject)
		{
			S.Ids[S.NextFree] = GenInvalidObjectID;
			S.NextFree = (S.NextFree + 1) % FGenStream::Max;
			S.TargetObject = Victim;
		}
		S.TargetPos = FVector3f::ZeroVector;
	}
	else if (VictimPos)
	{
		if (S.TargetPos != *VictimPos)
		{
			S.Ids[S.NextFree] = GenInvalidObjectID;
			S.NextFree = (S.NextFree + 1) % FGenStream::Max;
			S.TargetPos = *VictimPos;
		}
		S.TargetObject = GenInvalidObjectID;
	}
	S.Ids[S.NextFree] = Shot;
	S.NextFree = (S.NextFree + 1) % FGenStream::Max;
	if (S.NextFree == S.FirstValid)
	{
		S.FirstValid = (S.FirstValid + 1) % FGenStream::Max; // (full: the oldest drops off rather than the whole ring)
	}
}

bool FGenObject::UpdateProjectileStream()
{
	// ProjectileStreamUpdate::update: cullFrontOfList - "Chew off the front if they are gone. Don't chew on the
	// middle, as bad ones there are just a break in the chain" - and considerDying: "If I have no projectiles to
	// watch, and my master is dead, then yes, I want to die".
	FGenStream& S = *Stream;
	auto Gone = [this](FGenObjectID Id)
	{
		const FGenObject* P = Logic->FindObject(Id);
		return !P || P->IsDestroyed() || P->IsDead();
	};
	while (S.FirstValid != S.NextFree && Gone(S.Ids[S.FirstValid]))
	{
		S.FirstValid = (S.FirstValid + 1) % FGenStream::Max;
	}
	if (S.FirstValid == S.NextFree && S.Owner != GenInvalidObjectID)
	{
		const FGenObject* Shooter = Logic->FindObject(S.Owner);
		return !Shooter || Shooter->IsDestroyed();
	}
	return false;
}

void UGenGameLogic::GetVisibleStreams(TArray<FStreamLine>& Out) const
{
	for (FGenObjectID Id : ProjectileStreams)
	{
		const FGenObject* O = FindObject(Id);
		if (!O || O->IsDestroyed() || !O->Stream || !O->GetTemplate()->StreamDraw.bValid)
		{
			continue;
		}
		const FGenObject::FGenStream& S = *O->Stream;
		const FGenObject* Owner = FindObject(S.Owner);
		// (Not what the local player can't see: a shooter in the fog, or stealthed.)
		const FGenObject* Seen = Owner ? Owner : O;
		if (Seen->GetLocalShroud() == EGenObjectShroud::Fogged || Seen->GetLocalShroud() == EGenObjectShroud::Shrouded || GetStealthLook(*Seen) == EGenStealthLook::Invisible)
		{
			continue;
		}
		const FGenStreamDraw& D = O->GetTemplate()->StreamDraw;
		// getAllPoints: the shots in flight, oldest first, holes where they are gone; "this makes the stream skim
		// along my roof, if I have a roof".
		TArray<FVector3f, TInlineAllocator<FGenObject::FGenStream::Max>> All;
		const bool bRoof = Owner && Owner->IsKindOf(TEXT("VEHICLE"));
		const FVector3f OwnerPos = Owner ? GetRenderPosition(*Owner) : FVector3f::ZeroVector;
		for (int32 i = S.FirstValid; i != S.NextFree; i = (i + 1) % FGenObject::FGenStream::Max)
		{
			const FGenObject* P = FindObject(S.Ids[i]);
			if (!P || P->IsDestroyed() || P->IsDead())
			{
				All.Add(FVector3f::ZeroVector);
				continue;
			}
			FVector3f Point = GetRenderPosition(*P);
			if (bRoof && FVector2f::Distance(FVector2f(OwnerPos.X, OwnerPos.Y), FVector2f(Point.X, Point.Y)) <= Owner->GetTemplate()->GeometryMajorRadius * 1.5f)
			{
				Point.Z = FMath::Max(Point.Z, OwnerPos.Z + Owner->GetTemplate()->GetMaxHeightAbovePosition() + 0.5f);
			}
			All.Add(Point);
		}
		// W3DProjectileStreamDraw::doDrawModule: only the newest MaxSegments, cut at the holes into lines of two
		// points or more.
		FStreamLine* Line = nullptr;
		for (int32 i = D.MaxSegments > 0 ? FMath::Max(0, All.Num() - D.MaxSegments) : 0; i < All.Num(); ++i)
		{
			if (All[i].IsZero())
			{
				Line = nullptr;
				continue;
			}
			if (!Line)
			{
				Line = &Out.AddDefaulted_GetRef();
				Line->Draw = &D;
			}
			Line->Points.Add(All[i]);
		}
	}
	Out.RemoveAll([](const FStreamLine& L) { return L.Points.Num() < 2; });
}

void FGenObject::UpdateBoneFX(uint32 Frame)
{
	// BoneFXUpdate::update; BoneFXDamage::onBodyDamageStateChange -> changeBodyDamageState: the particle systems
	// of the old state go, and the new state's clocks start ("initTimes").
	const TArray<FGenThingTemplate::FBoneFX>& L = Template->BoneFX;
	FRandomStream& Rand = Logic->GetRandom();
	auto NextDelay = [&Rand](const FGenThingTemplate::FBoneFX& X)
	{
		if (X.MaxDelay <= X.MinDelay)
		{
			return X.MinDelay;
		}
		// (FX lists and creation lists take the logic's random delay, particle systems the client's.)
		return X.Kind == FGenThingTemplate::FBoneFX::ParticleSystem ? FMath::RandRange(X.MinDelay, X.MaxDelay) : Rand.RandRange(X.MinDelay, X.MaxDelay);
	};
	const int32 State = GetDamageState();
	if (State != BoneFXState)
	{
		if (BoneFXState >= 0)
		{
			for (uint32 Sys : BoneFXSystems)
			{
				Logic->DestroyParticleSystem(Sys); // killRunningParticleSystems
			}
			BoneFXSystems.Reset();
		}
		BoneFXState = State;
		BoneFXNext.Init(-1, L.Num());
		BoneFXBones.Init(FVector3f::ZeroVector, L.Num());
		UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
		for (int32 i = 0; i < L.Num(); ++i)
		{
			if (L[i].State != State)
			{
				continue;
			}
			BoneFXNext[i] = int32(Frame) + NextDelay(L[i]);
			// resolveBoneLocations: the bone on the model ("NULL", or none of that name: where the object is).
			TArray<FTransform3f> Bones;
			for (const FGenDrawModule& Draw : Template->Draws)
			{
				if (Assets && !L[i].Bone.Equals(TEXT("NULL"), ESearchCase::IgnoreCase) && Assets->GetPristineBones(Draw.DefaultModel(), L[i].Bone, 0, 1, Bones) > 0)
				{
					BoneFXBones[i] = Bones[0].GetTranslation();
					break;
				}
			}
		}
	}
	for (int32 i = 0; i < L.Num(); ++i)
	{
		if (BoneFXNext[i] < 0 || BoneFXNext[i] > int32(Frame))
		{
			continue;
		}
		const FGenThingTemplate::FBoneFX& X = L[i];
		const FVector3f World = GetTransform().TransformPositionNoScale(BoneFXBones[i]);
		switch (X.Kind)
		{
		case FGenThingTemplate::FBoneFX::FXList:
			Logic->DoFXPos(X.Name, World, nullptr, 0.f, nullptr, 0.f);
			break;
		case FGenThingTemplate::FBoneFX::OCL:
			Logic->DoOCL(X.Name, *this, &World);
			break;
		case FGenThingTemplate::FBoneFX::ParticleSystem:
			if (const uint32 Sys = Logic->CreateObjectParticleSystem(X.Name, ID, BoneFXBones[i]))
			{
				BoneFXSystems.Add(Sys);
			}
			break;
		}
		BoneFXNext[i] = X.bOnlyOnce ? -1 : int32(Frame) + NextDelay(X);
	}
}
