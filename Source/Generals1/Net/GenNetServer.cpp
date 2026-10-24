#include "Net/GenNetServer.h"
#include "Async/Async.h"
#include "Game/GenShellData.h"
#include "Net/GenUPnP.h"
#include "Generals1.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

using GenNet::EMsg;

namespace
{
	/** A player gone this long without a word has dropped out (a broken connection doesn't always say so). */
	constexpr double SilenceLimit = 15.0;
	/** Nobody in the lobby for this long, or nobody in a match for this long: the server closes. */
	constexpr double EmptyLobbyLimit = 30.0 * 60.0;
	constexpr double EmptyMatchLimit = 10.0 * 60.0;
	/** Games that haven't loaded the map after this long are left behind (they can come back). */
	constexpr double LoadingLimit = 120.0;

	uint64 NewToken()
	{
		const FGuid G = FGuid::NewGuid();
		return (uint64(G.A) << 32 | G.B) ^ (uint64(G.C) << 32 | G.D);
	}

	TArray<uint8> WriteText(const FString& Text)
	{
		TArray<uint8> Bytes;
		FMemoryWriter W(Bytes);
		FString T = Text;
		W << T;
		return Bytes;
	}
}

FGenNetServer::FGenNetServer() = default;

FGenNetServer::~FGenNetServer()
{
	Shutdown();
}

bool FGenNetServer::Start(int32 InPort, FString* OutWhy)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	Addr->SetAnyAddress();
	Addr->SetPort(InPort);
	Listener = SS->CreateSocket(NAME_Stream, TEXT("GenNet server"), Addr->GetProtocolType());
	if (!Listener || !Listener->Bind(*Addr) || !Listener->Listen(16))
	{
		if (OutWhy) { *OutWhy = FString::Printf(TEXT("port %d can't be used (is another server running?)"), InPort); }
		if (Listener) { SS->DestroySocket(Listener); Listener = nullptr; }
		return false;
	}
	Listener->SetNonBlocking(true);
	Port = InPort;

	// The lobby begins as the skirmish menu does: the default map, open places for whoever comes.
	FGenShellData::Get().Initialize();
	const FGenShellData& Data = FGenShellData::Get();
	Lobby.Map = Data.FindMap(TEXT("tournament desert")) ? TEXT("tournament desert") : (Data.GetMaps().Num() > 0 ? Data.GetMaps()[0].Name : FString());
	Lobby.StartingCash = Data.GetDefaultMoney();
	Lobby.bTestUnits = false;
	for (FGenSetupSlot& S : Lobby.Slots)
	{
		S = FGenSetupSlot();
		S.State = EGenSlotState::Open;
	}
	Lobby.AdjustSlotsForMap();
	LastActivity = FPlatformTime::Seconds();
	FrameSeconds = 1.0 / double(FMath::Max(1, GenLogic::GetLogicFPS()));
	Thread = FRunnableThread::Create(this, TEXT("GenNetServer"), 0, TPri_AboveNormal);
	UE_LOG(LogGenerals, Log, TEXT("Server: listening on port %d (build %s, data %08x)"), Port, *GenBuild::GetBuildId(), GenBuild::GetDataHash());

	// Friends reach this server through the home router: it is asked to forward the port here, and for the
	// address the internet sees - what they type in to join.
	if (!FParse::Param(FCommandLine::Get(), TEXT("GenNoUPnP")))
	{
		PortTask = Async(EAsyncExecution::Thread, [this]()
		{
			const GenUPnP::FResult R = GenUPnP::OpenPort(Port, TEXT("Generals network game"));
			FString Text;
			if (R.bPortOpen)
			{
				bPortMapped.store(true);
				Text = R.ExternalIP.IsEmpty() ? FString::Printf(TEXT("The router (%s) forwards port %d to this computer"), *R.Router, Port)
					: FString::Printf(TEXT("Friends join at %s:%d (the router forwards the port here)"), *R.ExternalIP, Port);
			}
			else
			{
				Text = FString::Printf(TEXT("The port couldn't be opened on the router (%s): forward TCP port %d to %s by hand to be joined from the internet"),
					*R.Error, Port, R.LocalIP.IsEmpty() ? TEXT("this computer") : *R.LocalIP);
			}
			UE_LOG(LogGenerals, Log, TEXT("Server: %s"), *Text);
			FScopeLock L(&PortLock);
			PortNotice = Text;
		});
	}
	return true;
}

void FGenNetServer::Shutdown()
{
	if (Thread)
	{
		bStopping.store(true);
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (PortTask.IsValid())
	{
		PortTask.Wait();
		PortTask = TFuture<void>();
	}
	if (bPortMapped.exchange(false))
	{
		GenUPnP::ClosePort(Port);
		UE_LOG(LogGenerals, Log, TEXT("Server: the router no longer forwards port %d"), Port);
	}
	Clients.Reset();
	if (Listener)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener);
		Listener = nullptr;
	}
}

uint32 FGenNetServer::Run()
{
	TArray<uint8> Payload;
	while (!bStopping.load())
	{
		Accept();
		for (int32 i = 0; i < Clients.Num(); ++i)
		{
			FClient& C = Clients[i];
			if (!C.Conn->Pump() || C.Conn->GetSilence() > SilenceLimit || (!C.bWelcomed && FPlatformTime::Seconds() - C.Since > 10.0))
			{
				C.bGone = true;
				continue;
			}
			EMsg Type;
			while (!C.bGone && C.Conn->Receive(Type, Payload))
			{
				Handle(C, Type, Payload);
			}
			if (!C.Conn->IsOpen())
			{
				C.bGone = true;
			}
		}
		// The ones who have gone (after the messages of everybody else this round).
		for (int32 i = Clients.Num() - 1; i >= 0; --i)
		{
			if (Clients[i].bGone)
			{
				OnLeave(Clients[i]);
				Clients[i].Conn->Close();
				Clients.RemoveAt(i);
			}
		}
		TickClock();
		// Where friends join, once the router has answered (and to everybody who comes after).
		{
			FScopeLock L(&PortLock);
			if (!PortNotice.IsEmpty() && !bPortNoticeSent && Clients.Num() > 0)
			{
				bPortNoticeSent = true;
				Notice(PortNotice);
			}
		}
		for (FClient& C : Clients)
		{
			C.Conn->Pump();
		}
		// Nobody left for a long while: done.
		if (Clients.Num() > 0)
		{
			LastActivity = FPlatformTime::Seconds();
		}
		else if (FPlatformTime::Seconds() - LastActivity > (Phase == EPhase::Lobby ? EmptyLobbyLimit : EmptyMatchLimit))
		{
			if (!bDone.load())
			{
				UE_LOG(LogGenerals, Log, TEXT("Server: nobody here for %.0f minutes - closing"), (FPlatformTime::Seconds() - LastActivity) / 60.0);
				SaveRecord();
			}
			bDone.store(true);
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return 0;
}

void FGenNetServer::Accept()
{
	bool bPending = false;
	while (Listener && Listener->HasPendingConnection(bPending) && bPending)
	{
		FSocket* Socket = Listener->Accept(TEXT("GenNet player"));
		if (!Socket)
		{
			break;
		}
		FClient& C = Clients.AddDefaulted_GetRef();
		C.Conn = MakeUnique<FGenNetConnection>(Socket);
		C.Id = NextClientId++;
		C.Since = FPlatformTime::Seconds();
		UE_LOG(LogGenerals, Log, TEXT("Server: connection %u from %s"), C.Id, *C.Conn->GetRemote());
	}
}

FGenNetServer::FClient* FGenNetServer::FindClient(uint32 Id)
{
	return Clients.FindByPredicate([Id](const FClient& C) { return C.Id == Id && !C.bGone; });
}

int32 FGenNetServer::SlotOf(uint32 ClientId) const
{
	for (int32 s = 0; s < FGenSetupData::MaxSlots; ++s)
	{
		if (SlotOwner[s] == ClientId) { return s; }
	}
	return INDEX_NONE;
}

void FGenNetServer::Handle(FClient& C, EMsg Type, const TArray<uint8>& Payload)
{
	if (!C.bWelcomed && Type != EMsg::Hello)
	{
		return;
	}
	FMemoryReader R(Payload);
	switch (Type)
	{
	case EMsg::Hello:
		OnHello(C, Payload);
		break;
	case EMsg::LobbyEdit:
		OnLobbyEdit(C, Payload);
		break;
	case EMsg::Name:
	{
		FString Name;
		R << Name;
		C.Name = Name.TrimStartAndEnd().Left(24);
		const int32 Slot = SlotOf(C.Id);
		if (Phase == EPhase::Lobby && Slot != INDEX_NONE)
		{
			Lobby.Slots[Slot].Name = C.Name;
			BroadcastLobby();
		}
		break;
	}
	case EMsg::StartRequest:
		OnStartRequest(C);
		break;
	case EMsg::Loaded:
		C.bLoaded = true;
		UE_LOG(LogGenerals, Log, TEXT("Server: %s (player %d) has loaded the map"), *C.Name, C.Player);
		break;
	case EMsg::Order:
		if (Phase == EPhase::Playing && C.Player != INDEX_NONE)
		{
			FGenCommand Order;
			Order.Serialize(R);
			if (!R.IsError() && Order.Type != EGenCommandType::Session)
			{
				Order.Player = C.Player; // (a game gives orders for its own player only)
				Incoming.Add(MoveTemp(Order));
			}
		}
		break;
	case EMsg::Checksums:
		NoteChecksums(C, Payload);
		break;
	case EMsg::Chat:
	{
		FString Text;
		R << Text;
		Broadcast(EMsg::Chat, WriteText(FString::Printf(TEXT("%s: %s"), *C.Name, *Text.Left(200))), false);
		break;
	}
	case EMsg::Ping:
		C.Conn->Send(EMsg::Pong, Payload);
		break;
	default:
		break;
	}
}

void FGenNetServer::OnHello(FClient& C, const TArray<uint8>& Payload)
{
	FMemoryReader R(Payload);
	uint32 Protocol = 0, DataHash = 0;
	FString BuildId, Name;
	uint64 Token = 0;
	R << Protocol << BuildId << DataHash << Name << Token;
	if (R.IsError() || Protocol != GenNet::ProtocolVersion)
	{
		Refuse(C, FString::Printf(TEXT("a different version of the game (network version %u, the host's %u)"), Protocol, GenNet::ProtocolVersion));
		return;
	}
	// Everybody plays the same program with the same data, or the games would come to different results.
	if (BuildId != GenBuild::GetBuildId())
	{
		Refuse(C, FString::Printf(TEXT("a different build of the game (yours %s, the host's %s) - everybody needs the same one"), *BuildId, *GenBuild::GetBuildId()));
		return;
	}
	if (DataHash != GenBuild::GetDataHash())
	{
		Refuse(C, FString::Printf(TEXT("different Zero Hour game data (yours %08x, the host's %08x) - a modded or differently patched install?"), DataHash, GenBuild::GetDataHash()));
		return;
	}
	C.Name = Name.TrimStartAndEnd().Left(24);
	if (C.Name.IsEmpty())
	{
		C.Name = FString::Printf(TEXT("Player %u"), C.Id);
	}

	// Back to a match it was playing?
	if (Phase != EPhase::Lobby && Token != 0)
	{
		const int32 Seat = SeatToken.IndexOfByKey(Token);
		if (Seat != INDEX_NONE)
		{
			// (A connection of the same seat that hasn't been noticed to be dead yet gives way.)
			for (FClient& Other : Clients)
			{
				if (&Other != &C && Other.Player == Seat && !Other.bGone)
				{
					Other.Player = INDEX_NONE;
					Other.bGone = true;
				}
			}
			C.Token = Token;
			C.Player = Seat;
			C.bWelcomed = true;
			C.bLoaded = false;
			TArray<uint8> Welcome;
			FMemoryWriter W(Welcome);
			uint8 bHost = C.Id == HostId ? 1 : 0, bRejoin = 1;
			W << C.Id << C.Token << bHost << bRejoin;
			C.Conn->Send(EMsg::Welcome, Welcome);
			SendHistory(C);
			if (!SeatConnected[Seat])
			{
				SeatConnected[Seat] = true;
				FGenCommand Back(EGenCommandType::Session);
				Back.Name = TEXT("return");
				Back.A = Seat;
				Incoming.Add(MoveTemp(Back));
			}
			UE_LOG(LogGenerals, Log, TEXT("Server: %s comes back as player %d at frame %u"), *C.Name, Seat, NextFrame);
			Notice(FString::Printf(TEXT("%s is back"), *SeatName[Seat]));
			return;
		}
	}
	if (Phase != EPhase::Lobby)
	{
		Refuse(C, TEXT("a match is under way"));
		return;
	}
	int32 Slot = INDEX_NONE;
	for (int32 s = 0; s < FGenSetupData::MaxSlots && Slot == INDEX_NONE; ++s)
	{
		if (Lobby.Slots[s].State == EGenSlotState::Open)
		{
			Slot = s;
		}
	}
	if (Slot == INDEX_NONE)
	{
		Refuse(C, TEXT("the game is full"));
		return;
	}
	FGenSetupSlot& S = Lobby.Slots[Slot];
	S = FGenSetupSlot();
	S.State = EGenSlotState::Player;
	S.Name = C.Name;
	SlotOwner[Slot] = C.Id;
	C.Token = NewToken();
	C.bWelcomed = true;
	if (HostId == 0)
	{
		HostId = C.Id; // (the first to come sets the game up)
	}
	TArray<uint8> Welcome;
	FMemoryWriter W(Welcome);
	uint8 bHost = C.Id == HostId ? 1 : 0, bRejoin = 0;
	W << C.Id << C.Token << bHost << bRejoin;
	C.Conn->Send(EMsg::Welcome, Welcome);
	UE_LOG(LogGenerals, Log, TEXT("Server: %s joins in slot %d%s"), *C.Name, Slot, bHost ? TEXT(" (host)") : TEXT(""));
	Notice(FString::Printf(TEXT("%s has joined"), *C.Name));
	{
		FScopeLock L(&PortLock);
		if (bPortNoticeSent && C.Id == HostId)
		{
			C.Conn->Send(EMsg::Notice, WriteText(PortNotice)); // (the host is the one who passes it on)
		}
	}
	BroadcastLobby();
}

void FGenNetServer::OnLobbyEdit(FClient& C, const TArray<uint8>& Payload)
{
	if (Phase != EPhase::Lobby)
	{
		return;
	}
	FMemoryReader R(Payload);
	FGenSetupData Wanted;
	Wanted.Serialize(R);
	if (R.IsError())
	{
		return;
	}
	// What a player may change: its own side, colour, team and start position; the host also the map, the
	// money, the places that aren't players' (open, closed, computer) and the computer players' choices.
	const bool bHost = C.Id == HostId;
	if (bHost)
	{
		if (!Wanted.Map.Equals(Lobby.Map, ESearchCase::IgnoreCase))
		{
			Lobby.SetMap(Wanted.Map);
		}
		if (FGenShellData::Get().GetMoneyChoices().Contains(Wanted.StartingCash))
		{
			Lobby.StartingCash = Wanted.StartingCash;
		}
		Lobby.bTestUnits = Wanted.bTestUnits;
	}
	for (int32 s = 0; s < FGenSetupData::MaxSlots; ++s)
	{
		const FGenSetupSlot& Want = Wanted.Slots[s];
		const FGenSetupSlot& Have = Lobby.Slots[s];
		const bool bOwn = SlotOwner[s] == C.Id;
		const bool bHostsSlot = bHost && Have.State != EGenSlotState::Player;
		if (bHostsSlot && Want.State != Have.State && Want.State != EGenSlotState::Player)
		{
			Lobby.SetSlotState(s, Want.State);
		}
		if ((bOwn || bHostsSlot) && Lobby.Slots[s].IsOccupied())
		{
			if (Want.Color != Have.Color) { Lobby.SetSlotColor(s, Want.Color); }
			if (Want.Side != Have.Side) { Lobby.SetSlotSide(s, Want.Side); }
			if (Want.Team != Have.Team) { Lobby.SetSlotTeam(s, Want.Team); }
			if (Want.StartPos != Have.StartPos) { Lobby.SetSlotStartPos(s, Want.StartPos); }
		}
	}
	Lobby.AdjustSlotsForMap();
	BroadcastLobby();
}

void FGenNetServer::OnStartRequest(FClient& C)
{
	if (Phase != EPhase::Lobby || C.Id != HostId)
	{
		return;
	}
	FGenMatchSettings Settings = FGenMatchSettings::FromCommandLine();
	FRandomStream Random(Settings.Seed);
	TArray<FGenSkirmishSlot> Slots;
	TArray<int32> SlotOfPlayer;
	const FString Error = Lobby.Resolve(Random, Slots, &SlotOfPlayer);
	if (!Error.IsEmpty())
	{
		C.Conn->Send(EMsg::Notice, WriteText(Error));
		return;
	}
	Settings.Map = Lobby.Map;
	Settings.StartingCash = Lobby.StartingCash;
	Settings.Slots = Slots;
	Settings.RevealedPlayer = INDEX_NONE;
	Settings.bNoAI = false;
	Match = Settings;
	// Who plays which player: the players are the occupied slots in order, from player 1 (0 is nobody's).
	const int32 NumPlayers = Slots.Num() + 1;
	SeatToken.Init(0, NumPlayers);
	SeatConnected.Init(false, NumPlayers);
	SeatName.Init(FString(), NumPlayers);
	for (int32 p = 0; p < SlotOfPlayer.Num(); ++p)
	{
		const int32 Player = p + 1;
		SeatName[Player] = Slots[p].Name;
		if (FClient* Sitter = FindClient(SlotOwner[SlotOfPlayer[p]]))
		{
			Sitter->Player = Player;
			SeatToken[Player] = Sitter->Token;
			SeatConnected[Player] = true;
		}
	}
	History.Reset();
	Incoming.Reset();
	Reports.Reset();
	NextFrame = 0;
	Phase = EPhase::Loading;
	PhaseSince = FPlatformTime::Seconds();
	UE_LOG(LogGenerals, Log, TEXT("Server: the match begins: %s"), *Match.Describe());
	for (FClient& Each : Clients)
	{
		if (!Each.bWelcomed || Each.bGone)
		{
			continue;
		}
		TArray<uint8> Start;
		FMemoryWriter W(Start);
		Match.Serialize(W);
		W << Each.Player;
		Each.Conn->Send(EMsg::Start, Start);
	}
}

void FGenNetServer::OnLeave(FClient& C)
{
	if (!C.bWelcomed)
	{
		return;
	}
	UE_LOG(LogGenerals, Log, TEXT("Server: %s (connection %u, player %d) has gone"), *C.Name, C.Id, C.Player);
	const int32 Slot = SlotOf(C.Id);
	if (Phase == EPhase::Lobby && Slot != INDEX_NONE)
	{
		Lobby.Slots[Slot] = FGenSetupSlot();
		Lobby.Slots[Slot].State = EGenSlotState::Open;
		SlotOwner[Slot] = 0;
	}
	if (C.Id == HostId)
	{
		// The next to have come runs the lobby.
		HostId = 0;
		for (const FClient& Other : Clients)
		{
			if (&Other != &C && Other.bWelcomed && !Other.bGone)
			{
				HostId = Other.Id;
				break;
			}
		}
	}
	if (Phase != EPhase::Lobby && SeatConnected.IsValidIndex(C.Player) && SeatConnected[C.Player])
	{
		// The others play on; the simulation hears it at the next frame (the player's allies may take over).
		SeatConnected[C.Player] = false;
		FGenCommand Dropped(EGenCommandType::Session);
		Dropped.Name = TEXT("drop");
		Dropped.A = C.Player;
		Incoming.Add(MoveTemp(Dropped));
		Notice(FString::Printf(TEXT("%s has dropped out"), *SeatName[C.Player]));
	}
	else if (Phase == EPhase::Lobby)
	{
		Notice(FString::Printf(TEXT("%s has left"), *C.Name));
	}
	C.bWelcomed = false;
	if (Phase == EPhase::Lobby)
	{
		BroadcastLobby();
	}
}

void FGenNetServer::TickClock()
{
	if (Phase == EPhase::Loading)
	{
		// Everybody's map is loaded (or the slow ones are left behind): the clock starts.
		bool bAllLoaded = true;
		for (const FClient& C : Clients)
		{
			bAllLoaded &= !(C.bWelcomed && C.Player != INDEX_NONE && !C.bLoaded);
		}
		if (bAllLoaded || FPlatformTime::Seconds() - PhaseSince > LoadingLimit)
		{
			Phase = EPhase::Playing;
			ClockStart = FPlatformTime::Seconds();
			UE_LOG(LogGenerals, Log, TEXT("Server: everybody is ready%s - the clock runs"), bAllLoaded ? TEXT("") : TEXT(" (or has taken too long)"));
		}
		return;
	}
	if (Phase != EPhase::Playing)
	{
		return;
	}
	// Every frame that is due is closed with the orders that came in for it and sent to every game.
	const double Now = FPlatformTime::Seconds();
	int32 Closed = 0;
	while (Now >= ClockStart + double(NextFrame + 1) * FrameSeconds && Closed < 30)
	{
		TArray<uint8> Turn;
		FMemoryWriter W(Turn);
		uint32 FrameNo = NextFrame;
		int32 Num = Incoming.Num();
		W << FrameNo << Num;
		for (FGenCommand& C : Incoming)
		{
			C.Frame = NextFrame;
			C.Serialize(W);
		}
		History.Append(MoveTemp(Incoming));
		Incoming.Reset();
		Broadcast(EMsg::Turn, Turn, true);
		++NextFrame;
		++Closed;
	}
	if (Closed >= 30)
	{
		// (The server itself was held up for a second: the clock is set back rather than racing to catch up.)
		ClockStart = Now - double(NextFrame) * FrameSeconds;
	}
}

void FGenNetServer::NoteChecksums(const FClient& C, const TArray<uint8>& Payload)
{
	if (Phase != EPhase::Playing || C.Player == INDEX_NONE)
	{
		return;
	}
	FMemoryReader R(Payload);
	uint32 First = 0;
	int32 Num = 0;
	R << First << Num;
	Num = FMath::Clamp(Num, 0, 4096);
	for (int32 i = 0; i < Num && !R.IsError(); ++i)
	{
		uint32 Crc = 0;
		R << Crc;
		const uint32 FrameNo = First + uint32(i);
		TArray<TPair<int32, uint32>>& List = Reports.FindOrAdd(FrameNo);
		for (const TPair<int32, uint32>& Other : List)
		{
			// (Games that came apart stay apart: the first frame is what matters; after it, a word every five minutes.)
			if (Other.Value != Crc && (LastMismatchFrame == 0 || FrameNo > LastMismatchFrame + uint32(FMath::RoundToInt32(300.0 / FrameSeconds))))
			{
				const bool bFirst = LastMismatchFrame == 0;
				LastMismatchFrame = FrameNo;
				const FString Detail = FString::Printf(TEXT("player %d (%s) %08x, player %d (%s) %08x"), Other.Key,
					SeatName.IsValidIndex(Other.Key) ? *SeatName[Other.Key] : TEXT("?"), Other.Value, C.Player, *SeatName[C.Player], Crc);
				UE_LOG(LogGenerals, Warning, TEXT("Server: MISMATCH at frame %u: %s"), FrameNo, *Detail);
				TArray<uint8> Msg;
				FMemoryWriter W(Msg);
				uint32 F = FrameNo;
				FString D = Detail;
				W << F << D;
				Broadcast(EMsg::Mismatch, Msg, true);
				// Every game writes its whole state at one and the same frame (the next one closed): the dumps of
				// two computers, side by side, show which objects went apart.
				if (bFirst)
				{
					FGenCommand Dump(EGenCommandType::Session);
					Dump.Name = TEXT("dump");
					Incoming.Add(MoveTemp(Dump));
				}
			}
		}
		List.Emplace(C.Player, Crc);
	}
	// (Only the last minute is kept: every game reports within it, or it isn't playing.)
	const uint32 Keep = uint32(FMath::RoundToInt32(60.0 / FrameSeconds));
	if (NextFrame > Keep && Reports.Num() > int32(Keep) * 2)
	{
		const uint32 Oldest = NextFrame - Keep;
		for (auto It = Reports.CreateIterator(); It; ++It)
		{
			if (It.Key() < Oldest) { It.RemoveCurrent(); }
		}
	}
}

void FGenNetServer::Refuse(FClient& C, const FString& Why)
{
	UE_LOG(LogGenerals, Log, TEXT("Server: connection %u refused: %s"), C.Id, *Why);
	C.Conn->Send(EMsg::Refused, WriteText(Why));
	C.Conn->Pump();
	C.bGone = true;
}

void FGenNetServer::Notice(const FString& Text)
{
	Broadcast(EMsg::Notice, WriteText(Text), false);
}

void FGenNetServer::BroadcastLobby()
{
	TArray<uint8> Bytes;
	FMemoryWriter W(Bytes);
	Lobby.Serialize(W);
	for (uint32& Owner : SlotOwner)
	{
		W << Owner;
	}
	W << HostId;
	Broadcast(EMsg::Lobby, Bytes, false);
}

void FGenNetServer::Broadcast(EMsg Type, const TArray<uint8>& Payload, bool bInMatchOnly)
{
	for (FClient& C : Clients)
	{
		if (C.bWelcomed && !C.bGone && (!bInMatchOnly || C.Player != INDEX_NONE))
		{
			C.Conn->Send(Type, Payload);
		}
	}
}

void FGenNetServer::SendHistory(FClient& C)
{
	// The settings, its player and every order so far: the game starts the match and works its way up to the
	// present with these, then goes on with the turns that follow.
	TArray<uint8> Bytes;
	FMemoryWriter W(Bytes);
	Match.Serialize(W);
	W << C.Player;
	uint32 Closed = NextFrame;
	int32 Num = History.Num();
	W << Closed << Num;
	for (FGenCommand& Cmd : History)
	{
		Cmd.Serialize(W);
	}
	C.Conn->Send(EMsg::History, Bytes);
	UE_LOG(LogGenerals, Log, TEXT("Server: the history so far to %s: %u frames, %d orders, %d bytes"), *C.Name, Closed, Num, Bytes.Num());
}

void FGenNetServer::SaveRecord()
{
	if (bRecordSaved || (History.Num() == 0 && NextFrame == 0))
	{
		return;
	}
	bRecordSaved = true;
	// The match as the server saw it: settings and orders (no checksums - it works nothing out itself).
	FGenReplay Record;
	Record.Date = FDateTime::Now().ToString();
	Record.Settings = Match;
	Record.Commands = History;
	Record.BuildId = GenBuild::GetBuildId();
	Record.DataHash = GenBuild::GetDataHash();
	const FString Path = FGenReplay::GetDefaultFolder() / FString::Printf(TEXT("Server_%s.genrep"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	Record.Save(Path);
	UE_LOG(LogGenerals, Log, TEXT("Server: the match's orders -> %s"), *Path);
}
