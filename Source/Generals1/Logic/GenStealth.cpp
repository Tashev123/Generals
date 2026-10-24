// Stealth and its detection.
// Follows StealthUpdate.cpp (allowedToStealth, update, markAsDetected, receiveGrant),
// StealthDetectorUpdate.cpp, StealthUpgrade.cpp and GrantStealthBehavior.cpp (the GPS scrambler).
// Not ported: disguises (the bomb truck), a carrier taking its rider's stealth, special abilities.

#include "Logic/GenCommandData.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

void FGenObject::InitStealth()
{
	const uint32 Frame = Logic->GetFrame();
	const FGenStealthData& S = Template->Stealth;
	if (S.bValid)
	{
		// StealthUpdate::StealthUpdate: a unit that disguises itself only "stealths" once disguised, and the
		// module every object has by default sleeps until the GPS scrambler wakes it.
		bStealthEnabled = !S.bDisguisesAsTeam && !S.bGrantedBySpecialPower;
		bCanStealth = S.bInnateStealth;
		StealthAllowedFrame = Frame + uint32(S.StealthDelay);
	}
	const FGenStealthDetector& D = Template->Detector;
	if (D.bValid)
	{
		bDetectorEnabled = !D.bInitiallyDisabled;
		// (The original starts each detector at a random frame within its rate; the object's number spreads
		// them just as well without drawing from the simulation's random stream.)
		NextDetectFrame = Frame + 1 + ID % uint32(FMath::Max(D.Rate, 1));
	}
	GrantStealthRadius = Template->GrantStealth.StartRadius;
}

bool FGenObject::IsFiringWeaponStatus() const
{
	// OBJECT_STATUS_IS_FIRING_WEAPON: in range of its target and working the weapon (firing, between
	// shots, reloading).
	return FiringState[0] != 0 || FiringState[1] != 0 || FiringState[2] != 0;
}

bool FGenObject::AllowedToStealth(uint32 Frame)
{
	// StealthUpdate::allowedToStealth
	const FGenStealthData& S = Template->Stealth;
	const uint32 Flags = S.Forbidden;
	if ((Flags & GenStealth::Attacking) && IsFiringWeaponStatus())
	{
		return false;
	}
	if ((Flags & GenStealth::NoBlackMarket) && NextBlackMarketCheckFrame < Frame)
	{
		// (Looked into only every so often; between looks nothing changes.)
		NextBlackMarketCheckFrame += uint32(S.BlackMarketCheckFrames) + ID % 11;
		if (!Logic->HasFinishedKindOf(Owner, TEXT("FS_BLACK_MARKET")))
		{
			return false;
		}
	}
	if (!bCanStealth)
	{
		return false;
	}
	if ((Flags & GenStealth::TakingDamage) && LastDamageFrame != 0 && LastDamageFrame + 1 >= Frame)
	{
		return false;
	}
	if ((Flags & GenStealth::FiringWeapon) && IsFiringWeaponStatus())
	{
		if ((Flags & GenStealth::FiringWeapon) == GenStealth::FiringWeapon)
		{
			return false;
		}
		// Only the weapons named give it away, and only as they are fired.
		const uint32 LastFrame = Frame > 0 ? Frame - 1 : 0;
		static const uint32 SlotFlags[3] = { GenStealth::FiringPrimary, GenStealth::FiringSecondary, GenStealth::FiringTertiary };
		for (int32 s = 0; s < 3; ++s)
		{
			if ((Flags & SlotFlags[s]) && Weapons[s].Template && Weapons[s].FireCount > 0 && Weapons[s].LastShotFrame >= LastFrame)
			{
				return false;
			}
		}
	}
	// Inside a transport nothing is stealthed; in a garrison it is.
	if (const FGenObject* Container = IsContained() ? Logic->FindObject(ContainedBy) : nullptr)
	{
		if (Container->GetTemplate()->Contain.Type != EGenContainType::Garrison)
		{
			return false;
		}
	}
	if ((Flags & GenStealth::RidersAttacking) && Contain && Logic->IsPassengerAllowedToFireFrom(*this))
	{
		for (FGenObjectID RiderId : Logic->GetRiders(*this))
		{
			const FGenObject* Rider = Logic->FindObject(RiderId);
			if (Rider && Rider->TargetID != GenInvalidObjectID)
			{
				return false;
			}
		}
	}
	if ((Flags & GenStealth::Moving) && Template->bHasPhysics && Velocity.Size() > S.MoveThresholdSpeed)
	{
		return false;
	}
	return true;
}

void FGenObject::UpdateStealth(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Stealth");
	// StealthUpdate::update
	UpdateDisguise(Frame);
	if (!bStealthEnabled)
	{
		return;
	}
	if (AllowedToStealth(Frame))
	{
		if (StealthAllowedFrame > Frame)
		{
			return; // behaving, but not for long enough yet
		}
		if (!bStealthed)
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [stealth f%u] #%u p%d %s is stealthed"), Frame, ID, Owner, *Template->Name);
			Logic->PlayObjectSound(*this, { TEXT("SoundStealthOn") });
		}
		bStealthed = true;
	}
	else
	{
		StealthAllowedFrame = Frame + uint32(Template->Stealth.StealthDelay);
		if (bStealthed)
		{
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [stealth f%u] #%u p%d %s shows itself"), Frame, ID, Owner, *Template->Name);
			// "if you are destealthing on your own free will, play sound for all to hear" (the original plays the "on" sound here too)
			Logic->PlayObjectSound(*this, { TEXT("SoundStealthOn") });
		}
		bStealthed = false;
	}
	const bool bNowDetected = DetectionExpiresFrame > Frame;
	if (bNowDetected && !bDetected)
	{
		Logic->PlayObjectSound(*this, { TEXT("SoundStealthOff") });
	}
	else if (!bNowDetected && bDetected && Owner == Logic->GetLocalPlayer())
	{
		Logic->PlayObjectSound(*this, { TEXT("SoundStealthOn") });
	}
	bDetected = bNowDetected;
}

void FGenObject::DisguiseAs(const FGenObject* Target)
{
	// StealthUpdate::disguiseAsObject
	const FGenStealthData& S = Template->Stealth;
	if (Target)
	{
		// (What is itself disguised passes on the look it has.)
		NextDisguise = Target->DisguiseTemplate ? Target->DisguiseTemplate : Target->Template;
		NextDisguisePlayer = Target->DisguiseTemplate ? Target->DisguisePlayer : Target->Owner;
		bStealthEnabled = true; // ("must be enabled manually if using disguise system")
		DisguiseFramesTotal = DisguiseFramesLeft = FMath::Max(1, S.DisguiseFrames);
		bDisguiseSwapped = false;
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [stealth f%u] #%u p%d %s disguises itself as p%d's %s"), Logic->GetFrame(), ID, Owner, *Template->Name,
			NextDisguisePlayer, *NextDisguise->Name);
	}
	else if (DisguiseTemplate || NextDisguise)
	{
		NextDisguise = nullptr;
		NextDisguisePlayer = INDEX_NONE;
		DisguiseFramesTotal = DisguiseFramesLeft = FMath::Max(1, S.DisguiseRevealFrames);
		bDisguiseSwapped = false;
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [stealth f%u] #%u p%d %s drops its disguise"), Logic->GetFrame(), ID, Owner, *Template->Name);
	}
}

void FGenObject::UpdateDisguise(uint32 Frame)
{
	// StealthUpdate::update, the disguise transition: "switch models at the halfway point".
	if (DisguiseFramesLeft <= 0)
	{
		return;
	}
	--DisguiseFramesLeft;
	const FGenStealthData& S = Template->Stealth;
	const float Factor = 1.f - float(DisguiseFramesLeft) / float(FMath::Max(1, DisguiseFramesTotal));
	if (Factor >= 0.5f && !bDisguiseSwapped)
	{
		static const int32 DisguisedBit = GenConditions::Index(TEXT("DISGUISED"));
		bDisguiseSwapped = true;
		DisguiseTemplate = NextDisguise;
		DisguisePlayer = NextDisguisePlayer;
		SetModelCondition(DisguisedBit, DisguiseTemplate != nullptr);
		if (DisguiseTemplate)
		{
			Logic->PlayObjectSound(*this, { TEXT("DisguiseStarted") });
			if (!S.DisguiseFX.IsEmpty()) { Logic->DoFXPos(S.DisguiseFX, Position, nullptr, 0.f, nullptr, 0.f); }
		}
		else if (!S.DisguiseRevealFX.IsEmpty())
		{
			Logic->DoFXPos(S.DisguiseRevealFX, Position, nullptr, 0.f, nullptr, 0.f);
		}
	}
	if (DisguiseFramesLeft == 0 && !DisguiseTemplate)
	{
		// "We're finished removing disguise so turn off stealth update."
		bStealthEnabled = false;
		bStealthed = false;
		bDetected = false;
	}
}

void FGenObject::MarkAsDetected(uint32 NumFrames)
{
	// StealthUpdate::markAsDetected
	const uint32 Now = Logic->GetFrame();
	if (DisguiseTemplate || NextDisguise)
	{
		DisguiseAs(nullptr); // "If we are disguised, remove the disguise permanently!"
	}
	if (NumFrames == 0)
	{
		DetectionExpiresFrame = Now + uint32(Template->Stealth.StealthDelay);
	}
	else if (DetectionExpiresFrame < Now + NumFrames)
	{
		DetectionExpiresFrame = Now + NumFrames;
	}
	if (Template->Stealth.bOrderIdleEnemiesToAttackMeUponReveal)
	{
		Logic->WakeEnemiesNear(*this);
	}
}

void FGenObject::ReceiveStealthGrant()
{
	// StealthUpdate::receiveGrant (the GPS scrambler): stealthed from now on, by the unit's own rules.
	if (!Template->Stealth.bValid || Template->Stealth.bDisguisesAsTeam)
	{
		return; // "so bombtrucks and stuff do not get foiled by this"
	}
	bStealthEnabled = true;
	bCanStealth = true;
	if (!bStealthed)
	{
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [stealth f%u] #%u p%d %s is granted stealth"), Logic->GetFrame(), ID, Owner, *Template->Name);
	}
	bStealthed = true;
	StealthAllowedFrame = Logic->GetFrame();
}

void FGenObject::UpdateStealthDetector(uint32 Frame)
{
	GEN_PERF_SCOPE("Obj.Detector");
	// StealthDetectorUpdate::update
	const FGenStealthDetector& D = Template->Detector;
	if (!bDetectorEnabled || Frame < NextDetectFrame)
	{
		return;
	}
	NextDetectFrame = Frame + uint32(FMath::Max(D.Rate, 1));
	if (bUnderConstruction)
	{
		NextDetectFrame = Frame + 1; // looks again as soon as it is finished
		return;
	}
	if (SellFrame != 0)
	{
		return;
	}
	if (const FGenObject* Container = IsContained() ? Logic->FindObject(ContainedBy) : nullptr)
	{
		const bool bGarrison = Container->GetTemplate()->Contain.Type == EGenContainType::Garrison;
		if (bGarrison ? !D.bWhileGarrisoned : !D.bWhileContained)
		{
			return;
		}
	}
	const bool bFound = Logic->DetectStealth(*this, D.Range > 0.f ? D.Range : GetVisionRange(), uint32(FMath::Max(D.Rate, 1)));

	// "Make sure the detector is visible to the local player before we add effects or sounds" - and not a
	// stealthed one of somebody else's: the IR show at its bone, and a ping (loud when it found somebody).
	if (LocalShroud <= EGenObjectShroud::PartialClear && (!bStealthed || Owner == Logic->GetLocalPlayer())
		&& (!D.PingSound.IsEmpty() || !D.LoudPingSound.IsEmpty() || !D.IRBeaconParticleSys.IsEmpty() || !D.IRParticleSys.IsEmpty()))
	{
		FVector3f Bone(-1.66f, 5.5f, 15.f); // ("Coord3D bonePosition = {-1.66f,5.5f,15}" when the model has no such bone)
		TArray<FTransform3f> Bones;
		const FGenDrawModule* Draw = Template->PrimaryDraw();
		UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
		if (Draw && Assets && !D.IRParticleSysBone.IsEmpty() && Assets->GetPristineBones(Draw->DefaultModel(), D.IRParticleSysBone, 0, 1, Bones) > 0)
		{
			Bone = Bones[0].GetTranslation();
		}
		const FString& Ping = bFound ? D.IRBrightParticleSys : D.IRParticleSys;
		if (!Ping.IsEmpty())
		{
			Logic->CreateObjectParticleSystem(Ping, ID, Bone);
		}
		if (!D.IRBeaconParticleSys.IsEmpty())
		{
			Logic->CreateObjectParticleSystem(D.IRBeaconParticleSys, ID, Bone);
		}
		Logic->PlayEventForObject(bFound ? D.LoudPingSound : D.PingSound, *this);
	}
}

bool FGenObject::UpdateGrantStealth()
{
	// GrantStealthBehavior::update: a circle that grows, stealthing what it passes over, and is gone.
	const FGenGrantStealth& G = Template->GrantStealth;
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS()); // (the grow rate is per 30 Hz frame)
	GrantStealthRadius += G.GrowRate * RateScale;
	const bool bFinalScan = GrantStealthRadius >= G.FinalRadius || G.GrowRate <= 0.f;
	if (bFinalScan)
	{
		GrantStealthRadius = G.FinalRadius;
	}
	Logic->GrantStealth(*this, GrantStealthRadius);
	if (bFinalScan)
	{
		bPendingDestroy = true;
		return false;
	}
	return true;
}

// ---- the logic's side ---------------------------------------------------------------------------

bool UGenGameLogic::HasFinishedKindOf(int32 Player, const TCHAR* Kind) const
{
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject* O = Objects[Id].Get();
		if (O->GetOwner() == Player && !O->IsDead() && !O->IsDestroyed() && !O->IsUnderConstruction() && !O->IsBeingSold() && O->IsKindOf(Kind))
		{
			return true;
		}
	}
	return false;
}

bool UGenGameLogic::IsPassengerAllowedToFireFrom(const FGenObject& Container) const
{
	return Container.GetContain() && Container.GetContain()->bPassengersFire;
}

void UGenGameLogic::WakeEnemiesNear(const FGenObject& Revealed)
{
	// setWakeupIfInRange: idle enemies that can see the spot look for a target now rather than at their next turn.
	const FVector2f At(Revealed.Position.X, Revealed.Position.Y);
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject& O = *Objects[Id];
		if (!O.bHasAI || O.bDead || GetRelationship(O.Owner, Revealed.Owner) != EGenRelationship::Enemies)
		{
			continue;
		}
		const float Vision = O.GetVisionRange();
		if (FVector2f::DistSquared(FVector2f(O.Position.X, O.Position.Y), At) <= Vision * Vision)
		{
			O.NextAutoAcquireFrame = 0;
		}
	}
}

bool UGenGameLogic::DetectStealth(FGenObject& Detector, float Range, uint32 Rate)
{
	bool bFoundSomeone = false;
	const FGenStealthDetector& D = Detector.Template->Detector;
	const FVector2f At(Detector.Position.X, Detector.Position.Y);
	auto Announce = [this](const TCHAR* Label, const TCHAR* Fallback, const FLinearColor& Color)
	{
		FString Message = FGenCommandData::Get().GetText(Label);
		if (Message.IsEmpty() || Message.Equals(Label, ESearchCase::IgnoreCase)) { Message = Fallback; }
		FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
		Text.Text = Message;
		Text.Color = Color;
		Text.Frame = Frame;
		Text.bScreen = true;
	};
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject& Them = *Objects[Id];
		// Enemies and neutrals, within range (centre to centre), of the kinds this detector looks for.
		if (&Them == &Detector || Them.bDead || Them.bPendingDestroy || GetRelationship(Detector.Owner, Them.Owner) == EGenRelationship::Allies
			|| FVector2f::DistSquared(FVector2f(Them.Position.X, Them.Position.Y), At) > Range * Range)
		{
			continue;
		}
		bool bKindOk = true;
		for (const FString& Kind : D.RequiredKindOf) { bKindOk &= Them.IsKindOf(*Kind); }
		for (const FString& Kind : D.ForbiddenKindOf) { bKindOk &= !Them.IsKindOf(*Kind); }
		if (!bKindOk)
		{
			continue;
		}
		if (Them.bStealthed && Them.Template->Stealth.bValid)
		{
			bFoundSomeone = true; // "we have found someone"
			if (!Them.bDetected)
			{
				UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [stealth f%u] #%u p%d %s detects #%u p%d %s"), Frame, Detector.ID, Detector.Owner, *Detector.Template->Name,
					Them.ID, Them.Owner, *Them.Template->Name);
				if (Detector.Owner == LocalPlayer && TryRadarEvent(EGenRadarEvent::StealthDiscovered, Them.Position))
				{
					PlayMiscSound(TEXT("StealthDiscoveredSound"), LocalPlayer);
					Announce(TEXT("MESSAGE:StealthDiscovered"), TEXT("Stealth unit discovered"), FLinearColor(0.5f, 1.f, 0.5f));
				}
				if (Them.Owner == LocalPlayer)
				{
					CreateRadarEvent(Them.Position, EGenRadarEvent::StealthNeutralized);
					PlayMiscSound(TEXT("StealthNeutralizedSound"), LocalPlayer);
					Announce(TEXT("MESSAGE:StealthNeutralized"), TEXT("Our stealth unit has been detected"), FLinearColor(1.f, 0.6f, 0.3f));
				}
			}
			Them.MarkAsDetected(Rate + 1);
		}
		else if (Them.Contain && Them.Template->Contain.Type == EGenContainType::Garrison)
		{
			// "Perhaps they are garrisoning something stealthy, eh?"
			for (FGenObjectID RiderId : GetRiders(Them))
			{
				FGenObject* Rider = FindObject(RiderId);
				if (Rider && Rider->Template->Stealth.bValid)
				{
					bFoundSomeone = true;
					if (GetRelationship(Detector.Owner, Rider->Owner) != EGenRelationship::Allies)
					{
						Rider->MarkAsDetected(Rate + 2);
					}
				}
			}
		}
	}
	return bFoundSomeone;
}

void UGenGameLogic::GrantStealth(const FGenObject& Grantor, float Radius)
{
	const FGenGrantStealth& G = Grantor.Template->GrantStealth;
	const FVector2f At(Grantor.Position.X, Grantor.Position.Y);
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject& O = *Objects[Id];
		if (&O == &Grantor || O.bDead || O.bPendingDestroy || GetRelationship(Grantor.Owner, O.Owner) != EGenRelationship::Allies
			|| FVector2f::DistSquared(FVector2f(O.Position.X, O.Position.Y), At) > Radius * Radius)
		{
			continue;
		}
		bool bKind = G.KindOf.Num() == 0;
		for (const FString& Kind : G.KindOf) { bKind |= O.IsKindOf(*Kind); }
		if (bKind)
		{
			O.ReceiveStealthGrant();
		}
	}
}

EGenStealthLook UGenGameLogic::GetStealthLook(const FGenObject& O) const
{
	// StealthUpdate::calcStealthedStatusForPlayer, for the local player.
	if (!O.bStealthed || O.bDead)
	{
		return EGenStealthLook::None; // "making sure he turns visible when he dies"
	}
	if (O.Template->Stealth.bDisguisesAsTeam)
	{
		// "Disguised stuff doesn't work well when combined with the normal detected stuff, so special case
		// it": it is in plain sight - to an enemy as one of his own (STEALTHLOOK_DISGUISED_ENEMY), to us as it is.
		return EGenStealthLook::None;
	}
	// (Whoever is out of the game watches as everybody's friend.)
	const bool bFriendly = GetRelationship(LocalPlayer, O.Owner) == EGenRelationship::Allies || IsPlayerDefeated(LocalPlayer);
	if (O.bDetected)
	{
		return bFriendly ? EGenStealthLook::VisibleFriendlyDetected : EGenStealthLook::VisibleDetected;
	}
	return bFriendly ? EGenStealthLook::VisibleFriendly : EGenStealthLook::Invisible;
}
