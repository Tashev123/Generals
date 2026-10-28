// Special powers (the general's powers fired from a command center), and what they send:
// aircraft that deliver a payload, the bombs and parachutes they drop, markers that act once and vanish.
// Follows SpecialPowerModule.cpp, OCLSpecialPower.cpp, CashHackSpecialPower.cpp, the DeliverPayload nugget of
// ObjectCreationList.cpp, DeliverPayloadAIUpdate.cpp, HeightDieUpdate.cpp, FireWeaponWhenDeadBehavior.cpp,
// ParachuteContain.cpp (much simplified) and DeletionUpdate.cpp.

#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioManager.h"
#include "Client/GenFXData.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Logic/GenUpgrades.h"
#include "Map/GenMapData.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

namespace
{
	constexpr float CreateAboveLocationHeight = 300.f;  // CREATE_ABOVE_LOCATION_HEIGHT

	int32 ParachutingBit() { static const int32 I = GenConditions::Index(TEXT("PARACHUTING")); return I; }
	int32 FreeFallBit() { static const int32 I = GenConditions::Index(TEXT("FREEFALL")); return I; }
}

// ---- the map's edges ----------------------------------------------------------------------------

void UGenGameLogic::GetMapExtent(FVector2f& OutLo, FVector2f& OutHi, bool bIncludeBorder) const
{
	// TerrainLogic::getExtent / getExtentIncludingBorder: world (0, 0) is the corner of the playable area.
	const FGenMapData* Map = GetMap();
	const float Cell = GenLogic::PathfindCellSize;
	const float Border = Map ? Map->Border * Cell : 0.f;
	OutLo = FVector2f::ZeroVector;
	OutHi = Map ? FVector2f((Map->Width - 2 * Map->Border) * Cell, (Map->Height - 2 * Map->Border) * Cell) : FVector2f(1000.f, 1000.f);
	if (bIncludeBorder)
	{
		OutLo -= FVector2f(Border, Border);
		OutHi += FVector2f(Border, Border);
	}
}

FVector3f UGenGameLogic::FindEdgePoint(const FVector3f& Pos, bool bFarthest) const
{
	// TerrainLogic::findClosestEdgePoint / findFarthestEdgePoint: the point on the map's rim straight out from Pos.
	FVector2f Lo, Hi;
	GetMapExtent(Lo, Hi, false);
	const FVector2f Candidates[4] = { FVector2f(Lo.X, Pos.Y), FVector2f(Hi.X, Pos.Y), FVector2f(Pos.X, Lo.Y), FVector2f(Pos.X, Hi.Y) };
	int32 Best = 0;
	float BestDist = bFarthest ? -1.f : FLT_MAX;
	for (int32 i = 0; i < 4; ++i)
	{
		const float Dist = FVector2f::DistSquared(Candidates[i], FVector2f(Pos.X, Pos.Y));
		if (bFarthest ? Dist > BestDist : Dist < BestDist)
		{
			BestDist = Dist;
			Best = i;
		}
	}
	return FVector3f(Candidates[Best].X, Candidates[Best].Y, GetGroundHeight(Candidates[Best].X, Candidates[Best].Y));
}

// ---- special powers -----------------------------------------------------------------------------

int32 UGenGameLogic::FindPowerModule(const FGenObject& Obj, const FString& Power) const
{
	const TArray<FGenSpecialPowerModule>& Modules = Obj.GetTemplate()->PowerModules;
	return Modules.IndexOfByPredicate([&Power](const FGenSpecialPowerModule& M) { return M.Power.Equals(Power, ESearchCase::IgnoreCase); });
}

bool UGenGameLogic::IsPowerReady(const FGenObject& Obj, int32 Module, uint32* OutReadyFrame, FString* OutWhy) const
{
	auto No = [OutWhy](const FString& Why) { if (OutWhy) { *OutWhy = Why; } return false; };
	if (OutReadyFrame) { *OutReadyFrame = 0; }
	const TArray<FGenSpecialPowerModule>& Modules = Obj.GetTemplate()->PowerModules;
	if (!Modules.IsValidIndex(Module) || !Players.IsValidIndex(Obj.GetOwner()) || Obj.IsDead() || Obj.IsUnderConstruction() || Obj.IsBeingSold())
	{
		return No(TEXT("Not available"));
	}
	const FGenSpecialPowerModule& M = Modules[Module];
	const FGenSpecialPowerTemplate* T = FGenUpgradeData::Get().FindPower(M.Power);
	if (!T)
	{
		return No(TEXT("Not available"));
	}
	if (!T->RequiredScience.IsEmpty() && !HasScience(Obj.GetOwner(), T->RequiredScience))
	{
		return No(TEXT("Requires a general's promotion"));
	}
	if (Obj.IsDisabled())
	{
		return No(TEXT("Disabled")); // (SpecialPowerModule::isReady: not while its countdown is paused)
	}
	if (M.Type == EGenPowerModule::SpecialAbility && T->bPorted && Obj.FindAbility(M.Power) != INDEX_NONE && Obj.IsPowerPaused(Module))
	{
		return No(TEXT("Requires an upgrade"));
	}
	if (M.Type == EGenPowerModule::Other || M.Type == EGenPowerModule::CashBounty || !T->bPorted || Obj.IsPowerPaused(Module)
		|| (M.Type == EGenPowerModule::SpecialAbility && Obj.FindAbility(M.Power) == INDEX_NONE
			// (The particle cannon's is a SpecialAbility too, with "UpdateModuleStartsAttack": its own update does the work.)
			&& !(Obj.GetTemplate()->ParticleCannon.bValid && Obj.GetTemplate()->ParticleCannon.Power.Equals(M.Power, ESearchCase::IgnoreCase))
			// (... and so is the one that sends for a gunship: SpectreGunshipDeploymentUpdate.)
			&& !Obj.GetTemplate()->GunshipDeployments.ContainsByPredicate([&M](const FGenGunshipDeployment& G) { return G.Power.Equals(M.Power, ESearchCase::IgnoreCase); })
			// (... and the strategy center's: BattlePlanUpdate.)
			&& !(Obj.GetTemplate()->BattlePlan.bValid && Obj.GetTemplate()->BattlePlan.Power.Equals(M.Power, ESearchCase::IgnoreCase))))
	{
		return No(TEXT("Not available yet"));
	}
	// One timer for the whole player (general's powers), or the building's own (superweapons).
	uint32 Ready = Obj.GetPowerReadyFrame(Module);
	if (T->bSharedSyncedTimer)
	{
		const uint32* Shared = Players[Obj.GetOwner()].PowerReadyFrame.Find(T->Name.ToLower());
		Ready = Shared ? *Shared : 0;
	}
	// (-GenPowersReady: test runs don't wait out a superweapon's five minutes.)
	if (bPowersReady)
	{
		Ready = 0;
	}
	if (OutReadyFrame) { *OutReadyFrame = Ready; }
	if (Frame < Ready)
	{
		const int32 Seconds = FMath::CeilToInt32(float(Ready - Frame) / float(GenLogic::GetLogicFPS()));
		return No(FString::Printf(TEXT("Ready in %d:%02d"), Seconds / 60, Seconds % 60));
	}
	return true;
}

FGenObject* UGenGameLogic::FindPowerSource(int32 Player, const FString& Power, bool bMustBeReady, int32* OutModule) const
{
	FGenObject* Fallback = nullptr;
	int32 FallbackModule = INDEX_NONE;
	for (FGenObjectID Id : ObjectOrder)
	{
		FGenObject* O = Objects[Id].Get();
		if (O->GetOwner() != Player || O->IsDead() || O->IsDestroyed() || O->IsUnderConstruction())
		{
			continue;
		}
		if (O->GetTemplate()->Gunship.bValid)
		{
			continue; // (the gunship carries the power's module too, but it is what the power sends, not who sends it)
		}
		const int32 Module = FindPowerModule(*O, Power);
		if (Module == INDEX_NONE || O->GetTemplate()->PowerModules[Module].bScriptedOnly)
		{
			continue;
		}
		if (IsPowerReady(*O, Module))
		{
			if (OutModule) { *OutModule = Module; }
			return O;
		}
		if (!Fallback)
		{
			Fallback = O;
			FallbackModule = Module;
		}
	}
	if (OutModule) { *OutModule = FallbackModule; }
	return bMustBeReady ? nullptr : Fallback;
}

void UGenGameLogic::IssueSpecialPower(FGenObjectID Source, const FString& Power, const FVector3f& TargetPos, FGenObjectID TargetObj)
{
	FGenCommand C(EGenCommandType::SpecialPower);
	C.Units = { Source };
	C.Name = Power;
	C.Pos = TargetPos;
	C.Target = TargetObj;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecSpecialPower(const FGenCommand& C)
{
	const FGenObjectID Source = C.Units.Num() > 0 ? C.Units[0] : GenInvalidObjectID;
	FGenObject* S = FindObject(Source);
	const int32 Module = S ? FindPowerModule(*S, C.Name) : INDEX_NONE;
	const bool bFired = Module != INDEX_NONE && FireSpecialPower(*S, Module, C.Pos, C.Target);
	UE_CLOG(bTrace && !bFired, LogGenerals, Log, TEXT("  [power f%u] %s from #%u %s at (%.0f, %.0f): not fired (%s)"), Frame, *C.Name, Source,
		S ? *S->GetTemplate()->Name : TEXT("nothing"), C.Pos.X, C.Pos.Y, Module == INDEX_NONE ? TEXT("no such power there") : TEXT("not ready"));
}

bool UGenGameLogic::FireSpecialPower(FGenObject& Source, int32 Module, const FVector3f& InTargetPos, FGenObjectID TargetObj)
{
	// (A particle cannon whose beam is burning: the order says where the beam goes next.)
	if (Source.GetTemplate()->ParticleCannon.bValid && Source.GetTemplate()->PowerModules.IsValidIndex(Module)
		&& Source.SteerParticleCannon(Source.GetTemplate()->PowerModules[Module].Power,
			FVector3f(InTargetPos.X, InTargetPos.Y, GetGroundHeight(InTargetPos.X, InTargetPos.Y)), Frame))
	{
		return true;
	}
	if (!IsPowerReady(Source, Module))
	{
		return false;
	}
	const FGenSpecialPowerModule& M = Source.GetTemplate()->PowerModules[Module];
	const FGenSpecialPowerTemplate& T = *FGenUpgradeData::Get().FindPower(M.Power);
	const int32 Player = Source.GetOwner();
	FVector3f TargetPos = InTargetPos;
	const FGenObject* Victim = FindObject(TargetObj);
	if (Victim)
	{
		TargetPos = Victim->GetPosition();
	}
	TargetPos.Z = GetGroundHeight(TargetPos.X, TargetPos.Y);

	const bool bCannon = Source.BeginParticleCannon(M.Power, TargetPos, Frame) || DeployGunship(Source, M.Power, TargetPos);
	switch (bCannon ? EGenPowerModule::Other : M.Type)
	{
	case EGenPowerModule::Other:
		if (!bCannon)
		{
			return false;
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] p%d fires %s at (%.0f, %.0f)"), Frame, Player, *T.Name, TargetPos.X, TargetPos.Y);
		break;
	case EGenPowerModule::SpecialAbility:
	{
		// SpecialAbility::doSpecialPowerAtObject: the unit's SpecialAbilityUpdate takes it from here; the
		// power starts recharging when the unit gets down to it.
		// (ActionManager::canDoSpecialPowerAtObject; with nothing aimed at, canDoSpecialPower: only the
		// remote charges do anything then - they go off.)
		const bool bAtPosition = !Victim && T.AbilityKind == EGenAbilityKind::HelixNapalmBomb;
		if (TargetObj != GenInvalidObjectID && !Victim)
		{
			return false; // what it was aimed at is gone
		}
		if (!bAtPosition && !CanUseAbilityOn(Source, Module, Victim, false))
		{
			return false;
		}
		const FString Hunt = Source.HuntButton; // (the order may come from the unit's own hunt)
		Source.OnPlayerCommand(false);
		Source.HuntButton = Hunt;
		if (!Source.StartAbility(Source.FindAbility(M.Power), TargetObj, bAtPosition ? &TargetPos : nullptr))
		{
			return false;
		}
		PlaySoundAt(M.InitiateSound, Source);
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [ability f%u] #%u p%d %s sets off to use %s on #%u"), Frame, Source.GetID(), Player, *Source.GetTemplate()->Name, *T.Name, TargetObj);
		return true;
	}
	case EGenPowerModule::CleanupArea:
	{
		// CleanupAreaPower::doSpecialPowerAtLocation: "if (getObject()->isDisabled()) return;" - and its
		// CleanupHazardUpdate takes it from there.
		if (Source.IsDisabled() || !Source.GetTemplate()->CleanupHazard.bValid)
		{
			return false;
		}
		Source.OnPlayerCommand(false);
		Source.SetCleanupAreaParameters(TargetPos, M.CleanupMoveRange);
		PlaySoundAt(M.InitiateSound, Source);
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [cleanup f%u] #%u p%d %s is sent to clean up round (%.0f, %.0f), %.0f about"), Frame, Source.GetID(), Player,
			*Source.GetTemplate()->Name, TargetPos.X, TargetPos.Y, M.CleanupMoveRange);
		break;
	}
	case EGenPowerModule::OCL:
	{
		// OCLSpecialPower::findOCL: the strongest version whose science the player owns.
		FString OCLName = M.OCL;
		for (const TPair<FString, FString>& Up : M.UpgradeOCLs)
		{
			if (HasScience(Player, Up.Key))
			{
				OCLName = Up.Value;
				break;
			}
		}
		if (M.bAdjustToPassable && !Pathfinder.IsPassable(FVector2f(TargetPos.X, TargetPos.Y), GenSurface::Ground))
		{
			TargetPos = FindPositionAround(TargetPos, GenLogic::PathfindCellSize, 500.f, GenSurface::Ground);
		}
		FVector3f CreatePos = TargetPos;
		if (M.CreateLocation == TEXT("CREATE_AT_EDGE_NEAR_SOURCE")) { CreatePos = FindEdgePoint(Source.GetPosition(), false); }
		else if (M.CreateLocation == TEXT("CREATE_AT_EDGE_NEAR_TARGET")) { CreatePos = FindEdgePoint(TargetPos, false); }
		else if (M.CreateLocation == TEXT("CREATE_AT_EDGE_FARTHEST_FROM_TARGET")) { CreatePos = FindEdgePoint(TargetPos, true); CreatePos.Z += CreateAboveLocationHeight; }
		else if (M.CreateLocation == TEXT("CREATE_ABOVE_LOCATION")) { CreatePos.Z += CreateAboveLocationHeight; }
		// OCLSpecialPower's ReferenceObject (the sneak attack's tunnel): the power puts a building down, and
		// only where that building could be built.
		const FGenThingTemplate* Reference = M.ReferenceObject.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(M.ReferenceObject);
		if (Reference && !IsLocationLegalToBuild(*Reference, TargetPos, Reference->PlacementViewAngle))
		{
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] p%d: %s can't go at (%.0f, %.0f) - no %s could be built there"), Frame, Player, *T.Name, TargetPos.X, TargetPos.Y, *Reference->Name);
			return false;
		}
		RunPowerOCL(OCLName, Source, CreatePos, TargetPos, Reference ? &Reference->PlacementViewAngle : nullptr);
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] p%d fires %s (%s) at (%.0f, %.0f) from (%.0f, %.0f)"), Frame, Player, *T.Name, *OCLName,
			TargetPos.X, TargetPos.Y, CreatePos.X, CreatePos.Y);
		break;
	}
	case EGenPowerModule::CashHack:
	{
		// CashHackSpecialPower: money straight out of the pocket of whoever owns the targeted building.
		if (!Victim || !Players.IsValidIndex(Victim->GetOwner()) || GetRelationship(Player, Victim->GetOwner()) != EGenRelationship::Enemies)
		{
			return false;
		}
		int32 Amount = M.MoneyAmount;
		for (const TPair<FString, int32>& Up : M.UpgradeMoney)
		{
			if (HasScience(Player, Up.Key))
			{
				Amount = Up.Value;
				break;
			}
		}
		FGenPlayer& From = Players[Victim->GetOwner()];
		Amount = Withdraw(Victim->GetOwner(), Amount);
		Deposit(Player, Amount);
		ScoreMoneyEarned(Player, Amount);
		FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
		Text.Text = FString::Printf(TEXT("-$%d"), Amount);
		Text.Pos = Victim->GetPosition();
		Text.Color = From.Color;
		Text.Frame = Frame;
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] p%d fires %s: $%d taken from p%d"), Frame, Player, *T.Name, Amount, From.Index);
		break;
	}
	case EGenPowerModule::SpyVision:
	{
		// SpyVisionSpecialPower -> SpyVisionUpdate::activateSpyVision: for the duration, what the enemy's
		// units see, this player sees too. (Kept per player here; the original marks each enemy unit.)
		FGenPlayer& Spy = Players[Player];
		Spy.SpyVisionEndFrame = Frame + uint32(FMath::Max(M.Duration, 1));
		Spy.SpyVisionSource = Source.GetID();
		for (FGenPlayer& Other : Players)
		{
			if (GetRelationship(Player, Other.Index) == EGenRelationship::Enemies)
			{
				Other.SpiedByMask |= 1u << Player;
			}
		}
		UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] p%d fires %s: sees what the enemy sees until f%u"), Frame, Player, *T.Name, Spy.SpyVisionEndFrame);
		break;
	}
	default:
		return false;
	}

	// SpecialPowerModule::startPowerRecharge
	if (T.bSharedSyncedTimer)
	{
		Players[Player].PowerReadyFrame.Add(T.Name.ToLower(), Frame + uint32(T.ReloadTime));
	}
	else if (Source.PowerReady.IsValidIndex(Module))
	{
		Source.PowerReady[Module] = Frame + uint32(T.ReloadTime);
	}
	++Players[Player].PowersFired;
	Source.OnLauncherPowerFired(T.Name, Frame);
	// SpecialPowerModule::aboutToDoSpecialPower: "Let EVA do her thing" - "all players need to hear these warnings".
	EvaSuperweapon(TEXT("Launched"), Source, T);
	// SpecialPowerModule::createViewObject: its owner sees the spot it was aimed at for a while (ViewObjectDuration,
	// ViewObjectRange) - the superweapons, the generals' strikes and drops, a charge planted.
	RevealAreaFor(Player, FVector2f(TargetPos.X, TargetPos.Y), T.ViewObjectRange, uint32(T.ViewObjectFrames));
	// SpecialPowerModule::initiateIntentToDoSpecialPower: what its owner hears, and what is heard where it is aimed.
	if (Audio)
	{
		Audio->PlayUI(T.InitiateSound, Player);
		Audio->PlayAt(T.InitiateAtLocationSound, TargetPos, Player);
	}
	return true;
}

void UGenGameLogic::IssueCapture(const TArray<FGenObjectID>& Units, FGenObjectID Target)
{
	FGenCommand C(EGenCommandType::Capture);
	C.Units = Units;
	C.Target = Target;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecCapture(const FGenCommand& C)
{
	const FGenObject* Victim = FindObject(C.Target);
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (!U || !Victim || U->IsDead())
		{
			continue;
		}
		const TArray<FGenSpecialPowerModule>& Modules = U->GetTemplate()->PowerModules;
		for (int32 i = 0; i < Modules.Num(); ++i)
		{
			const FGenSpecialPowerTemplate* T = FGenUpgradeData::Get().FindPower(Modules[i].Power);
			if (Modules[i].Type == EGenPowerModule::SpecialAbility && T && T->Enum.EndsWith(TEXT("SPECIAL_INFANTRY_CAPTURE_BUILDING"), ESearchCase::IgnoreCase))
			{
				FireSpecialPower(*U, i, Victim->GetPosition(), C.Target);
				break;
			}
		}
	}
}

void UGenGameLogic::IssueFireWeapon(const TArray<FGenObjectID>& Units, FGenObjectID Target, int32 Slot)
{
	FGenCommand C(EGenCommandType::FireWeapon);
	C.Units = Units;
	C.Target = Target;
	C.A = Slot;
	SubmitCommand(MoveTemp(C));
}

void UGenGameLogic::ExecFireWeapon(const FGenCommand& C)
{
	const FGenObject* Victim = FindObject(C.Target);
	const int32 Slot = C.A;
	for (FGenObjectID Id : C.Units)
	{
		FGenObject* U = FindObject(Id);
		if (U && Victim && !U->IsDead() && !U->IsContained() && Slot >= 0 && Slot < 3 && U->GetWeapon(Slot).Template)
		{
			U->OnPlayerCommand(false);
			U->CommandAttackWithSlot(C.Target, Slot);
		}
	}
}

void UGenGameLogic::AddFloatingText(const FString& InText, const FVector3f& Pos, const FLinearColor& Color)
{
	FGenFloatingText& Text = FloatingTexts.AddDefaulted_GetRef();
	Text.Text = InText;
	Text.Pos = Pos;
	Text.Color = Color;
	Text.Frame = Frame;
}

void UGenGameLogic::PlaySoundAt(const FString& Event, const FGenObject& Obj)
{
	if (Audio && !Event.IsEmpty())
	{
		Audio->PlayAt(Event, Obj.GetPosition(), Obj.GetOwner());
	}
}

void UGenGameLogic::PlayEventForObject(const FString& Event, const FGenObject& Obj)
{
	if (Audio && !Event.IsEmpty())
	{
		Audio->PlayForObject(Event, Obj);
	}
}

void UGenGameLogic::SetAbilityLoopSound(const FGenObject& Obj, const FString& Event)
{
	if (Audio)
	{
		Audio->SetObjectLoop(Obj.GetID(), 6, Event); // (0 moving, 1 ambient, 2-4 weapons, 5 a helicopter going down)
	}
}

void UGenGameLogic::RunPowerOCL(const FString& OCLName, const FGenObject& Source, const FVector3f& CreatePos, const FVector3f& TargetPos, const float* PlacedAngle)
{
	const FGenOCL* OCL = FGenFXData::Get().FindOCL(OCLName);
	if (!OCL)
	{
		return;
	}
	const FGenGameData& GD = FGenGameData::Get();
	const int32 Player = Source.GetOwner();

	// CreateObject nuggets: things that simply appear (ambushing infantry, a drone, a marker that acts once).
	for (const FGenOCLNugget& N : OCL->Nuggets)
	{
		if (N.bDebris || N.Names.Num() == 0)
		{
			continue;
		}
		for (int32 n = 0; n < N.Count; ++n)
		{
			const FGenThingTemplate* T = GD.FindTemplate(N.Names[Random.RandRange(0, N.Names.Num() - 1)]);
			if (!T)
			{
				continue;
			}
			FVector3f Pos = CreatePos;
			if (N.bSpreadFormation && N.Count > 1)
			{
				// (Around the spot rather than all on it; on ground they can stand on.)
				const float Angle = 2.f * PI * (float(n) + Random.FRandRange(-0.2f, 0.2f)) / float(N.Count);
				const float Radius = N.MinDistanceFormation * Random.FRandRange(1.f, 1.5f);
				Pos.X += FMath::Cos(Angle) * Radius;
				Pos.Y += FMath::Sin(Angle) * Radius;
				if (!Pathfinder.IsPassable(FVector2f(Pos.X, Pos.Y), GenSurface::Ground))
				{
					Pos = FindPositionAround(Pos, GenLogic::PathfindCellSize, 100.f, GenSurface::Ground);
				}
			}
			const float Ground = GetGroundHeight(Pos.X, Pos.Y);
			Pos.Z = FMath::Max(Pos.Z, Ground);
			// (ObjectCreationList::create with an angle: a building the power puts down faces the way it was placed.)
			if (FGenObject* O = CreateObject(T, Player, Pos, PlacedAngle ? *PlacedAngle : Random.FRandRange(-PI, PI)))
			{
				O->Producer = Source.GetID();
				if (!T->GrantStealth.RadiusParticleSystem.IsEmpty())
				{
					AttachParticleSystem(T->GrantStealth.RadiusParticleSystem, O->GetID()); // GrantStealthBehavior's radius effect
				}
				if (T->WeaponBonusPulse.bValid)
				{
					// (A marker without a model that is gone again in a frame: what its look carries - the frenzy's cloud - is set off here.)
					if (const FGenDrawModule* Draw = T->PrimaryDraw())
					{
						const int32 StateIndex = Draw->FindBestState(FGenConditionBits());
						for (const TPair<FString, FString>& Bone : Draw->States.IsValidIndex(StateIndex) ? Draw->States[StateIndex].ParticleBones : TArray<TPair<FString, FString>>())
						{
							AttachParticleSystem(Bone.Value, O->GetID());
						}
					}
				}
			}
		}
	}

	// DeliverPayload nuggets: aircraft from the map's edge.
	for (const FGenDeliverPayload& P : OCL->Payloads)
	{
		StartDelivery(P, Source, CreatePos, TargetPos);
	}

	// FireWeapon and Attack nuggets: the superweapons - "you must have a primary and secondary source for this effect".
	if (FGenObject* Shooter = FindObject(Source.GetID()))
	{
		for (const FString& WeaponName : OCL->FireWeapons)
		{
			if (const FGenWeaponTemplate* W = GD.FindWeapon(WeaponName))
			{
				FireTempWeapon(*W, *Shooter, TargetPos);
			}
		}
		for (const FGenOCL::FAttack& A : OCL->Attacks)
		{
			// "lock merely fires till the weapon is empty or the attack is 'done'"
			Shooter->CommandAttackPosition(TargetPos, A.NumShots, A.Slot);
			UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [power f%u] #%u %s attacks (%.0f, %.0f) with %d shots"), Frame, Shooter->GetID(),
				*Shooter->GetTemplate()->Name, TargetPos.X, TargetPos.Y, A.NumShots);
		}
	}
}

void UGenGameLogic::StartDelivery(const FGenDeliverPayload& P, const FGenObject& Source, const FVector3f& Primary, const FVector3f& Secondary)
{
	// DeliverPayloadNugget::create
	const FGenThingTemplate* TransportT = FGenGameData::Get().FindTemplate(P.Transport);
	if (!TransportT)
	{
		return;
	}
	FVector2f Dir(Primary.X - Secondary.X, Primary.Y - Secondary.Y);
	if (!Dir.Normalize())
	{
		Dir = FVector2f(1.f, 0.f);
	}
	// A formation fans out to both sides behind its leader.
	const FVector2f CCW(-Dir.Y + Dir.X, Dir.X + Dir.Y);
	const FVector2f CW(Dir.Y + Dir.X, -Dir.X + Dir.Y);
	int32 TotalItems = 0;
	for (const TPair<FString, int32>& Item : P.Payload)
	{
		TotalItems += Item.Value;
	}

	for (int32 Index = 0; Index < P.FormationSize; ++Index)
	{
		const float Multiplier = float(int32((Index + 1) / 2 * P.FormationSpacing));
		const FVector2f Offset = (P.FormationSize > 1 ? ((Index % 2) ? CCW : CW) : FVector2f::ZeroVector) * Multiplier;
		FVector3f StartPos(Primary.X + Offset.X, Primary.Y + Offset.Y, Primary.Z);
		const FVector3f MoveTo(Secondary.X + Offset.X, Secondary.Y + Offset.Y, Secondary.Z);
		FVector3f Target(Secondary.X + Offset.X * (1.f - P.WeaponConvergenceFactor), Secondary.Y + Offset.Y * (1.f - P.WeaponConvergenceFactor), Secondary.Z);
		if (P.WeaponErrorRadius > 1.f && Index > 0)
		{
			const float R = Random.FRandRange(0.f, P.WeaponErrorRadius);
			const float A = Random.FRandRange(0.f, 2.f * PI);
			Target.X += R * FMath::Cos(A);
			Target.Y += R * FMath::Sin(A);
		}
		Target.Z = GetGroundHeight(Target.X, Target.Y);
		const float Orient = FMath::Atan2(MoveTo.Y - StartPos.Y, MoveTo.X - StartPos.X);
		if (P.DeliveryDistance > 0.f)
		{
			// (Back far enough that it is at speed and level by the time it gets there.)
			StartPos.X -= FMath::Cos(Orient) * P.DeliveryDistance * 1.5f;
			StartPos.Y -= FMath::Sin(Orient) * P.DeliveryDistance * 1.5f;
		}
		FGenObject* Transport = CreateObject(TransportT, Source.GetOwner(), StartPos, Orient);
		if (!Transport)
		{
			continue;
		}
		Transport->Producer = Source.GetID();
		const FGenLocomotorTemplate* Loco = Transport->Locomotor;
		if (P.bStartAtPreferredHeight && Loco)
		{
			StartPos.Z = GetGroundHeight(StartPos.X, StartPos.Y) + Loco->PreferredHeight;
			Transport->SetPosition(StartPos);
		}
		if (P.bStartAtMaxSpeed && Loco)
		{
			Transport->Velocity = FVector3f(FMath::Cos(Orient), FMath::Sin(Orient), 0.f) * Loco->Speed;
		}
		Transport->SnapshotPrevious();

		Transport->Delivery = MakeUnique<FGenObject::FDelivery>();
		if (P.DeliveryDecal.IsValid() && P.DeliveryDecalRadius > 0.f)
		{
			DeliveryMarkers.RemoveAll([this](FGenObjectID Id)
			{
				const FGenObject* O = FindObject(Id);
				const FGenRadiusDecalTemplate* T = nullptr;
				FVector3f Pos;
				float Radius = 0.f;
				return !O || !O->GetDeliveryMarker(Pos, Radius, T);
			});
			DeliveryMarkers.Add(Transport->GetID());
		}
		FGenObject::FDelivery& D = *Transport->Delivery;
		D.Data = &P;
		D.Target = Target;
		D.MoveTo = FVector3f(MoveTo.X, MoveTo.Y, GetGroundHeight(MoveTo.X, MoveTo.Y));
		D.ItemsLeft = TotalItems;
		D.VisibleLeft = P.VisibleNumBones;
		D.StartFrame = P.DelayDeliveryMax > 0 ? Frame + uint32(Random.RandRange(0, P.DelayDeliveryMax)) : 0;
		// What hangs under the wings shows until it is fired.
		for (int32 i = 1; !P.VisibleSubObjectBaseName.IsEmpty() && i <= P.VisibleNumBones; ++i)
		{
			Transport->SubObjectOverrides.Emplace(FString::Printf(TEXT("%s%02d"), *P.VisibleSubObjectBaseName, i), true);
		}
		++Transport->SubObjectVersion;
	}
}

// ---- the aircraft making a delivery (DeliverPayloadAIUpdate) ------------------------------------

bool FGenObject::GetDeliveryMarker(FVector3f& OutPos, float& OutRadius, const FGenRadiusDecalTemplate*& OutDecal) const
{
	if (bDead || bPendingDestroy || !Delivery || !Delivery->Data || Delivery->State >= 2 || !Delivery->Data->DeliveryDecal.IsValid() || Delivery->Data->DeliveryDecalRadius <= 0.f)
	{
		return false;
	}
	OutPos = Delivery->Target;
	OutRadius = Delivery->Data->DeliveryDecalRadius;
	OutDecal = &Delivery->Data->DeliveryDecal;
	return true;
}

bool FGenObject::UpdateDelivery(uint32 Frame)
{
	FDelivery& D = *Delivery;
	const FGenDeliverPayload& P = *D.Data;
	if (Frame < D.StartFrame)
	{
		Velocity = FVector3f::ZeroVector;
		return false; // DelayDeliveryMax: not yet
	}
	bJetDriving = true; // this module flies the aircraft, not a move order
	const FVector2f Pos(Position.X, Position.Y);
	const FVector2f Dir = GetDirection2D();
	const float Dist = FVector2f::Distance(Pos, FVector2f(D.Target.X, D.Target.Y));
	const bool bInbound = Dist < D.PrevDist;
	D.PrevDist = Dist;
	// isCloseEnoughToTarget: on the way in, the doors may open PreOpenDistance early.
	const bool bCloseEnough = Dist < P.DeliveryDistance + (bInbound ? P.PreOpenDistance : 0.f);
	const FVector3f Ahead(Position.X + Dir.X * 1000.f, Position.Y + Dir.Y * 1000.f, Position.Z);

	// The A10 dives at its target, guns blazing, and pulls up again.
	if (P.DiveStartDistance > 0.f && D.DiveState < 2)
	{
		if (D.DiveState == 0 && Dist <= P.DiveStartDistance)
		{
			D.DiveState = 1;
			bLocoPreciseZ = true;
		}
		if (D.DiveState == 1)
		{
			PreciseGoalZ = D.Target.Z;
			const float Dist3D = (Position - D.Target).Size();
			if (Dist3D <= P.DiveEndDistance)
			{
				D.DiveState = 2;
				bLocoPreciseZ = false;
			}
			else if (!P.StrafingWeaponSlot.IsEmpty() && Weapons[0].Template && Frame >= Weapons[0].ReadyFrame && Velocity.Z < 5.f)
			{
				// Bullets walk along the ground towards (and past) the target.
				const float Ratio = (P.DiveStartDistance - Dist3D) / FMath::Max(P.DiveStartDistance - P.DiveEndDistance, 1.f);
				const FVector2f Along = FVector2f(Velocity.X, Velocity.Y).GetSafeNormal() * (Ratio * 100.f);
				FVector3f Strafe(D.Target.X + Along.X * 0.67f, D.Target.Y + Along.Y * 0.67f, 0.f);
				Strafe.Z = Logic->GetGroundHeight(Strafe.X, Strafe.Y);
				const FGenWeaponTemplate& W = *Weapons[0].Template;
				// ("fireCurrentWeapon": its sound and its flash, then where the bullets land.)
				Logic->DoWeaponFireSound(*this, 0, W);
				Logic->DoWeaponFireFX(*this, 0, W, Strafe);
				Logic->DoFXPos(P.StrafeWeaponFX, Strafe, nullptr, 0.f, nullptr, 0.f);
				Logic->DealDamage(W, ID, GenInvalidObjectID, Strafe, true);
				++Weapons[0].FireCount;
				Weapons[0].ReadyFrame = Frame + uint32(FMath::Max(1, W.DelayBetweenShotsMin));
			}
		}
	}

	switch (D.State)
	{
	case 0: // ApproachState
		LocoMoveTowardsPosition(D.MoveTo, 99999.f, Locomotor->Speed, Frame);
		if (bCloseEnough)
		{
			D.State = 1;
			D.NextDropFrame = Frame + uint32(Template->DeliverDoorDelay);
		}
		break;

	case 1: // DeliveringState: something goes out every DropDelay frames
		LocoMoveTowardsPosition(bInbound ? D.MoveTo : Ahead, 99999.f, Locomotor->Speed, Frame);
		if (Frame < D.NextDropFrame)
		{
			break;
		}
		D.NextDropFrame = Frame + uint32(FMath::Max(1, P.DropDelay));
		if (!bCloseEnough)
		{
			// Flew past with things still on board: come round again, or give up.
			D.State = (++D.Attempts < P.MaxAttempts) ? 3 : 2;
			break;
		}
		if (D.ItemsLeft > 0)
		{
			DropPayloadItem(Frame);
		}
		for (int32 n = 0; n < P.VisibleItemsDroppedPerInterval && D.VisibleLeft > 0; ++n)
		{
			FireVisiblePayload(Frame);
		}
		if (D.ItemsLeft <= 0 && D.VisibleLeft <= 0)
		{
			D.State = 2;
		}
		break;

	case 3: // ConsiderNewApproachState: straight on until there is room to turn back
	{
		LocoMoveTowardsPosition(Ahead, 99999.f, Locomotor->Speed, Frame);
		const float TurnRadius = Locomotor->Speed / FMath::Max(Locomotor->TurnRate, 1e-4f);
		if (Dist > P.DeliveryDistance + P.PreOpenDistance + TurnRadius * 2.2f)
		{
			D.State = 0;
		}
		break;
	}

	default: // HeadOffMapState
	{
		if (P.bSelfDestructObject)
		{
			bPendingDestroy = true; // it was only ever a launcher
			return false;
		}
		LocoMoveTowardsPosition(Ahead, 99999.f, Locomotor->Speed, Frame);
		FVector2f Lo, Hi;
		Logic->GetMapExtent(Lo, Hi, true);
		if (Pos.X < Lo.X || Pos.Y < Lo.Y || Pos.X > Hi.X || Pos.Y > Hi.Y)
		{
			bPendingDestroy = true; // CleanUpState: gone over the horizon
			return false;
		}
		break;
	}
	}
	return true;
}

void FGenObject::DropPayloadItem(uint32 Frame)
{
	FDelivery& D = *Delivery;
	const FGenDeliverPayload& P = *D.Data;
	// Which item is next: the payload list in order.
	int32 Total = 0;
	for (const TPair<FString, int32>& Item : P.Payload)
	{
		Total += Item.Value;
	}
	int32 Index = Total - D.ItemsLeft;
	--D.ItemsLeft;
	const FGenThingTemplate* ItemT = nullptr;
	for (const TPair<FString, int32>& Item : P.Payload)
	{
		if (Index < Item.Value)
		{
			ItemT = FGenGameData::Get().FindTemplate(Item.Key);
			break;
		}
		Index -= Item.Value;
	}
	if (!ItemT)
	{
		return;
	}
	FRandomStream& Rand = Logic->GetRandom();
	FVector3f Pos = Position + P.DropOffset;
	if (P.DropVariance.X > 0.f) { Pos.X += Rand.FRandRange(-P.DropVariance.X, P.DropVariance.X); }
	if (P.DropVariance.Y > 0.f) { Pos.Y += Rand.FRandRange(-P.DropVariance.Y, P.DropVariance.Y); }
	if (P.DropVariance.Z > 0.f) { Pos.Z += Rand.FRandRange(-P.DropVariance.Z, P.DropVariance.Z); }

	FGenObject* Item = Logic->CreateObject(ItemT, Owner, Pos, Orientation);
	if (!Item)
	{
		return;
	}
	Item->Producer = ID;
	const FGenThingTemplate* ChuteT = P.PutInContainer.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(P.PutInContainer);
	FGenObject* Chute = (ChuteT && ChuteT->bParachute) ? Logic->CreateObject(ChuteT, Owner, Pos, Orientation) : nullptr;
	if (Chute)
	{
		// ParachuteContain: the item hangs under a parachute, which opens after a short free fall and
		// then steers for the target.
		Chute->Producer = ID;
		// (TransportContain's KeepContainerVelocityOnExit: it leaves the aircraft at the aircraft's speed.)
		// "ParachuteDirectly ... bunch up on the target": not the one spot for all of them, though (not the
		// original: the crates of a supply drop came down in a heap) - each its own place on a small spiral
		// round it.
		FVector3f LandAt = D.Target;
		if (P.bParachuteDirectly)
		{
			const int32 Dropped = Total - D.ItemsLeft - 1;
			const float Angle = float(Dropped) * 2.39996f; // (the golden angle: evenly spread, whatever the count)
			const float Radius = Dropped == 0 ? 0.f : 14.f * FMath::Sqrt(float(Dropped));
			LandAt.X += Radius * FMath::Cos(Angle);
			LandAt.Y += Radius * FMath::Sin(Angle);
		}
		Chute->CarryUnderParachute(*Item, P.bParachuteDirectly ? &LandAt : nullptr,
			(P.bInheritTransportVelocity || Template->Contain.bKeepVelocityOnExit) ? Velocity : FVector3f::ZeroVector);
	}
	else
	{
		// TransportContain KeepContainerVelocityOnExit: a bomb leaves at the aircraft's speed and falls.
		Item->Velocity = Velocity;
		Item->bFalling = true;
		Item->bFreePhysics = true;
		Item->Goal = D.Target;
	}
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] #%u %s drops %s (#%u) at (%.0f, %.0f) alt %.0f"), Frame, ID, *Template->Name, *ItemT->Name,
		Item->ID, Pos.X, Pos.Y, Pos.Z - Logic->GetGroundHeight(Pos.X, Pos.Y));
}

void FGenObject::CarryUnderParachute(FGenObject& Rider, const FVector3f* LandAt, const FVector3f& StartVelocity)
{
	ParachuteRider = Rider.ID;
	bFalling = true;
	bChuteLandsWhereItOpens = LandAt == nullptr;
	Goal = LandAt ? *LandAt : FVector3f(Position.X, Position.Y, 0.f);
	Velocity = StartVelocity;
	// (Dropped too low to open in time: pretend it started higher, so it opens at once.)
	ChuteStartZ = FMath::Max(Position.Z, Logic->GetGroundHeight(Position.X, Position.Y) + 2.f * Template->ParachuteOpenDist);
	Rider.bHeldByParachute = true;
	Rider.SetModelCondition(FreeFallBit(), true);
	SetModelCondition(FreeFallBit(), true);
	// ParachuteContain::updateBonePositions / positionContainedObjectsRelativeToContainer: the rider's PARA_MAN
	// bone (or the top of it, without one) hangs at the canopy's PARA_ATTCH - below the canopy, not inside it.
	// And onDrawableBoundToObject: "setDrawableHidden(!m_opened)" - no canopy while it falls free.
	ChuteRiderDrop = Rider.Template->GetMaxHeightAbovePosition();
	if (UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get())
	{
		TArray<FTransform3f> Bone;
		const FGenDrawModule* RiderDraw = Rider.Template->PrimaryDraw();
		const FGenDrawModule* ChuteDraw = Template->PrimaryDraw();
		float Man = ChuteRiderDrop, Attach = 0.f;
		if (RiderDraw && Assets->GetPristineBones(RiderDraw->DefaultModel(), TEXT("PARA_MAN"), 0, 1, Bone) > 0)
		{
			Man = Bone[0].GetTranslation().Z;
		}
		if (ChuteDraw && Assets->GetPristineBones(ChuteDraw->DefaultModel(), TEXT("PARA_ATTCH"), 0, 1, Bone) > 0)
		{
			Attach = Bone[0].GetTranslation().Z;
		}
		ChuteRiderDrop = FMath::Max(0.f, Man - Attach);
	}
	// (The canopy goes above where the rider is let go, not the rider below it.)
	SetPosition(FVector3f(Position.X, Position.Y, Position.Z + ChuteRiderDrop));
	ChuteStartZ += ChuteRiderDrop;
	SnapshotPrevious();
	bHidden = true;
}

void FGenObject::FireVisiblePayload(uint32 Frame)
{
	// The "visible payload": missiles under the wings (or one shell in a barrage gun), fired as projectiles.
	FDelivery& D = *Delivery;
	const FGenDeliverPayload& P = *D.Data;
	const int32 Index = P.VisibleNumBones - D.VisibleLeft; // 0-based
	--D.VisibleLeft;
	if (!P.VisibleSubObjectBaseName.IsEmpty())
	{
		SubObjectOverrides.Emplace(FString::Printf(TEXT("%s%02d"), *P.VisibleSubObjectBaseName, Index + 1), false);
		++SubObjectVersion;
	}
	const FGenGameData& GD = FGenGameData::Get();
	const FGenWeaponTemplate* W = GD.FindWeapon(P.VisiblePayloadWeaponTemplate);
	const FGenThingTemplate* PayloadT = GD.FindTemplate(P.VisiblePayloadTemplateName);
	if (!W || !PayloadT)
	{
		return;
	}
	FVector3f LaunchPos = Position;
	TArray<FTransform3f> Bones;
	const FGenDrawModule* Draw = Template->PrimaryDraw();
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	if (Assets && Draw && !P.VisibleDropBoneBaseName.IsEmpty()
		&& Assets->GetPristineBones(Draw->DefaultModel(), P.VisibleDropBoneBaseName, Index + 1, 1, Bones) > 0)
	{
		LaunchPos = GetTransform().TransformPositionNoScale(Bones[0].GetTranslation());
	}
	if (FGenObject* Shot = Logic->CreateObject(PayloadT, Owner, LaunchPos, Orientation))
	{
		Shot->Producer = ID;
		Shot->LaunchProjectile(W, ID, GenInvalidObjectID, D.Target);
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] #%u %s fires %s (#%u) at (%.0f, %.0f)"), Frame, ID, *Template->Name, *W->Name,
			Shot->ID, D.Target.X, D.Target.Y);
	}
}

// ---- what falls out, and what cleans itself up ---------------------------------------------------

void FGenObject::UpdateAreaWeapons(uint32 Frame)
{
	// FireWeaponUpdate: whenever the weapon is ready it goes off where the object is (a toxin puddle, a fire).
	for (int32 i = 0; i < AreaWeapons.Num(); ++i)
	{
		const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(Template->FireWeaponUpdates[i].Key);
		if (!W || Frame < AreaWeapons[i].ReadyFrame)
		{
			continue;
		}
		Logic->DoFXPos(W->FireFX, Position, nullptr, 0.f, nullptr, W->PrimaryDamageRadius);
		if (!W->FireOCL.IsEmpty()) { Logic->DoOCL(W->FireOCL, *this); }
		Logic->DealDamage(*W, ID, GenInvalidObjectID, Position, true);
		uint32 Delay = uint32(FMath::Max(1, W->DelayBetweenShotsMin));
		if (W->ClipSize > 0 && --AreaWeapons[i].Ammo <= 0)
		{
			if (W->ReloadType != EGenReloadType::Auto)
			{
				// "AutoReloadsClip = No": its clip fired, it is done (the thumps of a tunnel digging its way up).
				AreaWeapons[i].ReadyFrame = MAX_uint32;
				continue;
			}
			AreaWeapons[i].Ammo = W->ClipSize;
			Delay = FMath::Max(Delay, uint32(W->ClipReloadTime));
		}
		AreaWeapons[i].ReadyFrame = Frame + Delay;
	}
}

void FGenObject::InitPayload()
{
	const uint32 Frame = Logic->GetFrame();
	AreaWeapons.SetNum(Template->FireWeaponUpdates.Num());
	for (int32 i = 0; i < AreaWeapons.Num(); ++i)
	{
		const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(Template->FireWeaponUpdates[i].Key);
		AreaWeapons[i].ReadyFrame = Frame + uint32(Template->FireWeaponUpdates[i].Value);
		AreaWeapons[i].Ammo = W ? W->ClipSize : 0;
	}
	if (Template->bHasDeletion)
	{
		// DeletionUpdate: silently gone after its lifetime (at least one frame, so it gets to act first).
		const int32 Life = Logic->GetRandom().RandRange(Template->DeletionMin, FMath::Max(Template->DeletionMin, Template->DeletionMax));
		DeletionFrame = Frame + uint32(FMath::Max(Life, 1)) + 1;
	}
	HeightDieArmedFrame = Frame + uint32(Template->HeightDieInitialDelay);
	EMPFrame = Frame + uint32(Template->EMPStartDelay);
}

void FGenObject::DoEMP(uint32 Frame)
{
	// EMPUpdate::doDisableAttack: vehicles and base buildings in range go dead for a while; aircraft in the air drop.
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		FGenObject* O = Logic->FindObject(Id);
		if (!O || O == this || O->bDead || O->bPendingDestroy || O->Projectile.bActive)
		{
			continue;
		}
		const FGenThingTemplate* T = O->Template;
		const bool bStructure = T->IsKindOf(TEXT("STRUCTURE"));
		if ((O->Position - Position).SizeSquared() > FMath::Square(Template->EMPRadius + T->GetBoundingCircleRadius()))
		{
			continue;
		}
		if (Template->bEMPSparesOwnBuildings && bStructure && O->Owner == Owner)
		{
			continue;
		}
		if (!T->IsKindOf(TEXT("VEHICLE")) && !bStructure && !T->IsKindOf(TEXT("SPAWNS_ARE_THE_WEAPONS")))
		{
			continue; // people carry on
		}
		if (T->IsKindOf(TEXT("AIRCRAFT")) && O->IsAirborneTarget())
		{
			if (!T->IsKindOf(TEXT("EMP_HARDENED")))
			{
				O->Kill();
			}
			continue;
		}
		bool bFactionStructure = false;
		for (const FString& Kind : T->KindOf)
		{
			bFactionStructure |= Kind.StartsWith(TEXT("FS_"));
		}
		if (bStructure && !bFactionStructure)
		{
			continue; // only base buildings
		}
		O->EMPUntil(Frame + uint32(Template->EMPDisabledDuration));
		UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [power f%u] EMP disables #%u %s until f%u"), Frame, O->ID, *T->Name, O->DisabledUntilFrame);
	}
}

bool FGenObject::UpdatePayload(uint32 Frame)
{
	if (Template->EMPDisabledDuration > 0 && !bEMPDone && Frame >= EMPFrame)
	{
		bEMPDone = true;
		DoEMP(Frame);
	}
	if (DeletionFrame != 0 && Frame >= DeletionFrame)
	{
		bPendingDestroy = true;
		return false;
	}

	if (ParachuteRider != GenInvalidObjectID)
	{
		// ParachuteContain::update: free fall until it has dropped ParachuteOpenDist; then the canopy opens,
		// it sinks at its locomotor's vertical speed limit and steers for the landing spot.
		FGenObject* Rider = Logic->FindObject(ParachuteRider);
		if (!bChuteOpen)
		{
			Velocity.Z += FGenGameData::Get().Gravity;
			if (FMath::Abs(ChuteStartZ - Position.Z) >= Template->ParachuteOpenDist)
			{
				bChuteOpen = true;
				bHidden = false; // (the canopy shows from now on)
				if (bChuteLandsWhereItOpens)
				{
					Goal = FVector3f(Position.X, Position.Y, 0.f); // (it makes for the ground below, braking off what speed it has)
				}
				SetModelCondition(FreeFallBit(), false);
				SetModelCondition(ParachutingBit(), true);
				if (Rider)
				{
					Rider->SetModelCondition(FreeFallBit(), false);
					Rider->SetModelCondition(ParachutingBit(), true);
					Logic->PlayEventForObject(Template->ParachuteOpenSound, *Rider);
				}
			}
		}
		else
		{
			float MaxSpeed = Locomotor ? Locomotor->Speed : 2.f;
			const float Steer = Locomotor ? FMath::Max(Locomotor->Acceleration, 0.01f) : 0.07f;
			const float Sink = (Locomotor && Locomotor->SpeedLimitZ < 100.f) ? Locomotor->SpeedLimitZ : 0.5f;
			const float Brake = Locomotor ? FMath::Max(Locomotor->Lift, 0.02f) : 0.1f;
			// (It left the aircraft at the aircraft's speed: like any locomotor it brakes in time to stop
			// over its landing spot instead of sailing past it.)
			const FVector2f ToGoal(Goal.X - Position.X, Goal.Y - Position.Y);
			const float GoalDist = ToGoal.Size();
			if (!bChuteLandsWhereItOpens)
			{
				// (Not the original: a crate let go far short of the drop zone sank to the ground before it got
				// there. It glides as fast as it has to, to touch down on its spot - up to three times its pace.)
				const float Height = FMath::Max(Position.Z - ChuteRiderDrop - Logic->GetGroundHeight(Position.X, Position.Y), 0.f);
				const float FramesLeft = FMath::Max(Height / FMath::Max(Sink, 0.01f), 1.f);
				MaxSpeed = FMath::Clamp(GoalDist / FramesLeft * 1.15f, MaxSpeed, 3.f * MaxSpeed);
			}
			const float BrakeRate = Locomotor ? FMath::Max(Locomotor->Braking, Steer) : Steer;
			const FVector2f Want = GoalDist > 0.01f ? ToGoal / GoalDist * FMath::Min(MaxSpeed, FMath::Sqrt(2.f * BrakeRate * GoalDist)) : FVector2f::ZeroVector;
			FVector2f Vel(Velocity.X, Velocity.Y);
			Vel += (Want - Vel).GetClampedToMaxSize(Want.SizeSquared() < Vel.SizeSquared() ? BrakeRate : Steer);
			Velocity.X = Vel.X;
			Velocity.Y = Vel.Y;
			Velocity.Z += FMath::Clamp(-Sink - Velocity.Z, -Brake, Brake);
		}
		Position += Velocity;
		const float Ground = Logic->GetGroundHeight(Position.X, Position.Y);
		const bool bLanded = Position.Z - ChuteRiderDrop <= Ground; // (the rider's feet on the ground)
		if (bLanded)
		{
			Position.Z = Ground + ChuteRiderDrop;
		}
		if (Rider && !Rider->bDead)
		{
			Rider->Position = FVector3f(Position.X, Position.Y, Position.Z - ChuteRiderDrop);
			Rider->Velocity = FVector3f::ZeroVector;
			if (bLanded)
			{
				// ParachuteContain::onRemoving
				Rider->bHeldByParachute = false;
				Rider->SetModelCondition(FreeFallBit(), false);
				Rider->SetModelCondition(ParachutingBit(), false);
				Rider->SnapshotPrevious();
				if (Logic->GetAI(Rider->Owner) && Rider->HasWeapons())
				{
					Rider->CommandHunt(); // a computer player's paratroopers go looking for trouble
				}
			}
		}
		if (bLanded || !Rider || Rider->bDead)
		{
			ParachuteRider = GenInvalidObjectID;
			bFalling = false;
			Kill(); // the canopy collapses
		}
		return false;
	}

	if (Template->bHeightDie && Frame >= HeightDieArmedFrame && !bHeldByParachute)
	{
		// HeightDieUpdate: goes off this far above the ground.
		const float Height = Position.Z - Logic->GetGroundHeight(Position.X, Position.Y);
		if (Height <= Template->HeightDieTarget && (!Template->bHeightDieOnlyWhenMovingDown || Velocity.Z < 0.f))
		{
			Kill();
			return false;
		}
	}
	if (bFalling)
	{
		if (Template->SmartBombCourseCorrection > 0.f && Position.Z > Logic->GetGroundHeight(Position.X, Position.Y) + 9.f)
		{
			// SmartBombTargetHomingUpdate: pulled a little towards its target every frame.
			const float Keep = FMath::Clamp(Template->SmartBombCourseCorrection, 0.f, 1.f);
			Position.X = Goal.X * (1.f - Keep) + Position.X * Keep;
			Position.Y = Goal.Y * (1.f - Keep) + Position.Y * Keep;
		}
		// The nose follows the fall.
		if (Velocity.SizeSquared() > 0.01f)
		{
			SetRotation(FQuat4f::FindBetweenNormals(FVector3f::ForwardVector, Velocity.GetSafeNormal()));
			bFreeRotation = true;
		}
		if (Position.Z <= Logic->GetGroundHeight(Position.X, Position.Y) + 0.5f && Frame > HeightDieArmedFrame + 2)
		{
			Kill(); // reached the ground without going off: it goes off now
			return false;
		}
	}
	return true;
}
