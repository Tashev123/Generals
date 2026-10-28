#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioManager.h"
#include "Logic/GenGameData.h"
#include "Logic/GenUpgrades.h"
#include "Generals1.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---- orders in ----------------------------------------------------------------------------------

void UGenGameLogic::SubmitCommand(FGenCommand&& Command)
{
	if (Command.Player == INDEX_NONE)
	{
		Command.Player = LocalPlayer;
	}
	if (Playback)
	{
		return; // (a replay gives its own orders; whoever watches gives none)
	}
	if (OrderSource)
	{
		OrderSource->SendOrder(Command); // (it comes back with the frame the host gives it)
		return;
	}
	PendingCommands.Add(MoveTemp(Command));
}

void UGenGameLogic::QueueSimCommand(FGenCommand&& Command)
{
	SimCommands.Add(MoveTemp(Command));
}

void UGenGameLogic::IssueCheat(const FString& Step)
{
	FGenCommand C(EGenCommandType::Cheat);
	C.Name = Step;
	SubmitCommand(MoveTemp(C));
}

bool UGenGameLogic::CanPlayerCommand(int32 Player, const FGenObject& Obj) const
{
	const int32 Owner = Obj.GetOwner();
	if (Owner == Player)
	{
		return true;
	}
	// A human who has dropped out of a network game: its human allies command its army until it is back.
	return Players.IsValidIndex(Owner) && Players.IsValidIndex(Player) && Players[Owner].bDisconnected && Players[Owner].bHuman
		&& Players[Player].bHuman && !Players[Player].bDisconnected && GetRelationship(Player, Owner) == EGenRelationship::Allies;
}

void UGenGameLogic::ExecSession(const FGenCommand& C)
{
	if (C.Name == TEXT("dump"))
	{
		// (The host found two games apart: every game writes its state here, at the same frame.)
		DumpState(FPaths::ProjectLogDir() / FString::Printf(TEXT("StateDump_net_p%d_f%u.txt"), LocalPlayer, Frame));
		return;
	}
	if (!Players.IsValidIndex(C.A))
	{
		return;
	}
	FGenPlayer& P = Players[C.A];
	if (C.Name == TEXT("drop"))
	{
		P.bDisconnected = true;
		UE_LOG(LogGenerals, Log, TEXT("Session f%u: player %d (%s) has dropped out - %s"), Frame, C.A, *P.Name,
			P.Team >= 0 ? TEXT("its allies may command its army") : TEXT("its army waits"));
		AddScreenMessage(FString::Printf(TEXT("%s has dropped out"), *P.Name), FLinearColor(1.f, 0.6f, 0.3f));
	}
	else if (C.Name == TEXT("return"))
	{
		P.bDisconnected = false;
		UE_LOG(LogGenerals, Log, TEXT("Session f%u: player %d (%s) is back"), Frame, C.A, *P.Name);
		AddScreenMessage(FString::Printf(TEXT("%s is back"), *P.Name), FLinearColor(0.6f, 1.f, 0.6f));
	}
}

void UGenGameLogic::CatchUp()
{
	// The steps run here, one after another, for as long as this frame can spare; the screen jumps along.
	TGuardValue<bool> Guard(bCatchingUpNow, true);
	if (CompletedFrame > PublishedFrame)
	{
		PublishFrame();
	}
	const double Until = FPlatformTime::Seconds() + 0.04;
	int32 Steps = 0;
	if (Audio)
	{
		Audio->SetSuspended(true); // (minutes of the match in a second: its sounds aren't played)
	}
	while (OrderSource && OrderSource->IsFrameReady(Frame) && Frame == CompletedFrame && FPlatformTime::Seconds() < Until)
	{
		RunStep();
		++Steps;
	}
	if (Audio)
	{
		Audio->SetSuspended(false);
	}
	if (Steps > 0)
	{
		// (Twice: nothing is blended across the frames that were skipped.)
		PublishFrame();
		PublishFrame();
		SimClock = double(PublishedFrame) + RenderDelay;
	}
	if (Frame % uint32(GenLogic::GetLogicFPS() * 30) < uint32(Steps))
	{
		UE_LOG(LogGenerals, Log, TEXT("Catching up: frame %u of %u"), Frame, OrderSource ? OrderSource->GetReadyFrames() : 0u);
	}
}

void UGenGameLogic::ApplyCommands()
{
	bSetupPhase = false; // (what is made from here on, the steps make - in a replay too)
	// Orders carry the frame they were applied at: the step that makes frame N + 1 applies those of frame N.
	const uint32 CommandFrame = Frame;
	if (SimCommands.Num() > 0)
	{
		TArray<FGenCommand> Own = MoveTemp(SimCommands);
		SimCommands.Reset();
		for (FGenCommand& C : Own)
		{
			C.Frame = CommandFrame;
			ExecuteCommand(C);
		}
	}
	if (PendingLocal.Num() > 0)
	{
		TArray<TFunction<void()>> Local = MoveTemp(PendingLocal);
		PendingLocal.Reset();
		for (TFunction<void()>& F : Local)
		{
			F();
		}
	}
	TArray<FGenCommand> Commands;
	if (OrderSource)
	{
		OrderSource->TakeFrame(CommandFrame, Commands);
	}
	else if (Playback)
	{
		while (Playback->Commands.IsValidIndex(PlaybackNext) && Playback->Commands[PlaybackNext].Frame <= CommandFrame)
		{
			Commands.Add(Playback->Commands[PlaybackNext++]);
		}
	}
	else
	{
		Commands = MoveTemp(PendingCommands);
		PendingCommands.Reset();
	}
	for (FGenCommand& C : Commands)
	{
		C.Frame = CommandFrame;
		if (Recording)
		{
			Recording->Commands.Add(C);
		}
		ExecuteCommand(C);
		SimYield(TEXT("a player's order"));
	}
}

void UGenGameLogic::ExecuteCommand(const FGenCommand& In)
{
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [order] %s"), *In.Describe());
	// Only what the player may command is told (a selection can hold somebody else's things, looked at).
	FGenCommand C = In;
	C.Units.RemoveAll([this, &C](FGenObjectID Id)
	{
		const FGenObject* O = FindObject(Id);
		// (A player commanding an absent ally's army doesn't sell its buildings.)
		return !O || !CanPlayerCommand(C.Player, *O) || (C.Type == EGenCommandType::Sell && O->GetOwner() != C.Player);
	});
	switch (C.Type)
	{
	case EGenCommandType::Move: ExecMove(C); break;
	case EGenCommandType::AddWaypoint: ExecAddWaypoint(C); break;
	case EGenCommandType::AttackMove: ExecAttackMove(C); break;
	case EGenCommandType::Guard: ExecGuard(C); break;
	case EGenCommandType::Stop: ExecStop(C); break;
	case EGenCommandType::FireWeaponAt: ExecFireWeaponAt(C); break;
	case EGenCommandType::ForceAttack: ExecForceAttack(C); break;
	case EGenCommandType::ForceAttackGround: ExecForceAttackGround(C); break;
	case EGenCommandType::SwitchWeapon: ExecSwitchWeapon(C); break;
	case EGenCommandType::CreateFormation: ExecCreateFormation(C); break;
	case EGenCommandType::Land: ExecLand(C); break;
	case EGenCommandType::Attack: ExecAttack(C); break;
	case EGenCommandType::SteerGunship: ExecSteerGunship(C); break;
	case EGenCommandType::CombatDrop: ExecCombatDrop(C); break;
	case EGenCommandType::Enter: ExecEnter(C); break;
	case EGenCommandType::Exit: ExecExit(C); break;
	case EGenCommandType::Evacuate: ExecEvacuate(C); break;
	case EGenCommandType::Capture: ExecCapture(C); break;
	case EGenCommandType::HackInternet: ExecHackInternet(C); break;
	case EGenCommandType::FireWeapon: ExecFireWeapon(C); break;
	case EGenCommandType::SpecialPower: ExecSpecialPower(C); break;
	case EGenCommandType::Dock: ExecDock(C); break;
	case EGenCommandType::QueueUnit: ExecQueueUnit(C); break;
	case EGenCommandType::CancelUnit: ExecCancelUnit(C); break;
	case EGenCommandType::Construct: ExecConstruct(C); break;
	case EGenCommandType::ResumeConstruction: ExecResumeConstruction(C); break;
	case EGenCommandType::Repair: ExecRepair(C); break;
	case EGenCommandType::GetRepaired: ExecGetRepaired(C); break;
	case EGenCommandType::SetRallyPoint: ExecSetRallyPoint(C); break;
	case EGenCommandType::Sell: ExecSell(C); break;
	case EGenCommandType::CancelConstruction: ExecCancelConstruction(C); break;
	case EGenCommandType::ToggleOvercharge: ExecToggleOvercharge(C); break;
	case EGenCommandType::BattlePlan: ExecBattlePlan(C); break;
	case EGenCommandType::QueueUpgrade: ExecQueueUpgrade(C); break;
	case EGenCommandType::PurchaseScience: PurchaseScience(C.Player, C.Name); break;
	case EGenCommandType::Cheat: ExecCheat(C); break;
	case EGenCommandType::Session: ExecSession(C); break;
	default: break;
	}
}

// ---- test switches that change the game by hand -------------------------------------------------

void UGenGameLogic::ExecCheat(const FGenCommand& C)
{
	// (gen_test.ps1's -TestPowers steps that reach into the simulation; the rest of them are ordinary orders.)
	TArray<FString> Parts;
	C.Name.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() == 0)
	{
		return;
	}
	const FString& What = Parts[0];
	const FString Arg = Parts.Num() >= 2 ? Parts[1] : FString();
	const int32 Local = C.Player;
	auto IsType = [&Arg](const FGenObject& O)
	{
		// (By the name it was asked for, or by its own: several templates may list the same variations.)
		return O.GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase) || O.GetTemplate()->Name.Equals(Arg, ESearchCase::IgnoreCase);
	};
	if (What.Equals(TEXT("grant"), ESearchCase::IgnoreCase))
	{
		const FGenUpgradeTemplate* Upgrade = FGenUpgradeData::Get().FindUpgrade(Arg);
		if (Upgrade)
		{
			GrantPlayerUpgrade(Local, *Upgrade);
		}
		UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: upgrade %s %s"), Frame, *Arg, Upgrade ? TEXT("granted") : TEXT("unknown"));
	}
	else if (What.Equals(TEXT("kill"), ESearchCase::IgnoreCase))
	{
		const int32 Whose = Parts.Num() >= 3 ? FCString::Atoi(*Parts[2]) : Local;
		TArray<FGenObject*> Victims;
		for (FGenObjectID Id : ObjectOrder)
		{
			FGenObject* O = Objects[Id].Get();
			if (O->GetOwner() == Whose && !O->IsDead() && IsType(*O))
			{
				Victims.Add(O);
			}
		}
		for (FGenObject* O : Victims)
		{
			O->Kill(); // (after the search: what a death throws out are new objects)
		}
		UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d %s of player %d killed"), Frame, Victims.Num(), *Arg, Whose);
	}
	else if (What.Equals(TEXT("skill"), ESearchCase::IgnoreCase) && Players.IsValidIndex(Local))
	{
		AddSkillPoints(Local, FCString::Atoi(*Arg));
		const FGenPlayer& P = Players[Local];
		UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: +%s skill points -> rank %d, %d skill points, %d general's points"), Frame, *Arg,
			P.RankLevel, P.SkillPoints, P.SciencePurchasePoints);
	}
	else if (What.Equals(TEXT("aiskill"), ESearchCase::IgnoreCase))
	{
		// The same for every computer player (they then spend the points on their skill set).
		for (const FGenPlayer& P : Players)
		{
			if (GetAI(P.Index))
			{
				AddSkillPoints(P.Index, FCString::Atoi(*Arg));
			}
		}
	}
	else if (What.Equals(TEXT("hunt"), ESearchCase::IgnoreCase))
	{
		// Every unit of ours hunts with that button, as the computer player's teams do (CommandButtonHuntUpdate).
		int32 Num = 0;
		for (FGenObjectID Id : ObjectOrder)
		{
			FGenObject* O = Objects[Id].Get();
			if (O->GetOwner() == Local && !O->IsDead() && O->CanMove())
			{
				O->SetHuntButton(Arg);
				++Num;
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d units told to hunt with %s"), Frame, Num, *Arg);
	}
	else if (What.Equals(TEXT("damage"), ESearchCase::IgnoreCase) && Parts.Num() >= 3)
	{
		// Our first living object of that type keeps that percentage of its health. ("damage,Type,Percent,Player": that player's.)
		const int32 Whose = Parts.Num() >= 4 ? FCString::Atoi(*Parts[3]) : Local;
		for (FGenObjectID Id : ObjectOrder)
		{
			FGenObject* O = Objects[Id].Get();
			if (O->GetOwner() == Whose && !O->IsDead() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
			{
				const float Keep = O->GetMaxHealth() * FCString::Atof(*Parts[2]) / 100.f;
				O->AttemptDamage(FMath::Max(0.f, O->GetHealth() - Keep), TEXT("UNRESISTABLE"), TEXT("NORMAL"), GenInvalidObjectID);
				UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: #%u %s damaged to %.0f / %.0f"), Frame, Id, *Arg, O->GetHealth(), O->GetMaxHealth());
				break;
			}
		}
	}
	else if (What.Equals(TEXT("vet"), ESearchCase::IgnoreCase) && Parts.Num() >= 3)
	{
		// ("vet,Type,Level,Player": that player's instead of ours.)
		const int32 Whose = Parts.Num() >= 4 ? FCString::Atoi(*Parts[3]) : Local;
		int32 Count = 0;
		for (FGenObjectID Id : ObjectOrder)
		{
			FGenObject* O = Objects[Id].Get();
			if (O->GetOwner() == Whose && !O->IsDead() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase))
			{
				O->SetVeterancyLevel(FCString::Atoi(*Parts[2]));
				++Count;
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: %d %s set to veterancy level %s"), Frame, Count, *Arg, *Parts[2]);
	}
	else if (What.Equals(TEXT("destroy"), ESearchCase::IgnoreCase) && Parts.Num() >= 4)
	{
		// Anybody's object of that type nearest the spot.
		const FVector2f Near(FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
		FGenObject* Victim = nullptr;
		float BestDistSq = FLT_MAX;
		for (FGenObjectID Id : ObjectOrder)
		{
			FGenObject* O = Objects[Id].Get();
			const float DistSq = FVector2f::DistSquared(FVector2f(O->GetPosition().X, O->GetPosition().Y), Near);
			if (!O->IsDead() && O->GetTemplate()->Canonical()->Name.Equals(Arg, ESearchCase::IgnoreCase) && DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Victim = O;
			}
		}
		UE_LOG(LogGenerals, Log, TEXT("Test powers f%u: destroy %s: %s"), Frame, *Arg, Victim ? TEXT("done") : TEXT("none found"));
		if (Victim)
		{
			Victim->Kill();
		}
	}
	else if (What.Equals(TEXT("money"), ESearchCase::IgnoreCase))
	{
		AddMoney(Local, FCString::Atoi(*Arg));
	}
	else
	{
		UE_LOG(LogGenerals, Warning, TEXT("Test powers f%u: unknown cheat '%s'"), Frame, *C.Name);
	}
}

// ---- checks ---------------------------------------------------------------------------------------

namespace
{
	FORCEINLINE uint32 FloatBits(float F)
	{
		uint32 Bits;
		FMemory::Memcpy(&Bits, &F, sizeof(Bits));
		return Bits;
	}
}

uint32 UGenGameLogic::ComputeChecksum() const
{
	// FNV-1a over 32-bit words: a few words an object, a fraction of a millisecond for a big battle.
	uint32 H = 2166136261u;
	auto Mix = [&H](uint32 V) { H = (H ^ V) * 16777619u; };
	auto MixF = [&Mix](float F) { Mix(FloatBits(F)); };
	Mix(Frame);
	Mix(NextID);
	Mix(uint32(Random.GetCurrentSeed()));
	for (const FGenPlayer& P : Players)
	{
		Mix(uint32(P.Money));
		Mix(uint32(P.SkillPoints));
		Mix(uint32(P.SciencePurchasePoints));
		Mix(uint32(P.RankLevel));
		Mix((P.bDefeated ? 1u : 0u) | (P.bDisconnected ? 2u : 0u));
		Mix(uint32(P.Sciences.Num()));
		Mix(uint32(P.UpgradesComplete.Num()));
	}
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject& O = *Objects[Id];
		Mix(Id);
		Mix(uint32(O.Owner));
		MixF(O.Position.X);
		MixF(O.Position.Y);
		MixF(O.Position.Z);
		MixF(O.Orientation);
		MixF(O.Health);
		Mix(O.TargetID);
		Mix((O.bDead ? 1u : 0u) | (O.bMoving ? 2u : 0u));
	}
	return H;
}

bool UGenGameLogic::DumpState(const FString& Path) const
{
	// Exact values in hex beside readable ones: two dumps that differ anywhere differ in their text.
	FString Out;
	Out.Reserve(ObjectOrder.Num() * 300);
	Out.Appendf(TEXT("frame %u next id %u random %08x objects %d checksum %08x\n"), Frame, NextID, uint32(Random.GetCurrentSeed()), ObjectOrder.Num(), ComputeChecksum());
	for (const FGenPlayer& P : Players)
	{
		Out.Appendf(TEXT("player %d money %d skill %d points %d rank %d defeated %d sciences %d upgrades %d in production %d\n"), P.Index, P.Money, P.SkillPoints,
			P.SciencePurchasePoints, P.RankLevel, P.bDefeated ? 1 : 0, P.Sciences.Num(), P.UpgradesComplete.Num(), P.UpgradesInProduction.Num());
	}
	Out.Appendf(TEXT("path queue %d (head %d)\n"), PathQueue.Num(), PathQueueHead);
	for (FGenObjectID Id : ObjectOrder)
	{
		const FGenObject& O = *Objects[Id];
		Out.Appendf(TEXT("#%u %s p%d pos %.3f %.3f %.3f [%08x %08x %08x] ang %.4f [%08x] hp %.2f [%08x] vel [%08x %08x %08x] tgt %u ai %d move %d path %d/%d goal [%08x %08x] blocked %u stuck %u w0 %u/%d w1 %u/%d vet %d xp %d%s%s\n"),
			Id, *O.Template->Name, O.Owner, O.Position.X, O.Position.Y, O.Position.Z, FloatBits(O.Position.X), FloatBits(O.Position.Y), FloatBits(O.Position.Z),
			O.Orientation, FloatBits(O.Orientation), O.Health, FloatBits(O.Health), FloatBits(O.Velocity.X), FloatBits(O.Velocity.Y), FloatBits(O.Velocity.Z),
			O.TargetID, int32(O.AIMode), O.bMoving ? 1 : 0, O.PathIndex, O.Path.Num(), FloatBits(O.Goal.X), FloatBits(O.Goal.Y), O.BlockedFrames, O.StuckFrames,
			O.Weapons[0].ReadyFrame, O.Weapons[0].AmmoInClip, O.Weapons[1].ReadyFrame, O.Weapons[1].AmmoInClip, O.VeterancyLevel, O.Experience,
			O.bDead ? TEXT(" DEAD") : TEXT(""), O.IsWaitingForPath() ? TEXT(" WAITING-FOR-PATH") : TEXT(""));
	}
	const bool bSaved = FFileHelper::SaveStringToFile(Out, *Path);
	UE_LOG(LogGenerals, Log, TEXT("State dump of frame %u: %s (%s)"), Frame, *Path, bSaved ? TEXT("written") : TEXT("NOT WRITTEN"));
	return bSaved;
}

void UGenGameLogic::CheckStep()
{
	if (!Recording && !Playback && !OrderSource)
	{
		return;
	}
	GEN_PERF_SCOPE("Checksum");
	const uint32 Sum = ComputeChecksum();
	if (OrderSource)
	{
		OrderSource->NoteChecksum(Frame, Sum);
	}
	if (Recording)
	{
		Recording->Checksums.Add(Sum);
	}
	if (Playback)
	{
		const int32 Index = int32(Frame) - 1;
		if (PlaybackMismatchFrame == 0 && Playback->Checksums.IsValidIndex(Index) && Playback->Checksums[Index] != Sum)
		{
			PlaybackMismatchFrame = Frame;
			UE_LOG(LogGenerals, Warning, TEXT("Replay: MISMATCH at frame %u (recorded %08x, now %08x)"), Frame, Playback->Checksums[Index], Sum);
			DumpState(FPaths::ProjectLogDir() / FString::Printf(TEXT("StateDump_%s_f%u.txt"), *DumpTag, Frame));
		}
		else if (Index == Playback->Checksums.Num() - 1)
		{
			UE_LOG(LogGenerals, Log, TEXT("Replay: end of the recording at frame %u: %s"), Frame,
				PlaybackMismatchFrame == 0 ? TEXT("every frame the same") : *FString::Printf(TEXT("went apart at frame %u"), PlaybackMismatchFrame));
		}
		else if (Frame % uint32(GenLogic::GetLogicFPS() * 60) == 0 && Index < Playback->Checksums.Num())
		{
			UE_LOG(LogGenerals, Log, TEXT("Replay: frame %u of %d %s"), Frame, Playback->Checksums.Num(), PlaybackMismatchFrame == 0 ? TEXT("the same so far") : TEXT("(apart)"));
		}
	}
	if (DumpFrames.Contains(Frame))
	{
		DumpState(FPaths::ProjectLogDir() / FString::Printf(TEXT("StateDump_%s_f%u.txt"), *DumpTag, Frame));
	}
}

// ---- recording and playback -----------------------------------------------------------------------

bool UGenGameLogic::SaveRecording(const FString& InPath)
{
	if (!Recording)
	{
		return false;
	}
	const FString Path = InPath.IsEmpty() ? FGenReplay::GetDefaultFolder() / TEXT("LastReplay.genrep") : InPath;
	Recording->BuildId = GenBuild::GetBuildId();
	Recording->DataHash = GenBuild::GetDataHash();
	const bool bSaved = Recording->Save(Path);
	UE_LOG(LogGenerals, Log, TEXT("Replay: %d orders, %d frames -> %s (%s)"), Recording->Commands.Num(), Recording->Checksums.Num(), *Path,
		bSaved ? TEXT("saved") : TEXT("NOT SAVED"));
	return bSaved;
}

void UGenGameLogic::SetPlayback(TUniquePtr<FGenReplay> Replay)
{
	Playback = MoveTemp(Replay);
	PlaybackNext = 0;
	PlaybackMismatchFrame = 0;
}

void UGenGameLogic::PlaceSetupObjects(const TArray<FGenSetupObject>& SetupObjects)
{
	for (const FGenSetupObject& S : SetupObjects)
	{
		if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(S.Template))
		{
			CreateObject(T, S.Owner, S.Pos, S.Angle);
		}
		else
		{
			UE_LOG(LogGenerals, Warning, TEXT("Replay: the start's %s isn't in the game data"), *S.Template);
		}
	}
}
